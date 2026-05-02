#include "GitLink/GitLink_Provider.h"

#include "GitLink/GitLink_Settings.h"
#include "GitLink_CommandDispatcher.h"
#include "GitLink_ChangelistState.h"
#include "GitLink_StateCache.h"
#include "GitLink_BackgroundPoll.h"
#include "GitLink_Menu.h"
#include "GitLink_HookProbe.h"
#include "GitLink_LfsHttpClient.h"
#include "GitLink_Subprocess.h"
#include "GitLinkLog.h"
#include "Slate/SGitLink_Settings.h"

#include <Async/Async.h>
#include <Editor.h>
#include <Framework/Application/SlateApplication.h>
#include <Misc/App.h>
#include <Misc/FileHelper.h>
#include <Misc/PackageName.h>
#include <Subsystems/AssetEditorSubsystem.h>
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
	auto Enumerate_TrackedFiles(gitlink::FRepository& InRepo) -> TArray<FString>;
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

	// Stage B — unwire single-file LFS lock refresh signals.
	if (_AppActivationHandle.IsValid() && FSlateApplication::IsInitialized())
	{
		FSlateApplication::Get().OnApplicationActivationStateChanged().Remove(_AppActivationHandle);
		_AppActivationHandle.Reset();
	}
	if (_AssetEditorOpenedHandle.IsValid() && GEditor != nullptr)
	{
		if (UAssetEditorSubsystem* AssetEditor = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>())
		{
			AssetEditor->OnAssetOpenedInEditor().Remove(_AssetEditorOpenedHandle);
		}
		_AssetEditorOpenedHandle.Reset();
	}
	if (_PackageDirtiedHandle.IsValid())
	{
		UPackage::PackageMarkedDirtyEvent.Remove(_PackageDirtiedHandle);
		_PackageDirtiedHandle.Reset();
	}
	{
		FScopeLock Lock(&_LockRefreshDebounceLock);
		_LastLockRefreshSec.Reset();
	}

	if (_Menu.IsValid())
	{
		_Menu->Unregister();
		_Menu.Reset();
	}

	_Repository.Reset();
	_LfsHttpClient.Reset();
	_Subprocess.Reset();
	_bGitRepositoryFound = false;
	_bLfsAvailable = false;
	_LockableExtensions.Reset();
	_SubmodulePaths.Reset();
	_LfsSubmodulePaths.Reset();
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

	// In-process LFS HTTP client — used by the background poll instead of shelling out to
	// `git lfs locks --verify --json` per repo. Endpoint URLs are resolved per-repo below
	// once we know the LFS-using submodule set.
	_LfsHttpClient = MakeUnique<FGitLink_LfsHttpClient>(*_Subprocess);

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

		// Enumerate each submodule's tracked-files index. Used by GetState to distinguish
		// "tracked submodule file the parent's status walk can't see" (default to Unmodified)
		// from "untracked file in a submodule" (must default to NotInRepo so CanCheckout
		// returns false). Without this distinction, brand-new files inside submodules
		// trigger the editor's "check out?" prompt because the optimistic Unmodified
		// default lets CanCheckout pass.
		//
		// Lower-case the keys for case-insensitive lookup on Windows. The set stores
		// repo-relative paths (forward-slash form), matching what Enumerate_TrackedFiles
		// returns from libgit2.
		_SubmoduleTrackedSets.Reset();
		_SubmoduleTrackedSets.Reserve(_SubmodulePaths.Num());
		int32 TotalSubmoduleTracked = 0;
		for (const FString& SubRoot : _SubmodulePaths)
		{
			gitlink::FOpenParams SubParams;
			SubParams.Path = SubRoot;
			TUniquePtr<gitlink::FRepository> SubRepo = gitlink::FRepository::Open(SubParams);
			if (!SubRepo.IsValid())
			{
				UE_LOG(LogGitLink, Verbose,
					TEXT("CheckRepositoryStatus: could not open submodule '%s' for tracked-set enumeration: %s"),
					*SubRoot, *gitlink::FRepository::Get_LastOpenError());
				continue;
			}

			const TArray<FString> SubTracked = gitlink::op::Enumerate_TrackedFiles(*SubRepo);
			TSet<FString>& SetRef = _SubmoduleTrackedSets.Add(SubRoot);
			SetRef.Reserve(SubTracked.Num());
			for (const FString& RelPath : SubTracked)
			{ SetRef.Add(RelPath.ToLower()); }
			TotalSubmoduleTracked += SubTracked.Num();
		}

		UE_LOG(LogGitLink, Log,
			TEXT("CheckRepositoryStatus: indexed %d tracked file(s) across %d submodule(s) for fast TrackedInSubmodule lookups"),
			TotalSubmoduleTracked, _SubmoduleTrackedSets.Num());

		// Treat every submodule as a candidate LFS repo. Previously we tried to filter by
		// scanning each submodule's top-level .gitattributes for `filter=lfs` / `lockable`,
		// but that probe missed submodules whose lockable patterns live in nested
		// .gitattributes (e.g. <sub>/Content/.gitattributes) or whose top-level file is
		// missing entirely — and a missed submodule meant its locks never appeared in the
		// editor across restarts (CLAUDE.md "Submodule LFS locks not visible after restart").
		//
		// The original perf concern (avoid 30+ remote calls per poll cycle) is now
		// addressed by:
		//   - concurrent polls (ParallelFor in Cmd_UpdateStatus / Cmd_Connect)
		//   - per-repo URL cache in FGitLink_LfsHttpClient (resolved once, not per poll)
		//   - per-host backoff on 429 responses
		//   - graceful subprocess fallback when endpoint resolution fails for a repo
		//
		// Endpoint resolution below will silently no-op for non-LFS submodules (no LFS URL
		// in their git config), so the runtime cost of including them here is bounded to
		// one resolve attempt at connect time.
		_LfsSubmodulePaths = _SubmodulePaths;

		UE_LOG(LogGitLink, Log,
			TEXT("CheckRepositoryStatus: treating all %d submodules as LFS candidates (endpoint resolution will filter)"),
			_LfsSubmodulePaths.Num());

		for (const FString& SubPath : _SubmodulePaths)
		{
			UE_LOG(LogGitLink, Verbose, TEXT("CheckRepositoryStatus: submodule path: '%s'"), *SubPath);
		}

		// Resolve LFS endpoint URLs for the parent repo + each LFS-using submodule. This is
		// the only place that shells out for endpoint resolution; per-poll calls hit only the
		// in-memory cache. A repo without a usable HTTPS LFS URL (e.g. SSH-only remotes) is
		// silently skipped — Cmd_UpdateStatus / Cmd_Connect fall back to the subprocess path
		// for those repos at poll time.
		if (_bLfsAvailable && _LfsHttpClient.IsValid())
		{
			int32 ResolvedCount = 0;
			if (_LfsHttpClient->Resolve_LfsUrlForRepo(_PathToRepositoryRoot))
			{ ++ResolvedCount; }
			for (const FString& SubRoot : _LfsSubmodulePaths)
			{
				if (_LfsHttpClient->Resolve_LfsUrlForRepo(SubRoot))
				{ ++ResolvedCount; }
			}

			UE_LOG(LogGitLink, Log,
				TEXT("CheckRepositoryStatus: LFS HTTP client resolved %d/%d LFS endpoint(s)"),
				ResolvedCount, 1 + _LfsSubmodulePaths.Num());
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

	// Stage B — single-file LFS lock refresh signals. Wire three orthogonal triggers and
	// route them through Request_LockRefreshForFile, which debounces and runs the HTTP probe
	// off the game thread. Each handle is rebound here on re-init; Close() unbinds them.

	// (a) App activation — when the editor regains focus, refresh open assets in case
	// teammates locked something while we were backgrounded. Cap to a small batch to avoid
	// pounding the LFS server with one request per open tab on Alt-Tab.
	if (_AppActivationHandle.IsValid() && FSlateApplication::IsInitialized())
	{
		FSlateApplication::Get().OnApplicationActivationStateChanged().Remove(_AppActivationHandle);
	}
	if (FSlateApplication::IsInitialized())
	{
		_AppActivationHandle = FSlateApplication::Get().OnApplicationActivationStateChanged().AddLambda(
			[this](const bool bInIsActive)
			{
				if (!bInIsActive || GEditor == nullptr) { return; }
				UAssetEditorSubsystem* AssetEditor = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
				if (AssetEditor == nullptr) { return; }

				constexpr int32 kMaxBatchOnActivate = 16;
				int32 Issued = 0;
				for (UObject* Asset : AssetEditor->GetAllEditedAssets())
				{
					if (Asset == nullptr) { continue; }
					UPackage* Pkg = Asset->GetOutermost();
					if (Pkg == nullptr) { continue; }
					FString PackageFilename;
					if (!FPackageName::TryConvertLongPackageNameToFilename(
							Pkg->GetName(), PackageFilename, FPackageName::GetAssetPackageExtension()))
					{ continue; }
					Request_LockRefreshForFile(PackageFilename);
					if (++Issued >= kMaxBatchOnActivate) { break; }
				}
			});
	}

	// (b) Asset opened — fires when the user double-clicks an asset and the asset editor
	// instantiates. Earlier than pre-checkout; gives the probe a head start so the icon is
	// already accurate by the time the user can click Check Out.
	if (_AssetEditorOpenedHandle.IsValid() && GEditor != nullptr)
	{
		if (UAssetEditorSubsystem* AssetEditor = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>())
		{ AssetEditor->OnAssetOpenedInEditor().Remove(_AssetEditorOpenedHandle); }
	}
	if (GEditor != nullptr)
	{
		if (UAssetEditorSubsystem* AssetEditor = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>())
		{
			_AssetEditorOpenedHandle = AssetEditor->OnAssetOpenedInEditor().AddLambda(
				[this](UObject* InAsset, IAssetEditorInstance* /*InInstance*/)
				{
					if (InAsset == nullptr) { return; }
					UPackage* Pkg = InAsset->GetOutermost();
					if (Pkg == nullptr) { return; }
					FString PackageFilename;
					if (!FPackageName::TryConvertLongPackageNameToFilename(
							Pkg->GetName(), PackageFilename, FPackageName::GetAssetPackageExtension()))
					{ return; }
					Request_LockRefreshForFile(PackageFilename);
				});
		}
	}

	// (c) Package dirtied — first user edit. Debounced like the others; the typical UX is
	// "user edits something, then notices the lock state". A probe right at dirty-time is
	// nearly always wasted, but cheap; it ensures the next CheckOut prompt is correct.
	if (_PackageDirtiedHandle.IsValid())
	{ UPackage::PackageMarkedDirtyEvent.Remove(_PackageDirtiedHandle); }
	_PackageDirtiedHandle = UPackage::PackageMarkedDirtyEvent.AddLambda(
		[this](UPackage* InPackage, bool /*bInWasDirty*/)
		{
			if (InPackage == nullptr) { return; }
			FString PackageFilename;
			if (!FPackageName::TryConvertLongPackageNameToFilename(
					InPackage->GetName(), PackageFilename, FPackageName::GetAssetPackageExtension()))
			{ return; }
			Request_LockRefreshForFile(PackageFilename);
		});

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

namespace
{
	// 2 seconds matches the foreground freshness target. A user opening 50 tabs in a burst
	// (Alt-Tab → all open editors signal at once) shouldn't fire 50 simultaneous HTTPs; the
	// debounce collapses repeated signals for the same path into one probe, while letting
	// genuine activity through.
	constexpr double kSingleFileRefreshDebounceSec = 2.0;
}

auto FGitLink_Provider::Request_LockRefreshForFile(const FString& InAbsolutePath) -> void
{
	if (!_bGitRepositoryFound || !_bLfsAvailable)
	{ return; }

	FGitLink_LfsHttpClient* HttpClient = _LfsHttpClient.Get();
	if (HttpClient == nullptr || !gitlink::lfs_http::Is_Enabled())
	{ return; }

	// Normalize once — used for the cache lookup, debounce key, and the HTTP repo-relative
	// derivation. The state cache normalizes its own keys, but the debounce map and the HTTP
	// path want consistent input.
	FString Normalized = FPaths::ConvertRelativePathToFull(InAbsolutePath);
	FPaths::NormalizeFilename(Normalized);

	if (!Normalized.StartsWith(_PathToRepositoryRoot))
	{ return; }

	if (!Is_FileLockable(Normalized))
	{ return; }

	// Submodule files: only probe tracked entries. Untracked-in-submodule = the file isn't in
	// HEAD yet, so a server-side lock is meaningless and the f345260 "auto-lock-on-create"
	// fix would be regressed if we let untracked files through to a lock probe → checkout.
	if (Is_InSubmodule(Normalized) && !Is_TrackedInSubmodule(Normalized))
	{ return; }

	// Per-path debounce. Hot path — keep the critical section short.
	const double NowSec = FPlatformTime::Seconds();
	{
		FScopeLock Lock(&_LockRefreshDebounceLock);
		const double* LastSec = _LastLockRefreshSec.Find(Normalized);
		if (LastSec != nullptr && (NowSec - *LastSec) < kSingleFileRefreshDebounceSec)
		{ return; }
		_LastLockRefreshSec.Add(Normalized, NowSec);
	}

	// Determine which repo the file belongs to (parent or a specific submodule). HTTP path
	// is keyed by repo root; the repo's LFS endpoint must already be cached.
	const FString RepoRoot = Is_InSubmodule(Normalized)
		? Get_SubmoduleRoot(Normalized)
		: _PathToRepositoryRoot;
	if (RepoRoot.IsEmpty() || !HttpClient->Has_LfsUrl(RepoRoot))
	{ return; }

	UE_LOG(LogGitLink, Verbose,
		TEXT("Request_LockRefreshForFile: probing '%s' (repo='%s')"), *Normalized, *RepoRoot);

	// Off-thread the HTTP probe so the game thread (which fired the delegate) is never
	// blocked. Marshal the result back via AsyncTask(GameThread) so the cache mutation is
	// race-free with other readers.
	AsyncTask(ENamedThreads::AnyBackgroundThreadNormalTask,
		[this, Normalized, RepoRoot, HttpClient]()
		{
			const FGitLink_LfsHttpClient::FSingleFileLockResult Result =
				HttpClient->Request_SingleFileLock(RepoRoot, Normalized);
			if (!Result.bSuccess)
			{ return; }

			AsyncTask(ENamedThreads::GameThread,
				[this, Normalized, Result]()
				{
					if (!_StateCache.IsValid())
					{ return; }

					const FGitLink_FileStateRef Existing = _StateCache->Find_FileState(Normalized);

					// The single-file probe is scoped to teammate-driven transitions —
					// NotLocked ↔ LockedOther, plus LockUser updates within LockedOther.
					// Lock=Locked (held by us) is locally-authoritative: it's set by
					// Cmd_CheckOut, cleared by Cmd_Revert / Cmd_CheckIn, and outside-our-
					// control transitions are caught by the full-sweep lock-released
					// detection in Cmd_UpdateStatus (which compares the cache against the
					// full /locks/verify response, not a single-path probe that may be
					// stale).
					//
					// Why this guard exists: GitHub LFS read replicas aren't immediately
					// consistent after a `git lfs lock`. The post-CheckOut UpdateStatus
					// fires this probe ~50 ms after the lock subprocess returns; the probe
					// hits a replica that hasn't seen the new lock yet and reports
					// NotLocked. Without this guard, the completion lambda flipped the
					// cache from Lock=Locked back to NotLocked — the "click Lock → flicker
					// → reverts to unlocked" UX from v0.3.1. Symmetric race after Cmd_Revert
					// (probe returns stale Locked) was equally bad.
					const EGitLink_LockState ExistingLock = Existing->_State.Lock;
					if (ExistingLock == EGitLink_LockState::Locked
						|| Result.Lock == EGitLink_LockState::Locked)
					{ return; }

					if (Existing->_State.Lock == Result.Lock
						&& Existing->_State.LockUser == Result.LockOwner)
					{ return; }  // no observable change — skip cache write + broadcast

					FGitLink_CompositeState Composite = Existing->_State;
					Composite.Lock     = Result.Lock;
					Composite.LockUser = Result.LockOwner;
					if (Composite.Tree == EGitLink_TreeState::NotInRepo
						&& Result.Lock != EGitLink_LockState::NotLocked)
					{
						// Locked file we hadn't seen yet — give it a sensible Tree default so
						// content-browser predicates don't read it as "not in repo".
						Composite.Tree = EGitLink_TreeState::Unmodified;
					}

					FGitLink_FileStateRef NewState = MakeShared<FGitLink_FileState, ESPMode::ThreadSafe>(Normalized);
					NewState->_State      = Composite;
					NewState->_Changelist = Existing->_Changelist;
					NewState->_History    = Existing->_History;
					NewState->_TimeStamp  = FDateTime::UtcNow();

					_StateCache->Set_FileState(Normalized, NewState);

					UE_LOG(LogGitLink, Verbose,
						TEXT("Request_LockRefreshForFile: '%s' Lock=%d Owner='%s' (single-file probe)"),
						*Normalized, static_cast<int32>(Result.Lock), *Result.LockOwner);

					Broadcast_StateChanged();
				});
		});
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

		// Stamp the bInSubmodule flag so command code can partition the batch by repo.
		// Predicates are otherwise submodule-agnostic — submodule files are lockable,
		// checkoutable, etc. on the same basis as outer-repo files. Real lock state comes
		// from the per-submodule LFS poll in Cmd_UpdateStatus.
		if (Is_InSubmodule(Normalized))
		{
			State->_State.bInSubmodule = true;

			// If we don't yet have a real Tree state for this file, decide between two
			// optimistic defaults based on the per-submodule tracked-files index built at
			// connect time (Is_TrackedInSubmodule):
			//
			//   - Tracked in the submodule's index → default Unmodified. The parent's
			//     libgit2 status walk doesn't recurse into submodules, so we'd otherwise
			//     have no entry for clean tracked submodule files. This default keeps
			//     CanCheckout truthy until the next per-submodule status walk in
			//     Cmd_UpdateStatus stamps the real state.
			//
			//   - NOT in the tracked set → leave NotInRepo. This is the "brand-new file
			//     in a submodule" case. Without this guard, Tree=Unmodified would let
			//     CanCheckout return true and the editor would prompt to LFS-lock a
			//     file that isn't even in git yet. Cmd_MarkForAdd is the correct flow
			//     for these files; once they're staged the next UpdateStatus pass
			//     stamps Tree=Staged via the per-submodule status walk.
			if (State->_State.Tree == EGitLink_TreeState::NotInRepo ||
				State->_State.Tree == EGitLink_TreeState::Unset)
			{
				if (Is_TrackedInSubmodule(Normalized))
				{
					State->_State.Tree = EGitLink_TreeState::Unmodified;
				}
				// else: leave Tree=NotInRepo. CanCheckout's bUntracked guard fires on
				// NotInRepo and blocks the misleading "check out?" prompt for new files.
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

auto FGitLink_Provider::Get_LfsHttpClient() const -> FGitLink_LfsHttpClient*
{
	return _LfsHttpClient.Get();
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

auto FGitLink_Provider::Is_TrackedInSubmodule(const FString& InAbsolutePath) const -> bool
{
	if (_SubmoduleTrackedSets.IsEmpty())
	{ return false; }

	FString Normalized = FPaths::ConvertRelativePathToFull(InAbsolutePath);
	FPaths::NormalizeFilename(Normalized);

	for (const TPair<FString, TSet<FString>>& Pair : _SubmoduleTrackedSets)
	{
		const FString& SubRoot = Pair.Key;  // has trailing forward slash
		if (!Normalized.StartsWith(SubRoot, ESearchCase::IgnoreCase))
		{ continue; }

		FString Relative = Normalized.RightChop(SubRoot.Len());
		Relative.ReplaceInline(TEXT("\\"), TEXT("/"));
		Relative.RemoveFromStart(TEXT("/"));
		Relative = Relative.ToLower();
		if (Pair.Value.Contains(Relative))
		{ return true; }

		// File is inside this submodule but not tracked. Don't fall through to other
		// submodules — submodule roots don't overlap.
		return false;
	}
	return false;
}

auto FGitLink_Provider::Open_SubmoduleRepositoryFor(const FString& InAbsolutePath) const
	-> TUniquePtr<gitlink::FRepository>
{
	const FString SubmoduleRoot = Get_SubmoduleRoot(InAbsolutePath);
	if (SubmoduleRoot.IsEmpty())
	{ return nullptr; }

	gitlink::FOpenParams Params;
	Params.Path = SubmoduleRoot;
	TUniquePtr<gitlink::FRepository> Repo = gitlink::FRepository::Open(Params);
	if (!Repo.IsValid())
	{
		UE_LOG(LogGitLink, Warning,
			TEXT("Open_SubmoduleRepositoryFor: failed to open submodule repo at '%s' (last error: %s)"),
			*SubmoduleRoot, *gitlink::FRepository::Get_LastOpenError());
		return nullptr;
	}
	return Repo;
}

auto FGitLink_Provider::Broadcast_StateChanged() -> void
{
	check(IsInGameThread());
	_OnSourceControlStateChanged.Broadcast();
}

#undef LOCTEXT_NAMESPACE
