#pragma once

#include "GitLinkCore/Repository/GitLink_Repository_Params.h"
#include "GitLinkCore/Types/GitLink_Progress.h"
#include "GitLinkCore/Types/GitLink_Types.h"

#include <CoreMinimal.h>
#include <HAL/CriticalSection.h>
#include <Templates/UniquePtr.h>

// Forward-declare the libgit2 C type so GitLinkCore consumers never need to include git2.h.
struct git_repository;

// --------------------------------------------------------------------------------------------------------------------
// The git facade. gitlink::IRepository is an abstract interface; gitlink::FRepository is the concrete
// libgit2-backed implementation. Consumers hold a TUniquePtr<IRepository> returned from FRepository::Open.
//
// Thread safety:
//   Every call that touches the underlying git_repository* takes the owning FCriticalSection exclusively,
//   because libgit2 is NOT thread-safe for concurrent access to the same repository object. Long-running
//   reads (log walk, status enumeration) should prefer FAsyncStatusProvider, which opens a FRESH
//   git_repository* per query to side-step the stat cache.
// --------------------------------------------------------------------------------------------------------------------

namespace gitlink
{
	class GITLINKCORE_API IRepository
	{
	public:
		virtual ~IRepository() = default;

		// Query
		virtual auto Get_Path() const -> const FString& = 0;
		virtual auto IsOpen()  const -> bool = 0;

		// Status
		virtual auto Get_Status() -> FStatus = 0;

		// Index manipulation
		virtual auto Stage         (const TArray<FString>& InPaths) -> FResult = 0;
		virtual auto Unstage       (const TArray<FString>& InPaths) -> FResult = 0;
		virtual auto StageAll      ()                              -> FResult = 0;
		virtual auto UnstageAll    ()                              -> FResult = 0;
		virtual auto DiscardChanges(const TArray<FString>& InPaths) -> FResult = 0;

		// Commits
		virtual auto Commit(const FCommitParams& InParams) -> FResult = 0;

		// History
		virtual auto Get_Log(const FLogQuery& InQuery) -> TArray<FCommit> = 0;

		// Branches
		virtual auto Get_Branches()            -> TArray<FBranch> = 0;
		virtual auto Get_CurrentBranchName()   -> FString         = 0;

		// Remotes
		virtual auto Get_Remotes() -> TArray<FRemote> = 0;

		// Network
		virtual auto Fetch(const FFetchParams& InParams, FProgressCallback InProgress = nullptr) -> FResult = 0;
		virtual auto Push (const FPushParams&  InParams, FProgressCallback InProgress = nullptr) -> FResult = 0;
	};

	// --------------------------------------------------------------------------------------------------------------------
	class GITLINKCORE_API FRepository : public IRepository
	{
	public:
		// Factory: attempts to open the repository at InParams.Path. Returns a valid instance on success,
		// or nullptr if open failed (call Get_LastOpenError for details).
		static auto Open(const FOpenParams& InParams) -> TUniquePtr<FRepository>;

		// Human-readable reason the most recent Open call failed. Empty if the last call succeeded.
		static auto Get_LastOpenError() -> const FString&;

		~FRepository() override;

		FRepository(const FRepository&)            = delete;
		FRepository& operator=(const FRepository&) = delete;
		FRepository(FRepository&&)                 = delete;
		FRepository& operator=(FRepository&&)      = delete;

		// IRepository ----------------------------------------------------------------------------------------------------
		auto Get_Path() const -> const FString& override { return _Path; }
		auto IsOpen()  const -> bool override;

		auto Get_Status() -> FStatus override;

		auto Stage         (const TArray<FString>& InPaths) -> FResult override;
		auto Unstage       (const TArray<FString>& InPaths) -> FResult override;
		auto StageAll      ()                              -> FResult override;
		auto UnstageAll    ()                              -> FResult override;
		auto DiscardChanges(const TArray<FString>& InPaths) -> FResult override;

		auto Commit(const FCommitParams& InParams) -> FResult override;

		auto Get_Log(const FLogQuery& InQuery) -> TArray<FCommit> override;

		auto Get_Branches()          -> TArray<FBranch> override;
		auto Get_CurrentBranchName() -> FString         override;

		auto Get_Remotes() -> TArray<FRemote> override;

		auto Fetch(const FFetchParams& InParams, FProgressCallback InProgress = nullptr) -> FResult override;
		auto Push (const FPushParams&  InParams, FProgressCallback InProgress = nullptr) -> FResult override;

		// Access to the underlying libgit2 handle and serialising mutex.
		// Operation implementations in Private/Operations/ use these to call into libgit2 directly.
		// External code should NOT touch either.
		auto Get_RawHandle() const -> git_repository* { return _Repo; }
		auto Get_Mutex()           -> FCriticalSection& { return _RepoMutex; }

	private:
		FRepository(FString InPath, git_repository* InRepo);

		FString         _Path;
		git_repository* _Repo = nullptr;

		// FCriticalSection is recursive on all platforms UE ships on (CRITICAL_SECTION on Win,
		// PTHREAD_MUTEX_RECURSIVE elsewhere), which matches the FtxCatalyst GitKit pattern.
		mutable FCriticalSection _RepoMutex;
	};
}
