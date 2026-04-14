#include "GitLink_BackgroundPoll.h"

#include "GitLink/GitLink_Provider.h"
#include "GitLinkLog.h"

#include <ISourceControlModule.h>
#include <ISourceControlProvider.h>
#include <SourceControlOperations.h>

// --------------------------------------------------------------------------------------------------------------------

FGitLink_BackgroundPoll::FGitLink_BackgroundPoll(FGitLink_Provider& InOwner)
	: _Owner(InOwner)
{
}

auto FGitLink_BackgroundPoll::SetInterval(float InIntervalSeconds) -> void
{
	_IntervalSeconds   = InIntervalSeconds;
	_TimeSinceLastPoll = 0.f;

	if (_IntervalSeconds > 0.f)
	{
		UE_LOG(LogGitLink, Log,
			TEXT("BackgroundPoll: interval set to %.0f seconds"), _IntervalSeconds);
	}
	else
	{
		UE_LOG(LogGitLink, Log, TEXT("BackgroundPoll: disabled (interval <= 0)"));
	}
}

auto FGitLink_BackgroundPoll::Tick(float InDeltaTime) -> void
{
	if (_IntervalSeconds <= 0.f)
	{ return; }

	// Paused by an in-flight operation (commit, revert, etc.)
	if (_PauseCount.Load() > 0)
	{ return; }

	_TimeSinceLastPoll += InDeltaTime;
	if (_TimeSinceLastPoll < _IntervalSeconds)
	{ return; }

	_TimeSinceLastPoll = 0.f;

	if (!_Owner.IsAvailable())
	{ return; }

	// We dispatch through the provider's public Execute path so the full dispatcher → handler →
	// state-cache-merge → broadcast chain fires automatically.
	ISourceControlProvider& Provider = ISourceControlModule::Get().GetProvider();

	// Verify the active provider is actually ours — another plugin could have been switched to.
	if (&Provider != &_Owner)
	{ return; }

	TSharedRef<ISourceControlOperation, ESPMode::ThreadSafe> StatusOp =
		ISourceControlOperation::Create<FUpdateStatus>();

	Provider.Execute(
		StatusOp,
		TArray<FString>(),
		EConcurrency::Asynchronous,
		FSourceControlOperationComplete::CreateLambda(
			[](const FSourceControlOperationRef& /*Op*/, ECommandResult::Type InResult)
			{
				if (InResult != ECommandResult::Succeeded)
				{
					UE_LOG(LogGitLink, Verbose,
						TEXT("BackgroundPoll: status refresh result=%d"),
						static_cast<int32>(InResult));
				}
			}));

	UE_LOG(LogGitLink, Verbose, TEXT("BackgroundPoll: dispatched status refresh"));
}

auto FGitLink_BackgroundPoll::Request_ImmediatePoll() -> void
{
	_TimeSinceLastPoll = _IntervalSeconds;
}
