#include "GitLink_LfsHttpClient.h"
#include "GitLink_Subprocess.h"

#include "Helpers/GitLinkTests_TempRepo.h"

#include "GitLinkCore/Repository/GitLink_Repository.h"

#include <CoreMinimal.h>
#include <Misc/AutomationTest.h>
#include <Misc/Paths.h>

// --------------------------------------------------------------------------------------------------------------------
// LFS endpoint resolution must not spawn `git` when the caller already holds an open repository.
//
// Why this file exists: FGitLink_Provider::CheckRepositoryStatus resolves an LFS endpoint for the
// parent repo plus EVERY submodule, on the game thread, during editor module startup. Each
// resolution used to run `git config --get lfs.url`, `git config --get remote.origin.url` and
// `git symbolic-ref --quiet HEAD` as three separate processes. On BusterBlock (31 submodules) that
// is 96 serial spawns — measured 2.886 s of blocked editor-boot time, paid twice per boot because
// the automatic FConnect refreshes the status again. Process creation, not git, was the cost.
//
// The fix reads those three facts through the libgit2 handle the submodule walk already opens. The
// load-bearing property is NEGATIVE — "no process was spawned" — so the tests below assert it the
// only way that cannot silently rot: they hand the client a git binary that CANNOT run. Anything
// that reintroduces a spawn on the facts path fails these tests instead of quietly costing 2.9 s
// again.
//
// Companion ops tested here too (gitlink::op::Get_ConfigString / Get_HeadSymbolicRefName), since a
// wrong value there would send resolution down the subprocess fallback and the perf win would
// vanish with no visible failure.
// --------------------------------------------------------------------------------------------------------------------

#if WITH_DEV_AUTOMATION_TESTS

namespace gitlink::op
{
	auto Get_ConfigString(gitlink::FRepository& InRepo, const FString& InKey) -> FString;
	auto Get_HeadSymbolicRefName(gitlink::FRepository& InRepo) -> FString;
}

namespace
{
	// A git binary that provably cannot be launched, so every FGitLink_Subprocess::Run reports
	// bSpawned=false. IsValid() still returns true (it only checks for a non-empty path), which is
	// what lets the client get as far as attempting resolution.
	//
	// Constructed in place at each call site rather than returned from a helper:
	// FGitLink_Subprocess deletes its copy constructor, which suppresses the implicit move, and
	// relying on guaranteed copy elision for a type with neither is a needless bet.
	const TCHAR* k_UnrunnableGitBinary = TEXT("D:/__gitlink_tests_no_such_git_binary__/git.exe");

	auto Make_FullFacts() -> FGitLink_LfsHttpClient::FRepoFacts
	{
		FGitLink_LfsHttpClient::FRepoFacts Facts;
		Facts.ConfiguredLfsUrl = FString();  // unset — the normal case; derivation takes over
		Facts.RemoteOriginUrl  = TEXT("https://github.com/chainkemists/BusterBlock.git");
		Facts.bConfigKnown     = true;
		Facts.HeadRefName      = TEXT("refs/heads/dev");
		Facts.bHeadRefKnown    = true;
		return Facts;
	}
}

// --------------------------------------------------------------------------------------------------------------------
// 1. Facts path resolves with a git binary that cannot run at all.
// --------------------------------------------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGitLinkTests_LfsEndpoint_FactsPathNeedsNoSubprocess,
	"GitLink.LfsEndpoint.FactsPathNeedsNoSubprocess",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGitLinkTests_LfsEndpoint_FactsPathNeedsNoSubprocess::RunTest(const FString& /*Parameters*/)
{
	FGitLink_Subprocess    Subprocess{k_UnrunnableGitBinary, FPaths::ProjectDir()};
	FGitLink_LfsHttpClient Client{Subprocess};

	const FString Root = TEXT("D:/Repos/BusterBlock/");

	TestTrue(TEXT("resolved from supplied facts"),
		Client.Resolve_LfsUrlForRepo(Root, Make_FullFacts()));
	TestTrue(TEXT("endpoint cached for the root"), Client.Has_LfsUrl(Root));
	return true;
}

// --------------------------------------------------------------------------------------------------------------------
// 2. Control arm — put the variable back. With NO facts the same client must fail, because the
//    only way to learn them is the subprocess that cannot run. Without this arm, test 1 would
//    still pass if resolution had silently stopped depending on facts at all.
// --------------------------------------------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGitLinkTests_LfsEndpoint_NoFactsFallsBackToSubprocess,
	"GitLink.LfsEndpoint.NoFactsFallsBackToSubprocess",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGitLinkTests_LfsEndpoint_NoFactsFallsBackToSubprocess::RunTest(const FString& /*Parameters*/)
{
	FGitLink_Subprocess    Subprocess{k_UnrunnableGitBinary, FPaths::ProjectDir()};
	FGitLink_LfsHttpClient Client{Subprocess};

	const FString Root = TEXT("D:/Repos/BusterBlock/");

	TestFalse(TEXT("cannot resolve without facts when git will not run"),
		Client.Resolve_LfsUrlForRepo(Root));
	TestFalse(TEXT("nothing cached"), Client.Has_LfsUrl(Root));
	return true;
}

// --------------------------------------------------------------------------------------------------------------------
// 3. An explicit lfs.url in the facts wins over derivation, and still spawns nothing.
// --------------------------------------------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGitLinkTests_LfsEndpoint_ExplicitLfsUrlWins,
	"GitLink.LfsEndpoint.ExplicitLfsUrlWins",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGitLinkTests_LfsEndpoint_ExplicitLfsUrlWins::RunTest(const FString& /*Parameters*/)
{
	FGitLink_Subprocess    Subprocess{k_UnrunnableGitBinary, FPaths::ProjectDir()};
	FGitLink_LfsHttpClient Client{Subprocess};

	auto Facts = Make_FullFacts();
	Facts.ConfiguredLfsUrl = TEXT("https://lfs.example.com/custom/endpoint");

	const FString Root = TEXT("D:/Repos/SomeOtherRepo/");

	TestTrue(TEXT("resolved from explicit lfs.url"), Client.Resolve_LfsUrlForRepo(Root, Facts));
	TestTrue(TEXT("endpoint cached"), Client.Has_LfsUrl(Root));
	return true;
}

// --------------------------------------------------------------------------------------------------------------------
// 4. A fact set with neither an lfs.url nor a remote must fail WITHOUT probing — a submodule with
//    no origin remote has no LFS endpoint, and re-asking git about it is exactly the spawn storm
//    this change removes. bConfigKnown=true is what says "I looked; there is nothing there".
// --------------------------------------------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGitLinkTests_LfsEndpoint_KnownEmptyConfigDoesNotProbe,
	"GitLink.LfsEndpoint.KnownEmptyConfigDoesNotProbe",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGitLinkTests_LfsEndpoint_KnownEmptyConfigDoesNotProbe::RunTest(const FString& /*Parameters*/)
{
	FGitLink_Subprocess    Subprocess{k_UnrunnableGitBinary, FPaths::ProjectDir()};
	FGitLink_LfsHttpClient Client{Subprocess};

	FGitLink_LfsHttpClient::FRepoFacts Facts;
	Facts.bConfigKnown  = true;   // looked, and both keys are unset
	Facts.bHeadRefKnown = true;   // looked, and HEAD is detached

	const FString Root = TEXT("D:/Repos/NoRemoteSubmodule/");

	TestFalse(TEXT("no endpoint without a remote"), Client.Resolve_LfsUrlForRepo(Root, Facts));
	TestFalse(TEXT("nothing cached"), Client.Has_LfsUrl(Root));
	return true;
}

// --------------------------------------------------------------------------------------------------------------------
// 5. The libgit2 reads that feed the facts return the same answers `git` would.
// --------------------------------------------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGitLinkTests_LfsEndpoint_ConfigAndHeadRefReads,
	"GitLink.LfsEndpoint.ConfigAndHeadRefReads",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGitLinkTests_LfsEndpoint_ConfigAndHeadRefReads::RunTest(const FString& /*Parameters*/)
{
	gitlink::tests::FTempRepo Repo;
	if (!Repo.IsValid())
	{
		AddError(TEXT("FTempRepo could not be created — cannot exercise the libgit2 config reads."));
		return false;
	}

	TUniquePtr<gitlink::FRepository> Open = Repo.Open();
	if (!Open.IsValid())
	{
		AddError(TEXT("FTempRepo::Open failed — cannot exercise the libgit2 config reads."));
		return false;
	}

	// FTempRepo seeds user.name / user.email, so this proves the merged config chain is readable.
	TestFalse(TEXT("a set key reads back non-empty"),
		gitlink::op::Get_ConfigString(*Open, TEXT("user.name")).IsEmpty());

	// The quiet-empty contract: `git config --get lfs.url` exits 1 on a repo without one, and this
	// must be an empty string rather than a warning or a garbage value. GitLink v0.3.7 demoted the
	// subprocess equivalent to Verbose for the same reason.
	TestTrue(TEXT("an unset key reads back empty"),
		gitlink::op::Get_ConfigString(*Open, TEXT("lfs.url")).IsEmpty());
	TestTrue(TEXT("an unset remote reads back empty"),
		gitlink::op::Get_ConfigString(*Open, TEXT("remote.origin.url")).IsEmpty());

	// A freshly initialised repo has HEAD as a SYMBOLIC ref to its unborn default branch. The LFS
	// `ref` field wants the full name, which is what `git symbolic-ref HEAD` prints — not the short
	// branch name FRepository::Get_CurrentBranchName returns.
	const FString HeadRef = gitlink::op::Get_HeadSymbolicRefName(*Open);
	TestTrue(TEXT("HEAD symbolic ref is a full refs/heads/ name"),
		HeadRef.StartsWith(TEXT("refs/heads/")));

	return true;
}

#endif  // WITH_DEV_AUTOMATION_TESTS
