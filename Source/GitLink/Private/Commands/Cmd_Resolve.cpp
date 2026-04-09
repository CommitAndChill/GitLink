#include "Cmd_Shared.h"
#include "GitLink_CommandDispatcher.h"

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

		const FResult StageRes = InCtx.Repository->Stage(InFiles);
		if (!StageRes)
		{
			return FCommandResult::Fail(FText::FromString(StageRes.ErrorMessage));
		}

		// After resolving, the file is staged as Modified (or Added if it was new). We can't
		// tell which without re-querying status, so we report Modified/Staged — the editor
		// will call UpdateStatus afterward to refresh if it cares about the distinction.
		FCommandResult Result = FCommandResult::Ok();
		Result.UpdatedStates.Reserve(InFiles.Num());
		for (const FString& File : InFiles)
		{
			Result.UpdatedStates.Add(Make_FileState(
				Normalize_AbsolutePath(File),
				EGitLink_FileState::Modified,
				EGitLink_TreeState::Staged));
		}
		return Result;
	}
}

#undef LOCTEXT_NAMESPACE
