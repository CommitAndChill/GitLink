# Benchmarks

How long common source control operations take in GitLink compared with the two plugins most Unreal teams would
otherwise use:

- **Git LFS 2** — the community plugin maintained by Project Borealis ([ProjectBorealis/UEGitPlugin](https://github.com/ProjectBorealis/UEGitPlugin)).
- **Git** — Epic's own Git plugin that ships with the engine (beta).

## Results

**Pending.** The comparison run on a large production project (≈33k LFS assets, 31 submodules) has not been
completed yet. Until then, the harness below lets you measure all three plugins on your own project, and the one
measured GitLink data point is in the [changelog](../CHANGELOG.md): provider start-up LFS endpoint discovery went from
~2.9 s of blocking `git` process spawns to a negligible in-process read on a 31-submodule project (v0.6.0).

## Method

**Harness.** `Benchmarks/SourceControlBenchmark` in this repository is a small editor plugin with one automation
test, `SourceControlBenchmark.Run`. It drives the provider only through Unreal's generic `ISourceControlProvider`
interface, so the identical code times every plugin. Each operation is a synchronous `Execute` on the game thread,
timed with `FPlatformTime::Seconds`:

| Operation | What it does |
|---|---|
| Init | `SetProvider(<name>)` — the provider's own initialisation. |
| Connect (cold) | The first `FConnect` after Init — what the editor waits for when a project opens. |
| Connect (warm) | Subsequent `FConnect` calls. |
| Status, 100 / 1000 files | `FUpdateStatus` on 100 / 1000 explicit `.uasset` paths (the same sorted set for every provider). |
| Status, refresh | `FUpdateStatus` with an empty file list — each provider's "refresh everything". What this covers differs by provider (see notes). |
| Mark for add, 100 | `FMarkForAdd` on 100 new small text files. |
| Revert, 100 | `FRevert` on those same 100 files. |

**Process.** One fresh editor process per run (`-nullrhi`, automation test run), with no revision control provider
configured beforehand, so Init and the first Connect are a genuine cold start. Each run records one cold Connect and
five warm repetitions of everything else; five runs per provider. Tables report the median across all samples.

**Isolation.** Git LFS 2 and Epic's Git plugin share the plugin name `GitSourceControl`, so they cannot be loaded at
the same time. Each provider was benchmarked in its own configuration of the same checkout:

| Subject | Plugins enabled |
|---|---|
| GitLink | GitLink; `GitSourceControl` disabled |
| Git LFS 2 | Project Borealis `GitSourceControl` in `Plugins/`; GitLink disabled |
| Git (Epic) | no project `GitSourceControl` (so the engine's loads); GitLink disabled |

## Reproducing

1. Copy `Benchmarks/SourceControlBenchmark` to `<YourProject>/Plugins/SourceControlBenchmark` and enable it for the
   Editor target.
2. Enable exactly one provider plugin as in the table above, and make sure no provider is selected yet (delete
   `Saved/Config/WindowsEditor/SourceControlSettings.ini`).
3. Build the editor, then run:

   ```powershell
   $env:SCBENCH_PROVIDER = "GitLink"      # or "Git LFS 2", or "Git"
   $env:SCBENCH_ITERATIONS = "5"
   UnrealEditor-Cmd.exe "<path>.uproject" -ExecCmds="Automation RunTests SourceControlBenchmark.Run; Quit" -unattended -nullrhi -nosplash -log
   ```

4. Results are appended to `Saved/SourceControlBenchmark/results.csv` (`provider,operation,iteration,ms,ok`) and
   echoed to the log as `SCBENCH,…` lines. Repeat step 3 for as many cold runs as you want, then switch providers
   (step 2) and repeat.

The benchmark leaves the repository as it found it: the 100 files it adds are reverted and deleted.
