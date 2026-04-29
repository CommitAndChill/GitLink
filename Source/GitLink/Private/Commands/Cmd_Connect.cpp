#include "Cmd_Shared.h"
#include "GitLink_CommandDispatcher.h"
#include "GitLink_LfsHttpClient.h"

#include "GitLink/GitLink_Provider.h"
#include "GitLinkLog.h"

#include "GitLinkCore/Repository/GitLink_Repository.h"

#include <Misc/Paths.h>
#include <SourceControlOperations.h>

// Forward declarations for the GitLinkCore ops we need but aren't exposed via a public header.
namespace gitlink::op
{
	auto Enumerate_TrackedFiles(gitlink::FRepository& InRepo) -> TArray<FString>;
}

namespace gitlink::cmd
{
	// Forward declaration — defined in Cmd_UpdateStatus.cpp. Cmd_Connect runs an initial
	// UpdateStatus on success so the state cache starts populated with the working tree's
	// dirty files, which lets the content browser show modification markers immediately
	// instead of waiting for the user to click Refresh (or for the background poll that
	// doesn't exist yet in v1).
	extern auto UpdateStatus(
		FCommandContext& InCtx,
		const FSourceControlOperationRef& InOperation,
		const TArray<FString>& InFiles) -> FCommandResult;
}

// --------------------------------------------------------------------------------------------------------------------
// Cmd_Connect — handles FConnect, Unreal's "is the provider usable?" probe.
//
// The repository was already opened in FGitLink_Provider::Init() via CheckRepositoryStatus(),
// so this handler's job is just to verify a real repo handle exists and report a sensible
// error otherwise. The UE editor uses the error text in the Connect dialog.
// --------------------------------------------------------------------------------------------------------------------

#define LOCTEXT_NAMESPACE "GitLinkCmdConnect"

namespace gitlink::cmd
{
	auto Connect(
		FCommandContext&                  InCtx,
		const FSourceControlOperationRef& InOperation,
		const TArray<FString>&            /*InFiles*/) -> FCommandResult
	{
		TSharedRef<FConnect> ConnectOp = StaticCastSharedRef<FConnect>(InOperation);

		if (InCtx.Repository == nullptr || !InCtx.Repository->IsOpen())
		{
			const FString LastError = gitlink::FRepository::Get_LastOpenError();
			const FText ErrText = FText::Format(
				LOCTEXT("ConnectNoRepo",
					"GitLink: no git repository found. {0}"),
				LastError.IsEmpty()
					? LOCTEXT("ConnectNoRepoGeneric", "Check the repository root override in Editor Preferences > GitLink.")
					: FText::FromString(LastError));

			ConnectOp->SetErrorText(ErrText);
			return FCommandResult::Fail(ErrText);
		}

		const FString BranchName = InCtx.Repository->Get_CurrentBranchName();

		UE_LOG(LogGitLink, Log,
			TEXT("Cmd_Connect: connected to '%s' on branch '%s'"),
			*InCtx.RepoRootAbsolute,
			BranchName.IsEmpty() ? TEXT("(detached)") : *BranchName);

		FCommandResult Result = FCommandResult::Ok();
		Result.InfoMessages.Add(FText::Format(
			LOCTEXT("ConnectOk", "Connected to git repository '{0}' on branch '{1}'."),
			FText::FromString(InCtx.RepoRootAbsolute),
			FText::FromString(BranchName)));

		// Initial status population — two stages:
		//
		//   Stage 1: run the dirty-status scan to get states for everything the editor should
		//            mark as modified / added / deleted / etc. These are the high-value states.
		//
		//   Stage 2: enumerate the git index and emit Unmodified states for every tracked file
		//            NOT already in the dirty snapshot. Without this, content-browser right-click
		//            menus would show everything as grayed-out for clean files because
		//            IsSourceControlled() returns false for default (Tree=NotInRepo) states.
		//
		// Both stages feed into Result.UpdatedStates which the dispatcher merges into the cache
		// on the game thread before broadcasting OnSourceControlStateChanged.
		FCommandResult StatusResult = UpdateStatus(InCtx, InOperation, /*InFiles=*/ {});
		if (StatusResult.bOk)
		{
			Result.UpdatedStates = MoveTemp(StatusResult.UpdatedStates);
		}
		else
		{
			// Don't fail Connect on a status failure — the repo is open, user can click Refresh.
			UE_LOG(LogGitLink, Warning,
				TEXT("Cmd_Connect: dirty-status scan failed, continuing"));
		}

		const int32 DirtyCount = Result.UpdatedStates.Num();

		// Stamp Lock state on the dirty states too — dirty files can still be lockable assets
		// (e.g. a modified .uasset) and we want CanCheckout / IsCheckedOutOther to report the
		// right thing for them once we populate actual lock-by-others info in a follow-up.
		for (const FGitLink_FileStateRef& State : Result.UpdatedStates)
		{
			const bool bLockable = InCtx.Provider.Is_FileLockable(State->GetFilename());
			State->_State.Lock = bLockable ? EGitLink_LockState::NotLocked : EGitLink_LockState::Unlockable;
		}

		// Build a set of the dirty-file paths we've already emitted, so we don't clobber a real
		// Modified/Added/Deleted state with an Unmodified one from the index enumeration.
		TSet<FString> DirtyPaths;
		DirtyPaths.Reserve(DirtyCount);
		for (const FGitLink_FileStateRef& State : Result.UpdatedStates)
		{
			DirtyPaths.Add(State->GetFilename());
		}

		const TArray<FString> TrackedRelative = gitlink::op::Enumerate_TrackedFiles(*InCtx.Repository);

		int32 CleanCount = 0;
		int32 LockableCount = 0;
		Result.UpdatedStates.Reserve(Result.UpdatedStates.Num() + TrackedRelative.Num());
		for (const FString& Relative : TrackedRelative)
		{
			const FString Absolute = Normalize_AbsolutePath(FPaths::Combine(InCtx.RepoRootAbsolute, Relative));
			if (DirtyPaths.Contains(Absolute))
			{ continue; }  // dirty snapshot already has this file, don't overwrite

			const bool bLockable = InCtx.Provider.Is_FileLockable(Absolute);

			FGitLink_CompositeState Composite;
			Composite.File = EGitLink_FileState::Unknown;
			Composite.Tree = EGitLink_TreeState::Unmodified;
			Composite.Lock = bLockable ? EGitLink_LockState::NotLocked : EGitLink_LockState::Unlockable;

			Result.UpdatedStates.Add(Make_FileState(Absolute, Composite));
			++CleanCount;
			if (bLockable) { ++LockableCount; }
		}

		// NOTE: tracked-clean submodule files are intentionally NOT pre-populated here.
		// Provider::GetState consults Provider::Is_TrackedInSubmodule (built at connect
		// time from each submodule's git index) and stamps Tree=Unmodified on demand
		// for tracked submodule files, or leaves Tree=NotInRepo for brand-new files —
		// which is what blocks the misleading "check out?" prompt for new files in
		// submodules. Pre-populating here would risk path-casing key mismatches with
		// editor queries, which the on-demand path avoids by reusing the editor's
		// exact normalized path.

		// Stage 3: query LFS locks via /locks/verify so the editor shows accurate lock state
		// from the start — including files locked by OTHER users.
		//
		// The verify endpoint returns server-authoritative `ours` / `theirs` partitions, which
		// avoids comparing usernames (`git config user.name` can differ from the LFS server
		// identity). Default transport is the in-process HTTP client; subprocess is the
		// per-repo fallback when endpoint resolution or auth fails.
		//
		// Submodules: each submodule is polled independently against its own LFS endpoint.
		// Results are merged into a single absolute-path-keyed map before applying.
		if (InCtx.Subprocess != nullptr && InCtx.Subprocess->IsValid() && InCtx.Provider.Is_LfsAvailable())
		{
			TMap<FString, FString> RemoteLocksAbs;
			TSet<FString>          OursAbs;

			auto AbsolutizeFor = [](const FString& InRepoRoot, const FString& InRelPath) -> FString
			{
				return Normalize_AbsolutePath(FPaths::Combine(InRepoRoot, InRelPath));
			};

			// Build the (repo_root, cwd_override) list — parent first, then each LFS-using
			// submodule. Non-LFS submodules (source-only plugins, etc.) are skipped — they
			// don't have lockable content so polling their LFS endpoints is wasted work.
			const TArray<FString> LfsSubmodulePaths = InCtx.Provider.Get_LfsSubmodulePaths();
			TArray<TPair<FString, FString>> RepoPolls;
			RepoPolls.Reserve(1 + LfsSubmodulePaths.Num());
			RepoPolls.Add({ InCtx.RepoRootAbsolute, FString() });
			for (const FString& SubRoot : LfsSubmodulePaths)
			{ RepoPolls.Add({ SubRoot, SubRoot }); }

			// Each `git lfs locks --verify --json` is a network call; doing 34 of them serially
			// stalls Connect for ~30s. Run them concurrently — FGitLink_Subprocess::Run is
			// thread-safe (only reads const _GitBinary/_WorkingDirectory and spawns a subprocess
			// per call). Merging into the maps happens sequentially after all complete.
			TArray<FGitLink_Subprocess::FLfsLocksSnapshot> Snapshots;
			Snapshots.SetNum(RepoPolls.Num());

			const bool bUseHttp = gitlink::lfs_http::Is_Enabled()
				&& InCtx.Provider.Get_LfsHttpClient() != nullptr;

			ParallelFor_BoundedConcurrency(RepoPolls.Num(), GMaxConcurrentRepoWorkers, [&](int32 Idx)
			{
				const FString& RepoRoot = RepoPolls[Idx].Key;
				if (bUseHttp && InCtx.Provider.Get_LfsHttpClient()->Has_LfsUrl(RepoRoot))
				{
					Snapshots[Idx] = InCtx.Provider.Get_LfsHttpClient()->Request_LocksVerify(RepoRoot);
					if (Snapshots[Idx].bSuccess) { return; }
				}
				Snapshots[Idx] = InCtx.Subprocess->QueryLfsLocks_Verified(RepoPolls[Idx].Value);
			});

			for (int32 Idx = 0; Idx < RepoPolls.Num(); ++Idx)
			{
				const FString& RepoRoot = RepoPolls[Idx].Key;
				const FGitLink_Subprocess::FLfsLocksSnapshot& Snap = Snapshots[Idx];
				for (const auto& [RelPath, Owner] : Snap.AllLocks)
				{
					const FString Abs = AbsolutizeFor(RepoRoot, RelPath);
					RemoteLocksAbs.Add(Abs, Owner);
					if (Snap.OursPaths.Contains(RelPath))
					{ OursAbs.Add(Abs); }
				}
			}

			int32 OurLockCount   = 0;
			int32 OtherLockCount = 0;

			for (const auto& [Absolute, LockOwner] : RemoteLocksAbs)
			{
				const bool bIsOurs = OursAbs.Contains(Absolute);
				const EGitLink_LockState LockState = bIsOurs
					? EGitLink_LockState::Locked
					: EGitLink_LockState::LockedOther;

				// Find or create a state entry for this locked file.
				bool bFound = false;
				for (const FGitLink_FileStateRef& State : Result.UpdatedStates)
				{
					if (State->GetFilename().Equals(Absolute, ESearchCase::IgnoreCase))
					{
						State->_State.Lock     = LockState;
						State->_State.LockUser = LockOwner;
						bFound = true;
						break;
					}
				}

				if (!bFound)
				{
					FGitLink_CompositeState Composite;
					Composite.File         = EGitLink_FileState::Unknown;
					Composite.Tree         = EGitLink_TreeState::Unmodified;
					Composite.Lock         = LockState;
					Composite.LockUser     = LockOwner;
					Composite.bInSubmodule = InCtx.Provider.Is_InSubmodule(Absolute);
					Result.UpdatedStates.Add(Make_FileState(Absolute, Composite));
				}

				if (bIsOurs) { ++OurLockCount; } else { ++OtherLockCount; }
			}

			if (OurLockCount > 0 || OtherLockCount > 0)
			{
				UE_LOG(LogGitLink, Log,
					TEXT("Cmd_Connect: discovered %d lock(s) held by us, %d by others (across 1 parent + %d LFS submodule(s))"),
					OurLockCount, OtherLockCount, LfsSubmodulePaths.Num());
			}
		}

		UE_LOG(LogGitLink, Log,
			TEXT("Cmd_Connect: initial state cache = %d dirty + %d unmodified-tracked file(s) ")
			TEXT("(%d lockable)"),
			DirtyCount, CleanCount, LockableCount);

		return Result;
	}
}

#undef LOCTEXT_NAMESPACE
