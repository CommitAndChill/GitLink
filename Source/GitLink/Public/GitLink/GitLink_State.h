#pragma once

#include "GitLink/GitLink_Changelist.h"
#include "GitLink/GitLink_Revision.h"

#include <CoreMinimal.h>
#include <ISourceControlState.h>
#include <Runtime/Launch/Resources/Version.h>

// --------------------------------------------------------------------------------------------------------------------
// Per-file state tracked by the GitLink provider.
//
// The priority enum (EGitLink_Status) mirrors the one in the existing GitSourceControl plugin —
// these categories are fundamentally bound to how git semantics map onto Unreal's
// IsCheckedOut / IsModified / IsAdded / IsDeleted / ... question shape, so the categories are
// nearly identical. Naming is brand-neutral.
// --------------------------------------------------------------------------------------------------------------------

// Coarse status priority — used by the icon + tooltip picker to decide what the editor shows.
enum class EGitLink_Status : uint8
{
	Unset,
	NotAtHead,
	LockedOther,
	NotLatest,
	Unmerged,
	Added,
	Deleted,
	Modified,
	CheckedOut,    // not modified on disk, but locked by us
	Untracked,
	Lockable,
	Unmodified,
	Ignored,
	None,
};

// The actual per-file git diff state (what the diff says about this file).
enum class EGitLink_FileState : uint8
{
	Unset,
	Unknown,
	Added,
	Copied,
	Deleted,
	Modified,
	Renamed,
	Missing,
	Unmerged,
};

// Where the file lives in the git workflow.
enum class EGitLink_TreeState : uint8
{
	Unset,
	Unmodified,   // synced to commit
	Working,      // modified in working tree, not staged
	Staged,       // staged in index
	Untracked,    // not tracked
	Ignored,      // gitignored
	NotInRepo,    // outside the repo
};

// LFS lock state.
enum class EGitLink_LockState : uint8
{
	Unset,
	Unknown,
	Unlockable,
	NotLocked,
	Locked,       // locked by us
	LockedOther,  // locked by someone else
};

// Relationship to the remote.
enum class EGitLink_RemoteState : uint8
{
	Unset,
	UpToDate,
	NotAtHead,
	NotLatest,
};

// Combined view — the only thing stored in the state cache per file.
struct FGitLink_CompositeState
{
	EGitLink_FileState   File   = EGitLink_FileState::Unknown;
	EGitLink_TreeState   Tree   = EGitLink_TreeState::NotInRepo;
	EGitLink_LockState   Lock   = EGitLink_LockState::Unknown;
	EGitLink_RemoteState Remote = EGitLink_RemoteState::UpToDate;

	FString LockUser;    // owner of an LFS lock, if any
	FString HeadBranch;  // branch that holds the latest modification, if ahead
};

// --------------------------------------------------------------------------------------------------------------------
class GITLINK_API FGitLink_FileState : public ISourceControlState
{
public:
	explicit FGitLink_FileState(const FString& InLocalFilename)
		: _LocalFilename(InLocalFilename)
	{
	}

	// ISourceControlState --------------------------------------------------------------------------------------------
	auto GetHistorySize() const -> int32 override;
	auto GetHistoryItem(int32 InHistoryIndex) const -> TSharedPtr<ISourceControlRevision, ESPMode::ThreadSafe> override;
	auto FindHistoryRevision(int32 InRevisionNumber) const -> TSharedPtr<ISourceControlRevision, ESPMode::ThreadSafe> override;
	auto FindHistoryRevision(const FString& InRevision) const -> TSharedPtr<ISourceControlRevision, ESPMode::ThreadSafe> override;

#if ENGINE_MAJOR_VERSION < 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION < 3)
	auto GetBaseRevForMerge() const -> TSharedPtr<ISourceControlRevision, ESPMode::ThreadSafe> override;
#else
	auto GetResolveInfo() const -> FResolveInfo override;
#endif

#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 2
	auto GetCurrentRevision() const -> TSharedPtr<ISourceControlRevision, ESPMode::ThreadSafe> override;
#endif

#if ENGINE_MAJOR_VERSION >= 5
	auto GetIcon() const -> FSlateIcon override;
#else
	auto GetIconName()      const -> FName override;
	auto GetSmallIconName() const -> FName override;
#endif

	auto GetDisplayName   () const -> FText              override;
	auto GetDisplayTooltip() const -> FText              override;
	auto GetFilename      () const -> const FString&    override { return _LocalFilename; }
	auto GetTimeStamp     () const -> const FDateTime&  override { return _TimeStamp; }

	auto CanCheckIn()      const -> bool override;
	auto CanCheckout()     const -> bool override;
	auto IsCheckedOut()    const -> bool override;
	auto IsCheckedOutOther(FString* OutWho = nullptr) const -> bool override;
	auto IsCheckedOutInOtherBranch(const FString& InCurrentBranch = FString()) const -> bool override;
	auto IsModifiedInOtherBranch  (const FString& InCurrentBranch = FString()) const -> bool override;
	auto IsCheckedOutOrModifiedInOtherBranch(const FString& InCurrentBranch = FString()) const -> bool override
	{
		return IsModifiedInOtherBranch(InCurrentBranch);
	}
	auto GetCheckedOutBranches  () const -> TArray<FString> override { return {}; }
	auto GetOtherUserBranchCheckedOuts() const -> FString override { return {}; }
	auto GetOtherBranchHeadModification(FString& OutHeadBranch, FString& OutAction, int32& OutHeadChangeList) const -> bool override;

	auto IsCurrent        () const -> bool override;
	auto IsSourceControlled() const -> bool override;
	auto IsAdded          () const -> bool override;
	auto IsDeleted        () const -> bool override;
	auto IsIgnored        () const -> bool override;
	auto CanEdit          () const -> bool override;
	auto IsUnknown        () const -> bool override;
	auto IsModified       () const -> bool override;
	auto CanAdd           () const -> bool override;
	auto CanDelete        () const -> bool override;
	auto IsConflicted     () const -> bool override;
	auto CanRevert        () const -> bool override;

	// Internal helpers
	auto Get_OverallStatus() const -> EGitLink_Status;

	// Public fields — populated by Cmd_UpdateStatus from gitlink::FFileChange.
	FGitLink_History        _History;
	FString                 _LocalFilename;
	FGitLink_CompositeState _State;
	FGitLink_Changelist     _Changelist;
	FDateTime               _TimeStamp = FDateTime(0);

#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 3
	FResolveInfo            _PendingResolveInfo;
#endif
	FString                 _PendingMergeBaseFileHash;

	// Head-branch metadata for the IsCheckedOutInOtherBranch / IsModifiedInOtherBranch API surface.
	// v1 leaves these unset so those methods return false; populated once multi-branch tracking lands.
	FString                 _HeadAction  = TEXT("Changed");
	FString                 _HeadCommit  = TEXT("Unknown");
	int64                   _HeadModTime = 0;
};

using FGitLink_FileStateRef = TSharedRef<FGitLink_FileState, ESPMode::ThreadSafe>;
