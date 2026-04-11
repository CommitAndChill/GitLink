#include "GitLink_StateCache.h"

#include <Misc/Paths.h>

namespace
{
	// Canonicalize a file path so lookups are case- and format-consistent.
	// All commands store state with Normalize_AbsolutePath (ConvertRelativePathToFull +
	// NormalizeFilename), so we apply the same transform on every cache key.
	auto NormalizeKey(const FString& InPath) -> FString
	{
		FString Out = FPaths::ConvertRelativePathToFull(InPath);
		FPaths::NormalizeFilename(Out);
		return Out;
	}
}

// --------------------------------------------------------------------------------------------------------------------
// File states
// --------------------------------------------------------------------------------------------------------------------
auto FGitLink_StateCache::GetOrCreate_FileState(const FString& InFilename) -> FGitLink_FileStateRef
{
	const FString Key = NormalizeKey(InFilename);

	// Fast path: read lock, look up existing.
	{
		FReadScopeLock Read(_FileStatesLock);
		if (const FGitLink_FileStateRef* Existing = _FileStates.Find(Key))
		{ return *Existing; }
	}

	// Slow path: upgrade to write, insert, return.
	FWriteScopeLock Write(_FileStatesLock);

	// Re-check under the write lock in case another thread won the race.
	if (const FGitLink_FileStateRef* Existing = _FileStates.Find(Key))
	{ return *Existing; }

	FGitLink_FileStateRef New = MakeShared<FGitLink_FileState, ESPMode::ThreadSafe>(Key);
	_FileStates.Add(Key, New);
	return New;
}

auto FGitLink_StateCache::Find_FileState(const FString& InFilename) const -> FGitLink_FileStateRef
{
	const FString Key = NormalizeKey(InFilename);

	FReadScopeLock Read(_FileStatesLock);
	if (const FGitLink_FileStateRef* Existing = _FileStates.Find(Key))
	{ return *Existing; }

	// Return a fresh default state for callers that don't care about distinguishing unknown
	// from present. GetCachedStateByPredicate-style callers should use Enumerate instead.
	return MakeShared<FGitLink_FileState, ESPMode::ThreadSafe>(Key);
}

auto FGitLink_StateCache::Set_FileState(const FString& InFilename, FGitLink_FileStateRef InState) -> void
{
	const FString Key = NormalizeKey(InFilename);

	FWriteScopeLock Write(_FileStatesLock);
	_FileStates.Add(Key, MoveTemp(InState));
}

auto FGitLink_StateCache::Remove_FileState(const FString& InFilename) -> bool
{
	FWriteScopeLock Write(_FileStatesLock);
	return _FileStates.Remove(InFilename) > 0;
}

auto FGitLink_StateCache::Enumerate_FileStates(TFunctionRef<bool(const FGitLink_FileStateRef&)> InPredicate) const
	-> TArray<FGitLink_FileStateRef>
{
	TArray<FGitLink_FileStateRef> Out;

	FReadScopeLock Read(_FileStatesLock);
	Out.Reserve(_FileStates.Num());
	for (const TPair<FString, FGitLink_FileStateRef>& Pair : _FileStates)
	{
		if (InPredicate(Pair.Value))
		{ Out.Add(Pair.Value); }
	}
	return Out;
}

auto FGitLink_StateCache::Get_NumFiles() const -> int32
{
	FReadScopeLock Read(_FileStatesLock);
	return _FileStates.Num();
}

auto FGitLink_StateCache::Get_AllFilenames() const -> TArray<FString>
{
	FReadScopeLock Read(_FileStatesLock);
	TArray<FString> Out;
	_FileStates.GetKeys(Out);
	return Out;
}

// --------------------------------------------------------------------------------------------------------------------
// Changelist states
// --------------------------------------------------------------------------------------------------------------------
auto FGitLink_StateCache::GetOrCreate_ChangelistState(const FGitLink_Changelist& InChangelist)
	-> FGitLink_ChangelistStateRef
{
	{
		FReadScopeLock Read(_ChangelistStatesLock);
		if (const FGitLink_ChangelistStateRef* Existing = _ChangelistStates.Find(InChangelist))
		{ return *Existing; }
	}

	FWriteScopeLock Write(_ChangelistStatesLock);
	if (const FGitLink_ChangelistStateRef* Existing = _ChangelistStates.Find(InChangelist))
	{ return *Existing; }

	FGitLink_ChangelistStateRef New = MakeShared<FGitLink_ChangelistState, ESPMode::ThreadSafe>(InChangelist);
	_ChangelistStates.Add(InChangelist, New);
	return New;
}

auto FGitLink_StateCache::Find_ChangelistState(const FGitLink_Changelist& InChangelist) const
	-> FGitLink_ChangelistStateRef
{
	FReadScopeLock Read(_ChangelistStatesLock);
	if (const FGitLink_ChangelistStateRef* Existing = _ChangelistStates.Find(InChangelist))
	{ return *Existing; }

	return MakeShared<FGitLink_ChangelistState, ESPMode::ThreadSafe>(InChangelist);
}

auto FGitLink_StateCache::Set_ChangelistState(const FGitLink_Changelist& InChangelist, FGitLink_ChangelistStateRef InState) -> void
{
	FWriteScopeLock Write(_ChangelistStatesLock);
	_ChangelistStates.Add(InChangelist, MoveTemp(InState));
}

auto FGitLink_StateCache::Enumerate_Changelists() const -> TArray<FGitLink_ChangelistStateRef>
{
	FReadScopeLock Read(_ChangelistStatesLock);
	TArray<FGitLink_ChangelistStateRef> Out;
	Out.Reserve(_ChangelistStates.Num());
	for (const TPair<FGitLink_Changelist, FGitLink_ChangelistStateRef>& Pair : _ChangelistStates)
	{
		Out.Add(Pair.Value);
	}
	return Out;
}

// --------------------------------------------------------------------------------------------------------------------
// Ignore-force refresh
// --------------------------------------------------------------------------------------------------------------------
auto FGitLink_StateCache::Add_IgnoreForceRefresh(const FString& InFilename) -> bool
{
	FScopeLock Lock(&_IgnoreForceLock);
	bool bAlreadyPresent = false;
	_IgnoreForceRefresh.Add(InFilename, &bAlreadyPresent);
	return !bAlreadyPresent;
}

auto FGitLink_StateCache::Remove_IgnoreForceRefresh(const FString& InFilename) -> bool
{
	FScopeLock Lock(&_IgnoreForceLock);
	return _IgnoreForceRefresh.Remove(InFilename) > 0;
}

auto FGitLink_StateCache::ShouldIgnoreForceRefresh(const FString& InFilename) const -> bool
{
	FScopeLock Lock(&_IgnoreForceLock);
	return _IgnoreForceRefresh.Contains(InFilename);
}

// --------------------------------------------------------------------------------------------------------------------
auto FGitLink_StateCache::Clear() -> void
{
	{ FWriteScopeLock W(_FileStatesLock);        _FileStates.Empty();       }
	{ FWriteScopeLock W(_ChangelistStatesLock);  _ChangelistStates.Empty(); }
	{ FScopeLock      L(&_IgnoreForceLock);      _IgnoreForceRefresh.Empty(); }
}
