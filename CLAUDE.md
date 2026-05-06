# GitLink — Architecture & Development Guide

## Versioning

**Current version: v0.3.5** — single source of truth is [`GITLINK_VERSION`](Source/GitLink/Public/GitLink/GitLink_Version.h) (compile-time constexpr) plus `VersionName` in [`GitLink.uplugin`](GitLink.uplugin) (runtime descriptor). Both are logged on module startup; a mismatch between them logs a `Warning` and means you forgot to rebuild.

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
| v0.3.5 | **Hotfix: lock indicator stuck after Revert (and symmetric CheckOut flicker) — LFS read-replica race against the immediate poll.** After a successful `git lfs unlock` on Revert, the dispatcher's `Request_ImmediatePoll()` (`GitLink_CommandDispatcher.cpp:208-210`, fired for every working-tree-modifying command) ran `Cmd_UpdateStatus` within milliseconds. GitHub's LFS read replicas typically converge in under a second but can lag by several seconds, so `/locks/verify` would still report our lock as held — the full-scan classified it as `Locked` (ours) and re-stamped the cache, leaving the editor's "checked out" indicator stuck until the next sweep ~120s later. Symmetric race on CheckOut: the post-lock immediate-poll could see a stale "no lock" replica and clear the just-locked file back to `NotLocked` via the lock-released-detection path. The v0.3.2 hotfix only protected the *single-file probe* against re-stamping `Lock=Locked`; the full-scan path had no analogous guard. **Fix**: introduced `FGitLink_Provider::Note_LocalLockOp` / `Was_RecentLocalLockOp` — a small per-path TTL map (default 30s, opportunistically pruned past 64 entries) that records every locally-completed lock or unlock. `Cmd_CheckOut` (post-stamp) and `Release_LfsLocksBestEffort` (post-unlock-success, used by Revert/CheckIn/Delete) call `Note_LocalLockOp`; `Cmd_UpdateStatus`'s full-scan lock-merge consults `Was_RecentLocalLockOp` at both branches (the lock-stamping path and the lock-released-detection path) and skips any update for guarded paths. Lock state for guarded paths is locally-authoritative for the window — symmetric to the v0.3.2 single-file-probe guard but at the full-scan level. The 30s window comfortably exceeds observed replica-lag worst cases; outside the window the next successful sweep restores cache from the server as before. Map size is bounded by the user's recent batch ops (typically <10 entries) and the threshold prune. **Tests**: `Test_Provider_RecentLockOpGuard.cpp` (5 cases — added-then-present, unknown-path-absent, path normalization across case/slash variants, re-Note refreshes, prune-preserves-fresh). Time-based expiry intentionally not tested (`FPlatformTime::Seconds()` not injectable; would require a clock-fn refactor on `FGitLink_Provider`). |
| v0.3.4 | **Bugfix: revert left LFS-tracked files as 132-byte pointer text instead of binaries.** `Cmd_Revert` reverts via `IRepository::DiscardChanges` → `git_checkout_head` (libgit2, in-process). libgit2 does **not** invoke Git's clean/smudge filter drivers, and GitLink registers no custom filter, so reverting any LFS-tracked path (`.umap`, `.uasset`, etc.) wrote the index pointer text straight to disk. Unreal then failed to load the asset; if the asset was open, the editor evicted to the default level. Bug existed since v0.1.0 — invisible for code reverts (no filter to run), surfaced now because the user reverted an open `.umap`. **Fix**: added `RehydrateLfsForRepo` in `Cmd_Revert.cpp`'s anonymous namespace, called after each successful `DiscardForRepo` (outer-repo + per-submodule batches). Shells out to `git lfs checkout -- <repo-relative-paths>` via the existing `FGitLink_Subprocess::RunLfs(args, cwdOverride)` plumbing — same pattern `Cmd_CheckOut` uses for `git lfs lock`. Submodule routing via `cwdOverride = SubmoduleRoot` so each submodule's own LFS endpoint is picked up. Idempotent + no-op for non-LFS paths, and no network when the LFS object is in the local cache. Failures are logged as `Warning` and non-fatal (the discard already succeeded — the user's local changes are gone, which is what they asked for; missing rehydration leaves the same pointer-on-disk state we had before the fix and the user can recover with the manual `git lfs checkout` command). **Out of scope**: same bug shape exists in `PullFastForward` (`GitLink_Op_Remote_Network.cpp`) — flagged for follow-up. **Recovery for users hit before the fix**: `git lfs checkout <path>` against the local LFS cache (no remote pull needed if the object was previously fetched). |
| v0.3.3 | **Polish: cold-start identity race + per-submodule status-walk CPU spike.** Two issues observed in v0.3.2 production. **(1)** The asset-opened / focus delegates fire single-file probes within milliseconds of editor start, before the first `/locks/verify` sweep populates `_IdentityByHost`. v0.3.0's pessimistic-LockedOther fallback for unknown identity meant our own locked files briefly displayed as "LockedOther by [our GitHub display name]" until the sweep caught up — a confusing "locked by myself" UX. Fix: when identity isn't learned yet AND the probe found a lock, `Request_SingleFileLock` now returns `bSuccess=false` (treat as "no answer"); caller leaves the cache alone and the sweep populates the file shortly after. Probes that found no lock are still applied (LockedOther → NotLocked teammate-released remains responsive). **(2)** Sweep CPU spike: with 34 submodules in BB, the per-submodule `libgit2 Get_Status()` walks fanned out across `GMaxConcurrentRepoWorkers` (8) saturated all 8 physical cores during each 120 s sweep. The 8-worker cap is sized for the network-bound LFS HTTP poll (workers spend most of their time in `FEvent::Wait`, so 8 in flight hides per-request latency cheaply); status walks are CPU + disk bound, the opposite shape. Split: introduced `GMaxConcurrentStatusWalkers = 4` and applied it only to the per-submodule status walk in `Cmd_UpdateStatus`. LFS HTTP poll concurrency stays at 8. Halves the peak CPU during each sweep; doubled wall-clock cost is invisible since the sweep is background and well under the 120 s interval. |
| v0.3.2 | **Hotfix: lock flicker — single-file probe overrode local lock op.** v0.3.1 fixed the deadlock by routing the pre-checkout `UpdateStatus` path through the async `Request_LockRefreshForFile`, but that exposed a race: GitHub LFS read replicas aren't immediately consistent after a `git lfs lock`. The post-CheckOut `UpdateStatus` fired the probe ~50 ms after the lock subprocess returned; the probe hit a replica that hadn't seen the new lock yet and reported `NotLocked`. The completion lambda dutifully flipped the cache from `Lock=Locked` back to `NotLocked`, producing the "click Lock → brief flicker to locked → reverts to unlocked" UX. Symmetric race on Revert (stale `Locked` after a successful unlock). **Fix**: `Request_LockRefreshForFile`'s game-thread completion now refuses any transition involving `Lock=Locked` — it only services teammate-driven `NotLocked ↔ LockedOther` transitions plus `LockUser` updates inside `LockedOther`. `Lock=Locked` is locally-authoritative (set by `Cmd_CheckOut`, cleared by `Cmd_Revert`/`Cmd_CheckIn`); outside-our-control transitions involving Locked are caught by the full-sweep lock-released detection in `Cmd_UpdateStatus`, which compares the cache against the full `/locks/verify` response (not a single-path probe). Foreground freshness for "teammate just locked one of my files" still holds (NotLocked → LockedOther). |
| v0.3.1 | **Hotfix: pre-checkout HTTP probe deadlocked the game thread.** v0.3.0's `Cmd_UpdateStatus` explicit-file-list branch issued synchronous `Request_SingleFileLock` calls inside a `ParallelFor_BoundedConcurrency`, blocking the calling thread for up to 12 s per probe. UE dispatches pre-checkout `UpdateStatus` synchronously on the **game thread**, but UE's HTTP module needs the game thread to tick to fire completion delegates — so every probe ran out the full timeout. With ~80 lockable files in a typical content-browser refresh at 8-worker parallelism, that pinned the editor for ~2 minutes at ~85% CPU on the spinning workers and made lock / unlock / view-history feel hung. Fix: replaced the synchronous probe loop with a fire-and-forget dispatch via `Provider::Request_LockRefreshForFile` (the same path the editor delegates use — background thread, debounced per-path at 2 s, cache mutation marshalled back to the game thread). The game thread is no longer blocked. Foreground freshness is preserved through the asset-opened / package-dirty / focus delegates that already probed the file shortly before the editor's pre-checkout `UpdateStatus`; the 2 s debounce collapses our redispatch into the in-flight probe, and the async cache update fires `OnSourceControlStateChanged` so the editor sees the fresh state on the next re-query. |
| v0.3.0 | **Stage B — hybrid LFS lock-state freshness.** Stage A (v0.1+) made each `/locks/verify` call cheap via `FGitLink_LfsHttpClient`; Stage B uses that on demand to fix the two remaining UX gaps — staleness on focus return (a teammate's lock took ≤30s to appear) and idle-editor cost (every 30s sweeping repos that no one is looking at). **Sweep cadence**: `PollIntervalSeconds` default 30 → 120, `GFullSweepThrottleSec` 20 → 110. The sweep is no longer the latency floor for foreground freshness. **On-demand single-file probes**: new `FGitLink_LfsHttpClient::Request_SingleFileLock` issues `GET <lfs-url>/locks?path=<rel>`; reuses the auth cache, 401 retry, 429 backoff, and rate-limit telemetry of the verify path. Response shape is unsplit (`locks` array with `owner.name` only), so the client lazily learns the LFS-server identity per host from the `ours` array of any successful `/locks/verify` page (`_IdentityByHost`) and uses that to classify Locked vs LockedOther. Identity not yet learned → pessimistic LockedOther (converges to correct after the next sweep). Pure helpers `FGitLink_Subprocess::Parse_LocksByPathJson` and `gitlink::lfs_http::detail::Extract_IdentityFromVerifyPage` pinned by tests. **Editor signal wiring** (`FGitLink_Provider`): three new delegates — `FSlateApplication::OnApplicationActivationStateChanged` (refresh up to 16 open assets on focus return), `UAssetEditorSubsystem::OnAssetOpenedInEditor` (head-start probe on asset open), `UPackage::PackageMarkedDirtyEvent` (defensive probe on first edit). All route through `Provider::Request_LockRefreshForFile` which drops out fast for non-lockable / outside-repo / untracked-submodule files (preserves the v0.2.0 fix), debounces per-path at 2s, runs the HTTP probe on a background thread, and marshals the cache write + `OnSourceControlStateChanged` back to the game thread. **Pre-checkout safety**: `Cmd_UpdateStatus`'s explicit-file-list branch issues per-file probes synchronously (bounded-parallel at 8 workers) for any lockable file with a resolved LFS endpoint; results override the cache's carry-forward lock state via post-`BuildState` stamping, closing the "lock is stale → click CheckOut → server rejects" hole. Tests: `Test_Subprocess_LocksByPathJson.cpp` (6 cases — happy path, empty array, multi-result first-wins, missing owner, backslash normalization, malformed) and `Test_LfsHttpClient_Identity.cpp` (4 cases — happy path, no-ours, failed-page, ours-with-empty-owner skip). Subprocess fallback intact (CVar `r.GitLink.LfsHttp 0` still works for the sweep). |
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
- **Remote lock polling — hybrid model (Stage B, v0.3.0)**. Two paths, both via `FGitLink_LfsHttpClient`:
  1. **Background sweep — `Cmd_UpdateStatus` full-scan**. Default cadence 120s (`PollIntervalSeconds`), throttle 110s (`GFullSweepThrottleSec`). Per-repo `/locks/verify` POST classifies all locks as `Locked` (ours) / `LockedOther` (theirs). Uses the server-authoritative `ours`/`theirs` split rather than username comparison to avoid identity mismatches between `git config user.name` and the LFS server identity. Lock-released detection (files in cache as Locked/LockedOther that no longer appear in any successful response) resets to NotLocked, gated by `SuccessfullyPolledRepos` so a failed poll never tears down state for files in other repos. **Side-effect**: the first successful page that contains an `ours` entry teaches `_IdentityByHost` what our LFS-server identity is on that host; the single-file path needs this to classify ours vs theirs because `GET /locks?path=...` doesn't pre-classify.
  2. **On-demand single-file probes — `FGitLink_LfsHttpClient::Request_SingleFileLock`**. `GET <lfs-url>/locks?path=<rel>`; reuses the auth cache, 401 retry, 429 backoff, and rate-limit telemetry of the verify path. Three editor delegates fire it via `Provider::Request_LockRefreshForFile`: focus return (`FSlateApplication::OnApplicationActivationStateChanged` — up to 16 open assets per activation), asset opened (`UAssetEditorSubsystem::OnAssetOpenedInEditor`), package dirtied (`UPackage::PackageMarkedDirtyEvent`). Per-path debounce at 2s collapses bursts (Alt-Tab opening 50 tabs → at most one probe per path in the window). Probe runs on a background thread; cache mutation + `OnSourceControlStateChanged` marshal to the game thread. Drops out fast for non-lockable files, files outside the repo, or untracked-in-submodule files (preserves the v0.2.0 "no auto-lock-on-create" fix). Pre-checkout `UpdateStatus` (explicit-file-list branch) also issues synchronous probes (bounded-parallel at 8 workers) for any lockable requested file — closes the "stale lock state → CheckOut click → server rejects" UX hole.
  Default transport is the in-process `FGitLink_LfsHttpClient` (direct HTTPS, pooled keep-alive); set `r.GitLink.LfsHttp 0` to fall back to `git lfs locks --verify --json` per repo for the sweep (single-file probes simply skip in that mode and the next sweep catches up). The HTTP path also auto-falls-back per-repo if auth or endpoint resolution fails, so a single misconfigured remote can't poison the poll cycle. On a `429 Too Many Requests`, the client records a per-host backoff deadline (parsed from `Retry-After`, clamped to ≤1 hour) and short-circuits subsequent calls to that host into the subprocess fallback until the deadline passes. Reads `X-RateLimit-Remaining` and emits a one-shot `Warning` log if the budget drops below 500 — at GitHub's 3000/min LFS quota this should virtually never fire; if it does, it's a real signal (too many editors / misconfiguration).

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
| `GitLink_LfsHttpClient.h/cpp` | In-process LFS HTTPS client. `Request_LocksVerify` (POST, sweep) + `Request_SingleFileLock` (GET, on-demand foreground freshness). Per-host auth cache, 401 retry, 429 backoff, rate-limit telemetry, lazily-learned `_IdentityByHost`. CVar: `r.GitLink.LfsHttp` |
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
