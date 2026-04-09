#include "GitLink/GitLink_Provider.h"

#include "GitLink/GitLink_Settings.h"
#include "GitLink_CommandDispatcher.h"
#include "GitLink_ChangelistState.h"
#include "GitLink_StateCache.h"
#include "GitLinkLog.h"

#include "GitLinkCore/Repository/GitLink_Repository.h"
#include "GitLinkCore/Repository/GitLink_Repository_Params.h"

#include <Misc/Paths.h>
#include <SourceControlOperations.h>

#define LOCTEXT_NAMESPACE "GitLinkProvider"

// --------------------------------------------------------------------------------------------------------------------

namespace
{
	const FName GProviderName(TEXT("GitLink"));
}

// --------------------------------------------------------------------------------------------------------------------
FGitLink_Provider::FGitLink_Provider()
	: _StateCache(MakeUnique<FGitLink_StateCache>())
	, _Dispatcher(MakeUnique<FGitLink_CommandDispatcher>(*this))
{
}

FGitLink_Provider::~FGitLink_Provider() = default;

// --------------------------------------------------------------------------------------------------------------------
// Lifecycle
// --------------------------------------------------------------------------------------------------------------------
auto FGitLink_Provider::Init(bool bInForceConnection) -> void
{
	UE_LOG(LogGitLink, Log, TEXT("FGitLink_Provider::Init(force=%s)"),
		bInForceConnection ? TEXT("true") : TEXT("false"));

	if (bInForceConnection)
	{
		CheckRepositoryStatus();
	}
}

auto FGitLink_Provider::Close() -> void
{
	UE_LOG(LogGitLink, Log, TEXT("FGitLink_Provider::Close"));

	_Repository.Reset();
	_bGitRepositoryFound = false;
	_PathToRepositoryRoot.Reset();
	_UserName.Reset();
	_UserEmail.Reset();
	_BranchName.Reset();
	_RemoteUrl.Reset();

	if (_StateCache.IsValid())
	{ _StateCache->Clear(); }
}

auto FGitLink_Provider::CheckRepositoryStatus() -> void
{
	_bGitRepositoryFound = false;
	_Repository.Reset();

	const UGitLink_Settings* Settings = GetDefault<UGitLink_Settings>();

	gitlink::FOpenParams Params;
	Params.Path = Settings != nullptr && !Settings->RepositoryRootOverride.IsEmpty()
		? Settings->RepositoryRootOverride
		: FPaths::ProjectDir();

	Params.Path = FPaths::ConvertRelativePathToFull(Params.Path);

	UE_LOG(LogGitLink, Log, TEXT("CheckRepositoryStatus: opening '%s'"), *Params.Path);

	_Repository = gitlink::FRepository::Open(Params);
	if (!_Repository.IsValid())
	{
		const FString& Err = gitlink::FRepository::Get_LastOpenError();
		UE_LOG(LogGitLink, Warning, TEXT("CheckRepositoryStatus: open failed: %s"), *Err);

		FScopeLock Lock(&_LastErrorsLock);
		_LastErrors.Add(FText::FromString(Err));
		return;
	}

	_bGitRepositoryFound    = true;
	_PathToRepositoryRoot   = _Repository->Get_Path();
	_BranchName             = _Repository->Get_CurrentBranchName();

	// Resolve the default remote URL (first remote, usually "origin").
	const TArray<gitlink::FRemote> Remotes = _Repository->Get_Remotes();
	if (Remotes.Num() > 0)
	{
		_RemoteUrl = Remotes[0].FetchUrl;
	}

	UE_LOG(LogGitLink, Log,
		TEXT("CheckRepositoryStatus: repo='%s' branch='%s' remote='%s'"),
		*_PathToRepositoryRoot, *_BranchName, *_RemoteUrl);
}

// --------------------------------------------------------------------------------------------------------------------
// Identity
// --------------------------------------------------------------------------------------------------------------------
auto FGitLink_Provider::GetName() const -> const FName&
{
	return GProviderName;
}

auto FGitLink_Provider::GetStatusText() const -> FText
{
	FFormatNamedArguments Args;
	Args.Add(TEXT("RepositoryRoot"), FText::FromString(_PathToRepositoryRoot));
	Args.Add(TEXT("Branch"),         FText::FromString(_BranchName));
	Args.Add(TEXT("Remote"),         FText::FromString(_RemoteUrl));

	if (!_bGitRepositoryFound)
	{
		return LOCTEXT("StatusNotConnected", "GitLink: no repository");
	}

	return FText::Format(
		LOCTEXT("StatusConnected", "GitLink repo: {RepositoryRoot}\nBranch: {Branch}\nRemote: {Remote}"),
		Args);
}

auto FGitLink_Provider::IsEnabled() const -> bool
{
	return true;
}

auto FGitLink_Provider::IsAvailable() const -> bool
{
	return _bGitRepositoryFound;
}

// --------------------------------------------------------------------------------------------------------------------
// State queries
// --------------------------------------------------------------------------------------------------------------------
auto FGitLink_Provider::GetState(
	const TArray<FString>& InFiles,
	TArray<FSourceControlStateRef>& OutState,
	EStateCacheUsage::Type /*InStateCacheUsage*/) -> ECommandResult::Type
{
	OutState.Reserve(InFiles.Num());
	for (const FString& File : InFiles)
	{
		OutState.Add(_StateCache->GetOrCreate_FileState(File));
	}
	return ECommandResult::Succeeded;
}

#if ENGINE_MAJOR_VERSION >= 5
auto FGitLink_Provider::GetState(
	const TArray<FSourceControlChangelistRef>& InChangelists,
	TArray<FSourceControlChangelistStateRef>& OutState,
	EStateCacheUsage::Type /*InStateCacheUsage*/) -> ECommandResult::Type
{
	OutState.Reserve(InChangelists.Num());
	for (const FSourceControlChangelistRef& Changelist : InChangelists)
	{
		// TSharedRef is never null — safe to downcast directly. Callers are required to only
		// pass in changelists this provider handed out.
		const FGitLink_Changelist* AsGitLink = static_cast<const FGitLink_Changelist*>(&Changelist.Get());
		OutState.Add(_StateCache->GetOrCreate_ChangelistState(*AsGitLink));
	}
	return ECommandResult::Succeeded;
}
#endif

auto FGitLink_Provider::GetCachedStateByPredicate(
	TFunctionRef<bool(const FSourceControlStateRef&)> InPredicate) const -> TArray<FSourceControlStateRef>
{
	TArray<FSourceControlStateRef> Out;
	const TArray<FGitLink_FileStateRef> Matched = _StateCache->Enumerate_FileStates(
		[&InPredicate](const FGitLink_FileStateRef& InFileState) -> bool
		{
			return InPredicate(InFileState);
		});

	Out.Reserve(Matched.Num());
	for (const FGitLink_FileStateRef& State : Matched)
	{
		Out.Add(State);
	}
	return Out;
}

// --------------------------------------------------------------------------------------------------------------------
// State change delegate
// --------------------------------------------------------------------------------------------------------------------
auto FGitLink_Provider::RegisterSourceControlStateChanged_Handle(
	const FSourceControlStateChanged::FDelegate& InStateChanged) -> FDelegateHandle
{
	return _OnSourceControlStateChanged.Add(InStateChanged);
}

auto FGitLink_Provider::UnregisterSourceControlStateChanged_Handle(FDelegateHandle InHandle) -> void
{
	_OnSourceControlStateChanged.Remove(InHandle);
}

// --------------------------------------------------------------------------------------------------------------------
// Execute
// --------------------------------------------------------------------------------------------------------------------
#if ENGINE_MAJOR_VERSION < 5
auto FGitLink_Provider::Execute(
	const FSourceControlOperationRef& InOperation,
	const TArray<FString>& InFiles,
	EConcurrency::Type InConcurrency,
	const FSourceControlOperationComplete& InOperationCompleteDelegate) -> ECommandResult::Type
{
	return _Dispatcher->Dispatch(InOperation, InFiles, InConcurrency, InOperationCompleteDelegate);
}
#else
auto FGitLink_Provider::Execute(
	const FSourceControlOperationRef& InOperation,
	FSourceControlChangelistPtr /*InChangelist*/,
	const TArray<FString>& InFiles,
	EConcurrency::Type InConcurrency,
	const FSourceControlOperationComplete& InOperationCompleteDelegate) -> ECommandResult::Type
{
	return _Dispatcher->Dispatch(InOperation, InFiles, InConcurrency, InOperationCompleteDelegate);
}
#endif

auto FGitLink_Provider::CanCancelOperation(const FSourceControlOperationRef& InOperation) const -> bool
{
	return _Dispatcher->CanCancel(InOperation);
}

auto FGitLink_Provider::CancelOperation(const FSourceControlOperationRef& InOperation) -> void
{
	_Dispatcher->Cancel(InOperation);
}

// --------------------------------------------------------------------------------------------------------------------
// Capability flags
// --------------------------------------------------------------------------------------------------------------------
auto FGitLink_Provider::UsesLocalReadOnlyState() const -> bool
{
	return false;
}

auto FGitLink_Provider::UsesChangelists() const -> bool
{
	return true;
}

auto FGitLink_Provider::UsesCheckout() const -> bool
{
	// "Checkout" in Unreal == acquire LFS lock. Driven by the settings toggle.
	const UGitLink_Settings* Settings = GetDefault<UGitLink_Settings>();
	return Settings != nullptr && Settings->bUseLfsLocking;
}

#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 1
auto FGitLink_Provider::UsesFileRevisions() const -> bool
{
	return true;
}

auto FGitLink_Provider::IsAtLatestRevision() const -> TOptional<bool>
{
	return {};
}

auto FGitLink_Provider::GetNumLocalChanges() const -> TOptional<int>
{
	if (!_bGitRepositoryFound)
	{ return {}; }

	return _StateCache->Get_NumFiles();
}
#endif

#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 2
auto FGitLink_Provider::AllowsDiffAgainstDepot     () const -> bool { return true;  }
auto FGitLink_Provider::UsesUncontrolledChangelists() const -> bool { return false; }
auto FGitLink_Provider::UsesSnapshots              () const -> bool { return false; }
#endif

#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 3
auto FGitLink_Provider::CanExecuteOperation(const FSourceControlOperationRef& /*InOperation*/) const -> bool
{
	return _bGitRepositoryFound;
}

auto FGitLink_Provider::GetStatus() const -> TMap<EStatus, FString>
{
	TMap<EStatus, FString> Out;
	Out.Add(EStatus::Repository, _PathToRepositoryRoot);
	Out.Add(EStatus::User,       _UserName);
	Out.Add(EStatus::Branch,     _BranchName);
	Out.Add(EStatus::Remote,     _RemoteUrl);
	return Out;
}
#endif

// --------------------------------------------------------------------------------------------------------------------
// Branches (v1: no multi-branch tracking — all stubs)
// --------------------------------------------------------------------------------------------------------------------
auto FGitLink_Provider::QueryStateBranchConfig(const FString& /*InConfigSrc*/, const FString& /*InConfigDest*/) -> bool
{
	return false;
}

auto FGitLink_Provider::RegisterStateBranches(const TArray<FString>& /*InBranchNames*/, const FString& /*InContentRootIn*/) -> void
{
}

auto FGitLink_Provider::GetStateBranchIndex(const FString& /*InBranchName*/) const -> int32
{
	return INDEX_NONE;
}

auto FGitLink_Provider::GetStateBranchAtIndex(int32 /*InBranchIndex*/, FString& /*OutBranchName*/) const -> bool
{
	return false;
}

// --------------------------------------------------------------------------------------------------------------------
// Tick / labels / changelists / settings widget
// --------------------------------------------------------------------------------------------------------------------
auto FGitLink_Provider::Tick() -> void
{
	if (_Dispatcher.IsValid())
	{ _Dispatcher->Tick(); }

	// Fire any accumulated state-change notifications the commands have queued. In Pass B there
	// are none; once the dispatcher produces state deltas this will broadcast.
	// _OnSourceControlStateChanged.Broadcast();
}

auto FGitLink_Provider::GetLabels(const FString& /*InMatchingSpec*/) const
	-> TArray<TSharedRef<class ISourceControlLabel>>
{
	return {};
}

#if ENGINE_MAJOR_VERSION >= 5
auto FGitLink_Provider::GetChangelists(EStateCacheUsage::Type /*InStateCacheUsage*/)
	-> TArray<FSourceControlChangelistRef>
{
	TArray<FSourceControlChangelistRef> Out;
	const TArray<FGitLink_ChangelistStateRef> States = _StateCache->Enumerate_Changelists();
	Out.Reserve(States.Num());
	for (const FGitLink_ChangelistStateRef& State : States)
	{
		Out.Add(MakeShared<FGitLink_Changelist, ESPMode::ThreadSafe>(State->_Changelist));
	}
	return Out;
}
#endif

#if SOURCE_CONTROL_WITH_SLATE
auto FGitLink_Provider::MakeSettingsWidget() const -> TSharedRef<class SWidget>
{
	// Real Slate widget lands in the UI pass. For now return an empty box so source control
	// settings doesn't crash.
	return SNew(SBox);
}
#endif

// --------------------------------------------------------------------------------------------------------------------
// Helpers
// --------------------------------------------------------------------------------------------------------------------
auto FGitLink_Provider::Get_StateCache() -> FGitLink_StateCache&
{
	return *_StateCache;
}

auto FGitLink_Provider::Get_Repository() const -> gitlink::FRepository*
{
	return _Repository.Get();
}

#undef LOCTEXT_NAMESPACE
