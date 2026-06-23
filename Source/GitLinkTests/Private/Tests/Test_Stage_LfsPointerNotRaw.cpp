#include "Helpers/GitLinkTests_TempRepo.h"

#include "Commands/Cmd_Shared.h" // gitlink::cmd::Stage_ViaGit (from GitLink/Private, exposed via PrivateIncludePaths)
#include "GitLink_Subprocess.h"  // from GitLink/Private, exposed via PrivateIncludePaths

#include "GitLinkCore/Repository/GitLink_Repository.h"
#include "GitLinkCore/Repository/GitLink_Repository_Params.h"
#include "GitLinkCore/Types/GitLink_Types.h"

#include <CoreMinimal.h>
#include <Misc/AutomationTest.h>

// --------------------------------------------------------------------------------------------------------------------
// Regression for the v0.4.3 LFS-staging fix.
//
// The bug: GitLink staged LFS-tracked files via libgit2's git_index_add_all, which does NOT run Git's clean filter
// drivers (and GitLink registers none). So the index got the asset's RAW bytes instead of the ~130-byte LFS pointer,
// and any later commit — even one made by the git CLI, which never re-runs the clean filter on already-staged content
// — shipped that raw blob into history, defeating LFS. The fix routes staging through a `git add` subprocess
// (gitlink::cmd::Stage_ViaGit), which runs the git-lfs clean filter.
//
// This stands up a real LFS-configured temp repo and asserts, on the production helper:
//   1. Stage_ViaGit lands an LFS POINTER in the index (not raw bytes)            <- the fix
//   2. libgit2's in-memory index is synced afterwards (Reload_Index)             <- status sees the staged file
//   3. CONTRAST: libgit2 IRepository::Stage lands the RAW blob                   <- documents the bug AND proves the
//                                                                                   pointer assertion can actually fail
//
// Hermetic (FTempRepo). Skips — does not fail — when git / git-lfs isn't on PATH, matching the rest of the suite.
// --------------------------------------------------------------------------------------------------------------------

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	// The first line of every git-lfs pointer file. The STAGED blob of a correctly-cleaned LFS file is this text;
	// a raw-staged file's blob is the asset's own bytes, which won't start with it.
	const FString GLfsPointerSig = TEXT("version https://git-lfs.github.com/spec/v1");

	// Both git and git-lfs invokable from PATH? On false, the caller should skip (return true, warn) rather than
	// fail — mirrors how Test_Subprocess_RunToFile handles git absence.
	auto GitLfsAvailable(FAutomationTestBase& InTest) -> bool
	{
		FGitLink_Subprocess Probe(TEXT("git"), FString());
		if (!Probe.Run({ TEXT("--version") }).bSpawned)
		{
			InTest.AddWarning(TEXT("git not on PATH — skipping GitLink.Stage.LfsPointerNotRaw"));
			return false;
		}
		if (!Probe.Run({ TEXT("lfs"), TEXT("version") }).IsSuccess())
		{
			InTest.AddWarning(TEXT("git-lfs not available — skipping GitLink.Stage.LfsPointerNotRaw"));
			return false;
		}
		return true;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGitLinkTests_Stage_LfsPointerNotRaw,
	"GitLink.Stage.LfsPointerNotRaw",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGitLinkTests_Stage_LfsPointerNotRaw::RunTest(const FString& /*Parameters*/)
{
	if (!GitLfsAvailable(*this))
	{ return true; }

	gitlink::tests::FTempRepo Repo;
	if (!TestTrue(TEXT("FTempRepo initialised"), Repo.IsValid()))
	{ return false; }

	// Subprocess rooted at the repo working tree — same shape Stage_ViaGit gets at the outer-repo call sites
	// (empty cwd override → the subprocess's configured working dir).
	FGitLink_Subprocess Sub(TEXT("git"), Repo.Get_Root());

	// --- Setup via subprocess (keeps .git/index on-disk authoritative through setup) ----------------------------
	// Register git-lfs's clean/smudge/process filters in this repo's .git/config.
	if (!TestTrue(TEXT("git lfs install --local"),
			Sub.Run({ TEXT("lfs"), TEXT("install"), TEXT("--local") }).IsSuccess()))
	{ return false; }

	// Route *.bin through the lfs filter (git-lfs writes .gitattributes itself — avoids any hand-written BOM).
	if (!TestTrue(TEXT("git lfs track *.bin"),
			Sub.Run({ TEXT("lfs"), TEXT("track"), TEXT("*.bin") }).IsSuccess()))
	{ return false; }

	// Commit .gitattributes so the filter is unambiguously in effect (FTempRepo seeds user.name/email into
	// .git/config, which the subprocess commit reads).
	if (!TestTrue(TEXT("stage .gitattributes"), Sub.Run({ TEXT("add"), TEXT("--"), TEXT(".gitattributes") }).IsSuccess()))
	{ return false; }
	if (!TestTrue(TEXT("commit .gitattributes"), Sub.Run({ TEXT("commit"), TEXT("-m"), TEXT("attrs") }).IsSuccess()))
	{ return false; }

	// An LFS-tracked asset. Content is irrelevant — filter=lfs cleans whatever bytes into a pointer based on the
	// path attribute, not the content — so plain ASCII makes the raw-vs-pointer comparison deterministic.
	const FString RelBin = TEXT("asset.bin");
	if (!TestTrue(TEXT("write asset.bin"),
			Repo.Write_File(RelBin, TEXT("pretend this is a multi-megabyte .uasset payload"))))
	{ return false; }

	// Fresh libgit2 handle, opened AFTER setup so its index loads clean.
	TUniquePtr<gitlink::FRepository> Ptr = Repo.Open();
	if (!TestTrue(TEXT("FRepository::Open"), Ptr.IsValid()))
	{ return false; }

	// Reads the STAGED (index) blob's bytes for a path. For a correctly-cleaned LFS file that's the pointer
	// text; for a raw-staged file it's the asset's own bytes.
	auto StagedBlob = [&Sub](const FString& InRel) -> FString
	{
		return Sub.Run({ TEXT("cat-file"), TEXT("blob"), FString::Printf(TEXT(":%s"), *InRel) }).StdOut;
	};

	// --- 1. THE FIX: Stage_ViaGit must put an LFS POINTER in the index ------------------------------------------
	const gitlink::FResult StageRes = gitlink::cmd::Stage_ViaGit(*Ptr, &Sub, FString(), { RelBin });
	if (!TestTrue(FString::Printf(TEXT("Stage_ViaGit succeeded: %s"), *StageRes.ErrorMessage), StageRes.bOk))
	{ return false; }

	TestTrue(TEXT("staged blob IS an LFS pointer (the fix)"), StagedBlob(RelBin).StartsWith(GLfsPointerSig));

	// --- 2. Reload_Index synced libgit2's in-memory index: status sees the subprocess-staged file ---------------
	{
		const gitlink::FStatus Status = Ptr->Get_Status();
		bool bStaged = false;
		for (const gitlink::FFileChange& Change : Status.Staged)
		{ if (Change.Path == RelBin) { bStaged = true; break; } }
		TestTrue(TEXT("libgit2 status shows the subprocess-staged file (Reload_Index synced the index)"), bStaged);
	}

	// --- 3. CONTRAST: libgit2 IRepository::Stage raw-stages an LFS file — documents the bug AND proves the
	//        pointer assertion above discriminates. Must use a FRESH, never-staged file: git_index_add_all
	//        trusts the index stat-cache and SKIPS re-hashing a file whose size/mtime are unchanged since the
	//        last `git add`, so re-staging asset.bin would deceptively keep its pointer. A brand-new file has
	//        no index entry, forcing libgit2 to hash the working-tree bytes — which, with no LFS clean filter
	//        in libgit2, is the raw asset, not a pointer. (That stat-cache nuance is itself part of why the
	//        fix routes through `git add`: libgit2 cannot be relied on to run the filter.)
	const FString RelBin2 = TEXT("asset2.bin");
	if (TestTrue(TEXT("write asset2.bin"), Repo.Write_File(RelBin2, TEXT("a second LFS-tracked payload"))))
	{
		if (TestTrue(TEXT("libgit2 Stage (fresh file) succeeded"), Ptr->Stage({ RelBin2 }).bOk))
		{
			TestFalse(TEXT("libgit2-staged blob is RAW, not a pointer (the bug this fix removes)"),
				StagedBlob(RelBin2).StartsWith(GLfsPointerSig));
		}
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
