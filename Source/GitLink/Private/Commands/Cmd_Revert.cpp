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
// are reverted against a freshly-opened FRepository for that submodule (Unstage + DiscardChanges).
// LFS unlock and read-only re-stamping are skipped for submodule files — they were never
// locked through GitLink (parent's LFS server doesn't manage them) and the read-only flag
// would block the user's subsequent edits in the submodule.
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

		// Separate files into two buckets: those with local modifications that need a
		// discard, and those that are only locked (checkout) but not yet modified on disk.
		// The latter case only needs an LFS unlock — no git index/working-tree work.
		TArray<FString> ModifiedFiles;
		TArray<FString> LockedOnlyFiles;
		ModifiedFiles.Reserve(InFiles.Num());
		LockedOnlyFiles.Reserve(InFiles.Num());

		for (const FString& File : InFiles)
		{
			const FGitLink_FileStateRef State = InCtx.StateCache.Find_FileState(File);
			const bool bIsModified =
				   State->_State.File == EGitLink_FileState::Added
				|| State->_State.File == EGitLink_FileState::Deleted
				|| State->_State.File == EGitLink_FileState::Modified
				|| State->_State.File == EGitLink_FileState::Renamed
				|| State->_State.File == EGitLink_FileState::Unmerged;

			if (bIsModified)
			{
				ModifiedFiles.Add(File);
			}
			else if (State->_State.Lock == EGitLink_LockState::Locked)
			{
				LockedOnlyFiles.Add(File);
			}
			else
			{
				// Neither modified nor locked — nothing to revert. Still emit a clean state
				// so the editor refreshes.
				ModifiedFiles.Add(File);  // let the discard handle it, it's harmless
			}
		}

		// Within ModifiedFiles, partition outer vs per-submodule. Each submodule needs its
		// own FRepository because libgit2 ops are scoped to a single repository handle.
		TArray<FString> OuterModified;
		TMap<FString, TArray<FString>> ModifiedBySubmoduleRoot;
		OuterModified.Reserve(ModifiedFiles.Num());

		for (const FString& File : ModifiedFiles)
		{
			const FString SubmoduleRoot = InCtx.Provider.Get_SubmoduleRoot(File);
			if (SubmoduleRoot.IsEmpty())
			{ OuterModified.Add(File); }
			else
			{ ModifiedBySubmoduleRoot.FindOrAdd(SubmoduleRoot).Add(File); }
		}

		// Outer repo: existing path.
		if (!OuterModified.IsEmpty())
		{
			TArray<FString> RelativeModified;
			RelativeModified.Reserve(OuterModified.Num());
			for (const FString& Abs : OuterModified)
			{
				RelativeModified.Add(ToRepoRelativePath(InCtx.RepoRootAbsolute, Abs));
			}

			const FResult DiscardRes = DiscardForRepo(*InCtx.Repository, RelativeModified);
			if (!DiscardRes)
			{
				UE_LOG(LogGitLink, Warning,
					TEXT("Cmd_Revert: outer DiscardChanges failed: %s"), *DiscardRes.ErrorMessage);
				return FCommandResult::Fail(FText::FromString(DiscardRes.ErrorMessage));
			}

			RehydrateLfsForRepo(InCtx, FString(), RelativeModified);
		}

		// Each submodule bucket: open inner repo, discard there.
		for (const TPair<FString, TArray<FString>>& Pair : ModifiedBySubmoduleRoot)
		{
			const FString& SubmoduleRoot = Pair.Key;
			const TArray<FString>& SubFiles = Pair.Value;

			TUniquePtr<gitlink::FRepository> SubRepo =
				InCtx.Provider.Open_SubmoduleRepositoryFor(SubFiles[0]);
			if (!SubRepo.IsValid())
			{
				UE_LOG(LogGitLink, Warning,
					TEXT("Cmd_Revert: could not open submodule repo at '%s' — skipping %d file(s)"),
					*SubmoduleRoot, SubFiles.Num());
				continue;
			}

			TArray<FString> SubRelative;
			SubRelative.Reserve(SubFiles.Num());
			for (const FString& Abs : SubFiles)
			{
				SubRelative.Add(ToRepoRelativePath(SubmoduleRoot, Abs));
			}

			const FResult SubDiscardRes = DiscardForRepo(*SubRepo, SubRelative);
			if (!SubDiscardRes)
			{
				UE_LOG(LogGitLink, Warning,
					TEXT("Cmd_Revert: submodule '%s' DiscardChanges failed: %s"),
					*SubmoduleRoot, *SubDiscardRes.ErrorMessage);
				return FCommandResult::Fail(FText::FromString(SubDiscardRes.ErrorMessage));
			}

			RehydrateLfsForRepo(InCtx, SubmoduleRoot, SubRelative);
		}

		// Release any LFS locks we held — Release_LfsLocksBestEffort partitions by repo so
		// outer-repo unlocks hit the parent's LFS server and submodule unlocks hit each
		// submodule's own server. The outcome split tells us which files genuinely freed
		// vs which the server still holds locks on (typical cause: lock owner identity on
		// the LFS server differs from local `git config user.name`, so the unlock is
		// rejected). We intentionally do NOT stamp Lock=NotLocked on failed files —
		// previously the cache flipped to NotLocked while the server lock persisted,
		// hiding the failure and leaving the file permanently stuck.
		const FLfsUnlockOutcome UnlockOutcome = Release_LfsLocksBestEffort(InCtx, InFiles);
		const TSet<FString> FailedSet(UnlockOutcome.FailedPaths);

		// Restore the read-only flag on lockable reverted files to match the un-checked-out
		// state. Skip files whose unlock failed — leaving them writable matches reality
		// (user still holds the lock conceptually) and avoids masking the failure.
		for (const FString& File : InFiles)
		{
			if (FailedSet.Contains(File))
			{ continue; }
			if (InCtx.Provider.Is_FileLockable(File) && FPaths::FileExists(File))
			{
				FPlatformFileManager::Get().GetPlatformFile().SetReadOnly(*File, true);
			}
		}

		UE_LOG(LogGitLink, Log,
			TEXT("Cmd_Revert: reverted %d modified, %d locked-only file(s); unlock failed for %d file(s)"),
			ModifiedFiles.Num(), LockedOnlyFiles.Num(), UnlockOutcome.FailedPaths.Num());

		FCommandResult Result = FCommandResult::Ok();

		if (!UnlockOutcome.FailedPaths.IsEmpty())
		{
			Result.bOk = false;

			// Aggregate every batch error into a single user-visible message. Truncate at a
			// sane length so the toast / dialog stays readable when many batches fail.
			FString Joined;
			for (int32 Idx = 0; Idx < UnlockOutcome.ErrorMessages.Num(); ++Idx)
			{
				if (Idx > 0) { Joined += TEXT("\n"); }
				Joined += UnlockOutcome.ErrorMessages[Idx];
			}
			constexpr int32 MaxLen = 1024;
			if (Joined.Len() > MaxLen)
			{ Joined = Joined.Left(MaxLen) + TEXT(" ..."); }

			Result.ErrorMessages.Add(FText::Format(
				LOCTEXT("RevertUnlockFailed",
					"Reverted local changes, but failed to release the LFS lock on {0} file(s). "
					"You still hold these locks on the server — fix the underlying error and try again. "
					"Common cause: the LFS server's lock owner identity differs from `git config user.name`. "
					"Details: {1}"),
				FText::AsNumber(UnlockOutcome.FailedPaths.Num()),
				FText::FromString(Joined)));
		}

		Result.UpdatedStates.Reserve(InFiles.Num());
		for (const FString& File : InFiles)
		{
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
