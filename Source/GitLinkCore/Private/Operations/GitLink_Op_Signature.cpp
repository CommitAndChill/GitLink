#include "Operations/GitLink_Ops.h"

#include "GitLinkCoreLog.h"

#if WITH_LIBGIT2
#include "Libgit2/GitLink_Libgit2_Error.h"
#include "Libgit2/GitLink_Libgit2_Handles.h"
#include <git2.h>
#endif

// --------------------------------------------------------------------------------------------------------------------
// op::Get_DefaultSignature — reads user.name / user.email from git config via
// git_signature_default. Used by the provider to populate the Slate settings panel and for
// audit logging. An empty signature is returned if the repo has no config; callers treat
// that as "not configured" rather than a fatal error.
// --------------------------------------------------------------------------------------------------------------------

namespace gitlink::op
{
	auto Get_DefaultSignature(FRepository& InRepo) -> FSignature
	{
		FSignature Out;

#if WITH_LIBGIT2
		if (!InRepo.IsOpen())
		{ return Out; }

		FScopeLock Lock(&InRepo.Get_Mutex());

		git_signature* RawSig = nullptr;
		if (git_signature_default(&RawSig, InRepo.Get_RawHandle()) < 0 || RawSig == nullptr)
		{
			if (RawSig) { git_signature_free(RawSig); }
			UE_LOG(LogGitLinkCore, Verbose,
				TEXT("Get_DefaultSignature: git_signature_default failed: %s"),
				*libgit2::Get_LastErrorMessage());
			return Out;
		}

		if (RawSig->name  != nullptr) { Out.Name  = FString(UTF8_TO_TCHAR(RawSig->name));  }
		if (RawSig->email != nullptr) { Out.Email = FString(UTF8_TO_TCHAR(RawSig->email)); }
		Out.When = FDateTime::FromUnixTimestamp(static_cast<int64>(RawSig->when.time));

		git_signature_free(RawSig);
#endif  // WITH_LIBGIT2

		return Out;
	}
}
