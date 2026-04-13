#include "GitLink_Module.h"

#include "GitLinkLog.h"

#include <Features/IModularFeatures.h>
#include <Interfaces/IPluginManager.h>
#include <ISourceControlModule.h>

// --------------------------------------------------------------------------------------------------------------------

#define LOCTEXT_NAMESPACE "FGitLinkModule"

auto FGitLinkModule::StartupModule() -> void
{
	// Log the plugin version from the .uplugin descriptor so it's easy to identify which build
	// is running when reading logs.
	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("GitLink"));
	if (Plugin.IsValid())
	{
		UE_LOG(LogGitLink, Log, TEXT("GitLink v%s: module startup"), *Plugin->GetDescriptor().VersionName);
	}
	else
	{
		UE_LOG(LogGitLink, Log, TEXT("GitLink: module startup"));
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
