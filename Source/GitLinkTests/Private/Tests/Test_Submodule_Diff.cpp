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
// Submodule-file content extraction.
//
// In production, FGitLink_Revision::Get (GitLink_Revision.cpp:37-42) detects when the requested file lives inside a
// submodule and swaps the working directory to the submodule root before calling FGitLink_Subprocess::RunToFile.
// At the subprocess boundary this manifests as passing `-C <submodule-abs-path>` as the first git arg so that
// `cat-file --filters HEAD:<path>` resolves against the submodule's object DB rather than the parent's.
//
// This suite proves that the repo-routing mechanic works end-to-end: commit a binary fixture inside a nested repo
// (standing in for the submodule), declare it in the outer repo's `.gitmodules` using the same pattern as
// Test_Repository_Submodules.cpp, then call RunToFile with `-C <inner-root>` and verify the bytes round-trip.
//
// We don't invoke the full FGitLink_Provider / FGitLink_Revision stack — that's editor-context heavy and out of
// scope per the test spec. Covering RunToFile + `-C` routing exercises the same binary extraction code path.
// --------------------------------------------------------------------------------------------------------------------

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	auto GitAvailable_Sub(FAutomationTestBase& InTest) -> bool
	{
		FGitLink_Subprocess Probe(TEXT("git"), FString());
		const FGitLink_SubprocessResult R = Probe.Run({ TEXT("--version") });
		if (!R.bSpawned)
		{
			InTest.AddWarning(TEXT("git.exe not on PATH — skipping submodule-diff test"));
			return false;
		}
		return R.IsSuccess();
	}

	auto MakeScratchOutputPath_Sub(const FString& InFilename) -> FString
	{
		const FString Dir = FPaths::ProjectSavedDir() / TEXT("GitLinkTests") / TEXT("SubmoduleDiff")
			/ FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphens);
		IPlatformFile& PF = FPlatformFileManager::Get().GetPlatformFile();
		PF.CreateDirectoryTree(*Dir);
		return Dir / InFilename;
	}
}

// --------------------------------------------------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGitLinkTests_Submodule_Diff_ExtractsBinaryFromSubmoduleRoot,
	"GitLink.Submodule.Diff.ExtractsBinaryFromSubmoduleRoot",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGitLinkTests_Submodule_Diff_ExtractsBinaryFromSubmoduleRoot::RunTest(const FString& /*Parameters*/)
{
	if (!GitAvailable_Sub(*this))
	{ return true; }

	// Parent repo — declares a submodule path in .gitmodules to match the pattern used by
	// Test_Repository_Submodules.cpp, even though this test exercises file extraction against the inner repo
	// directly rather than via libgit2's submodule APIs.
	gitlink::tests::FTempRepo Outer;
	if (!TestTrue(TEXT("Outer FTempRepo initialised"), Outer.IsValid()))
	{ return false; }

	const FString Gitmodules =
		TEXT("[submodule \"sub\"]\n")
		TEXT("\tpath = sub\n")
		TEXT("\turl = https://example.invalid/sub.git\n");
	TestTrue(TEXT("Write outer .gitmodules"), Outer.Write_File(TEXT(".gitmodules"), Gitmodules));
	TestTrue(TEXT("Write outer readme"),      Outer.Write_File(TEXT("readme.md"), TEXT("outer")));
	{
		TUniquePtr<gitlink::FRepository> OuterPtr = Outer.Open();
		if (!TestTrue(TEXT("Outer FRepository::Open"), OuterPtr.IsValid()))
		{ return false; }
		TestTrue(TEXT("Stage outer"), OuterPtr->Stage({ TEXT(".gitmodules"), TEXT("readme.md") }).bOk);
		gitlink::FCommitParams P; P.Message = TEXT("outer initial");
		TestTrue(TEXT("Commit outer"), OuterPtr->Commit(P).bOk);
	}

	// Inner repo — standing in for the submodule. An independent FTempRepo is hermetic, self-cleaning, and
	// produces the same `-C <inner-root>` behavior that RunToFile sees from FGitLink_Revision::Get's working-dir
	// swap in production.
	gitlink::tests::FTempRepo Inner;
	if (!TestTrue(TEXT("Inner FTempRepo initialised"), Inner.IsValid()))
	{ return false; }

	// Binary fixture: every byte 0..255 x 8 = 2048 bytes. NULs trip git's binary auto-detection so no smudge
	// filter (e.g. core.autocrlf) mangles the stream.
	constexpr int32 Repeats = 8;
	constexpr int32 NumBytes = 256 * Repeats;
	TArray<uint8> Pattern;
	Pattern.SetNumUninitialized(NumBytes);
	for (int32 i = 0; i < NumBytes; ++i)
	{ Pattern[i] = static_cast<uint8>(i & 0xFF); }

	const FString InnerFixture = Inner.Get_Root() / TEXT("asset.bin");
	if (!TestTrue(TEXT("Write inner fixture"), FFileHelper::SaveArrayToFile(Pattern, *InnerFixture)))
	{ return false; }

	{
		TUniquePtr<gitlink::FRepository> Ptr = Inner.Open();
		if (!TestTrue(TEXT("Inner FRepository::Open"), Ptr.IsValid()))
		{ return false; }

		TestTrue(TEXT("Stage in inner"), Ptr->Stage({ TEXT("asset.bin") }).bOk);

		gitlink::FCommitParams P;
		P.Message = TEXT("sub fixture");
		const gitlink::FResult Cr = Ptr->Commit(P);
		if (!TestTrue(FString::Printf(TEXT("Commit in inner: %s"), *Cr.ErrorMessage), Cr.bOk))
		{ return false; }
	}

	// The core assertion: RunToFile, routed at the inner repo via `-C`, extracts the fixture bytes unchanged.
	// This is the same code path FGitLink_Revision::Get takes when the requested file lives in a submodule.
	const FString OutPath = MakeScratchOutputPath_Sub(TEXT("sub_out.bin"));

	FGitLink_Subprocess Sub(TEXT("git"), FString());
	const bool bOk = Sub.RunToFile(
		{ TEXT("-C"), Inner.Get_Root(), TEXT("cat-file"), TEXT("--filters"), TEXT("HEAD:asset.bin") },
		OutPath);
	if (!TestTrue(TEXT("RunToFile extracted from submodule root"), bOk))
	{ return false; }

	IFileManager& FM = IFileManager::Get();
	TestTrue (TEXT("Output exists"),        FM.FileExists(*OutPath));
	TestEqual(TEXT("Output size matches"),  FM.FileSize(*OutPath), static_cast<int64>(NumBytes));

	TArray<uint8> RoundTripped;
	if (TestTrue(TEXT("LoadFileToArray"), FFileHelper::LoadFileToArray(RoundTripped, *OutPath)))
	{ TestTrue(TEXT("Bytes match inner fixture"), RoundTripped == Pattern); }

	// Sanity: the SAME args against the OUTER repo must fail — asset.bin was never committed there. Proves the
	// `-C` routing is actually what's making the positive case work, not coincidental PATH/cwd behavior.
	{
		// Expected non-zero-exit warning from the negative probe — keep it out of the automation report.
		AddExpectedMessagePlain(
			TEXT("RunToFile: git exited with code"),
			ELogVerbosity::Warning,
			EAutomationExpectedMessageFlags::Contains,
			1);

		const FString OuterOut = MakeScratchOutputPath_Sub(TEXT("outer_out.bin"));
		const bool bOuter = Sub.RunToFile(
			{ TEXT("-C"), Outer.Get_Root(), TEXT("cat-file"), TEXT("--filters"), TEXT("HEAD:asset.bin") },
			OuterOut);
		TestFalse(TEXT("Outer repo has no asset.bin — must fail"), bOuter);
		FM.Delete(*OuterOut, /*RequireExists=*/ false, /*EvenReadOnly=*/ true);
	}

	FM.Delete(*OutPath, /*RequireExists=*/ false, /*EvenReadOnly=*/ true);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
