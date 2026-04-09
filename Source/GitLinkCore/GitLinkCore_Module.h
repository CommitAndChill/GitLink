#pragma once

#include <CoreMinimal.h>
#include <Modules/ModuleManager.h>

// --------------------------------------------------------------------------------------------------------------------

class GITLINKCORE_API FGitLinkCoreModule : public IModuleInterface
{
public:
	auto StartupModule() -> void override;
	auto ShutdownModule() -> void override;

	static auto IsLibgit2Available() -> bool;

private:
	bool _Libgit2Initialized = false;
};
