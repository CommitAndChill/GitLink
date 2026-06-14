#include "Helpers/GitLinkTests_TempRepo.h"

#include "GitLinkCore/Repository/GitLink_Repository.h"
#include "GitLinkCore/Types/GitLink_Types.h"

#include <CoreMinimal.h>
#include <HAL/PlatformFileManager.h>
#include <Misc/AutomationTest.h>
#include <Misc/Paths.h>

// --------------------------------------------------------------------------------------------------------------------
// Regression for the v0.4.1 read-only-delete fix.
//
// BusterBlock's .uasset/.umap are git-lfs `lockable`, so git-lfs keeps them read-only on disk
// until the user holds the lock. A plain content-browser delete never runs the checkout (lock)
// path, so the working file is still read-only when Cmd_Delete tries to remove it — and on
// Windows IPlatformFile::DeleteFile is a bare DeleteFileW that fails ACCESS_DENIED on read-only
// files. Pre-fix that left the file on disk with nothing staged; the fix clears the read-only
// attribute before unlinking.
//
// This pins the whole chain at the repo/op level — FRepository::Stage is the exact code path
// Cmd_Delete stages deletions through (git_index_add_all):
//   1. (Windows) a committed, read-only working file cannot be removed by a raw DeleteFile,
//   2. after clearing read-only it CAN be removed, and
//   3. staging the removed path records a staged DELETION (not a no-op, not the old content).
// --------------------------------------------------------------------------------------------------------------------

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGitLinkTests_Delete_ReadOnlyFileStagesDeletion,
	"GitLink.Delete.ReadOnlyFileStagesDeletion",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGitLinkTests_Delete_ReadOnlyFileStagesDeletion::RunTest(const FString& /*Parameters*/)
{
	gitlink::tests::FTempRepo Repo;
	if (!TestTrue(TEXT("FTempRepo initialised"), Repo.IsValid()))
	{ return false; }

	// Commit a tracked file so a deletion is meaningful (it exists in HEAD).
	const FString RelPath = TEXT("asset.uasset");
	Repo.Write_File(RelPath, TEXT("payload"));

	TUniquePtr<gitlink::FRepository> Ptr = Repo.Open();
	if (!TestTrue(TEXT("FRepository::Open"), Ptr.IsValid()))
	{ return false; }

	if (!TestTrue(TEXT("Stage initial"), Ptr->Stage({ RelPath }).bOk))
	{ return false; }
	{
		gitlink::FCommitParams Params;
		Params.Message = TEXT("seed");
		if (!TestTrue(TEXT("Commit initial"), Ptr->Commit(Params).bOk))
		{ return false; }
	}

	const FString AbsPath = FPaths::Combine(Repo.Get_Root(), RelPath);
	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();

	// Simulate git-lfs's lockable behaviour: the working file is read-only until locked.
	if (!TestTrue(TEXT("SetReadOnly(true)"), PlatformFile.SetReadOnly(*AbsPath, true)))
	{ return false; }
	TestTrue(TEXT("File reports read-only"), PlatformFile.IsReadOnly(*AbsPath));

#if PLATFORM_WINDOWS
	// The premise of the bug: on Windows a raw delete of a read-only file fails, which is why
	// Cmd_Delete used to leave the file on disk and stage nothing. (POSIX unlink ignores the
	// file's own permission bits, so this only holds on Windows — hence the guard.)
	TestFalse(TEXT("Raw DeleteFile fails while read-only (Windows)"), PlatformFile.DeleteFile(*AbsPath));
	TestTrue (TEXT("File still present after the failed delete"),     PlatformFile.FileExists(*AbsPath));
#endif

	// The fix: clear read-only, THEN delete — now the unlink succeeds on every platform.
	TestTrue (TEXT("SetReadOnly(false)"),               PlatformFile.SetReadOnly(*AbsPath, false));
	TestTrue (TEXT("DeleteFile succeeds after clearing"), PlatformFile.DeleteFile(*AbsPath));
	TestFalse(TEXT("File gone from disk"),              PlatformFile.FileExists(*AbsPath));

	// Staging the removed path must record a DELETION (git_index_add_all picks up removals).
	if (!TestTrue(TEXT("Stage deletion"), Ptr->Stage({ RelPath }).bOk))
	{ return false; }

	const gitlink::FStatus Status = Ptr->Get_Status();
	bool bStagedDeletion = false;
	for (const gitlink::FFileChange& Change : Status.Staged)
	{
		if (Change.Path == RelPath && Change.Status == gitlink::EFileStatus::Deleted)
		{ bStagedDeletion = true; break; }
	}
	TestTrue(TEXT("Status shows a staged deletion for the path"), bStagedDeletion);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
