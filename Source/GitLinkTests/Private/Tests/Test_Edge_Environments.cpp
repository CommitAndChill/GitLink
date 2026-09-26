#include "Helpers/GitLinkTests_TempRemote.h"
#include "Helpers/GitLinkTests_TempRepo.h"

#include "Commands/Cmd_Push.h"
#include "GitLink_LfsHttpClient.h"

#include "GitLinkCore/Repository/GitLink_Repository.h"
#include "GitLinkCore/Repository/GitLink_Repository_Params.h"
#include "GitLinkCore/Types/GitLink_Types.h"

#include <CoreMinimal.h>
#include <HAL/FileManager.h>
#include <Misc/AutomationTest.h>
#include <Misc/FileHelper.h>
#include <Misc/Guid.h>
#include <Misc/Paths.h>

// --------------------------------------------------------------------------------------------------------------------
// Environments a new user hits in the first five minutes, before any content exists: no git on PATH, a folder that
// is not a repository, a brand-new repository with no commits, a detached HEAD, and paths with spaces / non-ASCII
// characters. Each must produce a clear failure or just work — never a crash or a hang.
// --------------------------------------------------------------------------------------------------------------------

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGitLinkTests_Edge_NoGitBinary,
	"GitLink.Edge.NoGitBinary",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGitLinkTests_Edge_NoGitBinary::RunTest(const FString& /*Parameters*/)
{
	// A git binary path that cannot be launched: push and credential lookups must fail fast and say so.
	FGitLink_Subprocess Missing(TEXT("D:/__gitlink_tests_no_such_git_binary__/git.exe"), FPaths::ProjectSavedDir());
	// The spawn failures are logged as warnings (by the engine and by GitLink); declare them so they are expected.
	AddExpectedMessagePlain(TEXT("CreateProc failed"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 0);
	AddExpectedMessagePlain(TEXT("__gitlink_tests_no_such_git_binary__"), ELogVerbosity::Warning,
		EAutomationExpectedMessageFlags::Contains, 0);
	AddExpectedMessagePlain(TEXT("failed to spawn"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 0);

	const double Start = FPlatformTime::Seconds();
	const gitlink::cmd::FPushOutcome Pushed = gitlink::cmd::Push(Missing, FString(), /*InTimeoutSec=*/ 10.0);
	TestFalse(TEXT("push reports failure"), Pushed.bOk);
	TestFalse(TEXT("push failure has a message"), Pushed.ErrorMessage.IsEmpty());

	FString User, Pass;
	TestFalse(TEXT("credential lookup reports no credential"),
		gitlink::lfs_http::detail::Fill_CredentialForUrl(Missing.Get_GitBinary(), FString(),
			TEXT("https://github.com/org/repo.git"), User, Pass));
	TestTrue(TEXT("both failed fast (< 5 s)"), FPlatformTime::Seconds() - Start < 5.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGitLinkTests_Edge_NotARepository,
	"GitLink.Edge.NotARepository",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGitLinkTests_Edge_NotARepository::RunTest(const FString& /*Parameters*/)
{
	const FString Dir = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("GitLinkTests"),
		TEXT("NotARepo_") + FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	IFileManager::Get().MakeDirectory(*Dir, /*Tree=*/ true);

	// Walk-up discovery would find the host project's own repository; open this exact folder instead.
	gitlink::FOpenParams Params;
	Params.Path = Dir;
	Params.bNoDiscover = true;
	TUniquePtr<gitlink::FRepository> Repo = gitlink::FRepository::Open(Params);
	TestFalse(TEXT("opening a plain folder fails cleanly"), Repo.IsValid() && Repo->IsOpen());

	IFileManager::Get().DeleteDirectory(*Dir, /*RequireExists=*/ false, /*Tree=*/ true);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGitLinkTests_Edge_UnbornHead,
	"GitLink.Edge.UnbornHead",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGitLinkTests_Edge_UnbornHead::RunTest(const FString& /*Parameters*/)
{
	// `git init` and nothing else — the state every new project starts in.
	gitlink::tests::FTempRepo Temp;
	if (!TestTrue(TEXT("temp repo"), Temp.IsValid()))
	{ return false; }

	TUniquePtr<gitlink::FRepository> Repo = Temp.Open();
	if (!TestTrue(TEXT("an empty repository opens"), Repo.IsValid()))
	{ return false; }

	Temp.Write_File(TEXT("readme.md"), TEXT("hello\n"));
	const gitlink::FStatus Status = Repo->Get_Status();
	TestEqual(TEXT("status works before the first commit"), Status.Num(), 1);

	gitlink::FLogQuery Query;
	TestEqual(TEXT("history of an unborn branch is empty, not an error"), Repo->Get_Log(Query).Num(), 0);

	TestTrue(TEXT("stage the first file"), Repo->Stage({ TEXT("readme.md") }).bOk);
	gitlink::FCommitParams Commit;
	Commit.Message = TEXT("first");
	const gitlink::FResult First = Repo->Commit(Commit);
	TestTrue(FString::Printf(TEXT("the first commit succeeds: %s"), *First.ErrorMessage), First.bOk);
	TestEqual(TEXT("history now has it"), Repo->Get_Log(Query).Num(), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGitLinkTests_Edge_DetachedHead,
	"GitLink.Edge.DetachedHead",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGitLinkTests_Edge_DetachedHead::RunTest(const FString& /*Parameters*/)
{
	if (!gitlink::tests::FTempRemote::Is_GitAvailable(*this))
	{ return true; }

	gitlink::tests::FTempRemote Remote;
	const FString Clone = Remote.Clone(TEXT("detached"));
	if (!TestFalse(TEXT("clone"), Clone.IsEmpty())
		|| !TestTrue(TEXT("detach HEAD"), Remote.Git(Clone, { TEXT("checkout"), TEXT("--detach") }).IsSuccess()))
	{ return false; }

	TUniquePtr<gitlink::FRepository> Repo = gitlink::tests::FTempRemote::Open(Clone);
	if (!TestTrue(TEXT("open"), Repo.IsValid()))
	{ return false; }

	TestTrue(TEXT("no branch name on a detached HEAD"), Repo->Get_CurrentBranchName().IsEmpty());

	const gitlink::FResult Fetched = Repo->Fetch(gitlink::FFetchParams{});
	TestTrue(FString::Printf(TEXT("fetch still works (falls back to origin): %s"), *Fetched.ErrorMessage), Fetched.bOk);

	const gitlink::FResult Pulled = Repo->PullFastForward(gitlink::FFetchParams{});
	TestFalse(TEXT("pull refuses on a detached HEAD"), Pulled.bOk);
	TestTrue(FString::Printf(TEXT("and says why (got '%s')"), *Pulled.ErrorMessage), Pulled.ErrorMessage.Contains(TEXT("detached")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGitLinkTests_Edge_SpacesAndUnicodeInPaths,
	"GitLink.Edge.SpacesAndUnicodeInPaths",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGitLinkTests_Edge_SpacesAndUnicodeInPaths::RunTest(const FString& /*Parameters*/)
{
	if (!gitlink::tests::FTempRemote::Is_GitAvailable(*this))
	{ return true; }

	// Both the checkout root and the file carry a space and non-ASCII characters (a user folder like
	// "C:/Users/Zoë Müller/..." is the usual way people hit this).
	gitlink::tests::FTempRemote Remote;
	const FString Alice = Remote.Clone(TEXT("Zoë Müller project"));
	const FString Bob   = Remote.Clone(TEXT("bob"));
	if (!TestFalse(TEXT("clone into a spaced, non-ASCII folder"), Alice.IsEmpty()) || !TestFalse(TEXT("bob"), Bob.IsEmpty()))
	{ return false; }

	const FString RelPath = TEXT("Content/Häuser und Straßen/Grüße Map.txt");
	const FString AbsPath = FPaths::Combine(Alice, RelPath);
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(AbsPath), /*Tree=*/ true);
	if (!TestTrue(TEXT("write file"), FFileHelper::SaveStringToFile(FString(TEXT("ünïcödé\n")), *AbsPath,
		FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)))
	{ return false; }

	{
		TUniquePtr<gitlink::FRepository> Repo = gitlink::tests::FTempRemote::Open(Alice);
		if (!TestTrue(TEXT("open"), Repo.IsValid()))
		{ return false; }

		TestEqual(TEXT("status sees the file"), Repo->Get_Status().Num(), 1);
		TestTrue(TEXT("stage"), Repo->Stage({ RelPath }).bOk);
		gitlink::FCommitParams Commit;
		Commit.Message = TEXT("unicode");
		TestTrue(TEXT("commit"), Repo->Commit(Commit).bOk);
	}

	const gitlink::cmd::FPushOutcome Pushed = gitlink::cmd::Push(Remote.Get_Subprocess(), Alice, 60.0);
	TestTrue(FString::Printf(TEXT("push from a non-ASCII path: %s"), *Pushed.ErrorMessage), Pushed.bOk);

	TUniquePtr<gitlink::FRepository> BobRepo = gitlink::tests::FTempRemote::Open(Bob);
	if (!TestTrue(TEXT("open bob"), BobRepo.IsValid()))
	{ return false; }
	const gitlink::FResult Pulled = BobRepo->PullFastForward(gitlink::FFetchParams{});
	TestTrue(FString::Printf(TEXT("pull: %s"), *Pulled.ErrorMessage), Pulled.bOk);
	TestEqual(TEXT("the file arrives under its exact name"),
		gitlink::tests::FTempRemote::Read_File(Bob, RelPath), FString(TEXT("ünïcödé\n")));
	return true;
}

#endif  // WITH_DEV_AUTOMATION_TESTS
