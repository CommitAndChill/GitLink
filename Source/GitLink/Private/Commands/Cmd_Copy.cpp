#include "GitLink_CommandDispatcher.h"
#include "GitLinkLog.h"

// --------------------------------------------------------------------------------------------------------------------
// Cmd_Copy — no-op on git. The editor issues this when a user duplicates an asset. Git
// auto-detects copies by content similarity during diff, so there's nothing to do at the
// SCC-provider layer. The new file will appear as Untracked until the user stages + commits.
// --------------------------------------------------------------------------------------------------------------------

namespace gitlink::cmd
{
	auto Copy(
		FCommandContext&                  /*InCtx*/,
		const FSourceControlOperationRef& /*InOperation*/,
		const TArray<FString>&            InFiles) -> FCommandResult
	{
		UE_LOG(LogGitLink, Verbose,
			TEXT("Cmd_Copy: no-op for %d file(s) (git detects copies automatically)"),
			InFiles.Num());
		return FCommandResult::Ok();
	}
}
