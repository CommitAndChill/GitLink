#include "Cmd_Shared.h"
#include "GitLink_CommandDispatcher.h"
#include "GitLink_LfsHttpClient.h"

#include "GitLink/GitLink_Provider.h"
#include "GitLink/GitLink_Revision.h"
#include "GitLinkLog.h"

#include "GitLinkCore/Repository/GitLink_Repository.h"
#include "GitLinkCore/Types/GitLink_Types.h"

#include <Misc/Paths.h>
#include <SourceControlOperations.h>
#include <Templates/Atomic.h>

// Forward declare the blob size op so we can populate FGitLink_Revision::_FileSize in history.
namespace gitlink::op
{
	auto Get_BlobSizeAtCommit(gitlink::FRepository& InRepo, const FString& InCommitHash, const FString& InRepoRelativePath) -> int64;
	auto Enumerate_TrackedFiles(gitlink::FRepository& InRepo) -> TArray<FString>;
}

namespace
{
	// Throttle for the expensive cross-repo sweep (per-submodule git status + per-repo LFS
	// lock poll). Background poll fires every PollIntervalSeconds on the timer (default 120s
	// post-Stage B). Saves trigger an immediate re-poll which would otherwise re-run the full
	// 34-repo sweep — that's the source of the visible CPU spikes. We skip the sweep if it
	// ran recently; the regular timer poll always runs because the throttle is < interval.
	//
	// Stage B raised this from 20s → 110s to match the 120s default sweep cadence. Foreground
	// freshness (≤2s on user-touched files) is now driven by single-file probes from editor
	// delegates (focus, package-dirty, asset-open, pre-checkout) — see GitLink_LfsHttpClient's
	// Request_SingleFileLock — so the cross-repo sweep no longer needs to be the latency floor
	// for "someone else locked this file".
	TAtomic<double> GLastFullSweepSec{0.0};
	constexpr double GFullSweepThrottleSec = 110.0;
}

// --------------------------------------------------------------------------------------------------------------------
// Cmd_UpdateStatus — translates gitlink::FStatus into FGitLink_FileState entries merged into
// the provider's cache. This is THE command that drives every content-browser icon + source
// control column, so correctness here gates everything else.
//
// Strategy:
//   1. Call Repository->Get_Status() once to snapshot the whole working tree.
//   2. Build a map from absolute-path -> FGitLink_CompositeState populated from the snapshot.
//      Files that appear in both Staged and Unstaged buckets merge into a single state whose
//      TreeState reflects the most relevant bucket (Working trumps Staged for the user's eye,
//      because "has unstaged changes" is the more actionable signal).
//   3. If the caller passed an explicit file list, emit an FGitLink_FileState for each requested
//      file, looking up the computed composite state (or Unmodified if not found). This ensures
//      the editor sees every file it asked about, even clean ones.
//   4. If the caller passed no files, emit states for every entry in the snapshot.
// --------------------------------------------------------------------------------------------------------------------

#define LOCTEXT_NAMESPACE "GitLinkCmdUpdateStatus"

namespace gitlink::cmd
{
	namespace
	{
		// LFS-aware bucket appender — extends the shared AppendBucket with LFS state propagation.
		auto AppendBucket_WithLfs(
			const TArray<FFileChange>& InChanges,
			EStatusBucket              InBucket,
			const FString&             InRepoRoot,
			TMap<FString, FGitLink_CompositeState>& OutCompositeByPath) -> void
		{
			for (const FFileChange& Change : InChanges)
			{
				const FString AbsolutePath = BuildAbsolutePath(InRepoRoot, Change.Path);
				FGitLink_CompositeState& State = OutCompositeByPath.FindOrAdd(AbsolutePath);
				Merge_CompositeState(State, InBucket, Change);

				// Stamp LFS state from the change. Real LFS integration lands with Cmd_CheckOut /
				// Cmd_Lfs; for now we propagate whatever libgit2 detected.
				if (Change.LfsStatus != ELfsStatus::NotLfs)
				{
					State.Lock = EGitLink_LockState::Unknown;  // lock state probed separately later
				}
			}
		}
	}

	// ----------------------------------------------------------------------------------------------------------------
	auto UpdateStatus(
		FCommandContext&                  InCtx,
		const FSourceControlOperationRef& InOperation,
		const TArray<FString>&            InFiles) -> FCommandResult
	{
		if (InCtx.Repository == nullptr || !InCtx.Repository->IsOpen())
		{
			return FCommandResult::Fail(LOCTEXT("UpdateStatusNoRepo",
				"GitLink: cannot update status — repository not open."));
		}

		// 1+2. Snapshot status, fold into per-path composite.
		const FStatus Snapshot = InCtx.Repository->Get_Status();

		TMap<FString, FGitLink_CompositeState> CompositeByPath;
		CompositeByPath.Reserve(Snapshot.Num());

		AppendBucket_WithLfs(Snapshot.Staged,     EStatusBucket::Staged,     InCtx.RepoRootAbsolute, CompositeByPath);
		AppendBucket_WithLfs(Snapshot.Unstaged,   EStatusBucket::Unstaged,   InCtx.RepoRootAbsolute, CompositeByPath);
		AppendBucket_WithLfs(Snapshot.Conflicted, EStatusBucket::Conflicted, InCtx.RepoRootAbsolute, CompositeByPath);

		FCommandResult Result = FCommandResult::Ok();
		Result.UpdatedStates.Reserve(FMath::Max(InFiles.Num(), CompositeByPath.Num()));

		// Check the operation flags. FUpdateStatus has bUpdateHistory which the editor sets
		// when the user opens the History dialog on a file — we need to walk the git log
		// filtered by that file and populate FGitLink_FileState::_History with revisions.
		bool bUpdateHistory = false;
		if (InOperation->GetName() == TEXT("UpdateStatus"))
		{
			TSharedRef<FUpdateStatus> UpdateOp = StaticCastSharedRef<FUpdateStatus>(InOperation);
			bUpdateHistory = UpdateOp->ShouldUpdateHistory();
		}

		const FDateTime Now = FDateTime::UtcNow();

		// Populate an FGitLink_FileState::_History array by walking the log, path-filtered
		// on the repo-relative path. Up to 100 revisions for v1.
		//
		// For files in submodules, we open the submodule's repo temporarily and query
		// its log instead of the parent repo's (which has no history for those files).
		auto BuildHistory = [&InCtx](const FString& InAbsoluteFilename) -> FGitLink_History
		{
			FGitLink_History History;

			// Determine which repo to query and the relative path within it.
			gitlink::FRepository* RepoToQuery = InCtx.Repository;
			TUniquePtr<gitlink::FRepository> SubmoduleRepo;
			FString RelativePath;
			FString RepoRoot = InCtx.RepoRootAbsolute;

			const FString SubmoduleRoot = InCtx.Provider.Get_SubmoduleRoot(InAbsoluteFilename);
			if (!SubmoduleRoot.IsEmpty())
			{
				// File is in a submodule — open the submodule's repo for history.
				gitlink::FOpenParams SubParams;
				SubParams.Path = SubmoduleRoot;
				SubmoduleRepo = gitlink::FRepository::Open(SubParams);

				if (SubmoduleRepo.IsValid())
				{
					RepoToQuery = SubmoduleRepo.Get();
					RepoRoot    = SubmoduleRoot;
				}
				else
				{
					UE_LOG(LogGitLink, Warning,
						TEXT("BuildHistory: could not open submodule repo at '%s'"), *SubmoduleRoot);
					return History;
				}
			}

			RelativePath = ToRepoRelativePath(RepoRoot, InAbsoluteFilename);

			if (RelativePath.IsEmpty() || RepoToQuery == nullptr)
			{
				UE_LOG(LogGitLink, Warning,
					TEXT("BuildHistory: could not derive relative path for '%s' (repoRoot='%s')"),
					*InAbsoluteFilename, *RepoRoot);
				return History;
			}

			UE_LOG(LogGitLink, Verbose,
				TEXT("BuildHistory: '%s' -> relative '%s' (submodule=%s)"),
				*InAbsoluteFilename, *RelativePath,
				SubmoduleRoot.IsEmpty() ? TEXT("no") : TEXT("yes"));

			FLogQuery Query;
			Query.PathFilter = RelativePath;
			Query.MaxCount   = 100;

			const TArray<FCommit> Commits = RepoToQuery->Get_Log(Query);
			History.Reserve(Commits.Num());

			int32 RevNumber = Commits.Num();
			for (int32 Idx = 0; Idx < Commits.Num(); ++Idx)
			{
				const FCommit& Commit = Commits[Idx];

				TSharedRef<FGitLink_Revision, ESPMode::ThreadSafe> Rev =
					MakeShared<FGitLink_Revision, ESPMode::ThreadSafe>();
				Rev->_Filename          = InAbsoluteFilename;
				Rev->_CommitId          = Commit.Hash;
				Rev->_ShortCommitId     = Commit.Hash.Left(8);
				Rev->_Description       = Commit.Summary.IsEmpty() ? Commit.Message : Commit.Summary;
				Rev->_UserName          = Commit.Author.Name;
				Rev->_Date              = Commit.Author.When;
				Rev->_RevisionNumber    = RevNumber--;

				// ChangeList column shows the numeric value of the first 8 hex chars
				// of the commit hash.
				Rev->_CheckInIdentifier = static_cast<int32>(
					FCString::Strtoui64(*Commit.Hash.Left(8), nullptr, 16));

				// Last entry in history = first commit that introduced the file.
				Rev->_Action = (Idx == Commits.Num() - 1) ? TEXT("add") : TEXT("modified");

				// Populate file size from the blob at this commit.
				Rev->_FileSize = static_cast<int32>(
					gitlink::op::Get_BlobSizeAtCommit(*RepoToQuery, Commit.Hash, RelativePath));

				History.Add(Rev);
			}

			// SubmoduleRepo destructs here, closing the handle automatically.
			return History;
		};

		auto BuildState = [&Now, &bUpdateHistory, &BuildHistory, &InCtx](
			const FString& InFilename, const FGitLink_CompositeState& InComposite)
			-> FGitLink_FileStateRef
		{
			FGitLink_FileStateRef State = MakeShared<FGitLink_FileState, ESPMode::ThreadSafe>(InFilename);
			State->_State     = InComposite;
			State->_TimeStamp = Now;

			// Always stamp bInSubmodule from the provider's authoritative path list. Callers
			// that populate CompositeByPath from git-status bucket data don't know about
			// submodules, and the flag must be correct for the state predicates to work.
			if (InCtx.Provider.Is_InSubmodule(InFilename))
			{
				State->_State.bInSubmodule = true;
			}

			// Carry forward the existing lock state from the cache so that a prior Locked
			// (from Cmd_CheckOut) isn't clobbered to NotLocked. Lock state is owned by
			// Cmd_CheckOut / Cmd_Revert / Cmd_Connect — UpdateStatus only needs to ensure
			// the lockable/unlockable distinction is correct for files not yet queried.
			const FGitLink_FileStateRef Existing = InCtx.StateCache.Find_FileState(InFilename);
			if (Existing->_State.Lock != EGitLink_LockState::Unknown)
			{
				State->_State.Lock = Existing->_State.Lock;
				State->_State.LockUser = Existing->_State.LockUser;
			}
			else
			{
				// Lockability is .gitattributes-driven and applies the same way to outer-repo
				// and submodule files (most projects use a consistent set of lockable patterns).
				// Real Lock=Locked/LockedOther state lands later from the per-submodule LFS
				// poll in this same command's full-scan path.
				const bool bLockable = InCtx.Provider.Is_FileLockable(InFilename);
				State->_State.Lock = bLockable
					? EGitLink_LockState::NotLocked
					: EGitLink_LockState::Unlockable;
			}

			// Assign to a changelist so View Changes knows where this file belongs.
			if (InComposite.Tree == EGitLink_TreeState::Staged)
			{
				State->_Changelist = FGitLink_Changelist::StagedChangelist;
			}
			else if (InComposite.Tree == EGitLink_TreeState::Working ||
					 InComposite.Tree == EGitLink_TreeState::Untracked)
			{
				State->_Changelist = FGitLink_Changelist::WorkingChangelist;
			}

			if (bUpdateHistory)
			{
				State->_History = BuildHistory(InFilename);
			}
			return State;
		};

		if (InFiles.IsEmpty())
		{
			// Full snapshot mode. The expensive cross-repo sweep (per-submodule git status +
			// per-repo LFS lock poll) is throttled — it always runs on the timer-driven
			// background poll (30s interval) but is skipped on save-triggered immediate polls
			// that fall inside the throttle window. Without this, every save would re-run
			// 34 git status walks + 34 LFS network calls, causing visible CPU spikes.
			const double NowSec = FPlatformTime::Seconds();
			const double LastSweep = GLastFullSweepSec.Load();
			const bool bRunSweep = (NowSec - LastSweep) >= GFullSweepThrottleSec;

			const TArray<FString> SubmodulePaths = InCtx.Provider.Get_SubmodulePaths();
			TSet<FString> SuccessfullyPolledRepos;  // absolute roots whose LFS poll returned trustworthy data

			if (bRunSweep)
			{
				GLastFullSweepSec.Store(NowSec);

				// Per-submodule git status. The parent status already populated CompositeByPath
				// above. For each submodule, open its repo and run Get_Status, then merge the
				// staged/unstaged buckets into CompositeByPath using the submodule root as the
				// path base. Result: untracked / modified submodule files now show real File +
				// Tree state, so CanCheckout's bUntracked guard works correctly.
				if (!SubmodulePaths.IsEmpty())
				{
					struct FSubStatus
					{
						bool    bOpened = false;
						FStatus Status;
					};
					TArray<FSubStatus> SubStatuses;
					SubStatuses.SetNum(SubmodulePaths.Num());

					ParallelFor_BoundedConcurrency(SubmodulePaths.Num(), GMaxConcurrentStatusWalkers, [&](int32 Idx)
					{
						TUniquePtr<gitlink::FRepository> SubRepo =
							InCtx.Provider.Open_SubmoduleRepositoryFor(SubmodulePaths[Idx]);
						if (SubRepo.IsValid())
						{
							SubStatuses[Idx].bOpened = true;
							SubStatuses[Idx].Status  = SubRepo->Get_Status();
						}
					});

					int32 SubDirty = 0;
					for (int32 Idx = 0; Idx < SubmodulePaths.Num(); ++Idx)
					{
						if (!SubStatuses[Idx].bOpened)
						{ continue; }

						const FString& SubRoot = SubmodulePaths[Idx];
						const FStatus& Snap    = SubStatuses[Idx].Status;
						AppendBucket_WithLfs(Snap.Staged,     EStatusBucket::Staged,     SubRoot, CompositeByPath);
						AppendBucket_WithLfs(Snap.Unstaged,   EStatusBucket::Unstaged,   SubRoot, CompositeByPath);
						AppendBucket_WithLfs(Snap.Conflicted, EStatusBucket::Conflicted, SubRoot, CompositeByPath);
						SubDirty += Snap.Num();
					}

					if (SubDirty > 0)
					{
						UE_LOG(LogGitLink, Verbose,
							TEXT("Cmd_UpdateStatus: per-submodule status added %d dirty entries across %d submodule(s)"),
							SubDirty, SubmodulePaths.Num());
					}
				}
			}
			else
			{
				UE_LOG(LogGitLink, Verbose,
					TEXT("Cmd_UpdateStatus: skipping cross-repo sweep (last %.1fs ago, throttle %.0fs)"),
					NowSec - LastSweep, GFullSweepThrottleSec);
			}

			// Emit a state for every entry in CompositeByPath. (May include submodule files
			// from the per-submodule status we just merged in.)
			for (const TPair<FString, FGitLink_CompositeState>& Pair : CompositeByPath)
			{
				Result.UpdatedStates.Add(BuildState(Pair.Key, Pair.Value));
			}

			// LFS lock refresh — also gated by the throttle. Each submodule typically has its
			// own LFS endpoint; we poll all repos in parallel and merge into absolute-path-keyed
			// maps before applying.
			if (bRunSweep && InCtx.Subprocess != nullptr && InCtx.Subprocess->IsValid() && InCtx.Provider.Is_LfsAvailable())
			{
				TMap<FString, FString> RemoteLocksAbs;  // absolute path → owner
				TSet<FString>          OursAbs;         // absolute paths the LFS server confirms are ours

				auto AbsolutizeFor = [](const FString& InRepoRoot, const FString& InRelPath) -> FString
				{
					return Normalize_AbsolutePath(FPaths::Combine(InRepoRoot, InRelPath));
				};

				// Scope LFS polling to repos that actually use LFS — most submodules in a
				// typical project (source-only plugins, etc.) don't have LFS configured at
				// all, and querying them every 30s is the dominant CPU cost. The per-submodule
				// list was probed at connect time by reading each submodule's .gitattributes.
				const TArray<FString> LfsSubmodulePaths = InCtx.Provider.Get_LfsSubmodulePaths();
				TArray<TPair<FString, FString>> RepoPolls;
				RepoPolls.Reserve(1 + LfsSubmodulePaths.Num());
				RepoPolls.Add({ InCtx.RepoRootAbsolute, FString() });
				for (const FString& SubRoot : LfsSubmodulePaths)
				{ RepoPolls.Add({ SubRoot, SubRoot }); }

				TArray<FGitLink_Subprocess::FLfsLocksSnapshot> Snapshots;
				Snapshots.SetNum(RepoPolls.Num());

				// Snapshot the LFS client + subprocess handles before fanning out. Worker threads
				// must NOT re-fetch via `InCtx.Provider.Get_LfsHttpClient()` mid-loop — a
				// concurrent `FGitLink_Provider::Close()` (fired by editor `Init(force=true)` or
				// re-init) resets the provider's TSharedPtr, and a worker dereferencing a stale
				// raw pointer crashed in `TScopeLock`'s ctor on `_EndpointsLock`. The local
				// snapshots here participate in TSharedPtr's refcount so the underlying objects
				// outlive every worker for the duration of this ParallelFor (which is blocking
				// on the calling task thread). See v0.3.6 in CLAUDE.md Version log.
				const TSharedPtr<FGitLink_LfsHttpClient> LfsClient = InCtx.Provider.Get_LfsHttpClient();
				const TSharedPtr<FGitLink_Subprocess>    Subprocess = InCtx.Subprocess;
				const bool bUseHttp = gitlink::lfs_http::Is_Enabled() && LfsClient.IsValid();

				ParallelFor_BoundedConcurrency(RepoPolls.Num(), GMaxConcurrentRepoWorkers, [&](int32 Idx)
				{
					const FString& RepoRoot = RepoPolls[Idx].Key;
					if (bUseHttp && LfsClient->Has_LfsUrl(RepoRoot))
					{
						Snapshots[Idx] = LfsClient->Request_LocksVerify(RepoRoot);
						if (Snapshots[Idx].bSuccess) { return; }
						// HTTP path failed (transport / 401 / etc.) — fall through to subprocess
						// for this repo. This keeps a single repo's auth misconfiguration from
						// poisoning the whole poll cycle.
					}
					Snapshots[Idx] = Subprocess->QueryLfsLocks_Verified(RepoPolls[Idx].Value);
				});

				for (int32 Idx = 0; Idx < RepoPolls.Num(); ++Idx)
				{
					const FString& RepoRoot = RepoPolls[Idx].Key;
					const FGitLink_Subprocess::FLfsLocksSnapshot& Snap = Snapshots[Idx];
					if (Snap.bSuccess)
					{ SuccessfullyPolledRepos.Add(RepoRoot); }
					for (const auto& [RelPath, Owner] : Snap.AllLocks)
					{
						const FString Abs = AbsolutizeFor(RepoRoot, RelPath);
						RemoteLocksAbs.Add(Abs, Owner);
						if (Snap.OursPaths.Contains(RelPath))
						{ OursAbs.Add(Abs); }
					}
				}

				int32 LockChanges = 0;

				for (const auto& [Absolute, LockOwner] : RemoteLocksAbs)
				{
					// Replica-lag guard: if we just locked or unlocked this file locally
					// within the last ~30s, the cache's lock state is locally-authoritative.
					// A stale /locks/verify response (from before the LFS server's read replica
					// has caught up to our write) must not be allowed to flip the state back.
					if (InCtx.Provider.Was_RecentLocalLockOp(Absolute))
					{ continue; }

					const bool bIsOurs = OursAbs.Contains(Absolute);
					const EGitLink_LockState NewLockState = bIsOurs
						? EGitLink_LockState::Locked
						: EGitLink_LockState::LockedOther;

					// Check if this lock is already correctly represented in our emitted states.
					bool bAlreadyEmitted = false;
					for (const FGitLink_FileStateRef& State : Result.UpdatedStates)
					{
						if (State->GetFilename().Equals(Absolute, ESearchCase::IgnoreCase))
						{
							if (State->_State.Lock != NewLockState || State->_State.LockUser != LockOwner)
							{
								State->_State.Lock     = NewLockState;
								State->_State.LockUser = LockOwner;
								++LockChanges;
							}
							bAlreadyEmitted = true;
							break;
						}
					}

					if (!bAlreadyEmitted)
					{
						// File is locked but not dirty — check if cache already has the right state.
						const FGitLink_FileStateRef Existing = InCtx.StateCache.Find_FileState(Absolute);
						if (Existing->_State.Lock != NewLockState || Existing->_State.LockUser != LockOwner)
						{
							FGitLink_CompositeState Composite = Existing->_State;
							Composite.Lock     = NewLockState;
							Composite.LockUser = LockOwner;

							// Ensure tree state is reasonable for a clean locked file.
							if (Composite.Tree == EGitLink_TreeState::NotInRepo)
							{
								Composite.Tree = EGitLink_TreeState::Unmodified;
							}

							// Preserve / set the bInSubmodule flag from authoritative provider data.
							Composite.bInSubmodule = InCtx.Provider.Is_InSubmodule(Absolute);

							Result.UpdatedStates.Add(Make_FileState(Absolute, Composite));
							++LockChanges;
						}
					}
				}

				// Lock-released detection: a file in our cache marked Locked/LockedOther that
				// no longer appears in any remote lock set should be reset. Critical: only
				// consider files in repos whose poll *succeeded*. A failed poll returns an
				// empty AllLocks which would otherwise be misread as "all locks released",
				// causing the cache to flip Lock=Locked → NotLocked → Locked on the next
				// successful poll, breaking Revert UX.
				TSet<FString> RemoteLockAbsoluteSet;
				RemoteLockAbsoluteSet.Reserve(RemoteLocksAbs.Num());
				for (const auto& [Absolute, _] : RemoteLocksAbs)
				{ RemoteLockAbsoluteSet.Add(Absolute); }

				const TArray<FGitLink_FileStateRef> LockedStates = InCtx.StateCache.Enumerate_FileStates(
					[](const FGitLink_FileStateRef& InState)
					{
						return InState->_State.Lock == EGitLink_LockState::Locked
							|| InState->_State.Lock == EGitLink_LockState::LockedOther;
					});
				for (const FGitLink_FileStateRef& Cached : LockedStates)
				{
					if (RemoteLockAbsoluteSet.Contains(Cached->GetFilename()))
					{ continue; }

					// Replica-lag guard: same rationale as the lock-stamping path above.
					// If we just locked the file locally and the server's response hasn't
					// caught up yet, the file appears absent from RemoteLockAbsoluteSet —
					// without this guard the cache would be cleared back to NotLocked.
					if (InCtx.Provider.Was_RecentLocalLockOp(Cached->GetFilename()))
					{ continue; }

					// Determine which repo this cached file belongs to and confirm we polled
					// it successfully this round. If not, skip — we don't have authoritative
					// data for that repo, so leave the cached lock state alone.
					const FString OwnerRepo = InCtx.Provider.Is_InSubmodule(Cached->GetFilename())
						? InCtx.Provider.Get_SubmoduleRoot(Cached->GetFilename())
						: InCtx.RepoRootAbsolute;
					if (!SuccessfullyPolledRepos.Contains(OwnerRepo))
					{ continue; }

					// Lock was genuinely released — update to NotLocked (or Unlockable if extension isn't lockable).
					const bool bLockable = InCtx.Provider.Is_FileLockable(Cached->GetFilename());

					FGitLink_CompositeState Composite = Cached->_State;
					Composite.Lock     = bLockable ? EGitLink_LockState::NotLocked : EGitLink_LockState::Unlockable;
					Composite.LockUser.Reset();

					Result.UpdatedStates.Add(Make_FileState(Cached->GetFilename(), Composite));
					++LockChanges;
				}

				if (LockChanges > 0)
				{
					UE_LOG(LogGitLink, Log,
						TEXT("Cmd_UpdateStatus: remote lock refresh across 1 parent + %d LFS submodule(s) of %d total — %d lock state change(s), %d/%d repos polled successfully"),
						LfsSubmodulePaths.Num(), SubmodulePaths.Num(), LockChanges,
						SuccessfullyPolledRepos.Num(), RepoPolls.Num());
				}
			}
		}
		else
		{
			// Explicit file list — every requested file gets an emitted state, even if it's
			// clean, so the editor knows to clear stale modification markers.
			//
			// Build a set of tracked files (paths in the git index) so we can distinguish
			// between (a) tracked + unmodified files and (b) brand new untracked files that
			// git doesn't know about yet. Without this, new files would be stamped as
			// Unmodified, which causes CanCheckout() to return true and the editor to prompt
			// for a checkout on a file that isn't in git.
			const TArray<FString> TrackedRelPaths = gitlink::op::Enumerate_TrackedFiles(*InCtx.Repository);
			TSet<FString> TrackedSet;
			TrackedSet.Reserve(TrackedRelPaths.Num());
			for (const FString& RelPath : TrackedRelPaths)
			{ TrackedSet.Add(RelPath); }

			// Stage B — fire-and-forget async LFS lock refresh for any lockable requested file.
			//
			// Earlier rev of this code blocked the calling thread on `Request_SingleFileLock`
			// inside a ParallelFor. That deadlocked when UpdateStatus was dispatched
			// synchronously on the game thread (the editor's pre-checkout flow does this):
			// UE's HTTP module needs the game thread to tick to fire completion delegates, but
			// the game thread was blocked inside our `FEvent::Wait`, so every probe ran out the
			// 12 s timeout. With ~80 files (typical content-browser refresh) at 8-worker
			// parallelism that pinned the editor for ~2 minutes at ~85% CPU on the spinning
			// workers — exactly what was reported in v0.3.0.
			//
			// Fix: kick off async refreshes via Provider::Request_LockRefreshForFile (the same
			// path the editor delegates use — background thread, debounced per-path at 2 s,
			// cache mutation marshalled back to the game thread). The game thread is never
			// blocked. Foreground freshness still holds because:
			//   1. The asset-opened / package-dirty / focus delegates already probed the file
			//      shortly before the editor dispatched this UpdateStatus. The 2s debounce
			//      collapses our redispatch into the existing in-flight probe.
			//   2. When the async probe completes, `OnSourceControlStateChanged` fires and the
			//      editor re-queries — the second cache read sees the fresh state.
			//   3. The cached lock state from the previous successful sweep / probe is what
			//      gets emitted on this UpdateStatus, which is far less stale than the 30s
			//      Stage A baseline.
			if (gitlink::lfs_http::Is_Enabled()
				&& InCtx.Provider.Is_LfsAvailable()
				&& InCtx.Provider.Get_LfsHttpClient().IsValid())
			{
				for (const FString& RequestedRaw : InFiles)
				{
					InCtx.Provider.Request_LockRefreshForFile(RequestedRaw);
				}
			}

			for (const FString& RequestedRaw : InFiles)
			{
				FString Normalized = RequestedRaw;
				FPaths::NormalizeFilename(Normalized);

				if (const FGitLink_CompositeState* Hit = CompositeByPath.Find(Normalized))
				{
					Result.UpdatedStates.Add(BuildState(Normalized, *Hit));
				}
				else
				{
					// Not in the dirty snapshot. Look it up in the git index to decide
					// between Unmodified (tracked + clean) and Untracked (new file).
					const FString RelPath = ToRepoRelativePath(InCtx.RepoRootAbsolute, Normalized);

					FGitLink_CompositeState Clean;
					Clean.File = EGitLink_FileState::Unknown;

					// Submodule files need the same tracked-vs-untracked decision the parent
					// makes via TrackedSet.Contains(RelPath), but driven by a per-submodule
					// tracked-files index built once at connect (Provider::Is_TrackedInSubmodule).
					// Without this distinction, brand-new files in a submodule would default to
					// Tree=Unmodified — which makes CanCheckout return true and the editor
					// auto-prompts to LFS-lock a file that isn't even in git yet (and revert
					// can't break the cycle: unlock succeeds but the next save re-locks it).
					// IsSourceControlled() forces true for submodule files so History stays
					// enabled regardless of the Tree value here.
					const bool bInSubmodule = InCtx.Provider.Is_InSubmodule(Normalized);
					Clean.bInSubmodule = bInSubmodule;

					if (bInSubmodule)
					{
						Clean.Tree = InCtx.Provider.Is_TrackedInSubmodule(Normalized)
							? EGitLink_TreeState::Unmodified
							: EGitLink_TreeState::Untracked;
					}
					else
					{
						Clean.Tree = TrackedSet.Contains(RelPath)
							? EGitLink_TreeState::Unmodified
							: EGitLink_TreeState::Untracked;
					}

					Result.UpdatedStates.Add(BuildState(Normalized, Clean));
				}
			}
		}

		UE_LOG(LogGitLink, Verbose,
			TEXT("Cmd_UpdateStatus: files_requested=%d, dirty_in_snapshot=%d, states_emitted=%d"),
			InFiles.Num(),
			CompositeByPath.Num(),
			Result.UpdatedStates.Num());

		// Unused in this command but satisfies -Wunused-parameter without adding /*_*/.
		(void)InOperation;

		return Result;
	}
}

#undef LOCTEXT_NAMESPACE
