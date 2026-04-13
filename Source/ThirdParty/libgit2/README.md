# libgit2 (vendored)

Drop-in location for the prebuilt libgit2 shared library that `GitLinkCore` links against.

## Target version

**libgit2 v1.9.0**

## Expected layout (Win64)

```
Source/ThirdParty/libgit2/
├── libgit2.Build.cs
├── README.md                  (this file)
├── include/
│   ├── git2.h
│   └── git2/                  (all public headers from upstream)
├── lib/Win64/git2.lib
└── bin/Win64/git2.dll
```

When all three of `include/git2.h`, `lib/Win64/git2.lib`, and `bin/Win64/git2.dll` are present, `libgit2.Build.cs` defines `WITH_LIBGIT2=1` and the plugin lights up. Otherwise it compiles with `WITH_LIBGIT2=0` and the provider self-disables at startup.

## Build recipe (Win64, MSVC)

From a clean libgit2 v1.9.0 source checkout:

```powershell
git clone --branch v1.9.0 --depth 1 https://github.com/libgit2/libgit2.git
cd libgit2
mkdir build
cd build

cmake .. ^
  -G "Visual Studio 17 2022" ^
  -A x64 ^
  -DCMAKE_BUILD_TYPE=Release ^
  -DBUILD_SHARED_LIBS=ON ^
  -DBUILD_TESTS=OFF ^
  -DBUILD_CLI=OFF ^
  -DUSE_HTTPS=WinHTTP ^
  -DUSE_SSH=OFF ^
  -DREGEX_BACKEND=builtin

cmake --build . --config Release
```

Then copy into this folder:

| From (in libgit2 build tree)              | To (under this folder)          |
|-------------------------------------------|---------------------------------|
| `../include/git2.h`                       | `include/git2.h`                |
| `../include/git2/*.h`                     | `include/git2/*.h`              |
| `build/Release/git2.lib`                  | `lib/Win64/git2.lib`            |
| `build/Release/git2.dll`                  | `bin/Win64/git2.dll`            |

### Refreshing UBT

Because UBT does not track `File.Exists()` in `Build.cs` as a build input, after dropping new binaries in you must **touch `libgit2.Build.cs`** (any harmless whitespace change) or delete `Plugins/GitLink/Intermediate/` so UBT re-evaluates.

## Why WinHTTP instead of OpenSSL

Avoids vendoring OpenSSL alongside libgit2 and picks up Windows trust store + proxy settings automatically.

## Mac / Linux

Not supported in v0.1. The `Build.cs` is structured so adding them later is a matter of dropping the right binaries into `lib/Mac/`, `bin/Mac/`, etc. and extending the platform check.
