#include "Cmd_Shared.h"
#include "GitLink_CommandDispatcher.h"

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

		// Stage 3: query existing LFS locks so reconnecting doesn't lose lock state.
		// Without this, a reconnect after a successful checkout would reset Lock to NotLocked
		// and the editor would try to check out again (failing with "Lock exists").
		if (InCtx.Subprocess != nullptr && InCtx.Subprocess->IsValid() && InCtx.Provider.Is_LfsAvailable())
		{
			const TMap<FString, FString> LocalLocks = InCtx.Subprocess->QueryLfsLocks_Local();
			const FString& CurrentUser = InCtx.Provider.Get_UserName();
			int32 LockCount = 0;

			for (const auto& [RelPath, LockOwner] : LocalLocks)
			{
				const FString Absolute = Normalize_AbsolutePath(
					FPaths::Combine(InCtx.RepoRootAbsolute, RelPath));

				// Find or create a state entry for this locked file.
				bool bFound = false;
				for (const FGitLink_FileStateRef& State : Result.UpdatedStates)
				{
					if (State->GetFilename().Equals(Absolute, ESearchCase::IgnoreCase))
					{
						State->_State.Lock     = EGitLink_LockState::Locked;
						State->_State.LockUser = LockOwner;
						bFound = true;
						break;
					}
				}

				if (!bFound)
				{
					// File is locked but wasn't in the dirty or tracked scan — add it.
					FGitLink_CompositeState Composite;
					Composite.File     = EGitLink_FileState::Unknown;
					Composite.Tree     = EGitLink_TreeState::Unmodified;
					Composite.Lock     = EGitLink_LockState::Locked;
					Composite.LockUser = LockOwner;
					Result.UpdatedStates.Add(Make_FileState(Absolute, Composite));
				}

				++LockCount;
			}

			if (LockCount > 0)
			{
				UE_LOG(LogGitLink, Log,
					TEXT("Cmd_Connect: discovered %d existing LFS lock(s) held locally"),
					LockCount);
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
