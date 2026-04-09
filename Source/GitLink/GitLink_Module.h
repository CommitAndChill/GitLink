#pragma once

#include "GitLink/GitLink_Provider.h"

#include <CoreMinimal.h>
#include <Modules/ModuleManager.h>

// --------------------------------------------------------------------------------------------------------------------

class GITLINK_API FGitLinkModule : public IModuleInterface
{
public:
	auto StartupModule() -> void override;
	auto ShutdownModule() -> void override;

	static auto Get() -> FGitLinkModule&;

	auto Get_Provider() -> FGitLink_Provider& { return _Provider; }

private:
	FGitLink_Provider _Provider;
	bool _bModularFeatureRegistered = false;
};
