#include "Operations/GitLink_Ops.h"

#include "GitLinkCoreLog.h"

#if WITH_LIBGIT2
#include "Libgit2/GitLink_Libgit2_Error.h"
#include "Libgit2/GitLink_Libgit2_Handles.h"
#include "Libgit2/GitLink_Libgit2_Oid.h"
#include <git2.h>
#endif

// --------------------------------------------------------------------------------------------------------------------
// op::ListBranches — enumerates local and remote-tracking branches.
// --------------------------------------------------------------------------------------------------------------------

namespace gitlink::op
{
	auto ListBranches(FRepository& InRepo) -> TArray<FBranch>
	{
		TArray<FBranch> Out;

#if WITH_LIBGIT2
		if (!InRepo.IsOpen())
		{ return Out; }

		FScopeLock Lock(&InRepo.Get_Mutex());

		git_repository* Raw = InRepo.Get_RawHandle();

		git_branch_iterator* RawIter = nullptr;
		if (const int32 Rc = git_branch_iterator_new(&RawIter, Raw, GIT_BRANCH_ALL); Rc < 0)
		{
			if (RawIter) { git_branch_iterator_free(RawIter); }
			UE_LOG(LogGitLinkCore, Warning, TEXT("ListBranches: git_branch_iterator_new failed: %s"),
				*libgit2::Get_LastErrorMessage());
			return Out;
		}
		libgit2::FBranchIterPtr Iter(RawIter);

		for (;;)
		{
			git_reference* RawRef = nullptr;
			git_branch_t   BranchType = GIT_BRANCH_LOCAL;

			const int32 Rc = git_branch_next(&RawRef, &BranchType, Iter.Get());
			if (Rc == GIT_ITEROVER)
			{ break; }
			if (Rc < 0 || RawRef == nullptr)
			{
				if (RawRef) { git_reference_free(RawRef); }
				UE_LOG(LogGitLinkCore, Warning, TEXT("ListBranches: git_branch_next failed: %s"),
					*libgit2::Get_LastErrorMessage());
				break;
			}
			libgit2::FReferencePtr Ref(RawRef);

			FBranch Branch;
			Branch.bIsRemote = (BranchType == GIT_BRANCH_REMOTE);

			if (const char* FullName = git_reference_name(Ref.Get()))
			{
				Branch.FullRef = FString(UTF8_TO_TCHAR(FullName));
			}

			const char* ShortName = nullptr;
			if (git_branch_name(&ShortName, Ref.Get()) == 0 && ShortName != nullptr)
			{
				Branch.Name = FString(UTF8_TO_TCHAR(ShortName));
			}

			if (!Branch.bIsRemote)
			{
				// git_branch_upstream returns GIT_ENOTFOUND when there's no upstream — that's fine.
				git_reference* RawUpstream = nullptr;
				if (git_branch_upstream(&RawUpstream, Ref.Get()) == 0 && RawUpstream != nullptr)
				{
					libgit2::FReferencePtr Upstream(RawUpstream);
					const char* UpstreamShort = nullptr;
					if (git_branch_name(&UpstreamShort, Upstream.Get()) == 0 && UpstreamShort != nullptr)
					{
						Branch.UpstreamName = FString(UTF8_TO_TCHAR(UpstreamShort));
					}
				}

				// Is this the HEAD branch?
				Branch.bIsHead = git_branch_is_head(Ref.Get()) == 1;
			}

			// Resolve tip OID.
			git_object* TipObj = nullptr;
			if (git_reference_peel(&TipObj, Ref.Get(), GIT_OBJECT_COMMIT) == 0 && TipObj != nullptr)
			{
				if (const git_oid* TipOid = git_object_id(TipObj))
				{
					Branch.TipHash = libgit2::OidToString(*TipOid);
				}
				git_object_free(TipObj);
			}

			Out.Add(MoveTemp(Branch));
		}

		UE_LOG(LogGitLinkCore, Verbose, TEXT("ListBranches: %d branch(es)"), Out.Num());
#endif  // WITH_LIBGIT2

		return Out;
	}
}
