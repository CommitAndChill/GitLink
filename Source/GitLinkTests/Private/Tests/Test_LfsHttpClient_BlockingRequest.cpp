#include "GitLink_LfsHttpClient.h"

#include <Async/Async.h>
#include <CoreMinimal.h>
#include <IPAddress.h>
#include <Misc/AutomationTest.h>
#include <Sockets.h>
#include <SocketSubsystem.h>

#include <atomic>

// --------------------------------------------------------------------------------------------------------------------
// gitlink::lfs_http::detail::Execute_BlockingRequest must complete while the GAME THREAD is blocked.
//
// Why this matters: every LFS lock query blocks its calling thread until the request's completion delegate fires.
// UE's default delegate policy delivers that completion from the game thread's FHttpManager tick, so a caller
// blocked while the game thread is busy got a guaranteed "HTTP request timed out after 10.00 seconds" however fast
// the server answered. The editor's startup Connect sweep runs exactly then — during the first frame — and every
// one of a 32-repo project's lock checks timed out, in waves of 8, before the editor drew a frame.
//
// Automation tests run ON the game thread, so calling the primitive straight from RunTest is the blocked-game-thread
// condition itself: nothing ticks FHttpManager until the call returns. The server is a raw loopback socket served
// from its own thread (UE's HTTPServer module ticks on the game thread, so it would stall the same way). Revert the
// delegate thread policy in Execute_BlockingRequest and both tests go red — the first by timing out, the second by
// falling through to the wait backstop instead of the engine's own timeout.
// --------------------------------------------------------------------------------------------------------------------

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	// One-connection HTTP/1.1 server on 127.0.0.1:<ephemeral>. Accepts a single connection on a background thread,
	// records the raw request, then either answers with InCannedResponse or (when it is empty) holds the connection
	// open without answering until Stop() — a server that accepted but never replies.
	class FLoopbackHttpServer
	{
	public:
		explicit FLoopbackHttpServer(FString InCannedResponse)
			: _CannedResponse(MoveTemp(InCannedResponse))
		{
			_Subsystem = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
			if (_Subsystem == nullptr)
			{ return; }

			_Listener = _Subsystem->CreateSocket(NAME_Stream, TEXT("GitLinkTests loopback listener"), FNetworkProtocolTypes::IPv4);
			if (_Listener == nullptr)
			{ return; }

			const TSharedRef<FInternetAddr> Addr = _Subsystem->CreateInternetAddr(FNetworkProtocolTypes::IPv4);
			Addr->SetLoopbackAddress();
			Addr->SetPort(0);
			if (!_Listener->Bind(*Addr) || !_Listener->Listen(1))
			{ return; }

			_Port = _Listener->GetPortNo();
			_Served = Async(EAsyncExecution::Thread, [this]() { return Serve(); });
		}

		~FLoopbackHttpServer()
		{
			Stop();
			if (_Served.IsValid())
			{ _Served.Wait(); }
			if (_Listener != nullptr)
			{
				_Listener->Close();
				_Subsystem->DestroySocket(_Listener);
			}
		}

		auto Is_Listening() const -> bool { return _Port > 0; }
		auto Get_Url(const FString& InPath) const -> FString { return FString::Printf(TEXT("http://127.0.0.1:%d%s"), _Port, *InPath); }
		auto Stop() -> void { _bStop = true; }

		// The raw request text the server received (empty if none arrived). Blocks until the serve thread is done.
		auto Get_ReceivedRequest() -> FString
		{
			Stop();
			return _Served.IsValid() ? _Served.Get() : FString();
		}

	private:
		auto Serve() -> FString
		{
			const double Deadline = FPlatformTime::Seconds() + 20.0;

			FSocket* Conn = nullptr;
			while (Conn == nullptr && !_bStop && FPlatformTime::Seconds() < Deadline)
			{
				bool bPending = false;
				if (_Listener->WaitForPendingConnection(bPending, FTimespan::FromMilliseconds(50)) && bPending)
				{ Conn = _Listener->Accept(TEXT("GitLinkTests loopback connection")); }
			}
			if (Conn == nullptr)
			{ return FString(); }

			// Read headers, then Content-Length bytes of body.
			TArray<uint8> Received;
			int32 BodyStart   = INDEX_NONE;
			int32 BodyLength  = 0;
			while (!_bStop && FPlatformTime::Seconds() < Deadline)
			{
				if (!Conn->Wait(ESocketWaitConditions::WaitForRead, FTimespan::FromMilliseconds(50)))
				{ continue; }

				uint8 Buffer[4096];
				int32 BytesRead = 0;
				if (!Conn->Recv(Buffer, sizeof(Buffer), BytesRead) || BytesRead <= 0)
				{ break; }
				Received.Append(Buffer, BytesRead);

				if (BodyStart == INDEX_NONE)
				{
					const FString SoFar = BytesToString(Received);
					const int32 HeaderEnd = SoFar.Find(TEXT("\r\n\r\n"));
					if (HeaderEnd == INDEX_NONE)
					{ continue; }
					BodyStart = HeaderEnd + 4;

					const int32 LenAt = SoFar.Find(TEXT("Content-Length:"), ESearchCase::IgnoreCase);
					if (LenAt != INDEX_NONE && LenAt < HeaderEnd)
					{ BodyLength = FCString::Atoi(*SoFar.Mid(LenAt + 15, HeaderEnd - LenAt - 15)); }
				}
				if (Received.Num() >= BodyStart + BodyLength)
				{ break; }
			}

			if (!_CannedResponse.IsEmpty())
			{
				const FTCHARToUTF8 Utf8(*_CannedResponse);
				int32 Offset = 0;
				while (Offset < Utf8.Length())
				{
					int32 Sent = 0;
					if (!Conn->Send(reinterpret_cast<const uint8*>(Utf8.Get()) + Offset, Utf8.Length() - Offset, Sent) || Sent <= 0)
					{ break; }
					Offset += Sent;
				}
			}
			else
			{
				// Never answer: hold the connection until the test is done with it.
				while (!_bStop && FPlatformTime::Seconds() < Deadline)
				{ FPlatformProcess::Sleep(0.02f); }
			}

			Conn->Close();
			_Subsystem->DestroySocket(Conn);
			return BytesToString(Received);
		}

		// Request bytes are ASCII/UTF-8; widen them byte-for-byte (FString's BytesToString is offset-encoded).
		static auto BytesToString(const TArray<uint8>& InBytes) -> FString
		{
			FString Out;
			Out.Reserve(InBytes.Num());
			for (const uint8 Byte : InBytes)
			{ Out.AppendChar(static_cast<TCHAR>(Byte)); }
			return Out;
		}

		FString              _CannedResponse;
		ISocketSubsystem*    _Subsystem = nullptr;
		FSocket*             _Listener  = nullptr;
		int32                _Port      = 0;
		std::atomic<bool>    _bStop     { false };
		TFuture<FString>     _Served;
	};
}

// --------------------------------------------------------------------------------------------------------------------
// A server that answers promptly is seen promptly, with the game thread blocked in the call for its whole duration.
// --------------------------------------------------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGitLinkTests_LfsHttp_BlockingRequest_CompletesWhileGameThreadBlocked,
	"GitLink.LfsHttp.BlockingRequest.CompletesWhileGameThreadBlocked",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGitLinkTests_LfsHttp_BlockingRequest_CompletesWhileGameThreadBlocked::RunTest(const FString& /*Parameters*/)
{
	// The whole point is that nothing ticks FHttpManager during the call. Off the game thread this proves nothing.
	if (!TestTrue(TEXT("precondition: the test body runs on the game thread"), IsInGameThread()))
	{ return false; }

	const FString ResponseBody = TEXT("{\"ours\":[],\"theirs\":[]}");
	FLoopbackHttpServer Server(FString::Printf(
		TEXT("HTTP/1.1 200 OK\r\n")
		TEXT("Content-Type: application/vnd.git-lfs+json\r\n")
		TEXT("X-RateLimit-Remaining: 2999\r\n")
		TEXT("Content-Length: %d\r\n")
		TEXT("Connection: close\r\n")
		TEXT("\r\n%s"),
		FTCHARToUTF8(*ResponseBody).Length(), *ResponseBody));
	if (!TestTrue(TEXT("loopback server is listening"), Server.Is_Listening()))
	{ return false; }

	constexpr float TimeoutSec = 10.0f;
	const double Start = FPlatformTime::Seconds();
	const gitlink::lfs_http::detail::FBlockingHttpResult Result = gitlink::lfs_http::detail::Execute_BlockingRequest(
		TEXT("POST"),
		Server.Get_Url(TEXT("/info/lfs/locks/verify")),
		{ { TEXT("Content-Type"), TEXT("application/vnd.git-lfs+json") } },
		TEXT("{\"ref\":{\"name\":\"refs/heads/main\"}}"),
		TimeoutSec);
	const double Elapsed = FPlatformTime::Seconds() - Start;

	const FString Request = Server.Get_ReceivedRequest();

	TestTrue(TEXT("request completed (not the wait backstop)"), Result.bCompleted);
	TestTrue(TEXT("transport succeeded"), Result.bSuccess);
	TestEqual(TEXT("status"), Result.HttpStatus, 200);
	TestEqual(TEXT("body round-trips"), Result.ResponseBody, ResponseBody);
	TestEqual(TEXT("rate-limit header is surfaced"), Result.RateLimitRemaining, FString(TEXT("2999")));
	TestTrue(TEXT("absent Retry-After reads empty"), Result.RetryAfter.IsEmpty());

	// Pre-fix this is >= TimeoutSec: the answer sat on the HTTP thread waiting for a game-thread tick.
	TestTrue(FString::Printf(TEXT("answered in well under the %.0f s timeout (took %.2f s)"), TimeoutSec, Elapsed),
		Elapsed < TimeoutSec * 0.5);

	// And it sent what it was asked to.
	TestTrue(TEXT("server saw the verb and path"), Request.StartsWith(TEXT("POST /info/lfs/locks/verify ")));
	TestTrue(TEXT("server saw the header"), Request.Contains(TEXT("application/vnd.git-lfs+json")));
	TestTrue(TEXT("server saw the body"), Request.EndsWith(TEXT("{\"ref\":{\"name\":\"refs/heads/main\"}}")));
	return true;
}

// --------------------------------------------------------------------------------------------------------------------
// A server that never answers fails at the ENGINE's timeout, delivered the same way. The primitive's own wait has a
// 2 s backstop past the request timeout; landing there (bCompleted=false) means the timeout completion was also
// stuck behind the blocked game thread.
// --------------------------------------------------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGitLinkTests_LfsHttp_BlockingRequest_SilentServerFailsAtEngineTimeout,
	"GitLink.LfsHttp.BlockingRequest.SilentServerFailsAtEngineTimeout",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGitLinkTests_LfsHttp_BlockingRequest_SilentServerFailsAtEngineTimeout::RunTest(const FString& /*Parameters*/)
{
	if (!TestTrue(TEXT("precondition: the test body runs on the game thread"), IsInGameThread()))
	{ return false; }

	FLoopbackHttpServer Server{ FString() };
	if (!TestTrue(TEXT("loopback server is listening"), Server.Is_Listening()))
	{ return false; }

	// The engine logs its own timeout; that is the expected outcome here, not noise.
	AddExpectedMessagePlain(TEXT("HTTP request timed out"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);

	const gitlink::lfs_http::detail::FBlockingHttpResult Result = gitlink::lfs_http::detail::Execute_BlockingRequest(
		TEXT("GET"),
		Server.Get_Url(TEXT("/info/lfs/locks?path=Content%2FSeed.uasset")),
		{},
		FString(),
		/*InTimeoutSec=*/ 2.0f);
	Server.Stop();

	TestTrue(TEXT("the engine's timeout completed the request (not the wait backstop)"), Result.bCompleted);
	TestFalse(TEXT("a request that timed out is not a success"), Result.bSuccess && Result.HttpStatus == 200);
	return true;
}

#endif  // WITH_DEV_AUTOMATION_TESTS
