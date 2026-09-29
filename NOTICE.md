# Third-party notices

GitLink's own terms are in [LICENSE.md](LICENSE.md). It also ships the following third-party software, which
stays under its own license.

## libgit2 v1.9.0

- **Shipped files:** `Source/ThirdParty/libgit2/bin/Win64/git2.dll`, `Source/ThirdParty/libgit2/lib/Win64/git2.lib`
  and the public headers under `Source/ThirdParty/libgit2/include/`.
- **License:** GNU GPL v2 with the libgit2 linking exception. The full text, including the notices for the
  components libgit2 itself bundles, is in [`Source/ThirdParty/libgit2/COPYING`](Source/ThirdParty/libgit2/COPYING)
  and must be kept alongside the binaries.
- **Corresponding source:** the upstream `v1.9.0` tag,
  commit [`338e6fb681369ff0537719095e22ce9dc602dbf0`](https://github.com/libgit2/libgit2/tree/338e6fb681369ff0537719095e22ce9dc602dbf0)
  ([source archive](https://github.com/libgit2/libgit2/archive/refs/tags/v1.9.0.tar.gz)).
- **Build configuration:** the binaries are built from an unmodified checkout of that tag with the recipe in
  [`Source/ThirdParty/libgit2/README.md`](Source/ThirdParty/libgit2/README.md#build-recipe-win64-msvc)
  (WinHTTP for HTTPS, no SSH).
