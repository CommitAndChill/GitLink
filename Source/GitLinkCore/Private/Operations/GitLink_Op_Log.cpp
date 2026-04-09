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

		FScopeLock Lock(&InRepo.Get_Mutex());

		git_repository* Raw = InRepo.Get_RawHandle();

		// Path filter setup: we resolve the path once here (converting to UTF-8) and then
		// in the walk loop we compare the blob OID at PathFilter in each commit's tree against
		// the blob OID at PathFilter in the first parent's tree. If they differ (or one is
		// missing), the commit touched the file and we include it in the result. This is cheap
		// because tree lookups are fast and we never materialize diffs.
		const bool bFilterByPath = !InQuery.PathFilter.IsEmpty();
		FTCHARToUTF8 PathFilterUtf8(bFilterByPath ? *InQuery.PathFilter : TEXT(""));

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

		// Writes the blob OID of PathFilter in InCommit's tree into OutOid and returns true,
		// or returns false (leaving OutOid untouched) when the path is not present or an error
		// occurs. Caller must NOT read OutOid on a false return.
		auto BlobAt = [PathUtf8 = bFilterByPath ? PathFilterUtf8.Get() : nullptr](
			git_commit* InCommit, git_oid& OutOid) -> bool
		{
			if (InCommit == nullptr || PathUtf8 == nullptr)
			{ return false; }

			git_tree* RawTree = nullptr;
			if (git_commit_tree(&RawTree, InCommit) < 0 || RawTree == nullptr)
			{
				if (RawTree) { git_tree_free(RawTree); }
				return false;
			}
			libgit2::FTreePtr Tree(RawTree);

			git_tree_entry* RawEntry = nullptr;
			if (git_tree_entry_bypath(&RawEntry, Tree.Get(), PathUtf8) < 0 || RawEntry == nullptr)
			{
				if (RawEntry) { git_tree_entry_free(RawEntry); }
				return false;
			}
			if (const git_oid* Oid = git_tree_entry_id(RawEntry))
			{
				OutOid = *Oid;
				git_tree_entry_free(RawEntry);
				return true;
			}
			git_tree_entry_free(RawEntry);
			return false;
		};

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

			if (bFilterByPath)
			{
				git_oid MyBlob;
				const bool bHaveMine = BlobAt(Commit.Get(), MyBlob);

				// Walk parents. A commit is considered to have touched the file when its blob
				// OID at PathFilter differs from EVERY parent's blob OID at PathFilter, OR when
				// the file exists in this commit but not in any parent (initial add / rename-in),
				// OR when the file existed in a parent but is now gone (rename-out / delete).
				bool bTouchedFile = false;
				const unsigned int ParentCount = git_commit_parentcount(Commit.Get());
				if (ParentCount == 0)
				{
					// Root commit: it "touched" the file iff the file exists in its tree.
					bTouchedFile = bHaveMine;
				}
				else
				{
					bool bMatchesAnyParent = false;
					for (unsigned int ParentIdx = 0; ParentIdx < ParentCount; ++ParentIdx)
					{
						git_commit* RawParent = nullptr;
						if (git_commit_parent(&RawParent, Commit.Get(), ParentIdx) < 0 || RawParent == nullptr)
						{
							if (RawParent) { git_commit_free(RawParent); }
							continue;
						}
						libgit2::FCommitPtr Parent(RawParent);

						git_oid ParentBlob;
						const bool bHaveParent = BlobAt(Parent.Get(), ParentBlob);

						if (bHaveMine && bHaveParent && git_oid_equal(&MyBlob, &ParentBlob) != 0)
						{
							// This parent has the same blob at that path — commit is a no-op
							// relative to this parent.
							bMatchesAnyParent = true;
							break;
						}
						if (!bHaveMine && !bHaveParent)
						{
							// Neither has the file — also a no-op relative to this parent.
							bMatchesAnyParent = true;
							break;
						}
					}
					bTouchedFile = !bMatchesAnyParent;
				}

				if (!bTouchedFile)
				{ continue; }
			}

			Out.Add(Build_FCommit(Commit.Get()));
		}

		UE_LOG(LogGitLinkCore, Verbose,
			TEXT("WalkLog: returned %d commit(s)%s"),
			Out.Num(),
			bFilterByPath ? TEXT(" (path-filtered)") : TEXT(""));
#endif  // WITH_LIBGIT2

		return Out;
	}
}
