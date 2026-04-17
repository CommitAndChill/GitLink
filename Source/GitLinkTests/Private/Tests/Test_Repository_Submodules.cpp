#include "Helpers/GitLinkTests_TempRepo.h"

#include "GitLinkCore/Repository/GitLink_Repository.h"
#include "GitLinkCore/Types/GitLink_Types.h"

#include <CoreMinimal.h>
#include <Misc/AutomationTest.h>

// --------------------------------------------------------------------------------------------------------------------
// Submodule enumeration via gitlink::op::Enumerate_SubmodulePaths. The op lives in a private header
// (GitLinkCore/Private/Operations/GitLink_Ops.h), so like GitLink_Provider.cpp we forward-declare it
// rather than reach into the private include tree.
// --------------------------------------------------------------------------------------------------------------------

namespace gitlink::op
{
	auto Enumerate_SubmodulePaths(gitlink::FRepository& InRepo) -> TArray<FString>;
}

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGitLinkTests_Repository_Submodules_NoneEmpty,
	"GitLink.Repository.Submodules.NoneWhenAbsent",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGitLinkTests_Repository_Submodules_NoneEmpty::RunTest(const FString& /*Parameters*/)
{
	gitlink::tests::FTempRepo Repo;
	if (!TestTrue(TEXT("FTempRepo initialised"), Repo.IsValid()))
	{ return false; }

	TUniquePtr<gitlink::FRepository> Ptr = Repo.Open();
	if (!TestTrue(TEXT("FRepository::Open"), Ptr.IsValid()))
	{ return false; }

	const TArray<FString> Subs = gitlink::op::Enumerate_SubmodulePaths(*Ptr);
	TestEqual(TEXT("No submodules in a fresh repo"), Subs.Num(), 0);

	return true;
}

// --------------------------------------------------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGitLinkTests_Repository_Submodules_DetectFromGitmodules,
	"GitLink.Repository.Submodules.DetectFromGitmodules",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGitLinkTests_Repository_Submodules_DetectFromGitmodules::RunTest(const FString& /*Parameters*/)
{
	gitlink::tests::FTempRepo Repo;
	if (!TestTrue(TEXT("FTempRepo initialised"), Repo.IsValid()))
	{ return false; }

	// Write a minimal .gitmodules declaring two submodules. We don't need the inner repos to exist
	// for git_submodule_foreach to report them — the file-based declaration alone is sufficient.
	const FString Gitmodules =
		TEXT("[submodule \"sub-one\"]\n")
		TEXT("\tpath = sub-one\n")
		TEXT("\turl = https://example.invalid/sub-one.git\n")
		TEXT("[submodule \"sub-two\"]\n")
		TEXT("\tpath = nested/sub-two\n")
		TEXT("\turl = https://example.invalid/sub-two.git\n");

	if (!TestTrue(TEXT("Write .gitmodules"),
		Repo.Write_File(TEXT(".gitmodules"), Gitmodules)))
	{ return false; }

	TUniquePtr<gitlink::FRepository> Ptr = Repo.Open();
	if (!TestTrue(TEXT("FRepository::Open"), Ptr.IsValid()))
	{ return false; }

	const TArray<FString> Subs = gitlink::op::Enumerate_SubmodulePaths(*Ptr);

	TestEqual(TEXT("Two submodules enumerated"), Subs.Num(), 2);

	// Paths come back with a trailing slash and forward-slashed separators.
	TestTrue(TEXT("sub-one/ present"),          Subs.Contains(FString(TEXT("sub-one/"))));
	TestTrue(TEXT("nested/sub-two/ present"),   Subs.Contains(FString(TEXT("nested/sub-two/"))));

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
