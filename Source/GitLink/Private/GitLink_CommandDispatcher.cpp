#include "GitLink_CommandDispatcher.h"

#include "GitLink/GitLink_Provider.h"
#include "GitLink_StateCache.h"
#include "GitLinkLog.h"

#include "GitLinkCore/Repository/GitLink_Repository.h"

#include <Async/Async.h>
#include <HAL/PlatformProcess.h>
#include <HAL/PlatformTime.h>
#include <Misc/ScopeExit.h>

// --------------------------------------------------------------------------------------------------------------------
// Forward declarations of the command registration table. As each Cmd_*.cpp lands it adds an
// extern declaration here and a Register call in Register_AllCommands below.
// --------------------------------------------------------------------------------------------------------------------

namespace gitlink::cmd
{
	extern auto Connect     (FCommandContext& Ctx, const FSourceControlOperationRef& Op, const TArray<FString>& Files) -> FCommandResult;
	extern auto UpdateStatus(FCommandContext& Ctx, const FSourceControlOperationRef& Op, const TArray<FString>& Files) -> FCommandResult;
	extern auto MarkForAdd  (FCommandContext& Ctx, const FSourceControlOperationRef& Op, const TArray<FString>& Files) -> FCommandResult;
	extern auto Delete      (FCommandContext& Ctx, const FSourceControlOperationRef& Op, const TArray<FString>& Files) -> FCommandResult;
	extern auto Revert      (FCommandContext& Ctx, const FSourceControlOperationRef& Op, const TArray<FString>& Files) -> FCommandResult;
	extern auto CheckIn     (FCommandContext& Ctx, const FSourceControlOperationRef& Op, const TArray<FString>& Files) -> FCommandResult;
	extern auto Copy        (FCommandContext& Ctx, const FSourceControlOperationRef& Op, const TArray<FString>& Files) -> FCommandResult;
	extern auto Resolve     (FCommandContext& Ctx, const FSourceControlOperationRef& Op, const TArray<FString>& Files) -> FCommandResult;
	extern auto Fetch       (FCommandContext& Ctx, const FSourceControlOperationRef& Op, const TArray<FString>& Files) -> FCommandResult;
	extern auto Sync        (FCommandContext& Ctx, const FSourceControlOperationRef& Op, const TArray<FString>& Files) -> FCommandResult;
	extern auto CheckOut    (FCommandContext& Ctx, const FSourceControlOperationRef& Op, const TArray<FString>& Files) -> FCommandResult;
	extern auto NewChangelist           (FCommandContext&, const FSourceControlOperationRef&, const TArray<FString>&) -> FCommandResult;
	extern auto DeleteChangelist        (FCommandContext&, const FSourceControlOperationRef&, const TArray<FString>&) -> FCommandResult;
	extern auto EditChangelist          (FCommandContext&, const FSourceControlOperationRef&, const TArray<FString>&) -> FCommandResult;
	extern auto MoveToChangelist        (FCommandContext&, const FSourceControlOperationRef&, const TArray<FString>&) -> FCommandResult;
	extern auto UpdateChangelistsStatus (FCommandContext&, const FSourceControlOperationRef&, const TArray<FString>&) -> FCommandResult;
}

namespace
{
	// FSourceControlOperationBase doesn't expose a static name method — the runtime FName comes
	// from GetName() on each instance, which by convention matches the class name minus the "F".
	// We use string literals here matching what ISourceControlOperation::GetName() produces.
	auto Register_AllCommands(FGitLink_CommandDispatcher& InDispatcher) -> void
	{
		InDispatcher.Register(TEXT("Connect"),      &gitlink::cmd::Connect);
		InDispatcher.Register(TEXT("UpdateStatus"), &gitlink::cmd::UpdateStatus);
		InDispatcher.Register(TEXT("MarkForAdd"),   &gitlink::cmd::MarkForAdd);
		InDispatcher.Register(TEXT("Delete"),       &gitlink::cmd::Delete);
		InDispatcher.Register(TEXT("Revert"),       &gitlink::cmd::Revert);
		InDispatcher.Register(TEXT("CheckIn"),      &gitlink::cmd::CheckIn);
		InDispatcher.Register(TEXT("Copy"),         &gitlink::cmd::Copy);
		InDispatcher.Register(TEXT("Resolve"),      &gitlink::cmd::Resolve);
		InDispatcher.Register(TEXT("Fetch"),                   &gitlink::cmd::Fetch);
		InDispatcher.Register(TEXT("Sync"),                    &gitlink::cmd::Sync);
		InDispatcher.Register(TEXT("CheckOut"),                &gitlink::cmd::CheckOut);
		InDispatcher.Register(TEXT("NewChangelist"),           &gitlink::cmd::NewChangelist);
		InDispatcher.Register(TEXT("DeleteChangelist"),        &gitlink::cmd::DeleteChangelist);
		InDispatcher.Register(TEXT("EditChangelist"),          &gitlink::cmd::EditChangelist);
		InDispatcher.Register(TEXT("MoveToChangelist"),        &gitlink::cmd::MoveToChangelist);
		InDispatcher.Register(TEXT("UpdateChangelistsStatus"), &gitlink::cmd::UpdateChangelistsStatus);
	}
}

// --------------------------------------------------------------------------------------------------------------------

FGitLink_CommandDispatcher::FGitLink_CommandDispatcher(FGitLink_Provider& InOwner)
	: _Owner(InOwner)
{
	Register_AllCommands(*this);
}

FGitLink_CommandDispatcher::~FGitLink_CommandDispatcher() = default;

auto FGitLink_CommandDispatcher::Register(FName InOpName, gitlink::cmd::FCommandFn InHandler) -> void
{
	_Handlers.Add(InOpName, MoveTemp(InHandler));
}

// --------------------------------------------------------------------------------------------------------------------
auto FGitLink_CommandDispatcher::Dispatch(
	const FSourceControlOperationRef& InOperation,
	const TArray<FString>& InFiles,
	EConcurrency::Type InConcurrency,
	const FSourceControlOperationComplete& InCompleteDelegate,
	FSourceControlChangelistPtr InChangelist) -> ECommandResult::Type
{
	const FName OpName = InOperation->GetName();
	gitlink::cmd::FCommandFn* Handler = _Handlers.Find(OpName);
	if (Handler == nullptr)
	{
		UE_LOG(LogGitLink, Warning,
			TEXT("Dispatch: no handler registered for operation '%s'"), *OpName.ToString());
		InCompleteDelegate.ExecuteIfBound(InOperation, ECommandResult::Failed);
		return ECommandResult::Failed;
	}

	gitlink::cmd::FCommandFn HandlerCopy = *Handler;

	// Snapshot provider state into the context HERE, on the game thread — the only thread that
	// reassigns the provider's Repository/Subprocess TSharedPtr slots and republishes the
	// connect snapshot. Building it inside the async body (on a worker) raced Init(force=true):
	// the worker could copy a TSharedPtr slot mid-reassignment. The context is moved into the
	// body, which keeps the snapshotted objects alive for its whole run (v0.3.10 follow-up).
	gitlink::cmd::FCommandContext Ctx = Build_Context(InChangelist);

	if (InConcurrency == EConcurrency::Synchronous)
	{
		const ECommandResult::Type SyncResult =
			Execute_AndComplete(MoveTemp(Ctx), MoveTemp(HandlerCopy), InOperation, InFiles, InCompleteDelegate);
		return SyncResult;
	}

	// Async: run on a task-graph thread and let Execute_AndComplete marshal the completion back
	// to the game thread.
	//
	// Account for the body BEFORE launching the task (we're on the game thread here, and so is
	// Shutdown_AndDrain() — so this increment is guaranteed to be visible to any subsequent drain;
	// there is no window where the task is queued but uncounted). The body decrements on the way
	// out via ON_SCOPE_EXIT — through the by-value-captured shared counter, never through `this`,
	// so even a body that outlives the 15s drain backstop can't corrupt freed dispatcher memory
	// with its bookkeeping. While the count is non-zero, Close()-at-exit's drain keeps the
	// provider alive, so the body can safely dereference it.
	_InFlightBodies->fetch_add(1, std::memory_order_acq_rel);
	Async(EAsyncExecution::TaskGraph,
		[
			this,
			Alive       = _Alive,
			InFlight    = _InFlightBodies,
			Ctx         = MoveTemp(Ctx),
			HandlerCopy = MoveTemp(HandlerCopy),
			OpRef       = InOperation,
			Files       = InFiles,
			Complete    = InCompleteDelegate
		]() mutable
		{
			ON_SCOPE_EXIT { InFlight->fetch_sub(1, std::memory_order_acq_rel); };

			// Engine is tearing down — the provider (and this dispatcher) are about to be
			// destroyed. Don't start dereferencing provider state; the drain in
			// FGitLink_Provider::Close() is what's waiting on the decrement above. A body already
			// PAST this point is covered by that same drain (the count keeps the provider alive).
			if (IsEngineExitRequested() || !Alive->load(std::memory_order_acquire))
			{ return; }

			Execute_AndComplete(MoveTemp(Ctx), MoveTemp(HandlerCopy), MoveTemp(OpRef), MoveTemp(Files), MoveTemp(Complete));
		});

	return ECommandResult::Succeeded;
}

// --------------------------------------------------------------------------------------------------------------------
auto FGitLink_CommandDispatcher::Build_Context(FSourceControlChangelistPtr InChangelist) -> gitlink::cmd::FCommandContext
{
	// The TSharedPtr slots this snapshots are reassigned (game thread) without a lock; building
	// the context anywhere else reintroduces the race this design exists to close.
	ensureMsgf(IsInGameThread(), TEXT("Build_Context must run on the game thread"));

	return gitlink::cmd::FCommandContext
	{
		.Provider               = _Owner,
		.StateCache             = _Owner.Get_StateCache(),
		.Repository             = _Owner.Get_Repository(),
		.Subprocess             = _Owner.Get_Subprocess(),
		.RepoRootAbsolute       = _Owner.Get_PathToRepositoryRoot(),
		.DestinationChangelist  = InChangelist,
	};
}

// --------------------------------------------------------------------------------------------------------------------
auto FGitLink_CommandDispatcher::Execute_AndComplete(
	gitlink::cmd::FCommandContext   InContext,
	gitlink::cmd::FCommandFn        InHandler,
	FSourceControlOperationRef      InOperation,
	TArray<FString>                 InFiles,
	FSourceControlOperationComplete InCompleteDelegate) -> ECommandResult::Type
{
	gitlink::cmd::FCommandResult Result;
	{
		// Invoke the handler. Any exceptions here are a programming error — we don't have
		// exception handling in UE code, so a bug would terminate, which is what we want.
		Result = InHandler(InContext, InOperation, InFiles);
	}

	const bool bCommandOk = Result.bOk;

	// Merge state deltas and fire the completion delegate on the game thread, regardless of
	// which thread we ran on. ExecuteOnGameThread_WhenReady handles the "already on game thread"
	// case by executing inline.
	auto ApplyOnGameThread =
		[
			this,
			Alive      = _Alive,
			Result     = MoveTemp(Result),
			OpRef      = MoveTemp(InOperation),
			Complete   = MoveTemp(InCompleteDelegate)
		]() mutable
		{
			// If the provider was torn down (engine exit) after this continuation was queued but
			// before the game thread ran it, `this`/`_Owner` are dangling. The shared Alive token
			// — captured by value, so it outlives the provider — lets us detect that and bail
			// before touching any freed state. (Reading `Alive` never dereferences `this`.)
			if (!Alive->load(std::memory_order_acquire))
			{ return; }

			// Merge updated states into the provider's cache.
			const bool bHadStateUpdates =
				Result.UpdatedStates.Num() > 0 || Result.UpdatedChangelistStates.Num() > 0;
			for (const FGitLink_FileStateRef& State : Result.UpdatedStates)
			{
				_Owner.Get_StateCache().Set_FileState(State->GetFilename(), State);
			}
			for (const FGitLink_ChangelistStateRef& ClState : Result.UpdatedChangelistStates)
			{
				_Owner.Get_StateCache().Set_ChangelistState(ClState->_Changelist, ClState);
			}

			// Log any info / error messages the command produced.
			for (const FText& Info : Result.InfoMessages)
			{
				UE_LOG(LogGitLink, Log, TEXT("%s: %s"),
					*OpRef->GetName().ToString(), *Info.ToString());
			}
			for (const FText& Err : Result.ErrorMessages)
			{
				UE_LOG(LogGitLink, Warning, TEXT("%s: %s"),
					*OpRef->GetName().ToString(), *Err.ToString());
				// Engine API name has a typo ("Messge" not "Message") — sic.
				OpRef->AddErrorMessge(Err);
			}

			Complete.ExecuteIfBound(
				OpRef,
				Result.bOk ? ECommandResult::Succeeded : ECommandResult::Failed);

			// Broadcast to the editor so content-browser markers and state columns refresh.
			// Only worth firing when state actually changed; skip for pure queries.
			if (bHadStateUpdates)
			{
				_Owner.Broadcast_StateChanged();

				// Working-tree-modifying commands (Revert, CheckIn, MarkForAdd, Delete,
				// Resolve, MoveToChangelist, Sync, etc.) should refresh View Changes
				// immediately. Read-only operations are excluded to avoid an infinite poll
				// loop — UpdateStatus and UpdateChangelistsStatus emit UpdatedStates too.
				const FName OpName = OpRef->GetName();
				const bool bIsReadOnlyOp =
				    OpName == TEXT("Connect")
				 || OpName == TEXT("UpdateStatus")
				 || OpName == TEXT("UpdateChangelistsStatus");

				if (!bIsReadOnlyOp)
				{
					_Owner.Request_ImmediatePoll();
				}
			}
		};

	if (IsInGameThread())
	{
		ApplyOnGameThread();
	}
	else
	{
		AsyncTask(ENamedThreads::GameThread, MoveTemp(ApplyOnGameThread));
	}

	return bCommandOk ? ECommandResult::Succeeded : ECommandResult::Failed;
}

// --------------------------------------------------------------------------------------------------------------------
auto FGitLink_CommandDispatcher::Shutdown_AndDrain() -> void
{
	// Neutralize any game-thread completion that's already queued (it captured a copy of this token
	// and will see `false` and bail) before we wait, so we don't race a continuation that re-enters
	// provider state during the drain.
	_Alive->store(false, std::memory_order_release);

	// Block until every in-flight command body has finished dereferencing the provider. Bodies
	// don't block on the game thread (they only enqueue their completion and return), so spinning
	// here — even though we ARE the game thread — can't deadlock. The v0.3.9 IsEngineExitRequested()
	// early-outs inside the LFS poll fan-out make each body return promptly during exit, so in
	// practice this returns almost immediately; the timeout below is a defensive backstop only.
	const double StartSec = FPlatformTime::Seconds();
	while (_InFlightBodies->load(std::memory_order_acquire) > 0)
	{
		FPlatformProcess::Sleep(0.001f);

		if (FPlatformTime::Seconds() - StartSec > 15.0)
		{
			UE_LOG(LogGitLink, Warning,
				TEXT("Shutdown_AndDrain: timed out after 15s with %d command body(ies) still ")
				TEXT("in flight; proceeding with teardown"),
				_InFlightBodies->load(std::memory_order_acquire));
			break;
		}
	}
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
