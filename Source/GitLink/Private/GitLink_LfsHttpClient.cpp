#include "GitLink_LfsHttpClient.h"

#include "GitLink_Subprocess.h"
#include "GitLinkLog.h"

#include <HAL/IConsoleManager.h>
#include <HAL/PlatformProcess.h>
#include <HttpModule.h>
#include <Interfaces/IHttpRequest.h>
#include <Interfaces/IHttpResponse.h>
#include <Misc/Base64.h>

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
	constexpr int32  kMaxPaginationPages = 10;
	constexpr float  kRequestTimeoutSec  = 10.0f;

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

// --------------------------------------------------------------------------------------------------------------------

auto FGitLink_LfsHttpClient::PostVerify_Once(
	const FResolvedEndpoint& InEndpoint,
	const FString&           InAuthHeader,
	const FString&           InCursor,
	int32&                   OutHttpStatus) -> FGitLink_Subprocess::FLfsLocksSnapshot
{
	OutHttpStatus = -1;
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
		FEvent*  Done       = nullptr;
		FString  ResponseBody;
		int32    HttpStatus = -1;
		bool     bSuccess   = false;

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
				State->HttpStatus   = InResp->GetResponseCode();
				State->ResponseBody = InResp->GetContentAsString();
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
		int32 HttpStatus = -1;
		FGitLink_Subprocess::FLfsLocksSnapshot Page = PostVerify_Once(
			Endpoint, AuthHeader, Cursor, HttpStatus);

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
