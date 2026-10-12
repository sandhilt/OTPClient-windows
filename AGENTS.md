# AGENTS.md

Guidance for AI coding agents working in this repository.

## What this is

A fork of [paolostivanin/OTPClient](https://github.com/paolostivanin/OTPClient) (a GTK4 / libadwaita TOTP/HOTP/Steam authenticator written in C11) whose goal is a **Windows port** with the graphical and OS-specific parts isolated behind small interfaces. Linux behaviour must not regress, and upstream updates must keep merging cheaply.

Two things override convenience:

1. **Upstream mergeability.** Read `docs/upstream-sync.md` before touching any existing file.
2. **Verify before claiming.** Unimplemented Windows API candidates in `docs/windows-port.md` are hypotheses, not decisions. Open decisions there are the owner's to make; do not build on one silently.

## Layout

```
src/common/          shared core: encrypted DB, importers, OTP, secrets, settings (not a library; each target compiles it)
src/gui/             GTK4 + libadwaita app (otpclient-window.c is 5.4k lines; ui/*.ui, gui.gresource.xml, dialogs/)
src/cli/             otpclient-cli
src/search-provider/ GNOME Shell / KRunner D-Bus provider (Linux only)
src/platform/windows/ Windows compatibility headers, adapters and regression checks (core + CLI)
cmake/               Windows UCRT toolchain, deferred target wiring and pkg-config shims
tests/               ctest suite (C, plus tests/isolated-session.py for D-Bus/Xvfb tests)
data/                desktop/appdata/gschema files, icons, D-Bus services, completions
.circleci/ .ci/      CI config and scripts (Linux distros, sanitizers, scan-build)
docs/                architecture, Windows port plan, platform audit, upstream-sync rules
```

Existing fork-only directories: `src/platform/windows/`, `cmake/`, `docs/`. Planned (do not create unless the task asks): `src/platform/linux/`, `packaging/windows/`.

Deeper reading: `docs/windows-build.md` (cross build, native execution and verification), `docs/architecture.md` (how it works today), `docs/windows-port.md` (seams, phases, open decisions), `docs/platform-dependencies.md` (every Linux-only call, with file:line), `docs/upstream-sync.md` (merge rules).

## Dev environment

Tools come from `flake.nix`, so nothing is installed on the host. With direnv (`.envrc` is `use flake`) run `direnv allow` once; otherwise `nix develop`. Shells: `default` (native Linux build and tests: gcc/clang, cmake, ninja, all pkg-config deps, gdb, valgrind, dbus, Xvfb) and `windows` (`nix develop .#windows`, **builds the core and otpclient-cli for x86_64 UCRT**: MinGW-w64 cross toolchain, cross-built glib/libgcrypt/jansson/libcotp/gettext, Wine; the cross packages are largely not in the binary cache and build from source). `flake.nix`, `flake.lock` and `.envrc` are fork-only files. Do not add tools to the host or to upstream files.

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

## Build and test (Windows core + CLI)

From the repository root:

```sh
nix build .#devShells.x86_64-linux.windows --no-link
nix develop .#windows
cmake -S . -B /tmp/otp-win-build -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$PWD/cmake/windows-ucrt.cmake" \
  -DOTP_WINDOWS_GETTEXT_ROOT="$OTP_WINDOWS_GETTEXT_ROOT" \
  -DBUILD_GUI=OFF -DBUILD_SEARCH_PROVIDER=OFF -DBUILD_TESTING=OFF \
  -DOTP_WINDOWS_SHIM_CHECKS=ON
cmake --build /tmp/otp-win-build -j8
```

- Scope is x64 first. Architecture/compiler selection belongs in the toolchain file, never in C sources; the stat ABI shim follows `_WIN64`. A 32-bit build is unverified.
- `BUILD_TESTING=OFF` disables the upstream ctest suite. `OTP_WINDOWS_SHIM_CHECKS=ON` builds separate shim and console regression executables; run them explicitly.
- Stage the cross runtime DLLs beside the executables (recipe and verification matrix in `docs/windows-build.md`). From WSL, copy to a Windows directory and run the cross-built `.exe` natively through interop; use PowerShell for interactive console tests. Wine is only a quick headless check, not proof of native console or ACL behaviour.
- GUI strategy is decided (2026-10-12, see `docs/windows-port.md`): GUI phase 1 ports GTK4/libadwaita to Windows; GUI phase 2 extracts a UI-agnostic core later. Secret storage, crash-dump prevention, real memlock sizing and directory durability are incomplete; see the `WINDOWS-STUB` markers.

## Code conventions

- Match the surrounding C style (GLib types and `g_autoptr`/`g_autofree`, 4-space indent, space before call parentheses: `g_free (x)`).
- Secrets: password and key material go in libgcrypt secure memory (`gcry_malloc_secure`, `secure_strdup`) and are freed with the `sensitive_*` helpers in `common.c`. Never log secrets; never copy them into ordinary heap buffers.
- The DB format is versioned (`DB_VERSION` 3). Never change the on-disk format as a side effect of porting.
- Do not add dependencies without checking that they exist on Windows.

## Upstream-sync rules (summary)

- Remotes: `origin` = this fork, `upstream` = paolostivanin/OTPClient. Sync with `git merge upstream/master`.
- **Do not move, rename or reformat upstream files.** No drive-by cleanup, no clang-format passes.
- Fork-only code goes in new directories. Edits to upstream files are one-liners that route into `src/platform/`; no Windows logic blocks, no `#ifdef` sprawl in the churn hotspots (`otpclient-window.c`, `otpclient-application.c`, `CMakeLists.txt`, `db-common.c`, `common.c`, `search-provider.c`, `exec-action.c`, `README.md`).
- Tiny routing edits (one line into `src/platform/`) are allowed. Each one must be listed in `docs/upstream-sync.md`.
- Anything not Windows-specific that upstream would accept is a candidate for an upstream PR instead.

## Windows-port rules (summary)

- Seams are either **shims** (compat headers / a libsecret replacement, no call-site edits) or **ports** (small interfaces: secrets, settings, paths, autostart, tray, session lock, hardening). See `docs/windows-port.md`.
- Compat headers and forced include redirects satisfy existing calls without editing upstream call sites. Keep shims under `src/platform/windows/compat/` and implementations under `src/platform/windows/`; mark incomplete semantics with `WINDOWS-STUB`, never silent success that claims a security guarantee.
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
