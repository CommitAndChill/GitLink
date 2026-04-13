#include "Cmd_Shared.h"
#include "GitLink_CommandDispatcher.h"
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
// --------------------------------------------------------------------------------------------------------------------

namespace gitlink::cmd
{
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

		// Discard changes for files that have actual modifications.
		if (!ModifiedFiles.IsEmpty())
		{
			// Convert to repo-relative paths — libgit2's git_checkout_head pathspec and
			// git_reset_default both expect paths relative to the repo root, not absolute.
			TArray<FString> RelativeModified;
			RelativeModified.Reserve(ModifiedFiles.Num());
			for (const FString& Abs : ModifiedFiles)
			{
				RelativeModified.Add(ToRepoRelativePath(InCtx.RepoRootAbsolute, Abs));
			}

			// First drop any index entries (handles newly-added files that have nothing in HEAD).
			const FResult UnstageRes = InCtx.Repository->Unstage(RelativeModified);
			if (!UnstageRes)
			{
				// Not fatal — proceed to DiscardChanges which is the main operation.
			}

			const FResult DiscardRes = InCtx.Repository->DiscardChanges(RelativeModified);
			if (!DiscardRes)
			{
				UE_LOG(LogGitLink, Warning,
					TEXT("Cmd_Revert: DiscardChanges failed: %s"), *DiscardRes.ErrorMessage);
				return FCommandResult::Fail(FText::FromString(DiscardRes.ErrorMessage));
			}
		}

		// Release any LFS locks we held on ALL reverted files — both modified and locked-only.
		// Since we just threw away local changes (or had none), there's no reason to hold the
		// exclusive edit lock anymore.
		Release_LfsLocksBestEffort(InCtx, InFiles);

		// Restore read-only flag on reverted files to match the un-checked-out state.
		for (const FString& File : InFiles)
		{
			if (InCtx.Provider.Is_FileLockable(File) && FPaths::FileExists(File))
			{
				FPlatformFileManager::Get().GetPlatformFile().SetReadOnly(*File, true);
			}
		}

		UE_LOG(LogGitLink, Log,
			TEXT("Cmd_Revert: reverted %d modified, %d locked-only file(s)"),
			ModifiedFiles.Num(), LockedOnlyFiles.Num());

		FCommandResult Result = FCommandResult::Ok();
		Result.UpdatedStates.Reserve(InFiles.Num());
		for (const FString& File : InFiles)
		{
			const bool bLockable = InCtx.Provider.Is_FileLockable(File);
			FGitLink_CompositeState Composite;
			Composite.File = EGitLink_FileState::Unknown;
			Composite.Tree = EGitLink_TreeState::Unmodified;
			Composite.Lock = bLockable ? EGitLink_LockState::NotLocked : EGitLink_LockState::Unlockable;
			Result.UpdatedStates.Add(Make_FileState(Normalize_AbsolutePath(File), Composite));
		}
		return Result;
	}
}

#undef LOCTEXT_NAMESPACE
