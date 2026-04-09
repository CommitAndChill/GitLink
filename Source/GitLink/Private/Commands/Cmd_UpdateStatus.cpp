#include "Cmd_Shared.h"
#include "GitLink_CommandDispatcher.h"

#include "GitLink/GitLink_Provider.h"
#include "GitLink/GitLink_Revision.h"
#include "GitLinkLog.h"

#include "GitLinkCore/Repository/GitLink_Repository.h"
#include "GitLinkCore/Types/GitLink_Types.h"

#include <Misc/Paths.h>
#include <SourceControlOperations.h>

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
		enum class EStatusBucket : uint8
		{
			Staged,
			Unstaged,
			Conflicted,
		};

		auto Map_FileStatus(EFileStatus InStatus) -> EGitLink_FileState
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

		auto Map_TreeState(EStatusBucket InBucket, EFileStatus InStatus) -> EGitLink_TreeState
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

		// Merge a change from a given bucket into the composite state. Working-tree changes
		// take precedence over staged ones for the purposes of the priority order — a file
		// staged then modified again should display as Modified (not Added/Staged).
		auto Merge_CompositeState(FGitLink_CompositeState& InOut, EStatusBucket InBucket, const FFileChange& InChange) -> void
		{
			const EGitLink_FileState File = Map_FileStatus(InChange.Status);
			const EGitLink_TreeState Tree = Map_TreeState(InBucket, InChange.Status);

			// First hit always wins on File; subsequent Working-bucket hits can override a Staged first hit.
			if (InOut.File == EGitLink_FileState::Unknown || InBucket != EStatusBucket::Staged)
			{
				InOut.File = File;
			}

			// Tree priority: Working > Staged > Untracked > Ignored > Unset
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

		auto BuildAbsolutePath(const FString& InRepoRoot, const FString& InRelativePath) -> FString
		{
			FString Joined = FPaths::Combine(InRepoRoot, InRelativePath);
			FPaths::NormalizeFilename(Joined);
			return Joined;
		}

		// Process one bucket into OutCompositeByPath.
		auto AppendBucket(
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

		AppendBucket(Snapshot.Staged,     EStatusBucket::Staged,     InCtx.RepoRootAbsolute, CompositeByPath);
		AppendBucket(Snapshot.Unstaged,   EStatusBucket::Unstaged,   InCtx.RepoRootAbsolute, CompositeByPath);
		AppendBucket(Snapshot.Conflicted, EStatusBucket::Conflicted, InCtx.RepoRootAbsolute, CompositeByPath);

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
		auto BuildHistory = [&InCtx](const FString& InAbsoluteFilename) -> FGitLink_History
		{
			FGitLink_History History;

			FString RelativePath = InAbsoluteFilename;
			if (!InCtx.RepoRootAbsolute.IsEmpty() && RelativePath.StartsWith(InCtx.RepoRootAbsolute))
			{
				RelativePath.RightChopInline(InCtx.RepoRootAbsolute.Len(), EAllowShrinking::No);
			}
			// libgit2 wants forward slashes in tree paths regardless of platform.
			RelativePath.ReplaceInline(TEXT("\\"), TEXT("/"));
			RelativePath.RemoveFromStart(TEXT("/"));

			if (RelativePath.IsEmpty() || InCtx.Repository == nullptr)
			{ return History; }

			FLogQuery Query;
			Query.PathFilter = RelativePath;
			Query.MaxCount   = 100;

			const TArray<FCommit> Commits = InCtx.Repository->Get_Log(Query);
			History.Reserve(Commits.Num());

			int32 RevNumber = Commits.Num();
			for (const FCommit& Commit : Commits)
			{
				TSharedRef<FGitLink_Revision, ESPMode::ThreadSafe> Rev =
					MakeShared<FGitLink_Revision, ESPMode::ThreadSafe>();
				Rev->_Filename          = InAbsoluteFilename;
				Rev->_CommitId          = Commit.Hash;
				Rev->_ShortCommitId     = Commit.ShortHash;
				Rev->_Description       = Commit.Summary.IsEmpty() ? Commit.Message : Commit.Summary;
				Rev->_UserName          = Commit.Author.Name;
				Rev->_Action            = TEXT("Changed");
				Rev->_Date              = Commit.Author.When;
				Rev->_RevisionNumber    = RevNumber--;
				Rev->_CheckInIdentifier = Rev->_RevisionNumber;
				History.Add(Rev);
			}

			return History;
		};

		auto BuildState = [&Now, &bUpdateHistory, &BuildHistory](
			const FString& InFilename, const FGitLink_CompositeState& InComposite)
			-> FGitLink_FileStateRef
		{
			FGitLink_FileStateRef State = MakeShared<FGitLink_FileState, ESPMode::ThreadSafe>(InFilename);
			State->_State     = InComposite;
			State->_TimeStamp = Now;

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
