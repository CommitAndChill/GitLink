# Contributing to GitLink

Thanks for helping. Bug reports with logs, reproductions and pull requests are all welcome.

## Reporting a bug

Open an issue with the bug template. The most useful things to include:

- GitLink version (log line `GitLink vX.Y.Z`), Unreal Engine version, `git --version`, `git lfs version`.
- What you did, what you expected, what happened.
- The relevant log lines after `Log LogGitLink Verbose` (remove anything private, such as internal URLs).

Security problems go through [SECURITY.md](SECURITY.md), not public issues.

## Building

GitLink is a normal Unreal source plugin. Put the repository at `<AnyProject>/Plugins/GitLink/` in a C++ project and
build the project's editor target (from your IDE, or `Engine/Build/BatchFiles/Build.bat <Project>Editor Win64
Development -Project="<path>.uproject"`). Win64 only for now; libgit2 is vendored under
`Source/ThirdParty/libgit2/` (see its README to rebuild it).

## Running the tests

Tests live in the `GitLinkTests` module and run with Unreal's automation framework. They need `git` and `git-lfs`
on `PATH`; tests that need Git skip themselves with a warning when it is missing.

From the editor: **Tools → Test Automation**, filter on `GitLink`.

From the command line:

```powershell
UnrealEditor-Cmd.exe "<path>.uproject" -ExecCmds="Automation RunTests GitLink; Quit" -unattended -nullrhi -nosplash -log
```

What the suites cover:

| Prefix | Needs | Covers |
|---|---|---|
| `GitLink.Repository.*`, `GitLink.FileState.*`, `GitLink.Lfs*`, `GitLink.VerifyJson.*`, … | nothing | libgit2 facade, state predicates, parsers, lifetime/concurrency |
| `GitLink.Remote.*`, `GitLink.HookProbe.*`, `GitLink.Edge.*` | `git` | fetch / pull / push against a local bare remote, hook detection, first-run edge cases |
| `GitLink.Live.*` | `GITLINK_LIVE_REMOTE` | fetch / pull and LFS lock/unlock against a real private HTTPS remote |

The live tests pass without doing anything unless the environment variable `GITLINK_LIVE_REMOTE` is set to the HTTPS
URL of a private repository your Git credential helper can already access. For the LFS test that repository must
contain an LFS-lockable `Content/Seed.uasset`:

```text
*.uasset filter=lfs diff=lfs merge=lfs -text lockable
```

Test fixtures create repositories under `<Project>/Saved/GitLinkTests/` and delete them afterwards.

## Making changes

- Read [CLAUDE.md](CLAUDE.md) first: it documents the architecture (command dispatch, state cache, LFS locking,
  submodule routing) and a list of pitfalls that each cost a real bug.
- Code style: trailing return types (`auto F() -> T`), `Get_` / `Set_` / `Is_` / `Request_` prefixes, early
  returns, and namespaces `gitlink` (core), `gitlink::cmd` (commands), `gitlink::op` (core operations). Match the
  surrounding code.
- Anything that stages LFS-tracked files must go through `gitlink::cmd::Stage_ViaGit` (libgit2 does not run the LFS
  clean filter). Anything that touches a submodule file must partition by repository first (`PartitionByRepo`).
- Add or update a test for behaviour you change. A test that would have passed before your fix does not count.
- Commit messages follow [Conventional Commits](https://www.conventionalcommits.org/) (`fix:`, `feat:`, `test:`,
  `docs:`, …) and explain *why*.
- Behaviour changes bump the version (`GITLINK_VERSION` in `Source/GitLink/Public/GitLink/GitLink_Version.h` and
  `VersionName` in `GitLink.uplugin`) and add an entry to [CHANGELOG.md](CHANGELOG.md).

## Benchmarks

`Benchmarks/SourceControlBenchmark` is a separate plugin that times any source control provider through Unreal's
generic interface. [Docs/BENCHMARKS.md](Docs/BENCHMARKS.md) explains how to run it on your own project.
