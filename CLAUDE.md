# GitLink — Architecture & Development Guide

## Overview

GitLink is a clean-room Unreal source control plugin using libgit2 in-process, with subprocess fallback for LFS and hooks. Two modules: `GitLinkCore` (pure git facade) and `GitLink` (Unreal SCC integration).

## Code Style

- Trailing return types (`-> Type`)
- `Get_` / `Set_` / `Is_` / `Request_` method prefixes
- Early-return inverted-if style
- `auto` for local variables
- No `Ck` or `ck` prefix/namespace anywhere — brand-neutral
- `namespace gitlink` for core types, `namespace gitlink::cmd` for commands, `namespace gitlink::op` for core operations

## Architecture

### Command Dispatch

All Unreal SCC operations route through `FGitLink_CommandDispatcher`:

```
Editor calls Provider.Execute(FCheckOut, files)
  -> Dispatcher.Dispatch("CheckOut", files, concurrency)
    -> gitlink::cmd::CheckOut(Context, Operation, Files)  // free function in Cmd_CheckOut.cpp
    -> FCommandResult { UpdatedStates, UpdatedChangelistStates, bOk }
    -> Dispatcher applies states to cache on game thread
    -> Broadcasts OnSourceControlStateChanged
```

Each command is a **single free function** in `namespace gitlink::cmd`, registered in `GitLink_CommandDispatcher.cpp`. No worker class hierarchy.

### State Cache

`FGitLink_StateCache` stores `TMap<FString, FGitLink_FileStateRef>` with **normalized path keys** (via `NormalizeKey()` — `ConvertRelativePathToFull` + `NormalizeFilename`). This is critical: the editor queries paths in various forms, and all must resolve to the same cache entry.

State predicates (`CanCheckout`, `CanRevert`, `IsCheckedOut`, etc.) are on `FGitLink_FileState` and drive every right-click menu option and content browser icon.

### LFS Locking

- **CheckOut** = `git lfs lock` via subprocess. Sets `Lock = Locked`, clears read-only flag.
- **Revert** = `git_checkout_head` to discard changes, then `git lfs unlock` to release the lock.
- **Idempotent checkout**: "Lock exists" is treated as success (file already locked by us).
- **No `--force` on unlock**: Callers always discard/commit before unlocking, so the working tree is clean. Omitting `--force` avoids the "admin access" error that occurs when the LFS server identity (GitHub username) differs from `git config user.name`.

### Submodule Support

At connect time, `git_submodule_foreach` enumerates submodule paths stored on the provider. `GetState()` stamps submodule files as `Unlockable` so all actions are greyed out. History queries open a temporary `FRepository` for the submodule's repo.

### Diff / Revision Get

`FGitLink_Revision::Get()` extracts file content at a commit via `git cat-file --filters <commit>:<path>`, using `RunToFile` which pipes binary stdout through `CreateProc` + `ReadPipeToArray` (binary-safe for .uasset). Supports submodule files by detecting the submodule root.

### View Changes / Changelists

`Cmd_UpdateChangelistsStatus` scans the file state cache and assigns dirty files to Working or Staged changelists. Only files under `Content/` directories appear (matching GitSourceControl behavior). `FCommandResult::UpdatedChangelistStates` carries changelist deltas through the dispatcher.

## Key Files

| File | Purpose |
|------|---------|
| `GitLink_Provider.h/cpp` | ISourceControlProvider, submodule detection, GetState with normalization |
| `GitLink_CommandDispatcher.h/cpp` | FCommandResult, command registration, sync/async dispatch |
| `GitLink_StateCache.h/cpp` | Thread-safe state storage with NormalizeKey |
| `GitLink_State.h/cpp` | FGitLink_FileState predicates (CanCheckout, IsModified, etc.) |
| `GitLink_Subprocess.h/cpp` | git.exe wrapper for LFS, check-attr, RunToFile |
| `GitLink_Revision.h/cpp` | ISourceControlRevision with Get() for diff |
| `Cmd_Connect.cpp` | Repo open, status scan, LFS lock discovery, submodule enumeration |
| `Cmd_CheckOut.cpp` | LFS lock + writable + idempotent |
| `Cmd_Revert.cpp` | Discard + unlock with modified/locked-only separation |
| `Cmd_UpdateStatus.cpp` | Status scan, history building (parent + submodule repos) |
| `Cmd_Changelists.cpp` | UpdateChangelistsStatus with Content/ filter |
| `Cmd_Shared.h` | Path helpers, Make_FileState, Release_LfsLocksBestEffort |

## Common Pitfalls

1. **Path normalization** — Always normalize paths before cache lookups. The editor passes relative, absolute, and variously-cased paths. `NormalizeKey()` in the cache handles this, but command code should use `Normalize_AbsolutePath()` from `Cmd_Shared.h`.

2. **Repo-relative vs absolute paths** — libgit2 operations (`git_checkout_head`, `git_reset_default`) need repo-relative paths. The editor passes absolute paths. Use `ToRepoRelativePath()` before calling into GitLinkCore.

3. **LFS unlock — no `--force`** — Never use `--force` on `git lfs unlock`. It triggers an "admin access required" error when the GitHub username differs from `git config user.name`. Instead, ensure the working tree is clean before unlocking (callers must discard/commit first).

4. **Cmd_Connect re-init** — The editor fires `Init(force=true)` multiple times during startup. `Cmd_Connect` rebuilds the entire state cache each time. LFS lock discovery must run during connect to avoid losing lock state.

5. **Submodule file state timing** — `GetState()` always stamps submodule files as Unlockable (no Lock==Unknown guard) because GetState may be called before `CheckRepositoryStatus()` populates `_SubmodulePaths`.

## Build Command

```
"D:/Repos/UnrealEngine-Angelscript/Engine/Build/BatchFiles/Build.bat" CkPluginsEditor Win64 Development "D:/Repos/CkPlugins2/CkPlugins.uproject" -waitmutex
```

## Remaining Work

### Functional (v1 complete — these are polish/v2)

- **View Changes auto-refresh on file modification** — Currently requires manual Refresh after modifying and saving files. The existing GitSourceControl plugin refreshes within 1–2 seconds. Could be addressed by having the background poll (currently 30s) trigger `UpdateChangelistsStatus`, or by shortening the poll interval when the View Changes window is open.
- **Revision Control toolbar buttons** — The existing plugin adds 4 custom commands to the bottom-right SCC menu: Push pending local commits, Pull, Revert, Refresh. These are plugin-specific UI registrations (custom `SExtensionButton` entries in the module startup) — not part of the SCC command framework.
- **Named changelists** — `.git/gitlink/changelists.json` persistence via `GitLink_Changelists_Store` (currently stubs). `NewChangelist`, `DeleteChangelist`, `EditChangelist` all return Ok without doing anything.
- **`lockable_exts=0`** — `ProbeLockableExtensions` returns empty; falls back to hardcoded extensions. Works but should be investigated — likely a `.gitattributes` parsing issue or the LFS attribute query not finding `lockable` patterns.
- **Unit tests** — `GitLinkTests` module (planned but not created)

### Known quirks (acceptable for v1)

- **CheckIn stages only the selected changelist** — When submitting from a specific changelist, files from other changelists are temporarily unstaged before commit, then re-staged after. This is necessary because `git commit` (via libgit2) always commits the entire index. The existing plugin avoids this by using `git commit -- <files>` via subprocess, which auto-scopes to those files.
- **"Working changes" as default submit description** — The submit dialog pre-fills the changelist name as the commit description. This is standard UE behavior for all SCC plugins with changelists — users overwrite it with their actual message.
- **Checkout dialog on save after Init(force=true)** — When opening certain assets, the editor fires Init(force=true) which rebuilds the state cache. If the lock state hasn't been applied yet, CanCheckout briefly returns true. Mitigated by carrying forward existing lock state in UpdateStatus/UpdateChangelistsStatus.
