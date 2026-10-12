# AGENTS.md

Guidance for AI coding agents working in this repository.

## What this is

A fork of [paolostivanin/OTPClient](https://github.com/paolostivanin/OTPClient) (a GTK4 / libadwaita TOTP/HOTP/Steam authenticator written in C11) whose goal is a **Windows port** with the graphical and OS-specific parts isolated behind small interfaces. Linux behaviour must not regress, and upstream updates must keep merging cheaply.

Two things override convenience:

1. **Upstream mergeability.** Read `docs/upstream-sync.md` before touching any existing file.
2. **Verify before claiming.** Windows API choices in `docs/windows-port.md` are hypotheses, not decisions. Open decisions there are the owner's to make; do not build on one silently.

## Layout

```
src/common/          shared core: encrypted DB, importers, OTP, secrets, settings (not a library; each target compiles it)
src/gui/             GTK4 + libadwaita app (otpclient-window.c is 5.4k lines; ui/*.ui, gui.gresource.xml, dialogs/)
src/cli/             otpclient-cli
src/search-provider/ GNOME Shell / KRunner D-Bus provider (Linux only)
tests/               ctest suite (C, plus tests/isolated-session.py for D-Bus/Xvfb tests)
data/                desktop/appdata/gschema files, icons, D-Bus services, completions
.circleci/ .ci/      CI config and scripts (Linux distros, sanitizers, scan-build)
docs/                architecture, Windows port plan, platform audit, upstream-sync rules
```

Planned and fork-only (do not create unless the task asks): `src/platform/{linux,windows}/`, `cmake/`, `packaging/windows/`.

Deeper reading: `docs/architecture.md` (how it works today), `docs/windows-port.md` (seams, phases, open decisions), `docs/platform-dependencies.md` (every Linux-only call, with file:line), `docs/upstream-sync.md` (merge rules).

## Dev environment

Tools come from `flake.nix`, so nothing is installed on the host. With direnv (`.envrc` is `use flake`) run `direnv allow` once; otherwise `nix develop`. Shells: `default` (native Linux build and tests: gcc/clang, cmake, ninja, all pkg-config deps, gdb, valgrind, dbus, Xvfb) and `windows` (`nix develop .#windows`, experimental and **not yet built end to end**, it only evaluates so far: MinGW-w64 UCRT cross toolchain, cross-built glib/libgcrypt/jansson/libcotp, Wine; the cross packages are largely not in the binary cache and build from source). `flake.nix`, `flake.lock` and `.envrc` are fork-only files. Do not add tools to the host or to upstream files.

## Build and test (Linux)

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Debug          # BUILD_TESTING is ON by default (include(CTest))
cmake --build build -j
ctest --test-dir build --output-on-failure
```

- Options: `-DBUILD_GUI=OFF`, `-DBUILD_CLI=OFF`, `-DBUILD_SEARCH_PROVIDER=OFF`, `-DSANITIZE=ON` (ASan+UBSan, use with Debug), `-DIS_FLATPAK=ON`, `-DENABLE_MINIMIZE_TO_TRAY=OFF`.
- Build dependencies (pkg-config): libgcrypt ≥ 1.10.1, libcotp ≥ 4.0.0 (CI builds it from source; nixpkgs packages it), jansson, glib/gio ≥ 2.74, uuid, libsecret; for the GUI also gtk4 ≥ 4.10, libadwaita ≥ 1.5, gdk-pixbuf, zbar, libprotobuf-c, libqrencode.
- D-Bus and GUI tests are gated on `python3`, `dbus-daemon`, `glib-compile-schemas` (and `Xvfb` for GUI tests) and run through `tests/isolated-session.py`. Without them those tests are simply not registered; do not treat their absence as a pass.
- CLI smoke test: `.ci/smoke-test.sh`.
- Compiler flags are strict (`-Wall -Wextra -Werror=format-security -Werror=implicit-function-declaration`). Do not add warnings.

## Code conventions

- Match the surrounding C style (GLib types and `g_autoptr`/`g_autofree`, 4-space indent, space before call parentheses: `g_free (x)`).
- Secrets: password and key material go in libgcrypt secure memory (`gcry_malloc_secure`, `secure_strdup`) and are freed with the `sensitive_*` helpers in `common.c`. Never log secrets; never copy them into ordinary heap buffers.
- The DB format is versioned (`DB_VERSION` 3). Never change the on-disk format as a side effect of porting.
- Do not add dependencies without checking that they exist on Windows.

## Upstream-sync rules (summary)

- Remotes: `origin` = this fork, `upstream` = paolostivanin/OTPClient. Sync with `git merge upstream/master`.
- **Do not move, rename or reformat upstream files.** No drive-by cleanup, no clang-format passes.
- Fork-only code goes in new directories. Edits to upstream files are one-liners that route into `src/platform/`; no Windows logic blocks, no `#ifdef` sprawl in the churn hotspots (`otpclient-window.c`, `otpclient-application.c`, `CMakeLists.txt`, `db-common.c`, `common.c`, `search-provider.c`, `exec-action.c`, `README.md`).
- Anything not Windows-specific that upstream would accept is a candidate for an upstream PR instead.

## Windows-port rules (summary)

- Seams are either **shims** (compat headers / a libsecret replacement, no call-site edits) or **ports** (small interfaces: secrets, settings, paths, autostart, tray, session lock, hardening). See `docs/windows-port.md`.
- The Linux implementation of a port wraps today's code unchanged. Windows is additive.
- The Windows build excludes the search provider, Flatpak and D-Bus service files.
- Before adding or changing a Linux-only call, update `docs/platform-dependencies.md`.

## Commits and PRs

- Commit messages in English, conventional style as in the history (`fix(tray): ...`, `ci: ...`, `docs: ...`, `release: ...`).
- Keep commits focused: a port change and an unrelated refactor are separate commits.
- Do not commit unless asked. Do not push, tag or bump the version.
- Never include session links or tool-generated URLs in commit messages or PR descriptions.

## Working here

- Run independent reads/searches/builds in parallel; run anything slow (builds, test suites) in the background.
- Cross-check any claim about the code against the code before writing it into `docs/`.
