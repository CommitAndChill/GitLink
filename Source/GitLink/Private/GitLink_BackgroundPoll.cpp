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

FGitLink_BackgroundPoll::~FGitLink_BackgroundPoll()
{
	Stop();
}

auto FGitLink_BackgroundPoll::Start(float InIntervalSeconds) -> void
{
	Stop();

	if (InIntervalSeconds <= 0.f)
	{
		UE_LOG(LogGitLink, Log, TEXT("BackgroundPoll: disabled (interval <= 0)"));
		return;
	}

	_IntervalSeconds    = InIntervalSeconds;
	_TimeSinceLastPoll  = 0.f;

	// FTSTicker fires on the game thread, which is what we want — the dispatch path marshals
	// state-cache merges + OnSourceControlStateChanged broadcasts there anyway.
	_TickerHandle = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateRaw(this, &FGitLink_BackgroundPoll::OnTick),
		/*InDelay=*/ 1.0f  // check once per second; we accumulate until the real interval
	);

	UE_LOG(LogGitLink, Log,
		TEXT("BackgroundPoll: started (every %.0f seconds)"), _IntervalSeconds);
}

auto FGitLink_BackgroundPoll::Stop() -> void
{
	if (_TickerHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(_TickerHandle);
		_TickerHandle.Reset();
		UE_LOG(LogGitLink, Log, TEXT("BackgroundPoll: stopped"));
	}
}

auto FGitLink_BackgroundPoll::OnTick(float InDeltaTime) -> bool
{
	// Paused by an in-flight operation (commit, revert, etc.)
	if (_PauseCount.Load() > 0)
	{ return true; }

	_TimeSinceLastPoll += InDeltaTime;
	if (_TimeSinceLastPoll < _IntervalSeconds)
	{ return true; }

	_TimeSinceLastPoll = 0.f;

	if (!_Owner.IsAvailable())
	{ return true; }

	// We dispatch through the provider's public Execute path so the full dispatcher → handler →
	// state-cache-merge → broadcast chain fires automatically.
	//
	// Note: we use FUpdateStatus (a standard engine operation), not the custom FGitFetch.
	// The Fetch command is registered as "Fetch" in our dispatcher, but the engine doesn't have
	// a built-in ISourceControlOperation class for it. Since FUpdateStatus with an empty file
	// list does a full working-tree status scan AND Cmd_Connect already ran an initial fetch,
	// the poll just refreshes status. A dedicated fetch can be triggered by the user via the
	// source control menu → Sync.

	ISourceControlProvider& Provider = ISourceControlModule::Get().GetProvider();

	// Verify the active provider is actually ours — another plugin could have been switched to.
	if (&Provider != &_Owner)
	{ return true; }

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

	return true;  // keep ticking
}
