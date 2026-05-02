#include "GitLink_LfsHttpClient.h"
#include "GitLink_Subprocess.h"

#include <CoreMinimal.h>
#include <Misc/AutomationTest.h>

// --------------------------------------------------------------------------------------------------------------------
// Tests for gitlink::lfs_http::detail::Extract_IdentityFromVerifyPage — the helper that learns
// the LFS-server identity for a host from a successful /locks/verify response.
//
// Why this matters: the single-file `GET /locks?path=...` path doesn't pre-classify Locked vs
// LockedOther; it relies on this cached identity. A bug here means we either never learn the
// identity (every probe falls back to LockedOther → user can't check out their own files) or
// learn the wrong one (we'd mis-classify a teammate's lock as ours and let the user attempt a
// checkout that the LFS server rejects).
// --------------------------------------------------------------------------------------------------------------------

#if WITH_DEV_AUTOMATION_TESTS

using gitlink::lfs_http::detail::Extract_IdentityFromVerifyPage;

namespace
{
	auto Make_Page(bool bSuccess,
		std::initializer_list<TPair<FString, FString>> InOurs,
		std::initializer_list<TPair<FString, FString>> InTheirs) -> FGitLink_Subprocess::FLfsLocksSnapshot
	{
		FGitLink_Subprocess::FLfsLocksSnapshot Page;
		Page.bSuccess = bSuccess;
		for (const auto& Pair : InOurs)
		{
			Page.AllLocks.Add(Pair.Key, Pair.Value);
			Page.OursPaths.Add(Pair.Key);
		}
		for (const auto& Pair : InTheirs)
		{
			Page.AllLocks.Add(Pair.Key, Pair.Value);
		}
		return Page;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGitLinkTests_Identity_HappyPath,
	"GitLink.LfsIdentity.HappyPath",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGitLinkTests_Identity_HappyPath::RunTest(const FString& /*Parameters*/)
{
	const auto Page = Make_Page(/*bSuccess=*/ true,
		/*Ours=*/   { { TEXT("a.uasset"), TEXT("alice") } },
		/*Theirs=*/ { { TEXT("b.uasset"), TEXT("bob")   } });

	FString Out;
	TestTrue(TEXT("extracted"), Extract_IdentityFromVerifyPage(Page, Out));
	TestEqual(TEXT("identity is ours owner"), Out, FString(TEXT("alice")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGitLinkTests_Identity_NoOurs,
	"GitLink.LfsIdentity.NoOurs",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGitLinkTests_Identity_NoOurs::RunTest(const FString& /*Parameters*/)
{
	// We hold no locks on this host yet — verify can't tell us who we are. The next sweep
	// after we lock something will fill it in.
	const auto Page = Make_Page(/*bSuccess=*/ true,
		/*Ours=*/   {},
		/*Theirs=*/ { { TEXT("b.uasset"), TEXT("bob") } });

	FString Out = TEXT("untouched");
	TestFalse(TEXT("not extracted"), Extract_IdentityFromVerifyPage(Page, Out));
	TestEqual(TEXT("out left untouched"), Out, FString(TEXT("untouched")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGitLinkTests_Identity_FailedPage,
	"GitLink.LfsIdentity.FailedPage",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGitLinkTests_Identity_FailedPage::RunTest(const FString& /*Parameters*/)
{
	// bSuccess=false → don't trust anything in the page (the JSON parser populates bSuccess
	// based on parse success, and we never want to learn from a parse failure even if some
	// fields were extracted).
	const auto Page = Make_Page(/*bSuccess=*/ false,
		/*Ours=*/   { { TEXT("a.uasset"), TEXT("alice") } },
		/*Theirs=*/ {});

	FString Out;
	TestFalse(TEXT("not extracted from failed page"), Extract_IdentityFromVerifyPage(Page, Out));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGitLinkTests_Identity_OursWithEmptyOwner,
	"GitLink.LfsIdentity.OursWithEmptyOwner",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGitLinkTests_Identity_OursWithEmptyOwner::RunTest(const FString& /*Parameters*/)
{
	// Edge: a lock in `ours` whose owner.name was missing from the JSON. The parser stores it
	// as empty string. Extract_IdentityFromVerifyPage MUST skip such entries — using "" as
	// our identity would mis-classify every locked file with an empty owner as Locked, which
	// is the wrong direction (we'd let the user attempt a doomed checkout).
	FGitLink_Subprocess::FLfsLocksSnapshot Page;
	Page.bSuccess = true;
	Page.AllLocks.Add(TEXT("a.uasset"), TEXT(""));   // empty owner
	Page.AllLocks.Add(TEXT("c.uasset"), TEXT("carol"));
	Page.OursPaths.Add(TEXT("a.uasset"));
	Page.OursPaths.Add(TEXT("c.uasset"));

	FString Out;
	TestTrue(TEXT("extracted"), Extract_IdentityFromVerifyPage(Page, Out));
	TestEqual(TEXT("skipped empty, picked carol"), Out, FString(TEXT("carol")));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
