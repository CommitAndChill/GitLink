# Changelog

All notable changes to GitLink. Versions follow [Semantic Versioning](https://semver.org/) while the plugin is in
beta (`0.x`): a minor bump can change behaviour. Engineering detail for each version — root causes, measurements and
test notes — is kept in the version log in [CLAUDE.md](CLAUDE.md).

## [0.8.0] — 2026-09-28

### Fixed
- **Editor startup no longer stalls on LFS lock checks.** Every lock query waited for a reply that only the game
  thread could deliver, so the checks run by the startup connection — which happen before the editor draws its first
  frame — each timed out after 10 seconds however fast the server answered (`LogHttp: HTTP request timed out after
  10.00 seconds ... /info/lfs/locks/verify`), and then repeated the query through `git lfs`. Replies are now
  delivered on the HTTP thread, so lock state arrives in the time the server takes to answer.

## [0.7.0] — 2026-09-28

### Fixed
- **Pull, Fetch and the background fetch now work with private HTTPS remotes.** They previously sent no credential
  and failed with HTTP 401 against any private repository (for example a private GitHub repo). Credentials now come
  from your Git credential helper — the same ones `git fetch` uses.
- Pull and Fetch use the remote your branch tracks instead of always `origin`.
- GitLink no longer compiles into game builds or stages `git2.dll` into packaged games when enabled without a
  per-project target restriction.
- Creating, editing or deleting a named changelist now reports that the feature is unsupported instead of appearing
  to succeed while doing nothing.
- Commit hooks are detected where Git actually looks for them: `core.hooksPath`, linked worktrees and submodules.
  Previously a repository using `core.hooksPath` (husky, lefthook, …) committed without running its hooks.
- A failed Push now shows Git's reason (for example "rejected — fetch first") instead of a generic failure.
- Checking out without Git installed now says what to install.
- Engine-version guards that would have silently disabled features on Unreal Engine 6 were corrected.

### Security
- A token embedded in a remote URL (`https://user:token@host/…`) is no longer written to logs, shown in the editor or
  passed to the credential helper.
- In-process LFS requests never send credentials over plain `http://` (a loopback test server excepted).
- Background credential lookups are non-interactive, so a hidden request can no longer open a login window.
- Windows integrated authentication (NTLM / Kerberos) during fetch is now opt-in (*Allow Windows Integrated
  Authentication*, off by default); previously it was offered to any server that asked.
- After a credential is rejected twice, in-process lock polling for that host pauses for 10 minutes instead of
  retrying every cycle.
- Added [SECURITY.md](SECURITY.md). libgit2's license text now ships with its binaries.

### Changed
- The plugin declares Win64 only (Mac/Linux were listed but have no backend yet).
- Settings tooltips describe what each setting does.

### Added
- Automated coverage for fetch, pull and push against a local remote, hook detection, and first-run edge cases
  (no Git, no commits yet, detached HEAD, non-ASCII paths); opt-in live tests against a real private remote and LFS
  server. See [CONTRIBUTING.md](CONTRIBUTING.md).

## [0.6.0]
- Faster editor startup on projects with many submodules: LFS endpoint discovery reads Git configuration in-process
  instead of starting three `git` processes per repository (measured 2.9 s → negligible on a 31-submodule project).

## [0.5.0]
- Faster editor startup: the provider no longer re-initialises when the editor asks it to connect a second time.

## [0.4.3]
- Fixed LFS-tracked assets being staged as raw binaries instead of LFS pointers, which could put `.uasset` / `.umap`
  content into Git history outside LFS.

## [0.4.2]
- Fixed a multi-second hang when checking out after deleting actors in a One File Per Actor level.

## [0.4.1]
- Fixed deleting read-only (lockable) assets silently leaving the file on disk. Deleting a file someone else has
  locked is now blocked.

## [0.4.0]
- Reliability pass: fixed a family of crashes when the provider reconnected or the editor shut down while operations
  were running; Check In no longer leaks files staged in another changelist into a commit; Pull can no longer leave
  the repository half-updated when the working tree is dirty; Push reports server-side rejections; hardened quoting
  of paths and commit messages passed to Git.

## [0.3.x]
- Lock state for files you are working with refreshes within seconds (on focus, asset open and first edit), while
  the full background sweep runs every 2 minutes. Multiple fixes for lock-state flicker after locking/unlocking,
  editor hangs during pre-checkout lock checks, CPU spikes during the background sweep, and crashes during cooking
  and shutdown.
- Revert now restores LFS-tracked files as real content instead of pointer text.

## [0.2.0]
- Fixed locks in submodules not appearing after an editor restart, unlock failures being reported as success, and
  newly created files in submodules prompting for check-out on save.

## [0.1.0]
- Initial release: libgit2-backed source control provider with Git LFS locking, Working/Staged changelists,
  check out / check in / revert / mark for add / delete / pull / fetch, history and diff, and submodule support.
