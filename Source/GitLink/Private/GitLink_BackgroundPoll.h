#pragma once

#include <CoreMinimal.h>

class FGitLink_Provider;

// --------------------------------------------------------------------------------------------------------------------
// FGitLink_BackgroundPoll — periodically refreshes the file status cache and LFS lock state
// so the editor's content browser markers stay current as files change on disk or as other
// team members lock/unlock files.
//
// Driven by the provider's Tick() (called every frame by ISourceControlModule) rather than
// FTSTicker, which avoids ticker handle issues when the provider re-initializes mid-session.
//
// Lifecycle:
//   - Constructed by FGitLink_Provider::CheckRepositoryStatus after the repo is open
//   - Tick() called from FGitLink_Provider::Tick() every frame
//   - Destroyed by FGitLink_Provider::Close
// --------------------------------------------------------------------------------------------------------------------

class FGitLink_BackgroundPoll
{
public:
	explicit FGitLink_BackgroundPoll(FGitLink_Provider& InOwner);
	~FGitLink_BackgroundPoll() = default;

	FGitLink_BackgroundPoll(const FGitLink_BackgroundPoll&)            = delete;
	FGitLink_BackgroundPoll& operator=(const FGitLink_BackgroundPoll&) = delete;

	auto SetInterval(float InIntervalSeconds) -> void;
	auto IsEnabled() const -> bool { return _IntervalSeconds > 0.f; }

	// Called every frame from the provider's Tick(). Accumulates time and dispatches
	// a status refresh when the interval is reached.
	auto Tick(float InDeltaTime) -> void;

	// Resets the accumulator so the next Tick() fires a poll almost immediately.
	// Used after file save to get sub-2-second View Changes refresh.
	auto Request_ImmediatePoll() -> void;

	// Temporarily pauses the poll for the duration of the returned scope guard. Used by
	// commands that modify the working tree (commit, revert) so the poll doesn't race.
	struct FScopedPause
	{
		FScopedPause(FGitLink_BackgroundPoll& InOwner) : _Owner(InOwner) { ++_Owner._PauseCount; }
		~FScopedPause() { --_Owner._PauseCount; }
		FGitLink_BackgroundPoll& _Owner;
	};

private:
	FGitLink_Provider& _Owner;
	float _IntervalSeconds = 30.f;
	float _TimeSinceLastPoll = 0.f;
	TAtomic<int32> _PauseCount{0};
};
