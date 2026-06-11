#include "Operations/GitLink_Ops.h"

#include "GitLinkCoreLog.h"

#if WITH_LIBGIT2
#include "Libgit2/GitLink_Libgit2_Error.h"
#include "Libgit2/GitLink_Libgit2_Handles.h"
#include <git2.h>
#endif

// --------------------------------------------------------------------------------------------------------------------
// op::FetchRemote / op::PushRemote — network operations with progress + credentials.
//
// Credentials policy (v1):
//   - HTTPS: we return GIT_PASSTHROUGH from the credential callback, which causes libgit2 to fall
//     back to its transport layer. Because the libgit2 binaries are built with USE_HTTPS=WinHTTP,
//     HTTPS auth is handled by Windows WinHTTP, which picks up credentials from the Windows
//     Credential Manager (same store `git` uses via git-credential-manager). No plaintext secrets
//     ever pass through the plugin.
//   - SSH: unsupported in v1. Our libgit2 build has USE_SSH=OFF (see Source/ThirdParty/libgit2/
//     README.md), so the credential callback will report an error for SSH-typed requests. Rebuild
//     libgit2 with SSH and add key-loading logic if needed.
//
// Progress:
//   - Fetch calls user's FProgressCallback from git_remote_fetch's transfer_progress hook. Return
//     false from the user callback to request cancellation (we translate to a non-zero return
//     from the libgit2 hook, which aborts the operation).
//   - Push uses push_transfer_progress hook, same contract.
// --------------------------------------------------------------------------------------------------------------------

namespace gitlink::op
{
#if WITH_LIBGIT2
	namespace
	{
		// Shared payload wired into both fetch_options and push_options so the libgit2 callbacks
		// can find their way back to the caller's FProgressCallback and observe cancellation.
		struct FCallbackPayload
		{
			FProgressCallback UserProgress;
			bool              bCancelRequested = false;
			FString           Stage;

			// Set by PushUpdateReference_Cb when the server rejects a ref update. git_remote_push
			// still returns 0 in that case, so PushRemote must check this after the call.
			FString           PushRefError;
		};

		// ---- Credential ----
		int CredentialAcquire_Cb(
			git_credential** OutCred,
			const char*      InUrl,
			const char*      /*InUsernameFromUrl*/,
			unsigned int     InAllowedTypes,
			void*            /*Payload*/)
		{
			// HTTPS userpass: hand off to the transport. WinHTTP picks up credentials from the
			// Windows credential store automatically, so GIT_PASSTHROUGH is the right answer.
			if ((InAllowedTypes & GIT_CREDENTIAL_USERPASS_PLAINTEXT) != 0)
			{
				*OutCred = nullptr;
				return GIT_PASSTHROUGH;
			}

			// Default credential flow (NTLM / Negotiate on Windows with WinHTTP) is preferred for
			// intranet HTTPS servers that expect integrated Windows auth.
			if ((InAllowedTypes & GIT_CREDENTIAL_DEFAULT) != 0)
			{
				return git_credential_default_new(OutCred);
			}

			UE_LOG(LogGitLinkCore, Warning,
				TEXT("CredentialAcquire: unsupported auth type(s) 0x%X for '%s' ")
				TEXT("(SSH requires libgit2 built with USE_SSH=ON)"),
				InAllowedTypes,
				InUrl ? UTF8_TO_TCHAR(InUrl) : TEXT("(null)"));
			return GIT_EUSER;
		}

		// ---- Fetch progress ----
		int FetchTransferProgress_Cb(const git_indexer_progress* InStats, void* InPayload)
		{
			auto* Payload = static_cast<FCallbackPayload*>(InPayload);
			if (Payload == nullptr || !Payload->UserProgress)
			{ return 0; }

			FProgressInfo Info;
			Info.TotalObjects    = InStats != nullptr ? InStats->total_objects    : 0u;
			Info.ReceivedObjects = InStats != nullptr ? InStats->received_objects : 0u;
			Info.IndexedObjects  = InStats != nullptr ? InStats->indexed_objects  : 0u;
			Info.ReceivedBytes   = InStats != nullptr ? InStats->received_bytes   : 0ull;
			Info.Stage           = Payload->Stage;

			const bool bContinue = Payload->UserProgress(Info);
			if (!bContinue)
			{
				Payload->bCancelRequested = true;
				return GIT_EUSER;  // aborts the fetch
			}
			return 0;
		}

		// ---- Push progress ----
		int PushTransferProgress_Cb(unsigned int InCurrent, unsigned int InTotal, size_t InBytes, void* InPayload)
		{
			auto* Payload = static_cast<FCallbackPayload*>(InPayload);
			if (Payload == nullptr || !Payload->UserProgress)
			{ return 0; }

			FProgressInfo Info;
			Info.TotalObjects    = InTotal;
			Info.ReceivedObjects = InCurrent;
			Info.IndexedObjects  = InCurrent;
			Info.ReceivedBytes   = static_cast<uint64>(InBytes);
			Info.Stage           = Payload->Stage;

			const bool bContinue = Payload->UserProgress(Info);
			if (!bContinue)
			{
				Payload->bCancelRequested = true;
				return GIT_EUSER;
			}
			return 0;
		}

		// ---- Push ref-update status ----
		// Called once per pushed ref after the server responds. status == nullptr means the ref was
		// accepted; non-null means the server rejected it (protected branch, pre-receive hook, ...)
		// and status carries the server's reason. This is the ONLY place rejections surface —
		// git_remote_push itself returns 0 regardless.
		int PushUpdateReference_Cb(const char* InRefname, const char* InStatus, void* InPayload)
		{
			if (InStatus == nullptr)
			{ return 0; }

			auto* Payload = static_cast<FCallbackPayload*>(InPayload);
			if (Payload == nullptr)
			{ return 0; }

			Payload->PushRefError = FString::Printf(
				TEXT("%s: %s"),
				InRefname != nullptr ? UTF8_TO_TCHAR(InRefname) : TEXT("(unknown ref)"),
				UTF8_TO_TCHAR(InStatus));
			return 0;
		}

		// Open the configured remote by name.
		auto Open_Remote(git_repository* InRepo, const FString& InRemoteName) -> git_remote*
		{
			git_remote* RawRemote = nullptr;
			const FTCHARToUTF8 NameUtf8(*InRemoteName);
			if (git_remote_lookup(&RawRemote, InRepo, NameUtf8.Get()) < 0)
			{
				if (RawRemote) { git_remote_free(RawRemote); }
				return nullptr;
			}
			return RawRemote;
		}
	}
#endif  // WITH_LIBGIT2

	// ----------------------------------------------------------------------------------------------------------------
	auto FetchRemote(FRepository& InRepo, const FFetchParams& InParams, FProgressCallback InProgress) -> FResult
	{
#if WITH_LIBGIT2
		if (!InRepo.IsOpen())
		{ return FResult::Fail(TEXT("FetchRemote: repository not open")); }

		FScopeLock Lock(&InRepo.Get_Mutex());

		git_repository* Raw = InRepo.Get_RawHandle();

		libgit2::FRemotePtr Remote(Open_Remote(Raw, InParams.RemoteName));
		if (!Remote.IsValid())
		{
			return FResult::Fail(FString::Printf(
				TEXT("FetchRemote: remote '%s' not found: %s"),
				*InParams.RemoteName, *libgit2::Get_LastErrorMessage()));
		}

		FCallbackPayload Payload;
		Payload.UserProgress = MoveTemp(InProgress);
		Payload.Stage        = TEXT("Fetching");

		git_fetch_options Opts;
		if (const int32 Rc = git_fetch_options_init(&Opts, GIT_FETCH_OPTIONS_VERSION); Rc < 0)
		{
			return libgit2::MakeFailResult(TEXT("FetchRemote: git_fetch_options_init"), Rc);
		}

		Opts.callbacks.credentials      = &CredentialAcquire_Cb;
		Opts.callbacks.transfer_progress = &FetchTransferProgress_Cb;
		Opts.callbacks.payload           = &Payload;
		Opts.prune                       = InParams.bPrune ? GIT_FETCH_PRUNE : GIT_FETCH_NO_PRUNE;

		const int32 Rc = git_remote_fetch(Remote.Get(), /*refspecs=*/ nullptr, &Opts, /*reflog_msg=*/ "GitLink fetch");
		if (Rc < 0)
		{
			if (Payload.bCancelRequested)
			{ return FResult::Fail(TEXT("FetchRemote: cancelled by progress callback")); }

			return libgit2::MakeFailResult(TEXT("FetchRemote: git_remote_fetch"), Rc);
		}

		UE_LOG(LogGitLinkCore, Log, TEXT("FetchRemote: '%s' completed"), *InParams.RemoteName);
		return FResult::Ok();
#else
		return FResult::Fail(TEXT("FetchRemote: compiled without libgit2"));
#endif
	}

	// ----------------------------------------------------------------------------------------------------------------
	auto PushRemote(FRepository& InRepo, const FPushParams& InParams, FProgressCallback InProgress) -> FResult
	{
#if WITH_LIBGIT2
		if (!InRepo.IsOpen())
		{ return FResult::Fail(TEXT("PushRemote: repository not open")); }

		FScopeLock Lock(&InRepo.Get_Mutex());

		git_repository* Raw = InRepo.Get_RawHandle();

		// Resolve the branch to push. Empty = current HEAD branch.
		FString BranchName = InParams.BranchName;
		if (BranchName.IsEmpty())
		{
			BranchName = InRepo.Get_CurrentBranchName();
			if (BranchName.IsEmpty())
			{
				return FResult::Fail(TEXT("PushRemote: cannot infer branch — HEAD is detached or unborn"));
			}
		}

		libgit2::FRemotePtr Remote(Open_Remote(Raw, InParams.RemoteName));
		if (!Remote.IsValid())
		{
			return FResult::Fail(FString::Printf(
				TEXT("PushRemote: remote '%s' not found: %s"),
				*InParams.RemoteName, *libgit2::Get_LastErrorMessage()));
		}

		// Build refspec: "[+]refs/heads/<branch>:refs/heads/<branch>"
		const FString RefspecStr = FString::Printf(
			TEXT("%srefs/heads/%s:refs/heads/%s"),
			InParams.bForce ? TEXT("+") : TEXT(""),
			*BranchName,
			*BranchName);

		const FTCHARToUTF8 RefspecUtf8(*RefspecStr);
		char* RefspecPtrs[1] = { const_cast<char*>(RefspecUtf8.Get()) };
		git_strarray RefspecArray;
		RefspecArray.strings = RefspecPtrs;
		RefspecArray.count   = 1;

		FCallbackPayload Payload;
		Payload.UserProgress = MoveTemp(InProgress);
		Payload.Stage        = TEXT("Pushing");

		git_push_options Opts;
		if (const int32 Rc = git_push_options_init(&Opts, GIT_PUSH_OPTIONS_VERSION); Rc < 0)
		{
			return libgit2::MakeFailResult(TEXT("PushRemote: git_push_options_init"), Rc);
		}

		Opts.callbacks.credentials           = &CredentialAcquire_Cb;
		Opts.callbacks.push_transfer_progress = &PushTransferProgress_Cb;
		Opts.callbacks.push_update_reference  = &PushUpdateReference_Cb;
		Opts.callbacks.payload                = &Payload;

		const int32 Rc = git_remote_push(Remote.Get(), &RefspecArray, &Opts);
		if (Rc < 0)
		{
			if (Payload.bCancelRequested)
			{ return FResult::Fail(TEXT("PushRemote: cancelled by progress callback")); }

			return libgit2::MakeFailResult(TEXT("PushRemote: git_remote_push"), Rc);
		}

		// git_remote_push returning 0 only means the transport round-trip succeeded — server-side
		// ref rejections are reported via the push_update_reference callback.
		if (!Payload.PushRefError.IsEmpty())
		{
			return FResult::Fail(FString::Printf(
				TEXT("PushRemote: server rejected ref update — %s"), *Payload.PushRefError));
		}

		UE_LOG(LogGitLinkCore, Log,
			TEXT("PushRemote: '%s' -> '%s'%s completed"),
			*BranchName,
			*InParams.RemoteName,
			InParams.bForce ? TEXT(" (force)") : TEXT(""));
		return FResult::Ok();
#else
		return FResult::Fail(TEXT("PushRemote: compiled without libgit2"));
#endif
	}

	// ----------------------------------------------------------------------------------------------------------------
	auto PullFastForward(FRepository& InRepo, const FFetchParams& InParams, FProgressCallback InProgress) -> FResult
	{
#if WITH_LIBGIT2
		// 1. Fetch first. Reuses the full fetch op so progress + credentials work identically.
		if (const FResult Fetched = FetchRemote(InRepo, InParams, InProgress); !Fetched)
		{
			return Fetched;
		}

		FScopeLock Lock(&InRepo.Get_Mutex());

		git_repository* Raw = InRepo.Get_RawHandle();

		// 2. Resolve HEAD. Must be a branch ref, not detached.
		git_reference* RawHead = nullptr;
		if (const int32 Rc = git_repository_head(&RawHead, Raw); Rc < 0)
		{
			if (RawHead) { git_reference_free(RawHead); }
			return libgit2::MakeFailResult(TEXT("PullFastForward: git_repository_head"), Rc);
		}
		libgit2::FReferencePtr Head(RawHead);

		if (git_reference_is_branch(Head.Get()) == 0)
		{
			return FResult::Fail(TEXT("PullFastForward: HEAD is not a branch (detached HEAD)"));
		}

		// 3. Get the upstream ref (e.g. refs/remotes/origin/main).
		git_reference* RawUpstream = nullptr;
		if (const int32 Rc = git_branch_upstream(&RawUpstream, Head.Get()); Rc < 0)
		{
			if (RawUpstream) { git_reference_free(RawUpstream); }
			return FResult::Fail(FString::Printf(
				TEXT("PullFastForward: no upstream configured for current branch: %s"),
				*libgit2::Get_LastErrorMessage()));
		}
		libgit2::FReferencePtr Upstream(RawUpstream);

		// 4. Convert upstream ref to an annotated commit for merge_analysis.
		git_annotated_commit* RawTheirsHead = nullptr;
		if (const int32 Rc = git_annotated_commit_from_ref(&RawTheirsHead, Raw, Upstream.Get()); Rc < 0)
		{
			if (RawTheirsHead) { git_annotated_commit_free(RawTheirsHead); }
			return libgit2::MakeFailResult(TEXT("PullFastForward: git_annotated_commit_from_ref"), Rc);
		}
		libgit2::FAnnotatedCommitPtr TheirsHead(RawTheirsHead);

		const git_annotated_commit* TheirsHeadConst = TheirsHead.Get();
		git_merge_analysis_t   Analysis   = GIT_MERGE_ANALYSIS_NONE;
		git_merge_preference_t Preference = GIT_MERGE_PREFERENCE_NONE;

		const int32 AnaRc = git_merge_analysis(&Analysis, &Preference, Raw, &TheirsHeadConst, 1);
		if (AnaRc < 0)
		{
			return libgit2::MakeFailResult(TEXT("PullFastForward: git_merge_analysis"), AnaRc);
		}

		// 5. Act on the analysis result.
		if ((Analysis & GIT_MERGE_ANALYSIS_UP_TO_DATE) != 0)
		{
			UE_LOG(LogGitLinkCore, Log, TEXT("PullFastForward: already up to date"));
			return FResult::Ok();
		}

		if ((Analysis & GIT_MERGE_ANALYSIS_FASTFORWARD) == 0)
		{
			return FResult::Fail(TEXT(
				"PullFastForward: non-fast-forward merge required. "
				"Use `git pull` or `git merge` from a terminal to resolve manually."));
		}

		// 6. Fast-forward: checkout the upstream tree FIRST, then move the branch ref. The order
		//    matters — a SAFE checkout can fail on a dirty working tree, and if the ref had already
		//    moved by then the repo would be stranded in a half-pulled state.
		const git_oid* TheirsOid = git_annotated_commit_id(TheirsHead.Get());

		git_commit* RawTheirsCommit = nullptr;
		if (const int32 Rc = git_commit_lookup(&RawTheirsCommit, Raw, TheirsOid); Rc < 0)
		{
			if (RawTheirsCommit) { git_commit_free(RawTheirsCommit); }
			return libgit2::MakeFailResult(TEXT("PullFastForward: git_commit_lookup"), Rc);
		}
		libgit2::FCommitPtr TheirsCommit(RawTheirsCommit);

		git_tree* RawTheirsTree = nullptr;
		if (const int32 Rc = git_commit_tree(&RawTheirsTree, TheirsCommit.Get()); Rc < 0)
		{
			if (RawTheirsTree) { git_tree_free(RawTheirsTree); }
			return libgit2::MakeFailResult(TEXT("PullFastForward: git_commit_tree"), Rc);
		}
		libgit2::FTreePtr TheirsTree(RawTheirsTree);

		git_checkout_options CheckoutOpts;
		if (const int32 Rc = git_checkout_options_init(&CheckoutOpts, GIT_CHECKOUT_OPTIONS_VERSION); Rc < 0)
		{
			return libgit2::MakeFailResult(TEXT("PullFastForward: git_checkout_options_init"), Rc);
		}
		CheckoutOpts.checkout_strategy = GIT_CHECKOUT_SAFE;  // safer than FORCE; errors if local dirty

		// git_checkout_tree takes any treeish git_object*; a git_tree IS-A git_object in libgit2.
		const auto* TheirsTreeAsObject = reinterpret_cast<const git_object*>(TheirsTree.Get());
		if (const int32 Rc = git_checkout_tree(Raw, TheirsTreeAsObject, &CheckoutOpts); Rc < 0)
		{
			return libgit2::MakeFailResult(
				TEXT("PullFastForward: git_checkout_tree (local modifications would be overwritten?)"),
				Rc);
		}

		// 7. Working tree now matches upstream — safe to point HEAD's ref at the upstream OID.
		git_reference* RawNewHead = nullptr;
		const int32 SetTargetRc = git_reference_set_target(
			&RawNewHead, Head.Get(), TheirsOid, "GitLink fast-forward");
		if (SetTargetRc < 0 || RawNewHead == nullptr)
		{
			if (RawNewHead) { git_reference_free(RawNewHead); }
			return libgit2::MakeFailResult(TEXT("PullFastForward: git_reference_set_target"), SetTargetRc);
		}
		libgit2::FReferencePtr NewHead(RawNewHead);

		UE_LOG(LogGitLinkCore, Log, TEXT("PullFastForward: fast-forwarded HEAD"));
		return FResult::Ok();
#else
		return FResult::Fail(TEXT("PullFastForward: compiled without libgit2"));
#endif
	}
}
