#include "Cmd_Shared.h"
#include "GitLink_CommandDispatcher.h"

#include "GitLinkCore/Repository/GitLink_Repository.h"

#define LOCTEXT_NAMESPACE "GitLinkCmdMarkForAdd"

// --------------------------------------------------------------------------------------------------------------------
// Cmd_MarkForAdd — "git add <files>". The editor calls this for untracked files the user wants
// to include in the next commit.
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

		const FResult StageRes = InCtx.Repository->Stage(InFiles);
		if (!StageRes)
		{
			return FCommandResult::Fail(FText::FromString(StageRes.ErrorMessage));
		}

		FCommandResult Result = FCommandResult::Ok();
		Result.UpdatedStates.Reserve(InFiles.Num());
		for (const FString& File : InFiles)
		{
			Result.UpdatedStates.Add(Make_FileState(
				Normalize_AbsolutePath(File),
				EGitLink_FileState::Added,
				EGitLink_TreeState::Staged));
		}
		return Result;
	}
}

#undef LOCTEXT_NAMESPACE
