#pragma once

#include <CoreMinimal.h>
#include <Modules/ModuleManager.h>

// --------------------------------------------------------------------------------------------------------------------

class GITLINK_API FGitLinkModule : public IModuleInterface
{
public:
	auto StartupModule() -> void override;
	auto ShutdownModule() -> void override;

	static auto Get() -> FGitLinkModule&;

private:
	bool _ProviderRegistered = false;
};
