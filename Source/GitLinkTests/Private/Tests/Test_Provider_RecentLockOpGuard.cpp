#include "GitLink/GitLink_Provider.h"

#include <CoreMinimal.h>
#include <Misc/AutomationTest.h>
#include <Misc/Paths.h>

// --------------------------------------------------------------------------------------------------------------------
// Tests for FGitLink_Provider::Note_LocalLockOp / Was_RecentLocalLockOp — the guard that
// suppresses the LFS-replica-lag race against the dispatcher's immediate poll.
//
// Why this matters: after a successful local `git lfs lock` or `unlock`, GitHub LFS read
// replicas can take 10–100ms+ to converge. The dispatcher fires Cmd_UpdateStatus immediately
// after Cmd_CheckOut/Cmd_Revert/Cmd_CheckIn return. Without this guard a stale /locks/verify
// response either re-promotes a just-released file to Lock=Locked (sticky lock indicator
// after Revert) or demotes a just-locked file to Lock=NotLocked (CheckOut flicker). The
// guard makes local lock-state writes authoritative for ~30s.
//
// These tests pin three contracts:
//   1. After Note, Was_Recent returns true for the same path.
//   2. Path normalization matches what callers pass in — case, slashes, relative-vs-absolute
//      must all resolve to the same guard entry, otherwise Cmd_UpdateStatus's lookup would
//      silently miss.
//   3. The prune-past-64 threshold doesn't drop fresh entries (only expired ones).
//
// Time-based expiry is NOT tested — FPlatformTime::Seconds() isn't injectable. If the guard
// window ever needs to be tested, refactor Provider to take a clock function pointer.
// --------------------------------------------------------------------------------------------------------------------

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGitLinkTests_RecentLockOpGuard_AddedThenPresent,
	"GitLink.RecentLockOpGuard.AddedThenPresent",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGitLinkTests_RecentLockOpGuard_AddedThenPresent::RunTest(const FString& /*Parameters*/)
{
	FGitLink_Provider Provider;

	const FString Path = TEXT("D:/Repo/Content/Foo.uasset");

	TestFalse(TEXT("absent before Note"), Provider.Was_RecentLocalLockOp(Path));
	Provider.Note_LocalLockOp(Path);
	TestTrue(TEXT("present after Note"), Provider.Was_RecentLocalLockOp(Path));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGitLinkTests_RecentLockOpGuard_UnknownPathAbsent,
	"GitLink.RecentLockOpGuard.UnknownPathAbsent",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGitLinkTests_RecentLockOpGuard_UnknownPathAbsent::RunTest(const FString& /*Parameters*/)
{
	FGitLink_Provider Provider;
	Provider.Note_LocalLockOp(TEXT("D:/Repo/A.uasset"));

	TestFalse(TEXT("unrelated path is not guarded"),
		Provider.Was_RecentLocalLockOp(TEXT("D:/Repo/B.uasset")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGitLinkTests_RecentLockOpGuard_PathNormalization,
	"GitLink.RecentLockOpGuard.PathNormalization",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGitLinkTests_RecentLockOpGuard_PathNormalization::RunTest(const FString& /*Parameters*/)
{
	// Contract: callers pass paths in whatever form the editor / command layer happens to
	// produce. The guard MUST resolve them to the same key as Normalize_AbsolutePath
	// (ConvertRelativePathToFull + NormalizeFilename). Otherwise Cmd_UpdateStatus's
	// Was_RecentLocalLockOp(Absolute) lookup misses entries Note_LocalLockOp put in via
	// a slightly differently-cased / slashed path, and the guard silently no-ops.
	FGitLink_Provider Provider;

	const FString Canonical = TEXT("D:/Repo/Content/Foo.uasset");
	Provider.Note_LocalLockOp(Canonical);

	// Backslashes — Windows editor APIs sometimes hand these out.
	TestTrue(TEXT("backslashed form hits same entry"),
		Provider.Was_RecentLocalLockOp(TEXT("D:\\Repo\\Content\\Foo.uasset")));

	// Mixed slashes — common when paths are concatenated from two sources.
	TestTrue(TEXT("mixed-slash form hits same entry"),
		Provider.Was_RecentLocalLockOp(TEXT("D:/Repo\\Content/Foo.uasset")));

	// Sanity: the canonical form still hits.
	TestTrue(TEXT("canonical form still hits"), Provider.Was_RecentLocalLockOp(Canonical));

	// Negative: a different path in the same directory must NOT hit.
	TestFalse(TEXT("sibling path is not guarded"),
		Provider.Was_RecentLocalLockOp(TEXT("D:/Repo/Content/Bar.uasset")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGitLinkTests_RecentLockOpGuard_ReNoteRefreshes,
	"GitLink.RecentLockOpGuard.ReNoteRefreshes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGitLinkTests_RecentLockOpGuard_ReNoteRefreshes::RunTest(const FString& /*Parameters*/)
{
	// Calling Note again on an already-tracked path should keep (or refresh) the entry, not
	// drop it. Real-world scenario: a CheckOut → Revert pair on the same file fires Note
	// twice in quick succession; the second call must not erase the first guard.
	FGitLink_Provider Provider;

	const FString Path = TEXT("D:/Repo/A.uasset");
	Provider.Note_LocalLockOp(Path);
	Provider.Note_LocalLockOp(Path);
	TestTrue(TEXT("still present after re-Note"), Provider.Was_RecentLocalLockOp(Path));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGitLinkTests_RecentLockOpGuard_PrunePreservesFresh,
	"GitLink.RecentLockOpGuard.PrunePreservesFresh",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGitLinkTests_RecentLockOpGuard_PrunePreservesFresh::RunTest(const FString& /*Parameters*/)
{
	// The opportunistic prune triggers once Num() > 64 and removes only entries that are
	// already past the 30s window. With 100 fresh entries, none should expire and all must
	// remain queryable. (If the prune were over-eager — e.g. dropping based on insertion
	// order rather than age — this would catch it.)
	FGitLink_Provider Provider;

	constexpr int32 Count = 100;
	TArray<FString> Paths;
	Paths.Reserve(Count);
	for (int32 Idx = 0; Idx < Count; ++Idx)
	{ Paths.Add(FString::Printf(TEXT("D:/Repo/A_%03d.uasset"), Idx)); }

	for (const FString& P : Paths)
	{ Provider.Note_LocalLockOp(P); }

	int32 Missing = 0;
	for (const FString& P : Paths)
	{
		if (!Provider.Was_RecentLocalLockOp(P))
		{ ++Missing; }
	}
	TestEqual(TEXT("no fresh entries dropped by prune"), Missing, 0);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
