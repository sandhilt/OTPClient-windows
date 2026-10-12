# Windows CLI cross-build (2026-10-12)

## Current result: core + CLI linked

The exported Windows environment produced
`/tmp/otp-win-build/src/cli/otpclient-cli.exe`, a `pei-x86-64` Windows console
executable using UCRT, with all `src/common/*.c` compiled into the CLI. The final
build had no compiler/linker warnings. No upstream source/build files,
`flake.nix`, or database format were changed. The architecture is selected only
in `cmake/windows-ucrt.cmake`.

Added files: `cmake/windows-ucrt.cmake`, `cmake/windows.cmake`,
`cmake/windows/pkgconfig/{uuid,libsecret-1}.pc`,
`src/platform/windows/compat.c`, and headers under
`src/platform/windows/compat/`: `otp-windows.h`, `sys/{prctl,resource,file,stat}.h`,
`termios.h`, `uuid/uuid.h`, `libsecret/secret.h`.

The toolchain loads compatibility only for Windows OTPClient builds (not CMake's
compiler probes). A deferred hook attaches the implementation/header to the CLI
and removes PIE and glibc FORTIFY options. Fake pkg-config metadata satisfies the
existing unconditional UUID/libsecret checks; it does not provide those libraries.
The layer uses `SecureZeroMemory`, UTF-16 handle-based no-follow reads,
`LockFileEx`, binary CRT descriptors, console echo modes and GLib UUIDs. Importer
fd paths are transport tokens: JSON callbacks and a `GFileInputStream` adapter
read the checked descriptor without reopening a filesystem path. URI contents
use libgcrypt secure memory; the Windows free adapter preserves allocator ownership.
These Windows implementations compiled, but their runtime semantics are unverified.

### Exact build commands used

Run in Bash from the repository root. Source each environment in the same shell
as its commands. No Nix command was run for this attempt.

```sh
source /tmp/claude-1000/-home-sandhilt-workspace-OTPClient-windows/42d068b6-91af-47fd-bf85-ce9c99ea1974/scratchpad/win-env.sh
export PKG_CONFIG_PATH=/nix/store/k9ybcll1npa94kppyyn11zg817a5n810-libgpg-error-x86_64-w64-mingw32-1.61-dev/lib/pkgconfig:/nix/store/634njvdl9j4y0781l28r40bcqwfbhr0w-pcre2-x86_64-w64-mingw32-10.48-dev/lib/pkgconfig:$PKG_CONFIG_PATH
cmake -S . -B /tmp/otp-win-build -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$PWD/cmake/windows-ucrt.cmake" \
  -DOTP_WINDOWS_GETTEXT_ROOT=/nix/store/ywigw24851mjsyanpykx4ps3c3i5nclb-gettext-x86_64-w64-mingw32-1.0 \
  -DBUILD_GUI=OFF -DBUILD_SEARCH_PROVIDER=OFF -DBUILD_TESTING=OFF
cmake --build /tmp/otp-win-build -j8
```

The exported shell omits gettext from its direct build inputs and the pkg-config
directories for cross `gpg-error` and `libpcre2-8`. These packages already exist
in the store, so the explicit paths above were sufficient; no download was needed.
Suggested manager change to the Windows shell only: add
`windowsPkgs.gettext`, `windowsPkgs.libgpg-error.dev`, and `windowsPkgs.pcre2.dev`
to its build inputs. Supply the cross gettext prefix to CMake. Do not add native
libraries to cross lookup. The missing optional `bash-completion.pc` message is
non-fatal; upstream falls back to a relative installation directory.

### Stubs and incomplete security properties

Every stub has a `/* WINDOWS-STUB: ... */` marker in the new platform code:

- Secret Service always reports `G_IO_ERROR_NOT_SUPPORTED`; async calls use
  GTask and cancellation. No credentials are stored, no native backend selected.
- `prctl(PR_SET_DUMPABLE)` and `setrlimit` fail with `ENOSYS`, causing the existing
  crash-dump warnings. Crash dumps are not prevented.
- `getrlimit(RLIMIT_MEMLOCK)` supplies a fixed 64 MiB budget, not a measured
  Windows/page-lock guarantee. Upstream still chooses its normal pool size;
  libgcrypt secure allocation remains in use.
- `umask`/`g_chmod` are no-ops; private Windows DACL creation is not implemented.
  The unchanged password-file mode check is not a DACL check and may reject
  Windows files. It is not bypassed.
- File `fsync` maps to `_commit`; parent-directory opening/flushing is unsupported
  and upstream warns. Crash-safe replacement/durability is unverified.
- Signal handling maps to CRT `signal`, with `SIGHUP` mapped to `SIGBREAK`;
  POSIX masks and console-control cleanup are not implemented. Console echo
  mode changes exist, but interactive cancellation is unverified.

The lock shim is implemented, not stubbed. Same-process exclusion and other
Windows-specific security/atomicity properties require runtime validation.
This is a compilation spike, not a production-ready Windows port.

### Linux regression and Wine

```sh
source /tmp/claude-1000/-home-sandhilt-workspace-OTPClient-windows/42d068b6-91af-47fd-bf85-ce9c99ea1974/scratchpad/linux-env.sh
cmake -S . -B /tmp/otp-linux-build -G Ninja && \
  cmake --build /tmp/otp-linux-build -j8 && \
  ctest --test-dir /tmp/otp-linux-build --output-on-failure
```

Linux GUI, CLI, search provider and tests compiled. All 36 tests were registered:
21 passed; 15 failed before their cases could execute because private D-Bus socket
creation was denied (`Failed to bind socket ...: Operation not permitted` in
`tests/isolated-session.py:45`). **36/36 is not confirmed.** The final check must
be rerun outside this socket-restricted sandbox.

```sh
source /tmp/claude-1000/-home-sandhilt-workspace-OTPClient-windows/42d068b6-91af-47fd-bf85-ce9c99ea1974/scratchpad/win-env.sh
mkdir -p /tmp/otp-wineprefix
WINEPREFIX=/tmp/otp-wineprefix WINEDEBUG=-all \
  wine /tmp/otp-win-build/src/cli/otpclient-cli.exe --version
```

Wine stopped before loading the executable: `sock_init: ERROR in
sock_check_pollhup()` / `wineserver: bind: Operation not permitted`. Therefore
`--version`, `--help` and import/list are unverified. Outside the sandbox, stage
the cross runtime DLLs beside the executable before running the smoke flow.
Build/test logs from this attempt are `/tmp/otp-win-{configure,build}.log` and
`/tmp/otp-linux-{configure,build,test}.log`. No commits or Git branch operations
were performed.

## Earlier attempt (superseded by the result above)

Status: **blocked before shell entry; no Windows executable linked and no
Linux test result obtained.** This is not a completed port.

`flake.nix` extends only `pkgsCross.ucrt64`, overriding Jansson's `postFixup`
with an empty string. The original fixup rewrites an ELF library reference in
`dev/lib/cmake/jansson/janssonTargets-release.cmake`. The Windows installation
instead has `out/cmake/janssonTargets-release.cmake`, DLLs in `out/bin`, and a
pkg-config file referencing `out/lib` and `dev/include`. No nixpkgs files or
upstream application files were changed. No dependencies were added.

## Commands attempted

Run from the repository root. The cache override directs cache writes to `/tmp`;
the default cache in `$HOME` was not writable in this session.

```sh
export XDG_CACHE_HOME=/tmp/otp-nix-cache
nix build .#devShells.x86_64-linux.windows --no-link
nix develop .#windows --command bash -c 'printf "%s\n" "$CC"; pkg-config --modversion glib-2.0 libgcrypt cotp jansson gettext-runtime'
nix develop .#windows --command bash -c 'cmake -S . -B /tmp/otp-win-build -G Ninja -DCMAKE_SYSTEM_NAME=Windows -DCMAKE_C_COMPILER=x86_64-w64-mingw32-gcc -DBUILD_GUI=OFF -DBUILD_SEARCH_PROVIDER=OFF -DBUILD_TESTING=OFF && cmake --build /tmp/otp-win-build -j8'
nix develop .#default --command bash -c 'cmake -B /tmp/otp-linux-build -G Ninja && cmake --build /tmp/otp-linux-build -j8 && ctest --test-dir /tmp/otp-linux-build'
```

All four Nix commands exited with:

```text
error: cannot connect to socket at '/nix/var/nix/daemon-socket/socket': Operation not permitted
```

The embedded CMake/build/test commands never started. The Windows configure
command above is the attempted compiler selection, not a working recipe: it
does not yet supply the required compat headers or pkg-config shims/toolchain.
GLib, libgcrypt, libcotp, Jansson and gettext build success remains unverified.

## Checks and remaining work

`nix-instantiate --parse flake.nix` passed. Read-only evaluation with
`nix-instantiate --store dummy:// --eval --strict` and an already available local
nixpkgs source confirmed that the shell selects cross GLib/libgcrypt/libcotp and
Jansson with an empty `postFixup`. This is evaluation, not a dependency build.
The same local evaluation confirmed `hostPlatform.libc == "ucrt"` and identical
native Linux shell derivation paths before and after the override. It did not
verify the locked-flake build or run Linux tests.

The first blocker is daemon access. Once that is available, rerun the shell
build before implementing the compat layer. Static source blockers still include
unconditional `uuid`/`libsecret-1` checks (`CMakeLists.txt:107-108`), Unix headers
and hardening (`src/common/common.c:5-9,100-130`), importer fd paths
(`src/common/aegis.c:102`, `authpro.c:64`, `twofas.c:89`, `freeotp.c:46`), and
the POSIX console prompt (`src/cli/exec-action.c:511-559`). These were inspected
in source; they are not compiler diagnostics from this attempt.

Stub list: **none implemented**. No security behaviour or DB format was changed.
No upstream files were edited. The required Linux 36/36 result remains pending;
the denied shell invocation is not a test pass. Wine was not attempted.

## Update from the manager (same day): the cross shell builds

Codex's sandbox cannot reach the Nix daemon, so it could not build anything. The
manager reran the builds outside that sandbox and fixed the cross dependencies in
`flake.nix` (all in the `windowsPkgs` overrides, no nixpkgs files touched):

| Package | Failure for `x86_64-w64-mingw32` | Override |
|---|---|---|
| jansson 2.15 | nixpkgs `postFixup` expects `dev/lib/cmake/jansson/...` | empty `postFixup` |
| libgcrypt 1.12.4 | `as: unrecognized option '--noexecstack'` while assembling `mpi/*-asm.S` | `--disable-asm` (C fallbacks; slower bignum math, which this app barely uses) |
| libcotp 4.1.0 | `ld: unrecognized option '-z'`: its CMake passes ELF-only `-z relro/noexecstack` unless `APPLE` | `if(NOT APPLE)` -> `if(NOT APPLE AND NOT WIN32)` (upstream candidate for libcotp) |

`nix build .#devShells.x86_64-linux.windows --no-link` now succeeds. Inside
`nix develop .#windows`: `x86_64-w64-mingw32-gcc` 16.2.0, glib 2.88.3, libgcrypt
1.12.4, cotp 4.1.0, jansson 2.15.0 and `wine` are available. Still **not** done:
no OTPClient source has been compiled for Windows yet.
