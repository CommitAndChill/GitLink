#include "GitLink_CommandDispatcher.h"

#include "GitLink/GitLink_Provider.h"
#include "GitLink_StateCache.h"
#include "GitLinkLog.h"

#include "GitLinkCore/Repository/GitLink_Repository.h"

#include <Async/Async.h>

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
	// We use string literals here to match what the existing GitSourceControl plugin does, which
	// mirrors what ISourceControlOperation::GetName() produces.
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
	const FSourceControlOperationComplete& InCompleteDelegate) -> ECommandResult::Type
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

	if (InConcurrency == EConcurrency::Synchronous)
	{
		const ECommandResult::Type SyncResult =
			Execute_AndComplete(MoveTemp(HandlerCopy), InOperation, InFiles, InCompleteDelegate);
		return SyncResult;
	}

	// Async: run on a task-graph thread and let Execute_AndComplete marshal the completion back
	// to the game thread.
	Async(EAsyncExecution::TaskGraph,
		[
			this,
			HandlerCopy = MoveTemp(HandlerCopy),
			OpRef       = InOperation,
			Files       = InFiles,
			Complete    = InCompleteDelegate
		]() mutable
		{
			Execute_AndComplete(MoveTemp(HandlerCopy), MoveTemp(OpRef), MoveTemp(Files), MoveTemp(Complete));
		});

	return ECommandResult::Succeeded;
}

// --------------------------------------------------------------------------------------------------------------------
auto FGitLink_CommandDispatcher::Execute_AndComplete(
	gitlink::cmd::FCommandFn        InHandler,
	FSourceControlOperationRef      InOperation,
	TArray<FString>                 InFiles,
	FSourceControlOperationComplete InCompleteDelegate) -> ECommandResult::Type
{
	// Build the context snapshotting what the handler needs, so it doesn't have to touch provider
	// internals while running.
	gitlink::cmd::FCommandContext Ctx
	{
		.Provider         = _Owner,
		.StateCache       = _Owner.Get_StateCache(),
		.Repository       = _Owner.Get_Repository(),
		.Subprocess       = _Owner.Get_Subprocess(),
		.RepoRootAbsolute = _Owner.Get_PathToRepositoryRoot(),
	};

	gitlink::cmd::FCommandResult Result;
	{
		// Invoke the handler. Any exceptions here are a programming error — we don't have
		// exception handling in UE code, so a bug would terminate, which is what we want.
		Result = InHandler(Ctx, InOperation, InFiles);
	}

	const bool bCommandOk = Result.bOk;

	// Merge state deltas and fire the completion delegate on the game thread, regardless of
	// which thread we ran on. ExecuteOnGameThread_WhenReady handles the "already on game thread"
	// case by executing inline.
	auto ApplyOnGameThread =
		[
			this,
			Result     = MoveTemp(Result),
			OpRef      = MoveTemp(InOperation),
			Complete   = MoveTemp(InCompleteDelegate)
		]() mutable
		{
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
