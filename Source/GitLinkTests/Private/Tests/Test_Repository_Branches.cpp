#include "Helpers/GitLinkTests_TempRepo.h"

#include "GitLinkCore/Repository/GitLink_Repository.h"
#include "GitLinkCore/Repository/GitLink_Repository_Params.h"
#include "GitLinkCore/Types/GitLink_Types.h"

#include <CoreMinimal.h>
#include <Misc/AutomationTest.h>

// --------------------------------------------------------------------------------------------------------------------
// Branch enumeration. Before any commit, HEAD is unborn — Get_Branches returns empty. After a commit, the initial
// branch becomes concrete and shows up with bIsHead=true. We do NOT assert on the branch NAME because libgit2's
// initial-branch default depends on init.defaultBranch in the host git config ("master" by libgit2 default,
// often "main" when the user has configured git ≥ 2.28).
// --------------------------------------------------------------------------------------------------------------------

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGitLinkTests_Repository_Branches_HeadVisibleAfterCommit,
	"GitLink.Repository.Branches.HeadVisibleAfterCommit",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGitLinkTests_Repository_Branches_HeadVisibleAfterCommit::RunTest(const FString& /*Parameters*/)
{
	gitlink::tests::FTempRepo Repo;
	if (!TestTrue(TEXT("FTempRepo initialised"), Repo.IsValid()))
	{ return false; }

	Repo.Write_File(TEXT("f.txt"), TEXT("x"));

	TUniquePtr<gitlink::FRepository> Ptr = Repo.Open();
	if (!TestTrue(TEXT("FRepository::Open"), Ptr.IsValid()))
	{ return false; }

	// Pre-commit: unborn HEAD.
	{
		const TArray<gitlink::FBranch> B = Ptr->Get_Branches();
		TestEqual(TEXT("No branches pre-commit"), B.Num(), 0);
	}

	TestTrue(TEXT("Stage"), Ptr->Stage({ TEXT("f.txt") }).bOk);
	{
		gitlink::FCommitParams P; P.Message = TEXT("first");
		TestTrue(TEXT("Commit"), Ptr->Commit(P).bOk);
	}

	const FString Head = Ptr->Get_CurrentBranchName();
	TestFalse(TEXT("Get_CurrentBranchName non-empty after commit"), Head.IsEmpty());

	const TArray<gitlink::FBranch> Branches = Ptr->Get_Branches();
	TestTrue(TEXT("At least one branch"), Branches.Num() >= 1);

	bool bFoundHead = false;
	for (const gitlink::FBranch& B : Branches)
	{
		if (B.bIsHead && B.Name == Head)
		{
			bFoundHead = true;
			TestFalse(TEXT("TipHash non-empty"), B.TipHash.IsEmpty());
			TestFalse(TEXT("HEAD branch is not marked remote"), B.bIsRemote);
			break;
		}
	}
	TestTrue(TEXT("HEAD branch located in Get_Branches"), bFoundHead);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
