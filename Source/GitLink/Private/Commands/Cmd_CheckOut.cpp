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
// Submodule files: input batch is partitioned by repo root. Each submodule has its own LFS
// endpoint / git config / credentials, so we run `git lfs lock` once per repo with the
// subprocess cwd set to that repo's root. The submodule's LFS server records the lock
// under the submodule's own user identity.
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

		// Filter to lockable files first (extension-based). Non-lockable files are silently
		// skipped — the editor sometimes asks to lock e.g. .uplugin which isn't covered by
		// .gitattributes lockable patterns.
		TArray<FString> LockableAbsolute;
		LockableAbsolute.Reserve(InFiles.Num());
		for (const FString& File : InFiles)
		{
			if (InCtx.Provider.Is_FileLockable(File))
			{ LockableAbsolute.Add(File); }
			else
			{
				UE_LOG(LogGitLink, Verbose,
					TEXT("Cmd_CheckOut: skipping non-lockable file '%s'"), *File);
			}
		}

		if (LockableAbsolute.IsEmpty())
		{ return FCommandResult::Ok(); }

		// Partition by repo so each submodule's lock request hits its own LFS server.
		const TArray<FRepoBatch> Batches = PartitionByRepo(InCtx, LockableAbsolute);

		FCommandResult Result = FCommandResult::Ok();
		Result.UpdatedStates.Reserve(LockableAbsolute.Num());

		int32 TotalLocked = 0;
		for (const FRepoBatch& Batch : Batches)
		{
			if (Batch.RelativeFiles.IsEmpty())
			{ continue; }

			TArray<FString> LockArgs;
			LockArgs.Reserve(Batch.RelativeFiles.Num() + 1);
			LockArgs.Add(TEXT("lock"));
			LockArgs.Append(Batch.RelativeFiles);

			const FString CwdOverride = Batch.bIsSubmodule ? Batch.RepoRoot : FString();
			const FGitLink_SubprocessResult LockResult = InCtx.Subprocess->RunLfs(LockArgs, CwdOverride);

			if (!LockResult.IsSuccess())
			{
				// "Lock exists" means WE already hold the lock — that's the desired end state,
				// so treat it as success. The editor re-runs checkout after a Cmd_Connect
				// reconnect, and we don't want to fail just because we're already locked.
				const FString ErrStr = LockResult.Get_CombinedError();
				const bool bAlreadyLockedByUs = ErrStr.Contains(TEXT("Lock exists"));

				if (!bAlreadyLockedByUs)
				{
					const FText Err = FText::Format(
						LOCTEXT("LockFailed", "GitLink: git lfs lock failed (cwd='{0}'): {1}"),
						FText::FromString(Batch.RepoRoot),
						FText::FromString(ErrStr));

					UE_LOG(LogGitLink, Warning, TEXT("%s"), *Err.ToString());
					return FCommandResult::Fail(Err);
				}

				UE_LOG(LogGitLink, Log,
					TEXT("Cmd_CheckOut: file(s) in '%s' already locked by us, treating as success"),
					*Batch.RepoRoot);
			}

			// Clear read-only and emit predictive Locked state for each file in the bucket.
			for (int32 Idx = 0; Idx < Batch.AbsoluteFiles.Num(); ++Idx)
			{
				const FString& Absolute = Batch.AbsoluteFiles[Idx];

				if (FPaths::FileExists(Absolute))
				{
					FPlatformFileManager::Get().GetPlatformFile().SetReadOnly(*Absolute, false);
				}

				FGitLink_FileStateRef Existing = InCtx.StateCache.Find_FileState(Absolute);
				FGitLink_CompositeState Composite = Existing->_State;
				Composite.Lock          = EGitLink_LockState::Locked;
				Composite.LockUser      = InCtx.Provider.Get_UserName();
				Composite.bInSubmodule  = Batch.bIsSubmodule;

				Result.UpdatedStates.Add(Make_FileState(Normalize_AbsolutePath(Absolute), Composite));
			}

			TotalLocked += Batch.RelativeFiles.Num();
		}

		UE_LOG(LogGitLink, Log,
			TEXT("Cmd_CheckOut: locked %d LFS file(s) across %d repo(s) as '%s'"),
			TotalLocked, Batches.Num(), *InCtx.Provider.Get_UserName());

		Result.InfoMessages.Add(FText::Format(
			LOCTEXT("LockOk", "Locked {0} file(s) via git-lfs."),
			FText::AsNumber(TotalLocked)));

		return Result;
	}
}

#undef LOCTEXT_NAMESPACE
