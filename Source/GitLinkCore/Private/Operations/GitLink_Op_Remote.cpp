#include "Operations/GitLink_Ops.h"

#include "GitLinkCoreLog.h"

#if WITH_LIBGIT2
#include "Libgit2/GitLink_Libgit2_Error.h"
#include "Libgit2/GitLink_Libgit2_Handles.h"
#include <git2.h>
#endif

// --------------------------------------------------------------------------------------------------------------------
// op::ListRemotes — enumerates configured remotes with their fetch and push URLs.
// --------------------------------------------------------------------------------------------------------------------

namespace gitlink::op
{
	auto ListRemotes(FRepository& InRepo) -> TArray<FRemote>
	{
		TArray<FRemote> Out;

#if WITH_LIBGIT2
		if (!InRepo.IsOpen())
		{ return Out; }

		FScopeLock Lock(&InRepo.Get_Mutex());

		git_repository* Raw = InRepo.Get_RawHandle();

		git_strarray Names = {0};
		if (const int32 Rc = git_remote_list(&Names, Raw); Rc < 0)
		{
			UE_LOG(LogGitLinkCore, Warning, TEXT("ListRemotes: git_remote_list failed: %s"),
				*libgit2::Get_LastErrorMessage());
			git_strarray_dispose(&Names);
			return Out;
		}

		Out.Reserve(static_cast<int32>(Names.count));

		for (size_t NameIdx = 0; NameIdx < Names.count; ++NameIdx)
		{
			const char* RawName = Names.strings[NameIdx];
			if (RawName == nullptr)
			{ continue; }

			git_remote* RawRemote = nullptr;
			if (git_remote_lookup(&RawRemote, Raw, RawName) < 0 || RawRemote == nullptr)
			{
				if (RawRemote) { git_remote_free(RawRemote); }
				continue;
			}
			libgit2::FRemotePtr Remote(RawRemote);

			FRemote Entry;
			Entry.Name = FString(UTF8_TO_TCHAR(RawName));

			if (const char* FetchUrl = git_remote_url(Remote.Get()))
			{
				Entry.FetchUrl = FString(UTF8_TO_TCHAR(FetchUrl));
			}
			if (const char* PushUrl = git_remote_pushurl(Remote.Get()))
			{
				Entry.PushUrl = FString(UTF8_TO_TCHAR(PushUrl));
			}
			if (Entry.PushUrl.IsEmpty())
			{
				// No explicit pushurl configured -> push goes to the fetch URL.
				Entry.PushUrl = Entry.FetchUrl;
			}

			Out.Add(MoveTemp(Entry));
		}

		git_strarray_dispose(&Names);

		UE_LOG(LogGitLinkCore, Verbose, TEXT("ListRemotes: %d remote(s)"), Out.Num());
#endif  // WITH_LIBGIT2

		return Out;
	}
}
