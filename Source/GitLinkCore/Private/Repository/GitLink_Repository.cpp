#include "GitLinkCore/Repository/GitLink_Repository.h"

#include "GitLinkCore_Module.h"
#include "GitLinkCoreLog.h"
#include "Operations/GitLink_Ops.h"

#if WITH_LIBGIT2
#include "Libgit2/GitLink_Libgit2_Error.h"
#include "Libgit2/GitLink_Libgit2_Handles.h"
#include "Libgit2/GitLink_Libgit2_Oid.h"
#include <git2.h>
#endif

// --------------------------------------------------------------------------------------------------------------------
// FRepository — concrete libgit2-backed repository.
//
// Only Open() / Close() / IsOpen() / Get_Path() / Get_CurrentBranchName() are functional at this point.
// Everything else returns a "not yet implemented" FResult or an empty collection. Each op family is
// filled in as its dedicated Op_*.cpp file lands in a follow-up commit.
// --------------------------------------------------------------------------------------------------------------------

namespace gitlink
{
	namespace
	{
		FString GLastOpenError;

		auto NotImplemented(const TCHAR* InOp) -> FResult
		{
			return FResult::Fail(FString::Printf(
				TEXT("%s: not yet implemented in this build of GitLinkCore"), InOp));
		}
	}

	auto FRepository::Get_LastOpenError() -> const FString&
	{
		return GLastOpenError;
	}

	// ----------------------------------------------------------------------------------------------------------------
	auto FRepository::Open(const FOpenParams& InParams) -> TUniquePtr<FRepository>
	{
		GLastOpenError.Reset();

		if (InParams.Path.IsEmpty())
		{
			GLastOpenError = TEXT("FRepository::Open: empty path");
			return nullptr;
		}

#if WITH_LIBGIT2
		if (!FGitLinkCoreModule::IsLibgit2Available())
		{
			GLastOpenError = TEXT("FRepository::Open: libgit2 module not initialized");
			return nullptr;
		}

		const FTCHARToUTF8 Utf8Path(*InParams.Path);

		git_repository* RawRepo = nullptr;
		int32 Rc = 0;

		if (InParams.bNoDiscover)
		{
			Rc = git_repository_open(&RawRepo, Utf8Path.Get());
		}
		else
		{
			Rc = git_repository_open_ext(
				&RawRepo,
				Utf8Path.Get(),
				GIT_REPOSITORY_OPEN_CROSS_FS,
				/*ceiling_dirs=*/ nullptr);
		}

		if (Rc < 0 || RawRepo == nullptr)
		{
			GLastOpenError = FString::Printf(
				TEXT("git_repository_open[_ext] failed (rc=%d): %s"),
				Rc,
				*libgit2::Get_LastErrorMessage());
			UE_LOG(LogGitLinkCore, Warning, TEXT("%s"), *GLastOpenError);
			return nullptr;
		}

		UE_LOG(LogGitLinkCore, Log, TEXT("FRepository: opened '%s'"), *InParams.Path);

		// Resolve the actual working tree path that libgit2 settled on (may differ from the input
		// when bNoDiscover was false and the input was a subdirectory).
		FString ResolvedPath = InParams.Path;
		if (const char* WorkDir = git_repository_workdir(RawRepo))
		{
			ResolvedPath = FString(UTF8_TO_TCHAR(WorkDir));
		}

		// Private ctor — wrap in TUniquePtr manually because MakeUnique can't see the private ctor.
		return TUniquePtr<FRepository>(new FRepository(MoveTemp(ResolvedPath), RawRepo));
#else
		GLastOpenError = TEXT("FRepository::Open: compiled without libgit2 (WITH_LIBGIT2=0)");
		UE_LOG(LogGitLinkCore, Warning, TEXT("%s"), *GLastOpenError);
		return nullptr;
#endif
	}

	// ----------------------------------------------------------------------------------------------------------------
	FRepository::FRepository(FString InPath, git_repository* InRepo)
		: _Path(MoveTemp(InPath))
		, _Repo(InRepo)
	{
	}

	FRepository::~FRepository()
	{
#if WITH_LIBGIT2
		if (_Repo != nullptr)
		{
			FScopeLock Lock(&_RepoMutex);
			git_repository_free(_Repo);
			_Repo = nullptr;
			UE_LOG(LogGitLinkCore, Log, TEXT("FRepository: closed '%s'"), *_Path);
		}
#endif
	}

	auto FRepository::IsOpen() const -> bool
	{
		return _Repo != nullptr;
	}

	// ----------------------------------------------------------------------------------------------------------------
	// Implemented: HEAD short name.
	// ----------------------------------------------------------------------------------------------------------------
	auto FRepository::Get_CurrentBranchName() -> FString
	{
#if WITH_LIBGIT2
		if (!IsOpen())
		{ return FString(); }

		FScopeLock Lock(&_RepoMutex);

		git_reference* RawHead = nullptr;
		const int32 Rc = git_repository_head(&RawHead, _Repo);
		if (Rc < 0 || RawHead == nullptr)
		{
			// Detached HEAD, unborn branch, empty repo, etc. — not a caller-facing error.
			if (RawHead) { git_reference_free(RawHead); }
			return FString();
		}

		libgit2::FReferencePtr Head(RawHead);

		const char* ShortName = nullptr;
		if (git_branch_name(&ShortName, Head.Get()) == 0 && ShortName != nullptr)
		{
			return FString(UTF8_TO_TCHAR(ShortName));
		}

		// Fallback: the full reference name.
		if (const char* FullName = git_reference_name(Head.Get()))
		{
			return FString(UTF8_TO_TCHAR(FullName));
		}

		return FString();
#else
		return FString();
#endif
	}

	// ----------------------------------------------------------------------------------------------------------------
	// Status delegates to op::ComputeStatus in Private/Operations/GitLink_Op_Status.cpp.
	// ----------------------------------------------------------------------------------------------------------------
	auto FRepository::Get_Status() -> FStatus
	{
		return op::ComputeStatus(*this);
	}

	// ----------------------------------------------------------------------------------------------------------------
	// Stubs — each lands in its own Op_*.cpp file in a follow-up commit.
	// ----------------------------------------------------------------------------------------------------------------

	auto FRepository::Stage(const TArray<FString>& InPaths)          -> FResult { return op::StagePaths(*this, InPaths); }
	auto FRepository::Unstage(const TArray<FString>& InPaths)        -> FResult { return op::UnstagePaths(*this, InPaths); }
	auto FRepository::StageAll()                                     -> FResult { return op::StageAll(*this); }
	auto FRepository::UnstageAll()                                   -> FResult { return op::UnstageAll(*this); }
	auto FRepository::DiscardChanges(const TArray<FString>& InPaths) -> FResult { return op::DiscardChanges(*this, InPaths); }

	auto FRepository::Commit(const FCommitParams& InParams) -> FResult { return op::CreateCommit(*this, InParams); }

	auto FRepository::Get_Log(const FLogQuery& InQuery) -> TArray<FCommit>
	{
		return op::WalkLog(*this, InQuery);
	}

	auto FRepository::Get_Branches() -> TArray<FBranch>
	{
		return op::ListBranches(*this);
	}

	auto FRepository::Get_Remotes() -> TArray<FRemote>
	{
		return op::ListRemotes(*this);
	}

	auto FRepository::Fetch(const FFetchParams& InParams, FProgressCallback InProgress) -> FResult
	{
		return op::FetchRemote(*this, InParams, MoveTemp(InProgress));
	}

	auto FRepository::Push(const FPushParams& InParams, FProgressCallback InProgress) -> FResult
	{
		return op::PushRemote(*this, InParams, MoveTemp(InProgress));
	}

	auto FRepository::PullFastForward(const FFetchParams& InParams, FProgressCallback InProgress) -> FResult
	{
		return op::PullFastForward(*this, InParams, MoveTemp(InProgress));
	}
}
