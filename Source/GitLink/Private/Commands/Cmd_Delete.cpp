#include "Cmd_Shared.h"
#include "GitLink_CommandDispatcher.h"
#include "GitLinkLog.h"

#include "GitLinkCore/Repository/GitLink_Repository.h"

#include <HAL/PlatformFileManager.h>

#define LOCTEXT_NAMESPACE "GitLinkCmdDelete"

// --------------------------------------------------------------------------------------------------------------------
// Cmd_Delete — "git rm <files>". The editor invokes this when the user deletes a source-
// controlled asset. By the time we're called, the editor has already removed the .uasset from
// disk, so we just need to stage the deletion via git_index_add_all (which picks up missing
// files as index removals).
//
// For safety we also remove any stragglers still on disk — happens when the editor only marks
// an asset for deletion but doesn't actually unlink.
//
// Submodule files: input batch is partitioned by submodule root. Files in the outer repo are
// staged against InCtx.Repository as before; files inside a submodule are staged against a
// freshly-opened FRepository for that submodule, so the deletion is recorded in the
// submodule's own index. The user is responsible for `git commit` inside the submodule —
// this command never commits.
// --------------------------------------------------------------------------------------------------------------------

namespace gitlink::cmd
{
	auto Delete(
		FCommandContext&                  InCtx,
		const FSourceControlOperationRef& /*InOperation*/,
		const TArray<FString>&            InFiles) -> FCommandResult
	{
		if (InCtx.Repository == nullptr || !InCtx.Repository->IsOpen())
		{
			return FCommandResult::Fail(LOCTEXT("NoRepo",
				"GitLink: cannot delete — repository not open."));
		}

		if (InFiles.IsEmpty())
		{
			return FCommandResult::Ok();
		}

		UE_LOG(LogGitLink, Log, TEXT("Cmd_Delete: deleting %d file(s)"), InFiles.Num());

		// Remove files that are still on disk. Tolerate missing files (editor may have already
		// unlinked them) — the stage step below will pick up the deletions either way.
		IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
		for (const FString& File : InFiles)
		{
			if (PlatformFile.FileExists(*File))
			{
				PlatformFile.DeleteFile(*File);
			}
		}

		// Partition into outer-repo files and per-submodule buckets keyed by submodule root.
		TArray<FString> OuterFiles;
		TMap<FString, TArray<FString>> FilesBySubmoduleRoot;
		OuterFiles.Reserve(InFiles.Num());

		for (const FString& File : InFiles)
		{
			const FString SubmoduleRoot = InCtx.Provider.Get_SubmoduleRoot(File);
			if (SubmoduleRoot.IsEmpty())
			{ OuterFiles.Add(File); }
			else
			{ FilesBySubmoduleRoot.FindOrAdd(SubmoduleRoot).Add(File); }
		}

		// Outer-repo bucket — existing path.
		if (!OuterFiles.IsEmpty())
		{
			const FResult StageRes = InCtx.Repository->Stage(OuterFiles);
			if (!StageRes)
			{
				UE_LOG(LogGitLink, Warning,
					TEXT("Cmd_Delete: outer Stage failed: %s"), *StageRes.ErrorMessage);
				return FCommandResult::Fail(FText::FromString(StageRes.ErrorMessage));
			}
		}

		// Submodule buckets — open each submodule's repo and stage repo-relative paths there.
		for (const TPair<FString, TArray<FString>>& Pair : FilesBySubmoduleRoot)
		{
			const FString& SubmoduleRoot = Pair.Key;
			const TArray<FString>& SubFiles = Pair.Value;

			TUniquePtr<gitlink::FRepository> SubRepo =
				InCtx.Provider.Open_SubmoduleRepositoryFor(SubFiles[0]);
			if (!SubRepo.IsValid())
			{
				UE_LOG(LogGitLink, Warning,
					TEXT("Cmd_Delete: could not open submodule repo at '%s' — skipping %d file(s)"),
					*SubmoduleRoot, SubFiles.Num());
				continue;
			}

			TArray<FString> SubRelative;
			SubRelative.Reserve(SubFiles.Num());
			for (const FString& Abs : SubFiles)
			{
				SubRelative.Add(ToRepoRelativePath(SubmoduleRoot, Abs));
			}

			const FResult SubStageRes = SubRepo->Stage(SubRelative);
			if (!SubStageRes)
			{
				UE_LOG(LogGitLink, Warning,
					TEXT("Cmd_Delete: submodule '%s' Stage failed: %s"),
					*SubmoduleRoot, *SubStageRes.ErrorMessage);
				return FCommandResult::Fail(FText::FromString(SubStageRes.ErrorMessage));
			}
		}

		// Release LFS locks ONLY on files that were never committed (File=Added in the
		// cache pre-delete). For tracked files (in HEAD) we leave the lock alone — the
		// deletion is staged but not yet committed, and someone else could still pick up
		// the file from HEAD until it is. CheckIn releases the lock when the deletion
		// is committed.
		//
		// Edge case: a file we have no cache entry for falls through to File=Unknown, which
		// we treat as "not Added" → don't release. That's the safe default; if there really
		// is no lock, Release_LfsLocksBestEffort would have been a no-op anyway. If there
		// IS a lock on a file we can't identify, leaving it for explicit user action
		// (Revert) is preferable to a silent server-side state change.
		TArray<FString> UncommittedAddsToUnlock;
		UncommittedAddsToUnlock.Reserve(InFiles.Num());
		for (const FString& File : InFiles)
		{
			const FGitLink_FileStateRef Existing = InCtx.StateCache.Find_FileState(File);
			if (Existing->_State.File == EGitLink_FileState::Added)
			{
				UncommittedAddsToUnlock.Add(File);
			}
		}

		const FLfsUnlockOutcome UnlockOutcome = Release_LfsLocksBestEffort(InCtx, UncommittedAddsToUnlock);
		const TSet<FString> FailedSet(UnlockOutcome.FailedPaths);

		UE_LOG(LogGitLink, Log,
			TEXT("Cmd_Delete: deleted %d file(s); released stale locks on %d uncommitted-add file(s) (%d failed); locks on %d tracked file(s) preserved for CheckIn"),
			InFiles.Num(),
			UncommittedAddsToUnlock.Num() - UnlockOutcome.FailedPaths.Num(),
			UnlockOutcome.FailedPaths.Num(),
			InFiles.Num() - UncommittedAddsToUnlock.Num());

		FCommandResult Result = FCommandResult::Ok();

		if (!UnlockOutcome.FailedPaths.IsEmpty())
		{
			Result.bOk = false;

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
				LOCTEXT("DeleteUnlockFailed",
					"Staged the deletion of {0} file(s), but failed to release stale LFS locks "
					"on {1} never-committed file(s). The server still records you as the lock "
					"holder; you may need to release these manually with `git lfs unlock`. "
					"(Locks on tracked files are intentionally preserved — they will be released "
					"by CheckIn when you commit the deletion.) Details: {2}"),
				FText::AsNumber(InFiles.Num()),
				FText::AsNumber(UnlockOutcome.FailedPaths.Num()),
				FText::FromString(Joined)));
		}

		// Build a set of files we attempted to unlock (the uncommitted-adds set), so we can
		// distinguish "tracked file — leave Lock alone" from "uncommitted add — unlock
		// succeeded, clear Lock" from "uncommitted add — unlock failed, preserve Lock".
		const TSet<FString> AttemptedUnlockSet(UncommittedAddsToUnlock);

		Result.UpdatedStates.Reserve(InFiles.Num());
		for (const FString& File : InFiles)
		{
			FGitLink_FileStateRef NewState = Make_FileState(
				Normalize_AbsolutePath(File),
				EGitLink_FileState::Deleted,
				EGitLink_TreeState::Staged);

			// Carry the bInSubmodule flag forward so command code that operates on the next
			// state (e.g. PartitionByRepo on subsequent ops) routes correctly.
			if (InCtx.Provider.Is_InSubmodule(File))
			{
				NewState->_State.bInSubmodule = true;
			}

			// Lock state policy on delete:
			//   1. Tracked file (we did NOT attempt unlock) → preserve cached Lock state.
			//      The deletion is staged but not committed; the user (or another working
			//      copy) could still revert to HEAD and pick up the file. Lock should be
			//      held until CheckIn commits the deletion (CheckIn's
			//      Release_LfsLocksBestEffort takes care of it then).
			//   2. Uncommitted-add, unlock succeeded → clear Lock to lockability default
			//      (NotLocked / Unlockable).
			//   3. Uncommitted-add, unlock failed → preserve cached Lock state so the
			//      editor still shows it locked and the user can retry / clean up manually.
			const bool bAttempted    = AttemptedUnlockSet.Contains(File);
			const bool bUnlockFailed = FailedSet.Contains(File);
			const FGitLink_FileStateRef Existing = InCtx.StateCache.Find_FileState(File);

			if (!bAttempted || bUnlockFailed)
			{
				NewState->_State.Lock     = Existing->_State.Lock;
				NewState->_State.LockUser = Existing->_State.LockUser;
			}
			else
			{
				const bool bLockable = InCtx.Provider.Is_FileLockable(File);
				NewState->_State.Lock = bLockable ? EGitLink_LockState::NotLocked : EGitLink_LockState::Unlockable;
			}

			Result.UpdatedStates.Add(NewState);
		}
		return Result;
	}
}

#undef LOCTEXT_NAMESPACE
