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
- **Remote lock polling**: Every background poll (30s), `Cmd_UpdateStatus` in full-scan mode queries `git lfs locks` (remote) and `git lfs locks --local` to classify all locks as `Locked` (ours) or `LockedOther` (someone else). Uses path membership (local ∩ remote) rather than username comparison to avoid identity mismatches between git config user.name and the LFS server identity. Also detects released locks — files that were Locked/LockedOther in the cache but no longer appear in the remote set are reset to NotLocked.

### CheckIn (Submit)

`Cmd_CheckIn` handles both explicit file lists and changelist-scoped View Changes submits:

1. **Staging**: If `InFiles` is provided, converts to repo-relative and stages. If empty, gathers files from the `DestinationChangelist` (or all changelists as fallback).
2. **Cross-changelist isolation**: Before committing, temporarily unstages files from OTHER changelists so they don't leak into the commit (`git commit` always commits the entire index). Re-stages them after.
3. **Commit**: In-process via libgit2, or subprocess for repos with hooks.
4. **Unlock**: Releases LFS locks unless `KeepCheckedOut` is checked.
5. **State update**: Stamps committed files as Unmodified/NotLocked (or Locked if KeepCheckedOut).

### Submodule Support

At connect time, `git_submodule_foreach` enumerates submodule paths stored on the provider. `GetState()` stamps submodule files as `Unlockable` so all actions are greyed out. History queries open a temporary `FRepository` for the submodule's repo.

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
| `GitLink_Subprocess.h/cpp` | git.exe wrapper for LFS, check-attr, RunToFile, QueryLfsLocks_Local |
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

5. **Submodule file state timing (fixed)** — Root cause was in `Cmd_UpdateStatus`'s `BuildState` lambda: when a submodule file had no existing cache entry (`Lock==Unknown`), `Is_FileLockable()` returned `true` for `.uasset`, stamping `Lock=NotLocked`. Combined with `Tree=Unmodified` (submodule files get this in the explicit-file-list path), `CanCheckout()` returned `true` → checkout dialog. Fixed by adding `Is_InSubmodule()` check in `BuildState`'s else-branch so submodule files always get `Lock=Unlockable`. Defence-in-depth: `GetOrCreate_FileState` also stamps Unlockable at creation via `Set_SubmodulePaths()`; `Cmd_CheckOut` short-circuits for submodule paths (clears read-only, emits Unlockable, returns success) as a last-resort safety net.

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
