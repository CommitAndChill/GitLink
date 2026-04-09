# GitLink

In-process libgit2-based source control provider for Unreal Engine. Clean-room replacement for the `GitSourceControl` plugin, designed to be standalone (no `CkFoundation`/`CkApplication`/`CkTests` dependencies) so it can drop into any Unreal project unchanged.

## Modules

| Module         | Type          | Purpose                                                                          |
|----------------|---------------|----------------------------------------------------------------------------------|
| `GitLinkCore`  | Runtime       | Pure C++ git facade over libgit2. No Unreal source-control types. Unit-testable. |
| `GitLink`      | UncookedOnly  | `ISourceControlProvider` implementation, command dispatcher, Slate settings UI.  |

## Dependencies

- `Core`, `CoreUObject`, `Engine`, `SourceControl`, `Slate`, `SlateCore`, `InputCore`, `UnrealEd`, `Projects`
- Vendored `libgit2 v1.9.0` in `Source/ThirdParty/libgit2/`

**No dependency** on any `Ck*` module.

## Build

The plugin compiles even when libgit2 binaries are absent: `Source/ThirdParty/libgit2/libgit2.Build.cs` probes for `include/git2.h`, `lib/Win64/git2.lib`, and `bin/Win64/git2.dll`, and defines `WITH_LIBGIT2=0` when any are missing. In that state the provider self-disables at startup. See `Source/ThirdParty/libgit2/README.md` for the build recipe.

## Enablement

Ships with `EnabledByDefault: false` so it coexists with the existing `GitSourceControl` plugin during development. Flip the flag once parity is verified.
