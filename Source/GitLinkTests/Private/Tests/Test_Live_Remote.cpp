#include "Helpers/GitLinkTests_TempRemote.h"

#include "Commands/Cmd_Shared.h"   // Make_GitCredentialProvider — the provider Cmd_Sync / Cmd_Fetch use
#include "GitLink_LfsHttpClient.h"

#include "GitLinkCore/Repository/GitLink_Repository.h"
#include "GitLinkCore/Repository/GitLink_Repository_Params.h"
#include "GitLinkCore/Types/GitLink_Types.h"

#include <Async/Async.h>
#include <CoreMinimal.h>
#include <HAL/PlatformMisc.h>
#include <HttpManager.h>
#include <HttpModule.h>
#include <Misc/AutomationTest.h>
#include <Misc/ScopeExit.h>

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

	// FGitLink_LfsHttpClient blocks its calling thread until the HTTP completion delegate fires, and that delegate
	// is delivered on the game thread — which is where automation tests run. So run the call on a worker and tick
	// the HTTP manager here until it finishes (the editor gets the same effect from its own tick loop).
	template <typename TResult>
	auto Run_PumpingHttp(TFunction<TResult()> InCall, double InTimeoutSec = 30.0) -> TOptional<TResult>
	{
		TFuture<TResult> Future = Async(EAsyncExecution::ThreadPool, MoveTemp(InCall));
		const double Deadline = FPlatformTime::Seconds() + InTimeoutSec;
		while (!Future.IsReady() && FPlatformTime::Seconds() < Deadline)
		{
			FHttpModule::Get().GetHttpManager().Tick(0.01f);
			FPlatformProcess::Sleep(0.01f);
		}
		if (!Future.IsReady())
		{ return {}; }
		return Future.Get();
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

// --------------------------------------------------------------------------------------------------------------------
// LFS locking end to end against a real LFS server (the live remote's). Locks and unlocks through `git lfs` exactly
// as Check Out / Revert do, and reads lock state back through GitLink's in-process HTTP client — both the
// /locks/verify sweep and the single-file probe — so ours/theirs classification is checked against the server.
// The remote must contain an LFS-lockable Content/Seed.uasset (see the scratch repo's seed commit).
// --------------------------------------------------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGitLinkTests_Live_Lfs_LockVerifyUnlock,
	"GitLink.Live.Lfs.LockVerifyUnlock",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGitLinkTests_Live_Lfs_LockVerifyUnlock::RunTest(const FString& /*Parameters*/)
{
	const FString Url = Get_LiveRemoteUrl(*this);
	if (Url.IsEmpty() || !gitlink::tests::FTempRemote::Is_GitAvailable(*this))
	{ return true; }

	const FString RelPath = TEXT("Content/Seed.uasset");

	gitlink::tests::FTempRemote Fixture;
	const FString Clone = Fixture.Clone(TEXT("live"), Url);
	if (!TestFalse(TEXT("clone the live remote"), Clone.IsEmpty()))
	{ return false; }

	FGitLink_Subprocess Git(TEXT("git"), Clone);
	if (!TestTrue(TEXT("git-lfs available"), Git.IsLfsAvailable()))
	{ return false; }

	// A previous failed run may have left the lock held; start clean and always release on the way out.
	Git.RunLfs({ TEXT("unlock"), TEXT("--"), RelPath }, Clone);
	ON_SCOPE_EXIT { Git.RunLfs({ TEXT("unlock"), TEXT("--"), RelPath }, Clone); };

	FGitLink_LfsHttpClient Client(Git);
	if (!TestTrue(TEXT("resolve LFS endpoint"), Client.Resolve_LfsUrlForRepo(Clone)))
	{ return false; }

	// Lock — the Check Out path.
	const FGitLink_SubprocessResult Locked = Git.RunLfs({ TEXT("lock"), TEXT("--"), RelPath }, Clone);
	if (!TestTrue(FString::Printf(TEXT("git lfs lock: %s"), *Locked.Get_CombinedError()), Locked.IsSuccess()))
	{ return false; }

	// The sweep sees it as ours.
	const TOptional<FGitLink_Subprocess::FLfsLocksSnapshot> AfterLock = Run_PumpingHttp<FGitLink_Subprocess::FLfsLocksSnapshot>(
		[&Client, &Clone]() { return Client.Request_LocksVerify(Clone); });
	if (!TestTrue(TEXT("/locks/verify completed"), AfterLock.IsSet()))
	{ return false; }
	TestTrue(TEXT("/locks/verify succeeded (auth + endpoint)"), AfterLock->bSuccess);
	TestTrue(TEXT("lock is listed"), AfterLock->AllLocks.Contains(RelPath));
	TestTrue(TEXT("lock is classified as ours"), AfterLock->OursPaths.Contains(RelPath));

	// The single-file probe (focus / asset-open / pre-checkout path) agrees, using the identity the sweep learned.
	const TOptional<FGitLink_LfsHttpClient::FSingleFileLockResult> Probe = Run_PumpingHttp<FGitLink_LfsHttpClient::FSingleFileLockResult>(
		[&Client, &Clone, &RelPath]() { return Client.Request_SingleFileLock(Clone, FPaths::Combine(Clone, RelPath)); });
	if (TestTrue(TEXT("single-file probe completed"), Probe.IsSet()))
	{
		TestTrue(TEXT("single-file probe succeeded"), Probe->bSuccess);
		TestEqual(TEXT("single-file probe says Locked (ours)"),
			static_cast<int32>(Probe->Lock), static_cast<int32>(EGitLink_LockState::Locked));
	}

	// Unlock — the Revert / Check In path (no --force: GitLink never forces).
	const FGitLink_SubprocessResult Unlocked = Git.RunLfs({ TEXT("unlock"), TEXT("--"), RelPath }, Clone);
	TestTrue(FString::Printf(TEXT("git lfs unlock: %s"), *Unlocked.Get_CombinedError()), Unlocked.IsSuccess());

	// LFS read replicas can lag a few seconds behind a write (see v0.3.5); poll briefly for the release.
	bool bReleased = false;
	for (int32 Attempt = 0; Attempt < 10 && !bReleased; ++Attempt)
	{
		const TOptional<FGitLink_Subprocess::FLfsLocksSnapshot> Page = Run_PumpingHttp<FGitLink_Subprocess::FLfsLocksSnapshot>(
			[&Client, &Clone]() { return Client.Request_LocksVerify(Clone); });
		bReleased = Page.IsSet() && Page->bSuccess && !Page->AllLocks.Contains(RelPath);
		if (!bReleased)
		{ FPlatformProcess::Sleep(1.0f); }
	}
	TestTrue(TEXT("server reports the lock released"), bReleased);
	return true;
}

#endif  // WITH_DEV_AUTOMATION_TESTS
