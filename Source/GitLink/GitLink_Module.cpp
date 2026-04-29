#include "GitLink_Module.h"

#include "GitLink/GitLink_Version.h"
#include "GitLinkLog.h"

#include <Features/IModularFeatures.h>
#include <Interfaces/IPluginManager.h>
#include <ISourceControlModule.h>

// --------------------------------------------------------------------------------------------------------------------

#define LOCTEXT_NAMESPACE "FGitLinkModule"

auto FGitLinkModule::StartupModule() -> void
{
	// Log compile-time version + build timestamp first — these come from GITLINK_VERSION /
	// GITLINK_BUILD_TIMESTAMP baked into this TU at compile time, so they pin the version of the
	// loaded DLL regardless of what the .uplugin descriptor says (the two are bumped in lockstep,
	// but a stale binary in Binaries/ would otherwise lie). Then log the runtime descriptor
	// version — a mismatch between the two is the signal "you forgot to rebuild".
	UE_LOG(LogGitLink, Log,
		TEXT("GitLink v%s (built %hs): module startup"),
		GITLINK_VERSION,
		GITLINK_BUILD_TIMESTAMP);

	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("GitLink"));
	if (Plugin.IsValid())
	{
		const FString DescriptorVersion = Plugin->GetDescriptor().VersionName;
		if (DescriptorVersion != FString(GITLINK_VERSION))
		{
			UE_LOG(LogGitLink, Warning,
				TEXT("GitLink: .uplugin descriptor reports v%s but compiled binary is v%s — ")
				TEXT("rebuild the plugin to bring the binary in sync, or bump GITLINK_VERSION ")
				TEXT("in GitLink_Version.h to match the descriptor."),
				*DescriptorVersion, GITLINK_VERSION);
		}
	}

	// Register the provider as a modular feature so ISourceControlModule can discover it and
	// offer it in the revision-control provider dropdown. Registration must come BEFORE the
	// provider is selected; Init() is only called by ISourceControlModule::SetProvider when the
	// user picks us.
	IModularFeatures::Get().RegisterModularFeature(TEXT("SourceControl"), &_Provider);
	_bModularFeatureRegistered = true;
}

auto FGitLinkModule::ShutdownModule() -> void
{
	if (_bModularFeatureRegistered)
	{
		_Provider.Close();
		IModularFeatures::Get().UnregisterModularFeature(TEXT("SourceControl"), &_Provider);
		_bModularFeatureRegistered = false;
	}

	UE_LOG(LogGitLink, Log, TEXT("GitLink: module shutdown"));
}

auto FGitLinkModule::Get() -> FGitLinkModule&
{
	return FModuleManager::LoadModuleChecked<FGitLinkModule>(TEXT("GitLink"));
}

IMPLEMENT_MODULE(FGitLinkModule, GitLink);

#undef LOCTEXT_NAMESPACE
