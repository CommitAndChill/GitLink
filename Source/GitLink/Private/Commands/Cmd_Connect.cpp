#include "Cmd_Shared.h"
#include "GitLink_CommandDispatcher.h"

#include "GitLink/GitLink_Provider.h"
#include "GitLinkLog.h"

#include "GitLinkCore/Repository/GitLink_Repository.h"

#include <CoreGlobals.h>
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
			// Read the open failure from the provider, NOT FRepository::Get_LastOpenError() —
			// that error channel is thread_local (the open failed on the game thread during
			// CheckRepositoryStatus; this handler may run on a task-graph worker, where the
			// thread-local slot is empty).
			const FText LastError = InCtx.Provider.Get_LastConnectError();
			const FText ErrText = FText::Format(
				LOCTEXT("ConnectNoRepo",
					"GitLink: no git repository found. {0}"),
				LastError.IsEmpty()
					? LOCTEXT("ConnectNoRepoGeneric", "Check the repository root override in Editor Preferences > GitLink.")
					: LastError);

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
		//   Stage 1: run the full UpdateStatus scan. This produces states for everything dirty
		//            AND — because UpdateStatus force-runs its cross-repo sweep when invoked
		//            from Connect — discovers all LFS locks (ours + other users', per-repo,
		//            with per-repo bSuccess gating and the replica-lag guard). Lock discovery
		//            used to be a separate Stage 3 here; it was deleted because it duplicated
		//            the sweep (double-polling every repo on first connect) and lacked the
		//            failed-poll gating, so a transient network blip during a reconnect wiped
		//            teammates' LockedOther state until the next timer sweep.
		//
		//   Stage 2: enumerate the git index and emit Unmodified states for every tracked file
		//            NOT already in the dirty snapshot. Without this, content-browser right-click
		//            menus would show everything as grayed-out for clean files because
		//            IsSourceControlled() returns false for default (Tree=NotInRepo) states.
		//
		// Both stages feed into Result.UpdatedStates which the dispatcher merges into the cache
		// on the game thread before broadcasting OnSourceControlStateChanged.
		//
		// Lock state on Stage-1 states is owned by UpdateStatus (carry-forward + sweep). Do NOT
		// re-stamp it here — an earlier revision overwrote every state with the NotLocked /
		// Unlockable default, destroying the sweep's results.
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

		// Build a set of the paths we've already emitted (dirty files + locked files from the
		// sweep), so we don't clobber a real Modified/Added/Deleted/Locked state with an
		// Unmodified one from the index enumeration.
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

		// NOTE: LFS lock discovery (formerly "Stage 3" here) lives inside the UpdateStatus call
		// above — its cross-repo sweep force-runs for the Connect operation. Keeping a second
		// sweep here double-polled every repo's /locks/verify endpoint on connect, and this
		// copy had drifted behind UpdateStatus's (no per-repo bSuccess gating, no replica-lag
		// guard), so a transient poll failure during a reconnect erased lock state.

		UE_LOG(LogGitLink, Log,
			TEXT("Cmd_Connect: initial state cache = %d dirty + %d unmodified-tracked file(s) ")
			TEXT("(%d lockable)"),
			DirtyCount, CleanCount, LockableCount);

		return Result;
	}
}

#undef LOCTEXT_NAMESPACE
