# Windows port plan

Status: **core + CLI build for x86_64 UCRT**, with native Windows 11 CLI, ACL/rename and console checks recorded (see `docs/windows-build.md`). The GUI strategy is decided (two GUI phases, see [Decisions](#decisions-2026-10-12)); the GUI port has not started. Implemented shims are identified below; unimplemented Windows API candidates remain hypotheses, not decisions.

## Goals

1. Build and run OTPClient on Windows.
2. Isolate everything graphical and OS-specific behind small interfaces, in the spirit of clean architecture: the core (database, crypto, importers, OTP) must not know which UI or OS it runs under.
3. Keep merging upstream cheap. See `docs/upstream-sync.md`; its rules take priority over tidiness.

## Current shape (why this is not a one-day job)

See `docs/architecture.md`. The short version:

- `src/common` already has no GTK, but its upstream sources still depend on `prctl`, `getrlimit/setrlimit`, `flock`, `umask`, `/proc/self/fd/N`, `libuuid`, `libsecret`, GSettings. The CLI now builds through Windows shims without call-site edits; not all Linux semantics are implemented.
- `src/gui` has no UI/domain boundary. `otpclient-window.c` (5443 lines) and `otpclient-application.c` (2245 lines) call `db-common`, libsecret, GSettings and gcrypt directly.
- Tray, autostart and screen-lock detection are Linux desktop services (StatusNotifier over D-Bus, XDG `autostart/*.desktop` files with a Flatpak-only Background portal, `org.*.ScreenSaver` and logind over D-Bus).
- `src/search-provider` is GNOME Shell / KRunner only.

## Strategy: two kinds of seam

**A. Shims (zero edits to upstream call sites).** For POSIX and library APIs that the code already calls, provide Windows-only replacements that satisfy the *existing* include and symbol:

- compat headers placed first on the include path for Windows builds: `<sys/file.h>` (`flock` → `LockFileEx`), `<sys/prctl.h>`, `<sys/resource.h>`, `<uuid/uuid.h>`, `<termios.h>`;
- a Windows implementation of the small `libsecret` surface the code uses (`secret_password_*`, `SecretSchema`) currently an explicit service-unavailable stub; Credential Manager or DPAPI remains a future candidate.

Lowest merge friction, because upstream call sites do not change. Risk: shims hide semantic differences (see "Hard problems"), so each one needs tests.

**B. Ports (explicit interfaces) where behaviour genuinely differs.** For things that do not map onto a POSIX call: tray icon, autostart, session lock/unlock, settings storage, per-user paths. Define a small C interface per concern in `src/platform/`, with a Linux implementation that wraps the code upstream already has and a Windows implementation. Upstream call sites change by a line or two to call the interface.

## Layout (fork-only; existing and planned)

```
src/platform/
  platform.h              planned umbrella header
  secrets.h  settings.h  paths.h  autostart.h  tray.h  session-lock.h  hardening.h (planned)
  linux/                  planned thin wrappers over today's code, no behaviour change
  windows/                compat.c, console.c, security.c/.h, private-file.c, shim-check.c, console-check.c
  windows/compat/         existing shim headers (strategy A)
cmake/
  windows-ucrt.cmake      existing x86_64 UCRT cross toolchain
  windows.cmake           existing deferred CLI compatibility wiring
  windows/pkgconfig/     existing uuid/libsecret-1 metadata shims
packaging/windows/        planned installer, manifest, icons
```

Nothing under `src/common`, `src/gui` or `src/cli` moves. Linux and Windows builds keep using the existing source globs. There is no `otpclient-core` target or `cmake/otpclient-core.cmake`: the Windows hook attaches shims to the upstream CLI target, which compiles all common sources. Architecture selection lives in the toolchain file, not in C sources; scope is x64 first, with the stat ABI shim selected by `_WIN64`.

## Seams (implemented shims and remaining candidates)

| Concern | Today | Windows candidate | Kind |
|---|---|---|---|
| Process hardening | `prctl(PR_SET_DUMPABLE)`, `RLIMIT_CORE` in `common.c` | A stub: returns `ENOSYS`, upstream warns; dump prevention is not implemented | A now; policy OPEN |
| Secure memory sizing | `RLIMIT_MEMLOCK` → libgcrypt pool in `common.c` | A stub: fixed 64 MiB budget; libgcrypt remains the secure allocator, page-lock guarantees unverified | A now; policy OPEN |
| DB file lock | `flock(LOCK_EX\|LOCK_NB)` in `db-common.c` | `LockFileEx`/`UnlockFileEx` implemented; contention → `EWOULDBLOCK`, unsupported operations → `ENOSYS`; exclusion/fallback still needs runtime tests | A |
| Permissions | `umask(0077)`, `chmod 0600` | no-op stubs; private GIO exports create/verify a protected user-only DACL, password files get a conservative DACL mode audit; DB/lock/temp ACLs still inherited | A (partial) |
| Atomic write | temp + `fsync` + `rename` + dir `fsync` | DB keeps upstream temp + `g_rename`; file `fsync` → `_commit`, directory open/flush unsupported. Private exports use held-handle `FileRenameInfo` + `FlushFileBuffers`; crash durability unverified | A (partial) |
| Importers' `/proc/self/fd/N` re-open | `aegis.c`, `authpro.c`, `twofas.c`, `freeotp.c` | existing spelling is a transport token; JSON callbacks and seekable GIO stream adapters read the checked descriptor without path reopening | A (implemented, no upstream edits) |
| UUID | `libuuid` in `aegis.c` | GLib random UUID converted to binary, lowercase formatter implemented | A |
| Secret storage | libsecret | `G_IO_ERROR_NOT_SUPPORTED` stub with GTask async/cancellation flow; stays unavailable through milestone 1; Credential Manager / DPAPI to be decided afterwards (decision 5) | A |
| Settings | GSettings | GLib registry backend first (untested); keyfile backend considered for the portable zip (decision 8) | B (`settings.h`) |
| Config/data dirs | `g_get_user_*_dir()` | same GLib calls work on Windows (`%APPDATA%`) | none |
| CLI password prompt | `termios`, `isatty`, `sigaction` | console modes and event-driven secure UTF-8 line input implemented; native generated Ctrl+C/Ctrl+Break pass, true SIGHUP/SIGTERM interruption unsupported | A |
| Tray | StatusNotifierItem over GDBus | `Shell_NotifyIcon` | B (`tray.h`) |
| Autostart | XDG `autostart/*.desktop` (Background portal on Flatpak) | `HKCU\...\Run` | B (`autostart.h`) |
| Session lock | `org.*.ScreenSaver` D-Bus | `WTSRegisterSessionNotification` | B (`session-lock.h`) |
| Webcam QR scan | zbar V4L | zbar has no V4L on Windows; needs another capture path | B; out until requested (decision 9) |
| Search provider | GNOME / KRunner D-Bus | not applicable; do not build on Windows | none |
| Flatpak / portals | `IS_FLATPAK` | not applicable | none |

The exhaustive, file-and-line version of this table is `docs/platform-dependencies.md`.

## Hard problems (decide early)

- **Secure memory.** libgcrypt secure memory depends on being able to lock pages. Whether the Windows build gives the same guarantees, and how `max_file_size_from_memlock` should be derived, needs a spike.
- **`/proc/self/fd` importers.** Resolved for the CLI by fd-backed parser shims, including seek/tell and initialized JSON errors. Full encrypted Authenticator Pro round-trip still needs runtime verification.
- **GSettings on Windows.** The GSettings API exists in GLib, but which backend is available (registry, key file, memory) and how the schema is compiled and installed must be checked against the chosen Windows GLib packages before relying on it.
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
- The core + CLI toolchain spike is complete through the Nix MinGW-w64 UCRT cross shell, not MSYS2 or MSVC. The Windows hook supplies metadata for the unconditional `uuid`/`libsecret` checks (`CMakeLists.txt:107-108`) and compat headers for Unix APIs. A GTK4/libadwaita build still needs a separate dependency/window spike; package availability alone does not settle the GUI decision.

## GUI decision: what "isolate the GUI" means (decided 2026-10-12)

**Decided: two GUI phases.** The project owner chose Option 1 for GUI phase 1. GUI phase 2 extracts a UI-agnostic core later, which is the first half of Option 2; whether a separate native UI is built on top is not decided. "GUI phase 1/2" is not the numbering of the Phases below. Both options share Phases 0–2 below. See Decision 2 in [Decisions](#decisions-2026-10-12).

**Option 1 — keep GTK4 / libadwaita, port it to Windows (GUI phase 1).**
- Pros: the GUI stays one code base shared with upstream, so most upstream GUI changes still merge; far less new code.
- Cons: not a native look; GTK4 + libadwaita on Windows must be verified in the MSYS2 UCRT64 toolchain (not built yet); the missing UI/domain boundary stays, so the "clean architecture" gain is limited to the OS seams; the churn hotspots (`otpclient-window.c`, `otpclient-application.c`) need edits wherever they touch GSettings, tray, lock or Unix signals. libsecret call sites should not need edits if the strategy-A shim is used.

**Option 2 — extract a UI-agnostic core (GUI phase 2); a separate native Windows UI on top is not decided.**
- Pros: real separation; the core is testable without any UI; the GUI can be replaced.
- Cons: a whole new UI to build and maintain; the existing GUI cannot be reused, so the fork stops benefiting from upstream GUI work; the logic currently embedded in `otpclient-window.c` and the dialogs has to be redefined as a core facade, which is where the upstream-merge cost lives.

Earlier recommendation, still the order of work: finish the remaining Phase 2 verification/security work before the GUI spike. The owner's decision was made with the working Windows CLI and core in hand.

## Decisions (2026-10-12)

Recorded from the project owner. "Decided" means the choice is made; "not started" means the work it implies has not begun. Item 2 uses "GUI phase" for a separate sequence from the numbered Phases below.

| # | Topic | Decision | Status |
|---|---|---|---|
| 1 | Finish line | Milestone 1: functional GUI on Windows without desktop integration (tray, autostart, session lock). Final goal: GUI with feature parity plus an installer. The Windows CLI and core already work (`docs/windows-build.md`). | decided |
| 2 | GUI strategy | Two GUI phases. GUI phase 1 ports GTK4 / libadwaita to Windows, with dependencies from a portable MSYS2 UCRT64 tree built under a throwaway folder (no installer, no PATH changes). The Nix cross shell stays for core and CLI: in nixpkgs, libadwaita, gdk-pixbuf and zbar are marked unix-only, and a gtk4 cross build pulls in X11 and GStreamer. That was read from nixpkgs, not proven by a build. GUI phase 2 extracts a UI-agnostic core later. New port interfaces use plain C types at the seams. | decided; not started |
| 3 | Upstream edits | One-line edits that route into `src/platform/` are allowed and must be listed in `docs/upstream-sync.md`. First candidate: the `-p` refusal message in `src/cli/exec-action.c`. | decided; edit not made |
| 4 | Publishing | Feature branches, pull requests inside the fork for review, then merge to the fork's `master`. No upstream PRs for now. Nothing is pushed until the owner asks. | decided |
| 5 | Secret storage on Windows | The libsecret shim stays "unavailable" through milestone 1. Credential Manager is considered afterwards; it changes the threat model, so that choice is made explicitly. | decided; Credential Manager not started |
| 6 | Rust | Deferred; outside "finish". | decided |
| 7 | Distribution | Portable zip for milestone 1. MSI/MSIX only at the final goal. No code signing until then. | decided; not started |
| 8 | GSettings backend | Decide after testing. Start with GLib's registry backend (untested); consider a keyfile backend for the portable zip. | not started |
| 9 | Desktop integration | Final goal only. Tray and autostart first; session lock and suspend lock after them. Webcam QR scan stays out until requested. QR from image and QR display stay. | not started |
| 10 | CI | GitHub Actions in the fork: Linux ctest through `nix develop`, and the Windows cross-build. No Windows runner for now, so native Windows tests (console, ACL, shim-check) stay manual. The workflow would live in `.github/workflows/` (fork-only, not created). | not started |
| 11 | ARM64 | Only if requested. Architecture stays in the toolchain file. | decided |

## Phases

0. **Toolchain spike — done for core + CLI.** Nix `pkgsCross.ucrt64` supplies MinGW-w64, GLib/GIO, libgcrypt, Jansson, libcotp and gettext. Windows-only overrides clear Jansson's ELF `postFixup`, add libgcrypt `--disable-asm`, guard libcotp's ELF linker flags and stage its DLL into `bin`; see `docs/windows-build.md`. GUI dependencies are not part of this cross shell.
1. **Core builds on Windows — done in the CLI target.** All common sources compile with fork-only shims and unchanged DB v3 format. The optional shim checks run natively; the upstream pure-logic ctest suite remains unwired/unverified on Windows. No standalone core library or Linux port interfaces have been added.
2. **CLI on Windows — mostly done.** Native Windows 11 import/list/show/plain exports, password-file ACL acceptance/refusal, protected export rename and console checks are recorded. Generated Ctrl+C/Ctrl+Break abort without Enter in under 25 ms (exit 3, mode restored); backspace, Unicode, typeahead flushing, successive prompts and piped input pass. Secret Service, crash-dump prevention, real memlock budgeting, umask/chmod and directory durability remain stubs; persistent settings, full encrypted AuthPro round-trip and physical keyboard Ctrl+C remain unverified. The verification matrix is in `docs/windows-build.md`.
3. **GUI on Windows (GUI phase 1, milestone 1) — not started.** Port GTK4 / libadwaita from a portable MSYS2 UCRT64 tree, built without an installer or PATH changes under a throwaway folder. Milestone 1 is a functional GUI without tray, autostart or session lock.
4. **Desktop integration (final goal) — not started.** Tray and autostart first, then session lock and suspend lock. Single instance is not covered by the decisions and stays undecided.
5. **Packaging and CI — not started.** Milestone 1 ships a portable zip with no code signing. The final goal adds an MSI/MSIX installer and signing. The CI plan is Decision 10; no Windows packaging path or workflow exists yet. The sync routine is documented.

## Future: Rust

Rust can be considered for leaf modules behind new C interfaces; a core rewrite is not on the Windows critical path. New port interfaces should use plain C types, opaque handles, numeric error codes and explicit allocation/free ownership. Do not expose GLib/GIO or Jansson types such as `GError`, `GSList`, `GFile` or `json_t *` at new seams. Existing compatibility shims must still match the upstream signatures; this rule does not require rewriting them.

Reasons to keep this bounded:

- `db-common.h` and `common.h` expose GLib/GIO containers, errors and Jansson objects broadly; `db-common.c`/`common.c` are not a small dependency-free C ABI.
- No Rust secure-memory replacement with guarantees equivalent to `gcry_malloc_secure` has been proven for this port. Passwords/keys and Jansson allocation still use libgcrypt (`common.c:139-144`).
- DB v3 uses a 16-byte IV (`db-common.h:14-17`, `db-common.c:1895-1898`); RustCrypto's [default `Aes256Gcm` alias](https://docs.rs/aes-gcm/latest/aes_gcm/type.Aes256Gcm.html) uses a 12-byte nonce. Preserve the existing format and verify compatibility rather than changing the IV as a side effect.
- `tests/fixtures/` has no complete v1/v2 DB fixtures. `test_malformed_db.c:110` synthesizes a truncated v2 header, and `test_db_transaction.c:328` forces migration on a generated current-format DB; neither proves legacy decrypt/migration interoperability.

## Out of scope for the Windows build

Flatpak, the D-Bus search provider and KRunner/GNOME Shell metadata, and `data/dbus-1/`.
