#pragma once

#include "GitLink/GitLink_Changelist.h"
#include "GitLink/GitLink_State.h"

#include <CoreMinimal.h>
#include <ISourceControlProvider.h>
#include <Runtime/Launch/Resources/Version.h>

namespace gitlink { class FRepository; }
class FGitLink_StateCache;
class FGitLink_CommandDispatcher;

// --------------------------------------------------------------------------------------------------------------------
// FGitLink_Provider — libgit2-backed ISourceControlProvider.
//
// Owned by FGitLinkModule (as a direct member, not a TUniquePtr, so the address stays stable for
// IModularFeatures registration). Init() opens the gitlink::FRepository; Execute() dispatches
// operations via FGitLink_CommandDispatcher (stubbed to Succeeded in this pass).
// --------------------------------------------------------------------------------------------------------------------

class GITLINK_API FGitLink_Provider final : public ISourceControlProvider
{
public:
	FGitLink_Provider();
	~FGitLink_Provider();

	FGitLink_Provider(const FGitLink_Provider&)            = delete;
	FGitLink_Provider& operator=(const FGitLink_Provider&) = delete;

	// ISourceControlProvider -----------------------------------------------------------------------------------------
	auto Init(bool bInForceConnection = true) -> void override;
	auto Close() -> void override;

	auto GetStatusText() const -> FText override;
	auto IsEnabled()     const -> bool  override;
	auto IsAvailable()   const -> bool  override;
	auto GetName() const -> const FName& override;

	auto QueryStateBranchConfig(const FString& InConfigSrc, const FString& InConfigDest) -> bool override;
	auto RegisterStateBranches(const TArray<FString>& InBranchNames, const FString& InContentRootIn) -> void override;
	auto GetStateBranchIndex(const FString& InBranchName) const -> int32 override;
	auto GetStateBranchAtIndex(int32 InBranchIndex, FString& OutBranchName) const -> bool override;

	auto GetState(
		const TArray<FString>& InFiles,
		TArray<FSourceControlStateRef>& OutState,
		EStateCacheUsage::Type InStateCacheUsage) -> ECommandResult::Type override;

#if ENGINE_MAJOR_VERSION >= 5
	auto GetState(
		const TArray<FSourceControlChangelistRef>& InChangelists,
		TArray<FSourceControlChangelistStateRef>& OutState,
		EStateCacheUsage::Type InStateCacheUsage) -> ECommandResult::Type override;
#endif

	auto GetCachedStateByPredicate(
		TFunctionRef<bool(const FSourceControlStateRef&)> InPredicate) const
		-> TArray<FSourceControlStateRef> override;

	auto RegisterSourceControlStateChanged_Handle(
		const FSourceControlStateChanged::FDelegate& InStateChanged) -> FDelegateHandle override;
	auto UnregisterSourceControlStateChanged_Handle(FDelegateHandle InHandle) -> void override;

#if ENGINE_MAJOR_VERSION < 5
	auto Execute(
		const FSourceControlOperationRef& InOperation,
		const TArray<FString>& InFiles,
		EConcurrency::Type InConcurrency = EConcurrency::Synchronous,
		const FSourceControlOperationComplete& InOperationCompleteDelegate = FSourceControlOperationComplete())
		-> ECommandResult::Type override;
#else
	auto Execute(
		const FSourceControlOperationRef& InOperation,
		FSourceControlChangelistPtr InChangelist,
		const TArray<FString>& InFiles,
		EConcurrency::Type InConcurrency = EConcurrency::Synchronous,
		const FSourceControlOperationComplete& InOperationCompleteDelegate = FSourceControlOperationComplete())
		-> ECommandResult::Type override;
#endif

	auto CanCancelOperation(const FSourceControlOperationRef& InOperation) const -> bool override;
	auto CancelOperation   (const FSourceControlOperationRef& InOperation)       -> void override;

	auto UsesLocalReadOnlyState() const -> bool override;
	auto UsesChangelists       () const -> bool override;
	auto UsesCheckout          () const -> bool override;

#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 1
	auto UsesFileRevisions  () const -> bool override;
	auto IsAtLatestRevision () const -> TOptional<bool> override;
	auto GetNumLocalChanges () const -> TOptional<int> override;
#endif

#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 2
	auto AllowsDiffAgainstDepot () const -> bool override;
	auto UsesUncontrolledChangelists() const -> bool override;
	auto UsesSnapshots() const -> bool override;
#endif

#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 3
	auto CanExecuteOperation(const FSourceControlOperationRef& InOperation) const -> bool override;
	auto GetStatus() const -> TMap<EStatus, FString> override;
#endif

	auto Tick() -> void override;
	auto GetLabels(const FString& InMatchingSpec) const -> TArray<TSharedRef<class ISourceControlLabel>> override;

#if ENGINE_MAJOR_VERSION >= 5
	auto GetChangelists(EStateCacheUsage::Type InStateCacheUsage) -> TArray<FSourceControlChangelistRef> override;
#endif

#if SOURCE_CONTROL_WITH_SLATE
	auto MakeSettingsWidget() const -> TSharedRef<class SWidget> override;
#endif

	using ISourceControlProvider::Execute;

	// GitLink helpers ------------------------------------------------------------------------------------------------

	auto Get_StateCache() -> FGitLink_StateCache&;
	auto Get_Repository() const -> gitlink::FRepository*;

	auto Get_PathToRepositoryRoot() const -> const FString& { return _PathToRepositoryRoot; }
	auto Get_UserName()             const -> const FString& { return _UserName; }
	auto Get_UserEmail()            const -> const FString& { return _UserEmail; }
	auto Get_BranchName()           const -> const FString& { return _BranchName; }
	auto Get_RemoteUrl()            const -> const FString& { return _RemoteUrl; }

	// Fire the OnSourceControlStateChanged delegate so the editor re-queries state for visible
	// files. Called from the dispatcher on the game thread after merging command results into
	// the cache.
	auto Broadcast_StateChanged() -> void;

private:
	// Opens the libgit2 repository at ProjectDir (or the configured override) and caches the
	// user / branch / remote metadata. Called from Init(true).
	auto CheckRepositoryStatus() -> void;

	TUniquePtr<gitlink::FRepository>       _Repository;
	TUniquePtr<FGitLink_StateCache>        _StateCache;
	TUniquePtr<FGitLink_CommandDispatcher> _Dispatcher;  // constructed empty in Pass B

	bool _bGitRepositoryFound = false;

	FString _PathToRepositoryRoot;  // resolved working tree root
	FString _UserName;
	FString _UserEmail;
	FString _BranchName;
	FString _RemoteUrl;

	FSourceControlStateChanged _OnSourceControlStateChanged;

	mutable FCriticalSection _LastErrorsLock;
	TArray<FText> _LastErrors;
};
