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

	// --- Index manipulation ---
	GITLINKCORE_API auto StagePaths     (FRepository& InRepo, const TArray<FString>& InPaths) -> FResult;
	GITLINKCORE_API auto UnstagePaths   (FRepository& InRepo, const TArray<FString>& InPaths) -> FResult;
	GITLINKCORE_API auto StageAll       (FRepository& InRepo) -> FResult;
	GITLINKCORE_API auto UnstageAll     (FRepository& InRepo) -> FResult;
	GITLINKCORE_API auto DiscardChanges (FRepository& InRepo, const TArray<FString>& InPaths) -> FResult;
}
