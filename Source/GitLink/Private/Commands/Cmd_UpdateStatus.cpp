#include "Cmd_Shared.h"
#include "GitLink_CommandDispatcher.h"

#include "GitLink/GitLink_Provider.h"
#include "GitLink/GitLink_Revision.h"
#include "GitLinkLog.h"

#include "GitLinkCore/Repository/GitLink_Repository.h"
#include "GitLinkCore/Types/GitLink_Types.h"

#include <Misc/Paths.h>
#include <SourceControlOperations.h>

// Forward declare the blob size op so we can populate FGitLink_Revision::_FileSize in history.
namespace gitlink::op
{
	auto Get_BlobSizeAtCommit(gitlink::FRepository& InRepo, const FString& InCommitHash, const FString& InRepoRelativePath) -> int64;
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
			// Full snapshot mode — emit a state for every dirty file only.
			for (const TPair<FString, FGitLink_CompositeState>& Pair : CompositeByPath)
			{
				Result.UpdatedStates.Add(BuildState(Pair.Key, Pair.Value));
			}

			// Refresh LFS lock state from the remote so we detect files locked by other users.
			// Only runs in full-scan mode (empty InFiles), which is what the background poll
			// dispatches every 30 seconds. File-specific status queries (e.g. editor opening a
			// folder) skip this to avoid a network call on every Content Browser navigation.
			if (InCtx.Subprocess != nullptr && InCtx.Subprocess->IsValid() && InCtx.Provider.Is_LfsAvailable())
			{
				const TMap<FString, FString> LocalLocks  = InCtx.Subprocess->QueryLfsLocks_Local();
				const TMap<FString, FString> RemoteLocks = InCtx.Subprocess->QueryLfsLocks_Remote();

				int32 LockChanges = 0;

				for (const auto& [RelPath, LockOwner] : RemoteLocks)
				{
					const FString Absolute = Normalize_AbsolutePath(
						FPaths::Combine(InCtx.RepoRootAbsolute, RelPath));

					const bool bIsOurs = LocalLocks.Contains(RelPath);
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

							Result.UpdatedStates.Add(Make_FileState(Absolute, Composite));
							++LockChanges;
						}
					}
				}

				// Also check for locks that were RELEASED since last poll — files that were
				// LockedOther (or Locked) in the cache but are no longer in the remote lock set.
				// Build a set of currently-locked absolute paths for fast lookup.
				TSet<FString> RemoteLockAbsolutes;
				RemoteLockAbsolutes.Reserve(RemoteLocks.Num());
				for (const auto& [RelPath, _] : RemoteLocks)
				{
					RemoteLockAbsolutes.Add(Normalize_AbsolutePath(
						FPaths::Combine(InCtx.RepoRootAbsolute, RelPath)));
				}

				const TArray<FGitLink_FileStateRef> LockedStates = InCtx.StateCache.Enumerate_FileStates(
					[](const FGitLink_FileStateRef& InState)
					{
						return InState->_State.Lock == EGitLink_LockState::Locked
							|| InState->_State.Lock == EGitLink_LockState::LockedOther;
					});
				for (const FGitLink_FileStateRef& Cached : LockedStates)
				{

					if (RemoteLockAbsolutes.Contains(Cached->GetFilename()))
					{ continue; }

					// Lock was released — update to NotLocked (or Unlockable if not lockable).
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
						TEXT("Cmd_UpdateStatus: remote lock refresh — %d lock state change(s)"),
						LockChanges);
				}
			}
		}
		else
		{
			// Explicit file list — every requested file gets an emitted state, even if it's
			// clean, so the editor knows to clear stale modification markers.
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
					// Not in the dirty set -> unmodified + tracked. Technically this could be
					// "untracked outside repo" too, but that determination needs a separate
					// pathspec test; for v1 we optimistically call it Unmodified.
					FGitLink_CompositeState Clean;
					Clean.File = EGitLink_FileState::Unknown;
					Clean.Tree = EGitLink_TreeState::Unmodified;
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
