#pragma once

#include <CoreMinimal.h>
#include <Engine/DeveloperSettings.h>
#include <UObject/Object.h>

#include "GitLink_Settings.generated.h"

// --------------------------------------------------------------------------------------------------------------------
// GitLink editor settings. Appears in Project Settings > Editor > GitLink.
//
// Kept intentionally minimal — the plugin reads these once at provider init and again whenever
// the user clicks Apply in the settings widget. No runtime reconfiguration beyond that.
// --------------------------------------------------------------------------------------------------------------------

UCLASS(config=Editor, DefaultConfig, meta=(DisplayName="GitLink"))
class GITLINK_API UGitLink_Settings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	UGitLink_Settings();

	// UDeveloperSettings ---------------------------------------------------------------------------------------------
	auto GetCategoryName() const -> FName override;
	auto GetSectionText() const -> FText override;
	auto GetSectionDescription() const -> FText override;

	// Settings -------------------------------------------------------------------------------------------------------

	/** Override path to the root of the git working tree. Leave empty to auto-discover from the project directory upward. */
	UPROPERTY(config, EditAnywhere, Category="GitLink", meta=(DisplayName="Repository Root Override"))
	FString RepositoryRootOverride;

	/** Enable the background fetch + status poll. */
	UPROPERTY(config, EditAnywhere, Category="GitLink|Background Poll", meta=(DisplayName="Enable Background Poll"))
	bool bEnableBackgroundPoll = true;

	/** Interval in seconds between background fetches. 0 disables the fetch but leaves status refresh enabled.
	 *  Stage B raised the default from 30s → 120s: foreground LFS lock freshness (≤2s on user-touched files)
	 *  is driven by editor delegates (focus, asset-open, package-dirty, pre-checkout) firing single-file HTTP
	 *  probes, so the background sweep no longer needs to be the latency floor for "someone else locked this". */
	UPROPERTY(config, EditAnywhere, Category="GitLink|Background Poll", meta=(DisplayName="Poll Interval (seconds)", ClampMin="0", UIMin="0"))
	int32 PollIntervalSeconds = 120;

	/** When true, routes commit/push through a git.exe subprocess if the repo has active hooks so they actually run. */
	UPROPERTY(config, EditAnywhere, Category="GitLink|Hooks", meta=(DisplayName="Fall Back to git.exe for Hooked Operations"))
	bool bSubprocessFallbackForHooks = true;

	/** When true, LFS lock/unlock is performed via a git.exe subprocess (libgit2 doesn't cover LFS). */
	UPROPERTY(config, EditAnywhere, Category="GitLink|LFS", meta=(DisplayName="Use LFS File Locking"))
	bool bUseLfsLocking = true;

	/** Path to the git.exe binary used for subprocess fallback + LFS. Empty auto-discovers via PATH. */
	UPROPERTY(config, EditAnywhere, Category="GitLink", meta=(DisplayName="git Binary Override"))
	FString GitBinaryOverride;
};
