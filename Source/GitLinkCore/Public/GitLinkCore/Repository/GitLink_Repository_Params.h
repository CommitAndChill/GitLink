#pragma once

#include <CoreMinimal.h>

// --------------------------------------------------------------------------------------------------------------------
// Parameter structs for repository-level operations.
// --------------------------------------------------------------------------------------------------------------------

namespace gitlink
{
	// --------------------------------------------------------------------------------------------------------------------
	// Options for opening an existing repository.
	// --------------------------------------------------------------------------------------------------------------------
	struct GITLINKCORE_API FOpenParams
	{
		// Absolute path to the working tree root (or a subdirectory — libgit2 will discover the .git upward).
		FString Path;

		// When true, refuse to open repositories found by upward discovery; only accept an exact match.
		bool bNoDiscover = false;
	};

	// --------------------------------------------------------------------------------------------------------------------
	// Options for creating a commit.
	// --------------------------------------------------------------------------------------------------------------------
	struct GITLINKCORE_API FCommitParams
	{
		FString Message;

		// If empty, falls back to libgit2's default_signature (user.name / user.email from config).
		FString AuthorName;
		FString AuthorEmail;

		// When true, amend HEAD instead of creating a new commit.
		bool bAmend = false;

		// When true, allow creating a commit with no changes against HEAD.
		bool bAllowEmpty = false;
	};

	// --------------------------------------------------------------------------------------------------------------------
	// Options for a fetch.
	// --------------------------------------------------------------------------------------------------------------------
	struct GITLINKCORE_API FFetchParams
	{
		// Empty = the remote the current branch tracks (branch.<name>.remote), falling back to
		// "origin" when HEAD is detached or has no upstream. PullFastForward fast-forwards to the
		// branch's upstream, so fetching any other remote would compare against a stale ref.
		FString RemoteName;
		bool    bPrune     = false;
	};

	// --------------------------------------------------------------------------------------------------------------------
	// Options for a push.
	// --------------------------------------------------------------------------------------------------------------------
	struct GITLINKCORE_API FPushParams
	{
		FString RemoteName  = TEXT("origin");
		FString BranchName;               // e.g. "main"; empty = current HEAD branch
		bool    bForce      = false;
	};
}
