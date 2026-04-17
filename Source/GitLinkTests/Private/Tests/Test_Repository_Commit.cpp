#include "Helpers/GitLinkTests_TempRepo.h"

#include "GitLinkCore/Repository/GitLink_Repository.h"
#include "GitLinkCore/Repository/GitLink_Repository_Params.h"
#include "GitLinkCore/Types/GitLink_Types.h"

#include <CoreMinimal.h>
#include <Misc/AutomationTest.h>

// --------------------------------------------------------------------------------------------------------------------
// Commit end-to-end: stage -> commit -> walk log back. FResult from Commit only carries bOk/ErrorMessage, so we
// verify by walking Get_Log() and inspecting HEAD's commit metadata.
// --------------------------------------------------------------------------------------------------------------------

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGitLinkTests_Repository_Commit_ThenLog,
	"GitLink.Repository.Commit.ThenLog",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGitLinkTests_Repository_Commit_ThenLog::RunTest(const FString& /*Parameters*/)
{
	gitlink::tests::FTempRepo Repo;
	if (!TestTrue(TEXT("FTempRepo initialised"), Repo.IsValid()))
	{ return false; }

	Repo.Write_File(TEXT("readme.md"), TEXT("hello world"));

	TUniquePtr<gitlink::FRepository> Ptr = Repo.Open();
	if (!TestTrue(TEXT("FRepository::Open"), Ptr.IsValid()))
	{ return false; }

	TestTrue(TEXT("Stage"), Ptr->Stage({ TEXT("readme.md") }).bOk);

	gitlink::FCommitParams Params;
	Params.Message = TEXT("initial commit");
	const gitlink::FResult CommitRes = Ptr->Commit(Params);
	if (!TestTrue(FString::Printf(TEXT("Commit: %s"), *CommitRes.ErrorMessage), CommitRes.bOk))
	{ return false; }

	// After commit, working tree is clean.
	{
		const gitlink::FStatus S = Ptr->Get_Status();
		TestTrue(TEXT("Working tree clean after commit"), S.IsEmpty());
	}

	// Log walk: exactly one commit, with our message and the seeded author.
	gitlink::FLogQuery Q;
	const TArray<gitlink::FCommit> Log = Ptr->Get_Log(Q);

	TestEqual(TEXT("Log has one commit"), Log.Num(), 1);
	if (Log.Num() >= 1)
	{
		const gitlink::FCommit& C = Log[0];
		TestEqual(TEXT("Summary matches"),       C.Summary, FString(TEXT("initial commit")));
		TestTrue (TEXT("Hash is 40 chars"),      C.Hash.Len() == 40);
		TestTrue (TEXT("ShortHash non-empty"),   !C.ShortHash.IsEmpty());
		TestEqual(TEXT("No parents (first)"),    C.ParentHashes.Num(), 0);
		TestEqual(TEXT("Author name from seed"), C.Author.Name, FString(TEXT("GitLinkTests")));
	}

	return true;
}

// --------------------------------------------------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGitLinkTests_Repository_Commit_WithPathFilter,
	"GitLink.Repository.Commit.LogPathFilter",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGitLinkTests_Repository_Commit_WithPathFilter::RunTest(const FString& /*Parameters*/)
{
	gitlink::tests::FTempRepo Repo;
	if (!TestTrue(TEXT("FTempRepo initialised"), Repo.IsValid()))
	{ return false; }

	// Commit A only touches a.txt; commit B only touches b.txt.
	Repo.Write_File(TEXT("a.txt"), TEXT("a1"));

	TUniquePtr<gitlink::FRepository> Ptr = Repo.Open();
	if (!TestTrue(TEXT("FRepository::Open"), Ptr.IsValid()))
	{ return false; }

	TestTrue(TEXT("Stage A"), Ptr->Stage({ TEXT("a.txt") }).bOk);
	{
		gitlink::FCommitParams P; P.Message = TEXT("A");
		TestTrue(TEXT("Commit A"), Ptr->Commit(P).bOk);
	}

	Repo.Write_File(TEXT("b.txt"), TEXT("b1"));
	TestTrue(TEXT("Stage B"), Ptr->Stage({ TEXT("b.txt") }).bOk);
	{
		gitlink::FCommitParams P; P.Message = TEXT("B");
		TestTrue(TEXT("Commit B"), Ptr->Commit(P).bOk);
	}

	// Unfiltered log = 2 commits.
	{
		gitlink::FLogQuery Q;
		TestEqual(TEXT("Unfiltered log"), Ptr->Get_Log(Q).Num(), 2);
	}

	// Filtered on a.txt = 1 commit (the A commit).
	{
		gitlink::FLogQuery Q;
		Q.PathFilter = TEXT("a.txt");
		const TArray<gitlink::FCommit> Log = Ptr->Get_Log(Q);
		TestEqual(TEXT("a.txt-filtered log has 1 commit"), Log.Num(), 1);
		if (Log.Num() == 1)
		{ TestEqual(TEXT("Summary is A"), Log[0].Summary, FString(TEXT("A"))); }
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
