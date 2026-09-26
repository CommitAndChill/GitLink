#include "Helpers/GitLinkTests_TempRemote.h"

#include "Commands/Cmd_Shared.h"   // Make_GitCredentialProvider — the provider Cmd_Sync / Cmd_Fetch use

#include "GitLinkCore/Repository/GitLink_Repository.h"
#include "GitLinkCore/Repository/GitLink_Repository_Params.h"
#include "GitLinkCore/Types/GitLink_Types.h"

#include <CoreMinimal.h>
#include <HAL/PlatformMisc.h>
#include <Misc/AutomationTest.h>

// --------------------------------------------------------------------------------------------------------------------
// Opt-in live tests against a real, private HTTPS remote.
//
// The local-bare-remote suite (Test_Remote_Operations.cpp) cannot see authentication: a filesystem remote never asks
// for credentials. These tests do. They run only when GITLINK_LIVE_REMOTE is set to the HTTPS URL of a private
// repository the current user can read (and the git CLI can already authenticate to); otherwise they pass with an
// informational note, so the default suite stays offline.
//
//   set GITLINK_LIVE_REMOTE=https://github.com/<owner>/<private-repo>.git
// --------------------------------------------------------------------------------------------------------------------

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	auto Get_LiveRemoteUrl(FAutomationTestBase& InTest) -> FString
	{
		const FString Url = FPlatformMisc::GetEnvironmentVariable(TEXT("GITLINK_LIVE_REMOTE")).TrimStartAndEnd();
		if (Url.IsEmpty())
		{ InTest.AddInfo(TEXT("GITLINK_LIVE_REMOTE not set — live remote test skipped")); }
		return Url;
	}
}

// --------------------------------------------------------------------------------------------------------------------
// libgit2 fetch + fast-forward pull authenticate against a private HTTPS remote. This is the path the editor's Sync
// and the background fetch take; the git CLI clone below proves the credentials themselves are available.
// --------------------------------------------------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGitLinkTests_Live_FetchAndPull_PrivateHttps,
	"GitLink.Live.FetchAndPull.PrivateHttps",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGitLinkTests_Live_FetchAndPull_PrivateHttps::RunTest(const FString& /*Parameters*/)
{
	const FString Url = Get_LiveRemoteUrl(*this);
	if (Url.IsEmpty() || !gitlink::tests::FTempRemote::Is_GitAvailable(*this))
	{ return true; }

	gitlink::tests::FTempRemote Fixture;
	const FString Clone = Fixture.Clone(TEXT("live"), Url);
	if (!TestFalse(TEXT("git CLI can clone the live remote (credentials available)"), Clone.IsEmpty()))
	{ return false; }

	TUniquePtr<gitlink::FRepository> Repo = gitlink::tests::FTempRemote::Open(Clone);
	if (!TestTrue(TEXT("open clone"), Repo.IsValid()))
	{ return false; }

	// Same credential source as Cmd_Sync / Cmd_Fetch.
	gitlink::FFetchParams Params;
	Params.Credentials = gitlink::cmd::Make_GitCredentialProvider(
		MakeShared<FGitLink_Subprocess>(TEXT("git"), Clone));

	const gitlink::FResult Fetched = Repo->Fetch(Params);
	TestTrue(FString::Printf(TEXT("libgit2 Fetch over HTTPS: %s"), *Fetched.ErrorMessage), Fetched.bOk);

	const gitlink::FResult Pulled = Repo->PullFastForward(Params);
	TestTrue(FString::Printf(TEXT("libgit2 PullFastForward over HTTPS: %s"), *Pulled.ErrorMessage), Pulled.bOk);
	return true;
}

// --------------------------------------------------------------------------------------------------------------------
// Control for the test above: with no credential provider libgit2 gets a 401 from a private remote. If this ever
// starts passing, the credential plumbing is no longer what makes the positive test pass.
// --------------------------------------------------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGitLinkTests_Live_Fetch_NoCredentialsIsRejected,
	"GitLink.Live.Fetch.NoCredentialsIsRejected",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGitLinkTests_Live_Fetch_NoCredentialsIsRejected::RunTest(const FString& /*Parameters*/)
{
	const FString Url = Get_LiveRemoteUrl(*this);
	if (Url.IsEmpty() || !gitlink::tests::FTempRemote::Is_GitAvailable(*this))
	{ return true; }

	gitlink::tests::FTempRemote Fixture;
	const FString Clone = Fixture.Clone(TEXT("live"), Url);
	if (!TestFalse(TEXT("git CLI can clone the live remote"), Clone.IsEmpty()))
	{ return false; }

	TUniquePtr<gitlink::FRepository> Repo = gitlink::tests::FTempRemote::Open(Clone);
	if (!TestTrue(TEXT("open clone"), Repo.IsValid()))
	{ return false; }

	const gitlink::FResult Fetched = Repo->Fetch(gitlink::FFetchParams{});
	TestFalse(TEXT("fetch without credentials is refused by a private remote"), Fetched.bOk);
	return true;
}

#endif  // WITH_DEV_AUTOMATION_TESTS
