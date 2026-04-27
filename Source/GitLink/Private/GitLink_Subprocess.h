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

class GITLINK_API FGitLink_Subprocess
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
	//
	// InCwdOverride: if non-empty, runs git with that working directory instead of the
	// configured one. Used to route operations into a submodule's repo so each submodule's
	// own LFS endpoint / git config / remote is picked up automatically.
	auto Run(const TArray<FString>& InArgs, const FString& InCwdOverride = FString())
		-> FGitLink_SubprocessResult;

	// Convenience for `git lfs <subcommand> <args>`. See Run() for InCwdOverride semantics.
	auto RunLfs(const TArray<FString>& InArgs, const FString& InCwdOverride = FString())
		-> FGitLink_SubprocessResult;

	// Probe: is git-lfs installed and usable?
	auto IsLfsAvailable() -> bool;

	// Probe: runs `git check-attr lockable` on the provided wildcard list (e.g. "*.uasset").
	// Returns the subset of wildcards that the repo's .gitattributes marks as lockable.
	// Extensions are returned WITHOUT the leading "*" — ".uasset", ".umap", etc. — so callers
	// can do direct FString::EndsWith comparisons.
	auto ProbeLockableExtensions(const TArray<FString>& InWildcards) -> TArray<FString>;

	// Queries LFS locks via `git lfs locks --verify --json`. Network call that contacts
	// the LFS server and returns separate "ours" and "theirs" buckets — the server-side
	// truth of which locks the current user holds, regardless of git config user.name.
	//
	// AllLocks: every lock the server knows about, keyed by repo-relative forward-slashed
	// path → owner display name.
	// OursPaths: subset of AllLocks paths that the LFS server confirms are owned by us.
	//
	// On failure (LFS not available, network error), returns an empty snapshot.
	// InCwdOverride: see Run().
	struct FLfsLocksSnapshot
	{
		// True when git-lfs returned a parseable response (even if no locks exist).
		// False on subprocess failure / non-zero exit / unparseable JSON. Distinguishes
		// "no locks" (trustworthy) from "couldn't query" (don't trust empty AllLocks).
		bool                   bSuccess = false;
		TMap<FString, FString> AllLocks;
		TSet<FString>          OursPaths;
	};
	auto QueryLfsLocks_Verified(const FString& InCwdOverride = FString()) -> FLfsLocksSnapshot;

	// Runs `git <args>` and writes stdout (binary-safe) to InOutputFile.
	// Used by FGitLink_Revision::Get to extract file content at a specific commit.
	// Returns true on success (exit code 0).
	auto RunToFile(const TArray<FString>& InArgs, const FString& InOutputFile,
		const FString& InWorkingDirOverride = FString()) -> bool;

private:
	FString _GitBinary;
	FString _WorkingDirectory;
};
