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
// --------------------------------------------------------------------------------------------------------------------

namespace gitlink::cmd
{
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

		// Stage files for commit. The editor passes absolute paths but libgit2's
		// git_index_add_all needs repo-relative pathspecs — convert before staging.
		//
		// When submitting from View Changes, the editor calls CheckIn with an EMPTY
		// file list (files are in the changelist, not InFiles). In that case we stage
		// all dirty files, matching `git commit -a` behaviour.
		if (!InFiles.IsEmpty())
		{
			TArray<FString> RelativePaths;
			RelativePaths.Reserve(InFiles.Num());
			for (const FString& File : InFiles)
			{
				RelativePaths.Add(ToRepoRelativePath(InCtx.RepoRootAbsolute, File));
			}

			UE_LOG(LogGitLink, Log,
				TEXT("Cmd_CheckIn: staging %d file(s) (first: '%s')"),
				RelativePaths.Num(),
				RelativePaths.Num() > 0 ? *RelativePaths[0] : TEXT("<none>"));

			const FResult StageRes = InCtx.Repository->Stage(RelativePaths);
			if (!StageRes)
			{
				return FCommandResult::Fail(FText::FromString(StageRes.ErrorMessage));
			}
		}
		else
		{
			// No explicit file list — View Changes submit path. Gather files from the
			// Working + Staged changelists in the cache and stage those specifically.
			// We can't use StageAll() because it tries to stage submodule directories
			// which libgit2 rejects with "invalid path".
			TArray<FString> ChangelistRelPaths;

			const TArray<FGitLink_ChangelistStateRef> AllCL = InCtx.StateCache.Enumerate_Changelists();
			for (const FGitLink_ChangelistStateRef& CL : AllCL)
			{
				for (const FSourceControlStateRef& FileState : CL->_Files)
				{
					ChangelistRelPaths.Add(
						ToRepoRelativePath(InCtx.RepoRootAbsolute, FileState->GetFilename()));
				}
			}

			if (ChangelistRelPaths.IsEmpty())
			{
				return FCommandResult::Fail(LOCTEXT("NothingToCommit",
					"GitLink: nothing to commit — no files in changelists."));
			}

			UE_LOG(LogGitLink, Log,
				TEXT("Cmd_CheckIn: staging %d changelist file(s) (first: '%s')"),
				ChangelistRelPaths.Num(),
				*ChangelistRelPaths[0]);

			const FResult StageRes = InCtx.Repository->Stage(ChangelistRelPaths);
			if (!StageRes)
			{
				return FCommandResult::Fail(FText::FromString(StageRes.ErrorMessage));
			}
		}

		// Two commit paths:
		//   A. Subprocess (git.exe): when the repo has pre-commit or commit-msg hooks, because
		//      libgit2 silently skips all hooks and the user expects them to run. This path
		//      shells out to `git commit -m "..."` which runs the hooks natively.
		//   B. In-process (libgit2): the fast default when no hooks are present.
		//
		// The setting bSubprocessFallbackForHooks gates path A entirely; if disabled, all
		// commits go through libgit2 regardless of hooks. That's the user's choice — they
		// accept that hooks won't run but get the speed benefit.
		const bool bUseSubprocess =
			InCtx.Provider.NeedsSubprocessForCommit() &&
			InCtx.Subprocess != nullptr &&
			InCtx.Subprocess->IsValid();

		if (bUseSubprocess)
		{
			// Subprocess commit. The stage step above used libgit2 (fast, in-process), and
			// now we shell out to git.exe for the commit itself so hooks run.
			TArray<FString> CommitArgs;
			CommitArgs.Add(TEXT("commit"));
			CommitArgs.Add(TEXT("-m"));
			CommitArgs.Add(Description.ToString());

			const FGitLink_SubprocessResult SubResult = InCtx.Subprocess->Run(CommitArgs);
			if (!SubResult.IsSuccess())
			{
				return FCommandResult::Fail(FText::Format(
					LOCTEXT("SubprocessCommitFailed", "GitLink: git commit (subprocess) failed: {0}"),
					FText::FromString(SubResult.Get_CombinedError())));
			}

			UE_LOG(LogGitLink, Log,
				TEXT("Cmd_CheckIn: committed via subprocess (hooks ran): %s"),
				*SubResult.StdOut.TrimStartAndEnd());
		}
		else
		{
			// In-process commit via libgit2 (fast, no hook execution).
			gitlink::FCommitParams Params;
			Params.Message = Description.ToString();

			const FResult CommitRes = InCtx.Repository->Commit(Params);
			if (!CommitRes)
			{
				return FCommandResult::Fail(FText::FromString(CommitRes.ErrorMessage));
			}
		}

		// Build the unified list of committed files. If InFiles was empty (View Changes
		// submit), we need to reconstruct it from the changelist states.
		TArray<FString> CommittedFiles = InFiles;
		if (CommittedFiles.IsEmpty())
		{
			const TArray<FGitLink_ChangelistStateRef> AllCL = InCtx.StateCache.Enumerate_Changelists();
			for (const FGitLink_ChangelistStateRef& CL : AllCL)
			{
				for (const FSourceControlStateRef& FileState : CL->_Files)
				{
					CommittedFiles.Add(FileState->GetFilename());
				}
			}
		}

		// Release LFS locks unless the user checked "Keep Files Checked Out".
		const bool bKeepCheckedOut = CheckInOp->GetKeepCheckedOut();
		if (!bKeepCheckedOut)
		{
			Release_LfsLocksBestEffort(InCtx, CommittedFiles);
		}

		// Build predictive Unmodified states for the committed files.
		FCommandResult Result = FCommandResult::Ok();
		Result.UpdatedStates.Reserve(CommittedFiles.Num());
		for (const FString& File : CommittedFiles)
		{
			const bool bLockable = InCtx.Provider.Is_FileLockable(File);
			FGitLink_CompositeState Composite;
			Composite.File = EGitLink_FileState::Unknown;
			Composite.Tree = EGitLink_TreeState::Unmodified;

			if (bKeepCheckedOut && bLockable)
			{
				Composite.Lock = EGitLink_LockState::Locked;
			}
			else
			{
				Composite.Lock = bLockable ? EGitLink_LockState::NotLocked : EGitLink_LockState::Unlockable;
			}

			Result.UpdatedStates.Add(Make_FileState(Normalize_AbsolutePath(File), Composite));
		}

		CheckInOp->SetSuccessMessage(FText::Format(
			LOCTEXT("CheckInSuccess", "Committed {0} file(s) on branch '{1}'."),
			FText::AsNumber(CommittedFiles.Num()),
			FText::FromString(InCtx.Provider.Get_BranchName())));

		Result.InfoMessages.Add(CheckInOp->GetSuccessMessage());
		return Result;
	}
}

#undef LOCTEXT_NAMESPACE
