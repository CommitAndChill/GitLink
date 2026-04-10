#pragma once

#include <CoreMinimal.h>

// --------------------------------------------------------------------------------------------------------------------
// FGitLink_HookProbe — scans `.git/hooks/` at connect time to detect which git hook scripts
// exist. libgit2 silently ignores hooks, so any operation that has a corresponding hook must
// be routed through `FGitLink_Subprocess` (git.exe) instead of the in-process libgit2 path
// so the hook actually runs.
//
// Probing is a single directory enumeration at connect time; the result is cached for the
// lifetime of the connection. Reconnecting re-probes.
//
// Supported hooks and which operations they gate:
//   pre-commit     -> Cmd_CheckIn (commit phase)
//   commit-msg     -> Cmd_CheckIn (commit phase)
//   pre-push       -> Cmd_Sync (push phase, if auto-push is implemented)
//   post-checkout  -> Cmd_Revert (working tree restore)
//   post-merge     -> Cmd_Sync (pull/merge phase)
//
// In v1, only pre-commit and commit-msg are acted on. The rest are detected and logged but
// don't yet switch the operation to subprocess mode.
// --------------------------------------------------------------------------------------------------------------------

struct FGitLink_HookFlags
{
	bool bHasPreCommit    = false;
	bool bHasCommitMsg    = false;
	bool bHasPrePush      = false;
	bool bHasPostCheckout = false;
	bool bHasPostMerge    = false;

	auto NeedsSubprocessForCommit() const -> bool { return bHasPreCommit || bHasCommitMsg; }
	auto NeedsSubprocessForPush()   const -> bool { return bHasPrePush; }
	auto HasAnyHooks()              const -> bool
	{
		return bHasPreCommit || bHasCommitMsg || bHasPrePush || bHasPostCheckout || bHasPostMerge;
	}
};

class FGitLink_HookProbe
{
public:
	// Scans InRepoRoot/.git/hooks/ and returns which hooks are present. Files ending in
	// ".sample" are excluded (git ships sample hooks that aren't active). Empty/non-existent
	// hooks directory returns all-false flags.
	static auto Probe(const FString& InRepoRoot) -> FGitLink_HookFlags;
};
