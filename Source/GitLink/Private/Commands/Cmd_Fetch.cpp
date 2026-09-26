#include "GitLink_CommandDispatcher.h"
#include "GitLinkLog.h"

#include "GitLinkCore/Repository/GitLink_Repository.h"
#include "GitLinkCore/Repository/GitLink_Repository_Params.h"

#define LOCTEXT_NAMESPACE "GitLinkCmdFetch"

// --------------------------------------------------------------------------------------------------------------------
// Cmd_Fetch — git fetch <the current branch's tracked remote, else origin>. Updates remote-tracking refs without touching the working tree.
// Used by the editor's "Check for updates" button and by FGitLink_BackgroundPoll on an interval.
// --------------------------------------------------------------------------------------------------------------------

namespace gitlink::cmd
{
	auto Fetch(
		FCommandContext&                  InCtx,
		const FSourceControlOperationRef& /*InOperation*/,
		const TArray<FString>&            /*InFiles*/) -> FCommandResult
	{
		if (InCtx.Repository == nullptr || !InCtx.Repository->IsOpen())
		{
			return FCommandResult::Fail(LOCTEXT("NoRepo",
				"GitLink: cannot fetch — repository not open."));
		}

		UE_LOG(LogGitLink, Log, TEXT("Cmd_Fetch: fetching from the tracked remote"));

		const gitlink::FFetchParams Params;  // empty RemoteName = the branch's tracked remote

		const FResult FetchRes = InCtx.Repository->Fetch(Params, /*InProgress=*/ nullptr);
		if (!FetchRes)
		{
			UE_LOG(LogGitLink, Warning,
				TEXT("Cmd_Fetch: failed: %s"), *FetchRes.ErrorMessage);
			return FCommandResult::Fail(FText::FromString(FetchRes.ErrorMessage));
		}

		UE_LOG(LogGitLink, Log, TEXT("Cmd_Fetch: fetch complete"));

		FCommandResult Result = FCommandResult::Ok();
		Result.InfoMessages.Add(LOCTEXT("FetchOk", "Fetched from the tracked remote."));
		return Result;
	}
}

#undef LOCTEXT_NAMESPACE
