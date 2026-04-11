#pragma once

#include <CoreMinimal.h>

// --------------------------------------------------------------------------------------------------------------------
// FGitLink_Subprocess — narrow wrapper around git.exe for operations libgit2 doesn't cover.
// In v1 this is primarily git-lfs locking; future use cases are pre-commit hook invocation
// and `git check-attr` queries.
//
// Built on FPlatformProcess::ExecProcess which blocks, captures stdout/stderr, and returns
// the exit code. Individual args are quoted for paths with spaces.
// --------------------------------------------------------------------------------------------------------------------

struct FGitLink_SubprocessResult
{
	bool    bSpawned  = false;   // false if git.exe couldn't be launched at all
	int32   ExitCode  = -1;
	FString StdOut;
	FString StdErr;

	auto IsSuccess() const -> bool { return bSpawned && ExitCode == 0; }

	// Combined human-readable error string for UI toasts. Prefers stderr, falls back to stdout,
	// then to a synthetic "exited with code N" message.
	auto Get_CombinedError() const -> FString;
};

// --------------------------------------------------------------------------------------------------------------------

class FGitLink_Subprocess
{
public:
	FGitLink_Subprocess() = default;
	FGitLink_Subprocess(FString InGitBinary, FString InWorkingDirectory);

	FGitLink_Subprocess(const FGitLink_Subprocess&)            = delete;
	FGitLink_Subprocess& operator=(const FGitLink_Subprocess&) = delete;

	auto IsValid() const -> bool { return !_GitBinary.IsEmpty(); }

	auto Get_GitBinary()       const -> const FString& { return _GitBinary; }
	auto Get_WorkingDirectory() const -> const FString& { return _WorkingDirectory; }

	// Runs `git <args>` in the configured working directory. Each element of InArgs is
	// quoted individually so paths with spaces survive.
	auto Run(const TArray<FString>& InArgs) -> FGitLink_SubprocessResult;

	// Convenience for `git lfs <subcommand> <args>`.
	auto RunLfs(const TArray<FString>& InArgs) -> FGitLink_SubprocessResult;

	// Probe: is git-lfs installed and usable?
	auto IsLfsAvailable() -> bool;

	// Probe: runs `git check-attr lockable` on the provided wildcard list (e.g. "*.uasset").
	// Returns the subset of wildcards that the repo's .gitattributes marks as lockable.
	// Extensions are returned WITHOUT the leading "*" — ".uasset", ".umap", etc. — so callers
	// can do direct FString::EndsWith comparisons.
	auto ProbeLockableExtensions(const TArray<FString>& InWildcards) -> TArray<FString>;

	// Queries LFS locks held by the current user via `git lfs locks --local`.
	// Returns a map of repo-relative paths (forward-slashed) to the lock owner name.
	// On failure (LFS not available, network error), returns an empty map.
	auto QueryLfsLocks_Local() -> TMap<FString, FString>;

private:
	FString _GitBinary;
	FString _WorkingDirectory;
};
