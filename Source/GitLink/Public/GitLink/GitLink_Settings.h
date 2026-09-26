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

	/** Interval in seconds between background fetches and lock sweeps. 0 disables the fetch but leaves status refresh enabled.
	 *  Lock changes on files you are working with are picked up within seconds regardless (editor focus, asset open,
	 *  package dirty and pre-checkout each probe that file), so this only bounds how stale the rest of the tree can get. */
	UPROPERTY(config, EditAnywhere, Category="GitLink|Background Poll", meta=(DisplayName="Poll Interval (seconds)", ClampMin="0", UIMin="0"))
	int32 PollIntervalSeconds = 120;

	/** When true and the repository has active git hooks, commits run through the git command line so the hooks execute.
	 *  (Push always uses the git command line.) */
	UPROPERTY(config, EditAnywhere, Category="GitLink|Hooks", meta=(DisplayName="Run Hooks via git Command Line"))
	bool bSubprocessFallbackForHooks = true;

	/** When true, Check Out acquires a Git LFS lock and Check In / Revert release it. Requires git-lfs. */
	UPROPERTY(config, EditAnywhere, Category="GitLink|LFS", meta=(DisplayName="Use LFS File Locking"))
	bool bUseLfsLocking = true;

	/** Path to the git executable used for LFS, push and hooked commits. Leave empty to find git on PATH. */
	UPROPERTY(config, EditAnywhere, Category="GitLink", meta=(DisplayName="git Binary Override"))
	FString GitBinaryOverride;
};
