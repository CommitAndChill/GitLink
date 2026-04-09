#include "GitLink_CommandDispatcher.h"

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
		const TArray<FString>&            /*InFiles*/) -> FCommandResult
	{
		return FCommandResult::Ok();
	}
}
