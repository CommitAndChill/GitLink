#include "Operations/GitLink_Ops.h"

#include "GitLinkCoreLog.h"

#if WITH_LIBGIT2
#include "Libgit2/GitLink_Libgit2_Error.h"
#include "Libgit2/GitLink_Libgit2_Handles.h"
#include "Libgit2/GitLink_Libgit2_Oid.h"
#include <git2.h>
#endif

// --------------------------------------------------------------------------------------------------------------------
// op::CreateCommit — builds a commit object from the current index and advances HEAD.
//
// Flow:
//   1. Get the repository index
//   2. git_index_write_tree -> OID of a tree representing the index
//   3. git_tree_lookup to resolve the tree object
//   4. Resolve HEAD: if it exists, that's our parent commit; if unborn, no parent
//   5. If !bAllowEmpty and we have a parent, bail when the new tree OID matches
//      the parent's tree OID (nothing to commit)
//   6. Build signature via git_signature_default (reads user.name/user.email) or
//      git_signature_now when the caller passed explicit values
//   7. git_commit_create with ref="HEAD" to advance the current branch
//
// Amend flow (bAmend=true):
//   - Requires HEAD to exist
//   - Uses git_commit_amend, which rewrites the commit HEAD points at
// --------------------------------------------------------------------------------------------------------------------

namespace gitlink::op
{
#if WITH_LIBGIT2
	namespace
	{
		// Build a git_signature from FCommitParams. Caller owns the returned pointer and must
		// git_signature_free() it. Returns a failing FResult in OutErr and nullptr on failure.
		auto Build_Signature(
			git_repository* InRepo,
			const FCommitParams& InParams,
			FResult& OutErr) -> git_signature*
		{
			git_signature* Sig = nullptr;

			if (!InParams.AuthorName.IsEmpty() && !InParams.AuthorEmail.IsEmpty())
			{
				const FTCHARToUTF8 NameUtf8(*InParams.AuthorName);
				const FTCHARToUTF8 EmailUtf8(*InParams.AuthorEmail);
				const int32 Rc = git_signature_now(&Sig, NameUtf8.Get(), EmailUtf8.Get());
				if (Rc < 0)
				{
					if (Sig) { git_signature_free(Sig); Sig = nullptr; }
					OutErr = libgit2::MakeFailResult(TEXT("CreateCommit: git_signature_now"), Rc);
					return nullptr;
				}
				return Sig;
			}

			const int32 Rc = git_signature_default(&Sig, InRepo);
			if (Rc < 0)
			{
				if (Sig) { git_signature_free(Sig); Sig = nullptr; }
				OutErr = libgit2::MakeFailResult(
					TEXT("CreateCommit: git_signature_default (is user.name / user.email configured?)"),
					Rc);
				return nullptr;
			}
			return Sig;
		}
	}
#endif  // WITH_LIBGIT2

	auto CreateCommit(FRepository& InRepo, const FCommitParams& InParams) -> FResult
	{
#if WITH_LIBGIT2
		if (!InRepo.IsOpen())
		{ return FResult::Fail(TEXT("CreateCommit: repository not open")); }

		if (InParams.Message.IsEmpty())
		{ return FResult::Fail(TEXT("CreateCommit: commit message is empty")); }

		FScopeLock Lock(&InRepo.Get_Mutex());

		git_repository* Raw = InRepo.Get_RawHandle();

		// 1. Write index -> tree OID.
		git_index* RawIndex = nullptr;
		if (const int32 Rc = git_repository_index(&RawIndex, Raw); Rc < 0)
		{
			if (RawIndex) { git_index_free(RawIndex); }
			return libgit2::MakeFailResult(TEXT("CreateCommit: git_repository_index"), Rc);
		}
		libgit2::FIndexPtr Index(RawIndex);

		git_oid TreeOid;
		if (const int32 Rc = git_index_write_tree(&TreeOid, Index.Get()); Rc < 0)
		{
			return libgit2::MakeFailResult(TEXT("CreateCommit: git_index_write_tree"), Rc);
		}

		// 2. Look up the tree object.
		git_tree* RawTree = nullptr;
		if (const int32 Rc = git_tree_lookup(&RawTree, Raw, &TreeOid); Rc < 0)
		{
			if (RawTree) { git_tree_free(RawTree); }
			return libgit2::MakeFailResult(TEXT("CreateCommit: git_tree_lookup"), Rc);
		}
		libgit2::FTreePtr Tree(RawTree);

		// 3. Resolve HEAD commit (may be null on unborn HEAD).
		git_commit* RawParent = nullptr;
		{
			git_object* HeadObj = nullptr;
			const int32 Rc = git_revparse_single(&HeadObj, Raw, "HEAD");
			if (Rc == 0 && HeadObj != nullptr)
			{
				// Peel to commit if we got something else (e.g. an annotated tag).
				git_object* Peeled = nullptr;
				if (git_object_peel(&Peeled, HeadObj, GIT_OBJECT_COMMIT) == 0 && Peeled != nullptr)
				{
					RawParent = reinterpret_cast<git_commit*>(Peeled);
				}
				git_object_free(HeadObj);
			}
			else if (HeadObj)
			{
				git_object_free(HeadObj);
			}
		}
		libgit2::FCommitPtr Parent(RawParent);

		// 4. Empty-commit check (only when we have a parent to compare against).
		if (!InParams.bAllowEmpty && !InParams.bAmend && Parent.IsValid())
		{
			const git_tree* ParentTree = nullptr;
			if (git_commit_tree(const_cast<git_tree**>(&ParentTree), Parent.Get()) == 0 && ParentTree != nullptr)
			{
				const git_oid* ParentTreeOid = git_tree_id(ParentTree);
				const bool bSameTree = git_oid_equal(ParentTreeOid, &TreeOid) != 0;
				git_tree_free(const_cast<git_tree*>(ParentTree));

				if (bSameTree)
				{
					return FResult::Fail(TEXT("CreateCommit: nothing to commit (index matches HEAD tree)"));
				}
			}
		}

		// 5. Amend guard.
		if (InParams.bAmend && !Parent.IsValid())
		{
			return FResult::Fail(TEXT("CreateCommit: cannot amend — HEAD has no commit yet"));
		}

		// 6. Build signature.
		FResult SigErr;
		git_signature* RawSig = Build_Signature(Raw, InParams, SigErr);
		if (RawSig == nullptr)
		{ return SigErr; }
		libgit2::FSignaturePtr Sig(RawSig);

		// 7. Commit.
		const FTCHARToUTF8 MessageUtf8(*InParams.Message);
		git_oid NewCommitOid;

		int32 CommitRc = 0;
		if (InParams.bAmend)
		{
			CommitRc = git_commit_amend(
				&NewCommitOid,
				Parent.Get(),
				/*update_ref=*/ "HEAD",
				Sig.Get(),
				Sig.Get(),
				/*message_encoding=*/ nullptr,
				MessageUtf8.Get(),
				Tree.Get());
		}
		else
		{
			const git_commit* ParentArray[1] = { Parent.Get() };
			const size_t ParentCount = Parent.IsValid() ? 1u : 0u;

			CommitRc = git_commit_create(
				&NewCommitOid,
				Raw,
				/*update_ref=*/ "HEAD",
				Sig.Get(),
				Sig.Get(),
				/*message_encoding=*/ nullptr,
				MessageUtf8.Get(),
				Tree.Get(),
				ParentCount,
				Parent.IsValid() ? ParentArray : nullptr);
		}

		if (CommitRc < 0)
		{
			return libgit2::MakeFailResult(
				InParams.bAmend
					? TEXT("CreateCommit: git_commit_amend")
					: TEXT("CreateCommit: git_commit_create"),
				CommitRc);
		}

		UE_LOG(LogGitLinkCore, Log,
			TEXT("CreateCommit: %s '%s'"),
			InParams.bAmend ? TEXT("amended") : TEXT("created"),
			*libgit2::OidToShortString(NewCommitOid));

		return FResult::Ok();
#else
		return FResult::Fail(TEXT("CreateCommit: compiled without libgit2"));
#endif
	}
}
