#pragma once

#include <CoreMinimal.h>
#include <Containers/Ticker.h>

class FGitLink_Provider;

// --------------------------------------------------------------------------------------------------------------------
// FGitLink_BackgroundPoll — periodically fetches from origin and refreshes the file status
// cache so the editor's content browser markers stay current as files change on disk or as
// other team members push commits.
//
// Lifecycle:
//   - Constructed by FGitLink_Provider::Init after the repo is open
//   - Start() registers a FTSTicker delegate on the game thread
//   - Stop() unregisters it (idempotent)
//   - Destroyed by FGitLink_Provider::Close
//
// Both Fetch and UpdateStatus are dispatched through the normal command dispatcher so they
// share the same threading / state-merge / broadcast path as user-initiated operations.
// --------------------------------------------------------------------------------------------------------------------

class FGitLink_BackgroundPoll
{
public:
	explicit FGitLink_BackgroundPoll(FGitLink_Provider& InOwner);
	~FGitLink_BackgroundPoll();

	FGitLink_BackgroundPoll(const FGitLink_BackgroundPoll&)            = delete;
	FGitLink_BackgroundPoll& operator=(const FGitLink_BackgroundPoll&) = delete;

	auto Start(float InIntervalSeconds) -> void;
	auto Stop() -> void;
	auto IsRunning() const -> bool { return _TickerHandle.IsValid(); }

	// Temporarily pauses the poll for the duration of the returned scope guard. Used by
	// commands that modify the working tree (commit, revert) so the poll doesn't race.
	struct FScopedPause
	{
		FScopedPause(FGitLink_BackgroundPoll& InOwner) : _Owner(InOwner) { ++_Owner._PauseCount; }
		~FScopedPause() { --_Owner._PauseCount; }
		FGitLink_BackgroundPoll& _Owner;
	};

private:
	auto OnTick(float InDeltaTime) -> bool;

	FGitLink_Provider& _Owner;
	FTSTicker::FDelegateHandle _TickerHandle;
	float _IntervalSeconds = 30.f;
	float _TimeSinceLastPoll = 0.f;
	TAtomic<int32> _PauseCount{0};
};
