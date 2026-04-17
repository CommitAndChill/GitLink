#include "Helpers/GitLinkTests_TempRepo.h"

#include "GitLinkCore/Repository/GitLink_Repository.h"
#include "GitLinkCore/Repository/GitLink_Repository_Params.h"
#include "GitLinkCore/Types/GitLink_Types.h"

#include <CoreMinimal.h>
#include <Misc/AutomationTest.h>

// --------------------------------------------------------------------------------------------------------------------
// Tests for gitlink::FRepository::Get_Status. Covers the major buckets: untracked, staged-add, modified, deleted.
// --------------------------------------------------------------------------------------------------------------------

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	auto FindByPath(const TArray<gitlink::FFileChange>& InList, const FString& InPath) -> const gitlink::FFileChange*
	{
		for (const gitlink::FFileChange& C : InList)
		{ if (C.Path == InPath) { return &C; } }
		return nullptr;
	}
}

// --------------------------------------------------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGitLinkTests_RepositoryStatus_Untracked,
	"GitLink.Repository.Status.Untracked",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGitLinkTests_RepositoryStatus_Untracked::RunTest(const FString& /*Parameters*/)
{
	gitlink::tests::FTempRepo Repo;
	if (!TestTrue(TEXT("FTempRepo initialised"), Repo.IsValid()))
	{ return false; }

	if (!TestTrue(TEXT("Write foo.txt"), Repo.Write_File(TEXT("foo.txt"), TEXT("hello"))))
	{ return false; }

	TUniquePtr<gitlink::FRepository> Ptr = Repo.Open();
	if (!TestTrue(TEXT("FRepository::Open"), Ptr.IsValid()))
	{ return false; }

	const gitlink::FStatus Status = Ptr->Get_Status();

	TestEqual(TEXT("One unstaged entry"),    Status.Unstaged.Num(),   1);
	TestEqual(TEXT("Nothing staged"),        Status.Staged.Num(),     0);
	TestEqual(TEXT("No conflicts"),          Status.Conflicted.Num(), 0);

	if (Status.Unstaged.Num() == 1)
	{
		const gitlink::FFileChange& Change = Status.Unstaged[0];
		TestEqual(TEXT("Path matches"), Change.Path, FString(TEXT("foo.txt")));
		TestEqual(TEXT("Status is Untracked"),
			static_cast<uint8>(Change.Status),
			static_cast<uint8>(gitlink::EFileStatus::Untracked));
	}

	return true;
}

// --------------------------------------------------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGitLinkTests_RepositoryStatus_StagedAdd,
	"GitLink.Repository.Status.StagedAdd",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGitLinkTests_RepositoryStatus_StagedAdd::RunTest(const FString& /*Parameters*/)
{
	gitlink::tests::FTempRepo Repo;
	if (!TestTrue(TEXT("FTempRepo initialised"), Repo.IsValid()))
	{ return false; }

	Repo.Write_File(TEXT("a.txt"), TEXT("alpha"));

	TUniquePtr<gitlink::FRepository> Ptr = Repo.Open();
	if (!TestTrue(TEXT("FRepository::Open"), Ptr.IsValid()))
	{ return false; }

	const gitlink::FResult StageRes = Ptr->Stage({ TEXT("a.txt") });
	if (!TestTrue(FString::Printf(TEXT("Stage: %s"), *StageRes.ErrorMessage), StageRes.bOk))
	{ return false; }

	const gitlink::FStatus Status = Ptr->Get_Status();

	TestEqual(TEXT("One staged entry"), Status.Staged.Num(),   1);
	TestEqual(TEXT("No unstaged"),      Status.Unstaged.Num(), 0);

	if (const gitlink::FFileChange* Found = FindByPath(Status.Staged, TEXT("a.txt")))
	{
		TestEqual(TEXT("Staged status is Added"),
			static_cast<uint8>(Found->Status),
			static_cast<uint8>(gitlink::EFileStatus::Added));
	}
	else
	{ AddError(TEXT("a.txt not found in Staged bucket")); }

	return true;
}

// --------------------------------------------------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGitLinkTests_RepositoryStatus_Modified,
	"GitLink.Repository.Status.Modified",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGitLinkTests_RepositoryStatus_Modified::RunTest(const FString& /*Parameters*/)
{
	gitlink::tests::FTempRepo Repo;
	if (!TestTrue(TEXT("FTempRepo initialised"), Repo.IsValid()))
	{ return false; }

	Repo.Write_File(TEXT("a.txt"), TEXT("alpha"));

	TUniquePtr<gitlink::FRepository> Ptr = Repo.Open();
	if (!TestTrue(TEXT("FRepository::Open"), Ptr.IsValid()))
	{ return false; }

	// Stage + commit so the file is tracked, then modify it on disk.
	TestTrue(TEXT("Stage"),  Ptr->Stage({ TEXT("a.txt") }).bOk);

	gitlink::FCommitParams Params;
	Params.Message = TEXT("initial");
	TestTrue(TEXT("Commit"), Ptr->Commit(Params).bOk);

	Repo.Write_File(TEXT("a.txt"), TEXT("alpha-modified"));

	const gitlink::FStatus Status = Ptr->Get_Status();

	TestEqual(TEXT("One unstaged entry"), Status.Unstaged.Num(), 1);
	TestEqual(TEXT("Nothing staged"),     Status.Staged.Num(),   0);

	if (const gitlink::FFileChange* Found = FindByPath(Status.Unstaged, TEXT("a.txt")))
	{
		TestEqual(TEXT("Status is Modified"),
			static_cast<uint8>(Found->Status),
			static_cast<uint8>(gitlink::EFileStatus::Modified));
	}
	else
	{ AddError(TEXT("a.txt not found in Unstaged bucket")); }

	return true;
}

// --------------------------------------------------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGitLinkTests_RepositoryStatus_Deleted,
	"GitLink.Repository.Status.Deleted",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGitLinkTests_RepositoryStatus_Deleted::RunTest(const FString& /*Parameters*/)
{
	gitlink::tests::FTempRepo Repo;
	if (!TestTrue(TEXT("FTempRepo initialised"), Repo.IsValid()))
	{ return false; }

	Repo.Write_File(TEXT("a.txt"), TEXT("alpha"));

	TUniquePtr<gitlink::FRepository> Ptr = Repo.Open();
	if (!TestTrue(TEXT("FRepository::Open"), Ptr.IsValid()))
	{ return false; }

	TestTrue(TEXT("Stage"),  Ptr->Stage({ TEXT("a.txt") }).bOk);
	gitlink::FCommitParams Params;
	Params.Message = TEXT("initial");
	TestTrue(TEXT("Commit"), Ptr->Commit(Params).bOk);

	TestTrue(TEXT("Delete a.txt"), Repo.Delete_File(TEXT("a.txt")));

	const gitlink::FStatus Status = Ptr->Get_Status();

	TestEqual(TEXT("One unstaged entry"), Status.Unstaged.Num(), 1);
	if (const gitlink::FFileChange* Found = FindByPath(Status.Unstaged, TEXT("a.txt")))
	{
		TestEqual(TEXT("Status is Deleted"),
			static_cast<uint8>(Found->Status),
			static_cast<uint8>(gitlink::EFileStatus::Deleted));
	}
	else
	{ AddError(TEXT("a.txt not found in Unstaged bucket")); }

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
