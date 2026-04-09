#include "GitLinkCore_Module.h"

#include "GitLinkCoreLog.h"

#if WITH_LIBGIT2
#include <git2.h>
#endif

// --------------------------------------------------------------------------------------------------------------------

#define LOCTEXT_NAMESPACE "FGitLinkCoreModule"

namespace
{
	bool GLibgit2Initialized = false;
}

auto FGitLinkCoreModule::StartupModule() -> void
{
#if WITH_LIBGIT2
	const int32 InitResult = git_libgit2_init();
	if (InitResult < 0)
	{
		UE_LOG(LogGitLinkCore, Error,
			TEXT("git_libgit2_init() failed with code %d; GitLink provider will self-disable."),
			InitResult);
		return;
	}

	_Libgit2Initialized = true;
	GLibgit2Initialized = true;

	int MajorVer = 0;
	int MinorVer = 0;
	int RevVer   = 0;
	git_libgit2_version(&MajorVer, &MinorVer, &RevVer);

	UE_LOG(LogGitLinkCore, Log,
		TEXT("GitLinkCore: libgit2 %d.%d.%d initialized (init count=%d)"),
		MajorVer, MinorVer, RevVer, InitResult);
#else
	UE_LOG(LogGitLinkCore, Warning,
		TEXT("GitLinkCore: compiled without libgit2 (WITH_LIBGIT2=0). ")
		TEXT("Drop libgit2 binaries into Plugins/GitLink/Source/ThirdParty/libgit2/ and rebuild."));
#endif
}

auto FGitLinkCoreModule::ShutdownModule() -> void
{
#if WITH_LIBGIT2
	if (_Libgit2Initialized)
	{
		git_libgit2_shutdown();
		_Libgit2Initialized = false;
		GLibgit2Initialized = false;
		UE_LOG(LogGitLinkCore, Log, TEXT("GitLinkCore: libgit2 shutdown"));
	}
#endif
}

auto FGitLinkCoreModule::IsLibgit2Available() -> bool
{
	return GLibgit2Initialized;
}

IMPLEMENT_MODULE(FGitLinkCoreModule, GitLinkCore);

#undef LOCTEXT_NAMESPACE
