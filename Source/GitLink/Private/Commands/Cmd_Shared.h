#pragma once

#include "GitLink/GitLink_Provider.h"
#include "GitLink/GitLink_State.h"
#include "GitLink_CommandDispatcher.h"
#include "GitLink_StateCache.h"
#include "GitLink_Subprocess.h"
#include "GitLinkLog.h"

#include <CoreMinimal.h>
#include <Misc/DateTime.h>
#include <Misc/Paths.h>

// --------------------------------------------------------------------------------------------------------------------
// Shared helpers for Cmd_*.cpp files. Kept in Private/ so nothing outside the GitLink module
// can see them. Intentionally free functions — no class, no namespace gymnastics.
// --------------------------------------------------------------------------------------------------------------------

namespace gitlink::cmd
{
	// Make an FGitLink_FileState stamped with the given composite state and current time.
	inline auto Make_FileState(const FString& InAbsoluteFilename, const FGitLink_CompositeState& InComposite)
		-> FGitLink_FileStateRef
	{
		FGitLink_FileStateRef State = MakeShared<FGitLink_FileState, ESPMode::ThreadSafe>(InAbsoluteFilename);
		State->_State     = InComposite;
		State->_TimeStamp = FDateTime::UtcNow();
		return State;
	}

	// Shortcut: make a state with just the pair (file, tree), leaving lock/remote at their
	// Unknown/UpToDate defaults. Useful for predictive post-op state updates — we know what
	// the state should look like after e.g. MarkForAdd succeeds, and can push the delta into
	// the cache without having to re-run the full status op.
	inline auto Make_FileState(const FString& InAbsoluteFilename, EGitLink_FileState InFile, EGitLink_TreeState InTree)
		-> FGitLink_FileStateRef
	{
		FGitLink_CompositeState Composite;
		Composite.File = InFile;
		Composite.Tree = InTree;
		return Make_FileState(InAbsoluteFilename, Composite);
	}

	// Normalize a user-supplied path to an absolute, forward-slashed form. The editor sometimes
	// passes relative paths (relative to Engine/Binaries/Win64/) so we must ConvertRelativePathToFull
	// before any repo-root prefix stripping or comparison.
	inline auto Normalize_AbsolutePath(const FString& InPath) -> FString
	{
		FString Out = FPaths::ConvertRelativePathToFull(InPath);
		FPaths::NormalizeFilename(Out);
		return Out;
	}

	// Convert an absolute file path to a repo-relative, forward-slash path suitable for passing
	// to git / git-lfs commands (both of which expect forward slashes regardless of platform).
	// Uses case-insensitive prefix stripping because Windows paths can differ in casing between
	// the repo root discovered by libgit2 and the paths the editor hands us.
	inline auto ToRepoRelativePath(const FString& InRepoRootAbsolute, const FString& InAbsolute) -> FString
	{
		FString NormAbs = FPaths::ConvertRelativePathToFull(InAbsolute);
		FPaths::NormalizeFilename(NormAbs);

		FString NormRoot = InRepoRootAbsolute;
		FPaths::NormalizeFilename(NormRoot);
		// Ensure trailing slash so the chop doesn't leave a leading '/' on the relative path.
		if (!NormRoot.EndsWith(TEXT("/")))
		{
			NormRoot += TEXT("/");
		}

		FString Rel = NormAbs;
		if (!NormRoot.IsEmpty() && Rel.StartsWith(NormRoot, ESearchCase::IgnoreCase))
		{
			Rel.RightChopInline(NormRoot.Len(), EAllowShrinking::No);
		}
		Rel.ReplaceInline(TEXT("\\"), TEXT("/"));
		Rel.RemoveFromStart(TEXT("/"));
		return Rel;
	}

	// Releases any LFS locks that the current user holds on the given files. Best-effort:
	// files that aren't locked (or aren't lockable) are silently skipped, so callers can pass
	// a mixed set without pre-filtering. Returns true if the unlock call succeeded (or was
	// a no-op); returns false only when the subprocess itself failed.
	//
	// Used by Cmd_Revert (release after discarding local changes) and Cmd_CheckIn (release
	// after the commit so the file is free for the next editor to check out).
	//
	// NOTE: consults InCtx.StateCache to skip files we know aren't Locked by us. Unknown-lock
	// files are still attempted because the editor cache might be stale.
	inline auto Release_LfsLocksBestEffort(
		FCommandContext&       InCtx,
		const TArray<FString>& InAbsoluteFiles) -> bool
	{
		if (InCtx.Subprocess == nullptr || !InCtx.Subprocess->IsValid())
		{ return true; }  // no LFS subsystem, nothing to do

		if (!InCtx.Provider.Is_LfsAvailable())
		{ return true; }

		TArray<FString> RelativePaths;
		RelativePaths.Reserve(InAbsoluteFiles.Num());

		for (const FString& Absolute : InAbsoluteFiles)
		{
			if (!InCtx.Provider.Is_FileLockable(Absolute))
			{ continue; }

			const FGitLink_FileStateRef State = InCtx.StateCache.Find_FileState(Absolute);
			if (State->_State.Lock == EGitLink_LockState::Unlockable ||
			    State->_State.Lock == EGitLink_LockState::LockedOther)
			{ continue; }  // definitely not ours to release

			RelativePaths.Add(ToRepoRelativePath(InCtx.RepoRootAbsolute, Absolute));
		}

		if (RelativePaths.IsEmpty())
		{ return true; }  // nothing to unlock

		// Query lock IDs for the files we want to unlock. Unlocking by --id is more
		// reliable than by path because it avoids the "admin access" error that occurs
		// when the LFS server's owner identity (e.g. GitHub username) differs from the
		// git config user.name. The --id form lets the server validate ownership via
		// the auth token directly.
		const TMap<FString, FString> LocalLocks = InCtx.Subprocess->QueryLfsLocks_Local();

		bool bAllOk = true;
		int32 UnlockedCount = 0;

		for (const FString& RelPath : RelativePaths)
		{
			// Find the lock ID for this file. QueryLfsLocks_Local returns path->owner,
			// but we need the ID. Fall back to path-based unlock if we can't find it.
			// We need to re-query with --json or parse the ID from the output.
			// For now, try --id from a fresh locks query, falling back to path.

			// Try path-based unlock first (works when identities match).
			TArray<FString> UnlockArgs = { TEXT("unlock"), TEXT("--force"), RelPath };
			FGitLink_SubprocessResult Result = InCtx.Subprocess->RunLfs(UnlockArgs);

			if (!Result.IsSuccess())
			{
				// Path-based unlock failed (likely identity mismatch). Try by --id.
				// Query the lock ID for this specific file.
				FGitLink_SubprocessResult LocksResult = InCtx.Subprocess->RunLfs(
					{ TEXT("locks"), TEXT("--path"), RelPath });

				if (LocksResult.IsSuccess())
				{
					// Parse "path\towner\tID:12345" to extract the ID
					FString LockId;
					const FString& Output = LocksResult.StdOut;
					int32 IdIdx = Output.Find(TEXT("ID:"));
					if (IdIdx != INDEX_NONE)
					{
						LockId = Output.Mid(IdIdx + 3).TrimStartAndEnd();
						// Strip any trailing whitespace or newline
						int32 EndIdx = 0;
						while (EndIdx < LockId.Len() && FChar::IsDigit(LockId[EndIdx]))
						{ ++EndIdx; }
						LockId.LeftInline(EndIdx);
					}

					if (!LockId.IsEmpty())
					{
						FGitLink_SubprocessResult IdResult = InCtx.Subprocess->RunLfs(
							{ TEXT("unlock"), TEXT("--id"), LockId });
						if (IdResult.IsSuccess())
						{
							++UnlockedCount;
							continue;
						}
						UE_LOG(LogGitLink, Warning,
							TEXT("Release_LfsLocksBestEffort: unlock --id=%s failed: %s"),
							*LockId, *IdResult.Get_CombinedError());
					}
				}

				UE_LOG(LogGitLink, Warning,
					TEXT("Release_LfsLocksBestEffort: could not unlock '%s': %s"),
					*RelPath, *Result.Get_CombinedError());
				bAllOk = false;
			}
			else
			{
				++UnlockedCount;
			}
		}

		if (UnlockedCount > 0)
		{
			UE_LOG(LogGitLink, Log,
				TEXT("Release_LfsLocksBestEffort: unlocked %d file(s)"), UnlockedCount);
		}
		return bAllOk;
	}
}
