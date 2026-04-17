#include "Helpers/GitLinkTests_TempRepo.h"

#include "GitLinkCore/Repository/GitLink_Repository.h"
#include "GitLinkCore/Types/GitLink_Types.h"

#include <CoreMinimal.h>
#include <Misc/AutomationTest.h>

// --------------------------------------------------------------------------------------------------------------------
// Stage / Unstage round-trip. Proves the index can be advanced and reverted without a commit in between.
// --------------------------------------------------------------------------------------------------------------------

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGitLinkTests_Repository_StageUnstage_RoundTrip,
	"GitLink.Repository.Staging.RoundTrip",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGitLinkTests_Repository_StageUnstage_RoundTrip::RunTest(const FString& /*Parameters*/)
{
	gitlink::tests::FTempRepo Repo;
	if (!TestTrue(TEXT("FTempRepo initialised"), Repo.IsValid()))
	{ return false; }

	Repo.Write_File(TEXT("one.txt"), TEXT("1"));
	Repo.Write_File(TEXT("two.txt"), TEXT("2"));

	TUniquePtr<gitlink::FRepository> Ptr = Repo.Open();
	if (!TestTrue(TEXT("FRepository::Open"), Ptr.IsValid()))
	{ return false; }

	// Both files start unstaged (untracked).
	{
		const gitlink::FStatus S = Ptr->Get_Status();
		TestEqual(TEXT("Initially 2 unstaged"), S.Unstaged.Num(), 2);
		TestEqual(TEXT("Initially 0 staged"),   S.Staged.Num(),   0);
	}

	// Stage one of them.
	const gitlink::FResult StageRes = Ptr->Stage({ TEXT("one.txt") });
	if (!TestTrue(FString::Printf(TEXT("Stage: %s"), *StageRes.ErrorMessage), StageRes.bOk))
	{ return false; }

	{
		const gitlink::FStatus S = Ptr->Get_Status();
		TestEqual(TEXT("After Stage: 1 staged"),   S.Staged.Num(),   1);
		TestEqual(TEXT("After Stage: 1 unstaged"), S.Unstaged.Num(), 1);
	}

	// Unstage it back.
	const gitlink::FResult UnstageRes = Ptr->Unstage({ TEXT("one.txt") });
	if (!TestTrue(FString::Printf(TEXT("Unstage: %s"), *UnstageRes.ErrorMessage), UnstageRes.bOk))
	{ return false; }

	{
		const gitlink::FStatus S = Ptr->Get_Status();
		TestEqual(TEXT("After Unstage: 0 staged"),   S.Staged.Num(),   0);
		TestEqual(TEXT("After Unstage: 2 unstaged"), S.Unstaged.Num(), 2);
	}

	return true;
}

// --------------------------------------------------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGitLinkTests_Repository_StageAll,
	"GitLink.Repository.Staging.StageAll",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGitLinkTests_Repository_StageAll::RunTest(const FString& /*Parameters*/)
{
	gitlink::tests::FTempRepo Repo;
	if (!TestTrue(TEXT("FTempRepo initialised"), Repo.IsValid()))
	{ return false; }

	Repo.Write_File(TEXT("one.txt"),        TEXT("1"));
	Repo.Write_File(TEXT("sub/two.txt"),    TEXT("2"));
	Repo.Write_File(TEXT("sub/deep/3.txt"), TEXT("3"));

	TUniquePtr<gitlink::FRepository> Ptr = Repo.Open();
	if (!TestTrue(TEXT("FRepository::Open"), Ptr.IsValid()))
	{ return false; }

	const gitlink::FResult Res = Ptr->StageAll();
	if (!TestTrue(FString::Printf(TEXT("StageAll: %s"), *Res.ErrorMessage), Res.bOk))
	{ return false; }

	const gitlink::FStatus Status = Ptr->Get_Status();
	TestEqual(TEXT("All 3 staged"),   Status.Staged.Num(),   3);
	TestEqual(TEXT("Nothing unstaged"), Status.Unstaged.Num(), 0);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
