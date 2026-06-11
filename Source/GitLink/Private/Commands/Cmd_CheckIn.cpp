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
		// (cross-batch isolation, subprocess-vs-libgit2 commit selection) so the
		// per-submodule path can stay simple.
		auto CommitOuterRepo(
			FCommandContext& InCtx,
			const FRepoBatch& InBatch,
			const FText& InDescription) -> FCommandResult
		{
			const TArray<FString>& StageRelPaths = InBatch.RelativeFiles;

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

			// Commit-scope isolation: `git commit` (via libgit2) always commits the ENTIRE
			// index, so anything staged that is NOT part of this batch must be temporarily
			// unstaged or it leaks into the user's commit. We derive "everything staged" from
			// a fresh index-vs-HEAD status rather than walking the cached changelists — the
			// changelist walk only knew about files the cache had assigned to changelists, so
			// explicit-file submits (right-click Check In, which carry no DestinationChangelist)
			// ran with NO isolation at all and silently committed files MarkForAdd'd or staged
			// earlier.
			TArray<FString> FilesToRestage;
			{
				TSet<FString> BatchSet;
				BatchSet.Reserve(StageRelPaths.Num());
				for (const FString& Rel : StageRelPaths)
				{ BatchSet.Add(Rel); }

				const FStatus IndexStatus = InCtx.Repository->Get_Status();
				for (const FFileChange& StagedChange : IndexStatus.Staged)
				{
					if (BatchSet.Contains(StagedChange.Path))
					{ continue; }
					FilesToRestage.Add(StagedChange.Path);
				}

				if (!FilesToRestage.IsEmpty())
				{
					InCtx.Repository->Unstage(FilesToRestage);
					UE_LOG(LogGitLink, Log,
						TEXT("Cmd_CheckIn: temporarily unstaged %d file(s) outside this submit"),
						FilesToRestage.Num());
				}
			}

			// Subprocess vs libgit2 commit.
			const bool bUseSubprocess =
				InCtx.Provider.NeedsSubprocessForCommit() &&
				InCtx.Subprocess != nullptr &&
				InCtx.Subprocess->IsValid();

			TArray<FString> CommitArgs;
			if (bUseSubprocess)
			{
				CommitArgs.Add(TEXT("commit"));
				CommitArgs.Add(TEXT("-m"));
				CommitArgs.Add(InDescription.ToString());
			}

			// Restore the temporarily-unstaged entries whether or not the commit succeeded.
			// A failed re-stage silently loses the user's staged set, so it's surfaced loudly.
			auto RestageOthers = [&]()
			{
				if (FilesToRestage.IsEmpty())
				{ return; }

				const FResult RestageRes = InCtx.Repository->Stage(FilesToRestage);
				if (!RestageRes)
				{
					UE_LOG(LogGitLink, Warning,
						TEXT("Cmd_CheckIn: failed to re-stage %d file(s) outside this submit: %s — ")
						TEXT("re-stage them manually (git add) or via MoveToChangelist"),
						FilesToRestage.Num(), *RestageRes.ErrorMessage);
					return;
				}
				UE_LOG(LogGitLink, Log,
					TEXT("Cmd_CheckIn: re-staged %d file(s) outside this submit"),
					FilesToRestage.Num());
			};

			const FGitLink_SubprocessResult SubResult = bUseSubprocess
				? InCtx.Subprocess->Run(CommitArgs)
				: FGitLink_SubprocessResult();
			if (bUseSubprocess)
			{
				if (!SubResult.IsSuccess())
				{
					RestageOthers();
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
					RestageOthers();
					return FCommandResult::Fail(FText::FromString(CommitRes.ErrorMessage));
				}
			}

			RestageOthers();

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

		if (!InFiles.IsEmpty())
		{
			AllAbsolute = InFiles;
		}
		else
		{
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
		//
		// Track per-batch outcome instead of returning on the first failure: commits are
		// per-repo and not transactional across repos. Returning early after the outer commit
		// succeeded left its files unstamped (cache showed them dirty), their LFS locks held
		// (the unlock pass below was never reached), and a retry would re-commit the
		// already-committed outer batch (libgit2 doesn't refuse an unchanged tree).
		TArray<FString> CommittedFiles;
		TSet<FString>   FailedFiles;
		TArray<FString> BatchErrors;
		CommittedFiles.Reserve(AllAbsolute.Num());

		for (const FRepoBatch& Batch : Batches)
		{
			if (Batch.RelativeFiles.IsEmpty())
			{ continue; }

			const FCommandResult RepoResult = Batch.bIsSubmodule
				? CommitSubmodule(InCtx, Batch, Description)
				: CommitOuterRepo(InCtx, Batch, Description);

			if (RepoResult.bOk)
			{
				CommittedFiles.Append(Batch.AbsoluteFiles);
			}
			else
			{
				FailedFiles.Append(Batch.AbsoluteFiles);
				for (const FText& Err : RepoResult.ErrorMessages)
				{ BatchErrors.Add(Err.ToString()); }
			}
		}

		if (CommittedFiles.IsEmpty())
		{
			return FCommandResult::Fail(FText::Format(
				LOCTEXT("CheckInAllFailed", "GitLink: check in failed: {0}"),
				FText::FromString(JoinTruncated(BatchErrors))));
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
			// Only files whose commit went through — releasing the lock on a file whose batch
			// failed would let someone else lock it while our uncommitted change still exists.
			UnlockOutcome = Release_LfsLocksBestEffort(InCtx, CommittedFiles);
		}
		else
		{
			// "Keep Checked Out" — every file should remain Locked. Treat all as released
			// from a stamping perspective; the per-file Lock=Locked decision below handles
			// the actual cache value.
			for (const FString& File : CommittedFiles)
			{ UnlockOutcome.Released.Add(File); }
		}
		const TSet<FString> FailedSet(UnlockOutcome.FailedPaths);

		// Build predictive Unmodified states for the committed files. Files in failed batches
		// keep their cached (dirty / locked) state so the editor still shows them as pending.
		FCommandResult Result = FCommandResult::Ok();
		Result.UpdatedStates.Reserve(CommittedFiles.Num());
		for (const FString& File : CommittedFiles)
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

		// Surface per-repo commit failures as a non-fatal error: some batches landed, the
		// listed ones did not (their files keep their pending state and held locks).
		if (!FailedFiles.IsEmpty())
		{
			Result.bOk = false;
			Result.ErrorMessages.Add(FText::Format(
				LOCTEXT("CheckInPartialFailure",
					"Committed {0} file(s), but {1} file(s) failed to commit and remain pending "
					"(their LFS locks are still held). Details: {2}"),
				FText::AsNumber(CommittedFiles.Num()),
				FText::AsNumber(FailedFiles.Num()),
				FText::FromString(JoinTruncated(BatchErrors))));
		}

		// Surface unlock failures as a non-fatal error on the result. The commit itself
		// succeeded, but the post-commit unlock did not. Without this, the failure went
		// to a Warning log only and the user never knew the lock was still held.
		if (!UnlockOutcome.FailedPaths.IsEmpty())
		{
			Result.bOk = false;

			Result.ErrorMessages.Add(FText::Format(
				LOCTEXT("CheckInUnlockFailed",
					"Commit succeeded, but failed to release the LFS lock on {0} file(s). "
					"You still hold these locks on the server. "
					"Common cause: the LFS server's lock owner identity differs from `git config user.name`. "
					"Details: {1}"),
				FText::AsNumber(UnlockOutcome.FailedPaths.Num()),
				FText::FromString(JoinTruncated(UnlockOutcome.ErrorMessages))));
		}

		const int32 RepoCount = Batches.Num();
		CheckInOp->SetSuccessMessage(FText::Format(
			LOCTEXT("CheckInSuccess", "Committed {0} file(s) across {1} repo(s) on branch '{2}'."),
			FText::AsNumber(CommittedFiles.Num()),
			FText::AsNumber(RepoCount),
			FText::FromString(InCtx.Provider.Get_BranchName())));

		Result.InfoMessages.Add(CheckInOp->GetSuccessMessage());
		return Result;
	}
}

#undef LOCTEXT_NAMESPACE
