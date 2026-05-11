#pragma once

#include "GitLink/GitLink_Changelist.h"
#include "GitLink/GitLink_State.h"

#include <CoreMinimal.h>
#include <ISourceControlProvider.h>
#include <Runtime/Launch/Resources/Version.h>

namespace gitlink { class FRepository; }
class FGitLink_StateCache;
class FGitLink_CommandDispatcher;
class FGitLink_Subprocess;
class FGitLink_LfsHttpClient;
class FGitLink_BackgroundPoll;
class FGitLink_Menu;

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

	// Subprocess + LFS HTTP client accessors return shared ownership handles. The provider is
	// the primary owner (constructed in CheckRepositoryStatus, dropped in Close) but commands
	// that fan their bodies out across worker threads (Cmd_UpdateStatus, Cmd_Connect) must
	// snapshot the TSharedPtr into a local *before* the parallel-for so an `Init(force=true)`
	// teardown mid-sweep can't free the underlying object while a worker is dereferencing it.
	// Returning a raw pointer here previously caused a TScopeLock use-after-free crash on
	// FGitLink_LfsHttpClient's mutex; see the v0.3.6 entry in CLAUDE.md's Version log.
	auto Get_Subprocess()    const -> TSharedPtr<FGitLink_Subprocess>;
	auto Get_LfsHttpClient() const -> TSharedPtr<FGitLink_LfsHttpClient>;

	// True when LFS is installed (git lfs version succeeded at connect time) AND the user
	// has bUseLfsLocking enabled in the plugin settings. Drives UsesCheckout().
	auto Is_LfsAvailable() const -> bool { return _bLfsAvailable; }
	auto NeedsSubprocessForCommit() const -> bool { return _bHasPreCommitOrCommitMsgHook; }

	// True when the file's extension matches a wildcard from .gitattributes marked with the
	// 'lockable' attribute. Populated once at connect time by probing via
	// `git check-attr lockable *.uasset *.umap ...`.
	auto Is_FileLockable(const FString& InAbsolutePath) const -> bool;

	// True when the file lives inside a git submodule.
	// Submodule files should not be checked out / checked in from the parent repo.
	auto Is_InSubmodule(const FString& InAbsolutePath) const -> bool;

	// Returns the absolute path to the submodule root containing this file, or empty if
	// the file is not in a submodule.
	auto Get_SubmoduleRoot(const FString& InAbsolutePath) const -> FString;

	// Opens a fresh FRepository rooted at the submodule that contains InAbsolutePath.
	// Returns nullptr if the path is not inside a known submodule, or if the open fails.
	// Used by command implementations (Cmd_Delete, Cmd_Revert) to route libgit2 ops into
	// the inner repo so working-tree mutations land in the submodule's index, not the
	// parent's.
	auto Open_SubmoduleRepositoryFor(const FString& InAbsolutePath) const
		-> TUniquePtr<gitlink::FRepository>;

	// Snapshot of the absolute submodule working-tree paths discovered at connect time.
	// Each entry has a trailing forward-slash. Used by Cmd_UpdateStatus / Cmd_Connect to
	// iterate over submodules for per-submodule LFS lock polling.
	auto Get_SubmodulePaths() const -> TArray<FString> { return _SubmodulePaths; }

	// Subset of Get_SubmodulePaths that have LFS configured (i.e. contain `filter=lfs` or
	// `lockable` patterns in their .gitattributes). Used to scope the per-poll LFS lock
	// query to repos that actually have lockable content — most submodules in a typical
	// project (source-only plugins, etc.) don't have LFS at all, and polling them costs
	// a network round-trip per poll for nothing. Probed once at connect time.
	auto Get_LfsSubmodulePaths() const -> TArray<FString> { return _LfsSubmodulePaths; }

	// True when the given absolute path is a tracked file inside one of the submodules.
	// Populated at connect time by enumerating each submodule's git index. Used by GetState
	// to decide whether a brand-new submodule file query should default to Tree=Unmodified
	// (tracked file the parent's status walk can't see) or Tree=NotInRepo (untracked file —
	// must NOT be offered for checkout, fixes the "new file in submodule prompts to lock"
	// bug). Case-insensitive lookup.
	auto Is_TrackedInSubmodule(const FString& InAbsolutePath) const -> bool;

	auto Get_PathToRepositoryRoot() const -> const FString& { return _PathToRepositoryRoot; }
	auto Get_UserName()             const -> const FString& { return _UserName; }
	auto Get_UserEmail()            const -> const FString& { return _UserEmail; }
	auto Get_BranchName()           const -> const FString& { return _BranchName; }
	auto Get_RemoteUrl()            const -> const FString& { return _RemoteUrl; }

	// Fire the OnSourceControlStateChanged delegate so the editor re-queries state for visible
	// files. Called from the dispatcher on the game thread after merging command results into
	// the cache.
	auto Broadcast_StateChanged() -> void;

	// Requests the background poll to fire on the next Tick. Used by the
	// PackageSavedWithContext hook to get sub-2-second View Changes refresh.
	auto Request_ImmediatePoll() -> void;

	// Stage B — async single-file LFS lock refresh, fired by editor delegates (focus,
	// asset-opened, package-dirtied) and by the explicit-file-list pre-checkout path.
	// Drops out fast for non-lockable / outside-repo / untracked-submodule files. Per-path
	// debounce (kSingleFileRefreshDebounceSec) prevents thundering herd when many files
	// signal at once. The HTTP probe runs on a background thread; cache mutation +
	// OnSourceControlStateChanged dispatch happen on the game thread.
	auto Request_LockRefreshForFile(const FString& InAbsolutePath) -> void;

	// Records that a local lock-state-changing op just succeeded for this file
	// (Cmd_CheckOut → Locked, or Cmd_Revert/CheckIn/Delete unlock → NotLocked).
	// `Was_RecentLocalLockOp` returns true for ~30s after, and Cmd_UpdateStatus's
	// full-scan refuses to overwrite the cache's lock state for guarded paths.
	//
	// Why: GitHub's LFS read replicas can lag a local lock/unlock by 10–100ms+.
	// The dispatcher's `Request_ImmediatePoll()` fires `Cmd_UpdateStatus` right
	// after our command returns; without this guard a stale `/locks/verify`
	// response either re-promotes our just-released file to Lock=Locked or
	// demotes our just-locked file to Lock=NotLocked, producing a visible flicker
	// (and in Revert's case a sticky lock indicator until the next sweep).
	//
	// Symmetric to v0.3.2's single-file-probe guard but at the full-scan level.
	auto Note_LocalLockOp     (const FString& InAbsolutePath)       -> void;
	auto Was_RecentLocalLockOp(const FString& InAbsolutePath) const -> bool;

private:
	// Opens the libgit2 repository at ProjectDir (or the configured override) and caches the
	// user / branch / remote metadata. Called from Init(true).
	auto CheckRepositoryStatus() -> void;

	// Called when the editor saves a package. Re-stages files in the Staged changelist
	// and requests an immediate background poll so View Changes updates quickly.
	auto OnPackageSaved(const FString& InFilename) -> void;

	TUniquePtr<gitlink::FRepository>       _Repository;
	TUniquePtr<FGitLink_StateCache>        _StateCache;
	TUniquePtr<FGitLink_CommandDispatcher> _Dispatcher;  // constructed empty in Pass B
	// Subprocess + LFS HTTP client are held as TSharedPtr so commands that fan out across
	// worker threads can snapshot a refcounted handle and keep the underlying object alive
	// across a concurrent Close() / Reset(). Provider is the primary owner; the secondary
	// references are short-lived (one per in-flight parallel command). See Get_LfsHttpClient()
	// comment above and the v0.3.6 entry in CLAUDE.md.
	TSharedPtr<FGitLink_Subprocess>        _Subprocess;     // present once a repo is open
	TSharedPtr<FGitLink_LfsHttpClient>     _LfsHttpClient;  // present once a repo is open
	TUniquePtr<FGitLink_BackgroundPoll>    _BackgroundPoll;
	TUniquePtr<FGitLink_Menu>              _Menu;

	bool _bGitRepositoryFound          = false;
	bool _bLfsAvailable                = false;
	bool _bHasPreCommitOrCommitMsgHook = false;  // from HookProbe at connect time

	// Extensions marked 'lockable' via .gitattributes. Populated at Connect time by running
	// `git check-attr lockable *.uasset *.umap ...`. Each entry includes the leading dot.
	TArray<FString> _LockableExtensions;

	// Absolute paths to submodule working directories, populated at connect time via
	// git_submodule_foreach. Used by Is_InSubmodule to detect files that should not
	// be checked out / checked in from the parent repo.
	TArray<FString> _SubmodulePaths;

	// Subset of _SubmodulePaths whose .gitattributes mentions LFS (filter=lfs or lockable).
	// Populated at connect time. Used to scope per-poll LFS lock queries.
	TArray<FString> _LfsSubmodulePaths;

	// Per-submodule tracked-file sets, keyed by absolute submodule root (with trailing /).
	// Each value is the set of repo-relative tracked paths in that submodule's git index,
	// stored lower-cased for case-insensitive lookup on Windows. Populated at connect time
	// alongside _SubmodulePaths and consumed by Is_TrackedInSubmodule + GetState. Empty
	// for repos without submodules. Refreshed only on full reconnect — adding a new file
	// to a submodule's index between connects won't be reflected here, but the next
	// UpdateStatus full sweep stamps the real Tree state regardless.
	TMap<FString, TSet<FString>> _SubmoduleTrackedSets;

	FString _PathToRepositoryRoot;  // resolved working tree root
	FString _UserName;
	FString _UserEmail;
	FString _BranchName;
	FString _RemoteUrl;

	FSourceControlStateChanged _OnSourceControlStateChanged;
	FDelegateHandle _PackageSavedHandle;

	// Stage B — editor signal delegate handles for single-file LFS lock refresh. Bound in
	// CheckRepositoryStatus, unbound in Close.
	FDelegateHandle _AppActivationHandle;     // FSlateApplication::OnApplicationActivationStateChanged
	FDelegateHandle _AssetEditorOpenedHandle; // UAssetEditorSubsystem::OnAssetOpenedInEditor
	FDelegateHandle _PackageDirtiedHandle;    // UPackage::PackageMarkedDirtyEvent

	// Per-path debounce for single-file LFS lock refreshes. Maps absolute path → wall-clock
	// seconds of the last refresh. Pruned only on Close (entries are tiny and the editor's
	// open-asset working set is bounded). Lock ordering: standalone, no nesting.
	mutable FCriticalSection _LockRefreshDebounceLock;
	TMap<FString, double>    _LastLockRefreshSec;

	// Recently-completed local lock ops (Cmd_CheckOut lock or unlock from Revert/CheckIn/Delete).
	// Keyed by Normalize_AbsolutePath form. Value is FPlatformTime::Seconds() at op completion.
	// Consulted by Cmd_UpdateStatus's full-scan lock merge to suppress the LFS-replica-lag race.
	// See Note_LocalLockOp / Was_RecentLocalLockOp above. Window is currently 30s.
	mutable FCriticalSection _RecentLocalLockOpsLock;
	TMap<FString, double>    _RecentLocalLockOps;

	mutable FCriticalSection _LastErrorsLock;
	TArray<FText> _LastErrors;
};
