#pragma once

#include "GitLink/GitLink_Changelist.h"
#include "GitLink/GitLink_Revision.h"

#include <CoreMinimal.h>
#include <ISourceControlState.h>
#include <Runtime/Launch/Resources/Version.h>

// --------------------------------------------------------------------------------------------------------------------
// Per-file state tracked by the GitLink provider.
//
// The priority enum (EGitLink_Status) is bound to how git semantics map onto Unreal's
// IsCheckedOut / IsModified / IsAdded / IsDeleted / ... question shape.
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

/// Combined per-file state — the only thing stored in the state cache per file.
/// Each dimension is independent: File tracks the diff type, Tree tracks the staging location,
/// Lock tracks LFS lock ownership, and Remote tracks whether the file is at the latest revision.
struct FGitLink_CompositeState
{
	EGitLink_FileState   File   = EGitLink_FileState::Unknown;   ///< What kind of change (added/modified/deleted/etc).
	EGitLink_TreeState   Tree   = EGitLink_TreeState::NotInRepo; ///< Where in the git workflow (working/staged/untracked).
	EGitLink_LockState   Lock   = EGitLink_LockState::Unknown;   ///< LFS lock state (locked by us, by other, not locked).
	EGitLink_RemoteState Remote = EGitLink_RemoteState::UpToDate; ///< Relationship to the remote tracking branch.

	FString LockUser;    ///< Owner of an LFS lock, if any (display name from the LFS server).
	FString HeadBranch;  ///< Branch that holds a newer version, if Remote != UpToDate.
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

	/// Collapse the composite state into a single priority enum for icon + tooltip display.
	auto Get_OverallStatus() const -> EGitLink_Status;

	// Public fields — populated by Cmd_UpdateStatus / Cmd_Changelists from gitlink::FFileChange.
	FGitLink_History        _History;         ///< Per-file commit history (newest first).
	FString                 _LocalFilename;   ///< Absolute path to the file on disk.
	FGitLink_CompositeState _State;           ///< Combined file/tree/lock/remote status.
	FGitLink_Changelist     _Changelist;      ///< Which changelist this file belongs to (Working or Staged).
	FDateTime               _TimeStamp = FDateTime(0); ///< When this state was last refreshed.

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
