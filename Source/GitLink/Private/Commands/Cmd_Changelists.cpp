#include "GitLink_CommandDispatcher.h"
#include "GitLink_StateCache.h"
#include "GitLinkLog.h"

#include "GitLink/GitLink_Changelist.h"

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
		// Build changelist states by scanning the file state cache for dirty files.
		// Files with TreeState == Staged go into the Staged changelist; everything else
		// dirty (Working, Untracked, etc.) goes into the Working changelist.
		FGitLink_ChangelistStateRef WorkingState = MakeShared<FGitLink_ChangelistState, ESPMode::ThreadSafe>(
			FGitLink_Changelist::WorkingChangelist, TEXT("Working changes"));
		FGitLink_ChangelistStateRef StagedState = MakeShared<FGitLink_ChangelistState, ESPMode::ThreadSafe>(
			FGitLink_Changelist::StagedChangelist, TEXT("Staged changes"));

		const TArray<FGitLink_FileStateRef> AllFiles = InCtx.StateCache.Enumerate_FileStates(
			[](const FGitLink_FileStateRef& /*State*/) { return true; });

		// Content directory suffix — only files under a /Content/ directory are shown in
		// View Changes, matching the existing GitSourceControl plugin's behaviour. This
		// filters out source code, config, submodule dirs, .claude/, etc.
		const FString ContentPathSegment = TEXT("/Content/");

		for (const FGitLink_FileStateRef& FileState : AllFiles)
		{
			const EGitLink_TreeState Tree = FileState->_State.Tree;
			const EGitLink_FileState File = FileState->_State.File;

			// Only show files under a Content/ directory.
			if (!FileState->GetFilename().Contains(ContentPathSegment))
			{ continue; }

			// Skip clean / non-repo files — they don't belong in any changelist.
			if (Tree == EGitLink_TreeState::Unmodified ||
				Tree == EGitLink_TreeState::Ignored ||
				Tree == EGitLink_TreeState::NotInRepo ||
				Tree == EGitLink_TreeState::Unset)
			{
				// Exception: if the file has a real modification state (Added/Modified/Deleted/Renamed)
				// but the tree state is Unmodified, it's likely a file that was only staged.
				if (File == EGitLink_FileState::Unknown || File == EGitLink_FileState::Missing)
				{ continue; }

				// Fall through to check for staged-only changes
				if (Tree != EGitLink_TreeState::Unmodified)
				{ continue; }
			}

			if (Tree == EGitLink_TreeState::Staged)
			{
				FileState->_Changelist = FGitLink_Changelist::StagedChangelist;
				StagedState->_Files.Add(FileState);
			}
			else
			{
				// Working, Untracked, or any other dirty state
				FileState->_Changelist = FGitLink_Changelist::WorkingChangelist;
				WorkingState->_Files.Add(FileState);
			}
		}

		WorkingState->_TimeStamp = FDateTime::UtcNow();
		StagedState->_TimeStamp  = FDateTime::UtcNow();

		FCommandResult Result = FCommandResult::Ok();
		Result.UpdatedChangelistStates.Add(WorkingState);
		Result.UpdatedChangelistStates.Add(StagedState);

		UE_LOG(LogGitLink, Log,
			TEXT("Cmd_UpdateChangelistsStatus: %d working, %d staged"),
			WorkingState->_Files.Num(), StagedState->_Files.Num());

		return Result;
	}
}

#undef LOCTEXT_NAMESPACE
