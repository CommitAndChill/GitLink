#include "GitLink_Module.h"

#include "GitLinkLog.h"

// --------------------------------------------------------------------------------------------------------------------
// Stub module: the FGitLink_Provider type will be defined in Public/GitLink/GitLink_Provider.h
// once the provider skeleton lands. Until then startup/shutdown just log so the plugin loads
// cleanly and we can confirm the module is wired correctly end-to-end.
// --------------------------------------------------------------------------------------------------------------------

#define LOCTEXT_NAMESPACE "FGitLinkModule"

auto FGitLinkModule::StartupModule() -> void
{
	UE_LOG(LogGitLink, Log, TEXT("GitLink: module startup (provider registration pending)"));

	// TODO: instantiate FGitLink_Provider and register it with the SourceControl module.
}

auto FGitLinkModule::ShutdownModule() -> void
{
	if (_ProviderRegistered)
	{
		// TODO: unregister provider from ISourceControlModule.
		_ProviderRegistered = false;
	}

	UE_LOG(LogGitLink, Log, TEXT("GitLink: module shutdown"));
}

auto FGitLinkModule::Get() -> FGitLinkModule&
{
	return FModuleManager::LoadModuleChecked<FGitLinkModule>(TEXT("GitLink"));
}

IMPLEMENT_MODULE(FGitLinkModule, GitLink);

#undef LOCTEXT_NAMESPACE
