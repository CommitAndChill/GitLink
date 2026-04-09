#include "GitLink_CommandDispatcher.h"
#include "GitLinkLog.h"

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
		FCommandContext&                  /*InCtx*/,
		const FSourceControlOperationRef& /*InOperation*/,
		const TArray<FString>&            /*InFiles*/) -> FCommandResult
	{
		return FCommandResult::Ok();
	}
}

#undef LOCTEXT_NAMESPACE
