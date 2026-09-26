# GitLink Roadmap

Known gaps and planned work. Each task is self-contained. Branch off `main`, and merge through a pull request.

## Current status (v0.7.0)

**Shipped:** libgit2-backed provider; Git LFS locking with in-process lock polling; Working/Staged changelists;
Check Out / Check In / Revert / Mark for Add / Delete / Pull (fast-forward) / Fetch; history and diff; submodule
routing for status, staging, commits, locks, history and diff; toolbar Push / Pull / Revert All / Refresh; hook
detection (including `core.hooksPath`); private-remote authentication through the Git credential helper;
automated tests including a local-remote suite and opt-in live tests.

## Open work, roughly by priority

| # | Gap | Notes |
|---|---|---|
| A | **SSH remotes for Pull / Fetch** | libgit2 is built without SSH, so Pull/Fetch need HTTPS. Either build libgit2 with libssh2, or run fetch through the Git executable for non-HTTPS remotes. Push already uses Git. |
| B | **Pull / Push for submodules** | Sync and Push act on the parent repository only. Needs per-submodule fetch + fast-forward and a push of submodules before the parent pointer. |
| C | **Conflict resolution** — Task 3 | Pull refuses non-fast-forward; Resolve only stages what is on disk. |
| D | **"Not at latest revision"** | `EGitLink_RemoteState::NotAtHead` is never computed, so the editor's "someone changed this upstream" warning never fires. The background fetch already updates remote-tracking refs; compare HEAD vs upstream per file. |
| E | **Named changelists** — Task 2 | Currently reported as unsupported. |
| F | **Timeouts on LFS subprocesses** | `FGitLink_Subprocess::Run` (git lfs lock / unlock / pull) has no timeout; git-lfs's own network timeouts bound it in practice, but a credential helper waiting on UI would not be. Route through `Run_Bounded`. |
| G | **Mac / Linux** — Task 6 | Win64-only libgit2 today. |
| H | **Copy and Annotate** | Copy (duplicate with history) is a no-op; Annotate (blame) is a stub. |
| I | Stash (Task 4), dynamic poll interval (Task 8) | Nice to have. |

---

## Task 1 — Investigate `lockable_exts=0` ✅ DONE

**Outcome:** No confirmed bug — the original parser worked correctly against git's actual output format (`*.uasset: lockable: set`) on Windows. Verified on 2026-04-17: after adding a `.gitattributes` with lockable entries to a project root, connect log reported `lockable_exts=2`. Previous `lockable_exts=0` sightings were real — the project simply had no `.gitattributes` with lockable patterns at the repo root, so the hardcoded fallback was doing its job.

**Changes made (defensive hardening):**
- `ProbeLockableExtensions` now trims each line before parsing (tolerates stray CR / trailing whitespace — hypothesis 2 from the original investigation).
- Splits on `": "` and inspects the trailing value rather than `EndsWith(": set")`, so a `set ` with trailing whitespace wouldn't be silently dropped.
- Added Verbose logging of the raw `git check-attr` stdout, plus a Verbose diagnostic when the probe returns empty. Future `lockable_exts=0` reports can be root-caused from logs.

**Files:** `Source/GitLink/Private/GitLink_Subprocess.cpp` (ProbeLockableExtensions, lines ~130-210).

**Follow-up (not blocking, host-project-specific):** If the host project has no root `.gitattributes`, the plugin falls back to hardcoded extensions. Committing a real `.gitattributes` (with an `[attr]lock` macro + `*.uasset lock` / `*.umap lock`, or via `git lfs track --lockable "*.uasset"`) makes the probe authoritative and enables the read-only-on-checkout behavior that drives UE's "must take lock before editing" flow.

---

## Task 2 — Named Changelists (persistence)

**Priority:** Low. UE's changelist UI is functional with just Working/Staged; named changelists are a power-user feature.

**Problem:** `Cmd_Changelists.cpp` has stubs for `NewChangelist`, `DeleteChangelist`, `EditChangelist`, and `MoveToChangelist`'s named-changelist path. They log and return Ok without doing anything.

**Design:**

```
┌────────────────────────────────────────┐
│        FGitLink_Changelists_Store      │ (NEW)
│────────────────────────────────────────│
│ Load/Save to .git/gitlink/changelists  │
│ Map<Name, TArray<FString> paths>       │
│ Get/Set/Rename/Delete                  │
└──────────────┬─────────────────────────┘
               │
               ▼
┌────────────────────────────────────────┐
│   Cmd_Changelists (existing)           │
│────────────────────────────────────────│
│ NewChangelist    → Store.Create        │
│ DeleteChangelist → Store.Delete        │
│ EditChangelist   → Store.Rename        │
│ MoveToChangelist → Store.MovePath      │
│                     + Stage/Unstage    │
│                       if dest==Staged  │
└────────────────────────────────────────┘
```

**File format** (`.git/gitlink/changelists.json`):
```json
{
  "version": 1,
  "changelists": [
    { "name": "FeatureX",    "description": "WIP combat rework",
      "files": ["Content/Player.uasset", "..."] },
    { "name": "BugFixes",    "description": "",
      "files": [...] }
  ]
}
```

**Files to create:** `Source/GitLink/Private/GitLink_ChangelistsStore.h/cpp`

**Files to modify:** `Source/GitLink/Private/Commands/Cmd_Changelists.cpp`, `Source/GitLink/Private/GitLink_Provider.cpp` (load store at connect, save on changes).

**Acceptance:**
- Right-click a file in View Changes → "Move to → New Changelist" prompts for a name.
- The changelist persists across editor restarts.
- Files moved to a named changelist stay in git's working tree (no staging) but appear under the custom group in View Changes.

---

## Task 3 — Conflict Resolution (resolve-by-strategy)

**Priority:** Medium. The Resolve stub only stages the file; users hit merge conflicts and have no way to pick sides from the editor.

**Current:** `Cmd_Resolve.cpp` runs `Repository->Stage(InFiles)` which clears the conflict entry by taking whatever's currently on disk. No strategy selection.

**Design:** Extend the resolve flow with a dialog:

```
Editor detects conflict
        │
        ▼
User right-clicks → "Resolve..."
        │
        ▼
┌──────────────────────────────────┐
│   GitLink Resolve Dialog (NEW)   │
│──────────────────────────────────│
│ ○ Keep mine (--ours)             │
│ ○ Take theirs (--theirs)         │
│ ○ Launch merge tool              │
│ ○ Mark as resolved (current)     │
└──────────────┬───────────────────┘
               │
               ▼
┌────────────────────────────────────────────┐
│ Cmd_Resolve dispatches based on strategy:  │
│──────────────────────────────────────────  │
│ Ours    → git checkout --ours <files>      │
│            + git add <files>               │
│ Theirs  → git checkout --theirs <files>    │
│            + git add <files>               │
│ Merge   → launch FMergeToolUtils (UE)      │
│ Manual  → just git add (current behavior)  │
└────────────────────────────────────────────┘
```

**Files to modify:** `Source/GitLink/Private/Commands/Cmd_Resolve.cpp`

**Files to add:** Optional Slate dialog widget in `Source/GitLink/Private/Slate/SGitLink_ResolveDialog.h/cpp`

**Libgit2 ops needed:** None — use subprocess `git checkout --ours/--theirs` for strategy paths (libgit2 checkout with conflict side requires more plumbing).

**Acceptance:**
- Create a merge conflict (branch with divergent changes).
- Editor shows the file as conflicted.
- Right-click → Resolve → pick "Take theirs" → conflict disappears, file content matches remote.

---

## Task 4 — Stash Support

**Priority:** Low. Nice quality-of-life for "I need to pull but have uncommitted changes" workflows.

**Design:** Custom toolbar buttons (not SCC operations — UE has no stash concept):

```
Revision Control Menu
├─ Git (existing)
│   ├─ Push
│   ├─ Pull
│   ├─ Revert All
│   └─ Refresh
└─ Stash (NEW section)
    ├─ Stash changes...    (prompts for name)
    ├─ Apply latest stash
    └─ View stashes...     (opens list dialog)
```

**Backend:** All via subprocess `git stash` commands — libgit2 has stash support but calling subprocess is simpler.

**Files to modify:** `Source/GitLink/Private/GitLink_Menu.cpp` — add Stash section.

**Files to add:** Optional `Source/GitLink/Private/Slate/SGitLink_StashList.h/cpp` for the list view.

**Acceptance:**
- Editor has "Stash changes" button that captures current working tree state.
- "Apply latest stash" restores it.
- Stash list dialog shows all stashes with dates and messages.

---

## Task 5 — Unit Tests (GitLinkTests module) ✅ DONE

**Status:** Done, and extended since: ~85 tests as of v0.7.0 (see [CONTRIBUTING.md](CONTRIBUTING.md) for the suite map). The original design notes are kept below. New suites just drop a file into `Source/GitLinkTests/Private/Tests/`.

**Design:** Add a third module `GitLinkTests` using UE's Automation Framework.

```
┌──────────────────────────────────────┐
│  GitLinkTests (new module, Editor)   │
│──────────────────────────────────────│
│ Uses ephemeral git repos in temp dir │
│ Drives FRepository directly          │
│ Does NOT depend on UE SCC types      │
└──────────────┬───────────────────────┘
               │ links
               ▼
     ┌─────────────────┐
     │  GitLinkCore    │ (already exists — pure git facade)
     └─────────────────┘
```

**Landed suites (all green):**
1. ✅ **Status scan** — `Test_Repository_Status.cpp`: Untracked, StagedAdd, Modified, Deleted.
2. ✅ **Staging** — `Test_Repository_Staging.cpp`: Stage/Unstage round-trip, StageAll with nested dirs.
3. ✅ **Commit** — `Test_Repository_Commit.cpp`: commit + log-walk back, log path-filter across two commits.
4. ✅ **Log walk** — folded into the Commit suite (the path-filter case exercises `FLogQuery.PathFilter`).
5. ✅ **Branches** — `Test_Repository_Branches.cpp`: unborn HEAD pre-commit, HEAD branch visible after first commit. (Intentionally doesn't pin the branch name since libgit2's default depends on `init.defaultBranch`.)
6. ✅ **Submodules** — `Test_Repository_Submodules.cpp`: empty-case + two-entry detection from a raw `.gitmodules` file (no inner working trees needed — `git_submodule_foreach` picks them up from the file).

**Files landed:**
- `Source/GitLinkTests/GitLinkTests.Build.cs` — depends on `GitLinkCore` + `libgit2` (the latter needed explicitly for link, not just headers).
- `Source/GitLinkTests/GitLinkTests_Module.cpp` / `GitLinkTestsLog.h` — minimal `IMPLEMENT_MODULE` + log category.
- `Source/GitLinkTests/Private/Helpers/GitLinkTests_TempRepo.h` / `.cpp` — libgit2-backed ephemeral repo under `Saved/GitLinkTests/<guid>/`, seeds `user.name` / `user.email` in the repo's local config so commits can fall through to `default_signature`.
- `Source/GitLinkTests/Private/Tests/Test_Repository_Status.cpp` / `_Staging.cpp` / `_Commit.cpp` / `_Branches.cpp` / `_Submodules.cpp` — 11 suites total.

**Uplugin:** `GitLinkTests` added as `Type: UncookedOnly, LoadingPhase: Default`.

**Acceptance:**
- `Window → Developer Tools → Session Frontend → Automation` shows `GitLink.*` tests. *(Verified by running the editor once and expanding the tree.)*
- Tests run green in CI-style invocation: `UE -ExecCmds="Automation RunTests GitLink; Quit"`.

---

## Task 6 — Multi-Platform Support (Mac / Linux)

**Priority:** Low unless you ship cross-platform. Currently Win64 only.

**What's needed:**
1. Build libgit2 v1.9.0 for Mac + Linux (static or dynamic — static avoids .dylib/.so deployment issues).
2. Drop binaries into `Source/ThirdParty/libgit2/lib/Mac/` and `.../lib/Linux/`.
3. Extend `libgit2.Build.cs` platform check to detect + link the right binary per `Target.Platform`.

**Diagram:**

```
libgit2.Build.cs
     │
     ▼
switch (Target.Platform)
 ├─ Win64   → lib/Win64/git2.lib   + bin/Win64/git2.dll      [exists]
 ├─ Mac     → lib/Mac/libgit2.a    (static)                  [TODO]
 └─ Linux   → lib/Linux/libgit2.a  (static)                  [TODO]
```

**Files to modify:** `Source/ThirdParty/libgit2/libgit2.Build.cs`, `Source/ThirdParty/libgit2/README.md` (extend build recipe).

**Acceptance:** Plugin compiles + runs on Mac and Linux editor builds, with LFS locking still functional.

---

## Task 7 — Staging-on-save — implemented, needs a test

**Priority:** Low. Would surprise users who don't expect auto-staging.

**What:** When a file in the Staged changelist is saved, auto-run `Stage()` on it. The old plugin does this to keep the index in sync with on-disk content.

**Current:** `FGitLink_Provider::OnPackageSaved` already does this — it's already implemented. Mark this task done once verified.

**Files:** `Source/GitLink/Private/GitLink_Provider.cpp` — `OnPackageSaved()` already contains the logic.

**Acceptance:** Modify a file that's in Staged changelist, save, run `git status` from CLI — should show the file as staged (not "staged with unstaged modifications").

---

## Task 8 — Configurable poll interval at runtime

**Priority:** Low. Cosmetic improvement.

**What:** Shorten the background poll interval (default 120 s) when View Changes is open, restore it when closed. Reduces staleness for users who keep the window open.

**Approach:** Hook the View Changes window's visibility (via Slate tab manager) and call `_BackgroundPoll->SetInterval(5.f)` / `SetInterval(30.f)`.

**Files:** Would need a new `GitLink_ViewChangesWatcher.cpp` plus a provider lifetime hook.

**Acceptance:** With View Changes open, the poll log shows `(every 5 seconds)`. Close the window, log shows the configured interval again.

---

## Suggested sequencing

Work the table at the top from A downward; A–D are the gaps users are most likely to hit.

---

## How to Pick Up a Task

1. Branch off `main`: `git checkout -b dev/<task-slug>` (e.g., `dev/lockable-exts-fix`).
2. Implement changes per the task's acceptance criteria.
3. Build and run the `GitLink` automation tests as described in [CONTRIBUTING.md](CONTRIBUTING.md); add tests for the new behaviour.
4. Check the task's acceptance criteria in the editor.
5. Open a PR to `main`.
