#include "GitLinkCore_Module.h"

#include "GitLinkCoreLog.h"

#include <HAL/PlatformProcess.h>
#include <Interfaces/IPluginManager.h>
#include <Misc/Paths.h>

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
	// git2.dll is delay-loaded and staged beside this module in the plugin's Binaries/<Platform>
	// (libgit2.Build.cs). The delay-load helper resolves it with the default DLL search, which does not
	// include plugin Binaries directories, so in a Shared build environment (modules load from the
	// engine's Binaries) the first libgit2 call below died with 0xc06d007e unless a stray git2.dll sat
	// beside the editor executable. Load it by full path first: the delay-load then binds to the
	// already-loaded module, and a missing DLL self-disables the provider instead of crashing.
	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("GitLink"));
	if (!Plugin.IsValid())
	{
		UE_LOG(LogGitLinkCore, Error,
			TEXT("GitLinkCore: plugin 'GitLink' not found; cannot locate git2.dll. GitLink provider will self-disable."));
		return;
	}

	const FString DllPath = FPaths::ConvertRelativePathToFull(FPaths::Combine(
		Plugin->GetBaseDir(), TEXT("Binaries"), FPlatformProcess::GetBinariesSubdirectory(), TEXT("git2.dll")));

	_Libgit2DllHandle = FPlatformProcess::GetDllHandle(*DllPath);
	if (_Libgit2DllHandle == nullptr)
	{
		UE_LOG(LogGitLinkCore, Error,
			TEXT("GitLinkCore: failed to load %s; GitLink provider will self-disable. Rebuild the editor to restage it."),
			*DllPath);
		return;
	}

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

	if (_Libgit2DllHandle != nullptr)
	{
		FPlatformProcess::FreeDllHandle(_Libgit2DllHandle);
		_Libgit2DllHandle = nullptr;
	}
#endif
}

auto FGitLinkCoreModule::IsLibgit2Available() -> bool
{
	return GLibgit2Initialized;
}

IMPLEMENT_MODULE(FGitLinkCoreModule, GitLinkCore);

#undef LOCTEXT_NAMESPACE
