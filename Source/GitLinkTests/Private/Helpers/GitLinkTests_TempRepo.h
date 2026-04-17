#pragma once

#include <CoreMinimal.h>
#include <Templates/UniquePtr.h>

// --------------------------------------------------------------------------------------------------------------------

namespace gitlink
{
	class FRepository;
}

namespace gitlink::tests
{
	// ----------------------------------------------------------------------------------------------------------------
	// RAII helper: creates an ephemeral git repository under Saved/GitLinkTests/<guid>/ via libgit2, seeds a minimal
	// user.name / user.email so FCommitParams with empty Author fields work, and recursively deletes the directory
	// on destruction. Copy/move are disabled to keep cleanup ownership unambiguous.
	// ----------------------------------------------------------------------------------------------------------------
	class FTempRepo
	{
	public:
		FTempRepo();
		~FTempRepo();

		FTempRepo(const FTempRepo&)            = delete;
		FTempRepo& operator=(const FTempRepo&) = delete;
		FTempRepo(FTempRepo&&)                 = delete;
		FTempRepo& operator=(FTempRepo&&)      = delete;

		// Root working directory of the ephemeral repo (absolute path, no trailing slash).
		auto Get_Root() const -> const FString& { return _Root; }

		// True only if git_repository_init succeeded and the user config was seeded.
		auto IsValid() const -> bool { return _bValid; }

		// Open the repo via FRepository::Open(FOpenParams{_Root}). Returns nullptr if the repo is invalid
		// or open fails — caller should test the pointer.
		auto Open() -> TUniquePtr<gitlink::FRepository>;

		// Convenience file helpers — InRelPath is relative to the repo root.
		auto Write_File (const FString& InRelPath, const FString& InContents) -> bool;
		auto Append_File(const FString& InRelPath, const FString& InContents) -> bool;
		auto Delete_File(const FString& InRelPath) -> bool;

	private:
		FString _Root;
		bool    _bValid = false;
	};
}
