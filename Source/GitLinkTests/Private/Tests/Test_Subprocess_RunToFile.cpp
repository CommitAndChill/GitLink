#include "Helpers/GitLinkTests_TempRepo.h"

#include "GitLink_Subprocess.h" // from Plugins/GitLink/Source/GitLink/Private (exposed via PrivateIncludePaths)

#include "GitLinkCore/Repository/GitLink_Repository.h"
#include "GitLinkCore/Repository/GitLink_Repository_Params.h"
#include "GitLinkCore/Types/GitLink_Types.h"

#include <CoreMinimal.h>
#include <HAL/FileManager.h>
#include <HAL/PlatformFileManager.h>
#include <Misc/AutomationTest.h>
#include <Misc/FileHelper.h>
#include <Misc/Paths.h>

// --------------------------------------------------------------------------------------------------------------------
// Integration tests for FGitLink_Subprocess::RunToFile — the helper used by FGitLink_Revision::Get to extract a
// file's binary contents at a specific commit via `git cat-file --filters`. Regression coverage for:
//
//   1. Pipe wiring: CreateProc's PipeWriteChild is the handle the CHILD writes to (its stdout). A prior swap with
//      PipeReadChild caused git to run, exit 0, and produce 0 bytes — RoundTripBinary catches any recurrence.
//   2. Silent-success-on-empty-stdout: RunToFile must return false when git exits 0 but writes no output. Without
//      this guard a pipe-wiring regression would masquerade as success.
//   3. Non-zero exit: RunToFile must return false when git fails (e.g. path not in commit).
//
// Hermetic: uses FTempRepo; no reliance on the user's repos. Gracefully skips when git.exe isn't on PATH.
// Repo routing uses the `-C <abs-path>` git arg rather than the constructor's working-dir parameter — this keeps
// the call shape identical to the submodule-diff suite in Test_Submodule_Diff.cpp, where `-C` is how we point git
// at the submodule root.
// --------------------------------------------------------------------------------------------------------------------

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	// Probe whether git.exe is invokable from the current PATH. Returns true if `git --version` spawned; on false,
	// a test should skip (return true without asserting) rather than fail, matching how libgit2 absence is handled.
	auto GitAvailable(FAutomationTestBase& InTest) -> bool
	{
		FGitLink_Subprocess Probe(TEXT("git"), FString());
		const FGitLink_SubprocessResult R = Probe.Run({ TEXT("--version") });
		if (!R.bSpawned)
		{
			InTest.AddWarning(TEXT("git.exe not on PATH — skipping FGitLink_Subprocess::RunToFile integration test"));
			return false;
		}
		return R.IsSuccess();
	}

	// Builds an absolute path under Saved/GitLinkTests/Subprocess/<guid>/<Filename> for the output file. Parent
	// directory is created so RunToFile's own SaveArrayToFile doesn't fail on missing dirs.
	auto MakeScratchOutputPath(const FString& InFilename) -> FString
	{
		const FString Dir = FPaths::ProjectSavedDir() / TEXT("GitLinkTests") / TEXT("Subprocess")
			/ FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphens);
		IPlatformFile& PF = FPlatformFileManager::Get().GetPlatformFile();
		PF.CreateDirectoryTree(*Dir);
		return Dir / InFilename;
	}

	// Commits a single file with the given bytes to InRepo and returns true on success.
	auto CommitFixture(gitlink::tests::FTempRepo& InRepo,
	                   const FString& InRelPath,
	                   const TArray<uint8>& InBytes,
	                   FAutomationTestBase& InTest) -> bool
	{
		const FString Abs = InRepo.Get_Root() / InRelPath;
		if (!FFileHelper::SaveArrayToFile(InBytes, *Abs))
		{ InTest.AddError(FString::Printf(TEXT("Failed to write fixture %s"), *Abs)); return false; }

		TUniquePtr<gitlink::FRepository> Ptr = InRepo.Open();
		if (!Ptr.IsValid())
		{ InTest.AddError(TEXT("FRepository::Open failed")); return false; }

		if (!Ptr->Stage({ InRelPath }).bOk)
		{ InTest.AddError(TEXT("Stage failed")); return false; }

		gitlink::FCommitParams Params;
		Params.Message = TEXT("fixture");
		const gitlink::FResult Cr = Ptr->Commit(Params);
		if (!Cr.bOk)
		{ InTest.AddError(FString::Printf(TEXT("Commit failed: %s"), *Cr.ErrorMessage)); return false; }

		return true;
	}
}

// --------------------------------------------------------------------------------------------------------------------
// Test 1 — positive round-trip. Fixture is every byte value 0x00..0xFF repeated, ~4 KiB, which reliably trips git's
// binary-auto-detection (presence of NUL bytes) so no smudge/clean filter mangles the content. A regression that
// swaps PipeWriteChild/PipeReadChild will trip either the zero-byte guard (RunToFile returns false) or, if that
// guard is also broken, the size/content assertions below.
// --------------------------------------------------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGitLinkTests_Subprocess_RunToFile_RoundTripBinary,
	"GitLink.Subprocess.RunToFile.RoundTripBinary",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGitLinkTests_Subprocess_RunToFile_RoundTripBinary::RunTest(const FString& /*Parameters*/)
{
	if (!GitAvailable(*this))
	{ return true; }

	gitlink::tests::FTempRepo Repo;
	if (!TestTrue(TEXT("FTempRepo initialised"), Repo.IsValid()))
	{ return false; }

	// Every byte value 0..255, repeated 16 times = 4096 bytes. NULs guarantee git treats this as binary.
	constexpr int32 Repeats = 16;
	constexpr int32 NumBytes = 256 * Repeats;
	TArray<uint8> Pattern;
	Pattern.SetNumUninitialized(NumBytes);
	for (int32 i = 0; i < NumBytes; ++i)
	{ Pattern[i] = static_cast<uint8>(i & 0xFF); }

	if (!CommitFixture(Repo, TEXT("binfile"), Pattern, *this))
	{ return false; }

	const FString OutPath = MakeScratchOutputPath(TEXT("out.bin"));

	FGitLink_Subprocess Sub(TEXT("git"), FString());
	const bool bOk = Sub.RunToFile(
		{ TEXT("-C"), Repo.Get_Root(), TEXT("cat-file"), TEXT("--filters"), TEXT("HEAD:binfile") },
		OutPath);
	if (!TestTrue(TEXT("RunToFile returned true"), bOk))
	{ return false; }

	IFileManager& FM = IFileManager::Get();
	TestTrue (TEXT("Output file exists"),      FM.FileExists(*OutPath));
	TestEqual(TEXT("Output size matches input"), FM.FileSize(*OutPath), static_cast<int64>(NumBytes));

	TArray<uint8> RoundTripped;
	if (TestTrue(TEXT("LoadFileToArray"), FFileHelper::LoadFileToArray(RoundTripped, *OutPath)))
	{ TestTrue(TEXT("Bytes match fixture"), RoundTripped == Pattern); }

	FM.Delete(*OutPath, /*RequireExists=*/ false, /*EvenReadOnly=*/ true);
	return true;
}

// --------------------------------------------------------------------------------------------------------------------
// Test 2 — `git update-ref <ref> <hash>` exits 0 with zero stdout. RunToFile must refuse this: a bug that ignored
// the empty-output case let the pipe-swap regression go undetected for months. This is the PURE 0-byte guard —
// git really does succeed, it just doesn't emit anything to capture.
// --------------------------------------------------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGitLinkTests_Subprocess_RunToFile_FailsOnZeroByteSuccess,
	"GitLink.Subprocess.RunToFile.FailsOnZeroByteSuccess",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGitLinkTests_Subprocess_RunToFile_FailsOnZeroByteSuccess::RunTest(const FString& /*Parameters*/)
{
	if (!GitAvailable(*this))
	{ return true; }

	gitlink::tests::FTempRepo Repo;
	if (!TestTrue(TEXT("FTempRepo initialised"), Repo.IsValid()))
	{ return false; }

	TArray<uint8> Seed; Seed.Add('x');
	if (!CommitFixture(Repo, TEXT("readme.md"), Seed, *this))
	{ return false; }

	// Resolve HEAD's 40-char SHA (needed as the rvalue for update-ref).
	TUniquePtr<gitlink::FRepository> Ptr = Repo.Open();
	const TArray<gitlink::FCommit> Log = Ptr.IsValid() ? Ptr->Get_Log({}) : TArray<gitlink::FCommit>{};
	if (!TestTrue(TEXT("HEAD hash resolved"), Log.Num() >= 1 && Log[0].Hash.Len() == 40))
	{ return false; }
	const FString Hash = Log[0].Hash;

	const FString OutPath = MakeScratchOutputPath(TEXT("empty.bin"));

	// Suppress the expected warning RunToFile emits when it rejects exit-0-empty-stdout, so this expected
	// behaviour doesn't show up as noise in the automation report.
	AddExpectedMessagePlain(
		TEXT("RunToFile: git exited 0 but produced no output"),
		ELogVerbosity::Warning,
		EAutomationExpectedMessageFlags::Contains,
		1);

	FGitLink_Subprocess Sub(TEXT("git"), FString());
	const bool bOk = Sub.RunToFile(
		{ TEXT("-C"), Repo.Get_Root(), TEXT("update-ref"), TEXT("refs/tests/zero-byte-guard"), Hash },
		OutPath);

	TestFalse(TEXT("RunToFile must reject exit-0-with-empty-stdout"), bOk);

	const bool bAbsentOrEmpty = !IFileManager::Get().FileExists(*OutPath)
		|| IFileManager::Get().FileSize(*OutPath) == 0;
	TestTrue(TEXT("Output file is absent or empty"), bAbsentOrEmpty);

	IFileManager::Get().Delete(*OutPath, /*RequireExists=*/ false, /*EvenReadOnly=*/ true);
	return true;
}

// --------------------------------------------------------------------------------------------------------------------
// Test 3 — `HEAD:does-not-exist` makes git exit non-zero. RunToFile must return false and must not leave a stale
// output file behind for callers (e.g. FGitLink_Revision::Get) to mistake as a valid extraction.
// --------------------------------------------------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGitLinkTests_Subprocess_RunToFile_FailsOnMissingPath,
	"GitLink.Subprocess.RunToFile.FailsOnMissingPath",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGitLinkTests_Subprocess_RunToFile_FailsOnMissingPath::RunTest(const FString& /*Parameters*/)
{
	if (!GitAvailable(*this))
	{ return true; }

	gitlink::tests::FTempRepo Repo;
	if (!TestTrue(TEXT("FTempRepo initialised"), Repo.IsValid()))
	{ return false; }

	TArray<uint8> Seed; Seed.Add('x');
	if (!CommitFixture(Repo, TEXT("readme.md"), Seed, *this))
	{ return false; }

	const FString OutPath = MakeScratchOutputPath(TEXT("nope.bin"));

	// Suppress the expected non-zero-exit warning so the automation report stays clean.
	AddExpectedMessagePlain(
		TEXT("RunToFile: git exited with code"),
		ELogVerbosity::Warning,
		EAutomationExpectedMessageFlags::Contains,
		1);

	FGitLink_Subprocess Sub(TEXT("git"), FString());
	const bool bOk = Sub.RunToFile(
		{ TEXT("-C"), Repo.Get_Root(), TEXT("cat-file"), TEXT("--filters"), TEXT("HEAD:does-not-exist") },
		OutPath);

	TestFalse(TEXT("RunToFile must fail on non-zero git exit"), bOk);
	TestFalse(TEXT("No output file left behind"), IFileManager::Get().FileExists(*OutPath));

	IFileManager::Get().Delete(*OutPath, /*RequireExists=*/ false, /*EvenReadOnly=*/ true);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
