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

		for (const FRepoBatch& Batch : Batches)
		{
			if (Batch.RelativeFiles.IsEmpty())
			{ continue; }

			gitlink::IRepository* TargetRepo = InCtx.Repository;
			TUniquePtr<gitlink::FRepository> SubRepoOwned;

			if (Batch.bIsSubmodule)
			{
				SubRepoOwned = InCtx.Provider.Open_SubmoduleRepositoryFor(Batch.AbsoluteFiles[0]);
				if (!SubRepoOwned.IsValid())
				{
					UE_LOG(LogGitLink, Warning,
						TEXT("Cmd_MarkForAdd: could not open submodule repo at '%s' — skipping %d file(s)"),
						*Batch.RepoRoot, Batch.AbsoluteFiles.Num());
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
				return FCommandResult::Fail(FText::FromString(StageRes.ErrorMessage));
			}
		}

		FCommandResult Result = FCommandResult::Ok();
		Result.UpdatedStates.Reserve(InFiles.Num());
		for (const FString& File : InFiles)
		{
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
