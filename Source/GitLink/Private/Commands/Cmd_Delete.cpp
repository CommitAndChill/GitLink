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

		const FResult StageRes = InCtx.Repository->Stage(InFiles);
		if (!StageRes)
		{
			UE_LOG(LogGitLink, Warning,
				TEXT("Cmd_Delete: Stage failed: %s"), *StageRes.ErrorMessage);
			return FCommandResult::Fail(FText::FromString(StageRes.ErrorMessage));
		}

		FCommandResult Result = FCommandResult::Ok();
		Result.UpdatedStates.Reserve(InFiles.Num());
		for (const FString& File : InFiles)
		{
			Result.UpdatedStates.Add(Make_FileState(
				Normalize_AbsolutePath(File),
				EGitLink_FileState::Deleted,
				EGitLink_TreeState::Staged));
		}
		return Result;
	}
}

#undef LOCTEXT_NAMESPACE
