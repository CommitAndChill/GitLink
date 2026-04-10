#include "Operations/GitLink_Ops.h"

#include "GitLinkCoreLog.h"

#if WITH_LIBGIT2
#include "Libgit2/GitLink_Libgit2_Error.h"
#include "Libgit2/GitLink_Libgit2_Handles.h"
#include "Libgit2/GitLink_Libgit2_Oid.h"
#include <git2.h>
#endif

// --------------------------------------------------------------------------------------------------------------------
// op::Get_BlobSizeAtCommit — resolves a commit, walks its tree to find the blob at the given
// path, and returns the raw size in bytes. Used to populate FGitLink_Revision::_FileSize so
// the editor's History dialog shows a real size instead of "0.0 B".
// --------------------------------------------------------------------------------------------------------------------

namespace gitlink::op
{
	auto Get_BlobSizeAtCommit(FRepository& InRepo, const FString& InCommitHash, const FString& InRepoRelativePath) -> int64
	{
#if WITH_LIBGIT2
		if (!InRepo.IsOpen() || InCommitHash.IsEmpty() || InRepoRelativePath.IsEmpty())
		{ return 0; }

		FScopeLock Lock(&InRepo.Get_Mutex());

		git_repository* Raw = InRepo.Get_RawHandle();

		// Resolve the commit OID.
		git_oid CommitOid;
		if (!libgit2::OidFromString(InCommitHash, CommitOid))
		{ return 0; }

		git_commit* RawCommit = nullptr;
		if (git_commit_lookup(&RawCommit, Raw, &CommitOid) < 0 || RawCommit == nullptr)
		{
			if (RawCommit) { git_commit_free(RawCommit); }
			return 0;
		}
		libgit2::FCommitPtr Commit(RawCommit);

		// Get the commit's tree.
		git_tree* RawTree = nullptr;
		if (git_commit_tree(&RawTree, Commit.Get()) < 0 || RawTree == nullptr)
		{
			if (RawTree) { git_tree_free(RawTree); }
			return 0;
		}
		libgit2::FTreePtr Tree(RawTree);

		// Look up the entry at the path.
		const FTCHARToUTF8 PathUtf8(*InRepoRelativePath);
		git_tree_entry* RawEntry = nullptr;
		if (git_tree_entry_bypath(&RawEntry, Tree.Get(), PathUtf8.Get()) < 0 || RawEntry == nullptr)
		{
			if (RawEntry) { git_tree_entry_free(RawEntry); }
			return 0;
		}

		const git_oid* BlobOid = git_tree_entry_id(RawEntry);
		if (BlobOid == nullptr)
		{
			git_tree_entry_free(RawEntry);
			return 0;
		}

		// Look up the blob and get its size.
		git_blob* RawBlob = nullptr;
		if (git_blob_lookup(&RawBlob, Raw, BlobOid) < 0 || RawBlob == nullptr)
		{
			if (RawBlob) { git_blob_free(RawBlob); }
			git_tree_entry_free(RawEntry);
			return 0;
		}

		const int64 Size = static_cast<int64>(git_blob_rawsize(RawBlob));

		git_blob_free(RawBlob);
		git_tree_entry_free(RawEntry);

		return Size;
#else
		return 0;
#endif
	}
}
