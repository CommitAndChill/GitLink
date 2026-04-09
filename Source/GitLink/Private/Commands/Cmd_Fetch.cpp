#include "GitLink_CommandDispatcher.h"

#include "GitLinkCore/Repository/GitLink_Repository.h"
#include "GitLinkCore/Repository/GitLink_Repository_Params.h"

#define LOCTEXT_NAMESPACE "GitLinkCmdFetch"

// --------------------------------------------------------------------------------------------------------------------
// Cmd_Fetch — git fetch origin. Updates remote-tracking refs without touching the working tree.
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

		gitlink::FFetchParams Params;
		Params.RemoteName = TEXT("origin");

		const FResult FetchRes = InCtx.Repository->Fetch(Params, /*InProgress=*/ nullptr);
		if (!FetchRes)
		{
			return FCommandResult::Fail(FText::FromString(FetchRes.ErrorMessage));
		}

		FCommandResult Result = FCommandResult::Ok();
		Result.InfoMessages.Add(LOCTEXT("FetchOk", "Fetched from 'origin'."));
		return Result;
	}
}

#undef LOCTEXT_NAMESPACE
