#include "Commands/Cmd_Shared.h"
#include "GitLink_LfsHttpClient.h"
#include "GitLink_Subprocess.h"

#include <CoreMinimal.h>
#include <Misc/AutomationTest.h>

// --------------------------------------------------------------------------------------------------------------------
// When the LFS lock sweep may skip its `git lfs locks --verify` child process, and what URL it asks.
//
// Why this file exists: on a project with 30 submodules, about half are read-only third-party
// repos. GitHub answers /locks/verify for those with 403 "You must have push access to verify
// locks" — and the sweep treated that like any other failure and re-asked through the git-lfs CLI,
// which sends the same request, gets the same 403, and prints an empty list. That was ~15 child
// processes of ~1.7 s each on every 120 s sweep, for no information. One more repo fell back
// because its remote is configured without ".git", and GitLink — unlike git-lfs — did not add it.
//
// These are pure-function tests: the sweep itself needs a live server and a credential helper,
// which this suite deliberately does not have.
// --------------------------------------------------------------------------------------------------------------------

#if WITH_DEV_AUTOMATION_TESTS

// --------------------------------------------------------------------------------------------------------------------
// 1. Derive_LfsUrlFromRemote: the git-lfs endpoint rule — "<remote>.git/info/lfs", adding ".git"
//    only when the remote lacks it.
// --------------------------------------------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGitLinkTests_LfsHttp_DeriveLfsUrlFromRemote,
	"GitLink.LfsHttp.DeriveLfsUrlFromRemote",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGitLinkTests_LfsHttp_DeriveLfsUrlFromRemote::RunTest(const FString& /*Parameters*/)
{
	using gitlink::lfs_http::detail::Derive_LfsUrlFromRemote;

	TestEqual(TEXT("remote already ending in .git is left alone"),
		Derive_LfsUrlFromRemote(TEXT("https://github.com/owner/repo.git")),
		FString(TEXT("https://github.com/owner/repo.git/info/lfs")));
	TestEqual(TEXT(".git is appended when the remote lacks it"),
		Derive_LfsUrlFromRemote(TEXT("https://github.com/owner/repo")),
		FString(TEXT("https://github.com/owner/repo.git/info/lfs")));
	TestEqual(TEXT("trailing slash is dropped before the suffix check"),
		Derive_LfsUrlFromRemote(TEXT("https://github.com/owner/repo/")),
		FString(TEXT("https://github.com/owner/repo.git/info/lfs")));
	TestEqual(TEXT("trailing slash after .git"),
		Derive_LfsUrlFromRemote(TEXT("https://github.com/owner/repo.git/")),
		FString(TEXT("https://github.com/owner/repo.git/info/lfs")));
	TestEqual(TEXT("surrounding whitespace from `git config` output is ignored"),
		Derive_LfsUrlFromRemote(TEXT("  https://github.com/owner/repo.git\n")),
		FString(TEXT("https://github.com/owner/repo.git/info/lfs")));
	TestEqual(TEXT("a name that merely contains .git still gets the suffix"),
		Derive_LfsUrlFromRemote(TEXT("https://github.com/owner/repo.github.io")),
		FString(TEXT("https://github.com/owner/repo.github.io.git/info/lfs")));
	TestTrue(TEXT("empty remote yields no endpoint"),
		Derive_LfsUrlFromRemote(TEXT("")).IsEmpty());
	return true;
}

// --------------------------------------------------------------------------------------------------------------------
// 2. Is_VerifyRefusedByServer: only a 403 that is not throttling. GitHub signals its secondary
//    rate limit as 403 too (with Retry-After, or with the budget spent) — that one is transient,
//    and mistaking it for a refusal would silently stop lock polling for a repo that does answer.
// --------------------------------------------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGitLinkTests_LfsHttp_VerifyRefusalClassification,
	"GitLink.LfsHttp.VerifyRefusalClassification",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGitLinkTests_LfsHttp_VerifyRefusalClassification::RunTest(const FString& /*Parameters*/)
{
	using gitlink::lfs_http::detail::Is_VerifyRefusedByServer;

	TestTrue (TEXT("plain 403 is a refusal"),
		Is_VerifyRefusedByServer(403, TEXT(""), TEXT("")));
	TestTrue (TEXT("403 with budget left is a refusal"),
		Is_VerifyRefusedByServer(403, TEXT(""), TEXT("2999")));
	TestFalse(TEXT("403 with Retry-After is throttling, not a refusal"),
		Is_VerifyRefusedByServer(403, TEXT("60"), TEXT("")));
	TestFalse(TEXT("403 with a spent rate-limit budget is throttling, not a refusal"),
		Is_VerifyRefusedByServer(403, TEXT(""), TEXT("0")));
	TestFalse(TEXT("401 is an auth problem — the credential retry / fallback path owns it"),
		Is_VerifyRefusedByServer(401, TEXT(""), TEXT("")));
	TestFalse(TEXT("404 is not classified as a refusal"),
		Is_VerifyRefusedByServer(404, TEXT(""), TEXT("")));
	TestFalse(TEXT("422 is not classified as a refusal"),
		Is_VerifyRefusedByServer(422, TEXT(""), TEXT("")));
	TestFalse(TEXT("429 is throttling"),
		Is_VerifyRefusedByServer(429, TEXT(""), TEXT("")));
	TestFalse(TEXT("5xx is a server fault"),
		Is_VerifyRefusedByServer(503, TEXT(""), TEXT("")));
	TestFalse(TEXT("no response at all"),
		Is_VerifyRefusedByServer(-1, TEXT(""), TEXT("")));
	TestFalse(TEXT("200 is not a refusal"),
		Is_VerifyRefusedByServer(200, TEXT(""), TEXT("")));
	return true;
}

// --------------------------------------------------------------------------------------------------------------------
// 3. Should_FallBackToLfsSubprocess: the sweep's decision. A refusal must not spawn the child, and
//    must also not count as a successful poll — the sweep's lock-released detection only clears
//    cached locks for repos whose poll succeeded, so a refused repo's cache is left alone.
// --------------------------------------------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGitLinkTests_LfsHttp_VerifyRefusalDoesNotFallBack,
	"GitLink.LfsHttp.VerifyRefusalDoesNotFallBack",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGitLinkTests_LfsHttp_VerifyRefusalDoesNotFallBack::RunTest(const FString& /*Parameters*/)
{
	using gitlink::cmd::Should_FallBackToLfsSubprocess;

	FGitLink_Subprocess::FLfsLocksSnapshot NoAnswer;
	TestTrue (TEXT("no answer (transport / auth / backoff / no endpoint) falls back"),
		Should_FallBackToLfsSubprocess(NoAnswer));

	FGitLink_Subprocess::FLfsLocksSnapshot Answered;
	Answered.bSuccess = true;
	TestFalse(TEXT("an answer never falls back, even with no locks"),
		Should_FallBackToLfsSubprocess(Answered));

	FGitLink_Subprocess::FLfsLocksSnapshot Refused;
	Refused.bRefusedByServer = true;
	TestFalse(TEXT("a refusal does not fall back"),
		Should_FallBackToLfsSubprocess(Refused));
	TestFalse(TEXT("a refusal is not a successful poll"),
		Refused.bSuccess);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
