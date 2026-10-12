# Windows CLI cross-build attempt (2026-10-12)

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

## Update from the manager (same day)

Codex's sandbox cannot reach the Nix daemon, so it could not build anything. The
manager reran `nix build .#devShells.x86_64-linux.windows --no-link` outside that
sandbox. With the Jansson `postFixup` override the Jansson failure is gone, and
the next cross-build failure is libgcrypt 1.12.4 for `x86_64-w64-mingw32`:
`x86_64-w64-mingw32-as: unrecognized option '--noexecstack'` while assembling
`mpi/*-asm.S` (`-Wa,--noexecstack` is passed by libgcrypt's build). Nix-level
builds therefore stay with the manager; Codex edits sources.
