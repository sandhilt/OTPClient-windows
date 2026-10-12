# Windows port plan

Status: **plan only, no Windows code written yet.** Every Windows API named here is a hypothesis to verify when the code is written, not a decision.

## Goals

1. Build and run OTPClient on Windows.
2. Isolate everything graphical and OS-specific behind small interfaces, in the spirit of clean architecture: the core (database, crypto, importers, OTP) must not know which UI or OS it runs under.
3. Keep merging upstream cheap. See `docs/upstream-sync.md`; its rules take priority over tidiness.

## Current shape (why this is not a one-day job)

See `docs/architecture.md`. The short version:

- `src/common` already has no GTK, but it is not portable: `prctl`, `getrlimit/setrlimit`, `flock`, `umask`, `/proc/self/fd/N`, `libuuid`, `libsecret`, GSettings.
- `src/gui` has no UI/domain boundary. `otpclient-window.c` (5443 lines) and `otpclient-application.c` (2245 lines) call `db-common`, libsecret, GSettings and gcrypt directly.
- Tray, autostart and screen-lock detection are Linux desktop services (StatusNotifier over D-Bus, XDG `autostart/*.desktop` files with a Flatpak-only Background portal, `org.*.ScreenSaver` and logind over D-Bus).
- `src/search-provider` is GNOME Shell / KRunner only.

## Strategy: two kinds of seam

**A. Shims (zero edits to upstream call sites).** For POSIX and library APIs that the code already calls, provide Windows-only replacements that satisfy the *existing* include and symbol:

- compat headers placed first on the include path for Windows builds: `<sys/file.h>` (`flock` → `LockFileEx`), `<sys/prctl.h>`, `<sys/resource.h>`, `<uuid/uuid.h>`, `<termios.h>`;
- a Windows implementation of the small `libsecret` surface the code uses (`secret_password_*`, `SecretSchema`) backed by Windows Credential Manager or DPAPI.

Lowest merge friction, because upstream call sites do not change. Risk: shims hide semantic differences (see "Hard problems"), so each one needs tests.

**B. Ports (explicit interfaces) where behaviour genuinely differs.** For things that do not map onto a POSIX call: tray icon, autostart, session lock/unlock, settings storage, per-user paths. Define a small C interface per concern in `src/platform/`, with a Linux implementation that wraps the code upstream already has and a Windows implementation. Upstream call sites change by a line or two to call the interface.

## Proposed layout (all new, fork-only)

```
src/platform/
  platform.h              umbrella header
  secrets.h  settings.h  paths.h  autostart.h  tray.h  session-lock.h  hardening.h
  linux/                  thin wrappers over today's code, no behaviour change
  windows/                Windows implementations
  windows/compat/         shim headers (strategy A)
cmake/
  otpclient-core.cmake    builds the common sources as a static library without moving them
  windows.cmake           toolchain, dependency lookup, packaging
packaging/windows/        installer, manifest, icons
```

Nothing under `src/common`, `src/gui` or `src/cli` moves. Linux builds keep using the existing source globs. Only the Windows path consumes the `otpclient-core` target.

## Seams (initial list, verify before implementing)

| Concern | Today | Windows candidate | Kind |
|---|---|---|---|
| Process hardening | `prctl(PR_SET_DUMPABLE)`, `RLIMIT_CORE` in `common.c` | no-op or `SetErrorMode`/WER settings | B (`hardening.h`) |
| Secure memory sizing | `RLIMIT_MEMLOCK` → libgcrypt pool in `common.c` | libgcrypt secure memory on Windows; working-set / `VirtualLock` limits | B |
| DB file lock | `flock(LOCK_EX\|LOCK_NB)` in `db-common.c` | `LockFileEx` | A |
| Permissions | `umask(0077)`, `chmod 0600` | per-user profile ACLs | B or accept no-op |
| Atomic write | temp + `fsync` + `rename` + dir `fsync` | `MoveFileExW(REPLACE_EXISTING\|WRITE_THROUGH)` / `ReplaceFileW` | B |
| Importers' `/proc/self/fd/N` re-open | `aegis.c`, `authpro.c`, `twofas.c`, `freeotp.c` | no equivalent; needs a different "open once, read from the fd" path | B (invasive; four files) |
| UUID | `libuuid` in `aegis.c` | `g_uuid_string_random()` (GLib ≥ 2.52) | A |
| Secret storage | libsecret | Credential Manager / DPAPI | A |
| Settings | GSettings | needs a verified Windows backend, else key-file / registry | B (`settings.h`) |
| Config/data dirs | `g_get_user_*_dir()` | same GLib calls work on Windows (`%APPDATA%`) | none |
| CLI password prompt | `termios`, `isatty`, `sigaction` | Win32 console mode (`SetConsoleMode`) | A or B |
| Tray | StatusNotifierItem over GDBus | `Shell_NotifyIcon` | B (`tray.h`) |
| Autostart | XDG `autostart/*.desktop` (Background portal on Flatpak) | `HKCU\...\Run` | B (`autostart.h`) |
| Session lock | `org.*.ScreenSaver` D-Bus | `WTSRegisterSessionNotification` | B (`session-lock.h`) |
| Webcam QR scan | zbar V4L | zbar has no V4L on Windows; needs another capture path | B, later |
| Search provider | GNOME / KRunner D-Bus | not applicable; do not build on Windows | none |
| Flatpak / portals | `IS_FLATPAK` | not applicable | none |

The exhaustive, file-and-line version of this table is `docs/platform-dependencies.md`.

## Hard problems (decide early)

- **Secure memory.** libgcrypt secure memory depends on being able to lock pages. Whether the Windows build gives the same guarantees, and how `max_file_size_from_memlock` should be derived, needs a spike.
- **`/proc/self/fd` importers.** This is the one seam that needs real code changes in four upstream files, not a shim.
- **GSettings on Windows.** The GSettings API exists in GLib, but which backend is available (registry, key file, memory) and how the schema is compiled and installed must be checked against the actual MSYS2 packages before relying on it.
- **32-bit `long` timestamps.** libcotp takes `long`; on 64-bit Windows `long` is 32 bits, so TOTP generation has a 2038 limit unless libcotp's API is handled (`cli/get-data.c:198-206`, `gui/otp-entry.c:489-576`).
- **Argon2id memory cost.** Defaults are 128 MiB with parallelism 4; confirm behaviour inside libgcrypt's secure-memory pool on Windows.
- **Dependencies.** libcotp is built from source in CI (together with libbaseencode, which the build itself does not reference). A search of the MSYS2 package index (2026-10-12) found no libcotp; the only "cotp" hit is `mingw-w64-ucrt-x86_64-cotp` 1.10.0, which is a different project and not libcotp (not inspected further). Plan to build libcotp from source on Windows. The rest is packaged, see below.

## Is GTK4 / libadwaita viable on Windows? (researched 2026-10-12)

Short answer: **yes, it is a supported-in-practice path, with caveats.** Evidence, from the MSYS2 package pages (`packages.msys2.org`), UCRT64 repo:

| Dependency (required by `CMakeLists.txt`) | MSYS2 UCRT64 package | Version found |
|---|---|---|
| gtk4 ≥ 4.10 | `mingw-w64-ucrt-x86_64-gtk4` | 4.22.4 |
| libadwaita ≥ 1.5 | `mingw-w64-ucrt-x86_64-libadwaita` | 1.10.0-2 |
| libgcrypt ≥ 1.10.1 | `mingw-w64-ucrt-x86_64-libgcrypt` | 1.12.4 |
| jansson ≥ 2.13 | `mingw-w64-ucrt-x86_64-jansson` | 2.15.1 |
| libsecret ≥ 0.20 | `mingw-w64-ucrt-x86_64-libsecret` | 0.21.7 (package exists; whether it is usable without a Secret Service on Windows was not checked, and the plan does not rely on it) |
| zbar ≥ 0.20 | `mingw-w64-ucrt-x86_64-zbar` | 0.23.93 (webcam capture on Windows not checked) |
| libprotobuf-c ≥ 1.3 | `mingw-w64-ucrt-x86_64-protobuf-c` | 1.5.2 |
| libqrencode ≥ 4.0 | `mingw-w64-ucrt-x86_64-qrencode` | 4.1.1 |
| libcotp ≥ 4.0 | not found | build from source |
| glib / gio ≥ 2.74, gdk-pixbuf | not looked up individually; both are dependencies of gtk4 | |

Other sources:

- The official GTK guide documents installing GTK4 on Windows with MSYS2 (`pacman -S mingw-w64-ucrt-x86_64-gtk4`): <https://gtk.org/docs/installations/windows/>.
- The gtk-rs book documents two routes for libadwaita on Windows: `gvsbuild build libadwaita librsvg` on top of a gvsbuild GTK4, or a manual MSVC + Meson build: <https://gtk-rs.org/gtk4-rs/git/book/libadwaita.html>. It does not mention MSYS2, and its manual-build example pins an old libadwaita branch, so treat it as a fallback route.
- A GNOME Discourse thread has developers reporting libadwaita apps running on Windows "relatively well", with caveats: GPU-driver-related instability, very large window shadows that can block clicks, and no native window snapping or tiling. Regressions may also go unnoticed because GTK developers mainly use Linux: <https://discourse.gnome.org/t/using-libadwaita-on-linux-windows-apps/29181>.
- GSettings on Windows: GLib ships a registry backend (`GRegistrySettingsBackend`, default root `HKEY_CURRENT_USER\Software\GSettings`; a custom root since GLib 2.78). Reads come from an in-memory cache, and change notification needs its watch thread. There is a documented key-length limit (255 characters per key, 16,383 per value). Reference: <https://docs.gtk.org/gio-win32/func.registry_settings_backend_new.html>. Not yet tested with this project's schema; the schema must still be compiled and shipped with the installer.

Consequences for the plan:

- Option 1 (GTK4 stays) is technically feasible at the dependency level; the open questions are quality (look and feel, GPU quirks) and the missing OS integrations (tray, autostart, lock), not whether the libraries exist.
- A Phase 0 spike should confirm by actually building: `pacman -S` the packages above in an MSYS2 UCRT64 shell and build libcotp. Configuring with `-DBUILD_SEARCH_PROVIDER=OFF` is necessary but not sufficient: `uuid` and `libsecret` remain unconditional `pkg_check_modules` requirements (`CMakeLists.txt:107-108`) and `src/common` still needs Unix headers and APIs (`common.c:5-9`). The Phase 1 shims have to land before the core configures and compiles.

## Open decision: what "isolate the GUI" means

This is for the project owner to decide. Both options share Phases 0–2 below, so the decision can wait until Phase 3.

**Option 1 — keep GTK4 / libadwaita, port it to Windows.**
- Pros: the GUI stays one code base shared with upstream, so most upstream GUI changes still merge; far less new code.
- Cons: not a native look; GTK4 + libadwaita on Windows must be verified in the chosen toolchain; the missing UI/domain boundary stays, so the "clean architecture" gain is limited to the OS seams; the churn hotspots (`otpclient-window.c`, `otpclient-application.c`) need edits wherever they touch GSettings, tray, lock or Unix signals. libsecret call sites should not need edits if the strategy-A shim is used.

**Option 2 — extract a UI-agnostic core, build a separate native Windows UI on top.**
- Pros: real separation; the core is testable without any UI; the GUI can be replaced.
- Cons: a whole new UI to build and maintain; the existing GUI cannot be reused, so the fork stops benefiting from upstream GUI work; the logic currently embedded in `otpclient-window.c` and the dialogs has to be redefined as a core facade, which is where the upstream-merge cost lives.

Recommendation: do Phases 0–2 (they pay off either way), then pick with a working Windows CLI and core in hand.

## Phases

0. **Toolchain spike.** Choose MSYS2 UCRT64 (GCC/Clang, closest to upstream's flags) or MSVC. Install glib, libgcrypt, jansson and the other dependencies; build libcotp. Output: a list of what installs and what does not.
1. **Core builds on Windows.** `cmake/otpclient-core.cmake`, compat headers, `hardening.h`, `paths.h`, file lock and atomic write. Goal: the pure-logic tests (listed in `docs/architecture.md`) pass on Windows.
2. **CLI on Windows.** Console password prompt, secret storage, settings. Goal: the smoke test (`.ci/smoke-test.sh`) equivalent passes.
3. **GUI decision and first window.** Per the open decision.
4. **Desktop integration.** Tray, autostart, session lock, single instance.
5. **Packaging and CI.** Installer, signing, a Windows CI job next to CircleCI, a documented sync routine.

## Out of scope for the Windows build

Flatpak, the D-Bus search provider and KRunner/GNOME Shell metadata, and `data/dbus-1/`.
