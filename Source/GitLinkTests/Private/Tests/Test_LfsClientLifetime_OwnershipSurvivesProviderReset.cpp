#include "GitLink/GitLink_Provider.h"
#include "GitLink_LfsHttpClient.h"
#include "GitLink_Subprocess.h"

#include <CoreMinimal.h>
#include <Misc/AutomationTest.h>
#include <Templates/SharedPointer.h>

// --------------------------------------------------------------------------------------------------------------------
// Contract tests for the lifetime model of FGitLink_LfsHttpClient and FGitLink_Subprocess.
//
// Background: a teammate's editor crashed in `TScopeLock<FWindowsRecursiveMutex>::TScopeLock`
// inside `FGitLink_LfsHttpClient::Has_LfsUrl`, called from a `ParallelFor_BoundedConcurrency`
// worker in `Cmd_UpdateStatus`. Root cause was that the provider exposed `_LfsHttpClient` /
// `_Subprocess` via raw-pointer accessors over `TUniquePtr` members. An editor `Init(force=true)`
// (which the editor fires multiple times during startup — Pitfall #4 in GitLink/CLAUDE.md) called
// `FGitLink_Provider::Close()` which `.Reset()`'d the unique pointers WHILE a worker thread was
// mid-deref of the raw pointer it had just fetched from `Get_LfsHttpClient()`. The mutex inside
// the freed object then crashed.
//
// Fix (v0.3.6): promote the members to `TSharedPtr`, return `TSharedPtr` from the accessors,
// and have commands that fan out across threads snapshot the handle locally before the
// ParallelFor. The worker lambdas dereference the local snapshot, which keeps the underlying
// object alive for the full parallel-for duration via refcount semantics.
//
// These contract tests lock in:
//   1. The accessor return type. A future refactor that regresses it back to a raw pointer
//      breaks the build, surfacing the regression at compile time rather than as a crash.
//   2. A live snapshot keeps the object alive past the primary owner's Reset(), and the
//      mutex inside is still acquirable (the crash signature was a destroyed mutex).
//   3. The object is reaped when the last reference drops.
//
// Companion stress test: Test_LfsClientLifetime_RacesAgainstReset.cpp (runtime repro).
// --------------------------------------------------------------------------------------------------------------------

#if WITH_DEV_AUTOMATION_TESTS

// --------------------------------------------------------------------------------------------------------------------
// 1. Type contract — compile-time guard.
//
// If anyone changes Get_LfsHttpClient() / Get_Subprocess() back to returning a raw pointer, the
// build breaks here with a clear message pointing at this file and the version log entry.
// --------------------------------------------------------------------------------------------------------------------
static_assert(
	std::is_same_v<
		decltype(std::declval<const FGitLink_Provider&>().Get_LfsHttpClient()),
		TSharedPtr<FGitLink_LfsHttpClient>>,
	"FGitLink_Provider::Get_LfsHttpClient() must return TSharedPtr<FGitLink_LfsHttpClient> — "
	"raw pointer caused a TScopeLock use-after-free; see v0.3.6 in Plugins/GitLink/CLAUDE.md.");

static_assert(
	std::is_same_v<
		decltype(std::declval<const FGitLink_Provider&>().Get_Subprocess()),
		TSharedPtr<FGitLink_Subprocess>>,
	"FGitLink_Provider::Get_Subprocess() must return TSharedPtr<FGitLink_Subprocess> — "
	"raw pointer was the same hazard shape as the LFS client; see v0.3.6 in Plugins/GitLink/CLAUDE.md.");

// --------------------------------------------------------------------------------------------------------------------
// 2. Lifetime contract — a snapshot keeps the object alive past the owner's Reset().
//
// Mirrors what Cmd_UpdateStatus does: it captures a TSharedPtr<FGitLink_LfsHttpClient> local before
// the ParallelFor, then worker threads deref the local. If the provider's Reset() runs concurrently
// the worker must still see a live mutex.
// --------------------------------------------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGitLinkTests_LfsClientLifetime_SnapshotKeepsObjectAlive,
	"GitLink.LfsClientLifetime.SnapshotKeepsObjectAlive",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGitLinkTests_LfsClientLifetime_SnapshotKeepsObjectAlive::RunTest(const FString& /*Parameters*/)
{
	// Stand up a subprocess + client the same way Provider::CheckRepositoryStatus does — no actual
	// repo on disk required, the client's ctor just stashes the subprocess reference.
	TSharedPtr<FGitLink_Subprocess>    Subprocess = MakeShared<FGitLink_Subprocess>(TEXT("git"), TEXT("."));
	TSharedPtr<FGitLink_LfsHttpClient> Client     = MakeShared<FGitLink_LfsHttpClient>(*Subprocess);

	// Weak reference lets us observe the destructor without forcing it to fire.
	TWeakPtr<FGitLink_LfsHttpClient> WeakClient = Client;

	// Mirror the Cmd_UpdateStatus pattern: a local snapshot taken before the parallel-for body.
	// Note: production code in Cmd_UpdateStatus (file ~L386) snapshots BOTH the client AND the
	// subprocess locally before the ParallelFor — the client only holds `FGitLink_Subprocess&`
	// (a raw reference, no refcount), so a worker that wants to use both must snapshot both.
	// This test exercises only the client snapshot since the crash site (`Has_LfsUrl`) doesn't
	// touch the subprocess; the sibling test `SubprocessSnapshotKeepsAlive` below covers the
	// symmetric subprocess case. We intentionally keep `Subprocess` alive here so the client's
	// `Has_LfsUrl` call below has a live referent for its `_Subprocess` member, which would
	// also be true in production (Cmd_UpdateStatus's local subprocess snapshot keeps it alive).
	TSharedPtr<FGitLink_LfsHttpClient> Snapshot = Client;

	// Provider::Close() equivalent — the primary owner drops its reference. Without TSharedPtr
	// semantics this is exactly the moment the bug manifested.
	Client.Reset();

	TestTrue(TEXT("Snapshot keeps the LFS client alive past the owner's reset"), WeakClient.IsValid());

	// The mutex inside the client is the actual crash site — acquire it to prove the memory is
	// still valid. `Has_LfsUrl` takes the lock and returns false for an unresolved repo.
	const bool bHas = Snapshot->Has_LfsUrl(TEXT("D:/nonexistent-repo-root"));
	TestFalse(TEXT("Empty endpoints map returns false (mutex is live + map is empty)"), bHas);

	// Now drop the snapshot — the destructor should run here, not before.
	Snapshot.Reset();
	TestFalse(TEXT("Client is destroyed when the last reference drops"), WeakClient.IsValid());

	return true;
}

// --------------------------------------------------------------------------------------------------------------------
// 3. The subprocess hazard had the same shape — verify the same contract on it.
// --------------------------------------------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGitLinkTests_LfsClientLifetime_SubprocessSnapshotKeepsAlive,
	"GitLink.LfsClientLifetime.SubprocessSnapshotKeepsAlive",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGitLinkTests_LfsClientLifetime_SubprocessSnapshotKeepsAlive::RunTest(const FString& /*Parameters*/)
{
	TSharedPtr<FGitLink_Subprocess> Owner = MakeShared<FGitLink_Subprocess>(TEXT("git"), TEXT("."));
	TWeakPtr<FGitLink_Subprocess>   Weak  = Owner;

	TSharedPtr<FGitLink_Subprocess> Snapshot = Owner;
	Owner.Reset();

	TestTrue(TEXT("Snapshot keeps the subprocess alive past the owner's reset"), Weak.IsValid());

	// IsValid() reads only const config members — safe smoke that the object's vtable + members
	// are reachable.
	(void)Snapshot->IsValid();

	Snapshot.Reset();
	TestFalse(TEXT("Subprocess destroyed when the last reference drops"), Weak.IsValid());

	return true;
}

#endif  // WITH_DEV_AUTOMATION_TESTS
