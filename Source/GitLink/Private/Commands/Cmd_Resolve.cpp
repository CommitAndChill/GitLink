#include "Cmd_Shared.h"
#include "GitLink_CommandDispatcher.h"
#include "GitLinkLog.h"

#include "GitLinkCore/Repository/GitLink_Repository.h"

#define LOCTEXT_NAMESPACE "GitLinkCmdResolve"

// --------------------------------------------------------------------------------------------------------------------
// Cmd_Resolve — "mark conflicted files as resolved". Semantically this is `git add <file>`
// after the user has manually edited the conflict markers or picked a side. Staging a
// conflicted file via git_index_add_all clears the conflict entries and inserts the working
// tree content as the new resolved version.
//
// This delegates to the same Stage op that Cmd_MarkForAdd uses. The difference is purely in
// the predictive state emitted back to the editor.
// --------------------------------------------------------------------------------------------------------------------

namespace gitlink::cmd
{
	auto Resolve(
		FCommandContext&                  InCtx,
		const FSourceControlOperationRef& /*InOperation*/,
		const TArray<FString>&            InFiles) -> FCommandResult
	{
		if (InCtx.Repository == nullptr || !InCtx.Repository->IsOpen())
		{
			return FCommandResult::Fail(LOCTEXT("NoRepo",
				"GitLink: cannot resolve — repository not open."));
		}

		if (InFiles.IsEmpty())
		{
			return FCommandResult::Ok();
		}

		UE_LOG(LogGitLink, Log, TEXT("Cmd_Resolve: resolving %d file(s)"), InFiles.Num());

		// Partition by repo and stage REPO-RELATIVE paths against the owning repository.
		// The previous code passed the editor's absolute paths straight into the outer repo's
		// Stage() — git_index_add_all matches pathspecs repo-relative, so the resolve silently
		// staged nothing (Pitfall #2), and conflicted files inside submodules were staged
		// against the wrong repo entirely.
		FCommandResult Result = FCommandResult::Ok();
		TSet<FString> FailedFiles;
		TArray<FString> BatchErrors;

		const TArray<FRepoBatch> Batches = PartitionByRepo(InCtx, InFiles);
		for (const FRepoBatch& Batch : Batches)
		{
			TUniquePtr<gitlink::FRepository> SubRepo;
			gitlink::FRepository* TargetRepo = InCtx.Repository.Get();
			if (Batch.bIsSubmodule)
			{
				SubRepo = InCtx.Provider.Open_SubmoduleRepositoryFor(Batch.AbsoluteFiles[0]);
				if (!SubRepo.IsValid())
				{
					FailedFiles.Append(Batch.AbsoluteFiles);
					BatchErrors.Add(FString::Printf(
						TEXT("could not open submodule repo at '%s'"), *Batch.RepoRoot));
					continue;
				}
				TargetRepo = SubRepo.Get();
			}

			const FResult StageRes = TargetRepo->Stage(Batch.RelativeFiles);
			if (!StageRes)
			{
				UE_LOG(LogGitLink, Warning,
					TEXT("Cmd_Resolve: Stage in '%s' failed: %s"),
					*Batch.RepoRoot, *StageRes.ErrorMessage);
				FailedFiles.Append(Batch.AbsoluteFiles);
				BatchErrors.Add(FString::Printf(
					TEXT("staging in '%s' failed: %s"), *Batch.RepoRoot, *StageRes.ErrorMessage));
			}
		}

		if (!BatchErrors.IsEmpty())
		{
			Result.bOk = false;
			Result.ErrorMessages.Add(FText::Format(
				LOCTEXT("ResolveFailed", "Failed to mark {0} file(s) resolved: {1}"),
				FText::AsNumber(FailedFiles.Num()),
				FText::FromString(JoinTruncated(BatchErrors))));
		}

		// After resolving, the file is staged as Modified (or Added if it was new). We can't
		// tell which without re-querying status, so we report Modified/Staged — the editor
		// will call UpdateStatus afterward to refresh if it cares about the distinction.
		// Files whose staging failed keep their cached (Conflicted) state.
		Result.UpdatedStates.Reserve(InFiles.Num());
		for (const FString& File : InFiles)
		{
			if (FailedFiles.Contains(File))
			{ continue; }

			Result.UpdatedStates.Add(Make_FileState(
				Normalize_AbsolutePath(File),
				EGitLink_FileState::Modified,
				EGitLink_TreeState::Staged));
		}
		return Result;
	}
}

#undef LOCTEXT_NAMESPACE
