#pragma once

#include <CoreMinimal.h>

// --------------------------------------------------------------------------------------------------------------------
// RAII wrappers for libgit2 C handles, so we can use TUniquePtr<git_xxx, Deleter> throughout
// the GitLinkCore .cpp files instead of hand-rolling free() calls in every function.
//
// Private header — only GitLinkCore .cpp files may include this. It pulls in git2.h.
// --------------------------------------------------------------------------------------------------------------------

#if WITH_LIBGIT2

#include <git2.h>
#include <Templates/UniquePtr.h>

namespace gitlink::libgit2
{
	struct FGitRepositoryDeleter  { void operator()(git_repository*  P) const { if (P) { git_repository_free(P);  } } };
	struct FGitIndexDeleter       { void operator()(git_index*       P) const { if (P) { git_index_free(P);       } } };
	struct FGitTreeDeleter        { void operator()(git_tree*        P) const { if (P) { git_tree_free(P);        } } };
	struct FGitCommitDeleter      { void operator()(git_commit*      P) const { if (P) { git_commit_free(P);      } } };
	struct FGitReferenceDeleter   { void operator()(git_reference*   P) const { if (P) { git_reference_free(P);   } } };
	struct FGitRemoteDeleter      { void operator()(git_remote*      P) const { if (P) { git_remote_free(P);      } } };
	struct FGitStatusListDeleter  { void operator()(git_status_list* P) const { if (P) { git_status_list_free(P); } } };
	struct FGitRevwalkDeleter     { void operator()(git_revwalk*     P) const { if (P) { git_revwalk_free(P);     } } };
	struct FGitSignatureDeleter   { void operator()(git_signature*   P) const { if (P) { git_signature_free(P);   } } };
	struct FGitBranchIteratorDeleter { void operator()(git_branch_iterator* P) const { if (P) { git_branch_iterator_free(P); } } };

	using FRepoPtr       = TUniquePtr<git_repository,      FGitRepositoryDeleter>;
	using FIndexPtr      = TUniquePtr<git_index,           FGitIndexDeleter>;
	using FTreePtr       = TUniquePtr<git_tree,            FGitTreeDeleter>;
	using FCommitPtr     = TUniquePtr<git_commit,          FGitCommitDeleter>;
	using FReferencePtr  = TUniquePtr<git_reference,       FGitReferenceDeleter>;
	using FRemotePtr     = TUniquePtr<git_remote,          FGitRemoteDeleter>;
	using FStatusListPtr = TUniquePtr<git_status_list,     FGitStatusListDeleter>;
	using FRevwalkPtr    = TUniquePtr<git_revwalk,         FGitRevwalkDeleter>;
	using FSignaturePtr  = TUniquePtr<git_signature,       FGitSignatureDeleter>;
	using FBranchIterPtr = TUniquePtr<git_branch_iterator, FGitBranchIteratorDeleter>;
}

#endif  // WITH_LIBGIT2
