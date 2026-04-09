#include "Operations/GitLink_Ops.h"

#include "GitLinkCoreLog.h"

#if WITH_LIBGIT2
#include "Libgit2/GitLink_Libgit2_Error.h"
#include "Libgit2/GitLink_Libgit2_Handles.h"
#include "Libgit2/GitLink_Libgit2_Oid.h"
#include <git2.h>
#endif

// --------------------------------------------------------------------------------------------------------------------
// op::WalkLog — revwalk from a start point (HEAD by default) collecting FCommit value structs.
//
// Path filtering (FLogQuery::PathFilter) is NOT yet implemented — libgit2's revwalk doesn't do
// it natively; you'd have to diff each commit against its parents. A log warning is emitted when
// it's set so callers know to wait for the follow-up. For now PathFilter is silently ignored.
// --------------------------------------------------------------------------------------------------------------------

namespace gitlink::op
{
#if WITH_LIBGIT2
	namespace
	{
		auto MakeSignature(const git_signature* InSig) -> FSignature
		{
			FSignature Out;
			if (InSig == nullptr)
			{ return Out; }

			if (InSig->name  != nullptr) { Out.Name  = FString(UTF8_TO_TCHAR(InSig->name));  }
			if (InSig->email != nullptr) { Out.Email = FString(UTF8_TO_TCHAR(InSig->email)); }

			// git_time.time is seconds since unix epoch.
			Out.When = FDateTime::FromUnixTimestamp(static_cast<int64>(InSig->when.time));
			return Out;
		}

		auto Build_FCommit(git_commit* InCommit) -> FCommit
		{
			FCommit Out;

			const git_oid* Oid = git_commit_id(InCommit);
			if (Oid != nullptr)
			{
				Out.Hash      = libgit2::OidToString(*Oid);
				Out.ShortHash = libgit2::OidToShortString(*Oid);
			}

			const unsigned int ParentCount = git_commit_parentcount(InCommit);
			Out.ParentHashes.Reserve(static_cast<int32>(ParentCount));
			for (unsigned int ParentIdx = 0; ParentIdx < ParentCount; ++ParentIdx)
			{
				const git_oid* ParentOid = git_commit_parent_id(InCommit, ParentIdx);
				if (ParentOid != nullptr)
				{
					Out.ParentHashes.Add(libgit2::OidToString(*ParentOid));
				}
			}

			Out.Author    = MakeSignature(git_commit_author(InCommit));
			Out.Committer = MakeSignature(git_commit_committer(InCommit));

			if (const char* Summary = git_commit_summary(InCommit))
			{
				Out.Summary = FString(UTF8_TO_TCHAR(Summary));
			}
			if (const char* Message = git_commit_message(InCommit))
			{
				Out.Message = FString(UTF8_TO_TCHAR(Message));
			}

			return Out;
		}
	}
#endif  // WITH_LIBGIT2

	auto WalkLog(FRepository& InRepo, const FLogQuery& InQuery) -> TArray<FCommit>
	{
		TArray<FCommit> Out;

#if WITH_LIBGIT2
		if (!InRepo.IsOpen())
		{ return Out; }

		if (!InQuery.PathFilter.IsEmpty())
		{
			UE_LOG(LogGitLinkCore, Warning,
				TEXT("WalkLog: PathFilter='%s' not yet implemented — walking full history"),
				*InQuery.PathFilter);
		}

		FScopeLock Lock(&InRepo.Get_Mutex());

		git_repository* Raw = InRepo.Get_RawHandle();

		git_revwalk* RawWalk = nullptr;
		if (const int32 Rc = git_revwalk_new(&RawWalk, Raw); Rc < 0)
		{
			if (RawWalk) { git_revwalk_free(RawWalk); }
			UE_LOG(LogGitLinkCore, Warning, TEXT("WalkLog: git_revwalk_new failed: %s"),
				*libgit2::Get_LastErrorMessage());
			return Out;
		}
		libgit2::FRevwalkPtr Walk(RawWalk);

		git_revwalk_sorting(Walk.Get(), GIT_SORT_TIME);

		int32 PushRc;
		if (InQuery.StartRef.IsEmpty())
		{
			PushRc = git_revwalk_push_head(Walk.Get());
		}
		else
		{
			const FTCHARToUTF8 StartUtf8(*InQuery.StartRef);
			git_object* StartObj = nullptr;
			if (git_revparse_single(&StartObj, Raw, StartUtf8.Get()) < 0 || StartObj == nullptr)
			{
				UE_LOG(LogGitLinkCore, Warning,
					TEXT("WalkLog: could not resolve StartRef '%s': %s"),
					*InQuery.StartRef, *libgit2::Get_LastErrorMessage());
				return Out;
			}
			PushRc = git_revwalk_push(Walk.Get(), git_object_id(StartObj));
			git_object_free(StartObj);
		}

		if (PushRc < 0)
		{
			UE_LOG(LogGitLinkCore, Warning, TEXT("WalkLog: revwalk_push failed: %s"),
				*libgit2::Get_LastErrorMessage());
			return Out;
		}

		const int32 MaxCount = InQuery.MaxCount > 0 ? InQuery.MaxCount : TNumericLimits<int32>::Max();
		Out.Reserve(FMath::Min(MaxCount, 256));

		git_oid NextOid;
		while (Out.Num() < MaxCount && git_revwalk_next(&NextOid, Walk.Get()) == 0)
		{
			git_commit* RawCommit = nullptr;
			if (git_commit_lookup(&RawCommit, Raw, &NextOid) < 0 || RawCommit == nullptr)
			{
				if (RawCommit) { git_commit_free(RawCommit); }
				continue;
			}
			libgit2::FCommitPtr Commit(RawCommit);
			Out.Add(Build_FCommit(Commit.Get()));
		}

		UE_LOG(LogGitLinkCore, Verbose, TEXT("WalkLog: returned %d commit(s)"), Out.Num());
#endif  // WITH_LIBGIT2

		return Out;
	}
}
