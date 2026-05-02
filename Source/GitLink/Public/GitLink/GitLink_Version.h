#pragma once

// --------------------------------------------------------------------------------------------------------------------
// GitLink version constant — single source of truth for the plugin's semantic version.
//
// HOW TO BUMP:
//   1. Edit GITLINK_VERSION below.
//   2. Edit the matching `VersionName` field in `Plugins/GitLink/GitLink.uplugin` to the same string.
//   3. Add a row to the "Version log" table in `Plugins/GitLink/CLAUDE.md` describing the change.
//
// Why both this header AND the .uplugin descriptor:
//   - The .uplugin's `VersionName` is what UE shows in the Plugins window, ships in cooked builds, and
//     is what `IPluginManager::FindPlugin(...)->GetDescriptor().VersionName` returns at runtime.
//   - GITLINK_VERSION is a compile-time constexpr usable in `static_assert`, log messages, telemetry,
//     and anywhere a runtime IPlugin lookup would be cumbersome. It also pins the version at compile
//     time of the binary you're running — useful when the source tree has been bumped past what the
//     loaded DLL was built against.
//
// The startup log line in `GitLink_Module.cpp` prints both, so a mismatch is visible in the Output
// Log immediately on editor launch.
// --------------------------------------------------------------------------------------------------------------------

// Semantic version: MAJOR.MINOR.PATCH. Bump MINOR for behavior changes, PATCH for bugfixes,
// MAJOR when the public C++ API breaks (FGitLink_Provider/FGitLink_FileState etc.).
#define GITLINK_VERSION TEXT("0.3.3")

// Build timestamp (ANSI string — `__DATE__` and `__TIME__` are compiler intrinsics, ANSI-only).
// Bake into whichever TU `#include`s this header and references it; that TU's `__DATE__`/`__TIME__`
// snapshot is what gets reported. Currently consumed only by `GitLink_Module.cpp`'s startup log,
// which is the right place: rebuilding the plugin recompiles that TU and captures the fresh
// timestamp. Use `%hs` (wide format spec for ANSI string) when logging via `UE_LOG`.
#define GITLINK_BUILD_TIMESTAMP (__DATE__ " " __TIME__)
