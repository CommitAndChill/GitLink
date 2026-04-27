#include "Commands/Cmd_Shared.h"

#include <CoreMinimal.h>
#include <HAL/PlatformMisc.h>
#include <HAL/PlatformProcess.h>
#include <Misc/AutomationTest.h>

#include <atomic>

// --------------------------------------------------------------------------------------------------------------------
// Tests for gitlink::cmd::ParallelFor_BoundedConcurrency — the helper that caps the number
// of concurrent cross-repo workers so the 30s background poll doesn't saturate every CPU
// core when fanning out to 30+ submodules at once.
//
// Correctness invariants:
//   1. Every index in [0, Count) is visited exactly once.
//   2. At no point are more than Max tasks running simultaneously.
//   3. Empty / negative inputs are no-ops (don't crash).
//
// (1) and (3) are deterministic. (2) we approximate by tracking a peak in-flight counter
// across all worker threads; tasks deliberately sleep briefly to give the cap a chance to
// be exceeded if it's broken.
// --------------------------------------------------------------------------------------------------------------------

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGitLinkTests_BoundedConcurrency_VisitsAllIndicesExactlyOnce,
	"GitLink.BoundedConcurrency.VisitsAllIndicesExactlyOnce",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGitLinkTests_BoundedConcurrency_VisitsAllIndicesExactlyOnce::RunTest(const FString& /*Parameters*/)
{
	constexpr int32 Count = 50;
	constexpr int32 Max   = 8;

	TArray<std::atomic<int32>> VisitCount;
	VisitCount.SetNum(Count);
	for (int32 Idx = 0; Idx < Count; ++Idx)
	{ VisitCount[Idx].store(0); }

	gitlink::cmd::ParallelFor_BoundedConcurrency(Count, Max, [&](int32 Idx)
	{
		VisitCount[Idx].fetch_add(1, std::memory_order_relaxed);
	});

	for (int32 Idx = 0; Idx < Count; ++Idx)
	{
		const int32 N = VisitCount[Idx].load(std::memory_order_relaxed);
		TestEqual(*FString::Printf(TEXT("Index %d visited exactly once"), Idx), N, 1);
	}
	return true;
}

// --------------------------------------------------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGitLinkTests_BoundedConcurrency_RespectsCap,
	"GitLink.BoundedConcurrency.RespectsCap",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGitLinkTests_BoundedConcurrency_RespectsCap::RunTest(const FString& /*Parameters*/)
{
	constexpr int32 Count = 32;
	constexpr int32 Max   = 4;

	std::atomic<int32> InFlight{0};
	std::atomic<int32> PeakInFlight{0};

	gitlink::cmd::ParallelFor_BoundedConcurrency(Count, Max, [&](int32 /*Idx*/)
	{
		const int32 Current = InFlight.fetch_add(1, std::memory_order_acq_rel) + 1;

		// Update the peak with a CAS loop so multiple racing workers don't lose updates.
		int32 OldPeak = PeakInFlight.load(std::memory_order_relaxed);
		while (Current > OldPeak &&
			!PeakInFlight.compare_exchange_weak(OldPeak, Current,
				std::memory_order_release, std::memory_order_relaxed))
		{
			// retry
		}

		// Hold the slot briefly so concurrent workers actually overlap and the peak can
		// reach the cap. Without a sleep, fast-running tasks complete before their peers
		// even start, masking a broken cap.
		FPlatformProcess::Sleep(0.01f);

		InFlight.fetch_sub(1, std::memory_order_acq_rel);
	});

	const int32 Peak = PeakInFlight.load(std::memory_order_relaxed);
	TestTrue(*FString::Printf(TEXT("Peak in-flight (%d) <= Max (%d)"), Peak, Max),
		Peak <= Max);

	// Also confirm the cap was actually exercised — if Peak is 1 the test isn't proving
	// anything because the work didn't overlap. Expect at least 2 concurrent workers given
	// the sleep above, except on single-core runners where the thread pool degenerates.
	if (FPlatformMisc::NumberOfCores() > 1)
	{
		TestTrue(*FString::Printf(TEXT("Peak in-flight (%d) >= 2 — cap was exercised"), Peak),
			Peak >= 2);
	}

	return true;
}

// --------------------------------------------------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGitLinkTests_BoundedConcurrency_EmptyAndNegativeAreNoops,
	"GitLink.BoundedConcurrency.EmptyAndNegativeAreNoops",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGitLinkTests_BoundedConcurrency_EmptyAndNegativeAreNoops::RunTest(const FString& /*Parameters*/)
{
	std::atomic<int32> Calls{0};
	auto Work = [&](int32 /*Idx*/) { Calls.fetch_add(1, std::memory_order_relaxed); };

	gitlink::cmd::ParallelFor_BoundedConcurrency(0,    8, Work);
	gitlink::cmd::ParallelFor_BoundedConcurrency(-5,   8, Work);

	TestEqual(TEXT("Zero calls when Count <= 0"),
		Calls.load(std::memory_order_relaxed), 0);

	return true;
}

// --------------------------------------------------------------------------------------------------------------------
// Defensive: when InMaxConcurrent <= 0 the helper falls back to "no cap" (treats it as
// equal to Count). All indices must still be visited; we don't assert the cap behavior in
// this case because there isn't one.
// --------------------------------------------------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGitLinkTests_BoundedConcurrency_ZeroMaxFallsBackToUncapped,
	"GitLink.BoundedConcurrency.ZeroMaxFallsBackToUncapped",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGitLinkTests_BoundedConcurrency_ZeroMaxFallsBackToUncapped::RunTest(const FString& /*Parameters*/)
{
	constexpr int32 Count = 16;
	std::atomic<int32> Calls{0};

	gitlink::cmd::ParallelFor_BoundedConcurrency(Count, 0, [&](int32 /*Idx*/)
	{
		Calls.fetch_add(1, std::memory_order_relaxed);
	});

	TestEqual(TEXT("All indices visited even when Max=0 (cap fallback)"),
		Calls.load(std::memory_order_relaxed), Count);

	return true;
}

#endif  // WITH_DEV_AUTOMATION_TESTS
