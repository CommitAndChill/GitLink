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
	// Forward declaration — defined later in this file. Called by MoveToChangelist to
	// refresh the View Changes window after a stage/unstage operation.
	auto UpdateChangelistsStatus(
		FCommandContext& InCtx,
		const FSourceControlOperationRef& InOperation,
		const TArray<FString>& InFiles) -> FCommandResult;

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
		FCommandContext&                  InCtx,
		const FSourceControlOperationRef& InOperation,
		const TArray<FString>&            InFiles) -> FCommandResult
	{
		if (InCtx.Repository == nullptr || !InCtx.Repository->IsOpen())
		{
			return FCommandResult::Fail(LOCTEXT("MoveNoRepo",
				"GitLink: cannot move to changelist — repository not open."));
		}

		if (!InCtx.DestinationChangelist.IsValid())
		{
			UE_LOG(LogGitLink, Warning, TEXT("Cmd_MoveToChangelist: no destination changelist"));
			return FCommandResult::Ok();  // silently succeed like the stub did
		}

		// Downcast the destination changelist to get the name.
		const FGitLink_Changelist* DestCL =
			static_cast<const FGitLink_Changelist*>(InCtx.DestinationChangelist.Get());
		const FString DestName = DestCL->Get_Name();

		const bool bToStaged  = DestName == FGitLink_Changelist::StagedChangelist.Get_Name();
		const bool bToWorking = DestName == FGitLink_Changelist::WorkingChangelist.Get_Name();
		if (!bToStaged && !bToWorking)
		{
			// Named changelists — stub for now (persistence layer pending).
			UE_LOG(LogGitLink, Log,
				TEXT("Cmd_MoveToChangelist: named changelist '%s' — %d file(s), persistence pending"),
				*DestName, InFiles.Num());
			return FCommandResult::Ok();
		}

		// Partition by repo and stage/unstage against the owning repository (Pitfall #5 —
		// "always partition first"). The previous code converted every path relative to the
		// OUTER root and ran against the outer index, so moving a submodule file between
		// changelists was a silent no-op (the outer-root-relative path points inside a gitlink
		// entry and matches nothing).
		TArray<FString> BatchErrors;
		const TArray<FRepoBatch> Batches = PartitionByRepo(InCtx, InFiles);
		for (const FRepoBatch& Batch : Batches)
		{
			TUniquePtr<gitlink::FRepository> SubRepo;
			gitlink::IRepository* TargetRepo = InCtx.Repository.Get();
			if (Batch.bIsSubmodule)
			{
				SubRepo = InCtx.Provider.Open_SubmoduleRepositoryFor(Batch.AbsoluteFiles[0]);
				if (!SubRepo.IsValid())
				{
					BatchErrors.Add(FString::Printf(
						TEXT("could not open submodule repo at '%s'"), *Batch.RepoRoot));
					continue;
				}
				TargetRepo = SubRepo.Get();
			}

			// Working → Staged = git add; Staged → Working = git restore --staged.
			const FResult OpResult = bToStaged
				? TargetRepo->Stage(Batch.RelativeFiles)
				: TargetRepo->Unstage(Batch.RelativeFiles);

			if (!OpResult)
			{
				BatchErrors.Add(FString::Printf(
					TEXT("%s in '%s' failed: %s"),
					bToStaged ? TEXT("stage") : TEXT("unstage"),
					*Batch.RepoRoot, *OpResult.ErrorMessage));
				continue;
			}

			UE_LOG(LogGitLink, Log,
				TEXT("Cmd_MoveToChangelist: %s %d file(s) in '%s'"),
				bToStaged ? TEXT("staged") : TEXT("unstaged"),
				Batch.RelativeFiles.Num(), *Batch.RepoRoot);
		}

		if (!BatchErrors.IsEmpty())
		{
			return FCommandResult::Fail(FText::Format(
				LOCTEXT("MoveFailed", "GitLink: move to changelist failed: {0}"),
				FText::FromString(JoinTruncated(BatchErrors))));
		}

		// Re-scan and return changelist states so the View Changes window refreshes
		// immediately without the user needing to hit Refresh.
		const TArray<FString> EmptyFiles;
		return UpdateChangelistsStatus(InCtx, InOperation, EmptyFiles);
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
		//
		// KNOWN LIMITATION: outer-repo only. Dirty files inside submodules don't appear in
		// View Changes (and therefore can't be submitted from a changelist) — they're still
		// fully serviceable via the content browser (right-click Check In with explicit files).
		// Walking every submodule here is deliberately avoided: this command runs on every
		// background poll AND every post-save immediate poll, and a per-submodule status walk
		// per refresh is exactly the CPU spike the v0.3.3 sweep throttle removed. A fix needs
		// either throttle-aware caching of per-submodule status or an explicit user-triggered
		// refresh path.
		const FStatus Snapshot = InCtx.Repository->Get_Status();

		TMap<FString, FGitLink_CompositeState> CompositeByPath;
		CompositeByPath.Reserve(Snapshot.Num());

		AppendBucket(Snapshot.Staged,     EStatusBucket::Staged,     InCtx.RepoRootAbsolute, CompositeByPath);
		AppendBucket(Snapshot.Unstaged,   EStatusBucket::Unstaged,   InCtx.RepoRootAbsolute, CompositeByPath);
		AppendBucket(Snapshot.Conflicted, EStatusBucket::Conflicted, InCtx.RepoRootAbsolute, CompositeByPath);

		// Content directory suffix — only files under a /Content/ directory are shown in
		// View Changes.
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
