#include "GitLink/GitLink_Provider.h"

#include "GitLink/GitLink_Settings.h"
#include "GitLink_CommandDispatcher.h"
#include "GitLink_ChangelistState.h"
#include "GitLink_StateCache.h"
#include "GitLink_BackgroundPoll.h"
#include "GitLink_Menu.h"
#include "GitLink_HookProbe.h"
#include "GitLink_Subprocess.h"
#include "GitLinkLog.h"
#include "Slate/SGitLink_Settings.h"

#include <Misc/App.h>
#include <UObject/ObjectSaveContext.h>
#include <UObject/Package.h>

#include "GitLinkCore/Repository/GitLink_Repository.h"
#include "GitLinkCore/Repository/GitLink_Repository_Params.h"
#include "GitLinkCore/Types/GitLink_Types.h"


// Op_Signature declaration — no public header in GitLinkCore exposes it because it's primarily
// an internal helper. Forward-declared here so the provider can call it.
namespace gitlink::op
{
	auto Get_DefaultSignature(gitlink::FRepository& InRepo) -> gitlink::FSignature;
	auto Enumerate_SubmodulePaths(gitlink::FRepository& InRepo) -> TArray<FString>;
}

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
	UE_LOG(LogGitLink, Log, TEXT("FGitLink_Provider::Init(force=%s, alreadyOpen=%s)"),
		bInForceConnection ? TEXT("true") : TEXT("false"),
		_bGitRepositoryFound ? TEXT("true") : TEXT("false"));

	// bForceConnection controls whether we re-probe when already connected; it does NOT gate
	// whether we try to connect at all. The editor passes force=false on quiet re-activation
	// (e.g. when the source control settings dialog reselects the current provider). We still
	// want to open the repository in that case.
	if (_bGitRepositoryFound && !bInForceConnection)
	{
		return;
	}

	CheckRepositoryStatus();
}

auto FGitLink_Provider::Close() -> void
{
	UE_LOG(LogGitLink, Log, TEXT("FGitLink_Provider::Close"));

	_BackgroundPoll.Reset();

	if (_PackageSavedHandle.IsValid())
	{
		UPackage::PackageSavedWithContextEvent.Remove(_PackageSavedHandle);
		_PackageSavedHandle.Reset();
	}

	if (_Menu.IsValid())
	{
		_Menu->Unregister();
		_Menu.Reset();
	}

	_Repository.Reset();
	_Subprocess.Reset();
	_bGitRepositoryFound = false;
	_bLfsAvailable = false;
	_LockableExtensions.Reset();
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

	// Read user.name / user.email from git config. Empty values mean the user hasn't configured
	// them; Cmd_CheckIn will surface a helpful error at commit time if that's the case.
	const gitlink::FSignature Sig = gitlink::op::Get_DefaultSignature(*_Repository);
	_UserName  = Sig.Name;
	_UserEmail = Sig.Email;

	// Stand up the subprocess helper (for LFS lock, check-attr, hook fallback) before we probe
	// anything that needs it. Falls back to "git" on PATH when no override is configured.
	const FString GitBinary = (Settings != nullptr && !Settings->GitBinaryOverride.IsEmpty())
		? Settings->GitBinaryOverride
		: TEXT("git");
	_Subprocess = MakeUnique<FGitLink_Subprocess>(GitBinary, _PathToRepositoryRoot);

	// Probe for git-lfs. If unavailable we'll still connect; UsesCheckout just stays off.
	_bLfsAvailable = _Subprocess->IsLfsAvailable();

	// Discover lockable extensions via .gitattributes. Drives Is_FileLockable and the Lock
	// state population in Cmd_Connect / Cmd_UpdateStatus.
	_LockableExtensions.Reset();
	if (_bLfsAvailable)
	{
		const TArray<FString> Wildcards {
			TEXT("*.uasset"),
			TEXT("*.umap"),
			TEXT("*.uexp"),
			TEXT("*.ubulk"),
		};
		_LockableExtensions = _Subprocess->ProbeLockableExtensions(Wildcards);
	}

	// Enumerate submodule paths so we can detect files that live in submodules.
	// These files should not be checked out / checked in from the parent repo.
	{
		const TArray<FString> RelativeSubmodules = gitlink::op::Enumerate_SubmodulePaths(*_Repository);
		_SubmodulePaths.Reset();
		_SubmodulePaths.Reserve(RelativeSubmodules.Num());
		for (const FString& RelPath : RelativeSubmodules)
		{
			FString AbsPath = FPaths::Combine(_PathToRepositoryRoot, RelPath);
			FPaths::NormalizeFilename(AbsPath);
			if (!AbsPath.EndsWith(TEXT("/")))
			{ AbsPath += TEXT("/"); }
			_SubmodulePaths.Add(MoveTemp(AbsPath));
		}

		// Push a snapshot into the state cache so new entries are stamped Unlockable at
		// creation time. This closes the timing window (CLAUDE.md Pitfall #5) where
		// GetOrCreate_FileState is called before GetState() has had a chance to annotate.
		_StateCache->Set_SubmodulePaths(_SubmodulePaths);

		for (const FString& SubPath : _SubmodulePaths)
		{
			UE_LOG(LogGitLink, Verbose, TEXT("CheckRepositoryStatus: submodule path: '%s'"), *SubPath);
		}
	}

	UE_LOG(LogGitLink, Log,
		TEXT("CheckRepositoryStatus: repo='%s' branch='%s' remote='%s' user='%s <%s>' ")
		TEXT("lfs=%s lockable_exts=%d (0=using hardcoded defaults) submodules=%d"),
		*_PathToRepositoryRoot, *_BranchName, *_RemoteUrl, *_UserName, *_UserEmail,
		_bLfsAvailable ? TEXT("yes") : TEXT("no"),
		_LockableExtensions.Num(),
		_SubmodulePaths.Num());

	// Probe for git hooks so we know whether to route commits through the subprocess.
	if (Settings != nullptr && Settings->bSubprocessFallbackForHooks)
	{
		const FGitLink_HookFlags HookFlags = FGitLink_HookProbe::Probe(_PathToRepositoryRoot);
		_bHasPreCommitOrCommitMsgHook = HookFlags.NeedsSubprocessForCommit();
	}

	// Create or reconfigure the background poll. Driven by the provider's Tick() every frame
	// rather than FTSTicker, which avoids ticker handle issues during Init(force=true) re-init.
	if (!_BackgroundPoll.IsValid())
	{
		_BackgroundPoll = MakeUnique<FGitLink_BackgroundPoll>(*this);
	}
	if (Settings != nullptr && Settings->bEnableBackgroundPoll && Settings->PollIntervalSeconds > 0)
	{
		_BackgroundPoll->SetInterval(static_cast<float>(Settings->PollIntervalSeconds));
	}
	else
	{
		_BackgroundPoll->SetInterval(0.f);
	}

	// Hook file saves for sub-2-second View Changes refresh. Unbind any previous handle
	// (re-init case) before re-binding.
	if (_PackageSavedHandle.IsValid())
	{
		UPackage::PackageSavedWithContextEvent.Remove(_PackageSavedHandle);
	}
#if ENGINE_MAJOR_VERSION >= 5
	_PackageSavedHandle = UPackage::PackageSavedWithContextEvent.AddLambda(
		[this](const FString& InFilename, UPackage* /*InPackage*/, FObjectPostSaveContext /*InContext*/)
		{
			OnPackageSaved(InFilename);
		});
#else
	_PackageSavedHandle = UPackage::PackageSavedEvent.AddLambda(
		[this](const FString& InFilename, UPackage* /*InPackage*/)
		{
			OnPackageSaved(InFilename);
		});
#endif

	// Register toolbar buttons (Push, Pull, Revert All, Refresh) in the SCC dropdown.
	if (!_Menu.IsValid())
	{
		_Menu = MakeUnique<FGitLink_Menu>();
	}
	_Menu->Register();
}

// --------------------------------------------------------------------------------------------------------------------
auto FGitLink_Provider::OnPackageSaved(const FString& InFilename) -> void
{
	if (!_bGitRepositoryFound)
	{ return; }

	FString Normalized = FPaths::ConvertRelativePathToFull(InFilename);
	FPaths::NormalizeFilename(Normalized);

	// Ignore saves outside the repository root.
	if (!Normalized.StartsWith(_PathToRepositoryRoot))
	{
		UE_LOG(LogGitLink, Verbose,
			TEXT("OnPackageSaved: ignoring '%s' (outside repo root '%s')"),
			*Normalized, *_PathToRepositoryRoot);
		return;
	}

	UE_LOG(LogGitLink, Log,
		TEXT("OnPackageSaved: '%s' — requesting immediate poll"), *Normalized);

	// If the file is in the Staged changelist, re-stage it so the index entry matches
	// the new on-disk content. Without this, saving a staged file creates a confusing
	// diff where the index has the old version and the working tree has the new one.
	if (_StateCache.IsValid() && _Repository.IsValid())
	{
		const FGitLink_FileStateRef State = _StateCache->Find_FileState(Normalized);
		if (State->_State.Tree == EGitLink_TreeState::Staged)
		{
			FString RelPath = Normalized;
			if (RelPath.StartsWith(_PathToRepositoryRoot, ESearchCase::IgnoreCase))
			{
				RelPath.RightChopInline(_PathToRepositoryRoot.Len(), EAllowShrinking::No);
				RelPath.RemoveFromStart(TEXT("/"));
			}
			_Repository->Stage({ RelPath });
		}
	}

	Request_ImmediatePoll();
}

auto FGitLink_Provider::Request_ImmediatePoll() -> void
{
	if (_BackgroundPoll.IsValid())
	{ _BackgroundPoll->Request_ImmediatePoll(); }
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
		// Normalize the path the same way commands do when storing state — without this,
		// the editor might query "D:/Repos/..." while the cache has it stored under a
		// slightly different form (case, trailing slashes, relative vs absolute).
		FString Normalized = FPaths::ConvertRelativePathToFull(File);
		FPaths::NormalizeFilename(Normalized);

		FGitLink_FileStateRef State = _StateCache->GetOrCreate_FileState(Normalized);

		// Files inside submodules should not be checkable-out / checkable-in from the
		// parent repo. Mark them as Unlockable + Unmodified so all action predicates
		// (CanCheckout, CanCheckIn, CanRevert, CanAdd) return false while
		// IsSourceControlled returns true. History still works via submodule repo open.
		// Always stamp on every GetState call — submodule paths may not be populated
		// during early init, so a previous query might have left Lock=Unknown.
		if (Is_InSubmodule(Normalized))
		{
			// Mark as in-submodule. The FGitLink_FileState predicates special-case this flag:
			//   - IsCheckedOut() returns true → UE's PromptToCheckoutPackages short-circuits
			//     via `bAlreadyCheckedOut = IsCheckedOut() || IsAdded()`, no dialog.
			//   - IsSourceControlled() returns true → History right-click stays enabled.
			//   - CanCheckout/CanCheckIn/CanRevert/CanAdd/CanDelete all return false → no
			//     bogus menu items that would try to operate on the parent repo.
			State->_State.bInSubmodule = true;
			State->_State.Lock = EGitLink_LockState::Unlockable;
			if (State->_State.Tree == EGitLink_TreeState::NotInRepo ||
				State->_State.Tree == EGitLink_TreeState::Unset)
			{
				State->_State.Tree = EGitLink_TreeState::Unmodified;
			}
		}

		OutState.Add(State);
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
	FSourceControlChangelistPtr InChangelist,
	const TArray<FString>& InFiles,
	EConcurrency::Type InConcurrency,
	const FSourceControlOperationComplete& InOperationCompleteDelegate) -> ECommandResult::Type
{
	return _Dispatcher->Dispatch(InOperation, InFiles, InConcurrency, InOperationCompleteDelegate, InChangelist);
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
	// "Checkout" in Unreal == acquire an LFS lock. Two conditions:
	//   1. The user has bUseLfsLocking on in the plugin settings
	//   2. git-lfs is actually installed (probed at connect time)
	//
	// We intentionally do NOT require .gitattributes to include the 'lockable' keyword.
	// Most UE repos configure LFS via 'filter=lfs' without 'lockable', and users still
	// expect the checkout workflow to work. The per-file Is_FileLockable gate (based on
	// file extension) prevents non-binary files from being locked.
	const UGitLink_Settings* Settings = GetDefault<UGitLink_Settings>();
	const bool bUserEnabled = Settings != nullptr && Settings->bUseLfsLocking;
	return bUserEnabled && _bLfsAvailable;
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

	if (_BackgroundPoll.IsValid())
	{ _BackgroundPoll->Tick(FApp::GetDeltaTime()); }
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
	return SNew(SGitLink_Settings);
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

auto FGitLink_Provider::Get_Subprocess() const -> FGitLink_Subprocess*
{
	return _Subprocess.Get();
}

auto FGitLink_Provider::Is_FileLockable(const FString& InAbsolutePath) const -> bool
{
	// If check-attr found explicit 'lockable' extensions, use those.
	if (!_LockableExtensions.IsEmpty())
	{
		for (const FString& Ext : _LockableExtensions)
		{
			if (InAbsolutePath.EndsWith(Ext, ESearchCase::IgnoreCase))
			{ return true; }
		}
		return false;
	}

	// Fallback: most UE repos use LFS for binary assets via 'filter=lfs' but omit the
	// 'lockable' keyword from .gitattributes. We hardcode the common UE binary extensions
	// so the checkout workflow works out of the box for typical Unreal projects.
	static const TCHAR* DefaultLockableExts[] = {
		TEXT(".uasset"),
		TEXT(".umap"),
		TEXT(".uexp"),
		TEXT(".ubulk"),
	};

	for (const TCHAR* Ext : DefaultLockableExts)
	{
		if (InAbsolutePath.EndsWith(Ext, ESearchCase::IgnoreCase))
		{ return true; }
	}
	return false;
}

auto FGitLink_Provider::Is_InSubmodule(const FString& InAbsolutePath) const -> bool
{
	FString Normalized = FPaths::ConvertRelativePathToFull(InAbsolutePath);
	FPaths::NormalizeFilename(Normalized);

	for (const FString& SubPath : _SubmodulePaths)
	{
		if (Normalized.StartsWith(SubPath, ESearchCase::IgnoreCase))
		{ return true; }
	}
	return false;
}

auto FGitLink_Provider::Get_SubmoduleRoot(const FString& InAbsolutePath) const -> FString
{
	FString Normalized = FPaths::ConvertRelativePathToFull(InAbsolutePath);
	FPaths::NormalizeFilename(Normalized);

	for (const FString& SubPath : _SubmodulePaths)
	{
		if (Normalized.StartsWith(SubPath, ESearchCase::IgnoreCase))
		{ return SubPath; }
	}
	return FString();
}

auto FGitLink_Provider::Broadcast_StateChanged() -> void
{
	check(IsInGameThread());
	_OnSourceControlStateChanged.Broadcast();
}

#undef LOCTEXT_NAMESPACE
