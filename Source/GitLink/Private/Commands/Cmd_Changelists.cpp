#include "Cmd_Shared.h"
#include "GitLink_CommandDispatcher.h"
#include "GitLinkLog.h"

#include "GitLink/GitLink_Changelist.h"

#include "GitLinkCore/Repository/GitLink_Repository.h"

#define LOCTEXT_NAMESPACE "GitLinkCmdChangelists"

// --------------------------------------------------------------------------------------------------------------------
// Cmd_Changelists — named changelist operations.
//
// GitLink implements Unreal changelists as virtual groupings stored in .git/gitlink/changelists.json.
// The persistence layer + the FMoveToChangelist / FNewChangelist / FDeleteChangelist /
// FEditChangelist operations land with GitLink_Changelists_Store.
//
// PASS 3 STUBS: all operations return success so the editor's 'Move to changelist' menu
// doesn't spew errors, but nothing actually happens. The static default "Working" and "Staged"
// changelists (declared in GitLink_Changelist.cpp) are the only ones the editor sees until
// the real store lands.
// --------------------------------------------------------------------------------------------------------------------

namespace gitlink::cmd
{
	auto NewChangelist(
		FCommandContext&                  /*InCtx*/,
		const FSourceControlOperationRef& /*InOperation*/,
		const TArray<FString>&            /*InFiles*/) -> FCommandResult
	{
		UE_LOG(LogGitLink, Log, TEXT("Cmd_NewChangelist[stub]"));
		return FCommandResult::Ok();
	}

	auto DeleteChangelist(
		FCommandContext&                  /*InCtx*/,
		const FSourceControlOperationRef& /*InOperation*/,
		const TArray<FString>&            /*InFiles*/) -> FCommandResult
	{
		UE_LOG(LogGitLink, Log, TEXT("Cmd_DeleteChangelist[stub]"));
		return FCommandResult::Ok();
	}

	auto EditChangelist(
		FCommandContext&                  /*InCtx*/,
		const FSourceControlOperationRef& /*InOperation*/,
		const TArray<FString>&            /*InFiles*/) -> FCommandResult
	{
		UE_LOG(LogGitLink, Log, TEXT("Cmd_EditChangelist[stub]"));
		return FCommandResult::Ok();
	}

	auto MoveToChangelist(
		FCommandContext&                  /*InCtx*/,
		const FSourceControlOperationRef& /*InOperation*/,
		const TArray<FString>&            InFiles) -> FCommandResult
	{
		UE_LOG(LogGitLink, Log,
			TEXT("Cmd_MoveToChangelist[stub]: %d file(s) — persistence layer pending"),
			InFiles.Num());
		return FCommandResult::Ok();
	}

	auto UpdateChangelistsStatus(
		FCommandContext&                  InCtx,
		const FSourceControlOperationRef& /*InOperation*/,
		const TArray<FString>&            /*InFiles*/) -> FCommandResult
	{
		if (InCtx.Repository == nullptr || !InCtx.Repository->IsOpen())
		{
			return FCommandResult::Fail(LOCTEXT("ChangelistsNoRepo",
				"GitLink: cannot update changelists — repository not open."));
		}

		// Run a fresh git status snapshot instead of reading the (potentially stale) file state
		// cache. This ensures View Changes reflects the working tree as it is RIGHT NOW, not as
		// it was the last time Cmd_Connect or Cmd_UpdateStatus ran.
		const FStatus Snapshot = InCtx.Repository->Get_Status();

		TMap<FString, FGitLink_CompositeState> CompositeByPath;
		CompositeByPath.Reserve(Snapshot.Num());

		AppendBucket(Snapshot.Staged,     EStatusBucket::Staged,     InCtx.RepoRootAbsolute, CompositeByPath);
		AppendBucket(Snapshot.Unstaged,   EStatusBucket::Unstaged,   InCtx.RepoRootAbsolute, CompositeByPath);
		AppendBucket(Snapshot.Conflicted, EStatusBucket::Conflicted, InCtx.RepoRootAbsolute, CompositeByPath);

		// Content directory suffix — only files under a /Content/ directory are shown in
		// View Changes, matching the existing GitSourceControl plugin's behaviour.
		const FString ContentPathSegment = TEXT("/Content/");
		const FDateTime Now = FDateTime::UtcNow();

		FGitLink_ChangelistStateRef WorkingState = MakeShared<FGitLink_ChangelistState, ESPMode::ThreadSafe>(
			FGitLink_Changelist::WorkingChangelist, TEXT("Working changes"));
		FGitLink_ChangelistStateRef StagedState = MakeShared<FGitLink_ChangelistState, ESPMode::ThreadSafe>(
			FGitLink_Changelist::StagedChangelist, TEXT("Staged changes"));

		FCommandResult Result = FCommandResult::Ok();
		Result.UpdatedStates.Reserve(CompositeByPath.Num());

		for (const TPair<FString, FGitLink_CompositeState>& Pair : CompositeByPath)
		{
			const FString& AbsolutePath = Pair.Key;
			const FGitLink_CompositeState& Composite = Pair.Value;

			// Only show files under a Content/ directory.
			if (!AbsolutePath.Contains(ContentPathSegment))
			{ continue; }

			// Build a file state and assign it to the correct changelist.
			FGitLink_FileStateRef FileState = MakeShared<FGitLink_FileState, ESPMode::ThreadSafe>(AbsolutePath);
			FileState->_State     = Composite;
			FileState->_TimeStamp = Now;

			// Carry forward the existing lock state from the cache so that a prior Locked
			// (from Cmd_CheckOut) isn't clobbered to NotLocked.
			const FGitLink_FileStateRef Existing = InCtx.StateCache.Find_FileState(AbsolutePath);
			if (Existing->_State.Lock != EGitLink_LockState::Unknown)
			{
				FileState->_State.Lock     = Existing->_State.Lock;
				FileState->_State.LockUser = Existing->_State.LockUser;
			}
			else
			{
				const bool bLockable = InCtx.Provider.Is_FileLockable(AbsolutePath);
				FileState->_State.Lock = bLockable
					? EGitLink_LockState::NotLocked
					: EGitLink_LockState::Unlockable;
			}

			if (Composite.Tree == EGitLink_TreeState::Staged)
			{
				FileState->_Changelist = FGitLink_Changelist::StagedChangelist;
				StagedState->_Files.Add(FileState);
			}
			else
			{
				FileState->_Changelist = FGitLink_Changelist::WorkingChangelist;
				WorkingState->_Files.Add(FileState);
			}

			// Also emit as UpdatedStates so the file state cache gets refreshed. This ensures
			// that after View Changes opens, the content browser icons reflect current reality.
			Result.UpdatedStates.Add(FileState);
		}

		WorkingState->_TimeStamp = Now;
		StagedState->_TimeStamp  = Now;

		Result.UpdatedChangelistStates.Add(WorkingState);
		Result.UpdatedChangelistStates.Add(StagedState);

		UE_LOG(LogGitLink, Log,
			TEXT("Cmd_UpdateChangelistsStatus: %d working, %d staged (fresh scan: %d dirty total)"),
			WorkingState->_Files.Num(), StagedState->_Files.Num(), CompositeByPath.Num());

		return Result;
	}
}

#undef LOCTEXT_NAMESPACE
