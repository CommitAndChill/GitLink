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

	// Read-only lookup. Returns the cached state if present, otherwise a fresh
	// default-constructed state that is NOT inserted into the cache — callers that need to
	// distinguish "absent" from "present but default" should use Enumerate_FileStates.
	auto Find_FileState(const FString& InFilename) const -> FGitLink_FileStateRef;

	// Overwrite or insert a file state.
	auto Set_FileState(const FString& InFilename, FGitLink_FileStateRef InState) -> void;

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

	// Called by the provider after connect to install a snapshot of submodule paths.
	// New cache entries whose key starts with a submodule prefix are stamped Unlockable
	// at creation time, closing the timing window described in CLAUDE.md Pitfall #5.
	auto Set_SubmodulePaths(TArray<FString> InSubmodulePaths) -> void;

	// --- Bulk ops ---
	auto Clear() -> void;

private:
	mutable FRWLock _FileStatesLock;
	TMap<FString, FGitLink_FileStateRef> _FileStates;

	// Snapshot of submodule absolute paths (trailing-slash-terminated). Written once per
	// connect under _SubmodulePathsLock; read from GetOrCreate_FileState. Lock ordering:
	// always acquire _SubmodulePathsLock before _FileStatesLock.
	mutable FRWLock  _SubmodulePathsLock;
	TArray<FString>  _SubmodulePaths;

	mutable FRWLock _ChangelistStatesLock;
	TMap<FGitLink_Changelist, FGitLink_ChangelistStateRef> _ChangelistStates;
};
