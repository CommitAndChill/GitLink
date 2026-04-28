# GitLink — Architecture & Development Guide

## Overview

GitLink is a clean-room Unreal source control plugin using libgit2 in-process, with subprocess fallback for LFS and hooks. Two modules: `GitLinkCore` (pure git facade) and `GitLink` (Unreal SCC integration).

## Code Style

- Trailing return types (`-> Type`)
- `Get_` / `Set_` / `Is_` / `Request_` method prefixes
- Early-return inverted-if style
- `auto` for local variables
- `namespace gitlink` for core types, `namespace gitlink::cmd` for commands, `namespace gitlink::op` for core operations

## Architecture

### Command Dispatch

All Unreal SCC operations route through `FGitLink_CommandDispatcher`:

```
Editor calls Provider.Execute(FCheckOut, Changelist, Files, Concurrency)
  -> Dispatcher.Dispatch("CheckOut", Files, Concurrency, Changelist)
    -> gitlink::cmd::CheckOut(Context, Operation, Files)  // free function in Cmd_CheckOut.cpp
    -> FCommandResult { UpdatedStates, UpdatedChangelistStates, bOk }
    -> Dispatcher applies states to cache on game thread
    -> Broadcasts OnSourceControlStateChanged
```

Each command is a **single free function** in `namespace gitlink::cmd`, registered in `GitLink_CommandDispatcher.cpp`. No worker class hierarchy.

The **destination changelist** is threaded through `Provider::Execute` → `Dispatcher::Dispatch` → `FCommandContext::DestinationChangelist`. Used by `MoveToChangelist` (stage/unstage direction) and `CheckIn` (changelist-scoped commits).

### State Cache

`FGitLink_StateCache` stores `TMap<FString, FGitLink_FileStateRef>` with **normalized path keys** (via `NormalizeKey()` — `ConvertRelativePathToFull` + `NormalizeFilename`). This is critical: the editor queries paths in various forms, and all must resolve to the same cache entry.

State predicates (`CanCheckout`, `CanRevert`, `IsCheckedOut`, etc.) are on `FGitLink_FileState` and drive every right-click menu option and content browser icon.

**Lock state preservation**: `UpdateStatus` and `UpdateChangelistsStatus` carry forward existing lock state from the cache instead of blindly stamping `NotLocked`. Lock state is owned exclusively by `Cmd_CheckOut` (sets Locked), `Cmd_Revert` (sets NotLocked after unlock), `Cmd_Connect` Stage 3 (discovers all locks), and the remote lock refresh in `Cmd_UpdateStatus` (detects LockedOther).

### LFS Locking

- **CheckOut** = `git lfs lock` via subprocess. Sets `Lock = Locked`, clears read-only flag.
- **Revert** = `git_checkout_head` to discard changes, then `git lfs unlock` to release the lock.
- **CheckIn** = stage + commit + unlock. Respects `KeepCheckedOut` checkbox.
- **Idempotent checkout**: "Lock exists" is treated as success (file already locked by us).
- **No `--force` on unlock**: Callers always discard/commit before unlocking, so the working tree is clean. Omitting `--force` avoids the "admin access" error that occurs when the LFS server identity (GitHub username) differs from `git config user.name`.
- **Remote lock polling**: Every background poll (30s), `Cmd_UpdateStatus` in full-scan mode queries the LFS server's `/locks/verify` endpoint to classify all locks as `Locked` (ours) or `LockedOther` (someone else). Uses the server-authoritative `ours`/`theirs` split rather than username comparison to avoid identity mismatches between `git config user.name` and the LFS server identity. Also detects released locks — files that were Locked/LockedOther in the cache but no longer appear in the response are reset to NotLocked. Default transport is the in-process `FGitLink_LfsHttpClient` (direct HTTPS, pooled keep-alive); set `r.GitLink.LfsHttp 0` to fall back to `git lfs locks --verify --json` per repo. The HTTP path also auto-falls-back per-repo if auth or endpoint resolution fails, so a single misconfigured remote can't poison the poll cycle.

### CheckIn (Submit)

`Cmd_CheckIn` handles both explicit file lists and changelist-scoped View Changes submits:

1. **Staging**: If `InFiles` is provided, converts to repo-relative and stages. If empty, gathers files from the `DestinationChangelist` (or all changelists as fallback).
2. **Cross-changelist isolation**: Before committing, temporarily unstages files from OTHER changelists so they don't leak into the commit (`git commit` always commits the entire index). Re-stages them after.
3. **Commit**: In-process via libgit2, or subprocess for repos with hooks.
4. **Unlock**: Releases LFS locks unless `KeepCheckedOut` is checked.
5. **State update**: Stamps committed files as Unmodified/NotLocked (or Locked if KeepCheckedOut).

### Submodule Support

At connect time, `git_submodule_foreach` enumerates submodule paths stored on the provider. State predicates are **submodule-agnostic** — submodule files are checkable-out, checkable-in, deletable, addable, etc. on the same basis as outer-repo files. Submodule special-casing happens at the command layer: `InFiles` is partitioned by submodule root via `PartitionByRepo` (in [Cmd_Shared.h](Source/GitLink/Private/Commands/Cmd_Shared.h)) and each bucket is processed against its own repo.

Two routing primitives:

1. **In-process libgit2** — `Provider::Open_SubmoduleRepositoryFor(InAbsolutePath)` returns a freshly-opened `gitlink::FRepository` for the containing submodule. Used by `Cmd_Delete`, `Cmd_Revert`, `Cmd_MarkForAdd`, `Cmd_CheckIn` for index/working-tree mutations. History and Diff use the same pattern.
2. **Subprocess git / git-lfs** — `FGitLink_Subprocess::Run(args, cwdOverride)` and `RunLfs(args, cwdOverride)` run with a per-call cwd. Empty override → parent's working directory. Submodule cwd → submodule's git config / remote / LFS endpoint are picked up implicitly. Used by `Cmd_CheckOut` (lock), `Cmd_CheckIn` (subprocess commit path), `Release_LfsLocksBestEffort` (unlock), and the Stage-3 lock discovery in `Cmd_Connect` / `Cmd_UpdateStatus`.

**State predicate contract for submodule files** ([GitLink_State.cpp](Source/GitLink/Private/GitLink_State.cpp)) — `bInSubmodule` is the only flag the predicates carry; all others mirror outer-repo behavior:

| Predicate | Submodule | Notes |
|---|---|---|
| `IsSourceControlled()` | `true` (forced) | Submodule files always show as tracked even if the parent's status walk doesn't see inside the submodule. Keeps History/Diff enabled. |
| `IsCurrent()` | `true` (optimistic) | We don't poll the submodule's remote tracking branch. Without this, UE's pre-delete check fires the misleading "not at latest" error. |
| `IsCheckedOut()` / `CanCheckout()` / `CanCheckIn()` / `CanDelete()` / `CanRevert()` / `CanAdd()` | same logic as outer-repo files | Driven purely by lock + tree state. Submodule routing happens in commands. |

**Lock state polling** — `Cmd_UpdateStatus` (full-scan path) and `Cmd_Connect` (Stage 3) iterate `Provider::Get_LfsSubmodulePaths()` (the LFS-using subset) and issue one `/locks/verify` per repo. Default transport is `FGitLink_LfsHttpClient::Request_LocksVerify` (in-process HTTPS); fallback is `FGitLink_Subprocess::QueryLfsLocks_Verified` per repo with the appropriate cwd. Results are merged into a single absolute-path-keyed map before applying so the existing emit-vs-cache update logic stays repo-agnostic. This is the only way to discover `LockedOther` for files inside a submodule — each submodule's LFS endpoint is separate.

**Submodule commit hooks** — `Cmd_CheckIn`'s subprocess-fallback-for-hooks path is **not** used for submodule commits. We commit submodules in-process via libgit2 unconditionally (no hook execution). The `bHasPreCommitOrCommitMsgHook` probe in `Cmd_Connect` only runs against the parent. If users have hooks in submodules they care about, they'd need to commit the submodule manually. Acceptable v2 limitation.

**Lockable extensions** — assumed to be the same across parent and all submodules. The parent's `_LockableExtensions` (probed via `git check-attr lockable` once at connect) is consulted for submodule files too. Most projects use a uniform `.gitattributes` lockable pattern set; if a submodule diverges, false-positive lockability would result in `git lfs lock` calls that the submodule's server rejects (handled gracefully — failure becomes a warning, not a hard error).

### Diff / Revision Get

`FGitLink_Revision::Get()` extracts file content at a commit via `git cat-file --filters <commit>:<path>`, using `RunToFile` which pipes binary stdout through `CreateProc` + `ReadPipeToArray` (binary-safe for .uasset). Supports submodule files by detecting the submodule root.

### View Changes / Changelists

- **UpdateChangelistsStatus** runs a fresh `Repository->Get_Status()` (not the stale cache) and assigns dirty Content/ files to Working or Staged changelists.
- **MoveToChangelist** stages (`git add`) or unstages (`git restore --staged`) files, then re-runs `UpdateChangelistsStatus` to refresh the View Changes window immediately.
- **Working/Staged changelists map 1:1 to git's staging area**.

## Key Files

| File | Purpose |
|------|---------|
| `GitLink_Provider.h/cpp` | ISourceControlProvider, submodule detection, GetState with normalization |
| `GitLink_CommandDispatcher.h/cpp` | FCommandResult, FCommandContext, command registration, sync/async dispatch, changelist threading |
| `GitLink_StateCache.h/cpp` | Thread-safe state storage with NormalizeKey |
| `GitLink_State.h/cpp` | FGitLink_FileState predicates (CanCheckout, IsModified, etc.) |
| `GitLink_Subprocess.h/cpp` | git.exe wrapper for LFS, check-attr, RunToFile, QueryLfsLocks_Verified (subprocess fallback) |
| `GitLink_LfsHttpClient.h/cpp` | In-process LFS `/locks/verify` client (direct HTTPS, replaces per-poll subprocess). CVar: `r.GitLink.LfsHttp` |
| `GitLink_Revision.h/cpp` | ISourceControlRevision with Get() for diff |
| `Cmd_Connect.cpp` | Repo open, status scan, LFS lock discovery (local + remote), submodule enumeration |
| `Cmd_CheckOut.cpp` | LFS lock + writable + idempotent |
| `Cmd_Revert.cpp` | Discard + unlock with modified/locked-only separation |
| `Cmd_CheckIn.cpp` | Stage + commit + unlock with changelist scoping, KeepCheckedOut, cross-CL isolation |
| `Cmd_UpdateStatus.cpp` | Status scan, history building (parent + submodule repos), lock state preservation, remote lock polling (LockedOther detection) |
| `Cmd_Changelists.cpp` | UpdateChangelistsStatus (fresh scan), MoveToChangelist (stage/unstage + refresh) |
| `Cmd_Shared.h` | Path helpers, Make_FileState, Release_LfsLocksBestEffort, status-mapping helpers |

## Common Pitfalls

1. **Path normalization** — Always normalize paths before cache lookups. The editor passes relative, absolute, and variously-cased paths. `NormalizeKey()` in the cache handles this, but command code should use `Normalize_AbsolutePath()` from `Cmd_Shared.h`.

2. **Repo-relative vs absolute paths** — libgit2 operations (`git_checkout_head`, `git_index_add_all`) need repo-relative paths. The editor passes absolute paths. Use `ToRepoRelativePath()` before calling into GitLinkCore. This was the root cause of multiple bugs (Revert no-op, CheckIn "nothing to commit", staging failures).

3. **LFS unlock — no `--force`** — Never use `--force` on `git lfs unlock`. It triggers an "admin access required" error when the GitHub username differs from `git config user.name`. Instead, ensure the working tree is clean before unlocking (callers must discard/commit first).

4. **Cmd_Connect re-init** — The editor fires `Init(force=true)` multiple times during startup. `Cmd_Connect` rebuilds the entire state cache each time. LFS lock discovery (Stage 3) must run during connect to avoid losing lock state.

5. **Submodule routing — predicates vs commands** — State predicates are submodule-agnostic; only `bInSubmodule` is carried through and `IsSourceControlled()`/`IsCurrent()` are forced true for submodule files (the latter because we don't poll their remotes). Everything else (locking, commits, deletes, adds) is decided by the same lock + tree state that drives outer-repo files, and the actual repo routing is done by commands via `PartitionByRepo` + `Provider::Open_SubmoduleRepositoryFor` (libgit2) or `FGitLink_Subprocess::Run/RunLfs(args, cwdOverride)` (subprocess). When adding a new command that mutates the working tree or talks to LFS, **always partition first** — never assume `InCtx.Repository` is the right handle for the input batch.

6. **git commit commits the entire index** — libgit2's `git_commit_create` always commits from the current index tree. When multiple changelists have files staged, submitting one changelist must temporarily unstage the others (see CheckIn cross-CL isolation above).

7. **Empty InFiles on View Changes submit** — The editor sends empty `InFiles` when submitting from View Changes. `Cmd_CheckIn` falls back to gathering files from the `DestinationChangelist` in the cache. Don't use `StageAll()` as it stages submodule directories.

## Build

Build as part of your project's editor target. Example:

```
"<EnginePath>/Engine/Build/BatchFiles/Build.bat" <ProjectName>Editor Win64 Development "<ProjectPath>/<ProjectName>.uproject" -waitmutex
```

Unity build uses project defaults (no explicit `bUseUnity` override in either `.Build.cs`).

## Remaining Work

### Polish / v2

- **View Changes auto-refresh on file modification** — Currently requires manual Refresh after modifying and saving files. Could be addressed by having the background poll (currently 30s) trigger `UpdateChangelistsStatus`, or by shortening the poll interval when the View Changes window is open.
- **Revision Control toolbar buttons** — 4 custom commands in the bottom-right SCC menu: Push pending local commits, Pull, Revert, Refresh. These are plugin-specific UI registrations (custom `SExtensionButton` entries in the module startup) — not part of the SCC command framework.
- **Named changelists** — `.git/gitlink/changelists.json` persistence via `GitLink_Changelists_Store` (currently stubs). `NewChangelist`, `DeleteChangelist`, `EditChangelist` all return Ok without doing anything.
- **`lockable_exts=0`** — `ProbeLockableExtensions` returns empty; falls back to hardcoded extensions. Works but should be investigated — likely a `.gitattributes` parsing issue or the LFS attribute query not finding `lockable` patterns.
- **Conflict resolution** — `Cmd_Resolve.cpp` stages conflicted files but doesn't offer resolve-by-strategy (ours/theirs/merge tool). Could be wired into the UE SCC resolve flow.
- **Stash support** — UE's SCC framework doesn't have a stash concept, but a custom toolbar button or command could expose git stash create/apply/drop.
- **Unit tests** — `GitLinkTests` module (planned but not created)

### Known quirks (acceptable for v1)

- **CheckIn stages only the selected changelist** — When submitting from a specific changelist, files from other changelists are temporarily unstaged before commit, then re-staged after. This is necessary because `git commit` (via libgit2) always commits the entire index. A subprocess-based `git commit -- <files>` would auto-scope, but we prefer in-process commits for speed.
- **"Working changes" as default submit description** — The submit dialog pre-fills the changelist name as the commit description. This is standard UE behavior for all SCC plugins with changelists — users overwrite it with their actual message.
- **Checkout dialog on save after Init(force=true)** — When opening certain assets, the editor fires Init(force=true) which rebuilds the state cache. If the lock state hasn't been applied yet, CanCheckout briefly returns true. Mitigated by carrying forward existing lock state in UpdateStatus/UpdateChangelistsStatus.
