#pragma once

#include <CoreMinimal.h>
#include <ISourceControlChangelist.h>
#include <Runtime/Launch/Resources/Version.h>

// --------------------------------------------------------------------------------------------------------------------
// GitLink "changelists" are virtual groupings on top of the git staging index, persisted in
// .git/gitlink/changelists.json as { "<name>": ["path1", "path2", ...] }. There's no real git
// concept to map onto, so each provider instance owns the mapping.
//
// Two well-known changelists always exist:
//   - WorkingChangelist: everything in the working tree that isn't staged
//   - StagedChangelist : everything in the git index
//
// Named changelists are optional and land in Cmd_Changelists later.
// --------------------------------------------------------------------------------------------------------------------

class GITLINK_API FGitLink_Changelist : public ISourceControlChangelist
{
public:
	FGitLink_Changelist() = default;

	explicit FGitLink_Changelist(FString InName, bool bInInitialized = false)
		: _Name(MoveTemp(InName))
		, _bInitialized(bInInitialized)
	{
	}

	// ISourceControlChangelist ---------------------------------------------------------------------------------------
	auto CanDelete() const -> bool override { return false; }

#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 3
	auto IsDefault()    const -> bool    override { return _Name == WorkingChangelist._Name; }
	auto GetIdentifier() const -> FString override { return _Name; }
#endif

	// Helpers --------------------------------------------------------------------------------------------------------
	auto Get_Name()      const -> FString { return _Name; }
	auto IsInitialized() const -> bool    { return _bInitialized; }
	auto Set_Initialized()     -> void    { _bInitialized = true; }

	auto Reset() -> void
	{
		_Name.Reset();
		_bInitialized = false;
	}

	friend auto operator==(const FGitLink_Changelist& InLhs, const FGitLink_Changelist& InRhs) -> bool
	{
		return InLhs._Name == InRhs._Name;
	}
	friend auto operator!=(const FGitLink_Changelist& InLhs, const FGitLink_Changelist& InRhs) -> bool
	{
		return !(InLhs == InRhs);
	}
	friend auto GetTypeHash(const FGitLink_Changelist& InChangelist) -> uint32
	{
		return GetTypeHash(InChangelist._Name);
	}

	/** Default changelist for unstaged working-tree changes. Maps to git's working tree. */
	static FGitLink_Changelist WorkingChangelist;

	/** Default changelist for staged (indexed) changes. Maps to git's staging area. */
	static FGitLink_Changelist StagedChangelist;

private:
	FString _Name;
	bool    _bInitialized = false;
};

using FGitLink_ChangelistRef = TSharedRef<FGitLink_Changelist, ESPMode::ThreadSafe>;
