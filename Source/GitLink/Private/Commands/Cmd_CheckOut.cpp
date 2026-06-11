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

		// Lock PER FILE, not per batch. `git lfs lock` with N paths reports failure for the
		// whole invocation, and the old batch-level handling had two real bugs in mixed
		// batches: (a) one "Lock exists" (ours) plus one genuine rejection stamped the WHOLE
		// batch Locked + writable (and Note_LocalLockOp then shielded the wrong state from
		// sweep correction for 30s); (b) a failing later batch returned Fail(), discarding
		// the Locked states of earlier batches whose server-side locks were already acquired
		// — invisible held locks. Checkout batches are typically small (user-selected), so
		// one subprocess per file is acceptable.
		int32 TotalLocked = 0;
		TArray<FString> FailedFiles;
		TArray<FString> FailureDetails;

		for (const FRepoBatch& Batch : Batches)
		{
			const FString CwdOverride = Batch.bIsSubmodule ? Batch.RepoRoot : FString();

			for (int32 Idx = 0; Idx < Batch.AbsoluteFiles.Num(); ++Idx)
			{
				const FString& Absolute = Batch.AbsoluteFiles[Idx];
				const FString& Relative = Batch.RelativeFiles[Idx];

				// `--` terminates option parsing so a path starting with '-' can't be read as
				// a git-lfs flag.
				const FGitLink_SubprocessResult LockResult =
					InCtx.Subprocess->RunLfs({ TEXT("lock"), TEXT("--"), Relative }, CwdOverride);

				if (!LockResult.IsSuccess())
				{
					// "Lock exists" is ambiguous: it fires both when WE already hold the lock
					// (idempotent re-lock — desired end state, e.g. the editor re-runs checkout
					// after a reconnect) and when SOMEONE ELSE does. Disambiguate via the cache:
					// only treat it as success when we have no record of another user's lock.
					const FString ErrStr = LockResult.Get_CombinedError();
					const bool bLockExists = ErrStr.Contains(TEXT("Lock exists"));
					const FGitLink_FileStateRef Existing = InCtx.StateCache.Find_FileState(Absolute);
					const bool bCacheSaysOther =
						Existing->_State.Lock == EGitLink_LockState::LockedOther;

					if (!bLockExists || bCacheSaysOther)
					{
						UE_LOG(LogGitLink, Warning,
							TEXT("Cmd_CheckOut: git lfs lock '%s' failed (cwd='%s'): %s"),
							*Relative, *Batch.RepoRoot, *ErrStr);
						FailedFiles.Add(Absolute);
						FailureDetails.Add(FString::Printf(TEXT("'%s': %s"), *Relative, *ErrStr));
						continue;
					}

					UE_LOG(LogGitLink, Log,
						TEXT("Cmd_CheckOut: '%s' already locked by us, treating as success"), *Relative);
				}

				// Clear read-only and emit predictive Locked state for this file.
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

				// Suppress the LFS-replica-lag race: the dispatcher's immediate poll fires
				// Cmd_UpdateStatus right after we return, and a stale /locks/verify response
				// would otherwise demote our just-locked file back to NotLocked.
				InCtx.Provider.Note_LocalLockOp(Absolute);

				++TotalLocked;
			}
		}

		UE_LOG(LogGitLink, Log,
			TEXT("Cmd_CheckOut: locked %d of %d LFS file(s) across %d repo(s) as '%s'"),
			TotalLocked, LockableAbsolute.Num(), Batches.Num(), *InCtx.Provider.Get_UserName());

		if (!FailedFiles.IsEmpty())
		{
			// Partial failure: the states for successfully-locked files are still applied (the
			// dispatcher merges UpdatedStates regardless of bOk), so the cache matches the
			// server even when the operation as a whole reports failure.
			Result.bOk = false;
			Result.ErrorMessages.Add(FText::Format(
				LOCTEXT("LockFailedPartial",
					"GitLink: failed to lock {0} of {1} file(s): {2}"),
				FText::AsNumber(FailedFiles.Num()),
				FText::AsNumber(LockableAbsolute.Num()),
				FText::FromString(JoinTruncated(FailureDetails))));
			return Result;
		}

		Result.InfoMessages.Add(FText::Format(
			LOCTEXT("LockOk", "Locked {0} file(s) via git-lfs."),
			FText::AsNumber(TotalLocked)));

		return Result;
	}
}

#undef LOCTEXT_NAMESPACE
