#include "Cmd_Shared.h"
#include "GitLink_CommandDispatcher.h"
#include "GitLink_Subprocess.h"
#include "GitLinkLog.h"

#include "GitLinkCore/Repository/GitLink_Repository.h"

#include <HAL/PlatformFileManager.h>

#define LOCTEXT_NAMESPACE "GitLinkCmdRevert"

// --------------------------------------------------------------------------------------------------------------------
// Cmd_Revert — "throw away my local changes, go back to HEAD". This is one Unreal operation
// mapped to two git concepts:
//
//   1. Working tree contents must match HEAD (git checkout HEAD -- <files>)
//   2. Index entries must match HEAD (git reset HEAD -- <files>)
//
// Our DiscardChanges op uses git_checkout_head with GIT_CHECKOUT_FORCE, which writes both the
// index AND the working tree to HEAD's state. That covers step 1 and 2 in one call.
//
// For newly-added files (not yet in HEAD), DiscardChanges would be a no-op because HEAD has
// nothing to check out. For those, we additionally call Unstage to drop the index entry.
//
// Submodule files: input batch is partitioned by submodule root. Files inside a submodule
// are reverted against a freshly-opened FRepository for that submodule (Unstage + DiscardChanges),
// and unlocked against that submodule's own LFS server via Release_LfsLocksBestEffort's
// per-repo routing (v0.2.0). Read-only re-stamping applies uniformly to all reverted files.
// --------------------------------------------------------------------------------------------------------------------

namespace gitlink::cmd
{
	namespace
	{
		// Performs the unstage + discard-changes pair against the given repo for the given
		// repo-relative paths. Returns the FResult from DiscardChanges (the dominant operation).
		auto DiscardForRepo(
			gitlink::IRepository& InRepo,
			const TArray<FString>& InRepoRelative) -> FResult
		{
			if (InRepoRelative.IsEmpty())
			{ return FResult::Ok(); }

			const FResult UnstageRes = InRepo.Unstage(InRepoRelative);
			if (!UnstageRes)
			{
				// Not fatal — proceed to DiscardChanges which is the main operation.
			}

			return InRepo.DiscardChanges(InRepoRelative);
		}

		// Re-runs the LFS smudge filter for the given repo-relative paths against the given cwd
		// (empty = parent repo, submodule root = submodule). libgit2's git_checkout_head does NOT
		// run Git's smudge filters, so without this call any LFS-tracked file lands as its
		// 132-byte pointer text instead of the real binary — Unreal then fails to load the asset
		// and (if it was open) kicks the user to the default level. `git lfs checkout` is a
		// no-op for non-LFS paths and idempotent for already-materialized LFS files; safe to
		// call unconditionally on the discarded path set.
		auto RehydrateLfsForRepo(
			FCommandContext&        InCtx,
			const FString&          InCwdOverride,
			const TArray<FString>&  InRepoRelative) -> void
		{
			if (InRepoRelative.IsEmpty() || InCtx.Subprocess == nullptr || !InCtx.Subprocess->IsValid())
			{ return; }

			TArray<FString> Args;
			Args.Reserve(InRepoRelative.Num() + 2);
			Args.Add(TEXT("checkout"));
			Args.Add(TEXT("--"));
			for (const FString& Rel : InRepoRelative)
			{ Args.Add(Rel); }

			const FGitLink_SubprocessResult Result = InCtx.Subprocess->RunLfs(Args, InCwdOverride);
			if (!Result.IsSuccess())
			{
				UE_LOG(LogGitLink, Warning,
					TEXT("Cmd_Revert: 'git lfs checkout' rehydration returned %d (cwd='%s'): %s"),
					Result.ExitCode, *InCwdOverride, *Result.StdErr);
			}
		}
	}

	auto Revert(
		FCommandContext&                  InCtx,
		const FSourceControlOperationRef& /*InOperation*/,
		const TArray<FString>&            InFiles) -> FCommandResult
	{
		if (InCtx.Repository == nullptr || !InCtx.Repository->IsOpen())
		{
			return FCommandResult::Fail(LOCTEXT("NoRepo",
				"GitLink: cannot revert — repository not open."));
		}

		if (InFiles.IsEmpty())
		{
			return FCommandResult::Ok();
		}

		UE_LOG(LogGitLink, Log, TEXT("Cmd_Revert: reverting %d file(s)"), InFiles.Num());

		// Every requested file goes into the discard set — DiscardChanges is a no-op for files
		// that match HEAD, so there's nothing to save by filtering. An earlier revision
		// classified files via the state cache and SKIPPED the discard for cached-clean-but-
		// locked files; with a stale cache (file modified since the last sweep) that skipped
		// the actual revert, released the lock, and stamped Unmodified — a revert that
		// reverted nothing while reporting success.
		//
		// Files whose repo couldn't be opened or whose discard failed are tracked and excluded
		// from the unlock pass (releasing a lock while un-reverted changes sit on disk is a
		// lock-discipline violation) and from the clean-state stamping below.
		TSet<FString> FailedFiles;
		TArray<FString> BatchErrors;

		const TArray<FRepoBatch> Batches = PartitionByRepo(InCtx, InFiles);
		for (const FRepoBatch& Batch : Batches)
		{
			TUniquePtr<gitlink::FRepository> SubRepo;
			gitlink::IRepository* TargetRepo = InCtx.Repository.Get();
			if (Batch.bIsSubmodule)
			{
				SubRepo = InCtx.Provider.Open_SubmoduleRepositoryFor(Batch.AbsoluteFiles[0]);
				if (!SubRepo.IsValid())
				{
					UE_LOG(LogGitLink, Warning,
						TEXT("Cmd_Revert: could not open submodule repo at '%s' — skipping %d file(s)"),
						*Batch.RepoRoot, Batch.AbsoluteFiles.Num());
					FailedFiles.Append(Batch.AbsoluteFiles);
					BatchErrors.Add(FString::Printf(
						TEXT("could not open submodule repo at '%s'"), *Batch.RepoRoot));
					continue;
				}
				TargetRepo = SubRepo.Get();
			}

			const FResult DiscardRes = DiscardForRepo(*TargetRepo, Batch.RelativeFiles);
			if (!DiscardRes)
			{
				UE_LOG(LogGitLink, Warning,
					TEXT("Cmd_Revert: DiscardChanges in '%s' failed: %s"),
					*Batch.RepoRoot, *DiscardRes.ErrorMessage);
				FailedFiles.Append(Batch.AbsoluteFiles);
				BatchErrors.Add(FString::Printf(
					TEXT("discard in '%s' failed: %s"), *Batch.RepoRoot, *DiscardRes.ErrorMessage));
				continue;
			}

			RehydrateLfsForRepo(InCtx, Batch.bIsSubmodule ? Batch.RepoRoot : FString(), Batch.RelativeFiles);
		}

		// Release any LFS locks we held — Release_LfsLocksBestEffort partitions by repo so
		// outer-repo unlocks hit the parent's LFS server and submodule unlocks hit each
		// submodule's own server. The outcome split tells us which files genuinely freed
		// vs which the server still holds locks on (typical cause: lock owner identity on
		// the LFS server differs from local `git config user.name`, so the unlock is
		// rejected). We intentionally do NOT stamp Lock=NotLocked on failed files —
		// previously the cache flipped to NotLocked while the server lock persisted,
		// hiding the failure and leaving the file permanently stuck.
		//
		// Only successfully-discarded files are unlocked: a file whose discard failed still
		// has the user's changes on disk, so the lock must stay held.
		TArray<FString> UnlockCandidates;
		UnlockCandidates.Reserve(InFiles.Num());
		for (const FString& File : InFiles)
		{
			if (!FailedFiles.Contains(File))
			{ UnlockCandidates.Add(File); }
		}

		const FLfsUnlockOutcome UnlockOutcome = Release_LfsLocksBestEffort(InCtx, UnlockCandidates);
		const TSet<FString> FailedSet(UnlockOutcome.FailedPaths);

		// Restore the read-only flag on lockable reverted files to match the un-checked-out
		// state. Skip files whose discard or unlock failed — leaving them writable matches
		// reality (user still holds the lock conceptually) and avoids masking the failure.
		for (const FString& File : InFiles)
		{
			if (FailedSet.Contains(File) || FailedFiles.Contains(File))
			{ continue; }
			if (InCtx.Provider.Is_FileLockable(File) && FPaths::FileExists(File))
			{
				FPlatformFileManager::Get().GetPlatformFile().SetReadOnly(*File, true);
			}
		}

		UE_LOG(LogGitLink, Log,
			TEXT("Cmd_Revert: reverted %d of %d file(s); discard failed for %d, unlock failed for %d"),
			InFiles.Num() - FailedFiles.Num(), InFiles.Num(),
			FailedFiles.Num(), UnlockOutcome.FailedPaths.Num());

		FCommandResult Result = FCommandResult::Ok();

		if (!BatchErrors.IsEmpty())
		{
			Result.bOk = false;
			Result.ErrorMessages.Add(FText::Format(
				LOCTEXT("RevertDiscardFailed",
					"Failed to revert {0} file(s); their local changes are untouched and any LFS "
					"locks on them remain held. Details: {1}"),
				FText::AsNumber(FailedFiles.Num()),
				FText::FromString(JoinTruncated(BatchErrors))));
		}

		if (!UnlockOutcome.FailedPaths.IsEmpty())
		{
			Result.bOk = false;

			Result.ErrorMessages.Add(FText::Format(
				LOCTEXT("RevertUnlockFailed",
					"Reverted local changes, but failed to release the LFS lock on {0} file(s). "
					"You still hold these locks on the server — fix the underlying error and try again. "
					"Common cause: the LFS server's lock owner identity differs from `git config user.name`. "
					"Details: {1}"),
				FText::AsNumber(UnlockOutcome.FailedPaths.Num()),
				FText::FromString(JoinTruncated(UnlockOutcome.ErrorMessages))));
		}

		Result.UpdatedStates.Reserve(InFiles.Num());
		for (const FString& File : InFiles)
		{
			// Files whose discard failed keep their cached (dirty / locked) state — the next
			// UpdateStatus reflects the on-disk truth.
			if (FailedFiles.Contains(File))
			{ continue; }

			// Lockability is .gitattributes-driven and uniform across outer-repo and submodule
			// files (locking is routed to each submodule's own LFS server, but the lockable
			// extension list is shared). Don't gate on bInSubmodule here.
			const bool bLockable     = InCtx.Provider.Is_FileLockable(File);
			const bool bUnlockFailed = FailedSet.Contains(File);

			FGitLink_CompositeState Composite;
			Composite.File          = EGitLink_FileState::Unknown;
			Composite.Tree          = EGitLink_TreeState::Unmodified;

			// On unlock failure, preserve the cached Lock state (typically Locked) so the
			// editor still shows the file as checked out and the user understands the
			// revert wasn't a complete no-op state-wise. On success, fall back to the
			// lockability default.
			if (bUnlockFailed)
			{
				const FGitLink_FileStateRef Existing = InCtx.StateCache.Find_FileState(File);
				Composite.Lock     = Existing->_State.Lock;
				Composite.LockUser = Existing->_State.LockUser;
			}
			else
			{
				Composite.Lock = bLockable ? EGitLink_LockState::NotLocked : EGitLink_LockState::Unlockable;
			}
			Composite.bInSubmodule  = InCtx.Provider.Is_InSubmodule(File);
			Result.UpdatedStates.Add(Make_FileState(Normalize_AbsolutePath(File), Composite));
		}
		return Result;
	}
}

#undef LOCTEXT_NAMESPACE
