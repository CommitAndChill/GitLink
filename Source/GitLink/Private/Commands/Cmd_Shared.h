#pragma once

#include "GitLink/GitLink_State.h"

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

	// Normalize a user-supplied path (usually absolute, sometimes with backslashes on Windows)
	// for comparison with libgit2's relative paths. Matches what Cmd_UpdateStatus does.
	inline auto Normalize_AbsolutePath(const FString& InPath) -> FString
	{
		FString Out = InPath;
		FPaths::NormalizeFilename(Out);
		return Out;
	}
}
