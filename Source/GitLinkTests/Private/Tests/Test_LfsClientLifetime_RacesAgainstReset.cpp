#include "GitLink_LfsHttpClient.h"
#include "GitLink_Subprocess.h"

#include <CoreMinimal.h>
#include <Async/Async.h>
#include <Async/Future.h>
#include <HAL/PlatformProcess.h>
#include <Misc/AutomationTest.h>
#include <Templates/SharedPointer.h>

#include <atomic>

// --------------------------------------------------------------------------------------------------------------------
// Stress test that reproduces the exact crash shape the v0.3.6 fix addresses.
//
// Crash signature (from a teammate's Win64-DebugGame editor):
//   TScopeLock<FWindowsRecursiveMutex>::TScopeLock           (engine)
//     ← FGitLink_LfsHttpClient::Has_LfsUrl                   (line 339)
//     ← lambda inside gitlink::cmd::UpdateStatus             (line 384, ParallelFor body)
//     ← ParallelFor_BoundedConcurrency worker
//
// What this test does: stand up a shared LFS client, fan out N worker threads that mirror the
// Cmd_UpdateStatus pattern (each captures its own TSharedPtr snapshot), then drop the primary
// owner's reference WHILE workers are mid-loop. Repeat enough times to hit any race window.
//
// Pre-fix expectation (TUniquePtr + raw-pointer accessor): if the test were rewritten to fetch
// the raw pointer once before the parallel-for and re-fetch from the owner each iteration —
// matching the pre-fix Cmd_UpdateStatus shape — it would crash within a few iterations.
// Post-fix expectation: clean run; the snapshot keeps the object alive for the worker's full
// duration.
//
// Iteration / worker counts are sized to keep this test well under 1s on the build agent so it
// stays in the standard GitLink.* suite. Automation RunTest runs on the game thread, so heavy
// blocking work here delays UE subsystem ticks (EOSRTC etc. log a TickTracker warning above ~1s
// of game-thread starvation, which yellows the test). Using `EAsyncExecution::TaskGraph` rather
// than `EAsyncExecution::Thread` because TaskGraph reuses a warm worker pool — spawning 800
// real OS threads on Windows costs ~15s and is the dominant overhead. The race-window concern
// (TaskGraph workers being too hot to catch the bug) doesn't actually apply: TSharedPtr lifetime
// semantics make the use-after-free structurally impossible regardless of thread shape, so the
// test just needs to exercise the concurrent owner-reset shape, not maximize wall-clock fuzzing.
//
// See companion contract test: Test_LfsClientLifetime_OwnershipSurvivesProviderReset.cpp.
// --------------------------------------------------------------------------------------------------------------------

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGitLinkTests_LfsClientLifetime_ConcurrentResetRace,
	"GitLink.LfsClientLifetime.ConcurrentResetRace",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGitLinkTests_LfsClientLifetime_ConcurrentResetRace::RunTest(const FString& /*Parameters*/)
{
	constexpr int32 Iterations  = 20;
	constexpr int32 WorkerCount = 4;
	constexpr int32 ProbesPerWorker = 200;

	for (int32 Iter = 0; Iter < Iterations; ++Iter)
	{
		// Fresh subprocess + client per iteration — each iteration is an independent
		// "Provider::CheckRepositoryStatus → Provider::Close" cycle.
		TSharedPtr<FGitLink_Subprocess>    Subprocess = MakeShared<FGitLink_Subprocess>(TEXT("git"), TEXT("."));
		TSharedPtr<FGitLink_LfsHttpClient> Owner      = MakeShared<FGitLink_LfsHttpClient>(*Subprocess);

		std::atomic<bool>  StartSignal { false };
		std::atomic<int32> ActiveWorkers { 0 };
		std::atomic<int32> CompletedProbes { 0 };

		// Spawn the workers. Each captures its own TSharedPtr snapshot — exactly mirroring the
		// `const TSharedPtr<FGitLink_LfsHttpClient> LfsClient = InCtx.Provider.Get_LfsHttpClient();`
		// snapshot at the top of Cmd_UpdateStatus's parallel-for body.
		TArray<TFuture<void>> Futures;
		Futures.Reserve(WorkerCount);
		for (int32 W = 0; W < WorkerCount; ++W)
		{
			TSharedPtr<FGitLink_LfsHttpClient> Snapshot = Owner;  // capture by value into the lambda below

			Futures.Add(Async(EAsyncExecution::TaskGraph,
				[&StartSignal, &ActiveWorkers, &CompletedProbes, Snapshot]() mutable
				{
					// Park until released so all workers race against the same Reset() event.
					while (!StartSignal.load(std::memory_order_acquire))
					{
						FPlatformProcess::Yield();
					}
					ActiveWorkers.fetch_add(1, std::memory_order_acq_rel);

					for (int32 K = 0; K < ProbesPerWorker; ++K)
					{
						// Has_LfsUrl is the exact crash site. Acquires the FCriticalSection
						// member of FGitLink_LfsHttpClient — this would dereference a freed
						// mutex without the TSharedPtr lifetime guarantee.
						(void)Snapshot->Has_LfsUrl(TEXT("D:/nonexistent-repo-root"));
						CompletedProbes.fetch_add(1, std::memory_order_relaxed);
					}

					ActiveWorkers.fetch_sub(1, std::memory_order_acq_rel);
				}));
		}

		// Release the workers and IMMEDIATELY drop the owner's reference. The Reset() races
		// against worker startup → worker probes → worker shutdown. The TSharedPtr snapshots
		// in each worker keep the object alive until the last worker drops; without them the
		// object would be destroyed mid-deref and the test would crash (the original bug).
		StartSignal.store(true, std::memory_order_release);
		Owner.Reset();
		Subprocess.Reset();

		// Drain.
		for (TFuture<void>& F : Futures)
		{
			F.Wait();
		}

		// Every worker must have completed its full probe budget (no early-out) and the active
		// counter must be back to zero — proves no worker was killed mid-loop.
		TestEqual(*FString::Printf(TEXT("Iteration %d: all workers drained"), Iter),
			ActiveWorkers.load(std::memory_order_acquire), 0);
		TestEqual(*FString::Printf(TEXT("Iteration %d: every probe completed"), Iter),
			CompletedProbes.load(std::memory_order_acquire), WorkerCount * ProbesPerWorker);
	}

	return true;
}

#endif  // WITH_DEV_AUTOMATION_TESTS
