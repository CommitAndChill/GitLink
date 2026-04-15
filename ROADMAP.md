# GitLink Roadmap

Tasks remaining after v1.0. Each task below is self-contained and can be handed to a separate agent. Branches should be created off `main` and merged via PR.

## Current Status

```
                                        [dev/auto-refresh-and-toolbar]
                                                      │
                                                      ▼
                 ┌─────────────────────┬──────────────────────┐
  main ──────────┤ 0: untracked fix    │ 1: auto-refresh      │
  (v1.0)         │    (merged)         │ 2: toolbar buttons   │
                 └─────────────────────┴──────────────────────┘
```

**Shipped (main):** libgit2-backed SCC provider, LFS locking, Working/Staged changelists, CheckOut/CheckIn/Revert/MarkForAdd/Delete/Sync/Fetch, history, submodule handling, remote lock polling, command logging, untracked-file checkout fix.

**Pending merge (dev/auto-refresh-and-toolbar):** View Changes auto-refresh on save + working-tree-modifying ops, 4 toolbar buttons (Push/Pull/Revert All/Refresh).

---

## Task 1 — Investigate `lockable_exts=0`

**Priority:** Medium. Works today via hardcoded fallback, but silently ignoring `.gitattributes` is a correctness bug.

**Problem:** `ProbeLockableExtensions` in `GitLink_Subprocess.cpp` parses `git check-attr lockable *.uasset *.umap ...` output. At connect time the log shows `lockable_exts=0 (0=using hardcoded defaults)` even for repos with proper `.gitattributes` lockable entries.

**Investigation path:**

```
CheckRepositoryStatus()
        │
        ▼
Subprocess.ProbeLockableExtensions({ "*.uasset", "*.umap", "*.uexp", "*.ubulk" })
        │
        ▼
`git check-attr lockable *.uasset *.umap *.uexp *.ubulk`
        │
        ▼
Parses lines ending with ": set"
        │
        ▼
Returns empty → hardcoded fallback kicks in
```

**Hypotheses to test (in order):**
1. `git check-attr` output format differs from parser's expectation (e.g., `*.uasset: lockable: set` vs `*.uasset lockable set`).
2. The parser's `EndsWith(": set")` check is too strict (trailing whitespace? CRLF?).
3. `git check-attr` requires files to exist on disk matching the pattern.
4. The subprocess isn't running with the correct working directory.

**Files:** `Source/GitLink/Private/GitLink_Subprocess.cpp` (lines ~130-185).

**Acceptance:**
- Run `git check-attr lockable Content/Weapon/Sword.uasset` manually to see raw output.
- Add Verbose logging of the raw stdout in `ProbeLockableExtensions` to capture what the subprocess returns.
- Fix the parser to handle the actual output.
- Connect log shows `lockable_exts=N (N>0)` on a repo with configured lockable attributes.

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

## Task 5 — Unit Tests (GitLinkTests module)

**Priority:** Medium. No tests currently = no regression safety net for future changes.

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

**Test coverage priorities:**
1. **Status scan** — Create temp repo, add/modify/delete files, verify `Get_Status()` returns correct buckets.
2. **Staging** — `Stage()` + `Unstage()` round-trip.
3. **Commit** — `Commit()` produces expected head hash.
4. **Log walk** — `Get_Log()` with path filter returns expected revisions.
5. **Branches** — `Get_Branches()` + `Get_CurrentBranchName()`.
6. **Submodules** — Open nested repo, verify `Enumerate_SubmodulePaths` returns correct list.

**Files to create:**
- `Source/GitLinkTests/GitLinkTests.Build.cs`
- `Source/GitLinkTests/GitLinkTests_Module.cpp`
- `Source/GitLinkTests/Private/Tests/Test_Repository_Status.cpp` (one file per test suite)
- `Source/GitLinkTests/Private/Helpers/TempRepo.h` (helper that creates/destroys a git repo in a temp dir)

**Update:** `GitLink.uplugin` — add the new module as `Type: DeveloperTool, LoadingPhase: Default`.

**Acceptance:**
- `Window → Developer Tools → Session Frontend → Automation` shows `GitLink.*` tests.
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

## Task 7 — Staging-on-save (optional, mirrors old plugin)

**Priority:** Low. Would surprise users who don't expect auto-staging.

**What:** When a file in the Staged changelist is saved, auto-run `Stage()` on it. The old plugin does this to keep the index in sync with on-disk content.

**Current:** `FGitLink_Provider::OnPackageSaved` already does this — it's already implemented. Mark this task done once verified.

**Files:** `Source/GitLink/Private/GitLink_Provider.cpp` — `OnPackageSaved()` already contains the logic.

**Acceptance:** Modify a file that's in Staged changelist, save, run `git status` from CLI — should show the file as staged (not "staged with unstaged modifications").

---

## Task 8 — Configurable poll interval at runtime

**Priority:** Low. Cosmetic improvement.

**What:** Shorten the 30s background poll interval when View Changes is open, restore it when closed. Reduces staleness for users who keep the window open.

**Approach:** Hook the View Changes window's visibility (via Slate tab manager) and call `_BackgroundPoll->SetInterval(5.f)` / `SetInterval(30.f)`.

**Files:** Would need a new `GitLink_ViewChangesWatcher.cpp` plus a provider lifetime hook.

**Acceptance:** With View Changes open, the poll log shows `(every 5 seconds)`. Close the window, log shows `(every 30 seconds)`.

---

## Suggested Sequencing

```
Week 1:  Task 1 (lockable_exts investigation — small, unblocks correctness)
Week 2:  Task 5 (tests — unblocks safe changes for tasks 2/3)
Week 3:  Task 3 (conflict resolution — user-facing, medium scope)
Week 4:  Task 2 (named changelists — new feature, medium scope)
Later:   Tasks 4, 6, 7, 8 (lower priority / optional)
```

---

## How to Pick Up a Task

1. Branch off `main`: `git checkout -b dev/<task-slug>` (e.g., `dev/lockable-exts-fix`).
2. Implement changes per the task's acceptance criteria.
3. Build with `"<EnginePath>/Engine/Build/BatchFiles/Build.bat" <Project>Editor Win64 Development "<Project>.uproject" -waitmutex`.
4. Test manually per the task's acceptance section.
5. Open a PR to `main`.
