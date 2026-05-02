#pragma once

#include "GitLink_Subprocess.h"

#include "GitLink/GitLink_State.h"

#include <CoreMinimal.h>
#include <HAL/CriticalSection.h>

class FGitLink_Subprocess;

namespace gitlink::lfs_http
{
	// True when `r.GitLink.LfsHttp` is non-zero. When false, callers fall back to the
	// `git lfs locks --verify --json` subprocess path.
	auto Is_Enabled() -> bool;

	// Pure helpers — exposed so the test module can pin their contracts. The HTTP client
	// composes these to build LFS URLs and parse credential-helper output; bugs here would
	// silently route requests at the wrong host or fail to find a cached credential.
	namespace detail
	{
		// Returns "scheme://host[:port]" from an http(s) URL — the credential cache key.
		// Empty for non-http(s) URLs (ssh, file, git@-style, malformed).
		GITLINK_API auto Get_HostKey(const FString& InUrl) -> FString;

		// Returns "host[:port]" for use in `git credential fill` input. Empty on non-http(s).
		GITLINK_API auto Get_HostPort(const FString& InUrl) -> FString;

		// Converts `git@host:path` (scp-style) to `https://host/path`. Returns empty for
		// any other form (already-https, ssh://, etc.).
		GITLINK_API auto Convert_SshRemoteToHttps(const FString& InUrl) -> FString;

		// Parses `git credential fill` line-oriented `key=value` stdout into username and
		// password. Returns true if a non-empty password was found (username may be empty
		// for token-only flows).
		GITLINK_API auto Parse_CredentialOutput(const FString& InText, FString& OutUser, FString& OutPass) -> bool;

		// Extracts the LFS-server identity (`owner.name` of any one of the entries in
		// `Page.OursPaths`) from a successful `/locks/verify` page. Returns true and writes
		// OutName when a non-empty owner name was found; false and leaves OutName untouched
		// otherwise. This is how the single-file path classifies Locked-vs-LockedOther without
		// needing a second `/locks/verify` round-trip per probe.
		GITLINK_API auto Extract_IdentityFromVerifyPage(
			const FGitLink_Subprocess::FLfsLocksSnapshot& InPage, FString& OutName) -> bool;

		// Parses an HTTP `Retry-After` header value into seconds. Accepts integer seconds
		// (the form GitHub uses); falls back to InDefaultSec for HTTP-date form or any
		// non-numeric string. Returns 0 for empty/whitespace input — caller treats this
		// as "no Retry-After header was present".
		GITLINK_API auto Parse_RetryAfterSeconds(const FString& InHeaderValue, int32 InDefaultSec) -> int32;
	}
}

// --------------------------------------------------------------------------------------------------------------------
// FGitLink_LfsHttpClient — in-process replacement for `git lfs locks --verify --json`.
//
// Posts directly to the LFS server's /locks/verify endpoint via UE's HTTP module instead of
// shelling out to git-lfs every poll. Cuts the per-call cost from a process spawn + git startup
// + per-call TLS handshake (~100–300 ms) to a pooled HTTPS request (~5–60 ms after warmup),
// which is what makes the 30 s background poll's CPU spike disappear.
//
// Auth lifts the Basic credential from git's credential helper via a one-time
// `git credential fill` per host at connect, cached in-process. If the cached token is rejected
// (401), the cache entry is invalidated and a single re-fill is attempted.
//
// Thread-safety: all public methods are safe to call from the bounded-parallel worker pool used
// by Cmd_UpdateStatus / Cmd_Connect. The credential cache is guarded by a critical section.
// HTTP requests are launched from worker threads; UE's HTTP module is thread-safe for
// AddRequest. Completion delegates fire on the game thread (which keeps ticking during the
// poll), so the worker waits on an FEvent until completion or a 10 s timeout.
// --------------------------------------------------------------------------------------------------------------------

class FGitLink_LfsHttpClient
{
public:
	// InSubprocess is borrowed for one-shot `git config` / `git credential fill` invocations
	// at connect / first-use. Must outlive the client.
	explicit FGitLink_LfsHttpClient(FGitLink_Subprocess& InSubprocess);
	~FGitLink_LfsHttpClient() = default;

	FGitLink_LfsHttpClient(const FGitLink_LfsHttpClient&)            = delete;
	FGitLink_LfsHttpClient& operator=(const FGitLink_LfsHttpClient&) = delete;

	// Resolves and caches the LFS endpoint URL for a repo root. Runs at connect time so the
	// poll path doesn't need to shell out. Tries `git -C <root> config --get lfs.url`, then
	// falls back to deriving from `remote.origin.url`. Returns true if a usable URL was found.
	auto Resolve_LfsUrlForRepo(const FString& InRepoRoot) -> bool;

	// True iff Resolve_LfsUrlForRepo has cached a usable URL for this repo.
	auto Has_LfsUrl(const FString& InRepoRoot) const -> bool;

	// Synchronous /locks/verify against the LFS server for InRepoRoot. Returns the same
	// FLfsLocksSnapshot shape that FGitLink_Subprocess::QueryLfsLocks_Verified returns today,
	// so call-site code merging into RemoteLocksAbs / OursPaths is unchanged.
	//
	// On 401: invalidates the cached credential for the host, re-fills once, retries once.
	// Second 401 → returns bSuccess=false (caller leaves the cached lock state alone, same as
	// the current subprocess path).
	//
	// On any other transport / non-2xx error: returns bSuccess=false.
	//
	// Blocks the caller for up to ~10 s while the request completes.
	auto Request_LocksVerify(const FString& InRepoRoot) -> FGitLink_Subprocess::FLfsLocksSnapshot;

	// Stage B — single-file lock probe. Result of a synchronous `GET /locks?path=<rel>` query
	// against the LFS server for the repo containing InAbsolutePath. Used by the editor-delegate
	// path (focus / asset-opened / package-dirty) and the explicit-file-list pre-checkout path
	// in Cmd_UpdateStatus to keep foreground freshness ≤2 s without paying the cross-repo sweep
	// every poll.
	struct FSingleFileLockResult
	{
		bool                 bSuccess  = false;                         // true iff the HTTP query parsed OK
		EGitLink_LockState   Lock      = EGitLink_LockState::NotLocked; // Locked / LockedOther / NotLocked
		FString              LockOwner;                                  // empty when NotLocked
	};

	// Issues `GET <lfsUrl>/locks?path=<repo-relative URL-encoded>`. Classifies the returned lock
	// (if any) as Locked vs LockedOther by comparing `owner.name` to the cached LFS-server
	// identity for the host (populated lazily from the first successful `/locks/verify`). When
	// the identity hasn't been learned yet, the result is pessimistically reported as
	// LockedOther — converges to correct after the first sweep.
	//
	// Returns bSuccess=false on transport failure / 401 (after one re-fill) / 429 (host in
	// backoff) / endpoint-unresolved. Caller should leave cached lock state alone in that case.
	//
	// InRepoRoot must be the repo root containing InAbsolutePath (resolved by the caller via
	// Provider::Is_InSubmodule + Get_SubmoduleRoot or RepoRootAbsolute).
	auto Request_SingleFileLock(
		const FString& InRepoRoot,
		const FString& InAbsolutePath) -> FSingleFileLockResult;

	// Cached LFS-server identity for a host key (e.g. "https://github.com"). Empty if no
	// successful `/locks/verify` has populated it yet. Test-friendly accessor.
	auto Get_IdentityForHost(const FString& InHostKey) const -> FString;

private:
	struct FResolvedEndpoint
	{
		FString LfsUrl;     // e.g. https://github.com/foo/bar.git/info/lfs
		FString HostKey;    // scheme://host[:port] — credential cache key
		FString RefName;    // current branch, e.g. refs/heads/main (best-effort)
	};

	// Returns the cached `Authorization: Basic <b64>` value for HostKey, filling it via
	// `git credential fill` if not cached. Empty return = couldn't obtain a credential.
	auto Get_BasicAuthHeader(const FString& InHostKey) -> FString;

	// Forces re-fill of the credential for HostKey on next Get_BasicAuthHeader.
	auto Invalidate_CredentialForHost(const FString& InHostKey) -> void;

	// Issues a single POST to LfsUrl/locks/verify, parsing the response. Returns the parsed
	// snapshot on 2xx. OutHttpStatus receives the HTTP status code (-1 on transport failure).
	// OutRetryAfterSec receives the response's Retry-After header (0 if absent).
	auto PostVerify_Once(
		const FResolvedEndpoint& InEndpoint,
		const FString&           InAuthHeader,
		const FString&           InCursor,
		int32&                   OutHttpStatus,
		int32&                   OutRetryAfterSec) -> FGitLink_Subprocess::FLfsLocksSnapshot;

	// Stage B — single-file probe. Issues `GET <LfsUrl>/locks?path=<repo-relative URL-encoded>`
	// and parses the response. Returns the first lock entry (if any). Same status / retry-after
	// out-params as PostVerify_Once.
	auto Get_LocksByPath_Once(
		const FResolvedEndpoint& InEndpoint,
		const FString&           InAuthHeader,
		const FString&           InRepoRelativePath,
		int32&                   OutHttpStatus,
		int32&                   OutRetryAfterSec) -> FGitLink_Subprocess::FLfsSingleLockParse;

	// Records the LFS-server identity learned from a successful `/locks/verify` page (any
	// `owner.name` in the `ours` set). No-op if InName is empty or already cached.
	auto Note_IdentityFromVerifyPage(
		const FString&                                  InHostKey,
		const FGitLink_Subprocess::FLfsLocksSnapshot&   InPage) -> void;

	// Returns true if a recent 429 told us to back off this host until some future time.
	// Used to short-circuit Request_LocksVerify before issuing a doomed HTTP call.
	auto Is_HostInBackoff(const FString& InHostKey) const -> bool;

	// Records that the server told us to back off this host for InRetryAfterSec seconds.
	auto Set_HostBackoff(const FString& InHostKey, int32 InRetryAfterSec) -> void;

	FGitLink_Subprocess& _Subprocess;

	mutable FCriticalSection            _EndpointsLock;
	TMap<FString, FResolvedEndpoint>    _EndpointsByRepo;   // repo root → resolved endpoint

	mutable FCriticalSection            _CredentialsLock;
	TMap<FString, FString>              _AuthByHost;        // host key → "Basic <b64>"

	// Per-host backoff deadlines (FPlatformTime::Seconds when we may try again). Populated
	// from Retry-After on a 429 response and consulted on every Request_LocksVerify call.
	// Bounded growth: one entry per LFS host (typically 1–2 in practice).
	mutable FCriticalSection            _BackoffLock;
	TMap<FString, double>               _BackoffUntilByHost;

	// Stage B — LFS-server identity per host key. Learned lazily on the first successful
	// /locks/verify response that included a non-empty `ours` array; the corresponding
	// `owner.name` IS our LFS-server identity. Used by the single-file path to classify
	// `Locked` vs `LockedOther` since /locks?path=... doesn't pre-classify.
	mutable FCriticalSection            _IdentityLock;
	TMap<FString, FString>              _IdentityByHost;
};
