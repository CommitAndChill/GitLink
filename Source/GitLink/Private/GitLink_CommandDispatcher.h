#pragma once

#include "GitLink/GitLink_State.h"

#include <CoreMinimal.h>
#include <ISourceControlProvider.h>
#include <Templates/Function.h>

class FGitLink_Provider;
class FGitLink_StateCache;
class FGitLink_Subprocess;
namespace gitlink { class FRepository; }

// --------------------------------------------------------------------------------------------------------------------
// FGitLink_CommandDispatcher — routes FSourceControlOperation instances to free-function handlers
// living under Private/Commands/Cmd_*.cpp in namespace gitlink::cmd. Replaces the 13-subclass
// IGitSourceControlWorker hierarchy in the existing plugin with a flat FName -> TFunction map.
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
		FGitLink_Subprocess*  Subprocess;  // may be nullptr if no git binary configured / Close()d

		// Absolute path to the repository working tree root — copied from the provider so command
		// handlers don't need to take the provider lock to read it.
		FString RepoRootAbsolute;
	};

	struct FCommandResult
	{
		bool bOk = false;
		TArray<FText> InfoMessages;
		TArray<FText> ErrorMessages;

		// State deltas to merge into the cache on the game thread. Commands populate this on
		// their worker thread; the dispatcher applies it when the result comes back.
		TArray<FGitLink_FileStateRef> UpdatedStates;

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
		const FSourceControlOperationComplete& InCompleteDelegate) -> ECommandResult::Type;

	auto CanCancel(const FSourceControlOperationRef& InOperation) const -> bool;
	auto Cancel   (const FSourceControlOperationRef& InOperation)       -> void;

	auto Tick() -> void;

private:
	// Runs the handler synchronously and merges the result into the state cache + fires the
	// completion delegate. Safe to call from any thread — state merge is marshalled to game thread.
	auto Execute_AndComplete(
		gitlink::cmd::FCommandFn          InHandler,
		FSourceControlOperationRef        InOperation,
		TArray<FString>                   InFiles,
		FSourceControlOperationComplete   InCompleteDelegate) -> void;

	FGitLink_Provider& _Owner;
	TMap<FName, gitlink::cmd::FCommandFn> _Handlers;
};
