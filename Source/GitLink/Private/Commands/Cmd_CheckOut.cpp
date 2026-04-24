#include "Cmd_Shared.h"
#include "GitLink/GitLink_Provider.h"
#include "GitLink_CommandDispatcher.h"
#include "GitLink_Subprocess.h"
#include "GitLinkLog.h"

#include <HAL/PlatformFileManager.h>

#define LOCTEXT_NAMESPACE "GitLinkCmdCheckOut"

// --------------------------------------------------------------------------------------------------------------------
// Cmd_CheckOut — acquire a Git LFS lock on each selected file.
//
// In Unreal source control terms, "Check Out" means "I want exclusive edit rights on this
// file until I Check In or Revert". For LFS-tracked binary assets this maps directly to
// `git lfs lock <relative path>`, which communicates with the LFS server to record the lock
// for your user. Other users trying to lock the same file will be denied until you release it.
//
// Flow:
//   1. Verify LFS is available (UsesCheckout() already gated this, but double-check)
//   2. Filter requested files down to LFS-lockable ones (.uasset / .umap / ... per
//      .gitattributes). Non-lockable files are silently skipped
//   3. Convert absolute paths to repo-relative forward-slash paths
//   4. Run `git lfs lock <paths>` via FGitLink_Subprocess in ONE call (batched)
//   5. On success, emit Locked states for every file we locked so the content browser
//      immediately shows the checked-out icon
//   6. On failure, report the LFS error (typically "already locked by <other user>" or
//      "no such file") and return a failing FCommandResult
// --------------------------------------------------------------------------------------------------------------------

namespace gitlink::cmd
{
	auto CheckOut(
		FCommandContext&                  InCtx,
		const FSourceControlOperationRef& /*InOperation*/,
		const TArray<FString>&            InFiles) -> FCommandResult
	{
		if (InCtx.Subprocess == nullptr || !InCtx.Subprocess->IsValid())
		{
			return FCommandResult::Fail(LOCTEXT("NoGit",
				"GitLink: CheckOut requires git.exe via subprocess, which is not configured."));
		}

		if (!InCtx.Provider.Is_LfsAvailable())
		{
			return FCommandResult::Fail(LOCTEXT("NoLfs",
				"GitLink: CheckOut requires git-lfs, which was not detected at connect time. "
				"Install git-lfs and reconnect."));
		}

		if (InFiles.IsEmpty())
		{ return FCommandResult::Ok(); }

		// Filter to lockable files, skipping submodules. Submodule files should never reach
		// this command: FGitLink_FileState reports IsCheckedOut()=true / CanCheckout()=false
		// for them, so UE's PromptToCheckoutPackages short-circuits before invoking CheckOut.
		// This filter is defense-in-depth in case some other code path calls Execute(CheckOut)
		// with a submodule file explicitly — we silently no-op rather than hitting the parent
		// repo's LFS server (which has no knowledge of child-repo files).
		TArray<FString> LockableAbsolute;
		LockableAbsolute.Reserve(InFiles.Num());

		for (const FString& File : InFiles)
		{
			if (InCtx.Provider.Is_InSubmodule(File))
			{
				UE_LOG(LogGitLink, Verbose,
					TEXT("Cmd_CheckOut: skipping submodule file '%s'"), *File);
			}
			else if (InCtx.Provider.Is_FileLockable(File))
			{
				LockableAbsolute.Add(File);
			}
			else
			{
				UE_LOG(LogGitLink, Verbose,
					TEXT("Cmd_CheckOut: skipping non-lockable file '%s'"), *File);
			}
		}

		if (LockableAbsolute.IsEmpty())
		{ return FCommandResult::Ok(); }

		FCommandResult Result = FCommandResult::Ok();
		Result.UpdatedStates.Reserve(LockableAbsolute.Num());

		// Build the argument list: "lock <relative paths...>"
		TArray<FString> LockArgs;
		LockArgs.Reserve(LockableAbsolute.Num() + 1);
		LockArgs.Add(TEXT("lock"));
		for (const FString& Absolute : LockableAbsolute)
		{
			LockArgs.Add(ToRepoRelativePath(InCtx.RepoRootAbsolute, Absolute));
		}

		const FGitLink_SubprocessResult LockResult = InCtx.Subprocess->RunLfs(LockArgs);
		if (!LockResult.IsSuccess())
		{
			// "Lock exists" means WE already hold the lock — that's the desired end state,
			// so treat it as success. The editor re-runs checkout after a Cmd_Connect
			// reconnect, and we don't want to fail just because we're already locked.
			const FString ErrStr = LockResult.Get_CombinedError();
			const bool bAlreadyLockedByUs = ErrStr.Contains(TEXT("Lock exists"));

			if (bAlreadyLockedByUs)
			{
				UE_LOG(LogGitLink, Log,
					TEXT("Cmd_CheckOut: file(s) already locked by us, treating as success"));
			}
			else
			{
				const FText Err = FText::Format(
					LOCTEXT("LockFailed", "GitLink: git lfs lock failed: {0}"),
					FText::FromString(ErrStr));

				UE_LOG(LogGitLink, Warning, TEXT("%s"), *Err.ToString());
				return FCommandResult::Fail(Err);
			}
		}

		UE_LOG(LogGitLink, Log,
			TEXT("Cmd_CheckOut: locked %d LFS file(s) as '%s'"),
			LockableAbsolute.Num(),
			*InCtx.Provider.Get_UserName());

		// Clear the read-only flag on each locked file so the editor can save to it
		// without showing the "Make Writable" prompt. This matches Perforce/SVN behavior
		// where "Check Out" == "make writable + acquire exclusive edit rights".
		for (const FString& Absolute : LockableAbsolute)
		{
			if (FPaths::FileExists(Absolute))
			{
				FPlatformFileManager::Get().GetPlatformFile().SetReadOnly(*Absolute, false);
			}
		}

		// Emit predictive Locked states for every file we just locked. The editor will
		// re-query status and see IsCheckedOut() == true.
		for (const FString& Absolute : LockableAbsolute)
		{
			// Pull the existing state so we don't clobber File/Tree if the file is dirty.
			FGitLink_FileStateRef Existing = InCtx.StateCache.Find_FileState(Absolute);
			FGitLink_CompositeState Composite = Existing->_State;
			Composite.Lock = EGitLink_LockState::Locked;
			Composite.LockUser = InCtx.Provider.Get_UserName();

			Result.UpdatedStates.Add(Make_FileState(Normalize_AbsolutePath(Absolute), Composite));
		}

		Result.InfoMessages.Add(FText::Format(
			LOCTEXT("LockOk", "Locked {0} file(s) via git-lfs."),
			FText::AsNumber(LockableAbsolute.Num())));

		return Result;
	}
}

#undef LOCTEXT_NAMESPACE
