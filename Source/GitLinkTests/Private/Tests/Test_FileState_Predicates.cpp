#include "GitLink/GitLink_State.h"

#include <CoreMinimal.h>
#include <Misc/AutomationTest.h>

// --------------------------------------------------------------------------------------------------------------------
// Tests for FGitLink_FileState predicates.
//
// Current contract for submodule files (see GitLink_State.cpp + Plugins/GitLink/CLAUDE.md
// "State predicate contract for submodule files"):
//
//   - IsSourceControlled() is **Tree-state driven**, identical to outer-repo files. Tracked
//     submodule files (Tree=Unmodified/Working/Staged) report true; untracked submodule files
//     (Tree=Untracked/NotInRepo/Ignored) report false. Correct Tree state is supplied at
//     populate time via Provider::Is_TrackedInSubmodule (per-submodule index built at connect).
//
//     This used to short-circuit to true for any submodule file, but that workaround caused
//     the editor's auto-CheckOut-on-save flow to prompt locking on brand-new files in
//     submodules — so it was removed. The regression test for that is
//     SubmoduleUntracked_NotSourceControlled below.
//
//   - IsCurrent() short-circuits to true (we don't poll submodule remote tracking; without
//     this short-circuit, Unreal's pre-delete check fires "not at latest" on every submodule
//     file because Remote stays at its Unset default).
//
//   - CanCheckout / CanCheckIn / CanAdd / CanDelete / CanRevert / IsCheckedOut behave
//     identically for submodule and outer-repo files. Repo routing happens at the command
//     layer via PartitionByRepo, not in the predicates. CanCheckout has one extra gate that
//     applies to BOTH repos: File=Added (staged-as-added but never committed) is not
//     lockable — locks are only meaningful once the file exists in HEAD.
//
// These tests lock down the contract — if a regression flips IsSourceControlled back to
// force-true for submodules, or removes the File=Added block on CanCheckout, multiple
// assertions here will fail.
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
// REGRESSION: a brand-new untracked file in a submodule must report NOT source-controlled
// AND NOT checkable-out. This is the configuration that triggered the "auto-CheckOut on
// save in submodules" regression — the editor saw IsSourceControlled=true (forced for any
// submodule file pre-fix) and ran CheckOut on save, prompting users to LFS-lock files that
// weren't even in git yet.
//
// Both gates are defenses-in-depth and both must hold:
//   - IsSourceControlled=false stops UE's auto-CheckOut-on-save flow at the gate.
//   - CanCheckout=false also keeps the right-click "Check Out" menu item disabled.
// --------------------------------------------------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGitLinkTests_FileState_SubmoduleUntracked_NotSourceControlledNotCheckoutable,
	"GitLink.FileState.SubmoduleUntracked.NotSourceControlledNotCheckoutable",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGitLinkTests_FileState_SubmoduleUntracked_NotSourceControlledNotCheckoutable::RunTest(const FString& /*Parameters*/)
{
	FGitLink_CompositeState S;
	S.bInSubmodule = true;
	S.Tree         = EGitLink_TreeState::Untracked;
	S.Lock         = EGitLink_LockState::NotLocked;

	const FGitLink_FileState F = MakeState(S);

	TestFalse(TEXT("IsSourceControlled false — gates UE's auto-CheckOut-on-save flow"),
		F.IsSourceControlled());
	TestFalse(TEXT("CanCheckout false — also disables right-click Check Out menu item"),
		F.CanCheckout());
	// CanAdd is true for untracked files in either repo — Cmd_MarkForAdd routes to the
	// submodule's index.
	TestTrue (TEXT("CanAdd true for untracked submodule file"), F.CanAdd());

	// Re-test for an untracked file with Tree=NotInRepo (the default after Provider::GetState
	// when Is_TrackedInSubmodule returns false). Same expectations.
	S.Tree = EGitLink_TreeState::NotInRepo;
	const FGitLink_FileState F2 = MakeState(S);
	TestFalse(TEXT("IsSourceControlled false for Tree=NotInRepo submodule file"),
		F2.IsSourceControlled());
	TestFalse(TEXT("CanCheckout false for Tree=NotInRepo submodule file"),
		F2.CanCheckout());

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
// REGRESSION: IsSourceControlled() must follow Tree state for BOTH outer-repo and submodule
// files. There is no "force-true for any submodule file" short-circuit anymore.
//
// The previous force-true behavior was the root cause of the "auto-CheckOut on save in
// submodules" regression: it made every untracked submodule file report as source-controlled,
// which drove UE's auto-checkout-on-save flow to prompt locking on brand-new files. Now that
// Provider::GetState stamps the correct Tree state via Is_TrackedInSubmodule (Unmodified for
// tracked submodule files, NotInRepo for untracked ones), the predicate can be uniform.
//
// History/Diff still work for tracked submodule files because their Tree=Unmodified
// (populated from the per-submodule git index at connect) satisfies this predicate.
// --------------------------------------------------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGitLinkTests_FileState_IsSourceControlled_FollowsTreeState_BothRepos,
	"GitLink.FileState.IsSourceControlled.FollowsTreeState.BothRepos",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGitLinkTests_FileState_IsSourceControlled_FollowsTreeState_BothRepos::RunTest(const FString& /*Parameters*/)
{
	for (const bool bSub : { false, true })
	{
		const TCHAR* Tag = bSub ? TEXT("(submodule)") : TEXT("(outer)");

		// Tracked-clean: source controlled. (Tree=Unmodified is what
		// Provider::Is_TrackedInSubmodule produces for files in the per-submodule index.)
		{
			FGitLink_CompositeState S;
			S.bInSubmodule = bSub;
			S.Tree         = EGitLink_TreeState::Unmodified;
			TestTrue(*FString::Printf(TEXT("IsSourceControlled true when Tree=Unmodified %s"), Tag),
				MakeState(S).IsSourceControlled());
		}

		// Untracked: NOT source controlled — this is the gate that stops auto-CheckOut on save.
		{
			FGitLink_CompositeState S;
			S.bInSubmodule = bSub;
			S.Tree         = EGitLink_TreeState::Untracked;
			TestFalse(*FString::Printf(TEXT("IsSourceControlled false when Tree=Untracked %s"), Tag),
				MakeState(S).IsSourceControlled());
		}

		// NotInRepo: NOT source controlled. Submodule files only end up here when
		// Is_TrackedInSubmodule returns false (i.e., the file isn't in the per-submodule
		// index). Pre-fix this returned true for submodule files, which was the regression.
		{
			FGitLink_CompositeState S;
			S.bInSubmodule = bSub;
			S.Tree         = EGitLink_TreeState::NotInRepo;
			TestFalse(*FString::Printf(TEXT("IsSourceControlled false when Tree=NotInRepo %s"), Tag),
				MakeState(S).IsSourceControlled());
		}

		// Ignored: NOT source controlled.
		{
			FGitLink_CompositeState S;
			S.bInSubmodule = bSub;
			S.Tree         = EGitLink_TreeState::Ignored;
			TestFalse(*FString::Printf(TEXT("IsSourceControlled false when Tree=Ignored %s"), Tag),
				MakeState(S).IsSourceControlled());
		}
	}
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

// --------------------------------------------------------------------------------------------------------------------
// REGRESSION: a file that was MarkForAdd'd but never committed (File=Added, Tree=Staged)
// must NOT be checkable-out, in either repo. LFS locks are only meaningful once the file
// exists in HEAD; before commit, no other working copy can have a conflicting version.
//
// Without this gate, the user lifecycle was: create file → MarkForAdd (auto, on save) →
// editor's auto-CheckOut path runs again on next save → prompts to lock a file that
// nobody else can possibly conflict on. Even a manual right-click → Check Out should be
// blocked here.
//
// CanCheckIn / CanRevert remain true (you must be able to commit the addition or undo it).
// --------------------------------------------------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGitLinkTests_FileState_FileAdded_NotCheckoutable_BothRepos,
	"GitLink.FileState.FileAdded.NotCheckoutable.BothRepos",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGitLinkTests_FileState_FileAdded_NotCheckoutable_BothRepos::RunTest(const FString& /*Parameters*/)
{
	for (const bool bSub : { false, true })
	{
		FGitLink_CompositeState S;
		S.bInSubmodule = bSub;
		S.File         = EGitLink_FileState::Added;
		S.Tree         = EGitLink_TreeState::Staged;
		S.Lock         = EGitLink_LockState::NotLocked;

		const FGitLink_FileState F = MakeState(S);

		const TCHAR* Tag = bSub ? TEXT("(submodule)") : TEXT("(outer)");
		TestFalse(*FString::Printf(TEXT("CanCheckout false for File=Added %s"), Tag),
			F.CanCheckout());
		TestTrue (*FString::Printf(TEXT("CanCheckIn true for File=Added %s — needed to commit the addition"), Tag),
			F.CanCheckIn());
		TestTrue (*FString::Printf(TEXT("CanRevert true for File=Added %s — needed to undo the addition"), Tag),
			F.CanRevert());
	}
	return true;
}

#endif  // WITH_DEV_AUTOMATION_TESTS
