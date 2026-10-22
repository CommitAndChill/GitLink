#include "GitLinkCore_Module.h"

#include "GitLinkCoreLog.h"

#include <HAL/PlatformProcess.h>
#include <Interfaces/IPluginManager.h>
#include <Misc/Paths.h>

#if WITH_LIBGIT2
#include <git2.h>
#endif

#if WITH_LIBGIT2 && PLATFORM_WINDOWS
#include <Windows/WindowsHWrapper.h>
#endif

// --------------------------------------------------------------------------------------------------------------------

#define LOCTEXT_NAMESPACE "FGitLinkCoreModule"

namespace
{
	bool GLibgit2Initialized = false;

#if WITH_LIBGIT2
	// The file a loaded DLL was actually mapped from; reports where a bare-name load resolved.
	auto Get_LoadedDllPath(void* InDllHandle) -> FString
	{
#if PLATFORM_WINDOWS
		TArray<TCHAR> Buffer;
		Buffer.SetNumZeroed(32768);
		const DWORD Length = ::GetModuleFileNameW(static_cast<HMODULE>(InDllHandle), Buffer.GetData(), static_cast<DWORD>(Buffer.Num()));
		if (Length > 0 && Length < static_cast<DWORD>(Buffer.Num()))
		{
			return FString(Buffer.GetData());
		}
#endif
		return TEXT("<unknown path>");
	}
#endif
}

auto FGitLinkCoreModule::StartupModule() -> void
{
#if WITH_LIBGIT2
	// git2.dll is delay-loaded (libgit2.Build.cs), and the delay-load helper resolves it with the default
	// DLL search. That search does not include plugin Binaries directories (the module manager adds a
	// module's directory only around the LoadLibrary of that module), so git2.dll is loaded explicitly
	// here, before the first libgit2 call; the delay-load then binds to the already-loaded module.
	// Candidates, in order:
	//   1. The directory GitLinkCore's own binary was loaded from. RuntimeDependencies stages git2.dll
	//      into $(BinaryOutputDir), i.e. beside this module, so this is right in every build environment.
	//   2. <Plugin>/Binaries/<Platform> (the v0.9.0 location). The same directory as (1) in a normal
	//      layout, so it is skipped when identical.
	//   3. The default DLL search by bare name (executable's folder, PATH, ...), i.e. the pre-v0.9.0
	//      behaviour. Older staging put git2.dll beside the target executable, and CI cleans keep
	//      Binaries/, so a leftover copy there keeps working; the path it resolved to is logged so a stray
	//      copy is visible.
	// (2) and (3) log a Warning. Nothing found is a Warning too, not an Error: a commandlet fails on any
	// Error, and an unavailable editor-only source-control provider must not fail a cook. The provider
	// self-disables, and git_libgit2_init() is never called without a loaded DLL.
	const FString ModuleFilename = FModuleManager::Get().GetModuleFilename(TEXT("GitLinkCore"));
	const FString ModuleDirDllPath = ModuleFilename.IsEmpty()
		? FString{}
		: FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::GetPath(ModuleFilename), TEXT("git2.dll")));

	FString PluginDllPath;
	if (const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("GitLink")); Plugin.IsValid())
	{
		PluginDllPath = FPaths::ConvertRelativePathToFull(FPaths::Combine(
			Plugin->GetBaseDir(), TEXT("Binaries"), FPlatformProcess::GetBinariesSubdirectory(), TEXT("git2.dll")));
	}

	TArray<FString> TriedPaths;
	const auto TryLoad = [this, &TriedPaths](const FString& InDllPath) -> bool
	{
		TriedPaths.Add(InDllPath);
		_Libgit2DllHandle = FPlatformProcess::GetDllHandle(*InDllPath);
		return _Libgit2DllHandle != nullptr;
	};

	if (!ModuleDirDllPath.IsEmpty() && TryLoad(ModuleDirDllPath))
	{
		UE_LOG(LogGitLinkCore, Log,
			TEXT("GitLinkCore: loaded git2.dll from GitLinkCore's binary directory: %s"),
			*ModuleDirDllPath);
	}
	else if (!PluginDllPath.IsEmpty() && !FPaths::IsSamePath(PluginDllPath, ModuleDirDllPath) && TryLoad(PluginDllPath))
	{
		UE_LOG(LogGitLinkCore, Warning,
			TEXT("GitLinkCore: git2.dll is not beside GitLinkCore's binary (%s); loaded it from the plugin's Binaries: %s"),
			ModuleDirDllPath.IsEmpty() ? TEXT("<module path unknown>") : *ModuleDirDllPath, *PluginDllPath);
	}
	else if (const FString TriedFullPaths = FString::Join(TriedPaths, TEXT(", ")); TryLoad(TEXT("git2.dll")))
	{
		UE_LOG(LogGitLinkCore, Warning,
			TEXT("GitLinkCore: git2.dll not found at [%s]; loaded it through the default DLL search from %s. ")
			TEXT("This is likely a leftover copy from older staging; rebuild the editor so it is staged beside GitLinkCore."),
			*TriedFullPaths, *Get_LoadedDllPath(_Libgit2DllHandle));
	}
	else
	{
		UE_LOG(LogGitLinkCore, Warning,
			TEXT("GitLinkCore: could not load git2.dll (tried [%s] and the default DLL search); ")
			TEXT("GitLink provider will self-disable. Rebuild the editor to restage it."),
			*TriedFullPaths);
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
