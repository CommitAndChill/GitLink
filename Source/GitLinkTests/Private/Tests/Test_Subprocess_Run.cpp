#include "Helpers/GitLinkTests_TempRepo.h"

#include "GitLink_Subprocess.h" // from Plugins/GitLink/Source/GitLink/Private (exposed via PrivateIncludePaths)

#include "GitLinkCore/Repository/GitLink_Repository.h"
#include "GitLinkCore/Repository/GitLink_Repository_Params.h"
#include "GitLinkCore/Types/GitLink_Types.h"

#include <CoreMinimal.h>
#include <HAL/PlatformTime.h>
#include <Misc/AutomationTest.h>
#include <Misc/FileHelper.h>
#include <Misc/Paths.h>

#if PLATFORM_WINDOWS
#include <Windows/AllowWindowsPlatformTypes.h>
#include <Windows/WindowsHWrapper.h>
#include <Windows/HideWindowsPlatformTypes.h>
#endif

// --------------------------------------------------------------------------------------------------------------------
// Contract tests for FGitLink_Subprocess::Run — the call under every `git lfs lock` / `unlock`, `git add`,
// hook-running commit and LFS lock-sweep fallback.
//
// Why this file exists: Run used to sit on FPlatformProcess::ExecProcess, whose Windows implementation waits for
// the child with `do { ReadPipes(); Sleep(0); } while (IsProcRunning)` — a busy-wait. Every live child pinned one
// core on the calling thread for its whole lifetime. The 120 s background sweep ran ~15 `git lfs locks --verify`
// children of ~1.7 s each, 8 at a time: measured ~13 CPU-seconds per sweep inside the editor process, visible as
// a periodic CPU spike. DoesNotBusyWaitWhileChildRuns is the regression test for that; the rest pin the parts of
// the ExecProcess contract callers depend on, so replacing the wait could not silently change them:
//
//   - stdout and stderr stay SEPARATE (callers parse StdOut as JSON; git-lfs writes warnings to stderr),
//   - the exit code is the child's,
//   - output larger than the pipe buffer is drained while the child runs (no deadlock, nothing dropped),
//   - a binary that cannot be launched reports bSpawned=false and still counts as a spawn attempt.
//
// Hermetic: FTempRepo only. Skips when git is not on PATH.
// --------------------------------------------------------------------------------------------------------------------

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	auto GitAvailable_ForRun(FAutomationTestBase& InTest) -> bool
	{
		FGitLink_Subprocess Probe(TEXT("git"), FString());
		const FGitLink_SubprocessResult R = Probe.Run({ TEXT("--version") });
		if (!R.bSpawned)
		{
			InTest.AddWarning(TEXT("git.exe not on PATH — skipping FGitLink_Subprocess::Run integration test"));
			return false;
		}
		return R.IsSuccess();
	}

#if PLATFORM_WINDOWS
	// Kernel + user CPU time consumed so far by the calling thread, in seconds.
	auto Get_CurrentThreadCpuSeconds() -> double
	{
		FILETIME Creation, Exit, Kernel, User;
		if (!::GetThreadTimes(::GetCurrentThread(), &Creation, &Exit, &Kernel, &User))
		{ return 0.0; }

		const auto ToSeconds = [](const FILETIME& InTime) -> double
		{
			const uint64 Ticks = (static_cast<uint64>(InTime.dwHighDateTime) << 32) | InTime.dwLowDateTime;
			return static_cast<double>(Ticks) / 1.0e7;
		};
		return ToSeconds(Kernel) + ToSeconds(User);
	}
#endif
}

// --------------------------------------------------------------------------------------------------------------------
// 1. The regression test. A child that does nothing for ~2 s must cost the WAITING thread almost no CPU.
//    `ping -n 3` is two one-second waits between three echoes; cmd.exe is bound as the "git binary" so the test
//    depends on nothing but Windows itself. A busy-wait burns ~100% of wall time here; a sleeping wait a few
//    percent. The 25% bound leaves an order of magnitude on both sides.
// --------------------------------------------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGitLinkTests_Subprocess_Run_DoesNotBusyWaitWhileChildRuns,
	"GitLink.Subprocess.Run.DoesNotBusyWaitWhileChildRuns",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGitLinkTests_Subprocess_Run_DoesNotBusyWaitWhileChildRuns::RunTest(const FString& /*Parameters*/)
{
#if PLATFORM_WINDOWS
	FGitLink_Subprocess Slow(TEXT("cmd.exe"), FString());

	const double WallStart = FPlatformTime::Seconds();
	const double CpuStart  = Get_CurrentThreadCpuSeconds();

	const FGitLink_SubprocessResult R = Slow.Run({ TEXT("/c"), TEXT("ping"), TEXT("-n"), TEXT("3"), TEXT("127.0.0.1") });

	const double CpuSec  = Get_CurrentThreadCpuSeconds() - CpuStart;
	const double WallSec = FPlatformTime::Seconds() - WallStart;

	if (!TestTrue(TEXT("slow child ran to a clean exit"), R.IsSuccess()))
	{ return false; }

	// Guards the measurement itself: if the child returned at once there was nothing to wait on and the ratio
	// below would say nothing.
	if (!TestTrue(FString::Printf(TEXT("child ran long enough to measure (%.2f s wall)"), WallSec), WallSec > 1.0))
	{ return false; }

	TestTrue(
		FString::Printf(TEXT("waiting thread used %.3f s CPU over %.2f s wall (%.0f%%) — must stay under 25%%"),
			CpuSec, WallSec, 100.0 * CpuSec / WallSec),
		CpuSec < 0.25 * WallSec);
#endif
	return true;
}

// --------------------------------------------------------------------------------------------------------------------
// 2. stdout and stderr are captured separately, and the exit code is the child's.
// --------------------------------------------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGitLinkTests_Subprocess_Run_SeparatesStdOutFromStdErr,
	"GitLink.Subprocess.Run.SeparatesStdOutFromStdErr",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGitLinkTests_Subprocess_Run_SeparatesStdOutFromStdErr::RunTest(const FString& /*Parameters*/)
{
	if (!GitAvailable_ForRun(*this))
	{ return true; }

	gitlink::tests::FTempRepo Repo;
	if (!TestTrue(TEXT("FTempRepo initialised"), Repo.IsValid()))
	{ return false; }

	FGitLink_Subprocess Sub(TEXT("git"), FString());

	// Success: text on stdout only.
	const FGitLink_SubprocessResult Ok = Sub.Run({ TEXT("--version") });
	TestTrue (TEXT("--version spawned"),            Ok.bSpawned);
	TestEqual(TEXT("--version exit code"),          Ok.ExitCode, 0);
	TestTrue (TEXT("--version wrote to stdout"),    Ok.StdOut.StartsWith(TEXT("git version")));
	TestTrue (TEXT("--version left stderr empty"),  Ok.StdErr.IsEmpty());

	// Failure: git's message on stderr only, and its exit code (128 = fatal) passed through.
	const FGitLink_SubprocessResult Bad = Sub.Run(
		{ TEXT("-C"), Repo.Get_Root(), TEXT("rev-parse"), TEXT("--verify"), TEXT("refs/heads/gitlink-tests-no-such-ref") });
	TestTrue (TEXT("rev-parse spawned"),              Bad.bSpawned);
	TestEqual(TEXT("rev-parse exit code"),            Bad.ExitCode, 128);
	TestFalse(TEXT("rev-parse is not a success"),     Bad.IsSuccess());
	TestTrue (TEXT("rev-parse left stdout empty"),    Bad.StdOut.TrimStartAndEnd().IsEmpty());
	TestTrue (TEXT("rev-parse wrote fatal to stderr"), Bad.StdErr.Contains(TEXT("fatal")));

	// The working-directory override must reach the child: the same query without `-C`.
	const FGitLink_SubprocessResult TopLevel = Sub.Run({ TEXT("rev-parse"), TEXT("--is-inside-work-tree") }, Repo.Get_Root());
	TestTrue (TEXT("cwd override spawned"), TopLevel.IsSuccess());
	TestEqual(TEXT("cwd override ran inside the temp repo"), TopLevel.StdOut.TrimStartAndEnd(), FString(TEXT("true")));

	return true;
}

// --------------------------------------------------------------------------------------------------------------------
// 3. Output far larger than a pipe buffer. The child blocks on a full pipe until the parent reads, so a wait that
//    did not drain while the child runs would hang here forever, and one that drained only once would truncate.
// --------------------------------------------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGitLinkTests_Subprocess_Run_DrainsLargeOutput,
	"GitLink.Subprocess.Run.DrainsLargeOutput",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGitLinkTests_Subprocess_Run_DrainsLargeOutput::RunTest(const FString& /*Parameters*/)
{
	if (!GitAvailable_ForRun(*this))
	{ return true; }

	gitlink::tests::FTempRepo Repo;
	if (!TestTrue(TEXT("FTempRepo initialised"), Repo.IsValid()))
	{ return false; }

	// 1 MiB of ASCII, LF-only lines, so neither line-ending conversion nor UTF-8 decoding can change its length.
	constexpr int32 NumBytes = 1024 * 1024;
	FString Expected;
	Expected.Reserve(NumBytes);
	for (int32 Idx = 0; Idx < NumBytes; ++Idx)
	{ Expected += (Idx % 64 == 63) ? TEXT('\n') : static_cast<TCHAR>(TEXT('a') + (Idx % 26)); }

	const FString Abs = Repo.Get_Root() / TEXT("big.txt");
	if (!TestTrue(TEXT("fixture written"),
		FFileHelper::SaveStringToFile(Expected, *Abs, FFileHelper::EEncodingOptions::ForceAnsi)))
	{ return false; }

	{
		TUniquePtr<gitlink::FRepository> Ptr = Repo.Open();
		if (!TestTrue(TEXT("repo opened"), Ptr.IsValid()))
		{ return false; }
		if (!TestTrue(TEXT("fixture staged"), Ptr->Stage({ TEXT("big.txt") }).bOk))
		{ return false; }

		gitlink::FCommitParams Params;
		Params.Message = TEXT("fixture");
		if (!TestTrue(TEXT("fixture committed"), Ptr->Commit(Params).bOk))
		{ return false; }
	}

	FGitLink_Subprocess Sub(TEXT("git"), FString());
	const FGitLink_SubprocessResult R = Sub.Run({ TEXT("-C"), Repo.Get_Root(), TEXT("show"), TEXT("HEAD:big.txt") });

	if (!TestTrue(TEXT("git show succeeded"), R.IsSuccess()))
	{ return false; }
	TestEqual(TEXT("every byte arrived"), R.StdOut.Len(), NumBytes);
	TestTrue (TEXT("content is intact"),  R.StdOut.Equals(Expected, ESearchCase::CaseSensitive));
	TestTrue (TEXT("stderr stayed empty"), R.StdErr.IsEmpty());
	return true;
}

// --------------------------------------------------------------------------------------------------------------------
// 4. A binary that cannot be launched: no result, no exit code to trust, one spawn attempt counted, and the
//    Warning other tests (Test_LfsEndpoint_ResolvesWithoutSubprocess) count by its exact text.
// --------------------------------------------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGitLinkTests_Subprocess_Run_ReportsSpawnFailure,
	"GitLink.Subprocess.Run.ReportsSpawnFailure",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGitLinkTests_Subprocess_Run_ReportsSpawnFailure::RunTest(const FString& /*Parameters*/)
{
	AddExpectedMessagePlain(TEXT("Subprocess: failed to spawn"), ELogVerbosity::Warning,
		EAutomationExpectedMessageFlags::Contains, 1);

	FGitLink_Subprocess Sub(TEXT("D:/__gitlink_tests_no_such_git_binary__/git.exe"), FString());
	const FGitLink_SubprocessResult R = Sub.Run({ TEXT("--version") });

	TestFalse(TEXT("not spawned"),          R.bSpawned);
	TestFalse(TEXT("not a success"),        R.IsSuccess());
	TestTrue (TEXT("no stdout"),            R.StdOut.IsEmpty());
	TestEqual(TEXT("one spawn attempt"),    Sub.Get_SpawnAttemptCount(), 1);
	TestEqual(TEXT("error text names the launch failure"),
		R.Get_CombinedError(), FString(TEXT("(git could not be launched)")));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
