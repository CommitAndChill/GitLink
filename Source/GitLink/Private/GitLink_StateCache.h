#pragma once

#include "GitLink/GitLink_Changelist.h"
#include "GitLink/GitLink_State.h"
#include "GitLink_ChangelistState.h"

#include <CoreMinimal.h>
#include <HAL/CriticalSection.h>
#include <Misc/ScopeRWLock.h>

// --------------------------------------------------------------------------------------------------------------------
// Thread-safe cache of per-file states and per-changelist states.
//
// Protected by an FRWLock so concurrent readers (e.g. a background poll) can't race an
// ongoing UpdateStatus write.
// --------------------------------------------------------------------------------------------------------------------

class FGitLink_StateCache
{
public:
	// --- File state access ---

	// Fetches an existing state, or creates a default-constructed one if absent. Always returns a valid ref.
	auto GetOrCreate_FileState(const FString& InFilename) -> FGitLink_FileStateRef;

	// Read-only lookup. Returns nullptr if absent.
	auto Find_FileState(const FString& InFilename) const -> FGitLink_FileStateRef;

	// Overwrite or insert a file state.
	auto Set_FileState(const FString& InFilename, FGitLink_FileStateRef InState) -> void;

	// Remove a file from the cache. Returns true if it was present.
	auto Remove_FileState(const FString& InFilename) -> bool;

	// Snapshot all cached files for GetCachedStateByPredicate.
	auto Enumerate_FileStates(TFunctionRef<bool(const FGitLink_FileStateRef&)> InPredicate) const
		-> TArray<FGitLink_FileStateRef>;

	auto Get_NumFiles() const -> int32;
	auto Get_AllFilenames() const -> TArray<FString>;

	// --- Changelist state access ---

	auto GetOrCreate_ChangelistState(const FGitLink_Changelist& InChangelist) -> FGitLink_ChangelistStateRef;
	auto Find_ChangelistState(const FGitLink_Changelist& InChangelist) const -> FGitLink_ChangelistStateRef;
	auto Set_ChangelistState(const FGitLink_Changelist& InChangelist, FGitLink_ChangelistStateRef InState) -> void;
	auto Enumerate_Changelists() const -> TArray<FGitLink_ChangelistStateRef>;

	// --- Ignore-force helpers ---
	// Unreal's source control often issues a redundant UpdateStatus immediately after an operation.
	// Add a file to this set so the next UpdateStatus leaves it alone.

	auto Add_IgnoreForceRefresh   (const FString& InFilename) -> bool;  // true if inserted
	auto Remove_IgnoreForceRefresh(const FString& InFilename) -> bool;  // true if removed
	auto ShouldIgnoreForceRefresh (const FString& InFilename) const -> bool;

	// --- Bulk ops ---
	auto Clear() -> void;

private:
	mutable FRWLock _FileStatesLock;
	TMap<FString, FGitLink_FileStateRef> _FileStates;

	mutable FRWLock _ChangelistStatesLock;
	TMap<FGitLink_Changelist, FGitLink_ChangelistStateRef> _ChangelistStates;

	mutable FCriticalSection _IgnoreForceLock;
	TSet<FString> _IgnoreForceRefresh;
};
