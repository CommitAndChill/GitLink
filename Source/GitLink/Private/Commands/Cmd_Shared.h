#pragma once

#include "GitLink/GitLink_Provider.h"
#include "GitLink/GitLink_State.h"
#include "GitLink_CommandDispatcher.h"
#include "GitLink_StateCache.h"
#include "GitLink_Subprocess.h"
#include "GitLinkLog.h"

#include <Async/ParallelFor.h>
#include <CoreMinimal.h>
#include <Misc/DateTime.h>
#include <Misc/Paths.h>

#include "GitLinkCore/Types/GitLink_Types.h"

// --------------------------------------------------------------------------------------------------------------------
// Shared helpers for Cmd_*.cpp files. Kept in Private/ so nothing outside the GitLink module
// can see them. Intentionally free functions — no class, no namespace gymnastics.
// --------------------------------------------------------------------------------------------------------------------

namespace gitlink::cmd
{
	// Maximum number of concurrent expensive cross-repo workers per poll cycle. Caps two
	// kinds of work that scale linearly with the number of submodules:
	//
	//   1. `git lfs locks --verify --json` subprocess spawns (one per LFS-using repo).
	//      Each is a process startup + HTTPS round-trip + JSON parse. Unbounded
	//      parallelism saturates all CPU cores during the 30s timer poll.
	//
	//   2. Per-submodule libgit2 `Get_Status()` walks (one per submodule). Disk-bound
	//      and contention-prone when run all-at-once.
	//
	// Empirically 8 keeps CPU usage smooth on a 16-thread CPU while still completing
	// each cycle in well under the 30s background-poll interval.
	constexpr int32 GMaxConcurrentRepoWorkers = 8;

	// Cap for the CPU-bound per-submodule libgit2 `Get_Status()` walks specifically.
	// `GMaxConcurrentRepoWorkers` is sized for the network-bound LFS HTTP poll (8 in flight
	// hides per-request latency without saturating cores — most workers spend their time in
	// FEvent::Wait). Status walks are the opposite shape: each walk reads the entire
	// submodule's working tree off-disk and hashes it through libgit2 — pure CPU + disk.
	// Stage B observation: with 34 submodules in the BB project, an 8-worker fan-out of
	// status walks pinned all 8 physical cores during each sweep, producing the visible
	// 120 s CPU spike. 4 keeps each spike under ~50% aggregate CPU; the sweep is background,
	// so the doubled wall-clock cost (still well under the 120 s poll interval on this
	// hardware) is invisible.
	constexpr int32 GMaxConcurrentStatusWalkers = 4;

	// Runs InWork on every index in [0, InCount) with at most InMaxConcurrent invocations
	// in flight at any one time. Implemented as sequential chunks of ParallelFor — every
	// chunk fans out to the thread pool, but no more than InMaxConcurrent tasks run
	// simultaneously. Use this in place of ParallelFor for cross-repo work that spawns
	// subprocesses or does heavy disk I/O.
	inline auto ParallelFor_BoundedConcurrency(
		int32 InCount,
		int32 InMaxConcurrent,
		TFunctionRef<void(int32)> InWork) -> void
	{
		if (InCount <= 0)
		{ return; }
		if (InMaxConcurrent <= 0)
		{ InMaxConcurrent = InCount; }

		for (int32 ChunkStart = 0; ChunkStart < InCount; ChunkStart += InMaxConcurrent)
		{
			const int32 ChunkEnd = FMath::Min(ChunkStart + InMaxConcurrent, InCount);
			ParallelFor(ChunkEnd - ChunkStart, [&InWork, ChunkStart](int32 LocalIdx)
			{
				InWork(ChunkStart + LocalIdx);
			});
		}
	}

	// Make an FGitLink_FileState stamped with the given composite state and current time.
	inline auto Make_FileState(const FString& InAbsoluteFilename, const FGitLink_CompositeState& InComposite)
		-> FGitLink_FileStateRef
	{
		FGitLink_FileStateRef State = MakeShared<FGitLink_FileState, ESPMode::ThreadSafe>(InAbsoluteFilename);
		State->_State     = InComposite;
		State->_TimeStamp = FDateTime::UtcNow();
		return State;
	}

	// Shortcut: make a state with just the pair (file, tree), leaving lock/remote at their
	// Unknown/UpToDate defaults. Useful for predictive post-op state updates — we know what
	// the state should look like after e.g. MarkForAdd succeeds, and can push the delta into
	// the cache without having to re-run the full status op.
	inline auto Make_FileState(const FString& InAbsoluteFilename, EGitLink_FileState InFile, EGitLink_TreeState InTree)
		-> FGitLink_FileStateRef
	{
		FGitLink_CompositeState Composite;
		Composite.File = InFile;
		Composite.Tree = InTree;
		return Make_FileState(InAbsoluteFilename, Composite);
	}

	// Normalize a user-supplied path to an absolute, forward-slashed form. The editor sometimes
	// passes relative paths (relative to Engine/Binaries/Win64/) so we must ConvertRelativePathToFull
	// before any repo-root prefix stripping or comparison.
	inline auto Normalize_AbsolutePath(const FString& InPath) -> FString
	{
		FString Out = FPaths::ConvertRelativePathToFull(InPath);
		FPaths::NormalizeFilename(Out);
		return Out;
	}

	// Convert an absolute file path to a repo-relative, forward-slash path suitable for passing
	// to git / git-lfs commands (both of which expect forward slashes regardless of platform).
	// Uses case-insensitive prefix stripping because Windows paths can differ in casing between
	// the repo root discovered by libgit2 and the paths the editor hands us.
	inline auto ToRepoRelativePath(const FString& InRepoRootAbsolute, const FString& InAbsolute) -> FString
	{
		FString NormAbs = FPaths::ConvertRelativePathToFull(InAbsolute);
		FPaths::NormalizeFilename(NormAbs);

		FString NormRoot = InRepoRootAbsolute;
		FPaths::NormalizeFilename(NormRoot);
		// Ensure trailing slash so the chop doesn't leave a leading '/' on the relative path.
		if (!NormRoot.EndsWith(TEXT("/")))
		{
			NormRoot += TEXT("/");
		}

		FString Rel = NormAbs;
		if (!NormRoot.IsEmpty() && Rel.StartsWith(NormRoot, ESearchCase::IgnoreCase))
		{
			Rel.RightChopInline(NormRoot.Len(), EAllowShrinking::No);
		}
		Rel.ReplaceInline(TEXT("\\"), TEXT("/"));
		Rel.RemoveFromStart(TEXT("/"));
		return Rel;
	}

	// One bucket of files that all live in the same repo (either the outer repo, or one
	// specific submodule). Used to route command operations into the correct repo so each
	// submodule's own libgit2 handle / LFS server / git config is picked up automatically.
	struct FRepoBatch
	{
		// Absolute path to the repo's working tree root, with trailing forward slash.
		// For the outer repo this matches FCommandContext::RepoRootAbsolute.
		FString RepoRoot;

		// True when this bucket is a submodule (i.e. RepoRoot != ctx.RepoRootAbsolute).
		// Cmd_* implementations switch on this to decide whether to use ctx.Repository
		// (the outer FRepository owned by the provider) or open a fresh FRepository via
		// Provider::Open_SubmoduleRepositoryFor.
		bool bIsSubmodule = false;

		// Files in this bucket, mirrored as absolute (editor form) and repo-relative
		// (libgit2 / git-lfs form) at the same indices.
		TArray<FString> AbsoluteFiles;
		TArray<FString> RelativeFiles;
	};

	// Partition a flat list of editor-supplied file paths into per-repo buckets. The
	// outer-repo bucket (if any) always comes first in the returned array; submodule
	// buckets follow in the order their roots were first encountered.
	inline auto PartitionByRepo(
		const FCommandContext& InCtx,
		const TArray<FString>& InFiles) -> TArray<FRepoBatch>
	{
		FRepoBatch OuterBatch;
		OuterBatch.RepoRoot     = InCtx.RepoRootAbsolute;
		OuterBatch.bIsSubmodule = false;

		TMap<FString, int32> SubBucketIndex;  // submodule root → index into Subs
		TArray<FRepoBatch> Subs;

		for (const FString& File : InFiles)
		{
			const FString SubmoduleRoot = InCtx.Provider.Get_SubmoduleRoot(File);
			if (SubmoduleRoot.IsEmpty())
			{
				OuterBatch.AbsoluteFiles.Add(File);
				OuterBatch.RelativeFiles.Add(ToRepoRelativePath(InCtx.RepoRootAbsolute, File));
			}
			else
			{
				int32 Idx = SubBucketIndex.FindOrAdd(SubmoduleRoot, INDEX_NONE);
				if (Idx == INDEX_NONE)
				{
					FRepoBatch Sub;
					Sub.RepoRoot     = SubmoduleRoot;
					Sub.bIsSubmodule = true;
					Idx = Subs.Add(MoveTemp(Sub));
					SubBucketIndex[SubmoduleRoot] = Idx;
				}
				Subs[Idx].AbsoluteFiles.Add(File);
				Subs[Idx].RelativeFiles.Add(ToRepoRelativePath(SubmoduleRoot, File));
			}
		}

		TArray<FRepoBatch> Out;
		Out.Reserve(1 + Subs.Num());
		if (!OuterBatch.AbsoluteFiles.IsEmpty())
		{ Out.Add(MoveTemp(OuterBatch)); }
		Out.Append(MoveTemp(Subs));
		return Out;
	}

	// Outcome of an unlock attempt across a batch of files. Successful files are split out
	// from failed ones so callers can stamp Lock=NotLocked only on files that actually got
	// released, and leave the rest as Locked in the cache so the editor still shows them as
	// checked out and the user can retry. Without this split, a silent unlock failure flips
	// the cache to NotLocked while the server lock persists — that's the "submodule unlock
	// stuck" bug (lock owner identity vs git config user.name mismatch is the common cause).
	struct FLfsUnlockOutcome
	{
		// Absolute paths that were either successfully released OR were skipped because they
		// had nothing to release (non-lockable, LockedOther, no LFS subsystem, etc). Either
		// way the caller should stamp Lock=NotLocked / Unlockable for these.
		TSet<FString> Released;

		// Absolute paths in batches whose `git lfs unlock` invocation failed. Caller must
		// leave these as Lock=Locked so the user can retry without losing the cache state.
		TArray<FString> FailedPaths;

		// One entry per failing batch, suitable for surfacing to the user in a command-result
		// FText (combined into a single error message at the call site).
		TArray<FString> ErrorMessages;
	};

	// Releases any LFS locks that the current user holds on the given files. Best-effort
	// per *file*: a failure on one batch does not abort the others.
	//
	// Used by Cmd_Revert (release after discarding local changes) and Cmd_CheckIn (release
	// after the commit so the file is free for the next editor to check out).
	//
	// Submodule files: partitioned by submodule root and unlocked against each submodule's
	// own LFS server via the cwd override on FGitLink_Subprocess::RunLfs. The parent and
	// each submodule typically have different LFS endpoints, so a single batched unlock
	// against the parent would 404 for submodule paths.
	//
	// NOTE: consults InCtx.StateCache to skip files we know aren't Locked by us. Unknown-lock
	// files are still attempted because the editor cache might be stale.
	inline auto Release_LfsLocksBestEffort(
		FCommandContext&       InCtx,
		const TArray<FString>& InAbsoluteFiles) -> FLfsUnlockOutcome
	{
		FLfsUnlockOutcome Outcome;
		Outcome.Released.Reserve(InAbsoluteFiles.Num());

		// Helper: every input file becomes "released" (caller stamps NotLocked). Used for
		// the no-LFS-subsystem early returns to preserve the previous bool=true semantics.
		auto MarkAllReleased = [&]()
		{
			for (const FString& Absolute : InAbsoluteFiles)
			{ Outcome.Released.Add(Absolute); }
		};

		if (InCtx.Subprocess == nullptr || !InCtx.Subprocess->IsValid())
		{ MarkAllReleased(); return Outcome; }

		if (!InCtx.Provider.Is_LfsAvailable())
		{ MarkAllReleased(); return Outcome; }

		// Filter to files that are (a) lockable by extension and (b) plausibly held by us.
		// Files we don't even attempt count as "released" from the caller's POV (there was
		// no lock on them to begin with), preserving the previous filter behavior.
		TArray<FString> Filtered;
		Filtered.Reserve(InAbsoluteFiles.Num());
		for (const FString& Absolute : InAbsoluteFiles)
		{
			if (!InCtx.Provider.Is_FileLockable(Absolute))
			{
				Outcome.Released.Add(Absolute);
				continue;
			}

			const FGitLink_FileStateRef State = InCtx.StateCache.Find_FileState(Absolute);
			if (State->_State.Lock == EGitLink_LockState::Unlockable ||
			    State->_State.Lock == EGitLink_LockState::LockedOther)
			{
				Outcome.Released.Add(Absolute);
				continue;  // definitely not ours to release
			}

			Filtered.Add(Absolute);
		}

		if (Filtered.IsEmpty())
		{ return Outcome; }

		const TArray<FRepoBatch> Batches = PartitionByRepo(InCtx, Filtered);

		for (const FRepoBatch& Batch : Batches)
		{
			if (Batch.RelativeFiles.IsEmpty())
			{ continue; }

			// No --force flag: callers (Cmd_Revert, Cmd_CheckIn) discard/commit changes
			// before unlocking, so the working tree should be clean. Omitting --force also
			// avoids the "admin access required" error that occurs when the LFS server's
			// lock owner identity (GitHub username) differs from git config user.name.
			TArray<FString> UnlockArgs;
			UnlockArgs.Reserve(Batch.RelativeFiles.Num() + 1);
			UnlockArgs.Add(TEXT("unlock"));
			UnlockArgs.Append(Batch.RelativeFiles);

			const FString CwdOverride = Batch.bIsSubmodule ? Batch.RepoRoot : FString();
			const FGitLink_SubprocessResult Result = InCtx.Subprocess->RunLfs(UnlockArgs, CwdOverride);
			if (!Result.IsSuccess())
			{
				const FString ErrText = Result.Get_CombinedError();
				UE_LOG(LogGitLink, Warning,
					TEXT("Release_LfsLocksBestEffort: git lfs unlock (cwd='%s') returned: %s"),
					*Batch.RepoRoot, *ErrText);

				// Every file in the failed batch stays Locked in the caller's view. Stamping
				// NotLocked while the server lock persists is the precise bug this fixes.
				Outcome.FailedPaths.Append(Batch.AbsoluteFiles);
				Outcome.ErrorMessages.Add(FString::Printf(
					TEXT("git lfs unlock (cwd='%s') failed: %s"),
					*Batch.RepoRoot, *ErrText));
				continue;
			}

			for (const FString& Abs : Batch.AbsoluteFiles)
			{
				Outcome.Released.Add(Abs);

				// Suppress the LFS-replica-lag race: the dispatcher's immediate poll fires
				// Cmd_UpdateStatus right after the calling command returns, and a stale
				// /locks/verify response would otherwise re-promote our just-released file
				// to Lock=Locked, leaving the editor's lock indicator sticky until the next
				// full sweep ~120s later.
				InCtx.Provider.Note_LocalLockOp(Abs);
			}

			UE_LOG(LogGitLink, Log,
				TEXT("Release_LfsLocksBestEffort: unlocked %d file(s) in %s"),
				Batch.RelativeFiles.Num(),
				Batch.bIsSubmodule ? *FString::Printf(TEXT("submodule '%s'"), *Batch.RepoRoot) : TEXT("outer repo"));
		}

		return Outcome;
	}

	// ----------------------------------------------------------------------------------------------------------------
	// Status mapping helpers — used by both Cmd_UpdateStatus and Cmd_UpdateChangelistsStatus to
	// translate gitlink::FStatus snapshots into FGitLink_CompositeState entries.
	// ----------------------------------------------------------------------------------------------------------------

	enum class EStatusBucket : uint8
	{
		Staged,
		Unstaged,
		Conflicted,
	};

	inline auto Map_FileStatus(EFileStatus InStatus) -> EGitLink_FileState
	{
		switch (InStatus)
		{
			case EFileStatus::Added:      return EGitLink_FileState::Added;
			case EFileStatus::Deleted:    return EGitLink_FileState::Deleted;
			case EFileStatus::Modified:   return EGitLink_FileState::Modified;
			case EFileStatus::Renamed:    return EGitLink_FileState::Renamed;
			case EFileStatus::TypeChange: return EGitLink_FileState::Modified;
			case EFileStatus::Untracked:  return EGitLink_FileState::Added;
			case EFileStatus::Ignored:    return EGitLink_FileState::Unknown;
			case EFileStatus::Conflicted: return EGitLink_FileState::Unmerged;
			case EFileStatus::Unmodified: return EGitLink_FileState::Unknown;
			default:                      return EGitLink_FileState::Unknown;
		}
	}

	inline auto Map_TreeState(EStatusBucket InBucket, EFileStatus InStatus) -> EGitLink_TreeState
	{
		switch (InBucket)
		{
			case EStatusBucket::Staged:     return EGitLink_TreeState::Staged;
			case EStatusBucket::Conflicted: return EGitLink_TreeState::Working;
			case EStatusBucket::Unstaged:
				if (InStatus == EFileStatus::Untracked) { return EGitLink_TreeState::Untracked; }
				if (InStatus == EFileStatus::Ignored)   { return EGitLink_TreeState::Ignored;   }
				return EGitLink_TreeState::Working;
		}
		return EGitLink_TreeState::Unset;
	}

	// Merge a change from a given bucket into a composite state. Working-tree changes take
	// precedence over staged ones — a file staged then modified again shows as Modified.
	inline auto Merge_CompositeState(FGitLink_CompositeState& InOut, EStatusBucket InBucket, const FFileChange& InChange) -> void
	{
		const EGitLink_FileState File = Map_FileStatus(InChange.Status);
		const EGitLink_TreeState Tree = Map_TreeState(InBucket, InChange.Status);

		if (InOut.File == EGitLink_FileState::Unknown || InBucket != EStatusBucket::Staged)
		{
			InOut.File = File;
		}

		auto Priority = [](EGitLink_TreeState T) -> int32
		{
			switch (T)
			{
				case EGitLink_TreeState::Working:    return 5;
				case EGitLink_TreeState::Staged:     return 4;
				case EGitLink_TreeState::Untracked:  return 3;
				case EGitLink_TreeState::Ignored:    return 2;
				case EGitLink_TreeState::Unmodified: return 1;
				default:                             return 0;
			}
		};
		if (Priority(Tree) > Priority(InOut.Tree))
		{
			InOut.Tree = Tree;
		}
	}

	inline auto BuildAbsolutePath(const FString& InRepoRoot, const FString& InRelativePath) -> FString
	{
		FString Joined = FPaths::Combine(InRepoRoot, InRelativePath);
		FPaths::NormalizeFilename(Joined);
		return Joined;
	}

	// Process one bucket of file changes into a composite-by-path map.
	inline auto AppendBucket(
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
		}
	}
}
