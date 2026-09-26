#include "Helpers/GitLinkTests_TempRemote.h"

#include "Commands/Cmd_Push.h"   // from Plugins/GitLink/Source/GitLink/Private (exposed via PrivateIncludePaths)

#include "GitLinkCore/Repository/GitLink_Repository.h"
#include "GitLinkCore/Repository/GitLink_Repository_Params.h"
#include "GitLinkCore/Types/GitLink_Types.h"

#include <CoreMinimal.h>
#include <Misc/AutomationTest.h>
#include <Misc/FileHelper.h>

// --------------------------------------------------------------------------------------------------------------------
// Remote operations — fetch, pull (fast-forward) and push — against a local bare repository.
//
// Before these tests nothing exercised a remote at all: Sync/Fetch/Push had only ever been verified by hand. The
// "server" is a bare repo on disk (FTempRemote), so the suite needs git on PATH but no network or credentials.
// Two clones stand in for two users: "alice" is the repo under test, "bob" makes the remote move underneath her.
//
// What is under test is GitLink's own code — FRepository::Fetch / PullFastForward (libgit2) and cmd::Push (the
// toolbar's `git push`). Setup and verification go through the git CLI, an independent observer.
// --------------------------------------------------------------------------------------------------------------------

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	using gitlink::tests::FTempRemote;

	// Standard two-user setup. Returns false (after reporting) if anything could not be created.
	auto Setup_TwoClones(FAutomationTestBase& InTest, FTempRemote& InRemote, FString& OutAlice, FString& OutBob) -> bool
	{
		if (!InTest.TestTrue(TEXT("bare remote created"), InRemote.IsValid()))
		{ return false; }

		OutAlice = InRemote.Clone(TEXT("alice"));
		OutBob   = InRemote.Clone(TEXT("bob"));
		return InTest.TestFalse(TEXT("alice cloned"), OutAlice.IsEmpty())
			&& InTest.TestFalse(TEXT("bob cloned"),   OutBob.IsEmpty());
	}

	// Bob commits a file and pushes it with the git CLI, moving the remote ahead of Alice.
	auto Bob_PushesFile(FAutomationTestBase& InTest, FTempRemote& InRemote, const FString& InBob,
		const FString& InRelPath, const FString& InContents) -> bool
	{
		return InTest.TestTrue(TEXT("bob commits"), InRemote.Commit_File(InBob, InRelPath, InContents, TEXT("bob change")))
			&& InTest.TestTrue(TEXT("bob pushes"), InRemote.Git(InBob, { TEXT("push") }).IsSuccess());
	}
}

// --------------------------------------------------------------------------------------------------------------------
// Fetch moves the remote-tracking ref and nothing else.
// --------------------------------------------------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGitLinkTests_Remote_Fetch_UpdatesRemoteTrackingRef,
	"GitLink.Remote.Fetch.UpdatesRemoteTrackingRef",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGitLinkTests_Remote_Fetch_UpdatesRemoteTrackingRef::RunTest(const FString& /*Parameters*/)
{
	if (!FTempRemote::Is_GitAvailable(*this))
	{ return true; }

	FTempRemote Remote;
	FString Alice, Bob;
	if (!Setup_TwoClones(*this, Remote, Alice, Bob)
		|| !Bob_PushesFile(*this, Remote, Bob, TEXT("bob.txt"), TEXT("from bob\n")))
	{ return false; }

	const FString AliceHeadBefore = Remote.Get_Sha(Alice, TEXT("HEAD"));
	const FString BobHead         = Remote.Get_Sha(Bob,   TEXT("HEAD"));

	TUniquePtr<gitlink::FRepository> Repo = FTempRemote::Open(Alice);
	if (!TestTrue(TEXT("open alice"), Repo.IsValid()))
	{ return false; }

	const gitlink::FResult Fetched = Repo->Fetch(gitlink::FFetchParams{});
	TestTrue(FString::Printf(TEXT("Fetch: %s"), *Fetched.ErrorMessage), Fetched.bOk);

	TestEqual(TEXT("origin/main now points at bob's commit"), Remote.Get_Sha(Alice, TEXT("origin/main")), BobHead);
	TestEqual(TEXT("alice's HEAD did not move"),              Remote.Get_Sha(Alice, TEXT("HEAD")), AliceHeadBefore);
	TestTrue(TEXT("working tree untouched (bob.txt not checked out)"),
		FTempRemote::Read_File(Alice, TEXT("bob.txt")).IsEmpty());
	return true;
}

// --------------------------------------------------------------------------------------------------------------------
// Fetch uses the remote the current branch tracks. Regression for Sync/Fetch hardcoding "origin": with the remote
// renamed, the old code failed with "remote 'origin' not found".
// --------------------------------------------------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGitLinkTests_Remote_Fetch_UsesTrackedRemote,
	"GitLink.Remote.Fetch.UsesTrackedRemote",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGitLinkTests_Remote_Fetch_UsesTrackedRemote::RunTest(const FString& /*Parameters*/)
{
	if (!FTempRemote::Is_GitAvailable(*this))
	{ return true; }

	FTempRemote Remote;
	FString Alice, Bob;
	if (!Setup_TwoClones(*this, Remote, Alice, Bob)
		|| !Bob_PushesFile(*this, Remote, Bob, TEXT("bob.txt"), TEXT("from bob\n")))
	{ return false; }

	// `git remote rename` also rewrites branch.main.remote, so main now tracks "company/main" and no "origin" exists.
	if (!TestTrue(TEXT("rename origin -> company"),
		Remote.Git(Alice, { TEXT("remote"), TEXT("rename"), TEXT("origin"), TEXT("company") }).IsSuccess()))
	{ return false; }

	TUniquePtr<gitlink::FRepository> Repo = FTempRemote::Open(Alice);
	if (!TestTrue(TEXT("open alice"), Repo.IsValid()))
	{ return false; }

	const gitlink::FResult Fetched = Repo->Fetch(gitlink::FFetchParams{});
	TestTrue(FString::Printf(TEXT("Fetch with no remote name: %s"), *Fetched.ErrorMessage), Fetched.bOk);
	TestEqual(TEXT("company/main updated"), Remote.Get_Sha(Alice, TEXT("company/main")), Remote.Get_Sha(Bob, TEXT("HEAD")));

	const gitlink::FResult Pulled = Repo->PullFastForward(gitlink::FFetchParams{});
	TestTrue(FString::Printf(TEXT("PullFastForward: %s"), *Pulled.ErrorMessage), Pulled.bOk);
	TestEqual(TEXT("alice fast-forwarded to bob"), Remote.Get_Sha(Alice, TEXT("HEAD")), Remote.Get_Sha(Bob, TEXT("HEAD")));
	return true;
}

// --------------------------------------------------------------------------------------------------------------------
// Pull fast-forwards HEAD, the index and the working tree.
// --------------------------------------------------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGitLinkTests_Remote_Pull_FastForwards,
	"GitLink.Remote.Pull.FastForwards",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGitLinkTests_Remote_Pull_FastForwards::RunTest(const FString& /*Parameters*/)
{
	if (!FTempRemote::Is_GitAvailable(*this))
	{ return true; }

	FTempRemote Remote;
	FString Alice, Bob;
	if (!Setup_TwoClones(*this, Remote, Alice, Bob)
		|| !Bob_PushesFile(*this, Remote, Bob, TEXT("bob.txt"), TEXT("from bob\n")))
	{ return false; }

	TUniquePtr<gitlink::FRepository> Repo = FTempRemote::Open(Alice);
	if (!TestTrue(TEXT("open alice"), Repo.IsValid()))
	{ return false; }

	const gitlink::FResult Pulled = Repo->PullFastForward(gitlink::FFetchParams{});
	TestTrue(FString::Printf(TEXT("PullFastForward: %s"), *Pulled.ErrorMessage), Pulled.bOk);

	TestEqual(TEXT("HEAD is bob's commit"), Remote.Get_Sha(Alice, TEXT("HEAD")), Remote.Get_Sha(Bob, TEXT("HEAD")));
	TestEqual(TEXT("bob.txt checked out"), FTempRemote::Read_File(Alice, TEXT("bob.txt")), FString(TEXT("from bob\n")));

	const FGitLink_SubprocessResult Status = Remote.Git(Alice, { TEXT("status"), TEXT("--porcelain") });
	TestTrue(TEXT("git status runs"), Status.IsSuccess());
	TestTrue(FString::Printf(TEXT("working tree clean after pull (got '%s')"), *Status.StdOut.TrimStartAndEnd()),
		Status.StdOut.TrimStartAndEnd().IsEmpty());
	return true;
}

// --------------------------------------------------------------------------------------------------------------------
// Pull with nothing new is a successful no-op.
// --------------------------------------------------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGitLinkTests_Remote_Pull_UpToDateIsNoOp,
	"GitLink.Remote.Pull.UpToDateIsNoOp",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGitLinkTests_Remote_Pull_UpToDateIsNoOp::RunTest(const FString& /*Parameters*/)
{
	if (!FTempRemote::Is_GitAvailable(*this))
	{ return true; }

	FTempRemote Remote;
	FString Alice, Bob;
	if (!Setup_TwoClones(*this, Remote, Alice, Bob))
	{ return false; }

	const FString HeadBefore = Remote.Get_Sha(Alice, TEXT("HEAD"));

	TUniquePtr<gitlink::FRepository> Repo = FTempRemote::Open(Alice);
	if (!TestTrue(TEXT("open alice"), Repo.IsValid()))
	{ return false; }

	const gitlink::FResult Pulled = Repo->PullFastForward(gitlink::FFetchParams{});
	TestTrue(FString::Printf(TEXT("PullFastForward: %s"), *Pulled.ErrorMessage), Pulled.bOk);
	TestEqual(TEXT("HEAD unchanged"), Remote.Get_Sha(Alice, TEXT("HEAD")), HeadBefore);
	return true;
}

// --------------------------------------------------------------------------------------------------------------------
// Diverged history: pull must refuse, say why, and leave HEAD and the working tree exactly as they were.
// --------------------------------------------------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGitLinkTests_Remote_Pull_DivergedFailsWithoutTouchingTree,
	"GitLink.Remote.Pull.DivergedFailsWithoutTouchingTree",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGitLinkTests_Remote_Pull_DivergedFailsWithoutTouchingTree::RunTest(const FString& /*Parameters*/)
{
	if (!FTempRemote::Is_GitAvailable(*this))
	{ return true; }

	FTempRemote Remote;
	FString Alice, Bob;
	if (!Setup_TwoClones(*this, Remote, Alice, Bob)
		|| !Bob_PushesFile(*this, Remote, Bob, TEXT("bob.txt"), TEXT("from bob\n"))
		|| !TestTrue(TEXT("alice commits locally"),
			Remote.Commit_File(Alice, TEXT("alice.txt"), TEXT("from alice\n"), TEXT("alice change"))))
	{ return false; }

	const FString AliceHead = Remote.Get_Sha(Alice, TEXT("HEAD"));

	TUniquePtr<gitlink::FRepository> Repo = FTempRemote::Open(Alice);
	if (!TestTrue(TEXT("open alice"), Repo.IsValid()))
	{ return false; }

	const gitlink::FResult Pulled = Repo->PullFastForward(gitlink::FFetchParams{});
	TestFalse(TEXT("PullFastForward refuses a non-fast-forward"), Pulled.bOk);
	TestTrue(FString::Printf(TEXT("error explains why (got '%s')"), *Pulled.ErrorMessage),
		Pulled.ErrorMessage.Contains(TEXT("non-fast-forward")));

	TestEqual(TEXT("HEAD unchanged"), Remote.Get_Sha(Alice, TEXT("HEAD")), AliceHead);
	TestEqual(TEXT("alice's file intact"), FTempRemote::Read_File(Alice, TEXT("alice.txt")), FString(TEXT("from alice\n")));
	TestTrue(TEXT("bob's file not checked out"), FTempRemote::Read_File(Alice, TEXT("bob.txt")).IsEmpty());
	TestEqual(TEXT("but the fetch half still ran"), Remote.Get_Sha(Alice, TEXT("origin/main")), Remote.Get_Sha(Bob, TEXT("HEAD")));
	return true;
}

// --------------------------------------------------------------------------------------------------------------------
// Push: a commit made through GitLinkCore reaches the remote and another user can pull it.
// --------------------------------------------------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGitLinkTests_Remote_Push_CommitRoundTrip,
	"GitLink.Remote.Push.CommitRoundTrip",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGitLinkTests_Remote_Push_CommitRoundTrip::RunTest(const FString& /*Parameters*/)
{
	if (!FTempRemote::Is_GitAvailable(*this))
	{ return true; }

	FTempRemote Remote;
	FString Alice, Bob;
	if (!Setup_TwoClones(*this, Remote, Alice, Bob))
	{ return false; }

	// Alice commits in-process (libgit2), the way Check In does for a repo without hooks.
	{
		TUniquePtr<gitlink::FRepository> Repo = FTempRemote::Open(Alice);
		if (!TestTrue(TEXT("open alice"), Repo.IsValid()))
		{ return false; }

		const FString Path = Alice / TEXT("alice.txt");
		if (!TestTrue(TEXT("write alice.txt"), FFileHelper::SaveStringToFile(FString(TEXT("from alice\n")), *Path)))
		{ return false; }

		TestTrue(TEXT("stage"), Repo->Stage({ TEXT("alice.txt") }).bOk);
		gitlink::FCommitParams Params;
		Params.Message = TEXT("alice change");
		const gitlink::FResult Committed = Repo->Commit(Params);
		if (!TestTrue(FString::Printf(TEXT("commit: %s"), *Committed.ErrorMessage), Committed.bOk))
		{ return false; }
	}

	const gitlink::cmd::FPushOutcome Pushed = gitlink::cmd::Push(Remote.Get_Subprocess(), Alice, /*InTimeoutSec=*/ 60.0);
	TestTrue(FString::Printf(TEXT("cmd::Push: %s"), *Pushed.ErrorMessage), Pushed.bOk);
	TestEqual(TEXT("remote main is alice's commit"),
		Remote.Get_Sha(Remote.Get_BareRoot(), TEXT("main")), Remote.Get_Sha(Alice, TEXT("HEAD")));

	TUniquePtr<gitlink::FRepository> BobRepo = FTempRemote::Open(Bob);
	if (!TestTrue(TEXT("open bob"), BobRepo.IsValid()))
	{ return false; }

	const gitlink::FResult Pulled = BobRepo->PullFastForward(gitlink::FFetchParams{});
	TestTrue(FString::Printf(TEXT("bob pulls: %s"), *Pulled.ErrorMessage), Pulled.bOk);
	TestEqual(TEXT("bob sees alice's file"), FTempRemote::Read_File(Bob, TEXT("alice.txt")), FString(TEXT("from alice\n")));
	return true;
}

// --------------------------------------------------------------------------------------------------------------------
// Push rejected (remote moved on): cmd::Push reports failure with git's reason instead of a bare "failed".
// --------------------------------------------------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGitLinkTests_Remote_Push_RejectedReportsReason,
	"GitLink.Remote.Push.RejectedReportsReason",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGitLinkTests_Remote_Push_RejectedReportsReason::RunTest(const FString& /*Parameters*/)
{
	if (!FTempRemote::Is_GitAvailable(*this))
	{ return true; }

	FTempRemote Remote;
	FString Alice, Bob;
	if (!Setup_TwoClones(*this, Remote, Alice, Bob)
		|| !Bob_PushesFile(*this, Remote, Bob, TEXT("bob.txt"), TEXT("from bob\n"))
		|| !TestTrue(TEXT("alice commits locally"),
			Remote.Commit_File(Alice, TEXT("alice.txt"), TEXT("from alice\n"), TEXT("alice change"))))
	{ return false; }

	const FString RemoteHeadBefore = Remote.Get_Sha(Remote.Get_BareRoot(), TEXT("main"));

	const gitlink::cmd::FPushOutcome Pushed = gitlink::cmd::Push(Remote.Get_Subprocess(), Alice, /*InTimeoutSec=*/ 60.0);
	TestFalse(TEXT("push is rejected"), Pushed.bOk);
	TestTrue(FString::Printf(TEXT("reason is surfaced (got '%s')"), *Pushed.ErrorMessage),
		Pushed.ErrorMessage.Contains(TEXT("rejected")));
	TestEqual(TEXT("remote unchanged"), Remote.Get_Sha(Remote.Get_BareRoot(), TEXT("main")), RemoteHeadBefore);
	return true;
}

#endif  // WITH_DEV_AUTOMATION_TESTS
