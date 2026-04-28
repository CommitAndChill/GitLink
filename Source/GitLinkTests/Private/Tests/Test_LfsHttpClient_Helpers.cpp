#include "GitLink_LfsHttpClient.h"

#include <CoreMinimal.h>
#include <Misc/AutomationTest.h>

// --------------------------------------------------------------------------------------------------------------------
// Tests for the pure helpers in gitlink::lfs_http::detail used by the in-process LFS client.
//
// Why these matter: Get_HostKey / Get_HostPort produce the credential-cache key — a wrong key
// silently routes auth at the wrong host or causes endless cache misses. Convert_SshRemoteToHttps
// is the only path by which an SSH-cloned repo becomes pollable via HTTPS LFS; an off-by-one
// here means SSH-cloned repos always fall through to the subprocess path, defeating the
// optimization. Parse_CredentialOutput must drop unknown fields gracefully — git's credential
// helpers add new keys over time (e.g. `password_expiry_utc`, `oauth_refresh_token`).
// --------------------------------------------------------------------------------------------------------------------

#if WITH_DEV_AUTOMATION_TESTS

using gitlink::lfs_http::detail::Get_HostKey;
using gitlink::lfs_http::detail::Get_HostPort;
using gitlink::lfs_http::detail::Convert_SshRemoteToHttps;
using gitlink::lfs_http::detail::Parse_CredentialOutput;

// --------------------------------------------------------------------------------------------------------------------
// Get_HostKey: "scheme://host[:port]" extraction. Used as the credential-cache key, so two URLs
// to the same logical host MUST hash to the same key.
// --------------------------------------------------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGitLinkTests_LfsHttp_HostKey,
	"GitLink.LfsHttp.HostKey",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGitLinkTests_LfsHttp_HostKey::RunTest(const FString& /*Parameters*/)
{
	// Happy paths.
	TestEqual(TEXT("https plain host"),
		Get_HostKey(TEXT("https://github.com/foo/bar.git")), FString(TEXT("https://github.com")));
	TestEqual(TEXT("https with port"),
		Get_HostKey(TEXT("https://git.example.com:8443/foo/bar.git/info/lfs")),
		FString(TEXT("https://git.example.com:8443")));
	TestEqual(TEXT("http plain"),
		Get_HostKey(TEXT("http://localhost/foo")), FString(TEXT("http://localhost")));
	TestEqual(TEXT("scheme is lowercased"),
		Get_HostKey(TEXT("HTTPS://Github.Com/foo")), FString(TEXT("https://Github.Com")));
	TestEqual(TEXT("no path component"),
		Get_HostKey(TEXT("https://github.com")), FString(TEXT("https://github.com")));

	// Non-http(s) — must return empty so callers fall back to subprocess.
	TestTrue(TEXT("ssh:// returns empty"),
		Get_HostKey(TEXT("ssh://git@github.com/foo/bar.git")).IsEmpty());
	TestTrue(TEXT("scp-style returns empty"),
		Get_HostKey(TEXT("git@github.com:foo/bar.git")).IsEmpty());
	TestTrue(TEXT("file:// returns empty"),
		Get_HostKey(TEXT("file:///tmp/repo.git")).IsEmpty());
	TestTrue(TEXT("empty input returns empty"),
		Get_HostKey(TEXT("")).IsEmpty());
	TestTrue(TEXT("garbage returns empty"),
		Get_HostKey(TEXT("not a url")).IsEmpty());

	return true;
}

// --------------------------------------------------------------------------------------------------------------------
// Get_HostPort: "host[:port]" — what we feed to `git credential fill` as the `host=` line.
// --------------------------------------------------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGitLinkTests_LfsHttp_HostPort,
	"GitLink.LfsHttp.HostPort",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGitLinkTests_LfsHttp_HostPort::RunTest(const FString& /*Parameters*/)
{
	TestEqual(TEXT("plain host"),
		Get_HostPort(TEXT("https://github.com/foo")), FString(TEXT("github.com")));
	TestEqual(TEXT("host:port preserved"),
		Get_HostPort(TEXT("https://git.example.com:8443/foo/bar")),
		FString(TEXT("git.example.com:8443")));

	TestTrue(TEXT("non-http returns empty"),
		Get_HostPort(TEXT("git@github.com:foo")).IsEmpty());
	TestTrue(TEXT("empty returns empty"),
		Get_HostPort(TEXT("")).IsEmpty());

	return true;
}

// --------------------------------------------------------------------------------------------------------------------
// Convert_SshRemoteToHttps: scp-style → HTTPS. The single biggest source of "why doesn't the
// HTTP path activate?" reports for SSH-cloned repos.
// --------------------------------------------------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGitLinkTests_LfsHttp_SshConversion,
	"GitLink.LfsHttp.SshConversion",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGitLinkTests_LfsHttp_SshConversion::RunTest(const FString& /*Parameters*/)
{
	// Canonical GitHub SSH form.
	TestEqual(TEXT("github scp-style → https"),
		Convert_SshRemoteToHttps(TEXT("git@github.com:foo/bar.git")),
		FString(TEXT("https://github.com/foo/bar.git")));

	// Self-hosted host.
	TestEqual(TEXT("self-hosted scp-style"),
		Convert_SshRemoteToHttps(TEXT("git@git.example.com:org/repo.git")),
		FString(TEXT("https://git.example.com/org/repo.git")));

	// Already-https → empty (caller passes through unchanged).
	TestTrue(TEXT("already https returns empty"),
		Convert_SshRemoteToHttps(TEXT("https://github.com/foo/bar.git")).IsEmpty());

	// ssh:// form is explicitly rejected (caller can choose what to do with it).
	TestTrue(TEXT("ssh:// returns empty"),
		Convert_SshRemoteToHttps(TEXT("ssh://git@github.com/foo/bar.git")).IsEmpty());

	// Garbage → empty.
	TestTrue(TEXT("plain text returns empty"),
		Convert_SshRemoteToHttps(TEXT("not a url")).IsEmpty());
	TestTrue(TEXT("empty returns empty"),
		Convert_SshRemoteToHttps(TEXT("")).IsEmpty());

	return true;
}

// --------------------------------------------------------------------------------------------------------------------
// Parse_CredentialOutput: line-oriented `key=value` parser. Must tolerate unknown keys
// (git/GCM add fields like `password_expiry_utc`, `oauth_refresh_token`, `capability[]`)
// and surface bSuccess=false when no password line was emitted.
// --------------------------------------------------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGitLinkTests_LfsHttp_CredentialOutput,
	"GitLink.LfsHttp.CredentialOutput",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGitLinkTests_LfsHttp_CredentialOutput::RunTest(const FString& /*Parameters*/)
{
	// Happy path.
	{
		FString User, Pass;
		const FString Output = TEXT("protocol=https\nhost=github.com\nusername=alice\npassword=ghp_secret\n");
		TestTrue (TEXT("happy path returns true"),  Parse_CredentialOutput(Output, User, Pass));
		TestEqual(TEXT("username extracted"),       User, FString(TEXT("alice")));
		TestEqual(TEXT("password extracted"),       Pass, FString(TEXT("ghp_secret")));
	}

	// Unknown keys are ignored — proves we don't break when GCM adds new fields.
	{
		FString User, Pass;
		const FString Output =
			TEXT("protocol=https\n")
			TEXT("host=github.com\n")
			TEXT("username=bob\n")
			TEXT("password=tok123\n")
			TEXT("password_expiry_utc=1735689600\n")
			TEXT("capability[]=authtype\n");
		TestTrue (TEXT("unknown keys tolerated"), Parse_CredentialOutput(Output, User, Pass));
		TestEqual(TEXT("password still extracted"), Pass, FString(TEXT("tok123")));
	}

	// Token-only flow — username can be empty, but password must be present.
	{
		FString User, Pass;
		const FString Output = TEXT("password=just_a_token\n");
		TestTrue (TEXT("password-only returns true"), Parse_CredentialOutput(Output, User, Pass));
		TestTrue (TEXT("username empty"),             User.IsEmpty());
		TestEqual(TEXT("password extracted"),         Pass, FString(TEXT("just_a_token")));
	}

	// No password line → false. Caller treats this as "couldn't get a credential" and falls
	// back to the subprocess path.
	{
		FString User, Pass;
		const FString Output = TEXT("protocol=https\nhost=github.com\nusername=carol\n");
		TestFalse(TEXT("no password returns false"), Parse_CredentialOutput(Output, User, Pass));
	}

	// Empty input.
	{
		FString User, Pass;
		TestFalse(TEXT("empty returns false"), Parse_CredentialOutput(TEXT(""), User, Pass));
	}

	// Lines without `=` are silently dropped — git emits a leading blank line in some
	// configurations, and we shouldn't crash or misparse.
	{
		FString User, Pass;
		const FString Output = TEXT("\nprotocol=https\n\nusername=dee\npassword=p\n\n");
		TestTrue (TEXT("blank lines ignored"), Parse_CredentialOutput(Output, User, Pass));
		TestEqual(TEXT("username extracted"),  User, FString(TEXT("dee")));
		TestEqual(TEXT("password extracted"),  Pass, FString(TEXT("p")));
	}

	// Values containing `=` are preserved (passwords can contain `=` as base64 padding).
	{
		FString User, Pass;
		const FString Output = TEXT("username=eve\npassword=base64==\n");
		TestTrue (TEXT("equals in value tolerated"), Parse_CredentialOutput(Output, User, Pass));
		TestEqual(TEXT("password preserved"),        Pass, FString(TEXT("base64==")));
	}

	return true;
}

#endif  // WITH_DEV_AUTOMATION_TESTS
