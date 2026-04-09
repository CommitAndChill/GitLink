#include "Cmd_Shared.h"
#include "GitLink_CommandDispatcher.h"

#include "GitLinkCore/Repository/GitLink_Repository.h"

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

		// First drop any index entries (handles newly-added files that have nothing in HEAD).
		// Tolerate errors on this one — an "unstaging" attempt for a file that wasn't staged in
		// the first place is harmless.
		const FResult UnstageRes = InCtx.Repository->Unstage(InFiles);
		if (!UnstageRes)
		{
			// Not fatal — proceed to DiscardChanges which is the main operation.
		}

		const FResult DiscardRes = InCtx.Repository->DiscardChanges(InFiles);
		if (!DiscardRes)
		{
			return FCommandResult::Fail(FText::FromString(DiscardRes.ErrorMessage));
		}

		// Release any LFS locks we held on the reverted files — since we just threw away
		// local changes, there's no reason to hold the exclusive edit lock anymore. Best-
		// effort: non-lockable or non-locked files are skipped silently.
		Release_LfsLocksBestEffort(InCtx, InFiles);

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
