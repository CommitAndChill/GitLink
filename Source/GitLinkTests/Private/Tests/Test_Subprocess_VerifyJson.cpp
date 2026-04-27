#include "GitLink_Subprocess.h"

#include <CoreMinimal.h>
#include <Misc/AutomationTest.h>

// --------------------------------------------------------------------------------------------------------------------
// Tests for FGitLink_Subprocess::Parse_LfsVerifyJson — the parser for `git lfs locks --verify --json`.
//
// Why this matters: pre-fix, GitLink classified lock ownership by checking whether a
// remote-lock path was also in `git lfs locks --local`'s output. That turned out to be
// unreliable — `--local` returns the full cached set including other users' locks after
// any prior remote query, despite git-lfs's docs saying otherwise. The fix relies on
// `--verify --json`'s server-authoritative `ours` / `theirs` split. These tests pin the
// parser's contract so a future regression can't silently put us back on the broken path.
// --------------------------------------------------------------------------------------------------------------------

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGitLinkTests_VerifyJson_OursTheirsSplit,
	"GitLink.VerifyJson.OursTheirsSplit",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGitLinkTests_VerifyJson_OursTheirsSplit::RunTest(const FString& /*Parameters*/)
{
	// Real git-lfs 3.x output shape (formatted for readability).
	const FString Json = TEXT(R"({
		"ours": [
			{"id":"100","path":"Content/A.uasset","owner":{"name":"Alice"},"locked_at":"2026-01-01T00:00:00Z"}
		],
		"theirs": [
			{"id":"200","path":"Content/B.uasset","owner":{"name":"Bob"},  "locked_at":"2026-01-02T00:00:00Z"},
			{"id":"300","path":"Content/C.uasset","owner":{"name":"Carol"},"locked_at":"2026-01-03T00:00:00Z"}
		]
	})");

	const FGitLink_Subprocess::FLfsLocksSnapshot Snap = FGitLink_Subprocess::Parse_LfsVerifyJson(Json);

	TestTrue (TEXT("bSuccess true on parseable JSON"),       Snap.bSuccess);
	TestEqual(TEXT("AllLocks contains 3 entries (1 ours + 2 theirs)"),
		Snap.AllLocks.Num(), 3);
	TestEqual(TEXT("OursPaths contains exactly 1 entry"),    Snap.OursPaths.Num(), 1);

	// Path → owner mapping for both buckets.
	const FString* OwnerA = Snap.AllLocks.Find(TEXT("Content/A.uasset"));
	const FString* OwnerB = Snap.AllLocks.Find(TEXT("Content/B.uasset"));
	const FString* OwnerC = Snap.AllLocks.Find(TEXT("Content/C.uasset"));
	if (TestNotNull(TEXT("A.uasset present in AllLocks"), OwnerA)) { TestEqual(TEXT("A owner=Alice"), *OwnerA, FString(TEXT("Alice"))); }
	if (TestNotNull(TEXT("B.uasset present in AllLocks"), OwnerB)) { TestEqual(TEXT("B owner=Bob"),   *OwnerB, FString(TEXT("Bob")));   }
	if (TestNotNull(TEXT("C.uasset present in AllLocks"), OwnerC)) { TestEqual(TEXT("C owner=Carol"), *OwnerC, FString(TEXT("Carol"))); }

	// Ownership: only A is ours.
	TestTrue (TEXT("A in OursPaths"),     Snap.OursPaths.Contains(TEXT("Content/A.uasset")));
	TestFalse(TEXT("B NOT in OursPaths"), Snap.OursPaths.Contains(TEXT("Content/B.uasset")));
	TestFalse(TEXT("C NOT in OursPaths"), Snap.OursPaths.Contains(TEXT("Content/C.uasset")));

	return true;
}

// --------------------------------------------------------------------------------------------------------------------
// Empty result: no locks anywhere on the server. Critical that bSuccess is true here —
// callers use bSuccess to gate the "lock released" detector. If empty parsed as failure,
// every successful no-locks poll would fail to clear stale Locked entries from the cache.
// --------------------------------------------------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGitLinkTests_VerifyJson_EmptyIsSuccess,
	"GitLink.VerifyJson.EmptyIsSuccess",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGitLinkTests_VerifyJson_EmptyIsSuccess::RunTest(const FString& /*Parameters*/)
{
	const FString Empty       = TEXT(R"({"ours":[],"theirs":[]})");
	const FString MissingArrs = TEXT(R"({})");

	for (const FString& Json : { Empty, MissingArrs })
	{
		const FGitLink_Subprocess::FLfsLocksSnapshot Snap =
			FGitLink_Subprocess::Parse_LfsVerifyJson(Json);

		TestTrue (TEXT("bSuccess true on empty / missing-array JSON"), Snap.bSuccess);
		TestEqual(TEXT("AllLocks empty"),                              Snap.AllLocks.Num(), 0);
		TestEqual(TEXT("OursPaths empty"),                             Snap.OursPaths.Num(), 0);
	}

	return true;
}

// --------------------------------------------------------------------------------------------------------------------
// Malformed JSON / non-JSON output: bSuccess MUST stay false. The lock-released detector
// relies on this to skip files in repos whose poll failed — without it, a transient git-lfs
// crash or network blip would cause every cached lock in that repo to flip to NotLocked,
// and the next successful poll would re-discover them, breaking Revert UX.
// --------------------------------------------------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGitLinkTests_VerifyJson_MalformedIsFailure,
	"GitLink.VerifyJson.MalformedIsFailure",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGitLinkTests_VerifyJson_MalformedIsFailure::RunTest(const FString& /*Parameters*/)
{
	const FString Garbage     = TEXT("not json at all");
	const FString TruncatedJs = TEXT(R"({"ours":[{"id":"1","path":)");
	const FString EmptyString = TEXT("");

	for (const FString& Json : { Garbage, TruncatedJs, EmptyString })
	{
		const FGitLink_Subprocess::FLfsLocksSnapshot Snap =
			FGitLink_Subprocess::Parse_LfsVerifyJson(Json);

		TestFalse(*FString::Printf(
			TEXT("bSuccess false on malformed input '%s'"),
			*Json.Left(40)),
			Snap.bSuccess);
	}

	return true;
}

// --------------------------------------------------------------------------------------------------------------------
// Locks missing the `path` field are silently dropped — they're unusable since we key
// AllLocks / OursPaths by path. Locks missing `owner.name` are kept with empty owner
// (the editor displays a blank "checked out by" tooltip rather than crashing).
// --------------------------------------------------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGitLinkTests_VerifyJson_PartialFieldsHandled,
	"GitLink.VerifyJson.PartialFieldsHandled",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGitLinkTests_VerifyJson_PartialFieldsHandled::RunTest(const FString& /*Parameters*/)
{
	// One lock with no path (must be skipped), one with no owner (must be kept w/ empty owner),
	// and one fully populated.
	const FString Json = TEXT(R"({
		"ours": [
			{"id":"1"},
			{"id":"2","path":"Content/NoOwner.uasset"},
			{"id":"3","path":"Content/Full.uasset","owner":{"name":"Dee"}}
		],
		"theirs": []
	})");

	const FGitLink_Subprocess::FLfsLocksSnapshot Snap =
		FGitLink_Subprocess::Parse_LfsVerifyJson(Json);

	TestTrue (TEXT("bSuccess true"), Snap.bSuccess);
	TestEqual(TEXT("Two locks parsed (entry without path was skipped)"),
		Snap.AllLocks.Num(), 2);

	const FString* NoOwner = Snap.AllLocks.Find(TEXT("Content/NoOwner.uasset"));
	const FString* Full    = Snap.AllLocks.Find(TEXT("Content/Full.uasset"));
	if (TestNotNull(TEXT("NoOwner present"), NoOwner)) { TestTrue (TEXT("NoOwner owner is empty"), NoOwner->IsEmpty()); }
	if (TestNotNull(TEXT("Full present"),    Full))    { TestEqual(TEXT("Full owner=Dee"), *Full, FString(TEXT("Dee"))); }

	return true;
}

// --------------------------------------------------------------------------------------------------------------------
// Backslash-to-forward-slash normalization on paths. git-lfs always emits forward slashes
// in JSON, but we run the same replace defensively in case a future version doesn't, since
// the rest of the code keys file maps on forward-slashed paths.
// --------------------------------------------------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGitLinkTests_VerifyJson_PathNormalization,
	"GitLink.VerifyJson.PathNormalization",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGitLinkTests_VerifyJson_PathNormalization::RunTest(const FString& /*Parameters*/)
{
	const FString Json = TEXT(R"({
		"ours":   [{"id":"1","path":"Content\\A\\B.uasset","owner":{"name":"X"}}],
		"theirs": []
	})");

	const FGitLink_Subprocess::FLfsLocksSnapshot Snap =
		FGitLink_Subprocess::Parse_LfsVerifyJson(Json);

	TestTrue(TEXT("Backslashes normalized to forward slashes"),
		Snap.AllLocks.Contains(TEXT("Content/A/B.uasset")));
	TestFalse(TEXT("Backslash path not present"),
		Snap.AllLocks.Contains(TEXT("Content\\A\\B.uasset")));

	return true;
}

#endif  // WITH_DEV_AUTOMATION_TESTS
