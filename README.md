<p align="center">
  <img src="Resources/GitLinkBanner.png" alt="GitLink — Your files are safe. Even from you." width="100%">
</p>

<p align="center">
  <strong>Git source control for Unreal Engine, without leaving the editor.</strong>
</p>

<p align="center">
  <img alt="Version 0.6.0" src="https://img.shields.io/badge/version-0.6.0-00bde8">
  <img alt="Beta" src="https://img.shields.io/badge/status-beta-246bfe">
  <img alt="Windows 64-bit" src="https://img.shields.io/badge/platform-Win64-08204f">
  <a href="LICENSE.md"><img alt="License" src="https://img.shields.io/badge/license-see%20LICENSE-ffffff"></a>
</p>

GitLink is an editor-native Git provider built for Unreal projects. It brings everyday source-control tasks, Git LFS file locking, and submodule-aware asset handling into Unreal's familiar Revision Control workflow.

> [!NOTE]
> GitLink is currently beta software. The repository includes the required libgit2 binaries for Win64; macOS and Linux are not yet supported by the bundled backend.

## ✨ Highlights

- **Work where you build.** See modified Content files in Unreal's **View Changes** window, move files between Working and Staged, write a commit message, and submit without switching tools.
- **Protect binary assets with Git LFS locks.** Checking out a lockable asset acquires its LFS lock; files locked by teammates show the lock owner's name so conflicts are clear before editing.
- **Use familiar editor actions.** Refresh, Pull, Push, Revert, history, and revision diff are integrated with Unreal's source-control UI, with convenient toolbar actions for common repository-wide operations.
- **Keep submodule-heavy projects usable.** Status, history, diff, staging, commits, and LFS operations are routed to the repository that owns each file—including initialized Git submodules.
- **Stay current automatically.** Background polling refreshes repository and lock state, while saves and editor activity trigger faster updates for the files you are actively using.
- **Respect repository hooks.** GitLink can fall back to the Git executable for supported hooked commit and push operations.

## Requirements

| Requirement | Why it is needed |
|---|---|
| Unreal Engine 5 project with C++ build support | GitLink is a source plugin and must be compiled for your editor build. |
| Windows 64-bit | The included libgit2 v1.9.0 binaries currently target Win64. |
| Git available on `PATH` | Used for filter-aware staging and selected fallback operations. |
| Git LFS available on `PATH` | Required for LFS locking and correct handling of LFS-managed files. |
| An existing Git working tree | GitLink connects to your project repository; it does not initialize or clone one. |

## 📦 Installation

### 1. Install the prerequisites

Install [Git](https://git-scm.com/download/win) and [Git LFS](https://git-lfs.com/), then initialize LFS for your user account:

```powershell
git lfs install
```

Make sure your repository also has a Git identity configured:

```powershell
git config user.name "Your Name"
git config user.email "you@example.com"
```

### 2. Add GitLink to your project

Close Unreal Editor, then place this repository at:

```text
<YourProject>/Plugins/GitLink/
```

Your project should contain this file when the copy is complete:

```text
<YourProject>/Plugins/GitLink/GitLink.uplugin
```

Open the project and allow Unreal to build the plugin if prompted. Teams distributing prebuilt projects should compile GitLink for the same engine version and configuration used by the rest of the project.

### 3. Enable and connect

1. Open **Edit → Plugins**, search for **GitLink**, and enable it.
2. Restart the editor when prompted.
3. Open the **Revision Control** menu and choose **Connect to Revision Control**.
4. Select **GitLink** as the provider and accept the connection settings.
5. Open **Project Settings → Editor → GitLink** to tune background polling, LFS locking, hook fallback, or the repository-root override.

GitLink normally discovers the repository by searching upward from the project directory. Use **Repository Root Override** only when the `.uproject` is not located beneath the intended working tree.

## 🔒 Recommended Git LFS setup

GitLink can recognize common Unreal binary assets automatically, but explicitly marking them as LFS-managed and lockable keeps the repository policy portable across every Git client:

```gitattributes
*.uasset filter=lfs diff=lfs merge=lfs -text lockable
*.umap   filter=lfs diff=lfs merge=lfs -text lockable
```

Commit `.gitattributes` before adding assets that should use LFS. Existing files may need a separate Git LFS migration; review that operation carefully before rewriting shared history.

## Everyday workflow

1. Check out a lockable asset in Unreal before editing it. GitLink acquires the corresponding LFS lock and makes the file writable.
2. Open **View Changes** to review files under **Working**.
3. Drag files to **Staged** when they are ready for the next commit.
4. Enter a commit message and submit the Staged group.
5. Push from the GitLink toolbar when you are ready to publish.

Pull uses fast-forward semantics and will refuse unsafe updates rather than create an implicit merge. Repository-wide **Revert All** is destructive, so review the pending changes before confirming it.

## Configuration

The **Project Settings → Editor → GitLink** page provides:

- automatic repository discovery or an explicit repository-root override;
- background fetch/status polling and its interval;
- Git LFS file locking;
- fallback to the Git executable when supported repository hooks are present; and
- an optional explicit path to the Git executable.

The provider connection panel also reports the detected repository root, branch, remote, Git identity, LFS availability, backend, and GitLink version.

## Current scope and limitations

- GitLink is an editor integration, not a complete replacement for every Git command. Use your normal Git client for branching, rebasing, conflict-heavy merges, repository setup, and history rewriting.
- The View Changes workflow intentionally models two groups: **Working** and **Staged**. It does not implement arbitrary named Git changelists.
- Pull supports fast-forward updates; it does not perform an automatic merge or rebase.
- Only Win64 ships with a ready-to-use libgit2 backend today. The module can compile without the backend, but the provider then disables itself at startup.
- LFS operations depend on the repository's remote, authentication, `.gitattributes`, and local Git LFS installation being configured correctly.

## For contributors

GitLink is split into two primary modules:

| Module | Type | Purpose |
|---|---|---|
| `GitLinkCore` | Runtime | A focused C++ facade over libgit2 with no Unreal source-control types. |
| `GitLink` | UncookedOnly | The Unreal source-control provider, async command dispatcher, menus, settings, and editor integration. |

`GitLinkTests` contains the automation coverage. The vendored backend lives under `Source/ThirdParty/libgit2/`; see its [build notes](Source/ThirdParty/libgit2/README.md) when updating or porting libgit2.

## License

See [LICENSE.md](LICENSE.md) for the terms that apply to this project.
