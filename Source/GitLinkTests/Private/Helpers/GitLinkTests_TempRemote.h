#pragma once

#include "GitLink_Subprocess.h" // from Plugins/GitLink/Source/GitLink/Private (exposed via PrivateIncludePaths)

#include <CoreMinimal.h>
#include <Templates/UniquePtr.h>

class FAutomationTestBase;

namespace gitlink
{
	class FRepository;
}

namespace gitlink::tests
{
	// ----------------------------------------------------------------------------------------------------------------
	// RAII helper for remote-operation tests: a local bare repository standing in for "the server", seeded with one
	// commit on `main`, plus any number of clones of it ("users"). Everything lives under
	// Saved/GitLinkTests/<guid>/ and is deleted on destruction. No network — the remote is a plain filesystem path,
	// which both libgit2 (local transport) and the git CLI accept.
	//
	// Setup and assertions go through the git command line (bounded, so a broken git cannot hang a test); the code
	// under test is whatever the test calls on top (FRepository::Fetch / PullFastForward, cmd::Push, ...).
	// Every commit passes -c commit.gpgsign=false so a developer's global signing config cannot fail the suite.
	// ----------------------------------------------------------------------------------------------------------------
	class FTempRemote
	{
	public:
		FTempRemote();
		~FTempRemote();

		FTempRemote(const FTempRemote&)            = delete;
		FTempRemote& operator=(const FTempRemote&) = delete;

		// True when git was found and the bare remote + seed commit were created.
		auto IsValid() const -> bool { return _bValid; }

		// Adds a warning and returns false when the git CLI is unavailable, so a test can skip cleanly.
		static auto Is_GitAvailable(FAutomationTestBase& InTest) -> bool;

		// Absolute path of the bare repository (usable as a remote URL).
		auto Get_BareRoot() const -> const FString& { return _BareRoot; }

		// Clones the remote into <base>/<InName> with a test identity configured. Returns the clone's absolute root,
		// or empty on failure. InUrl overrides the source (a real remote, for the opt-in live tests).
		auto Clone(const FString& InName, const FString& InUrl = FString()) -> FString;

		// Opens a clone (or any repo root) through GitLinkCore.
		static auto Open(const FString& InRoot) -> TUniquePtr<gitlink::FRepository>;

		// Runs git in InCwd (bounded to 60 s).
		auto Git(const FString& InCwd, const TArray<FString>& InArgs) -> FGitLink_SubprocessResult;

		// Writes InRelPath (relative to InRepoRoot), stages it and commits with the git CLI. Returns true on success.
		auto Commit_File(const FString& InRepoRoot, const FString& InRelPath,
			const FString& InContents, const FString& InMessage) -> bool;

		// Full SHA of InRev in InRepoRoot ("HEAD", "origin/main", ...), or empty if it does not resolve.
		auto Get_Sha(const FString& InRepoRoot, const FString& InRev) -> FString;

		// Reads a working-tree file (InRelPath relative to InRepoRoot) with CRLF normalised to LF; empty if missing.
		static auto Read_File(const FString& InRepoRoot, const FString& InRelPath) -> FString;

		// The subprocess used for setup, rooted at the fixture base — handy for passing to code under test with a
		// cwd override.
		auto Get_Subprocess() -> FGitLink_Subprocess& { return *_Git; }

	private:
		FString                          _Base;
		FString                          _BareRoot;
		TUniquePtr<FGitLink_Subprocess>  _Git;
		bool                             _bValid = false;
	};
}
