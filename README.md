# GitLink

In-process libgit2-based source control provider for Unreal Engine with LFS locking support. Standalone plugin that drops into any Unreal project unchanged.

## Modules

| Module         | Type          | Purpose                                                                          |
|----------------|---------------|----------------------------------------------------------------------------------|
| `GitLinkCore`  | Runtime       | Pure C++ git facade over libgit2. No Unreal source-control types. Unit-testable. |
| `GitLink`      | UncookedOnly  | `ISourceControlProvider` implementation, command dispatcher, Slate settings UI.  |

## Dependencies

- `Core`, `CoreUObject`, `Engine`, `SourceControl`, `Slate`, `SlateCore`, `InputCore`, `UnrealEd`, `Projects`
- Vendored `libgit2 v1.9.0` in `Source/ThirdParty/libgit2/`

## Build

The plugin compiles even when libgit2 binaries are absent: `Source/ThirdParty/libgit2/libgit2.Build.cs` probes for `include/git2.h`, `lib/Win64/git2.lib`, and `bin/Win64/git2.dll`, and defines `WITH_LIBGIT2=0` when any are missing. In that state the provider self-disables at startup. See `Source/ThirdParty/libgit2/README.md` for the build recipe.

## Enablement

Ships with `EnabledByDefault: false` in the `.uplugin`. Enable it in your project's `.uproject` or plugin settings.
