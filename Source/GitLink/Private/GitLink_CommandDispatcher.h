#pragma once

#include <CoreMinimal.h>
#include <ISourceControlProvider.h>

class FGitLink_Provider;

// --------------------------------------------------------------------------------------------------------------------
// FGitLink_CommandDispatcher — routes FSourceControlOperation instances to free-function
// handlers in namespace gitlink::cmd. Replaces the 13-subclass IGitSourceControlWorker hierarchy
// in the existing plugin with a flat map of FName -> TFunction.
//
// PASS B: stub only. The actual dispatcher wiring + command handlers land in the next pass.
// This file exists so FGitLink_Provider can hold a TUniquePtr to it without incomplete-type
// errors at Provider destructor-instantiation time.
// --------------------------------------------------------------------------------------------------------------------

class FGitLink_CommandDispatcher
{
public:
	explicit FGitLink_CommandDispatcher(FGitLink_Provider& InOwner);
	~FGitLink_CommandDispatcher();

	FGitLink_CommandDispatcher(const FGitLink_CommandDispatcher&)            = delete;
	FGitLink_CommandDispatcher& operator=(const FGitLink_CommandDispatcher&) = delete;

	// Dispatch an operation. Stub in Pass B — returns Succeeded without doing anything so the
	// editor does not error on Connect/UpdateStatus while we build the real dispatcher.
	auto Dispatch(
		const FSourceControlOperationRef& InOperation,
		const TArray<FString>& InFiles,
		EConcurrency::Type InConcurrency,
		const FSourceControlOperationComplete& InCompleteDelegate) -> ECommandResult::Type;

	auto CanCancel(const FSourceControlOperationRef& InOperation) const -> bool;
	auto Cancel   (const FSourceControlOperationRef& InOperation)       -> void;

	auto Tick() -> void;

private:
	FGitLink_Provider& _Owner;
};
