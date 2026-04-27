#include "Cmd_Shared.h"
#include "GitLink_CommandDispatcher.h"
#include "GitLinkLog.h"

#include "GitLinkCore/Repository/GitLink_Repository.h"

#include <HAL/PlatformFileManager.h>

#define LOCTEXT_NAMESPACE "GitLinkCmdDelete"

// --------------------------------------------------------------------------------------------------------------------
// Cmd_Delete — "git rm <files>". The editor invokes this when the user deletes a source-
// controlled asset. By the time we're called, the editor has already removed the .uasset from
// disk, so we just need to stage the deletion via git_index_add_all (which picks up missing
// files as index removals).
//
// For safety we also remove any stragglers still on disk — happens when the editor only marks
// an asset for deletion but doesn't actually unlink.
//
// Submodule files: input batch is partitioned by submodule root. Files in the outer repo are
// staged against InCtx.Repository as before; files inside a submodule are staged against a
// freshly-opened FRepository for that submodule, so the deletion is recorded in the
// submodule's own index. The user is responsible for `git commit` inside the submodule —
// this command never commits.
// --------------------------------------------------------------------------------------------------------------------

namespace gitlink::cmd
{
	auto Delete(
		FCommandContext&                  InCtx,
		const FSourceControlOperationRef& /*InOperation*/,
		const TArray<FString>&            InFiles) -> FCommandResult
	{
		if (InCtx.Repository == nullptr || !InCtx.Repository->IsOpen())
		{
			return FCommandResult::Fail(LOCTEXT("NoRepo",
				"GitLink: cannot delete — repository not open."));
		}

		if (InFiles.IsEmpty())
		{
			return FCommandResult::Ok();
		}

		UE_LOG(LogGitLink, Log, TEXT("Cmd_Delete: deleting %d file(s)"), InFiles.Num());

		// Remove files that are still on disk. Tolerate missing files (editor may have already
		// unlinked them) — the stage step below will pick up the deletions either way.
		IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
		for (const FString& File : InFiles)
		{
			if (PlatformFile.FileExists(*File))
			{
				PlatformFile.DeleteFile(*File);
			}
		}

		// Partition into outer-repo files and per-submodule buckets keyed by submodule root.
		TArray<FString> OuterFiles;
		TMap<FString, TArray<FString>> FilesBySubmoduleRoot;
		OuterFiles.Reserve(InFiles.Num());

		for (const FString& File : InFiles)
		{
			const FString SubmoduleRoot = InCtx.Provider.Get_SubmoduleRoot(File);
			if (SubmoduleRoot.IsEmpty())
			{ OuterFiles.Add(File); }
			else
			{ FilesBySubmoduleRoot.FindOrAdd(SubmoduleRoot).Add(File); }
		}

		// Outer-repo bucket — existing path.
		if (!OuterFiles.IsEmpty())
		{
			const FResult StageRes = InCtx.Repository->Stage(OuterFiles);
			if (!StageRes)
			{
				UE_LOG(LogGitLink, Warning,
					TEXT("Cmd_Delete: outer Stage failed: %s"), *StageRes.ErrorMessage);
				return FCommandResult::Fail(FText::FromString(StageRes.ErrorMessage));
			}
		}

		// Submodule buckets — open each submodule's repo and stage repo-relative paths there.
		for (const TPair<FString, TArray<FString>>& Pair : FilesBySubmoduleRoot)
		{
			const FString& SubmoduleRoot = Pair.Key;
			const TArray<FString>& SubFiles = Pair.Value;

			TUniquePtr<gitlink::FRepository> SubRepo =
				InCtx.Provider.Open_SubmoduleRepositoryFor(SubFiles[0]);
			if (!SubRepo.IsValid())
			{
				UE_LOG(LogGitLink, Warning,
					TEXT("Cmd_Delete: could not open submodule repo at '%s' — skipping %d file(s)"),
					*SubmoduleRoot, SubFiles.Num());
				continue;
			}

			TArray<FString> SubRelative;
			SubRelative.Reserve(SubFiles.Num());
			for (const FString& Abs : SubFiles)
			{
				SubRelative.Add(ToRepoRelativePath(SubmoduleRoot, Abs));
			}

			const FResult SubStageRes = SubRepo->Stage(SubRelative);
			if (!SubStageRes)
			{
				UE_LOG(LogGitLink, Warning,
					TEXT("Cmd_Delete: submodule '%s' Stage failed: %s"),
					*SubmoduleRoot, *SubStageRes.ErrorMessage);
				return FCommandResult::Fail(FText::FromString(SubStageRes.ErrorMessage));
			}
		}

		FCommandResult Result = FCommandResult::Ok();
		Result.UpdatedStates.Reserve(InFiles.Num());
		for (const FString& File : InFiles)
		{
			FGitLink_FileStateRef NewState = Make_FileState(
				Normalize_AbsolutePath(File),
				EGitLink_FileState::Deleted,
				EGitLink_TreeState::Staged);

			// Carry the bInSubmodule flag forward so command code that operates on the next
			// state (e.g. PartitionByRepo on subsequent ops) routes correctly.
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
