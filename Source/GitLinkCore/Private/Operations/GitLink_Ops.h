#pragma once

#include "GitLinkCore/Repository/GitLink_Repository.h"
#include "GitLinkCore/Repository/GitLink_Repository_Params.h"
#include "GitLinkCore/Types/GitLink_Progress.h"
#include "GitLinkCore/Types/GitLink_Types.h"

// --------------------------------------------------------------------------------------------------------------------
// Free-function declarations for every git operation. Each op family lives in its own .cpp file
// under Private/Operations/ (e.g. GitLink_Op_Status.cpp). FRepository's member methods are thin
// delegates that call into these.
//
// All ops take a non-const FRepository& by reference because they need to take the per-repo mutex
// and call into the raw libgit2 handle. They are internal to GitLinkCore — this header is NEVER
// included from Public/.
// --------------------------------------------------------------------------------------------------------------------

namespace gitlink::op
{
	// --- Status ---
	GITLINKCORE_API auto ComputeStatus(FRepository& InRepo) -> FStatus;

	// Enumerate every tracked file in the index as a repo-relative path. Fast (<100ms for
	// ~10k files on a warm index) because the index is memory-mapped. Used by Cmd_Connect to
	// pre-populate the state cache with Unmodified defaults for tracked files that won't
	// show up in the dirty-status snapshot.
	GITLINKCORE_API auto Enumerate_TrackedFiles(FRepository& InRepo) -> TArray<FString>;

	// --- Index manipulation ---
	GITLINKCORE_API auto StagePaths     (FRepository& InRepo, const TArray<FString>& InPaths) -> FResult;
	GITLINKCORE_API auto UnstagePaths   (FRepository& InRepo, const TArray<FString>& InPaths) -> FResult;
	GITLINKCORE_API auto StageAll       (FRepository& InRepo) -> FResult;
	GITLINKCORE_API auto UnstageAll     (FRepository& InRepo) -> FResult;
	GITLINKCORE_API auto DiscardChanges (FRepository& InRepo, const TArray<FString>& InPaths) -> FResult;

	// --- Commits ---
	GITLINKCORE_API auto CreateCommit(FRepository& InRepo, const FCommitParams& InParams) -> FResult;

	// --- History / refs / remotes ---
	GITLINKCORE_API auto WalkLog    (FRepository& InRepo, const FLogQuery& InQuery) -> TArray<FCommit>;
	GITLINKCORE_API auto ListBranches(FRepository& InRepo) -> TArray<FBranch>;
	GITLINKCORE_API auto ListRemotes (FRepository& InRepo) -> TArray<FRemote>;

	// --- Network ---
	GITLINKCORE_API auto FetchRemote    (FRepository& InRepo, const FFetchParams& InParams, FProgressCallback InProgress) -> FResult;
	GITLINKCORE_API auto PushRemote     (FRepository& InRepo, const FPushParams&  InParams, FProgressCallback InProgress) -> FResult;
	GITLINKCORE_API auto PullFastForward(FRepository& InRepo, const FFetchParams& InParams, FProgressCallback InProgress) -> FResult;

	// --- Signature ---
	// Reads user.name / user.email from git config (local repo first, then global). Returns a
	// default-constructed FSignature on error (empty Name/Email).
	GITLINKCORE_API auto Get_DefaultSignature(FRepository& InRepo) -> FSignature;

	// --- Blob queries ---
	// Returns the raw byte size of the blob at InRepoRelativePath in the commit InCommitHash.
	// Returns 0 when the path doesn't exist in that commit or the commit can't be resolved.
	GITLINKCORE_API auto Get_BlobSizeAtCommit(FRepository& InRepo, const FString& InCommitHash, const FString& InRepoRelativePath) -> int64;
}
