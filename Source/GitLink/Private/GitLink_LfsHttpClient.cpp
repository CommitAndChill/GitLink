#include "GitLink_LfsHttpClient.h"

#include "GitLink_Subprocess.h"
#include "GitLinkLog.h"

#include <HAL/IConsoleManager.h>
#include <HAL/PlatformProcess.h>
#include <HttpModule.h>
#include <Interfaces/IHttpRequest.h>
#include <Interfaces/IHttpResponse.h>
#include <GenericPlatform/GenericPlatformHttp.h>
#include <Misc/Base64.h>
#include <Misc/Paths.h>

// --------------------------------------------------------------------------------------------------------------------

static TAutoConsoleVariable<int32> CVarGitLinkLfsHttp(
	TEXT("r.GitLink.LfsHttp"),
	1,
	TEXT("When non-zero, GitLink polls LFS lock state via direct HTTPS to <lfs-url>/locks/verify ")
	TEXT("instead of shelling out to `git lfs locks --verify --json` per repo. Cuts the per-poll ")
	TEXT("CPU spike caused by N process spawns + TLS handshakes every 30 s. Set to 0 to fall back ")
	TEXT("to the subprocess path."),
	ECVF_Default);

namespace gitlink::lfs_http
{
	auto Is_Enabled() -> bool
	{
		return CVarGitLinkLfsHttp.GetValueOnAnyThread() != 0;
	}
}

// --------------------------------------------------------------------------------------------------------------------

namespace
{
	constexpr int32  kMaxPaginationPages       = 10;
	constexpr float  kRequestTimeoutSec        = 10.0f;
	constexpr int32  kRateLimitWarnThreshold   = 500;   // log when X-RateLimit-Remaining drops below this
	constexpr int32  kDefault429BackoffSec     = 60;    // used when Retry-After is missing / non-numeric
	constexpr int32  kMaxBackoffSec            = 3600;  // cap absurd Retry-After values

	// Strip a trailing slash so we can append "/locks/verify" cleanly.
	auto StripTrailingSlash(FString InUrl) -> FString
	{
		while (InUrl.EndsWith(TEXT("/")))
		{
			InUrl.LeftChopInline(1, EAllowShrinking::No);
		}
		return InUrl;
	}

	// Run `git credential fill` with stdin piped from InInput, capture stdout. Returns the
	// raw stdout text on success. The caller parses out username= / password=.
	auto Run_CredentialFill(
		const FString& InGitBinary,
		const FString& InWorkingDir,
		const FString& InInput) -> FString
	{
		void* StdInRead   = nullptr;
		void* StdInWrite  = nullptr;
		void* StdOutRead  = nullptr;
		void* StdOutWrite = nullptr;
		FPlatformProcess::CreatePipe(StdInRead,  StdInWrite,  /*bWritePipeLocal=*/ true);
		FPlatformProcess::CreatePipe(StdOutRead, StdOutWrite, /*bWritePipeLocal=*/ false);

		// CreateProc's PipeWriteChild = handle the CHILD writes to (its stdout, captured by us).
		//                  PipeReadChild  = handle the CHILD reads from (its stdin, fed by us).
		FProcHandle Proc = FPlatformProcess::CreateProc(
			*InGitBinary,
			TEXT("credential fill"),
			/*bLaunchDetached=*/ false,
			/*bLaunchHidden=*/ true,
			/*bLaunchReallyHidden=*/ true,
			/*OutProcessID=*/ nullptr,
			/*InPriority=*/ 0,
			InWorkingDir.IsEmpty() ? nullptr : *InWorkingDir,
			/*PipeWriteChild=*/ StdOutWrite,
			/*PipeReadChild=*/  StdInRead);

		if (!Proc.IsValid())
		{
			FPlatformProcess::ClosePipe(StdInRead,  StdInWrite);
			FPlatformProcess::ClosePipe(StdOutRead, StdOutWrite);
			UE_LOG(LogGitLink, Warning,
				TEXT("LfsHttpClient: failed to spawn 'git credential fill'"));
			return FString();
		}

		// Feed input. `git credential fill` reads key=value pairs until a blank line, then
		// emits the resolved credential to stdout and exits.
		FPlatformProcess::WritePipe(StdInWrite, InInput);

		// Drain stdout while the process runs.
		FString StdOutText;
		while (FPlatformProcess::IsProcRunning(Proc))
		{
			StdOutText += FPlatformProcess::ReadPipe(StdOutRead);
			FPlatformProcess::Sleep(0.01f);
		}
		StdOutText += FPlatformProcess::ReadPipe(StdOutRead);

		int32 ExitCode = -1;
		FPlatformProcess::GetProcReturnCode(Proc, &ExitCode);
		FPlatformProcess::CloseProc(Proc);
		FPlatformProcess::ClosePipe(StdInRead,  StdInWrite);
		FPlatformProcess::ClosePipe(StdOutRead, StdOutWrite);

		if (ExitCode != 0)
		{
			UE_LOG(LogGitLink, Verbose,
				TEXT("LfsHttpClient: 'git credential fill' exited %d"), ExitCode);
			return FString();
		}

		return StdOutText;
	}
}

// --------------------------------------------------------------------------------------------------------------------

namespace gitlink::lfs_http::detail
{
	auto Get_HostKey(const FString& InUrl) -> FString
	{
		int32 SchemeEnd = INDEX_NONE;
		if (!InUrl.FindChar(TEXT(':'), SchemeEnd))
		{ return FString(); }

		FString Scheme = InUrl.Left(SchemeEnd).ToLower();
		if (Scheme != TEXT("https") && Scheme != TEXT("http"))
		{ return FString(); }

		// Skip "://"
		const int32 HostStart = SchemeEnd + 3;
		if (HostStart >= InUrl.Len())
		{ return FString(); }

		const FString Rest = InUrl.Mid(HostStart);
		int32 SlashPos = INDEX_NONE;
		Rest.FindChar(TEXT('/'), SlashPos);
		const FString HostPort = SlashPos == INDEX_NONE ? Rest : Rest.Left(SlashPos);

		return FString::Printf(TEXT("%s://%s"), *Scheme, *HostPort);
	}

	auto Get_HostPort(const FString& InUrl) -> FString
	{
		const FString HostKey = Get_HostKey(InUrl);
		if (HostKey.IsEmpty())
		{ return FString(); }

		// Strip "scheme://"
		int32 ColonSlashSlash = INDEX_NONE;
		HostKey.FindChar(TEXT(':'), ColonSlashSlash);
		if (ColonSlashSlash == INDEX_NONE || ColonSlashSlash + 3 >= HostKey.Len())
		{ return FString(); }
		return HostKey.Mid(ColonSlashSlash + 3);
	}

	auto Convert_SshRemoteToHttps(const FString& InUrl) -> FString
	{
		// Reject ssh://… form here — handled separately if we ever care.
		if (InUrl.StartsWith(TEXT("ssh://")) || InUrl.StartsWith(TEXT("http")))
		{ return FString(); }

		// scp-like: user@host:path
		int32 AtPos = INDEX_NONE;
		InUrl.FindChar(TEXT('@'), AtPos);
		int32 ColonPos = INDEX_NONE;
		InUrl.FindChar(TEXT(':'), ColonPos);

		if (AtPos == INDEX_NONE || ColonPos == INDEX_NONE || ColonPos < AtPos)
		{ return FString(); }

		const FString Host = InUrl.Mid(AtPos + 1, ColonPos - (AtPos + 1));
		FString Path = InUrl.Mid(ColonPos + 1);
		Path.RemoveFromStart(TEXT("/"));

		return FString::Printf(TEXT("https://%s/%s"), *Host, *Path);
	}

	auto Parse_CredentialOutput(const FString& InText, FString& OutUser, FString& OutPass) -> bool
	{
		TArray<FString> Lines;
		InText.ParseIntoArrayLines(Lines, /*bCullEmpty=*/ true);
		for (FString Line : Lines)
		{
			Line.TrimStartAndEndInline();
			int32 EqPos = INDEX_NONE;
			if (!Line.FindChar(TEXT('='), EqPos))
			{ continue; }

			const FString Key   = Line.Left(EqPos);
			const FString Value = Line.Mid(EqPos + 1);
			if (Key == TEXT("username")) { OutUser = Value; }
			else if (Key == TEXT("password")) { OutPass = Value; }
		}
		return !OutPass.IsEmpty();   // username may legitimately be empty for token-only flows
	}

	auto Extract_IdentityFromVerifyPage(
		const FGitLink_Subprocess::FLfsLocksSnapshot& InPage, FString& OutName) -> bool
	{
		// Only trust pages that parsed cleanly. An empty `ours` set is unhelpful (no signal
		// — we may genuinely hold no locks on this host yet); in that case the next successful
		// verify will fill it in.
		if (!InPage.bSuccess || InPage.OursPaths.Num() == 0)
		{ return false; }

		for (const FString& OursPath : InPage.OursPaths)
		{
			const FString* Owner = InPage.AllLocks.Find(OursPath);
			if (Owner != nullptr && !Owner->IsEmpty())
			{
				OutName = *Owner;
				return true;
			}
		}
		return false;
	}

	auto Parse_RetryAfterSeconds(const FString& InHeaderValue, int32 InDefaultSec) -> int32
	{
		const FString Trimmed = InHeaderValue.TrimStartAndEnd();
		if (Trimmed.IsEmpty())
		{ return 0; }

		if (Trimmed.IsNumeric())
		{
			const int32 Parsed = FCString::Atoi(*Trimmed);
			return Parsed > 0 ? Parsed : InDefaultSec;
		}

		// HTTP-date form (e.g. "Wed, 21 Oct 2026 07:28:00 GMT"). Resolving against wall-clock
		// time in-process is brittle (clock skew, parsing variants); the default is fine for
		// our use case since LFS servers in the wild use the integer form.
		return InDefaultSec;
	}
}

// --------------------------------------------------------------------------------------------------------------------

FGitLink_LfsHttpClient::FGitLink_LfsHttpClient(FGitLink_Subprocess& InSubprocess)
	: _Subprocess(InSubprocess)
{
}

// --------------------------------------------------------------------------------------------------------------------

auto FGitLink_LfsHttpClient::Resolve_LfsUrlForRepo(const FString& InRepoRoot) -> bool
{
	if (InRepoRoot.IsEmpty() || !_Subprocess.IsValid())
	{ return false; }

	// 1) Prefer `git config --get lfs.url` — explicit configuration trumps derivation.
	FString LfsUrl;
	{
		const FGitLink_SubprocessResult Cfg = _Subprocess.Run(
			{ TEXT("config"), TEXT("--get"), TEXT("lfs.url") },
			InRepoRoot);
		if (Cfg.IsSuccess())
		{
			LfsUrl = Cfg.StdOut.TrimStartAndEnd();
		}
	}

	// 2) Fall back to <remote.origin.url>/info/lfs.
	if (LfsUrl.IsEmpty())
	{
		const FGitLink_SubprocessResult Remote = _Subprocess.Run(
			{ TEXT("config"), TEXT("--get"), TEXT("remote.origin.url") },
			InRepoRoot);
		if (!Remote.IsSuccess())
		{
			UE_LOG(LogGitLink, Verbose,
				TEXT("LfsHttpClient: no lfs.url and no remote.origin.url for '%s' — skipping HTTP path"),
				*InRepoRoot);
			return false;
		}

		FString RemoteUrl = Remote.StdOut.TrimStartAndEnd();
		if (RemoteUrl.StartsWith(TEXT("git@")) || RemoteUrl.StartsWith(TEXT("ssh://")))
		{
			RemoteUrl = gitlink::lfs_http::detail::Convert_SshRemoteToHttps(RemoteUrl);
		}
		if (!RemoteUrl.StartsWith(TEXT("http")))
		{
			UE_LOG(LogGitLink, Verbose,
				TEXT("LfsHttpClient: remote URL '%s' is not http(s) — falling back to subprocess for '%s'"),
				*RemoteUrl, *InRepoRoot);
			return false;
		}

		LfsUrl = StripTrailingSlash(RemoteUrl) + TEXT("/info/lfs");
	}

	const FString HostKey = gitlink::lfs_http::detail::Get_HostKey(LfsUrl);
	if (HostKey.IsEmpty())
	{
		UE_LOG(LogGitLink, Verbose,
			TEXT("LfsHttpClient: could not derive host key from '%s' — falling back to subprocess for '%s'"),
			*LfsUrl, *InRepoRoot);
		return false;
	}

	// Best-effort current branch name. Empty is acceptable; the LFS spec marks `ref` optional
	// on /locks/verify, and most servers (including GitHub) accept the omission.
	FString RefName;
	{
		const FGitLink_SubprocessResult Branch = _Subprocess.Run(
			{ TEXT("symbolic-ref"), TEXT("--quiet"), TEXT("HEAD") },
			InRepoRoot);
		if (Branch.IsSuccess())
		{
			RefName = Branch.StdOut.TrimStartAndEnd();
		}
	}

	{
		FScopeLock Lock(&_EndpointsLock);
		FResolvedEndpoint Endpoint;
		Endpoint.LfsUrl  = StripTrailingSlash(LfsUrl);
		Endpoint.HostKey = HostKey;
		Endpoint.RefName = RefName;
		_EndpointsByRepo.Add(InRepoRoot, MoveTemp(Endpoint));
	}

	UE_LOG(LogGitLink, Verbose,
		TEXT("LfsHttpClient: resolved LFS URL for '%s' → '%s' (ref='%s')"),
		*InRepoRoot, *LfsUrl, *RefName);

	return true;
}

auto FGitLink_LfsHttpClient::Has_LfsUrl(const FString& InRepoRoot) const -> bool
{
	FScopeLock Lock(&_EndpointsLock);
	return _EndpointsByRepo.Contains(InRepoRoot);
}

auto FGitLink_LfsHttpClient::Get_BasicAuthHeader(const FString& InHostKey) -> FString
{
	{
		FScopeLock Lock(&_CredentialsLock);
		if (const FString* Cached = _AuthByHost.Find(InHostKey))
		{ return *Cached; }
	}

	// Fill credential. `git credential fill` reads `protocol`, `host`, optional `path` from
	// stdin, terminated by a blank line, and emits username/password on stdout.
	const FString HostPort = gitlink::lfs_http::detail::Get_HostPort(InHostKey);
	if (HostPort.IsEmpty())
	{ return FString(); }

	const FString Scheme = InHostKey.StartsWith(TEXT("https")) ? TEXT("https") : TEXT("http");
	const FString Input = FString::Printf(
		TEXT("protocol=%s\nhost=%s\n\n"),
		*Scheme, *HostPort);

	const FString Output = Run_CredentialFill(
		_Subprocess.Get_GitBinary(),
		_Subprocess.Get_WorkingDirectory(),
		Input);
	if (Output.IsEmpty())
	{ return FString(); }

	FString User;
	FString Pass;
	if (!gitlink::lfs_http::detail::Parse_CredentialOutput(Output, User, Pass))
	{
		UE_LOG(LogGitLink, Verbose,
			TEXT("LfsHttpClient: 'git credential fill' returned no usable credential for '%s'"),
			*InHostKey);
		return FString();
	}

	const FString Combined = FString::Printf(TEXT("%s:%s"), *User, *Pass);
	const FString HeaderValue = FString::Printf(TEXT("Basic %s"), *FBase64::Encode(Combined));

	{
		FScopeLock Lock(&_CredentialsLock);
		_AuthByHost.Add(InHostKey, HeaderValue);
	}

	UE_LOG(LogGitLink, Verbose,
		TEXT("LfsHttpClient: cached credential for host '%s' (user='%s')"),
		*InHostKey, *User);

	return HeaderValue;
}

auto FGitLink_LfsHttpClient::Invalidate_CredentialForHost(const FString& InHostKey) -> void
{
	FScopeLock Lock(&_CredentialsLock);
	_AuthByHost.Remove(InHostKey);
}

auto FGitLink_LfsHttpClient::Is_HostInBackoff(const FString& InHostKey) const -> bool
{
	FScopeLock Lock(&_BackoffLock);
	const double* Until = _BackoffUntilByHost.Find(InHostKey);
	return Until != nullptr && *Until > FPlatformTime::Seconds();
}

auto FGitLink_LfsHttpClient::Set_HostBackoff(const FString& InHostKey, int32 InRetryAfterSec) -> void
{
	const int32 ClampedSec = FMath::Clamp(InRetryAfterSec, 1, kMaxBackoffSec);
	const double Until     = FPlatformTime::Seconds() + static_cast<double>(ClampedSec);
	FScopeLock Lock(&_BackoffLock);
	_BackoffUntilByHost.Add(InHostKey, Until);
}

// --------------------------------------------------------------------------------------------------------------------

auto FGitLink_LfsHttpClient::PostVerify_Once(
	const FResolvedEndpoint& InEndpoint,
	const FString&           InAuthHeader,
	const FString&           InCursor,
	int32&                   OutHttpStatus,
	int32&                   OutRetryAfterSec) -> FGitLink_Subprocess::FLfsLocksSnapshot
{
	OutHttpStatus    = -1;
	OutRetryAfterSec = 0;
	FGitLink_Subprocess::FLfsLocksSnapshot Out;

	// Build request body. Per the LFS spec: `ref` is optional; `cursor` is supplied for
	// pagination only.
	FString Body = TEXT("{");
	bool bFirstField = true;
	auto AddField = [&](const FString& Json)
	{
		if (!bFirstField) { Body += TEXT(","); }
		Body += Json;
		bFirstField = false;
	};
	if (!InEndpoint.RefName.IsEmpty())
	{
		AddField(FString::Printf(TEXT("\"ref\":{\"name\":\"%s\"}"), *InEndpoint.RefName));
	}
	if (!InCursor.IsEmpty())
	{
		AddField(FString::Printf(TEXT("\"cursor\":\"%s\""), *InCursor));
	}
	Body += TEXT("}");

	const FString FullUrl = InEndpoint.LfsUrl + TEXT("/locks/verify");

	// Shared completion state — owns the FEvent and is captured by the lambda by shared-ref.
	// If we time out and return before the delegate fires, the lambda still holds the last
	// shared-ref and the event survives until the lambda runs. The event is returned to the
	// pool exactly once, by the destructor.
	struct FCompletion
	{
		FEvent*  Done             = nullptr;
		FString  ResponseBody;
		FString  RateLimitRemaining;   // X-RateLimit-Remaining header (empty if absent)
		FString  RetryAfter;            // Retry-After header (empty if absent)
		int32    HttpStatus       = -1;
		bool     bSuccess         = false;

		~FCompletion()
		{
			if (Done != nullptr)
			{ FPlatformProcess::ReturnSynchEventToPool(Done); }
		}
	};
	const TSharedRef<FCompletion> State = MakeShared<FCompletion>();
	State->Done = FPlatformProcess::GetSynchEventFromPool(/*bIsManualReset=*/ false);

	const TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Req = FHttpModule::Get().CreateRequest();
	Req->SetURL(FullUrl);
	Req->SetVerb(TEXT("POST"));
	Req->SetHeader(TEXT("Accept"),       TEXT("application/vnd.git-lfs+json"));
	Req->SetHeader(TEXT("Content-Type"), TEXT("application/vnd.git-lfs+json"));
	Req->SetHeader(TEXT("Authorization"), InAuthHeader);
	Req->SetTimeout(kRequestTimeoutSec);
	Req->SetContentAsString(Body);
	Req->OnProcessRequestComplete().BindLambda(
		[State](FHttpRequestPtr /*InReq*/, FHttpResponsePtr InResp, bool bInSuccess)
		{
			State->bSuccess = bInSuccess && InResp.IsValid();
			if (InResp.IsValid())
			{
				State->HttpStatus         = InResp->GetResponseCode();
				State->ResponseBody       = InResp->GetContentAsString();
				State->RateLimitRemaining = InResp->GetHeader(TEXT("X-RateLimit-Remaining"));
				State->RetryAfter         = InResp->GetHeader(TEXT("Retry-After"));
			}
			State->Done->Trigger();
		});

	if (!Req->ProcessRequest())
	{
		UE_LOG(LogGitLink, Verbose,
			TEXT("LfsHttpClient: ProcessRequest() returned false for '%s'"), *FullUrl);
		return Out;
	}

	const bool bSignaled = State->Done->Wait(FTimespan::FromSeconds(kRequestTimeoutSec + 2.0));

	if (!bSignaled)
	{
		// Lambda may still fire later — that's fine, the State stays alive via its captured
		// shared-ref and the event is cleaned up by State's destructor when the lambda exits.
		Req->CancelRequest();
		UE_LOG(LogGitLink, Verbose,
			TEXT("LfsHttpClient: timeout waiting for '%s'"), *FullUrl);
		return Out;
	}

	OutHttpStatus = State->HttpStatus;

	// Surface low rate-limit budget once per session so a noisy editor + many submodules
	// can't exhaust the LFS quota silently. GitHub's LFS bucket is 3000/min authenticated
	// (separate from the 5000/hr REST quota), so we should virtually never see this — if
	// we do, it's a real signal something is misconfigured.
	if (!State->RateLimitRemaining.IsEmpty() && State->RateLimitRemaining.IsNumeric())
	{
		const int32 Remaining = FCString::Atoi(*State->RateLimitRemaining);
		static FThreadSafeBool bWarnedLowBudget = false;
		if (Remaining < kRateLimitWarnThreshold && !bWarnedLowBudget.AtomicSet(true))
		{
			UE_LOG(LogGitLink, Warning,
				TEXT("LfsHttpClient: LFS rate-limit budget low — %d remaining (host '%s'). ")
				TEXT("If this persists, set r.GitLink.LfsHttp 0 to fall back to subprocess polling."),
				Remaining, *InEndpoint.HostKey);
		}
	}

	OutRetryAfterSec = gitlink::lfs_http::detail::Parse_RetryAfterSeconds(
		State->RetryAfter, kDefault429BackoffSec);

	if (!State->bSuccess || State->HttpStatus < 200 || State->HttpStatus >= 300)
	{
		UE_LOG(LogGitLink, Verbose,
			TEXT("LfsHttpClient: '%s' returned HTTP %d (success=%s)"),
			*FullUrl, State->HttpStatus, State->bSuccess ? TEXT("true") : TEXT("false"));
		return Out;
	}

	Out = FGitLink_Subprocess::Parse_LfsVerifyJson(State->ResponseBody);
	return Out;
}

// --------------------------------------------------------------------------------------------------------------------

auto FGitLink_LfsHttpClient::Request_LocksVerify(const FString& InRepoRoot)
	-> FGitLink_Subprocess::FLfsLocksSnapshot
{
	FGitLink_Subprocess::FLfsLocksSnapshot Out;

	FResolvedEndpoint Endpoint;
	{
		FScopeLock Lock(&_EndpointsLock);
		const FResolvedEndpoint* Found = _EndpointsByRepo.Find(InRepoRoot);
		if (Found == nullptr)
		{
			UE_LOG(LogGitLink, Verbose,
				TEXT("LfsHttpClient: no resolved endpoint for '%s' — caller should fall back"),
				*InRepoRoot);
			return Out;
		}
		Endpoint = *Found;
	}

	// Server-driven backoff — honor a recent 429 by failing fast so the per-repo subprocess
	// fallback kicks in instead of pounding a host that's already told us to back off.
	if (Is_HostInBackoff(Endpoint.HostKey))
	{
		UE_LOG(LogGitLink, Verbose,
			TEXT("LfsHttpClient: skipping '%s' — host in 429 backoff, caller should fall back"),
			*Endpoint.HostKey);
		return Out;
	}

	FString AuthHeader = Get_BasicAuthHeader(Endpoint.HostKey);
	if (AuthHeader.IsEmpty())
	{
		UE_LOG(LogGitLink, Verbose,
			TEXT("LfsHttpClient: no credential available for '%s' — caller should fall back"),
			*Endpoint.HostKey);
		return Out;
	}

	// Page loop — accumulate AllLocks / OursPaths across pages.
	FString Cursor;
	int32   PagesFetched = 0;
	bool    bRetriedAuth = false;
	bool    bAnyPageOk   = false;

	while (PagesFetched < kMaxPaginationPages)
	{
		int32 HttpStatus     = -1;
		int32 RetryAfterSec  = 0;
		FGitLink_Subprocess::FLfsLocksSnapshot Page = PostVerify_Once(
			Endpoint, AuthHeader, Cursor, HttpStatus, RetryAfterSec);

		if (HttpStatus == 429)
		{
			const int32 BackoffSec = RetryAfterSec > 0 ? RetryAfterSec : kDefault429BackoffSec;
			Set_HostBackoff(Endpoint.HostKey, BackoffSec);
			UE_LOG(LogGitLink, Warning,
				TEXT("LfsHttpClient: host '%s' returned 429 — backing off %d s"),
				*Endpoint.HostKey, BackoffSec);
			break;
		}

		if (HttpStatus == 401 && !bRetriedAuth)
		{
			Invalidate_CredentialForHost(Endpoint.HostKey);
			AuthHeader = Get_BasicAuthHeader(Endpoint.HostKey);
			bRetriedAuth = true;
			if (AuthHeader.IsEmpty())
			{ break; }
			continue;
		}

		if (!Page.bSuccess)
		{
			break;   // transport error or non-2xx — abort accumulation
		}

		bAnyPageOk = true;
		Out.bSuccess = true;
		Out.AllLocks.Append(Page.AllLocks);
		Out.OursPaths.Append(Page.OursPaths);

		// Stage B — opportunistically learn the LFS-server identity for this host from any
		// successful page that contains an `ours` entry. This is the only way to correctly
		// classify Locked vs LockedOther on the cheaper `GET /locks?path=...` single-file
		// path, which doesn't pre-classify.
		Note_IdentityFromVerifyPage(Endpoint.HostKey, Page);

		// Pagination — Parse_LfsVerifyJson doesn't currently surface next_cursor. For the
		// moment we treat the response as single-page; cursor support is a v2 if we ever see
		// pagination at our lock counts. Break unconditionally.
		(void)Cursor;
		break;
	}

	if (!bAnyPageOk)
	{
		Out.bSuccess = false;
		Out.AllLocks.Reset();
		Out.OursPaths.Reset();
	}

	return Out;
}

// --------------------------------------------------------------------------------------------------------------------
// Stage B — single-file lock probe path. Reuses the auth cache, 429 backoff, sync-wait pattern,
// and rate-limit telemetry of the verify path; the only differences are the verb (GET), the URL
// shape (?path=<rel>), the response shape (raw `locks` array — no ours/theirs split), and the
// classification step (compare owner.name to the cached LFS-server identity).
// --------------------------------------------------------------------------------------------------------------------

auto FGitLink_LfsHttpClient::Note_IdentityFromVerifyPage(
	const FString&                                  InHostKey,
	const FGitLink_Subprocess::FLfsLocksSnapshot&   InPage) -> void
{
	// Cheap fast path — already learned, don't bother walking the page.
	{
		FScopeLock Lock(&_IdentityLock);
		if (_IdentityByHost.Contains(InHostKey))
		{ return; }
	}

	FString LearnedName;
	if (!gitlink::lfs_http::detail::Extract_IdentityFromVerifyPage(InPage, LearnedName))
	{ return; }

	{
		FScopeLock Lock(&_IdentityLock);
		// Re-check under the lock — another worker may have learned the identity in between.
		if (_IdentityByHost.Contains(InHostKey))
		{ return; }
		_IdentityByHost.Add(InHostKey, LearnedName);
	}

	UE_LOG(LogGitLink, Log,
		TEXT("LfsHttpClient: learned LFS-server identity for host '%s' = '%s'"),
		*InHostKey, *LearnedName);
}

auto FGitLink_LfsHttpClient::Get_IdentityForHost(const FString& InHostKey) const -> FString
{
	FScopeLock Lock(&_IdentityLock);
	const FString* Found = _IdentityByHost.Find(InHostKey);
	return Found != nullptr ? *Found : FString();
}

// --------------------------------------------------------------------------------------------------------------------

auto FGitLink_LfsHttpClient::Get_LocksByPath_Once(
	const FResolvedEndpoint& InEndpoint,
	const FString&           InAuthHeader,
	const FString&           InRepoRelativePath,
	int32&                   OutHttpStatus,
	int32&                   OutRetryAfterSec) -> FGitLink_Subprocess::FLfsSingleLockParse
{
	OutHttpStatus    = -1;
	OutRetryAfterSec = 0;
	FGitLink_Subprocess::FLfsSingleLockParse Out;

	const FString EncodedPath = FGenericPlatformHttp::UrlEncode(InRepoRelativePath);
	const FString FullUrl = FString::Printf(TEXT("%s/locks?path=%s"),
		*InEndpoint.LfsUrl, *EncodedPath);

	struct FCompletion
	{
		FEvent*  Done             = nullptr;
		FString  ResponseBody;
		FString  RateLimitRemaining;
		FString  RetryAfter;
		int32    HttpStatus       = -1;
		bool     bSuccess         = false;

		~FCompletion()
		{
			if (Done != nullptr)
			{ FPlatformProcess::ReturnSynchEventToPool(Done); }
		}
	};
	const TSharedRef<FCompletion> State = MakeShared<FCompletion>();
	State->Done = FPlatformProcess::GetSynchEventFromPool(/*bIsManualReset=*/ false);

	const TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Req = FHttpModule::Get().CreateRequest();
	Req->SetURL(FullUrl);
	Req->SetVerb(TEXT("GET"));
	Req->SetHeader(TEXT("Accept"),        TEXT("application/vnd.git-lfs+json"));
	Req->SetHeader(TEXT("Authorization"), InAuthHeader);
	Req->SetTimeout(kRequestTimeoutSec);
	Req->OnProcessRequestComplete().BindLambda(
		[State](FHttpRequestPtr /*InReq*/, FHttpResponsePtr InResp, bool bInSuccess)
		{
			State->bSuccess = bInSuccess && InResp.IsValid();
			if (InResp.IsValid())
			{
				State->HttpStatus         = InResp->GetResponseCode();
				State->ResponseBody       = InResp->GetContentAsString();
				State->RateLimitRemaining = InResp->GetHeader(TEXT("X-RateLimit-Remaining"));
				State->RetryAfter         = InResp->GetHeader(TEXT("Retry-After"));
			}
			State->Done->Trigger();
		});

	if (!Req->ProcessRequest())
	{
		UE_LOG(LogGitLink, Verbose,
			TEXT("LfsHttpClient: ProcessRequest() returned false for '%s'"), *FullUrl);
		return Out;
	}

	const bool bSignaled = State->Done->Wait(FTimespan::FromSeconds(kRequestTimeoutSec + 2.0));

	if (!bSignaled)
	{
		Req->CancelRequest();
		UE_LOG(LogGitLink, Verbose,
			TEXT("LfsHttpClient: timeout waiting for '%s'"), *FullUrl);
		return Out;
	}

	OutHttpStatus = State->HttpStatus;

	// Same one-shot rate-limit warning as the verify path. The static warning flag is per-process,
	// so a single warning will be emitted regardless of which path tripped it.
	if (!State->RateLimitRemaining.IsEmpty() && State->RateLimitRemaining.IsNumeric())
	{
		const int32 Remaining = FCString::Atoi(*State->RateLimitRemaining);
		static FThreadSafeBool bWarnedLowBudget = false;
		if (Remaining < kRateLimitWarnThreshold && !bWarnedLowBudget.AtomicSet(true))
		{
			UE_LOG(LogGitLink, Warning,
				TEXT("LfsHttpClient: LFS rate-limit budget low — %d remaining (host '%s'). ")
				TEXT("If this persists, set r.GitLink.LfsHttp 0 to fall back to subprocess polling."),
				Remaining, *InEndpoint.HostKey);
		}
	}

	OutRetryAfterSec = gitlink::lfs_http::detail::Parse_RetryAfterSeconds(
		State->RetryAfter, kDefault429BackoffSec);

	if (!State->bSuccess || State->HttpStatus < 200 || State->HttpStatus >= 300)
	{
		UE_LOG(LogGitLink, Verbose,
			TEXT("LfsHttpClient: '%s' returned HTTP %d (success=%s)"),
			*FullUrl, State->HttpStatus, State->bSuccess ? TEXT("true") : TEXT("false"));
		return Out;
	}

	Out = FGitLink_Subprocess::Parse_LocksByPathJson(State->ResponseBody);
	return Out;
}

// --------------------------------------------------------------------------------------------------------------------

auto FGitLink_LfsHttpClient::Request_SingleFileLock(
	const FString& InRepoRoot,
	const FString& InAbsolutePath) -> FSingleFileLockResult
{
	FSingleFileLockResult Out;

	if (InRepoRoot.IsEmpty() || InAbsolutePath.IsEmpty())
	{ return Out; }

	FResolvedEndpoint Endpoint;
	{
		FScopeLock Lock(&_EndpointsLock);
		const FResolvedEndpoint* Found = _EndpointsByRepo.Find(InRepoRoot);
		if (Found == nullptr)
		{
			UE_LOG(LogGitLink, Verbose,
				TEXT("LfsHttpClient: no resolved endpoint for '%s' — single-file probe skipped"),
				*InRepoRoot);
			return Out;
		}
		Endpoint = *Found;
	}

	if (Is_HostInBackoff(Endpoint.HostKey))
	{
		UE_LOG(LogGitLink, Verbose,
			TEXT("LfsHttpClient: skipping single-file probe — host '%s' in 429 backoff"),
			*Endpoint.HostKey);
		return Out;
	}

	// Compute repo-relative forward-slashed path. The LFS server stores paths in this form;
	// MakePathRelativeTo expects both sides forward-slashed and returns the same.
	FString RelPath = InAbsolutePath;
	FString RepoRoot = InRepoRoot;
	FPaths::NormalizeFilename(RelPath);
	FPaths::NormalizeFilename(RepoRoot);
	if (!RepoRoot.EndsWith(TEXT("/"))) { RepoRoot += TEXT("/"); }
	if (!FPaths::MakePathRelativeTo(RelPath, *RepoRoot))
	{
		UE_LOG(LogGitLink, Verbose,
			TEXT("LfsHttpClient: '%s' is not under repo root '%s' — single-file probe skipped"),
			*InAbsolutePath, *InRepoRoot);
		return Out;
	}
	if (RelPath.IsEmpty())
	{ return Out; }

	FString AuthHeader = Get_BasicAuthHeader(Endpoint.HostKey);
	if (AuthHeader.IsEmpty())
	{
		UE_LOG(LogGitLink, Verbose,
			TEXT("LfsHttpClient: no credential for '%s' — single-file probe skipped"),
			*Endpoint.HostKey);
		return Out;
	}

	bool bRetriedAuth = false;
	FGitLink_Subprocess::FLfsSingleLockParse Parse;
	int32 HttpStatus    = -1;
	int32 RetryAfterSec = 0;

	for (int32 Attempt = 0; Attempt < 2; ++Attempt)
	{
		Parse = Get_LocksByPath_Once(Endpoint, AuthHeader, RelPath, HttpStatus, RetryAfterSec);

		if (HttpStatus == 429)
		{
			const int32 BackoffSec = RetryAfterSec > 0 ? RetryAfterSec : kDefault429BackoffSec;
			Set_HostBackoff(Endpoint.HostKey, BackoffSec);
			UE_LOG(LogGitLink, Warning,
				TEXT("LfsHttpClient: host '%s' returned 429 on single-file probe — backing off %d s"),
				*Endpoint.HostKey, BackoffSec);
			return Out;
		}

		if (HttpStatus == 401 && !bRetriedAuth)
		{
			Invalidate_CredentialForHost(Endpoint.HostKey);
			AuthHeader = Get_BasicAuthHeader(Endpoint.HostKey);
			bRetriedAuth = true;
			if (AuthHeader.IsEmpty())
			{ return Out; }
			continue;
		}

		break;
	}

	// HTTP query succeeded iff we got a 2xx. An empty `locks` array on a 2xx is a real
	// "no lock exists for this path" answer — that's bSuccess=true with NotLocked.
	if (HttpStatus < 200 || HttpStatus >= 300)
	{ return Out; }

	if (!Parse.bFoundLock)
	{
		// "No lock for this path" — safe to apply (LockedOther → NotLocked teammate-released
		// transition). bSuccess=true so the caller updates the cache; an empty owner won't
		// be confused for a positive lock answer because Lock=NotLocked.
		Out.bSuccess = true;
		Out.Lock     = EGitLink_LockState::NotLocked;
		return Out;
	}

	// Probe found a lock. Classify Locked-by-us vs LockedOther by comparing owner.name to the
	// cached LFS-server identity. If identity isn't learned yet — which happens on cold start
	// before the first /locks/verify sweep populates it — we genuinely can't classify, so
	// return bSuccess=false. The caller (Request_LockRefreshForFile) treats that as "no
	// answer" and leaves the cache alone; the next sweep will fix things.
	//
	// v0.3.0 used a pessimistic LockedOther fallback here, on the theory that "user attempts
	// checkout of a file shown as LockedOther → server lets them" is better than the inverse.
	// In practice it produced a worse cold-start UX: the asset-opened/focus delegates fired
	// probes for our own locked files within milliseconds of editor start, before the sweep
	// could learn identity, and the cache was briefly populated with LockedOther+our-name —
	// confusing "locked by Matiaus (= me)" entries that flipped to Locked once the sweep
	// caught up. Returning bSuccess=false trades freshness during the cold-start window for
	// correctness; the sweep is still bounded by the 120 s interval, and the editor delegates
	// catch up as soon as identity is learned.
	const FString CachedIdentity = Get_IdentityForHost(Endpoint.HostKey);
	if (CachedIdentity.IsEmpty())
	{
		UE_LOG(LogGitLink, Verbose,
			TEXT("LfsHttpClient: probe found lock but identity for host '%s' isn't learned yet — deferring classification to next sweep"),
			*Endpoint.HostKey);
		return Out;  // bSuccess stays false; caller leaves cache alone
	}

	const bool bIsOurs = Parse.OwnerName.Equals(CachedIdentity, ESearchCase::IgnoreCase);
	Out.bSuccess  = true;
	Out.Lock      = bIsOurs ? EGitLink_LockState::Locked : EGitLink_LockState::LockedOther;
	Out.LockOwner = Parse.OwnerName;
	return Out;
}
