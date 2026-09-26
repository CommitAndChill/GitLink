# Security Policy

## Reporting a vulnerability

Please **do not** open a public issue for a security problem.

Report it privately through GitHub instead:
[**Report a vulnerability**](https://github.com/CommitAndChill/GitLink/security/advisories/new)
(Security tab → *Report a vulnerability*). Include the GitLink version (shown in the editor log
at startup as `GitLink v…`), your Unreal Engine version, and steps to reproduce.

You should get an acknowledgement within a few days. Fixes are released as a new tagged version
and noted in the changelog.

## Supported versions

Only the latest release receives security fixes.

## How GitLink handles credentials

GitLink never stores credentials of its own.

- **Git LFS lock requests** made in-process ask git for a credential with
  `git credential fill` (your configured credential helper, e.g. Git Credential Manager), hold it
  in memory for the editor session, and send it only to the host it was issued for. Interactive
  helper prompts are disabled for these background requests.
- Credentials are sent in-process only over **HTTPS** (plain `http://` is allowed solely for a
  loopback test server). Any other endpoint falls back to the `git-lfs` command-line tool.
- A token embedded in a remote URL (`https://user:token@host/…`) is stripped before the URL is
  logged or shown in the editor.
- Fetch / pull go through libgit2 using the operating system's HTTPS stack (WinHTTP on Windows)
  with normal certificate verification. Push, LFS lock/unlock and LFS content transfer run the
  `git` / `git-lfs` command-line tools, so they use exactly the credentials and settings your
  command-line git uses.
