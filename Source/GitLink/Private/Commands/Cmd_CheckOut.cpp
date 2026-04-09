#include "Cmd_Shared.h"
#include "GitLink_CommandDispatcher.h"
#include "GitLinkLog.h"

#define LOCTEXT_NAMESPACE "GitLinkCmdCheckOut"

// --------------------------------------------------------------------------------------------------------------------
// Cmd_CheckOut — "acquire an LFS lock". In Unreal's source control vocabulary, "checkout" means
// "I want exclusive edit rights on this file" and maps to git-lfs file locking for binary
// assets. libgit2 does NOT implement LFS at all, so this command needs a subprocess escape
// hatch (git.exe lfs lock / lfs locks --verify).
//
// PASS 3 STUB:
//   - Reports the request in the log and returns success so the editor doesn't error
//   - Predictive state delta: File=Modified, Tree=Staged, Lock=Locked so the content browser
//     shows the "checked out by me" icon immediately
//   - The real implementation lands with FGitLink_Subprocess + FGitLink_HookProbe
// --------------------------------------------------------------------------------------------------------------------

namespace gitlink::cmd
{
	auto CheckOut(
		FCommandContext&                  /*InCtx*/,
		const FSourceControlOperationRef& /*InOperation*/,
		const TArray<FString>&            InFiles) -> FCommandResult
	{
		UE_LOG(LogGitLink, Log,
			TEXT("Cmd_CheckOut[stub]: %d file(s) — LFS locking deferred to FGitLink_Subprocess pass"),
			InFiles.Num());

		FCommandResult Result = FCommandResult::Ok();
		Result.UpdatedStates.Reserve(InFiles.Num());
		for (const FString& File : InFiles)
		{
			FGitLink_CompositeState Composite;
			Composite.Tree = EGitLink_TreeState::Working;
			Composite.Lock = EGitLink_LockState::Locked;   // predictive — real state once LFS lands
			Result.UpdatedStates.Add(Make_FileState(Normalize_AbsolutePath(File), Composite));
		}

		Result.InfoMessages.Add(LOCTEXT("Stub",
			"GitLink: CheckOut is a stub in this build — LFS locking is not yet implemented. "
			"File state is marked locally as checked out, but no server-side LFS lock has been taken."));

		return Result;
	}
}

#undef LOCTEXT_NAMESPACE
