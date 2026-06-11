#include "Cmd_Shared.h"
#include "GitLink_CommandDispatcher.h"
#include "GitLinkLog.h"

#include "GitLinkCore/Repository/GitLink_Repository.h"

#define LOCTEXT_NAMESPACE "GitLinkCmdMarkForAdd"

// --------------------------------------------------------------------------------------------------------------------
// Cmd_MarkForAdd — "git add <files>". The editor calls this for untracked files the user wants
// to include in the next commit.
//
// Submodule files: input batch is partitioned by submodule root. Files in the outer repo are
// staged against InCtx.Repository as before; files inside a submodule are staged against a
// freshly-opened FRepository for that submodule.
// --------------------------------------------------------------------------------------------------------------------

namespace gitlink::cmd
{
	auto MarkForAdd(
		FCommandContext&                  InCtx,
		const FSourceControlOperationRef& /*InOperation*/,
		const TArray<FString>&            InFiles) -> FCommandResult
	{
		if (InCtx.Repository == nullptr || !InCtx.Repository->IsOpen())
		{
			return FCommandResult::Fail(LOCTEXT("NoRepo",
				"GitLink: cannot mark for add — repository not open."));
		}

		if (InFiles.IsEmpty())
		{
			return FCommandResult::Ok();
		}

		UE_LOG(LogGitLink, Log, TEXT("Cmd_MarkForAdd: staging %d file(s)"), InFiles.Num());

		const TArray<FRepoBatch> Batches = PartitionByRepo(InCtx, InFiles);

		// Files whose batch failed (submodule wouldn't open, staging error). Excluded from the
		// predictive Added/Staged stamping — stamping them anyway made the cache claim a stage
		// that never happened — and surfaced as a non-fatal error so the user knows.
		TSet<FString> FailedFiles;
		TArray<FString> BatchErrors;

		for (const FRepoBatch& Batch : Batches)
		{
			if (Batch.RelativeFiles.IsEmpty())
			{ continue; }

			gitlink::IRepository* TargetRepo = InCtx.Repository.Get();
			TUniquePtr<gitlink::FRepository> SubRepoOwned;

			if (Batch.bIsSubmodule)
			{
				SubRepoOwned = InCtx.Provider.Open_SubmoduleRepositoryFor(Batch.AbsoluteFiles[0]);
				if (!SubRepoOwned.IsValid())
				{
					UE_LOG(LogGitLink, Warning,
						TEXT("Cmd_MarkForAdd: could not open submodule repo at '%s' — skipping %d file(s)"),
						*Batch.RepoRoot, Batch.AbsoluteFiles.Num());
					FailedFiles.Append(Batch.AbsoluteFiles);
					BatchErrors.Add(FString::Printf(
						TEXT("could not open submodule repo at '%s'"), *Batch.RepoRoot));
					continue;
				}
				TargetRepo = SubRepoOwned.Get();
			}

			const FResult StageRes = TargetRepo->Stage(Batch.RelativeFiles);
			if (!StageRes)
			{
				UE_LOG(LogGitLink, Warning,
					TEXT("Cmd_MarkForAdd: Stage failed (cwd='%s'): %s"),
					*Batch.RepoRoot, *StageRes.ErrorMessage);
				FailedFiles.Append(Batch.AbsoluteFiles);
				BatchErrors.Add(FString::Printf(
					TEXT("staging in '%s' failed: %s"), *Batch.RepoRoot, *StageRes.ErrorMessage));
			}
		}

		FCommandResult Result = FCommandResult::Ok();

		if (!BatchErrors.IsEmpty())
		{
			Result.bOk = false;
			Result.ErrorMessages.Add(FText::Format(
				LOCTEXT("MarkForAddFailed", "Failed to mark {0} file(s) for add: {1}"),
				FText::AsNumber(FailedFiles.Num()),
				FText::FromString(JoinTruncated(BatchErrors))));
		}

		Result.UpdatedStates.Reserve(InFiles.Num());
		for (const FString& File : InFiles)
		{
			if (FailedFiles.Contains(File))
			{ continue; }

			FGitLink_FileStateRef NewState = Make_FileState(
				Normalize_AbsolutePath(File),
				EGitLink_FileState::Added,
				EGitLink_TreeState::Staged);

			if (InCtx.Provider.Is_InSubmodule(File))
			{
				NewState->_State.bInSubmodule = true;
			}

			Result.UpdatedStates.Add(NewState);
		}
		return Result;
	}
}

#undef LOCTEXT_NAMESPACE
