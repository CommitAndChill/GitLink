#pragma once

#include "GitLink/GitLink_Changelist.h"
#include "GitLink/GitLink_State.h"

#include <CoreMinimal.h>
#include <ISourceControlProvider.h>
#include <Runtime/Launch/Resources/Version.h>

#include <atomic>

namespace gitlink { class FRepository; }
class FGitLink_StateCache;
class FGitLink_CommandDispatcher;
class FGitLink_Subprocess;
class FGitLink_LfsHttpClient;
class FGitLink_BackgroundPoll;
class FGitLink_Menu;

// --------------------------------------------------------------------------------------------------------------------
// FGitLink_ConnectSnapshot — immutable bundle of the repository metadata discovered at connect
// time (CheckRepositoryStatus). Published as a TSharedPtr<const ...> with a single guarded
// pointer swap, so async command bodies on worker threads can read submodule paths, tracked
// sets, lockable extensions and identity strings without racing the game thread's
// Init(force=true) rebuild. Never mutate a published snapshot — build a fresh one and re-publish.
//
// Why this exists: the editor fires Init(force=true) repeatedly (CLAUDE.md Pitfall #4), and
// CheckRepositoryStatus used to Reset()+repopulate these containers in place on the game thread
// while Cmd_UpdateStatus / Cmd_CheckOut bodies iterated them from task-graph workers — the same
// use-after-free shape as the v0.3.6 LFS-client crash, just on the provider's own containers.
// --------------------------------------------------------------------------------------------------------------------

struct FGitLink_ConnectSnapshot
{
	FString PathToRepositoryRoot;  // resolved working tree root
	FString UserName;
	FString UserEmail;
	FString BranchName;
	FString RemoteUrl;

	// Extensions marked 'lockable' via .gitattributes (probed via `git check-attr lockable ...`).
	// Each entry includes the leading dot. Empty → Is_FileLockable falls back to hardcoded UE
	// binary extensions.
	TArray<FString> LockableExtensions;

	// Absolute paths to submodule working directories (trailing forward-slash), populated via
	// git_submodule_foreach.
	TArray<FString> SubmodulePaths;

	// Submodules to include in per-repo LFS lock polling. As of the v0.2.0 submodule-lock fix
	// this is the ENTIRE SubmodulePaths set — the old `.gitattributes` peek missed submodules
	// with nested attribute files, and a missed submodule meant its locks never appeared across
	// editor restarts. Endpoint resolution silently no-ops for non-LFS submodules, so polling
	// everything is bounded in cost.
	TArray<FString> LfsSubmodulePaths;

	// Per-submodule tracked-file sets, keyed by absolute submodule root (with trailing /).
	// Values are repo-relative tracked paths, lower-cased for case-insensitive lookup on
	// Windows. Refreshed only on full reconnect — a file added to a submodule's index between
	// connects won't appear here, but the next UpdateStatus full sweep stamps the real Tree
	// state regardless.
	TMap<FString, TSet<FString>> SubmoduleTrackedSets;
};

// --------------------------------------------------------------------------------------------------------------------
// FGitLink_Provider — libgit2-backed ISourceControlProvider.
//
// Owned by FGitLinkModule (as a direct member, not a TUniquePtr, so the address stays stable for
// IModularFeatures registration). Init() opens the gitlink::FRepository; Execute() dispatches
// operations via FGitLink_CommandDispatcher.
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

	// Repository / subprocess / LFS HTTP client accessors return shared ownership handles. The
	// provider is the primary owner (constructed in CheckRepositoryStatus, dropped in Close) but
	// async command bodies and commands that fan out across worker threads must hold the
	// TSharedPtr for as long as they dereference the object, so an `Init(force=true)` teardown
	// mid-command can't free it out from under them. Returning a raw pointer here previously
	// caused use-after-free crashes — on the LFS client's mutex (v0.3.6) and on the repository
	// handle held by in-flight async command bodies across a reconnect (the v0.3.10 follow-up).
	// MUST be called on the game thread (the TSharedPtr slot itself is reassigned there with no
	// lock); the dispatcher snapshots them into FCommandContext before launching the async body.
	auto Get_Repository()    const -> TSharedPtr<gitlink::FRepository>;
	auto Get_Subprocess()    const -> TSharedPtr<FGitLink_Subprocess>;
	auto Get_LfsHttpClient() const -> TSharedPtr<FGitLink_LfsHttpClient>;

	// Immutable snapshot of connect-time metadata. Safe to call from any thread; returns null
	// when no repository is open. Callers doing several related queries should grab the
	// snapshot once and reuse it (each public helper below grabs its own otherwise).
	auto Get_ConnectSnapshot() const -> TSharedPtr<const FGitLink_ConnectSnapshot>;

	// True when LFS is installed (git lfs version succeeded at connect time) AND the user
	// has bUseLfsLocking enabled in the plugin settings. Drives UsesCheckout().
	auto Is_LfsAvailable() const -> bool { return _bLfsAvailable; }
	auto NeedsSubprocessForCommit() const -> bool { return _bHasPreCommitOrCommitMsgHook; }

	// True when the file's extension matches a wildcard from .gitattributes marked with the
	// 'lockable' attribute. Populated once at connect time by probing via
	// `git check-attr lockable *.uasset *.umap ...`. Thread-safe (reads the connect snapshot).
	auto Is_FileLockable(const FString& InAbsolutePath) const -> bool;

	// True when the file lives inside a git submodule.
	// Submodule files should not be checked out / checked in from the parent repo.
	// Thread-safe (reads the connect snapshot).
	auto Is_InSubmodule(const FString& InAbsolutePath) const -> bool;

	// Returns the absolute path to the submodule root containing this file, or empty if
	// the file is not in a submodule. Thread-safe (reads the connect snapshot).
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
	// iterate over submodules for per-submodule LFS lock polling. Thread-safe.
	auto Get_SubmodulePaths() const -> TArray<FString>;

	// Submodules included in per-repo LFS lock polling. As of the v0.2.0 submodule-lock fix
	// this is the ENTIRE submodule set (see FGitLink_ConnectSnapshot::LfsSubmodulePaths for
	// why the old `.gitattributes` filter was dropped). Thread-safe.
	auto Get_LfsSubmodulePaths() const -> TArray<FString>;

	// True when the given absolute path is a tracked file inside one of the submodules.
	// Populated at connect time by enumerating each submodule's git index. Used by GetState
	// to decide whether a brand-new submodule file query should default to Tree=Unmodified
	// (tracked file the parent's status walk can't see) or Tree=NotInRepo (untracked file —
	// must NOT be offered for checkout, fixes the "new file in submodule prompts to lock"
	// bug). Case-insensitive lookup. Thread-safe (reads the connect snapshot).
	auto Is_TrackedInSubmodule(const FString& InAbsolutePath) const -> bool;

	// Connect-time metadata accessors. Return by value from the immutable connect snapshot so
	// they're safe to call from worker threads while the game thread re-runs
	// CheckRepositoryStatus. Empty when no repository is open.
	auto Get_PathToRepositoryRoot() const -> FString;
	auto Get_UserName()             const -> FString;
	auto Get_UserEmail()            const -> FString;
	auto Get_BranchName()           const -> FString;
	auto Get_RemoteUrl()            const -> FString;

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

	// Most recent repository-open failure from CheckRepositoryStatus, for surfacing in the
	// Connect dialog. Empty if the last open succeeded. Thread-safe.
	auto Get_LastConnectError() const -> FText;

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

	// Repository, subprocess and LFS HTTP client are held as TSharedPtr so async command bodies
	// (and commands that fan out across worker threads) hold a refcounted handle that keeps the
	// underlying object alive across a concurrent Close() / Reset(). Provider is the primary
	// owner; the secondary references are short-lived (one per in-flight command). The
	// TSharedPtr slots themselves are only reassigned on the game thread, and Get_* accessors
	// must only be called there (the dispatcher snapshots them into FCommandContext on the game
	// thread before launching the async body). See v0.3.6 / v0.3.10-follow-up in CLAUDE.md.
	TSharedPtr<gitlink::FRepository>       _Repository;     // present once a repo is open
	TSharedPtr<FGitLink_Subprocess>        _Subprocess;     // present once a repo is open
	TSharedPtr<FGitLink_LfsHttpClient>     _LfsHttpClient;  // present once a repo is open
	TUniquePtr<FGitLink_StateCache>        _StateCache;
	TUniquePtr<FGitLink_CommandDispatcher> _Dispatcher;
	TUniquePtr<FGitLink_BackgroundPoll>    _BackgroundPoll;
	TUniquePtr<FGitLink_Menu>              _Menu;

	// std::atomic because worker-thread code (Request_LockRefreshForFile fired from
	// Cmd_UpdateStatus's explicit-file branch, command predicates) reads these while the game
	// thread rebuilds the connection.
	std::atomic<bool> _bGitRepositoryFound{false};
	std::atomic<bool> _bLfsAvailable{false};
	std::atomic<bool> _bHasPreCommitOrCommitMsgHook{false};  // from HookProbe at connect time

	// Immutable connect-time metadata, published by CheckRepositoryStatus with a guarded
	// pointer swap and cleared in Close(). Read via Get_ConnectSnapshot() from any thread.
	mutable FCriticalSection                    _ConnectSnapshotLock;
	TSharedPtr<const FGitLink_ConnectSnapshot>  _ConnectSnapshot;

	// Liveness token for game-thread continuations queued by Request_LockRefreshForFile.
	// Captured by value so the copy outlives the provider; flipped false at engine-exit Close()
	// (and in the destructor) so a continuation that fires after teardown no-ops instead of
	// touching freed provider state. Same mechanics as the dispatcher's _Alive token (v0.3.10).
	TSharedRef<std::atomic<bool>> _ProviderAlive = MakeShared<std::atomic<bool>>(true);

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

	// Most recent connect failure, surfaced via the log. Overwritten (not appended) on each
	// failed CheckRepositoryStatus so repeated Init(force=true) retries on a machine without a
	// repo can't grow it unboundedly.
	mutable FCriticalSection _LastErrorLock;
	FText _LastError;
};
