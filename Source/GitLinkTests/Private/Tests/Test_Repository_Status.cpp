#include "Helpers/GitLinkTests_TempRepo.h"

#include "GitLinkCore/Repository/GitLink_Repository.h"
#include "GitLinkCore/Types/GitLink_Types.h"

#include <CoreMinimal.h>
#include <Misc/AutomationTest.h>

// --------------------------------------------------------------------------------------------------------------------
// Walking-skeleton suite for Task 5. Proves the GitLinkTests module is wired, that FTempRepo can stand up
// an ephemeral repo via libgit2, and that FRepository::Get_Status reports an untracked file correctly.
// --------------------------------------------------------------------------------------------------------------------

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGitLinkTests_RepositoryStatus_Untracked,
	"GitLink.Repository.Status.Untracked",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGitLinkTests_RepositoryStatus_Untracked::RunTest(const FString& /*Parameters*/)
{
	gitlink::tests::FTempRepo Repo;
	if (!TestTrue(TEXT("FTempRepo initialised"), Repo.IsValid()))
	{ return false; }

	if (!TestTrue(TEXT("Write_File produced foo.txt"),
		Repo.Write_File(TEXT("foo.txt"), TEXT("hello"))))
	{ return false; }

	TUniquePtr<gitlink::FRepository> Ptr = Repo.Open();
	if (!TestTrue(TEXT("FRepository::Open succeeded"), Ptr.IsValid()))
	{ return false; }

	const gitlink::FStatus Status = Ptr->Get_Status();

	TestEqual(TEXT("One unstaged entry"),     Status.Unstaged.Num(), 1);
	TestEqual(TEXT("Nothing staged"),         Status.Staged.Num(),   0);
	TestEqual(TEXT("No conflicts"),           Status.Conflicted.Num(), 0);

	if (Status.Unstaged.Num() == 1)
	{
		const gitlink::FFileChange& Change = Status.Unstaged[0];
		TestEqual(TEXT("Path matches"),       Change.Path, FString(TEXT("foo.txt")));
		TestEqual(TEXT("Status is Untracked"),
			static_cast<uint8>(Change.Status),
			static_cast<uint8>(gitlink::EFileStatus::Untracked));
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
