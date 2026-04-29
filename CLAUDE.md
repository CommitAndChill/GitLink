# GitLink — Architecture & Development Guide

## Versioning

**Current version: v0.2.0** — single source of truth is [`GITLINK_VERSION`](Source/GitLink/Public/GitLink/GitLink_Version.h) (compile-time constexpr) plus `VersionName` in [`GitLink.uplugin`](GitLink.uplugin) (runtime descriptor). Both are logged on module startup; a mismatch between them logs a `Warning` and means you forgot to rebuild.

**Rule: bump the version with every change session and record an entry in the log below.**

Bump checklist when ending a session that touches `Source/`:

1. Edit `GITLINK_VERSION` in `Source/GitLink/Public/GitLink/GitLink_Version.h`.
2. Edit `VersionName` (and increment the numeric `Version` if it's a behavior change) in `GitLink.uplugin`.
3. Add a row at the top of the **Version log** table below describing the change. Keep entries terse but specific — the goal is that someone scanning the table can identify when a particular regression was introduced or fixed.
4. Rebuild the plugin so the binary's `__DATE__`/`__TIME__` snapshot in `GITLINK_BUILD_TIMESTAMP` updates.

Versions follow semver-ish: bump MINOR for behavior changes (new commands, fixed bugs, predicate changes), PATCH for trivial bugfixes / docs / tests, MAJOR when the public C++ API breaks (`FGitLink_Provider`, `FGitLink_FileState`, command-result shapes).

### Version log

| Version | Summary |
|---|---|
| v0.2.0 | **Submodule LFS lock fixes (3 bugs).** ① **Locks invisible after editor restart**: dropped the `<sub>/.gitattributes` peek in `CheckRepositoryStatus` that was filtering submodules out of the lock-poll set whenever their lockable patterns lived in nested `.gitattributes` files. `_LfsSubmodulePaths = _SubmodulePaths` — endpoint resolution + per-repo URL cache + 429 backoff already bound the per-poll cost. ② **Unlock failures silently swallowed**: `Release_LfsLocksBestEffort` now returns `FLfsUnlockOutcome { Released, FailedPaths, ErrorMessages }`. `Cmd_Revert`/`Cmd_CheckIn`/`Cmd_Delete` consume the split — files in `FailedPaths` keep their cached `Lock=Locked` so the editor still shows them as checked out (was flipping to NotLocked while the server lock persisted, leaving files permanently stuck — typical cause is LFS server identity vs `git config user.name` mismatch). Failures bubble up as `Result.ErrorMessages` so users see a toast instead of a phantom "everything is fine". ③ **Auto-CheckOut prompt on save in submodules** (the headline regression): root cause was `IsSourceControlled()` unconditionally forcing `true` for any submodule file (`bInSubmodule` short-circuit) — that was a workaround for the parent's libgit2 status walk not seeing inside submodules, but it made every untracked submodule file look source-controlled, driving UE's auto-CheckOut-on-save flow to LFS-lock files that weren't even in git yet. Removed the force-true; added `Provider::Is_TrackedInSubmodule` backed by `_SubmoduleTrackedSets` (per-submodule index walked once at connect via `gitlink::op::Enumerate_TrackedFiles`); `Provider::GetState` and `Cmd_UpdateStatus`'s explicit-file-list branch consult it to stamp Tree=Unmodified for tracked submodule files vs. NotInRepo for untracked ones. Also added `File=Added` to `CanCheckout`'s blocked set — locks are only meaningful once the file exists in HEAD, so staged-but-not-committed files stay non-lockable end-to-end (defense-in-depth + matches the user-stated rule "should remain untracked or added until committed"). `Cmd_Delete` lock-release policy split: only release on `File=Added` (stale lock cleanup); for tracked deletes, preserve the lock and let `Cmd_CheckIn` release it when the deletion is committed. Test coverage in `Test_FileState_Predicates.cpp`: rewrote `IsSourceControlled.FollowsTreeState.BothRepos`, strengthened `SubmoduleUntracked.NotSourceControlledNotCheckoutable`, added `FileAdded.NotCheckoutable.BothRepos`. |
| v0.1.0 | Initial release. libgit2-backed SCC provider, LFS locking, Working/Staged changelists, CheckOut/CheckIn/Revert/MarkForAdd/Delete/Sync/Fetch, history, submodule handling, remote lock polling, `lockable_exts` probe hardening (ROADMAP T1). |

---

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

**Lock state preservation**: `UpdateStatus` and `UpdateChangelistsStatus` carry forward existing lock state from the cache instead of blindly stamping `NotLocked`. Lock state is owned exclusively by `Cmd_CheckOut` (sets Locked), `Cmd_Revert` (sets NotLocked after a *successful* unlock — failed unlocks preserve the cached Locked state via the `FLfsUnlockOutcome` split), `Cmd_CheckIn` (same pattern), `Cmd_Delete` (only releases the lock for `File=Added` files; tracked deletions keep the lock until committed), `Cmd_Connect` Stage 3 (discovers all locks), and the remote lock refresh in `Cmd_UpdateStatus` (detects LockedOther + released locks).

### LFS Locking

- **CheckOut** = `git lfs lock` via subprocess. Sets `Lock = Locked`, clears read-only flag. **Blocked** by `CanCheckout` for `File=Added` (uncommitted) files — locks are only meaningful once the file exists in HEAD.
- **Revert** = `git_checkout_head` to discard changes, then `git lfs unlock` to release the lock. Failed unlocks (e.g., LFS server identity mismatch) keep the cached `Lock=Locked` so the editor still shows "checked out" and the user can retry; the failure surfaces as a non-fatal `Result.ErrorMessages` entry.
- **CheckIn** = stage + commit + unlock. Respects `KeepCheckedOut` checkbox. Failed unlocks handled the same way as Revert.
- **Delete** = `git rm` + (sometimes) unlock. The unlock only fires for `File=Added` (never-committed) files — tracked deletions keep the lock until `Cmd_CheckIn` commits the deletion.
- **Idempotent checkout**: "Lock exists" is treated as success (file already locked by us).
- **No `--force` on unlock**: Callers always discard/commit before unlocking, so the working tree is clean. Omitting `--force` avoids the "admin access" error that occurs when the LFS server identity (GitHub username) differs from `git config user.name`.
- **Remote lock polling**: Every background poll (30s), `Cmd_UpdateStatus` in full-scan mode queries the LFS server's `/locks/verify` endpoint to classify all locks as `Locked` (ours) or `LockedOther` (someone else). Uses the server-authoritative `ours`/`theirs` split rather than username comparison to avoid identity mismatches between `git config user.name` and the LFS server identity. Also detects released locks — files that were Locked/LockedOther in the cache but no longer appear in the response are reset to NotLocked. Default transport is the in-process `FGitLink_LfsHttpClient` (direct HTTPS, pooled keep-alive); set `r.GitLink.LfsHttp 0` to fall back to `git lfs locks --verify --json` per repo. The HTTP path also auto-falls-back per-repo if auth or endpoint resolution fails, so a single misconfigured remote can't poison the poll cycle. On a `429 Too Many Requests`, the client records a per-host backoff deadline (parsed from `Retry-After`, clamped to ≤1 hour) and short-circuits subsequent calls to that host into the subprocess fallback until the deadline passes. Reads `X-RateLimit-Remaining` and emits a one-shot `Warning` log if the budget drops below 500 — at GitHub's 3000/min LFS quota this should virtually never fire; if it does, it's a real signal (too many editors / misconfiguration).

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

**State predicate contract for submodule files** ([GitLink_State.cpp](Source/GitLink/Private/GitLink_State.cpp)) — `bInSubmodule` is carried for command-layer routing (`PartitionByRepo`), but the **predicates themselves are uniform** across outer-repo and submodule files. Correct Tree state for submodule files is supplied at populate time via `Provider::Is_TrackedInSubmodule` (per-submodule index built at connect — see `_SubmoduleTrackedSets` in `GitLink_Provider`), so the predicates don't need a special-case branch.

| Predicate | Submodule | Notes |
|---|---|---|
| `IsSourceControlled()` | same logic as outer-repo files | Tree-based: `true` for tracked files (Tree=Unmodified/Working/Staged), `false` for Untracked/NotInRepo/Ignored. **History note:** History/Diff stay enabled for tracked submodule files because their Tree is `Unmodified` (stamped by `Provider::GetState` from the per-submodule tracked-set lookup). Untracked submodule files correctly fail this predicate, which is what stops UE's auto-CheckOut-on-save flow from prompting to LFS-lock newly-created files. |
| `IsCurrent()` | `true` (optimistic) | We don't poll the submodule's remote tracking branch. Without this, UE's pre-delete check fires the misleading "not at latest" error. |
| `IsCheckedOut()` / `CanCheckout()` / `CanCheckIn()` / `CanDelete()` / `CanRevert()` / `CanAdd()` | same logic as outer-repo files | Driven purely by lock + tree + file state. Submodule routing happens in commands. **CanCheckout note:** also blocks `File=Added` (staged-but-not-committed). Locks are only meaningful once the file exists in HEAD; before commit, the lock is just noise and the editor's auto-checkout flow would otherwise prompt to lock every freshly-marked-for-add file. |

**Per-submodule tracked-files index** — `FGitLink_Provider::_SubmoduleTrackedSets` (`TMap<FString /*submodule root*/, TSet<FString /*lower-cased repo-relative path*/>>`) is populated once at connect by walking each submodule's git index via `gitlink::op::Enumerate_TrackedFiles`. Consumed by `Provider::Is_TrackedInSubmodule`, which:

- `Provider::GetState` calls when stamping the optimistic default Tree state for submodule files. Tracked → `Unmodified`. Untracked → leave as `NotInRepo` (so `CanCheckout` blocks).
- `Cmd_UpdateStatus` calls in the explicit-file-list path (the "editor saved this file, refresh its state" code path) so newly-saved untracked files in submodules don't get stamped `Unmodified` and trigger the auto-CheckOut flow.

Stale-set caveat: the set is only refreshed on full reconnect. A file marked-for-add since the last connect won't appear in the set, but the next `UpdateStatus` full sweep runs `Get_Status` against each submodule and stamps the real Tree state regardless.

**Lock state polling** — `Cmd_UpdateStatus` (full-scan path) and `Cmd_Connect` (Stage 3) iterate `Provider::Get_LfsSubmodulePaths()` and issue one `/locks/verify` per repo. **As of the submodule-lock fix this list is the entire `_SubmodulePaths` set** — we no longer try to filter LFS-using submodules by peeking at `<sub>/.gitattributes` because that probe missed submodules with nested `.gitattributes` files (and a missed submodule meant its locks never appeared in the editor across restarts). Endpoint resolution silently no-ops for non-LFS submodules, the per-repo URL cache means resolution only happens once, and `429`/auth failures fall back to subprocess per-repo, so the runtime cost of polling all submodules is bounded. Default transport is `FGitLink_LfsHttpClient::Request_LocksVerify` (in-process HTTPS); fallback is `FGitLink_Subprocess::QueryLfsLocks_Verified` per repo with the appropriate cwd. Results are merged into a single absolute-path-keyed map before applying so the existing emit-vs-cache update logic stays repo-agnostic. This is the only way to discover `LockedOther` for files inside a submodule — each submodule's LFS endpoint is separate.

**Unlock failure surfacing** — `Release_LfsLocksBestEffort` returns `FLfsUnlockOutcome { Released, FailedPaths, ErrorMessages }` rather than a bool. Callers (`Cmd_Revert`, `Cmd_CheckIn`, `Cmd_Delete`) consume the split: files in `Released` get their cache `Lock` stamped to `NotLocked`/`Unlockable`; files in `FailedPaths` keep their cached `Lock=Locked` so the editor still shows them as checked out and the user can retry. Without this split, a silent unlock failure (most commonly: LFS server's lock owner identity differs from local `git config user.name`, so `git lfs unlock` is rejected) would have flipped the cache to `NotLocked` while the server lock persisted — leaving the file permanently stuck with no UI affordance to retry. Failures also bubble up as a non-fatal `Result.ErrorMessages` entry so users see "lock release failed" toasts instead of a phantom "everything is fine".

**Cmd_Delete lock-release policy** — Deletion only releases the LFS lock when the file's pre-delete state is `File=Added` (i.e. staged-as-added but never committed; the lock is stale because nothing in HEAD ever existed for others to conflict with). For tracked files (in HEAD), the lock is **preserved** through the delete — `Cmd_CheckIn` releases it when the deletion is committed. This matches the user-facing semantic: a staged-but-uncommitted deletion can still be reverted from HEAD, so anyone could pick the file back up; the lock has to remain held until the deletion is committed.

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

5. **Submodule routing — predicates vs commands** — State predicates are submodule-agnostic; only `bInSubmodule` is carried through, and only `IsCurrent()` is forced true for submodule files (because we don't poll their remotes). `IsSourceControlled()` used to also be forced true for submodule files but isn't anymore — that workaround caused new untracked files in submodules to be reported as source-controlled, which made UE's auto-CheckOut-on-save flow prompt the user to LFS-lock files that weren't even in git yet. The current contract: tracked submodule files satisfy `IsSourceControlled` because `Provider::GetState` stamps Tree=Unmodified on them via `Is_TrackedInSubmodule` (per-submodule index built at connect); untracked submodule files correctly fail it. Everything else (locking, commits, deletes, adds) is decided by the same lock + tree + file state that drives outer-repo files, and the actual repo routing is done by commands via `PartitionByRepo` + `Provider::Open_SubmoduleRepositoryFor` (libgit2) or `FGitLink_Subprocess::Run/RunLfs(args, cwdOverride)` (subprocess). When adding a new command that mutates the working tree or talks to LFS, **always partition first** — never assume `InCtx.Repository` is the right handle for the input batch.

6. **git commit commits the entire index** — libgit2's `git_commit_create` always commits from the current index tree. When multiple changelists have files staged, submitting one changelist must temporarily unstage the others (see CheckIn cross-CL isolation above).

7. **Empty InFiles on View Changes submit** — The editor sends empty `InFiles` when submitting from View Changes. `Cmd_CheckIn` falls back to gathering files from the `DestinationChangelist` in the cache. Don't use `StageAll()` as it stages submodule directories.

8. **`File=Added` is not lockable** — `CanCheckout` blocks files whose `File=Added` (staged-as-added, never committed). LFS locks are only meaningful once the file exists in HEAD, so the user-facing right-click "Check Out" stays grayed out until the file is committed. Combined with `IsSourceControlled` returning false for `Tree=Untracked`, this means the full new-file lifecycle is: untracked → MarkForAdd (Added/Staged, still not lockable) → CheckIn (Unmodified, now lockable). When adding a new predicate or modifying `CanCheckout`, preserve this gate.

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
