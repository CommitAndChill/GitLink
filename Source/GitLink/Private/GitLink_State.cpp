#include "GitLink/GitLink_State.h"

#include "GitLinkLog.h"

#include <RevisionControlStyle/RevisionControlStyle.h>
#include <Styling/SlateIconFinder.h>
#include <Textures/SlateIcon.h>

#define LOCTEXT_NAMESPACE "GitLinkState"

// --------------------------------------------------------------------------------------------------------------------
// FGitLink_FileState — translates the composite (FileState x TreeState x LockState x RemoteState)
// into the boolean predicates that Unreal's source control UI asks.
//
// The priority order of the tests matters because a file can be in multiple states at once
// (e.g. staged + modified since staging), and we have to pick ONE status to report.
// --------------------------------------------------------------------------------------------------------------------

auto FGitLink_FileState::Get_OverallStatus() const -> EGitLink_Status
{
	// Order matters — first match wins, and it determines the icon + tooltip.

	// Lock-other is highest priority because it blocks the user from editing.
	if (_State.Lock == EGitLink_LockState::LockedOther)
	{ return EGitLink_Status::LockedOther; }

	if (_State.Remote == EGitLink_RemoteState::NotAtHead)
	{ return EGitLink_Status::NotAtHead; }

	if (_State.Remote == EGitLink_RemoteState::NotLatest)
	{ return EGitLink_Status::NotLatest; }

	if (_State.File == EGitLink_FileState::Unmerged)
	{ return EGitLink_Status::Unmerged; }

	if (_State.File == EGitLink_FileState::Added)
	{ return EGitLink_Status::Added; }

	if (_State.File == EGitLink_FileState::Deleted)
	{ return EGitLink_Status::Deleted; }

	if (_State.File == EGitLink_FileState::Modified || _State.File == EGitLink_FileState::Renamed)
	{ return EGitLink_Status::Modified; }

	if (_State.Lock == EGitLink_LockState::Locked)
	{ return EGitLink_Status::CheckedOut; }

	if (_State.Tree == EGitLink_TreeState::Untracked)
	{ return EGitLink_Status::Untracked; }

	if (_State.Lock == EGitLink_LockState::NotLocked)
	{ return EGitLink_Status::Lockable; }

	if (_State.Tree == EGitLink_TreeState::Ignored)
	{ return EGitLink_Status::Ignored; }

	if (_State.Tree == EGitLink_TreeState::Unmodified)
	{ return EGitLink_Status::Unmodified; }

	if (_State.Tree == EGitLink_TreeState::NotInRepo)
	{ return EGitLink_Status::None; }

	return EGitLink_Status::Unset;
}

// --------------------------------------------------------------------------------------------------------------------
// History
// --------------------------------------------------------------------------------------------------------------------
auto FGitLink_FileState::GetHistorySize() const -> int32
{
	return _History.Num();
}

auto FGitLink_FileState::GetHistoryItem(int32 InHistoryIndex) const
	-> TSharedPtr<ISourceControlRevision, ESPMode::ThreadSafe>
{
	if (_History.IsValidIndex(InHistoryIndex))
	{ return _History[InHistoryIndex]; }

	return nullptr;
}

auto FGitLink_FileState::FindHistoryRevision(int32 InRevisionNumber) const
	-> TSharedPtr<ISourceControlRevision, ESPMode::ThreadSafe>
{
	for (const FGitLink_RevisionRef& Revision : _History)
	{
		if (Revision->GetRevisionNumber() == InRevisionNumber)
		{ return Revision; }
	}
	return nullptr;
}

auto FGitLink_FileState::FindHistoryRevision(const FString& InRevision) const
	-> TSharedPtr<ISourceControlRevision, ESPMode::ThreadSafe>
{
	for (const FGitLink_RevisionRef& Revision : _History)
	{
		if (Revision->GetRevision() == InRevision)
		{ return Revision; }
	}
	return nullptr;
}

// --------------------------------------------------------------------------------------------------------------------
// Merge resolve
// --------------------------------------------------------------------------------------------------------------------
#if ENGINE_MAJOR_VERSION < 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION < 3)
auto FGitLink_FileState::GetBaseRevForMerge() const
	-> TSharedPtr<ISourceControlRevision, ESPMode::ThreadSafe>
{
	for (const FGitLink_RevisionRef& Revision : _History)
	{
		if (Revision->_CommitId == _PendingMergeBaseFileHash)
		{ return Revision; }
	}
	return nullptr;
}
#else
auto FGitLink_FileState::GetResolveInfo() const -> FResolveInfo
{
	return _PendingResolveInfo;
}
#endif

#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 2
auto FGitLink_FileState::GetCurrentRevision() const
	-> TSharedPtr<ISourceControlRevision, ESPMode::ThreadSafe>
{
	// v1: not populated. Cmd_UpdateStatus will fill _History[0] with the current revision later.
	return nullptr;
}
#endif

// --------------------------------------------------------------------------------------------------------------------
// Icon / display
// --------------------------------------------------------------------------------------------------------------------
#if ENGINE_MAJOR_VERSION >= 5
auto FGitLink_FileState::GetIcon() const -> FSlateIcon
{
	switch (Get_OverallStatus())
	{
		case EGitLink_Status::Modified:
		case EGitLink_Status::CheckedOut:
			return FSlateIcon(FRevisionControlStyleManager::GetStyleSetName(), "RevisionControl.CheckedOut");
		case EGitLink_Status::LockedOther:
			return FSlateIcon(FRevisionControlStyleManager::GetStyleSetName(), "RevisionControl.CheckedOutByOtherUser");
		case EGitLink_Status::Added:
			return FSlateIcon(FRevisionControlStyleManager::GetStyleSetName(), "RevisionControl.OpenForAdd");
		case EGitLink_Status::Deleted:
			return FSlateIcon(FRevisionControlStyleManager::GetStyleSetName(), "RevisionControl.MarkedForDelete");
		case EGitLink_Status::Untracked:
			return FSlateIcon(FRevisionControlStyleManager::GetStyleSetName(), "RevisionControl.NotInDepot");
		case EGitLink_Status::NotAtHead:
		case EGitLink_Status::NotLatest:
			return FSlateIcon(FRevisionControlStyleManager::GetStyleSetName(), "RevisionControl.NotAtHeadRevision");
		case EGitLink_Status::Unmerged:
			return FSlateIcon(FRevisionControlStyleManager::GetStyleSetName(), "RevisionControl.Unmerged");
		default:
			return FSlateIcon();
	}
}
#else
auto FGitLink_FileState::GetIconName() const -> FName
{
	return NAME_None;
}
auto FGitLink_FileState::GetSmallIconName() const -> FName
{
	return NAME_None;
}
#endif

auto FGitLink_FileState::GetDisplayName() const -> FText
{
	switch (Get_OverallStatus())
	{
		case EGitLink_Status::Unmodified:  return LOCTEXT("Unmodified",  "Unmodified");
		case EGitLink_Status::Modified:    return LOCTEXT("Modified",    "Modified");
		case EGitLink_Status::Added:       return LOCTEXT("Added",       "Added");
		case EGitLink_Status::Deleted:     return LOCTEXT("Deleted",     "Deleted");
		case EGitLink_Status::Untracked:   return LOCTEXT("Untracked",   "Untracked");
		case EGitLink_Status::Ignored:     return LOCTEXT("Ignored",     "Ignored");
		case EGitLink_Status::CheckedOut:  return LOCTEXT("CheckedOut",  "Checked Out");
		case EGitLink_Status::LockedOther: return LOCTEXT("LockedOther", "Locked by Other");
		case EGitLink_Status::Lockable:    return LOCTEXT("Lockable",    "Lockable");
		case EGitLink_Status::NotAtHead:   return LOCTEXT("NotAtHead",   "Not At Head");
		case EGitLink_Status::NotLatest:   return LOCTEXT("NotLatest",   "Not Latest");
		case EGitLink_Status::Unmerged:    return LOCTEXT("Unmerged",    "Unmerged");
		default:                           return LOCTEXT("Unknown",     "Unknown");
	}
}

auto FGitLink_FileState::GetDisplayTooltip() const -> FText
{
	return GetDisplayName();
}

// --------------------------------------------------------------------------------------------------------------------
// Predicates
// --------------------------------------------------------------------------------------------------------------------
auto FGitLink_FileState::CanCheckIn() const -> bool
{
	return _State.File == EGitLink_FileState::Added
	    || _State.File == EGitLink_FileState::Deleted
	    || _State.File == EGitLink_FileState::Modified
	    || _State.File == EGitLink_FileState::Renamed;
}

auto FGitLink_FileState::CanCheckout() const -> bool
{
	// "Checkout" in Unreal source control == acquire an LFS lock.
	//
	// Untracked and Ignored files aren't in git yet — you can't lock a file that git-lfs
	// doesn't know about. The editor would otherwise prompt to check out new files that
	// were just created, which is nonsensical.
	const bool bUntracked =
	   _State.Tree == EGitLink_TreeState::Untracked
	|| _State.Tree == EGitLink_TreeState::Ignored
	|| _State.Tree == EGitLink_TreeState::NotInRepo;

	// Permissive policy for the lock state: allow checkout unless we positively know the
	// file is unlockable or already locked. Files that haven't been fully queried yet
	// (Lock == Unknown or Unset) are given the benefit of the doubt — Cmd_CheckOut will
	// filter to lockable extensions before actually calling git-lfs, so non-lockable files
	// won't accidentally get locked.
	const bool bLockOk =
	   _State.Lock != EGitLink_LockState::Unlockable
	&& _State.Lock != EGitLink_LockState::Locked
	&& _State.Lock != EGitLink_LockState::LockedOther;

	return !bUntracked && bLockOk;
}

auto FGitLink_FileState::IsCheckedOut() const -> bool
{
	return _State.Lock == EGitLink_LockState::Locked;
}

auto FGitLink_FileState::IsCheckedOutOther(FString* OutWho) const -> bool
{
	if (_State.Lock == EGitLink_LockState::LockedOther)
	{
		if (OutWho != nullptr)
		{ *OutWho = _State.LockUser; }
		return true;
	}
	return false;
}

auto FGitLink_FileState::IsCheckedOutInOtherBranch(const FString& /*InCurrentBranch*/) const -> bool
{
	// v1: we don't track per-branch modifications yet.
	return false;
}

auto FGitLink_FileState::IsModifiedInOtherBranch(const FString& /*InCurrentBranch*/) const -> bool
{
	return false;
}

auto FGitLink_FileState::GetOtherBranchHeadModification(
	FString& /*OutHeadBranch*/,
	FString& /*OutAction*/,
	int32& /*OutHeadChangeList*/) const -> bool
{
	return false;
}

auto FGitLink_FileState::IsCurrent() const -> bool
{
	return _State.Remote == EGitLink_RemoteState::UpToDate;
}

auto FGitLink_FileState::IsSourceControlled() const -> bool
{
	return _State.Tree != EGitLink_TreeState::Untracked
	    && _State.Tree != EGitLink_TreeState::Ignored
	    && _State.Tree != EGitLink_TreeState::NotInRepo
	    && _State.Tree != EGitLink_TreeState::Unset;
}

auto FGitLink_FileState::IsAdded() const -> bool
{
	return _State.File == EGitLink_FileState::Added;
}

auto FGitLink_FileState::IsDeleted() const -> bool
{
	return _State.File == EGitLink_FileState::Deleted;
}

auto FGitLink_FileState::IsIgnored() const -> bool
{
	return _State.Tree == EGitLink_TreeState::Ignored;
}

auto FGitLink_FileState::CanEdit() const -> bool
{
	// Editing is blocked only when someone else holds the LFS lock. Everything else is fair game.
	return _State.Lock != EGitLink_LockState::LockedOther;
}

auto FGitLink_FileState::IsUnknown() const -> bool
{
	return _State.File == EGitLink_FileState::Unknown
	    && _State.Tree == EGitLink_TreeState::Unset;
}

auto FGitLink_FileState::IsModified() const -> bool
{
	return _State.File == EGitLink_FileState::Modified
	    || _State.File == EGitLink_FileState::Added
	    || _State.File == EGitLink_FileState::Deleted
	    || _State.File == EGitLink_FileState::Renamed
	    || _State.File == EGitLink_FileState::Unmerged;
}

auto FGitLink_FileState::CanAdd() const -> bool
{
	return _State.Tree == EGitLink_TreeState::Untracked;
}

auto FGitLink_FileState::CanDelete() const -> bool
{
	return IsSourceControlled() && _State.File != EGitLink_FileState::Deleted;
}

auto FGitLink_FileState::IsConflicted() const -> bool
{
	return _State.File == EGitLink_FileState::Unmerged;
}

auto FGitLink_FileState::CanRevert() const -> bool
{
	// A file can be reverted if it has local modifications (the usual case) OR if it's
	// locked by us but not yet modified — reverting a locked-but-clean file releases the
	// LFS lock, which is the only way to "un-checkout" in a locking-based workflow.
	return IsModified() || _State.Lock == EGitLink_LockState::Locked;
}

#undef LOCTEXT_NAMESPACE
