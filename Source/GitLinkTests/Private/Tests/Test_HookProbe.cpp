#include "Helpers/GitLinkTests_TempRemote.h"

#include "GitLink_HookProbe.h"   // from Plugins/GitLink/Source/GitLink/Private (exposed via PrivateIncludePaths)

#include <CoreMinimal.h>
#include <Misc/AutomationTest.h>
#include <Misc/FileHelper.h>
#include <Misc/Paths.h>

// --------------------------------------------------------------------------------------------------------------------
// Hook detection decides whether Check In commits through the git CLI (so pre-commit / commit-msg hooks run) or
// in-process through libgit2 (which ignores hooks). Probing a fixed `.git/hooks` missed two common setups: a repo
// with `core.hooksPath` (husky, lefthook and most shared-hooks tooling set it), and a linked worktree, whose `.git`
// is a file — in both, hooks silently stopped running on Check In.
// --------------------------------------------------------------------------------------------------------------------

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	auto Write_Hook(const FString& InDir, const TCHAR* InName) -> bool
	{
		return FFileHelper::SaveStringToFile(FString(TEXT("#!/bin/sh\nexit 0\n")), *FPaths::Combine(InDir, InName));
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGitLinkTests_HookProbe_ResolvesLikeGit,
	"GitLink.HookProbe.ResolvesLikeGit",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGitLinkTests_HookProbe_ResolvesLikeGit::RunTest(const FString& /*Parameters*/)
{
	if (!gitlink::tests::FTempRemote::Is_GitAvailable(*this))
	{ return true; }

	gitlink::tests::FTempRemote Remote;
	const FString Repo = Remote.Clone(TEXT("repo"));
	if (!TestFalse(TEXT("clone"), Repo.IsEmpty()))
	{ return false; }

	FGitLink_Subprocess& Git = Remote.Get_Subprocess();

	// 1. Plain repo: .git/hooks.
	TestFalse(TEXT("fresh clone has no active commit hooks"), FGitLink_HookProbe::Probe(Repo, &Git).NeedsSubprocessForCommit());
	Write_Hook(FPaths::Combine(Repo, TEXT(".git"), TEXT("hooks")), TEXT("pre-commit"));
	TestTrue(TEXT(".git/hooks/pre-commit detected"), FGitLink_HookProbe::Probe(Repo, &Git).bHasPreCommit);

	// 2. A linked worktree shares the main repo's hooks; its .git is a file, so .git/hooks does not exist there.
	const FString Worktree = FPaths::Combine(FPaths::GetPath(Repo), TEXT("worktree"));
	if (TestTrue(TEXT("git worktree add"),
		Remote.Git(Repo, { TEXT("worktree"), TEXT("add"), TEXT("--detach"), Worktree }).IsSuccess()))
	{
		TestTrue(TEXT("worktree sees the main repo's pre-commit"), FGitLink_HookProbe::Probe(Worktree, &Git).bHasPreCommit);
		TestFalse(TEXT("the old .git/hooks lookup would have missed it"),
			FGitLink_HookProbe::Probe_Directory(FPaths::Combine(Worktree, TEXT(".git"), TEXT("hooks"))).bHasPreCommit);
	}

	// 3. core.hooksPath replaces .git/hooks entirely.
	const FString SharedHooks = FPaths::Combine(Repo, TEXT(".githooks"));
	TestTrue(TEXT("set core.hooksPath"),
		Remote.Git(Repo, { TEXT("config"), TEXT("core.hooksPath"), TEXT(".githooks") }).IsSuccess());
	Write_Hook(SharedHooks, TEXT("commit-msg"));

	const FGitLink_HookFlags WithHooksPath = FGitLink_HookProbe::Probe(Repo, &Git);
	TestTrue(TEXT("commit-msg found via core.hooksPath"), WithHooksPath.bHasCommitMsg);
	TestFalse(TEXT("pre-commit in .git/hooks is no longer active once core.hooksPath is set"), WithHooksPath.bHasPreCommit);

	// Without git available the probe still works, falling back to .git/hooks.
	TestTrue(TEXT("fallback without git reads .git/hooks"), FGitLink_HookProbe::Probe(Repo, nullptr).bHasPreCommit);
	return true;
}

#endif  // WITH_DEV_AUTOMATION_TESTS
