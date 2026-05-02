#include "GitLink_Subprocess.h"

#include <CoreMinimal.h>
#include <Misc/AutomationTest.h>

// --------------------------------------------------------------------------------------------------------------------
// Tests for FGitLink_Subprocess::Parse_LocksByPathJson — the single-file response parser used by
// the Stage B `GET /locks?path=...` probe.
//
// Why this matters: the single-file path is the freshness driver for foreground UX (focus,
// asset-opened, package-dirty, pre-checkout). A bug here would manifest as either (a) silently
// classifying every probe as NotLocked (false negative — user gets a confusing checkout failure)
// or (b) missing the owner.name and reporting LockedOther for our own locks (false positive —
// user can't check out their own files). Both UX failure modes are subtle enough that they
// can ship.
// --------------------------------------------------------------------------------------------------------------------

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGitLinkTests_LocksByPathJson_HappyPath,
	"GitLink.LocksByPathJson.HappyPath",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGitLinkTests_LocksByPathJson_HappyPath::RunTest(const FString& /*Parameters*/)
{
	// Real `GET /locks?path=...` response shape (LFS spec).
	const FString Json = TEXT(R"({
		"locks": [
			{"id":"500","path":"Content/Foo.uasset","owner":{"name":"alice"},"locked_at":"2026-04-01T00:00:00Z"}
		]
	})");

	const FGitLink_Subprocess::FLfsSingleLockParse Out =
		FGitLink_Subprocess::Parse_LocksByPathJson(Json);

	TestTrue(TEXT("lock found"), Out.bFoundLock);
	TestEqual(TEXT("path"),  Out.Path,      FString(TEXT("Content/Foo.uasset")));
	TestEqual(TEXT("owner"), Out.OwnerName, FString(TEXT("alice")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGitLinkTests_LocksByPathJson_EmptyArray,
	"GitLink.LocksByPathJson.EmptyArray",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGitLinkTests_LocksByPathJson_EmptyArray::RunTest(const FString& /*Parameters*/)
{
	// "locks" present but empty — the LFS spec says this means "no locks for this path". The
	// caller distinguishes this from a transport failure via the HTTP status code, not the
	// parser; the parser just reports bFoundLock=false.
	const FString Json = TEXT(R"({"locks":[]})");

	const FGitLink_Subprocess::FLfsSingleLockParse Out =
		FGitLink_Subprocess::Parse_LocksByPathJson(Json);

	TestFalse(TEXT("lock not found"), Out.bFoundLock);
	TestTrue (TEXT("path empty"),     Out.Path.IsEmpty());
	TestTrue (TEXT("owner empty"),    Out.OwnerName.IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGitLinkTests_LocksByPathJson_FirstWins,
	"GitLink.LocksByPathJson.FirstWins",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGitLinkTests_LocksByPathJson_FirstWins::RunTest(const FString& /*Parameters*/)
{
	// Servers SHOULDN'T return multiple matches for a path-filtered query, but if they do, the
	// parser must pick deterministically. Convention: first valid entry wins (matches what
	// /locks/verify would do — it doesn't deduplicate either).
	const FString Json = TEXT(R"({
		"locks": [
			{"id":"1","path":"Content/A.uasset","owner":{"name":"alice"}},
			{"id":"2","path":"Content/B.uasset","owner":{"name":"bob"}}
		]
	})");

	const FGitLink_Subprocess::FLfsSingleLockParse Out =
		FGitLink_Subprocess::Parse_LocksByPathJson(Json);

	TestTrue (TEXT("found"), Out.bFoundLock);
	TestEqual(TEXT("first path"),  Out.Path,      FString(TEXT("Content/A.uasset")));
	TestEqual(TEXT("first owner"), Out.OwnerName, FString(TEXT("alice")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGitLinkTests_LocksByPathJson_MissingOwner,
	"GitLink.LocksByPathJson.MissingOwner",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGitLinkTests_LocksByPathJson_MissingOwner::RunTest(const FString& /*Parameters*/)
{
	// Lock entry missing the owner object — LFS spec says owner is optional. Path still wins;
	// classification falls back to LockedOther because we can't compare an empty owner to the
	// cached LFS identity. The single-file caller handles that case.
	const FString Json = TEXT(R"({
		"locks": [
			{"id":"1","path":"Content/Bar.uasset","locked_at":"2026-04-01T00:00:00Z"}
		]
	})");

	const FGitLink_Subprocess::FLfsSingleLockParse Out =
		FGitLink_Subprocess::Parse_LocksByPathJson(Json);

	TestTrue (TEXT("found"),       Out.bFoundLock);
	TestEqual(TEXT("path"),        Out.Path,      FString(TEXT("Content/Bar.uasset")));
	TestTrue (TEXT("owner empty"), Out.OwnerName.IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGitLinkTests_LocksByPathJson_BackslashesNormalized,
	"GitLink.LocksByPathJson.BackslashesNormalized",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGitLinkTests_LocksByPathJson_BackslashesNormalized::RunTest(const FString& /*Parameters*/)
{
	// Some LFS servers (or buggy clients writing to the lock store) emit Windows-style paths.
	// The parser MUST forward-slash them to match the rest of the cache key normalization
	// (BuildAbsolutePath, Cmd_UpdateStatus all assume forward slashes).
	const FString Json = TEXT(R"({"locks":[{"id":"1","path":"Content\\Sub\\C.uasset","owner":{"name":"x"}}]})");

	const FGitLink_Subprocess::FLfsSingleLockParse Out =
		FGitLink_Subprocess::Parse_LocksByPathJson(Json);

	TestTrue (TEXT("found"), Out.bFoundLock);
	TestEqual(TEXT("path normalized"),
		Out.Path, FString(TEXT("Content/Sub/C.uasset")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGitLinkTests_LocksByPathJson_Malformed,
	"GitLink.LocksByPathJson.Malformed",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGitLinkTests_LocksByPathJson_Malformed::RunTest(const FString& /*Parameters*/)
{
	// Garbage in → bFoundLock=false, caller treats as "no answer". This MUST NOT crash; LFS
	// servers in the wild have been observed returning HTML error pages on auth failures even
	// when an Accept header asks for JSON, and the parser sees the body before the caller
	// rejects on status.
	{
		const FGitLink_Subprocess::FLfsSingleLockParse Out =
			FGitLink_Subprocess::Parse_LocksByPathJson(TEXT(""));
		TestFalse(TEXT("empty input"), Out.bFoundLock);
	}
	{
		const FGitLink_Subprocess::FLfsSingleLockParse Out =
			FGitLink_Subprocess::Parse_LocksByPathJson(TEXT("<html>oops</html>"));
		TestFalse(TEXT("html input"), Out.bFoundLock);
	}
	{
		// Valid JSON but no `locks` field — older / non-spec-conformant server responses.
		const FGitLink_Subprocess::FLfsSingleLockParse Out =
			FGitLink_Subprocess::Parse_LocksByPathJson(TEXT(R"({"message":"bad"})"));
		TestFalse(TEXT("missing locks field"), Out.bFoundLock);
	}
	{
		// Lock entry without `path` — skipped, no fallback in the array → bFoundLock=false.
		const FGitLink_Subprocess::FLfsSingleLockParse Out =
			FGitLink_Subprocess::Parse_LocksByPathJson(TEXT(R"({"locks":[{"id":"1","owner":{"name":"x"}}]})"));
		TestFalse(TEXT("missing path"), Out.bFoundLock);
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
