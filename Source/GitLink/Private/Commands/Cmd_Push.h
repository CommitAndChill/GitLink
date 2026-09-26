#pragma once

#include <CoreMinimal.h>

class FGitLink_Subprocess;

// --------------------------------------------------------------------------------------------------------------------
// cmd::Push — `git push` for one repository.
//
// Push is a toolbar action (FGitLink_Menu), not an ISourceControlProvider operation, so it is not registered with
// the dispatcher. It runs the git command line rather than libgit2 so the user's credential helper and pre-push
// hooks apply — including git-lfs's pre-push hook, which uploads the LFS objects the pushed commits reference.
// Only the given repository is pushed; submodules with their own unpushed commits are not.
// --------------------------------------------------------------------------------------------------------------------

namespace gitlink::cmd
{
	struct FPushOutcome
	{
		bool    bOk = false;
		FString ErrorMessage;   // git's own stderr on failure (trimmed), suitable for a notification
	};

	// InRepoRoot empty = the subprocess's working directory (the project repository). Bounded by InTimeoutSec so a
	// wedged credential helper or dead remote cannot pin the caller; the default accommodates large LFS uploads.
	GITLINK_API auto Push(
		FGitLink_Subprocess& InSubprocess,
		const FString&       InRepoRoot   = FString(),
		double               InTimeoutSec = 600.0) -> FPushOutcome;
}
