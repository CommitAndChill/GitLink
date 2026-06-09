#pragma once

#include "GitLink/GitLink_State.h"
#include "GitLink_ChangelistState.h"

#include <CoreMinimal.h>
#include <ISourceControlChangelist.h>
#include <ISourceControlProvider.h>
#include <Templates/Function.h>

#include <atomic>

class FGitLink_Provider;
class FGitLink_StateCache;
class FGitLink_Subprocess;
namespace gitlink { class FRepository; }

// --------------------------------------------------------------------------------------------------------------------
// FGitLink_CommandDispatcher — routes FSourceControlOperation instances to free-function handlers
// living under Private/Commands/Cmd_*.cpp in namespace gitlink::cmd. Uses a flat FName -> TFunction
// map rather than a worker class hierarchy.
//
// Execution model (v1):
//   - EConcurrency::Synchronous : runs the handler inline on the calling thread
//   - EConcurrency::Asynchronous: dispatched via Async(EAsyncExecution::TaskGraphThreadNormalPri)
//
// A dedicated FWorkerQueue with high/low priority lanes is the planned optimization — for now
// UE's TaskGraph is enough because gitlink::FRepository owns its own mutex.
//
// State-cache updates (FCommandResult::UpdatedStates) are always merged on the game thread, so
// OnSourceControlStateChanged can broadcast safely.
// --------------------------------------------------------------------------------------------------------------------

namespace gitlink::cmd
{
	struct FCommandContext
	{
		FGitLink_Provider&    Provider;
		FGitLink_StateCache&  StateCache;
		gitlink::FRepository* Repository;  // may be nullptr if the repo isn't open yet (e.g. during Connect)

		// Snapshot of the provider's subprocess handle. Held by-value so a concurrent
		// Init(force=true) tear-down can't free the underlying object while a command is mid-
		// execution (in particular, while a ParallelFor body is fanned out across workers).
		// Use `Subprocess->...` (operator-> on TSharedPtr) or `!Subprocess.IsValid()` to test;
		// `Subprocess == nullptr` also works because TSharedPtr has a nullptr comparison.
		// May be unset if no git binary is configured or the provider was Close()'d before this
		// context was built. See v0.3.6 entry in CLAUDE.md Version log.
		TSharedPtr<FGitLink_Subprocess> Subprocess;

		// Absolute path to the repository working tree root — copied from the provider so command
		// handlers don't need to take the provider lock to read it.
		FString RepoRootAbsolute;

		// Destination changelist for MoveToChangelist operations. Nullptr for all other ops.
		FSourceControlChangelistPtr DestinationChangelist;
	};

	struct FCommandResult
	{
		bool bOk = false;
		TArray<FText> InfoMessages;
		TArray<FText> ErrorMessages;

		// State deltas to merge into the cache on the game thread. Commands populate this on
		// their worker thread; the dispatcher applies it when the result comes back.
		TArray<FGitLink_FileStateRef> UpdatedStates;

		// Changelist state deltas — populated by Cmd_UpdateChangelistsStatus to tell the
		// dispatcher which files belong to which changelist (View Changes window).
		TArray<FGitLink_ChangelistStateRef> UpdatedChangelistStates;

		static auto Ok() -> FCommandResult
		{
			FCommandResult R;
			R.bOk = true;
			return R;
		}

		static auto Fail(FText InMessage) -> FCommandResult
		{
			FCommandResult R;
			R.bOk = false;
			R.ErrorMessages.Add(MoveTemp(InMessage));
			return R;
		}
	};

	using FCommandFn = TFunction<FCommandResult(
		FCommandContext&                  /*Ctx*/,
		const FSourceControlOperationRef& /*Operation*/,
		const TArray<FString>&            /*Files*/)>;
}

// --------------------------------------------------------------------------------------------------------------------

class FGitLink_CommandDispatcher
{
public:
	explicit FGitLink_CommandDispatcher(FGitLink_Provider& InOwner);
	~FGitLink_CommandDispatcher();

	FGitLink_CommandDispatcher(const FGitLink_CommandDispatcher&)            = delete;
	FGitLink_CommandDispatcher& operator=(const FGitLink_CommandDispatcher&) = delete;

	auto Register(FName InOpName, gitlink::cmd::FCommandFn InHandler) -> void;

	auto Dispatch(
		const FSourceControlOperationRef& InOperation,
		const TArray<FString>& InFiles,
		EConcurrency::Type InConcurrency,
		const FSourceControlOperationComplete& InCompleteDelegate,
		FSourceControlChangelistPtr InChangelist = nullptr) -> ECommandResult::Type;

	auto CanCancel(const FSourceControlOperationRef& InOperation) const -> bool;
	auto Cancel   (const FSourceControlOperationRef& InOperation)       -> void;

	auto Tick() -> void;

	// Called from FGitLink_Provider::Close() at engine exit only. Marks the dispatcher dead so any
	// already-queued game-thread completion no-ops instead of touching the provider, then blocks
	// until every in-flight async command BODY finishes dereferencing the provider on its worker
	// thread. This is what makes it safe for the module to destroy the provider (and its mutexes)
	// next: without it, an in-flight `UpdateStatus` body can fault in e.g. `Was_RecentLocalLockOp`
	// on the provider's freed `_RecentLocalLockOpsLock` (see v0.3.10 in CLAUDE.md's Version log).
	//
	// Deadlock-safe: a command body never blocks on the game thread — it only ENQUEUES its
	// game-thread completion (`AsyncTask(GameThread, ...)`) and returns — so waiting for bodies to
	// drain from the game thread can't deadlock. We deliberately do NOT drain on a routine
	// `Init(force=true)` reconnect Close(): the provider object survives that, so in-flight bodies
	// stay valid, and blocking the game thread up to the ~12s LFS timeout on every reconnect would
	// reintroduce a v0.3.1-class hang.
	auto Shutdown_AndDrain() -> void;

private:
	// Runs the handler synchronously and merges the result into the state cache + fires the
	// completion delegate. Safe to call from any thread — state merge is marshalled to game thread.
	auto Execute_AndComplete(
		gitlink::cmd::FCommandFn          InHandler,
		FSourceControlOperationRef        InOperation,
		TArray<FString>                   InFiles,
		FSourceControlOperationComplete   InCompleteDelegate,
		FSourceControlChangelistPtr       InChangelist) -> ECommandResult::Type;

	FGitLink_Provider& _Owner;
	TMap<FName, gitlink::cmd::FCommandFn> _Handlers;

	// Liveness token shared with every in-flight async command lambda (captured BY VALUE, so the
	// captured copy keeps the atomic alive even after this dispatcher — and the owning provider —
	// is destroyed). `Shutdown_AndDrain()` sets it false; the game-thread completion checks it and
	// bails before touching the (possibly freed) provider. Reading it via a captured copy never
	// dereferences `this`, which is the whole point.
	TSharedRef<std::atomic<bool>> _Alive = MakeShared<std::atomic<bool>>(true);

	// Count of async command BODIES currently executing on worker threads — the window during which
	// a body holds a raw reference to the provider. Incremented on the game thread before the task
	// is launched (so a body is accounted for before `Shutdown_AndDrain()` can observe the count),
	// decremented when the body returns. Drained by `Shutdown_AndDrain()`.
	std::atomic<int32> _InFlightBodies{0};
};
