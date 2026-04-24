#include "GitLink/GitLink_State.h"

#include <CoreMinimal.h>
#include <Misc/AutomationTest.h>

// --------------------------------------------------------------------------------------------------------------------
// Tests for FGitLink_FileState predicates — especially the submodule-file regression where UE5's
// PromptToCheckoutPackages would fire a "Check Out Assets" dialog whenever a file reported
// IsSourceControlled()=true AND CanCheckout()=false. The fix adds a bInSubmodule flag that flips
// IsCheckedOut() to true, which short-circuits the dialog via UE's
//     bAlreadyCheckedOut = IsCheckedOut() || IsAdded()
// branch. These tests lock down that exact combination so the regression can't silently return.
// --------------------------------------------------------------------------------------------------------------------

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	// Build a file state with the given composite fields populated. Defaults leave every
	// dimension at its "nothing known yet" value so tests only need to set the fields they care
	// about.
	auto MakeState(const FGitLink_CompositeState& InState) -> FGitLink_FileState
	{
		FGitLink_FileState F(TEXT("x.uasset"));
		F._State = InState;
		return F;
	}
}

// --------------------------------------------------------------------------------------------------------------------
// Submodule file: the regression case. All five assertions must hold simultaneously or the dialog
// comes back / History goes away / mutation menu items show up bogusly.
// --------------------------------------------------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGitLinkTests_FileState_Submodule_SuppressesCheckoutDialog,
	"GitLink.FileState.Submodule.SuppressesCheckoutDialog",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGitLinkTests_FileState_Submodule_SuppressesCheckoutDialog::RunTest(const FString& /*Parameters*/)
{
	FGitLink_CompositeState S;
	S.bInSubmodule = true;
	S.Tree = EGitLink_TreeState::Unmodified;
	S.Lock = EGitLink_LockState::Unlockable;

	const FGitLink_FileState F = MakeState(S);

	// UE's dialog trigger: if !IsCheckedOut() && !IsAdded() && IsSourceControlled() && !CanCheckout(),
	// the "Check Out Assets" dialog appears. For submodule files we want NO dialog, so
	// IsCheckedOut() OR IsAdded() must be true.
	TestTrue(TEXT("IsCheckedOut() OR IsAdded() must be true (bypasses UE's checkout dialog)"),
		F.IsCheckedOut() || F.IsAdded());

	// History is gated on IsSourceControlled() — keep it enabled so users can view commit history
	// for submodule assets (BuildHistory in Cmd_UpdateStatus opens the submodule repo).
	TestTrue(TEXT("IsSourceControlled() must be true so History menu stays enabled"),
		F.IsSourceControlled());

	// All mutation actions must be disabled — the parent repo can't modify submodule contents.
	TestFalse(TEXT("CanCheckout() must be false for submodule files"),         F.CanCheckout());
	TestFalse(TEXT("CanCheckIn() must be false for submodule files"),          F.CanCheckIn());
	TestFalse(TEXT("CanRevert() must be false for submodule files"),           F.CanRevert());
	TestFalse(TEXT("CanAdd() must be false for submodule files"),              F.CanAdd());
	TestFalse(TEXT("CanDelete() must be false for submodule files"),           F.CanDelete());

	return true;
}

// --------------------------------------------------------------------------------------------------------------------
// Submodule icon: the icon picker (Get_OverallStatus) must not return CheckedOut just because
// IsCheckedOut() is true — the icon comes from the composite state fields directly, not from the
// UE predicate. A submodule file with a clean Unmodified+Unlockable state should display as
// Unmodified (no misleading red checkmark).
// --------------------------------------------------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGitLinkTests_FileState_Submodule_IconIsUnmodified,
	"GitLink.FileState.Submodule.IconIsUnmodified",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGitLinkTests_FileState_Submodule_IconIsUnmodified::RunTest(const FString& /*Parameters*/)
{
	FGitLink_CompositeState S;
	S.bInSubmodule = true;
	S.Tree = EGitLink_TreeState::Unmodified;
	S.Lock = EGitLink_LockState::Unlockable;

	const FGitLink_FileState F = MakeState(S);

	TestEqual(TEXT("Overall status should be Unmodified (no misleading CheckedOut icon)"),
		static_cast<uint8>(F.Get_OverallStatus()),
		static_cast<uint8>(EGitLink_Status::Unmodified));

	return true;
}

// --------------------------------------------------------------------------------------------------------------------
// Non-submodule LFS lockable file, not yet locked — must behave normally: checkoutable, in SCC,
// not already checked out. This is the baseline case the submodule override must NOT break.
// --------------------------------------------------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGitLinkTests_FileState_NormalFile_CheckoutableWhenNotLocked,
	"GitLink.FileState.NormalFile.CheckoutableWhenNotLocked",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGitLinkTests_FileState_NormalFile_CheckoutableWhenNotLocked::RunTest(const FString& /*Parameters*/)
{
	FGitLink_CompositeState S;
	S.bInSubmodule = false;
	S.Tree = EGitLink_TreeState::Unmodified;
	S.Lock = EGitLink_LockState::NotLocked;

	const FGitLink_FileState F = MakeState(S);

	TestTrue (TEXT("IsSourceControlled() true for tracked file"), F.IsSourceControlled());
	TestFalse(TEXT("IsCheckedOut() false when not locked"),       F.IsCheckedOut());
	TestTrue (TEXT("CanCheckout() true when not locked"),         F.CanCheckout());

	return true;
}

// --------------------------------------------------------------------------------------------------------------------
// Non-submodule file locked by us — the "checked out" case. IsCheckedOut() should be true because
// of Lock=Locked (NOT because of the submodule flag), CanCheckout=false because it's already ours.
// --------------------------------------------------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGitLinkTests_FileState_NormalFile_CheckedOutWhenLocked,
	"GitLink.FileState.NormalFile.CheckedOutWhenLocked",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGitLinkTests_FileState_NormalFile_CheckedOutWhenLocked::RunTest(const FString& /*Parameters*/)
{
	FGitLink_CompositeState S;
	S.bInSubmodule = false;
	S.Tree = EGitLink_TreeState::Unmodified;
	S.Lock = EGitLink_LockState::Locked;

	const FGitLink_FileState F = MakeState(S);

	TestTrue (TEXT("IsCheckedOut() true when Lock=Locked"),             F.IsCheckedOut());
	TestFalse(TEXT("CanCheckout() false when already locked by us"),    F.CanCheckout());

	return true;
}

// --------------------------------------------------------------------------------------------------------------------
// Non-submodule Unlockable file (.txt, source, etc.) — NOT checked out. This is the case the
// previous dialog fix (just setting Lock=Unlockable) conflated with submodule files: before
// bInSubmodule, a submodule file and a .txt file had identical state and both failed CanCheckout.
// Now only the submodule variant reports IsCheckedOut()=true.
// --------------------------------------------------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGitLinkTests_FileState_UnlockableNonSubmodule_NotCheckedOut,
	"GitLink.FileState.UnlockableNonSubmodule.NotCheckedOut",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGitLinkTests_FileState_UnlockableNonSubmodule_NotCheckedOut::RunTest(const FString& /*Parameters*/)
{
	FGitLink_CompositeState S;
	S.bInSubmodule = false;
	S.Tree = EGitLink_TreeState::Unmodified;
	S.Lock = EGitLink_LockState::Unlockable;  // Non-.uasset file, say a .txt

	const FGitLink_FileState F = MakeState(S);

	TestFalse(TEXT("IsCheckedOut() false for Unlockable non-submodule files"),
		F.IsCheckedOut());
	TestFalse(TEXT("CanCheckout() false for Unlockable files (can't LFS-lock them)"),
		F.CanCheckout());

	return true;
}

// --------------------------------------------------------------------------------------------------------------------
// Submodule flag wins over Lock state: even if some path set Lock=NotLocked on a submodule file
// (the original bug from CLAUDE.md Pitfall #5), IsCheckedOut() must still report true to keep
// the dialog suppressed. This guards against future Lock-stamping regressions in Cmd_UpdateStatus
// or Cmd_Connect.
// --------------------------------------------------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGitLinkTests_FileState_Submodule_OverridesLockState,
	"GitLink.FileState.Submodule.OverridesLockState",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGitLinkTests_FileState_Submodule_OverridesLockState::RunTest(const FString& /*Parameters*/)
{
	// Simulate the regression: submodule file with Lock=NotLocked (what Cmd_Connect's Stage 1
	// loop produced before we added submodule handling). Without bInSubmodule the predicates
	// would have said IsSourceControlled=true + CanCheckout=true → UE would try to check out
	// and hit the parent's LFS server.
	FGitLink_CompositeState S;
	S.bInSubmodule = true;
	S.Tree = EGitLink_TreeState::Unmodified;
	S.Lock = EGitLink_LockState::NotLocked;

	const FGitLink_FileState F = MakeState(S);

	TestTrue (TEXT("IsCheckedOut() stays true for submodule regardless of Lock"),
		F.IsCheckedOut());
	TestFalse(TEXT("CanCheckout() stays false for submodule regardless of Lock"),
		F.CanCheckout());

	return true;
}

#endif  // WITH_DEV_AUTOMATION_TESTS
