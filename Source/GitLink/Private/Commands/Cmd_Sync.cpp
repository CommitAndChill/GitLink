#include "GitLink_CommandDispatcher.h"
#include "GitLinkLog.h"

#include "GitLinkCore/Repository/GitLink_Repository.h"
#include "GitLinkCore/Repository/GitLink_Repository_Params.h"

#define LOCTEXT_NAMESPACE "GitLinkCmdSync"

// --------------------------------------------------------------------------------------------------------------------
// Cmd_Sync — "git pull --ff-only". The editor issues this when the user clicks "Sync" in the
// source control menu. Non-fast-forward scenarios (divergent history, needed merge commit)
// return a clear error pointing at CLI resolution; automating merge resolution is out of scope
// for v1 and deferred to a follow-up along with Cmd_Resolve enhancements.
// --------------------------------------------------------------------------------------------------------------------

namespace gitlink::cmd
{
	auto Sync(
		FCommandContext&                  InCtx,
		const FSourceControlOperationRef& /*InOperation*/,
		const TArray<FString>&            /*InFiles*/) -> FCommandResult
	{
		if (InCtx.Repository == nullptr || !InCtx.Repository->IsOpen())
		{
			return FCommandResult::Fail(LOCTEXT("NoRepo",
				"GitLink: cannot sync — repository not open."));
		}

		UE_LOG(LogGitLink, Log, TEXT("Cmd_Sync: pulling from 'origin' (fast-forward)"));

		gitlink::FFetchParams Params;
		Params.RemoteName = TEXT("origin");

		const FResult PullRes = InCtx.Repository->PullFastForward(Params, /*InProgress=*/ nullptr);
		if (!PullRes)
		{
			UE_LOG(LogGitLink, Warning,
				TEXT("Cmd_Sync: pull failed: %s"), *PullRes.ErrorMessage);
			return FCommandResult::Fail(FText::FromString(PullRes.ErrorMessage));
		}

		UE_LOG(LogGitLink, Log, TEXT("Cmd_Sync: pull complete (fast-forward)"));

		// UpdateStates is left empty — after a fast-forward the editor will re-query via an
		// automatic UpdateStatus if anything changed on disk, which is when the real per-file
		// state deltas come through.
		FCommandResult Result = FCommandResult::Ok();
		Result.InfoMessages.Add(LOCTEXT("SyncOk",
			"Pulled from 'origin' (fast-forward)."));
		return Result;
	}
}

#undef LOCTEXT_NAMESPACE
