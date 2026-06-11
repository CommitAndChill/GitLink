#include "GitLink_CommandDispatcher.h"
#include "GitLink_Subprocess.h"
#include "GitLinkLog.h"

#include "GitLink/GitLink_Provider.h"

#include "GitLinkCore/Repository/GitLink_Repository.h"
#include "GitLinkCore/Repository/GitLink_Repository_Params.h"

#define LOCTEXT_NAMESPACE "GitLinkCmdSync"

// --------------------------------------------------------------------------------------------------------------------
// Cmd_Sync — "git pull --ff-only". The editor issues this when the user clicks "Sync" in the
// source control menu. Non-fast-forward scenarios (divergent history, needed merge commit)
// return a clear error pointing at CLI resolution; automating merge resolution is out of scope
// for v1 and deferred to a follow-up along with Cmd_Resolve enhancements.
// --------------------------------------------------------------------------------------------------------------------

namespace gitlink::cmd
{
	auto Sync(
		FCommandContext&                  InCtx,
		const FSourceControlOperationRef& /*InOperation*/,
		const TArray<FString>&            /*InFiles*/) -> FCommandResult
	{
		if (InCtx.Repository == nullptr || !InCtx.Repository->IsOpen())
		{
			return FCommandResult::Fail(LOCTEXT("NoRepo",
				"GitLink: cannot sync — repository not open."));
		}

		UE_LOG(LogGitLink, Log, TEXT("Cmd_Sync: pulling from 'origin' (fast-forward)"));

		gitlink::FFetchParams Params;
		Params.RemoteName = TEXT("origin");

		const FResult PullRes = InCtx.Repository->PullFastForward(Params, /*InProgress=*/ nullptr);
		if (!PullRes)
		{
			UE_LOG(LogGitLink, Warning,
				TEXT("Cmd_Sync: pull failed: %s"), *PullRes.ErrorMessage);
			return FCommandResult::Fail(FText::FromString(PullRes.ErrorMessage));
		}

		UE_LOG(LogGitLink, Log, TEXT("Cmd_Sync: pull complete (fast-forward)"));

		// Rehydrate LFS content. PullFastForward checks out via libgit2, which does NOT run
		// Git's smudge filters — any LFS-tracked file whose blob changed lands on disk as its
		// ~132-byte pointer text and Unreal fails to load the asset (the same bug shape
		// Cmd_Revert's v0.3.4 fix addressed). Unlike the revert case, freshly-pulled LFS
		// objects have never been downloaded (git_remote_fetch transfers only git objects; the
		// LFS transfer protocol is a separate channel), so this must be `git lfs pull` — it
		// fetches missing objects AND re-runs checkout. Idempotent; no-op when nothing changed.
		// Failure is non-fatal: the pull itself succeeded, and recovery is a manual
		// `git lfs pull`.
		//
		// Sync only pulls the parent repo (no submodule partitioning yet), so a single
		// parent-rooted rehydration matches its scope. If Sync grows submodule support, route
		// per-submodule via the cwdOverride like Cmd_Revert's RehydrateLfsForRepo.
		if (InCtx.Subprocess != nullptr && InCtx.Subprocess->IsValid() && InCtx.Provider.Is_LfsAvailable())
		{
			const FGitLink_SubprocessResult LfsRes = InCtx.Subprocess->RunLfs({ TEXT("pull") }, FString());
			if (!LfsRes.IsSuccess())
			{
				UE_LOG(LogGitLink, Warning,
					TEXT("Cmd_Sync: 'git lfs pull' rehydration returned %d: %s — LFS-tracked files ")
					TEXT("may be pointer text on disk; run `git lfs pull` manually to recover"),
					LfsRes.ExitCode, *LfsRes.StdErr);
			}
		}

		// UpdateStates is left empty — after a fast-forward the editor will re-query via an
		// automatic UpdateStatus if anything changed on disk, which is when the real per-file
		// state deltas come through.
		FCommandResult Result = FCommandResult::Ok();
		Result.InfoMessages.Add(LOCTEXT("SyncOk",
			"Pulled from 'origin' (fast-forward)."));
		return Result;
	}
}

#undef LOCTEXT_NAMESPACE
