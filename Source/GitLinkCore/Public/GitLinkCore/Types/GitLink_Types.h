#pragma once

#include <CoreMinimal.h>

// --------------------------------------------------------------------------------------------------------------------
// Plain value types used across the GitLinkCore facade. None of these depend on libgit2 headers —
// consumers of GitLinkCore can include this file without pulling in git2.h.
// --------------------------------------------------------------------------------------------------------------------

namespace gitlink
{
	// --------------------------------------------------------------------------------------------------------------------
	// Result of a git operation. All gitlink APIs return one of these instead of throwing.
	// --------------------------------------------------------------------------------------------------------------------
	struct GITLINKCORE_API FResult
	{
		bool    bOk = false;
		FString ErrorMessage;

		static auto Ok() -> FResult
		{
			FResult R;
			R.bOk = true;
			return R;
		}

		static auto Fail(FString InMessage) -> FResult
		{
			FResult R;
			R.bOk = false;
			R.ErrorMessage = MoveTemp(InMessage);
			return R;
		}

		explicit operator bool() const { return bOk; }
	};

	// --------------------------------------------------------------------------------------------------------------------
	// Per-file status. Maps to libgit2's git_status_t but with a cleaner enum surface.
	// --------------------------------------------------------------------------------------------------------------------
	enum class EFileStatus : uint8
	{
		Unmodified,
		Added,
		Modified,
		Deleted,
		Renamed,
		TypeChange,
		Untracked,
		Ignored,
		Conflicted,
	};

	// --------------------------------------------------------------------------------------------------------------------
	// LFS pointer state for a file, if relevant.
	// --------------------------------------------------------------------------------------------------------------------
	enum class ELfsStatus : uint8
	{
		NotLfs,
		Pointer,        // file on disk is still an LFS pointer (content not fetched)
		Resolved,       // file on disk is the real content
		BrokenPointer,  // corrupt / malformed pointer
	};

	// --------------------------------------------------------------------------------------------------------------------
	// A single changed file in the working tree or index.
	// --------------------------------------------------------------------------------------------------------------------
	struct GITLINKCORE_API FFileChange
	{
		FString     Path;
		FString     OldPath;        // populated on renames
		EFileStatus Status      = EFileStatus::Unmodified;
		ELfsStatus  LfsStatus   = ELfsStatus::NotLfs;
		bool        bIsSubmodule = false;
		int32       Additions   = 0;
		int32       Deletions   = 0;
	};

	// --------------------------------------------------------------------------------------------------------------------
	// Full working-tree status, split three ways to match how Unreal source control thinks.
	// --------------------------------------------------------------------------------------------------------------------
	struct GITLINKCORE_API FStatus
	{
		TArray<FFileChange> Staged;
		TArray<FFileChange> Unstaged;
		TArray<FFileChange> Conflicted;

		auto IsEmpty() const -> bool
		{
			return Staged.IsEmpty() && Unstaged.IsEmpty() && Conflicted.IsEmpty();
		}

		auto Num() const -> int32
		{
			return Staged.Num() + Unstaged.Num() + Conflicted.Num();
		}
	};

	// --------------------------------------------------------------------------------------------------------------------
	// Git signature (author / committer).
	// --------------------------------------------------------------------------------------------------------------------
	struct GITLINKCORE_API FSignature
	{
		FString    Name;
		FString    Email;
		FDateTime  When = FDateTime::MinValue();
	};

	// --------------------------------------------------------------------------------------------------------------------
	// A commit in the history, flattened to plain data.
	// --------------------------------------------------------------------------------------------------------------------
	struct GITLINKCORE_API FCommit
	{
		FString          Hash;          // full hex SHA-1
		FString          ShortHash;     // abbreviated
		TArray<FString>  ParentHashes;
		FSignature       Author;
		FSignature       Committer;
		FString          Summary;       // first line
		FString          Message;       // full
	};

	// --------------------------------------------------------------------------------------------------------------------
	// A branch (local or remote-tracking).
	// --------------------------------------------------------------------------------------------------------------------
	struct GITLINKCORE_API FBranch
	{
		FString Name;            // short name, e.g. "main"
		FString FullRef;         // e.g. "refs/heads/main"
		FString UpstreamName;    // e.g. "origin/main" (may be empty)
		FString TipHash;         // commit this branch points at
		bool    bIsRemote = false;
		bool    bIsHead   = false;
	};

	// --------------------------------------------------------------------------------------------------------------------
	// A configured remote (name + fetch/push URL).
	// --------------------------------------------------------------------------------------------------------------------
	struct GITLINKCORE_API FRemote
	{
		FString Name;
		FString FetchUrl;
		FString PushUrl;
	};

	// --------------------------------------------------------------------------------------------------------------------
	// Log query options.
	// --------------------------------------------------------------------------------------------------------------------
	struct GITLINKCORE_API FLogQuery
	{
		FString StartRef;     // empty = HEAD
		int32   MaxCount = 0; // 0 = unlimited
		FString PathFilter;   // empty = all
	};
}
