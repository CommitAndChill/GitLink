#include "GitLink_CommandDispatcher.h"

#include "GitLink/GitLink_Provider.h"
#include "GitLinkLog.h"

#include <SourceControlOperations.h>

// --------------------------------------------------------------------------------------------------------------------
// Pass B stub. The full dispatcher with the FName -> FCommandFn handler map and the
// per-op files (Cmd_Connect.cpp, Cmd_UpdateStatus.cpp, ...) lands in the next pass. For now
// Dispatch() logs the operation name and reports Succeeded so the editor can select the
// provider, call Connect, and not explode.
// --------------------------------------------------------------------------------------------------------------------

FGitLink_CommandDispatcher::FGitLink_CommandDispatcher(FGitLink_Provider& InOwner)
	: _Owner(InOwner)
{
}

FGitLink_CommandDispatcher::~FGitLink_CommandDispatcher() = default;

auto FGitLink_CommandDispatcher::Dispatch(
	const FSourceControlOperationRef& InOperation,
	const TArray<FString>& InFiles,
	EConcurrency::Type /*InConcurrency*/,
	const FSourceControlOperationComplete& InCompleteDelegate) -> ECommandResult::Type
{
	UE_LOG(LogGitLink, Log,
		TEXT("Dispatch[stub]: op='%s' files=%d -> Succeeded"),
		*InOperation->GetName().ToString(),
		InFiles.Num());

	// Fire the completion delegate immediately with Succeeded so callers that wait on it don't hang.
	InCompleteDelegate.ExecuteIfBound(InOperation, ECommandResult::Succeeded);
	return ECommandResult::Succeeded;
}

auto FGitLink_CommandDispatcher::CanCancel(const FSourceControlOperationRef& /*InOperation*/) const -> bool
{
	return false;
}

auto FGitLink_CommandDispatcher::Cancel(const FSourceControlOperationRef& /*InOperation*/) -> void
{
}

auto FGitLink_CommandDispatcher::Tick() -> void
{
}
