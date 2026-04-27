#include "GitLink/GitLink_State.h"

#include <CoreMinimal.h>
#include <Misc/AutomationTest.h>

// --------------------------------------------------------------------------------------------------------------------
// Tests for FGitLink_FileState predicates.
//
// v2 contract for submodule files (see GitLink_State.h doc on bInSubmodule):
//   - IsSourceControlled() short-circuits to true (so History/Diff stay enabled even before
//     per-submodule status has populated the cache).
//   - IsCurrent() short-circuits to true (we don't poll submodule remote tracking; without
//     this short-circuit, Unreal's pre-delete check fires "not at latest" on every submodule
//     file because Remote stays at its Unset default).
//   - All other predicates (CanCheckout / CanCheckIn / CanAdd / CanDelete / CanRevert /
//     IsCheckedOut) behave identically for submodule and outer-repo files. Routing into
//     the inner repo happens at the command layer via PartitionByRepo, not in the predicates.
//
// These tests lock down both the short-circuits and the "no special-casing" behavior — if a
// regression sneaks the old v1 short-circuits back in, multiple assertions here will fail.
// --------------------------------------------------------------------------------------------------------------------

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	auto MakeState(const FGitLink_CompositeState& InState) -> FGitLink_FileState
	{
		FGitLink_FileState F(TEXT("x.uasset"));
		F._State = InState;
		return F;
	}
}

// --------------------------------------------------------------------------------------------------------------------
// Outer-repo baseline: a tracked, unmodified, lockable, not-yet-locked file. The classic
// "ready to be checked out" case the editor cares about. This is the contract the submodule
// behavior must match (no v1-style submodule short-circuits forcing CanCheckout=false).
// --------------------------------------------------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGitLinkTests_FileState_OuterUnlocked_IsCheckoutable,
	"GitLink.FileState.OuterUnlocked.IsCheckoutable",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGitLinkTests_FileState_OuterUnlocked_IsCheckoutable::RunTest(const FString& /*Parameters*/)
{
	FGitLink_CompositeState S;
	S.bInSubmodule = false;
	S.Tree         = EGitLink_TreeState::Unmodified;
	S.Lock         = EGitLink_LockState::NotLocked;

	const FGitLink_FileState F = MakeState(S);

	TestTrue (TEXT("IsSourceControlled true for tracked file"), F.IsSourceControlled());
	TestFalse(TEXT("IsCheckedOut false when not locked"),       F.IsCheckedOut());
	TestTrue (TEXT("CanCheckout true when not locked"),         F.CanCheckout());
	TestFalse(TEXT("CanCheckIn false when not modified"),       F.CanCheckIn());
	TestFalse(TEXT("CanRevert false when clean"),               F.CanRevert());
	TestTrue (TEXT("CanDelete true for tracked, not-deleted"),  F.CanDelete());
	TestFalse(TEXT("CanAdd false for already-tracked file"),    F.CanAdd());

	return true;
}

// --------------------------------------------------------------------------------------------------------------------
// Outer-repo file we hold the lock on — IsCheckedOut=true (because Lock=Locked, NOT because
// of any submodule shortcut), CanCheckout=false (already ours), CanRevert=true (so we can
// release the lock).
// --------------------------------------------------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGitLinkTests_FileState_OuterLocked_IsCheckedOut,
	"GitLink.FileState.OuterLocked.IsCheckedOut",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGitLinkTests_FileState_OuterLocked_IsCheckedOut::RunTest(const FString& /*Parameters*/)
{
	FGitLink_CompositeState S;
	S.bInSubmodule = false;
	S.Tree         = EGitLink_TreeState::Unmodified;
	S.Lock         = EGitLink_LockState::Locked;

	const FGitLink_FileState F = MakeState(S);

	TestTrue (TEXT("IsCheckedOut true when Lock=Locked"),          F.IsCheckedOut());
	TestFalse(TEXT("CanCheckout false when already locked by us"), F.CanCheckout());
	TestTrue (TEXT("CanRevert true when locked (releases lock)"),  F.CanRevert());

	return true;
}

// --------------------------------------------------------------------------------------------------------------------
// Outer-repo Unlockable file (e.g., .txt) — not lockable per .gitattributes. CanCheckout
// must be false because there's nothing to lock; IsCheckedOut also false (no lock = not
// checked out).
// --------------------------------------------------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGitLinkTests_FileState_OuterUnlockable_NotCheckedOut,
	"GitLink.FileState.OuterUnlockable.NotCheckedOut",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGitLinkTests_FileState_OuterUnlockable_NotCheckedOut::RunTest(const FString& /*Parameters*/)
{
	FGitLink_CompositeState S;
	S.bInSubmodule = false;
	S.Tree         = EGitLink_TreeState::Unmodified;
	S.Lock         = EGitLink_LockState::Unlockable;

	const FGitLink_FileState F = MakeState(S);

	TestFalse(TEXT("IsCheckedOut false for Unlockable non-submodule"), F.IsCheckedOut());
	TestFalse(TEXT("CanCheckout false for Unlockable file"),           F.CanCheckout());

	return true;
}

// --------------------------------------------------------------------------------------------------------------------
// v2 SUBMODULE CONTRACT — predicates behave identically to outer-repo equivalents.
//
// Submodule, lockable, not yet locked → CanCheckout MUST be true. The editor offers the
// Check Out menu item and Cmd_CheckOut routes `git lfs lock` to the submodule's own LFS
// server. Pre-v2 this was forced to false, which prevented locking submodule files at all.
// --------------------------------------------------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGitLinkTests_FileState_SubmoduleUnlocked_IsCheckoutable,
	"GitLink.FileState.SubmoduleUnlocked.IsCheckoutable",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGitLinkTests_FileState_SubmoduleUnlocked_IsCheckoutable::RunTest(const FString& /*Parameters*/)
{
	FGitLink_CompositeState S;
	S.bInSubmodule = true;
	S.Tree         = EGitLink_TreeState::Unmodified;
	S.Lock         = EGitLink_LockState::NotLocked;

	const FGitLink_FileState F = MakeState(S);

	TestTrue (TEXT("IsSourceControlled true for submodule file"),   F.IsSourceControlled());
	TestFalse(TEXT("IsCheckedOut false (Lock=NotLocked, not v1 forced-true)"), F.IsCheckedOut());
	TestTrue (TEXT("CanCheckout true — routes to submodule LFS"),  F.CanCheckout());
	TestTrue (TEXT("CanDelete true — routes to submodule index"),  F.CanDelete());

	return true;
}

// --------------------------------------------------------------------------------------------------------------------
// Submodule file we hold the lock on. Same shape as the outer-repo locked case.
// --------------------------------------------------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGitLinkTests_FileState_SubmoduleLocked_IsCheckedOut,
	"GitLink.FileState.SubmoduleLocked.IsCheckedOut",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGitLinkTests_FileState_SubmoduleLocked_IsCheckedOut::RunTest(const FString& /*Parameters*/)
{
	FGitLink_CompositeState S;
	S.bInSubmodule = true;
	S.Tree         = EGitLink_TreeState::Unmodified;
	S.Lock         = EGitLink_LockState::Locked;

	const FGitLink_FileState F = MakeState(S);

	TestTrue (TEXT("IsCheckedOut true when Lock=Locked (regardless of submodule)"),
		F.IsCheckedOut());
	TestFalse(TEXT("CanCheckout false when already locked by us"),
		F.CanCheckout());
	TestTrue (TEXT("CanRevert true (so user can release the submodule lock)"),
		F.CanRevert());

	return true;
}

// --------------------------------------------------------------------------------------------------------------------
// REGRESSION: untracked submodule files must NOT be checkable-out.
//
// A new file the user just dropped into a submodule directory has Tree=Untracked (set by
// per-submodule Get_Status() in Cmd_UpdateStatus's full-scan path). CanCheckout's bUntracked
// guard must reject it — you can't `git lfs lock` a file that isn't tracked. Pre-fix, the
// optimistic Tree=Unmodified default for unknown submodule files defeated this guard and
// the editor offered Check Out on brand new files.
// --------------------------------------------------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGitLinkTests_FileState_SubmoduleUntracked_NotCheckoutable,
	"GitLink.FileState.SubmoduleUntracked.NotCheckoutable",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGitLinkTests_FileState_SubmoduleUntracked_NotCheckoutable::RunTest(const FString& /*Parameters*/)
{
	FGitLink_CompositeState S;
	S.bInSubmodule = true;
	S.Tree         = EGitLink_TreeState::Untracked;
	S.Lock         = EGitLink_LockState::NotLocked;

	const FGitLink_FileState F = MakeState(S);

	TestFalse(TEXT("CanCheckout false — can't LFS-lock an untracked file"),
		F.CanCheckout());
	// CanAdd is true for untracked files in either repo — Cmd_MarkForAdd routes to the
	// submodule's index.
	TestTrue (TEXT("CanAdd true for untracked submodule file"), F.CanAdd());

	return true;
}

// --------------------------------------------------------------------------------------------------------------------
// Modified submodule file → CanCheckIn / CanRevert true, just like outer-repo files.
// --------------------------------------------------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGitLinkTests_FileState_SubmoduleModified_CanCommitAndRevert,
	"GitLink.FileState.SubmoduleModified.CanCommitAndRevert",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGitLinkTests_FileState_SubmoduleModified_CanCommitAndRevert::RunTest(const FString& /*Parameters*/)
{
	FGitLink_CompositeState S;
	S.bInSubmodule = true;
	S.File         = EGitLink_FileState::Modified;
	S.Tree         = EGitLink_TreeState::Working;
	S.Lock         = EGitLink_LockState::Locked;

	const FGitLink_FileState F = MakeState(S);

	TestTrue(TEXT("CanCheckIn true for modified submodule file"), F.CanCheckIn());
	TestTrue(TEXT("CanRevert true for modified submodule file"), F.CanRevert());

	return true;
}

// --------------------------------------------------------------------------------------------------------------------
// Submodule short-circuit: IsCurrent() must be true even when Remote is at the default
// Unset value. We don't poll submodule remote tracking branches, so the optimistic-true
// default is required to keep Unreal's pre-delete check (which calls IsCurrent()) from
// firing the misleading "not at latest" error on every submodule file.
//
// This was the literal blocker behind the original "Fix Up Redirectors fails on submodule
// files" report.
// --------------------------------------------------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGitLinkTests_FileState_Submodule_IsCurrentEvenWithUnsetRemote,
	"GitLink.FileState.Submodule.IsCurrentEvenWithUnsetRemote",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGitLinkTests_FileState_Submodule_IsCurrentEvenWithUnsetRemote::RunTest(const FString& /*Parameters*/)
{
	FGitLink_CompositeState S;
	S.bInSubmodule = true;
	// Remote left at its default (the field default is UpToDate, but that's beside the
	// point — even if a stale code path leaves it Unset/NotAtHead, the submodule short-
	// circuit must dominate). Force NotAtHead to prove the override.
	S.Remote       = EGitLink_RemoteState::NotAtHead;
	S.Tree         = EGitLink_TreeState::Unmodified;

	const FGitLink_FileState F = MakeState(S);

	TestTrue(TEXT("IsCurrent true for submodule file regardless of Remote state"),
		F.IsCurrent());

	// Confirm an outer-repo file with the same Remote DOES report not-current.
	S.bInSubmodule = false;
	const FGitLink_FileState OuterF = MakeState(S);
	TestFalse(TEXT("IsCurrent false for outer-repo file when Remote=NotAtHead"),
		OuterF.IsCurrent());

	return true;
}

// --------------------------------------------------------------------------------------------------------------------
// Submodule short-circuit: IsSourceControlled() must be true even when Tree=NotInRepo.
// The parent repo's libgit2 status walk doesn't recurse into submodules, so files in a
// submodule the parent has never seen will hit GetState's optimistic-Unmodified default
// — but if some path leaves Tree at NotInRepo, History/Diff must still work.
// --------------------------------------------------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGitLinkTests_FileState_Submodule_IsSourceControlledEvenIfNotInRepo,
	"GitLink.FileState.Submodule.IsSourceControlledEvenIfNotInRepo",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGitLinkTests_FileState_Submodule_IsSourceControlledEvenIfNotInRepo::RunTest(const FString& /*Parameters*/)
{
	FGitLink_CompositeState S;
	S.bInSubmodule = true;
	S.Tree         = EGitLink_TreeState::NotInRepo;

	const FGitLink_FileState F = MakeState(S);

	TestTrue(TEXT("IsSourceControlled true for submodule file even if Tree=NotInRepo"),
		F.IsSourceControlled());

	// Outer-repo equivalent must NOT short-circuit.
	S.bInSubmodule = false;
	const FGitLink_FileState OuterF = MakeState(S);
	TestFalse(TEXT("IsSourceControlled false for outer-repo file when Tree=NotInRepo"),
		OuterF.IsSourceControlled());

	return true;
}

// --------------------------------------------------------------------------------------------------------------------
// Locked-by-other: editor shows "Checked out by X" icon. CanCheckout must be false (we
// can't take a lock someone else holds), CanEdit must be false (read-only). Same in both
// outer and submodule repos.
// --------------------------------------------------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGitLinkTests_FileState_LockedOther_NotEditable,
	"GitLink.FileState.LockedOther.NotEditable",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGitLinkTests_FileState_LockedOther_NotEditable::RunTest(const FString& /*Parameters*/)
{
	for (const bool bSub : { false, true })
	{
		FGitLink_CompositeState S;
		S.bInSubmodule = bSub;
		S.Tree         = EGitLink_TreeState::Unmodified;
		S.Lock         = EGitLink_LockState::LockedOther;
		S.LockUser     = TEXT("Sulfur-CK");

		const FGitLink_FileState F = MakeState(S);

		const TCHAR* Tag = bSub ? TEXT("(submodule)") : TEXT("(outer)");
		TestFalse(*FString::Printf(TEXT("CanCheckout false when locked by other %s"), Tag),
			F.CanCheckout());
		TestFalse(*FString::Printf(TEXT("CanEdit false when locked by other %s"),     Tag),
			F.CanEdit());
		TestEqual(*FString::Printf(TEXT("Status icon is LockedOther %s"),             Tag),
			static_cast<uint8>(F.Get_OverallStatus()),
			static_cast<uint8>(EGitLink_Status::LockedOther));
	}
	return true;
}

#endif  // WITH_DEV_AUTOMATION_TESTS
