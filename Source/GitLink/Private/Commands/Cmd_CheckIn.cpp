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

		// Stage requested files. An empty file list = commit whatever's already in the index.
		if (!InFiles.IsEmpty())
		{
			const FResult StageRes = InCtx.Repository->Stage(InFiles);
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

		// Release any LFS locks we held on the committed files so they're free for the next
		// editor to check out. This is typical behaviour for LFS-locked workflows — the
		// edit-commit cycle ends with the lock being released.
		Release_LfsLocksBestEffort(InCtx, InFiles);

		// Build predictive Unmodified states for the committed files, stamping the Lock
		// component back to NotLocked (or Unlockable for non-lockable files).
		FCommandResult Result = FCommandResult::Ok();
		Result.UpdatedStates.Reserve(InFiles.Num());
		for (const FString& File : InFiles)
		{
			const bool bLockable = InCtx.Provider.Is_FileLockable(File);
			FGitLink_CompositeState Composite;
			Composite.File = EGitLink_FileState::Unknown;
			Composite.Tree = EGitLink_TreeState::Unmodified;
			Composite.Lock = bLockable ? EGitLink_LockState::NotLocked : EGitLink_LockState::Unlockable;
			Result.UpdatedStates.Add(Make_FileState(Normalize_AbsolutePath(File), Composite));
		}

		CheckInOp->SetSuccessMessage(FText::Format(
			LOCTEXT("CheckInSuccess", "Committed {0} file(s) on branch '{1}'."),
			FText::AsNumber(InFiles.Num()),
			FText::FromString(InCtx.Provider.Get_BranchName())));

		Result.InfoMessages.Add(CheckInOp->GetSuccessMessage());
		return Result;
	}
}

#undef LOCTEXT_NAMESPACE
