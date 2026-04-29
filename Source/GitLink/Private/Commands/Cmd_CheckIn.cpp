#include "Cmd_Shared.h"
#include "GitLink_CommandDispatcher.h"
#include "GitLink/GitLink_Provider.h"

#include "GitLinkCore/Repository/GitLink_Repository.h"
#include "GitLinkCore/Repository/GitLink_Repository_Params.h"

#include <SourceControlOperations.h>

#define LOCTEXT_NAMESPACE "GitLinkCmdCheckIn"

// --------------------------------------------------------------------------------------------------------------------
// Cmd_CheckIn — "git add <files> && git commit -m '<description>'". The description comes from
// the FCheckIn operation; signature is derived from libgit2's git_signature_default which reads
// user.name / user.email from git config.
//
// The editor passes the full set of files to commit. We stage them (even if they were already
// staged by an earlier MarkForAdd — git_index_add_all is idempotent) and then build a commit.
//
// Submodule files: input batch is partitioned by submodule root. Each repo gets its own
// commit (with the same description). Outer-repo commits use the existing path including
// subprocess-fallback for hooks and cross-changelist isolation; submodule commits use
// libgit2 in-process always (we don't probe submodule hooks, and changelists are
// outer-repo-only).
// --------------------------------------------------------------------------------------------------------------------

namespace gitlink::cmd
{
	namespace
	{
		// Commits the given outer-repo bucket. Encapsulates the existing CheckIn logic
		// (cross-changelist isolation, subprocess-vs-libgit2 commit selection) so the new
		// per-submodule path can stay simple.
		auto CommitOuterRepo(
			FCommandContext& InCtx,
			const FRepoBatch& InBatch,
			const TArray<FString>& InGatheredRelPathsIfFromChangelist,  // empty if from explicit InFiles
			const FText& InDescription) -> FCommandResult
		{
			// Stage the bucket. If InGathered is non-empty we're in View-Changes mode and have
			// already gathered the relative paths from the changelist; otherwise stage the
			// explicit batch.
			const TArray<FString>& StageRelPaths = InGatheredRelPathsIfFromChangelist.IsEmpty()
				? InBatch.RelativeFiles
				: InGatheredRelPathsIfFromChangelist;

			if (StageRelPaths.IsEmpty())
			{
				return FCommandResult::Fail(LOCTEXT("NothingToCommit",
					"GitLink: nothing to commit — no files in changelists."));
			}

			UE_LOG(LogGitLink, Log,
				TEXT("Cmd_CheckIn: staging %d outer-repo file(s) (first: '%s')"),
				StageRelPaths.Num(), *StageRelPaths[0]);

			const FResult StageRes = InCtx.Repository->Stage(StageRelPaths);
			if (!StageRes)
			{
				return FCommandResult::Fail(FText::FromString(StageRes.ErrorMessage));
			}

			// Cross-changelist isolation: temporarily unstage files from OTHER changelists so
			// they don't leak into this commit. Only meaningful for the outer repo because
			// changelists are tracked against the outer repo's index.
			TArray<FString> FilesToRestage;
			if (InCtx.DestinationChangelist.IsValid())
			{
				const FGitLink_Changelist* SourceCL =
					static_cast<const FGitLink_Changelist*>(InCtx.DestinationChangelist.Get());
				const FString SourceName = SourceCL->Get_Name();

				const TArray<FGitLink_ChangelistStateRef> AllCL = InCtx.StateCache.Enumerate_Changelists();
				for (const FGitLink_ChangelistStateRef& CL : AllCL)
				{
					if (CL->_Changelist.Get_Name() == SourceName || CL->_Files.Num() == 0)
					{ continue; }

					TArray<FString> OtherRelPaths;
					for (const FSourceControlStateRef& FileState : CL->_Files)
					{
						// Skip submodule files — they live in submodule indexes, not the outer one.
						if (InCtx.Provider.Is_InSubmodule(FileState->GetFilename()))
						{ continue; }

						const FString Rel = ToRepoRelativePath(InCtx.RepoRootAbsolute, FileState->GetFilename());
						OtherRelPaths.Add(Rel);
						FilesToRestage.Add(Rel);
					}

					if (!OtherRelPaths.IsEmpty())
					{
						InCtx.Repository->Unstage(OtherRelPaths);
						UE_LOG(LogGitLink, Log,
							TEXT("Cmd_CheckIn: temporarily unstaged %d file(s) from '%s' changelist"),
							OtherRelPaths.Num(), *CL->_Changelist.Get_Name());
					}
				}
			}

			// Subprocess vs libgit2 commit.
			const bool bUseSubprocess =
				InCtx.Provider.NeedsSubprocessForCommit() &&
				InCtx.Subprocess != nullptr &&
				InCtx.Subprocess->IsValid();

			if (bUseSubprocess)
			{
				TArray<FString> CommitArgs;
				CommitArgs.Add(TEXT("commit"));
				CommitArgs.Add(TEXT("-m"));
				CommitArgs.Add(InDescription.ToString());

				const FGitLink_SubprocessResult SubResult = InCtx.Subprocess->Run(CommitArgs);
				if (!SubResult.IsSuccess())
				{
					if (!FilesToRestage.IsEmpty())
					{ InCtx.Repository->Stage(FilesToRestage); }
					return FCommandResult::Fail(FText::Format(
						LOCTEXT("SubprocessCommitFailed", "GitLink: git commit (subprocess) failed: {0}"),
						FText::FromString(SubResult.Get_CombinedError())));
				}

				UE_LOG(LogGitLink, Log,
					TEXT("Cmd_CheckIn: outer commit via subprocess (hooks ran): %s"),
					*SubResult.StdOut.TrimStartAndEnd());
			}
			else
			{
				gitlink::FCommitParams Params;
				Params.Message = InDescription.ToString();

				const FResult CommitRes = InCtx.Repository->Commit(Params);
				if (!CommitRes)
				{
					if (!FilesToRestage.IsEmpty())
					{ InCtx.Repository->Stage(FilesToRestage); }
					return FCommandResult::Fail(FText::FromString(CommitRes.ErrorMessage));
				}
			}

			if (!FilesToRestage.IsEmpty())
			{
				InCtx.Repository->Stage(FilesToRestage);
				UE_LOG(LogGitLink, Log,
					TEXT("Cmd_CheckIn: re-staged %d file(s) from other changelists"),
					FilesToRestage.Num());
			}

			return FCommandResult::Ok();
		}

		// Commits the given submodule bucket. Always libgit2 in-process — no hook probing,
		// no changelist isolation (changelists are outer-repo-only).
		auto CommitSubmodule(
			FCommandContext& InCtx,
			const FRepoBatch& InBatch,
			const FText& InDescription) -> FCommandResult
		{
			TUniquePtr<gitlink::FRepository> SubRepo =
				InCtx.Provider.Open_SubmoduleRepositoryFor(InBatch.AbsoluteFiles[0]);
			if (!SubRepo.IsValid())
			{
				return FCommandResult::Fail(FText::Format(
					LOCTEXT("SubOpenFailed", "GitLink: could not open submodule repo at '{0}'"),
					FText::FromString(InBatch.RepoRoot)));
			}

			UE_LOG(LogGitLink, Log,
				TEXT("Cmd_CheckIn: staging %d file(s) in submodule '%s'"),
				InBatch.RelativeFiles.Num(), *InBatch.RepoRoot);

			const FResult StageRes = SubRepo->Stage(InBatch.RelativeFiles);
			if (!StageRes)
			{
				return FCommandResult::Fail(FText::FromString(StageRes.ErrorMessage));
			}

			gitlink::FCommitParams Params;
			Params.Message = InDescription.ToString();

			const FResult CommitRes = SubRepo->Commit(Params);
			if (!CommitRes)
			{
				return FCommandResult::Fail(FText::Format(
					LOCTEXT("SubCommitFailed", "GitLink: submodule '{0}' commit failed: {1}"),
					FText::FromString(InBatch.RepoRoot),
					FText::FromString(CommitRes.ErrorMessage)));
			}

			UE_LOG(LogGitLink, Log,
				TEXT("Cmd_CheckIn: committed %d file(s) in submodule '%s'"),
				InBatch.RelativeFiles.Num(), *InBatch.RepoRoot);

			return FCommandResult::Ok();
		}
	}

	auto CheckIn(
		FCommandContext&                  InCtx,
		const FSourceControlOperationRef& InOperation,
		const TArray<FString>&            InFiles) -> FCommandResult
	{
		if (InCtx.Repository == nullptr || !InCtx.Repository->IsOpen())
		{
			return FCommandResult::Fail(LOCTEXT("NoRepo",
				"GitLink: cannot check in — repository not open."));
		}

		TSharedRef<FCheckIn> CheckInOp = StaticCastSharedRef<FCheckIn>(InOperation);
		const FText Description = CheckInOp->GetDescription();
		if (Description.IsEmpty())
		{
			return FCommandResult::Fail(LOCTEXT("EmptyMessage",
				"GitLink: cannot check in — commit description is empty."));
		}

		// Build the unified absolute-path list. If InFiles is empty (View Changes submit),
		// gather from the destination changelist (or all changelists as fallback).
		TArray<FString> AllAbsolute;
		bool bFromChangelist = false;

		if (!InFiles.IsEmpty())
		{
			AllAbsolute = InFiles;
		}
		else
		{
			bFromChangelist = true;
			const TArray<FGitLink_ChangelistStateRef> AllCL = InCtx.StateCache.Enumerate_Changelists();

			if (InCtx.DestinationChangelist.IsValid())
			{
				const FGitLink_Changelist* SourceCL =
					static_cast<const FGitLink_Changelist*>(InCtx.DestinationChangelist.Get());
				const FString SourceName = SourceCL->Get_Name();

				for (const FGitLink_ChangelistStateRef& CL : AllCL)
				{
					if (CL->_Changelist.Get_Name() != SourceName)
					{ continue; }
					for (const FSourceControlStateRef& FileState : CL->_Files)
					{ AllAbsolute.Add(FileState->GetFilename()); }
					break;
				}
			}
			else
			{
				for (const FGitLink_ChangelistStateRef& CL : AllCL)
				{
					for (const FSourceControlStateRef& FileState : CL->_Files)
					{ AllAbsolute.Add(FileState->GetFilename()); }
				}
			}

			if (AllAbsolute.IsEmpty())
			{
				return FCommandResult::Fail(LOCTEXT("NothingToCommit",
					"GitLink: nothing to commit — no files in changelists."));
			}
		}

		const TArray<FRepoBatch> Batches = PartitionByRepo(InCtx, AllAbsolute);

		// Commit each bucket. Outer first (so the outer commit's hook side-effects are
		// visible to subsequent submodule commits if any), then each submodule.
		for (const FRepoBatch& Batch : Batches)
		{
			if (Batch.RelativeFiles.IsEmpty())
			{ continue; }

			const FCommandResult RepoResult = Batch.bIsSubmodule
				? CommitSubmodule(InCtx, Batch, Description)
				: CommitOuterRepo(
					InCtx, Batch,
					bFromChangelist ? Batch.RelativeFiles : TArray<FString>(),
					Description);

			if (!RepoResult.bOk)
			{
				return RepoResult;
			}
		}

		// Release LFS locks (per-repo via Release_LfsLocksBestEffort) unless the user
		// checked "Keep Files Checked Out". The outcome lets us avoid stamping NotLocked
		// on files whose unlock failed — same rationale as Cmd_Revert: a silent failure
		// would flip the cache to NotLocked while the server lock persists, leaving the
		// file permanently stuck.
		const bool bKeepCheckedOut = CheckInOp->GetKeepCheckedOut();
		FLfsUnlockOutcome UnlockOutcome;
		if (!bKeepCheckedOut)
		{
			UnlockOutcome = Release_LfsLocksBestEffort(InCtx, AllAbsolute);
		}
		else
		{
			// "Keep Checked Out" — every file should remain Locked. Treat all as released
			// from a stamping perspective; the per-file Lock=Locked decision below handles
			// the actual cache value.
			for (const FString& File : AllAbsolute)
			{ UnlockOutcome.Released.Add(File); }
		}
		const TSet<FString> FailedSet(UnlockOutcome.FailedPaths);

		// Build predictive Unmodified states for the committed files.
		FCommandResult Result = FCommandResult::Ok();
		Result.UpdatedStates.Reserve(AllAbsolute.Num());
		for (const FString& File : AllAbsolute)
		{
			const bool bLockable     = InCtx.Provider.Is_FileLockable(File);
			const bool bSubmodule    = InCtx.Provider.Is_InSubmodule(File);
			const bool bUnlockFailed = FailedSet.Contains(File);

			FGitLink_CompositeState Composite;
			Composite.File          = EGitLink_FileState::Unknown;
			Composite.Tree          = EGitLink_TreeState::Unmodified;
			Composite.bInSubmodule  = bSubmodule;

			if (bUnlockFailed)
			{
				// Server still holds the lock — preserve the cached Lock state so the
				// editor keeps the file visibly checked out and the user can retry.
				const FGitLink_FileStateRef Existing = InCtx.StateCache.Find_FileState(File);
				Composite.Lock     = Existing->_State.Lock;
				Composite.LockUser = Existing->_State.LockUser;
			}
			else if (bKeepCheckedOut && bLockable)
			{
				Composite.Lock = EGitLink_LockState::Locked;
			}
			else
			{
				Composite.Lock = bLockable ? EGitLink_LockState::NotLocked : EGitLink_LockState::Unlockable;
			}

			Result.UpdatedStates.Add(Make_FileState(Normalize_AbsolutePath(File), Composite));
		}

		// Surface unlock failures as a non-fatal error on the result. The commit itself
		// succeeded, but the post-commit unlock did not. Without this, the failure went
		// to a Warning log only and the user never knew the lock was still held.
		if (!UnlockOutcome.FailedPaths.IsEmpty())
		{
			Result.bOk = false;

			FString Joined;
			for (int32 Idx = 0; Idx < UnlockOutcome.ErrorMessages.Num(); ++Idx)
			{
				if (Idx > 0) { Joined += TEXT("\n"); }
				Joined += UnlockOutcome.ErrorMessages[Idx];
			}
			constexpr int32 MaxLen = 1024;
			if (Joined.Len() > MaxLen)
			{ Joined = Joined.Left(MaxLen) + TEXT(" ..."); }

			Result.ErrorMessages.Add(FText::Format(
				LOCTEXT("CheckInUnlockFailed",
					"Commit succeeded, but failed to release the LFS lock on {0} file(s). "
					"You still hold these locks on the server. "
					"Common cause: the LFS server's lock owner identity differs from `git config user.name`. "
					"Details: {1}"),
				FText::AsNumber(UnlockOutcome.FailedPaths.Num()),
				FText::FromString(Joined)));
		}

		const int32 RepoCount = Batches.Num();
		CheckInOp->SetSuccessMessage(FText::Format(
			LOCTEXT("CheckInSuccess", "Committed {0} file(s) across {1} repo(s) on branch '{2}'."),
			FText::AsNumber(AllAbsolute.Num()),
			FText::AsNumber(RepoCount),
			FText::FromString(InCtx.Provider.Get_BranchName())));

		Result.InfoMessages.Add(CheckInOp->GetSuccessMessage());
		return Result;
	}
}

#undef LOCTEXT_NAMESPACE
