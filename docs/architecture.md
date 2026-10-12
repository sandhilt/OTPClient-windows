# Architecture (as of upstream 5.2.2)

This describes the upstream 5.2.2 architecture and the fork's current Windows core + CLI layer. Findings were gathered by reading the tree and spot-checked against the code. Line numbers drift; treat them as pointers.

OTPClient is a C11 / GLib application: a GTK4 + libadwaita GUI, a CLI, and a D-Bus search provider. All three share one set of "common" sources. The Windows build compiles the same common sources into the CLI, with fork-only shims in `src/platform/windows/` and target wiring in `cmake/`; the upstream source and build files are unchanged.

## Targets

| Target | Output | Sources | Notes |
|---|---|---|---|
| GUI | `otpclient` | `src/gui/*` + `src/common/*` | GTK4, libadwaita, zbar, protobuf-c, libqrencode, gdk-pixbuf |
| CLI | `otpclient-cli` | `src/cli/*` + `src/common/*` | No GTK. Terminal password prompt via termios (Win32 console shim on Windows) |
| Search provider | `otpclient-search-provider` | explicit subset of `src/common` + `src/search-provider/search-provider.c` | GNOME Shell and KDE KRunner over GDBus |

`src/common` is **not a library**. Each target globs or lists the sources and compiles them itself (`src/gui/CMakeLists.txt`, `src/cli/CMakeLists.txt`, `src/search-provider/CMakeLists.txt`, and `tests/CMakeLists.txt`). Flags are applied per target by `otpclient_apply_target_settings()` in the root `CMakeLists.txt`.

## `src/common` — the de-facto core

No GTK, GDK or libadwaita includes anywhere in it. It is still not "pure": on Linux it links libgcrypt, jansson, libcotp, libsecret and libuuid. Windows supplies the UUID surface and an explicitly unavailable libsecret stub instead of linking those two libraries.

- **Database** (`db-common.[ch]`): encrypted file, AES-256-GCM. Header `OTPClient`, `DB_VERSION` 3. v2 and v3 derive the key with Argon2id through libgcrypt (defaults: 4 iterations, 128 MiB, parallelism 4); v1 uses PBKDF2-SHA512 (`db_version < 2` in `db-common.c`). On Linux, writes are atomic (temp file, fsync, rename, fsync of the directory) and the file is created `0600`. Windows keeps this DB path, with binary CRT descriptors and file `_commit`; directory opening/flushing is unsupported and warns, and DB/lock/temp files still inherit ACLs. Private GIO exports use a separate protected-DACL handle adapter; that does not harden DB creation. Concurrent access is guarded by `flock()` on `<db_path>.lock`; if the database sits somewhere that cannot lock (for example the Flatpak document portal) it falls back to a lock under `g_get_user_data_dir()/otpclient/locks`, and execution can continue without a lock. Public API: `load_db`, `update_db`, `db_transaction`, `db_change_password`, `db_update_kdf_params`, `db_import_otps`, `db_copy_to`, `db_generate_hotp`.
- **Process hardening / secure memory** (`common.c`): `init_libs()` calls `prctl(PR_SET_DUMPABLE, 0)` and `setrlimit(RLIMIT_CORE, 0)`. `set_memlock_value()` reads and raises `RLIMIT_MEMLOCK`, and `secmem_pool_from_limits()` sizes libgcrypt's secure-memory pool from it. There is no direct `mlock()` call; locking is delegated to libgcrypt.
- **Importers / exporters**: `aegis.c`, `authpro.c`, `freeotp.c`, `twofas.c`, `import-export.c`, `parse-uri.c` (otpauth URIs), `settings-import-export.c`, plus Google Authenticator migration in `src/gui/google-migration.c` (protobuf-c).
- **OTP validation** (`otp-validation.c`) and OTP generation (libcotp).
- **Secrets** (`secret-schema.c`): libsecret wrapper with a legacy-attribute fallback. Used by the GUI, CLI and search provider.
- **Settings** (`gsettings-common.c`): GSettings schema `com.github.paolostivanin.OTPClient`, DB path, search-provider and DB-list getters. `gsettings_common_get_use_secret_service()` reads GSettings first and falls back to the key file (`otpclient.cfg`) only when GSettings is unavailable.
- **Paths**: `get_kf_ptr()` in `common.c` puts `otpclient.cfg` under `g_get_user_config_dir()`, or `g_get_user_data_dir()` when `IS_FLATPAK` is set.

## `src/gui`

- Entry: `main.c` → `otpclient-application.c` (`AdwApplication`) → `otpclient-window.c` (5443 lines, template from `ui/window.ui`). Context menus and the shortcuts window are separate `.ui` files, all bundled by `gui.gresource.xml` under `/com/github/paolostivanin/OTPClient`.
- 11 dialogs in `dialogs/` (password, import, export, settings, KDF, manual add, edit token, …). `database-sidebar.c` is a GObject model with no GTK calls in its `.c`.
- **There is no UI/domain boundary.** Widgets call the backend directly:
  - `otpclient-window.c` calls `db_generate_hotp`, `db_import_otps`, `db_copy_to`, allocates the master key with `gcry_calloc_secure`, and calls `secret_password_lookup_sync`.
  - libsecret is spread across `otpclient-application.c` (many call sites); GSettings is read and written from `otpclient-application.c`, `otpclient-window.c`, `settings-dialog.c` and `gui-misc.c`.
  - Dialogs call `db_transaction`, `db_update_kdf_params`, `db_import_otps` directly.
  - `otp-entry.c` (the OTP model) includes gcrypt and libcotp and generates codes itself.
- Platform-coupled modules (all Linux desktop specifics):
  - `tray.c`, `tray-menu-model.c`: StatusNotifierItem / StatusNotifierWatcher and `com.canonical.dbusmenu` over GDBus.
  - `autostart.c`: native builds write an XDG `autostart/*.desktop` file; the `org.freedesktop.portal.Desktop` Background portal path is Flatpak-only. `autostart-policy.c` is a pure GIO decision function.
  - `lock-app.c`: watches `org.gnome.ScreenSaver`, `org.cinnamon.ScreenSaver`, `org.freedesktop.ScreenSaver` and `com.canonical.Unity.Session` over the session bus, and (non-Flatpak builds only) logind's `PrepareForSleep` on the system bus.
  - `webcam-scanner.c`: zbar processor with an empty device string (V4L). `qrcode-parser.c`: zbar + gdk-pixbuf.
- GTK-free candidates (include chain checked): `autostart-policy`, `tray-menu-model`, `webcam-scanner`, `google-migration`, `otp-entry`, `otpclient-types.h`. `gui-misc.c`, `autostart.c` and `tray.c` pull GTK in transitively.

## `src/cli`

`main.c` (GIO option parsing), `exec-action.c` (actions, password handling), `get-data.c`. Uses `termios`, `isatty`, `sigaction`, `O_NOFOLLOW`, `getline`, and an unbuffered `read()` on the password fd. Windows redirects these calls through compat headers/forced includes: `console.c` consumes UTF-16 key records into a secure UTF-8 line buffer, waits on console input plus an interrupt event, and returns `EINTR` for Ctrl+C/Ctrl+Break. The unchanged caller restores modes/dispositions and re-raises the signal. Pipes/files retain CRT read semantics.

## `src/search-provider`

GDBus only (no GTK): owns the GNOME search-provider and `org.kde.krunner` names, calls the notification and clipboard (Klipper) services, loads the DB itself and fetches the key from libsecret. Reads `WAYLAND_DISPLAY`, `XDG_SESSION_TYPE` and `XDG_CURRENT_DESKTOP`.

## Build and CI

- CMake ≥ 3.25, C11 with extensions off. Options: `IS_FLATPAK`, `ENABLE_MINIMIZE_TO_TRAY`, `BUILD_GUI`, `BUILD_CLI`, `BUILD_SEARCH_PROVIDER`, `SANITIZE`. `include(CTest)` makes `BUILD_TESTING` default to ON.
- Linux-only link flags are gated on `CMAKE_SYSTEM_NAME STREQUAL "Linux"`: `--no-copy-dt-needed-entries`, `--as-needed`, `--no-undefined`, `-z relro,now`. `-pie`/`-fPIE` are gated on GNU|Clang, not on Linux, and several hardening flags are probed with `check_c_compiler_flag`.
- Hard pkg-config deps for every target: libgcrypt ≥ 1.10.1, libcotp ≥ 4.0.0, jansson, glib/gio ≥ 2.74, uuid, libsecret. GUI adds gtk4 ≥ 4.10, libadwaita ≥ 1.5, gdk-pixbuf, zbar, libprotobuf-c, libqrencode.
- CI is CircleCI (`.circleci/config.yml`): five distro build jobs (they build and install but do not run ctest), a sanitizer job (`-DSANITIZE=ON`, smoke test, ctest), and a clang `scan-build` job. `.ci/install_deps.sh` builds libbaseencode and libcotp from source (libbaseencode is a historical CI step: libcotp 4.1.0 in nixpkgs has no pkg-config requirement on it, and nothing in `CMakeLists.txt` or `src/` references it).

## Windows compatibility layer

`cmake/windows-ucrt.cmake` selects x86_64 UCRT and loads `cmake/windows.cmake` via `CMAKE_PROJECT_INCLUDE`. A deferred hook attaches `compat.c`, `console.c`, `security.c` and `private-file.c` to the existing CLI target, adds the compat include path and forced header, links gettext/`advapi32`, and removes PIE/glibc FORTIFY options. No separate `otpclient-core` library exists. GUI, provider, upstream tests and Flatpak must be disabled.

- `compat.c`: wiping, no-follow UTF-16 file opens, binary descriptors, `LockFileEx`, UUIDs, importer fd tokens and unavailable Secret Service. The `/proc/self/fd/N` spelling is a transport token on Windows; JSON callbacks and a seekable `GFileInputStream` read the checked descriptor without reopening its path.
- `security.c`: conservative owner/DACL audit mapped to group/other mode bits for the unchanged password-file check. It is not a full AccessCheck evaluator.
- `private-file.c`: protected current-user-only DACL at creation, held-handle writes/flush/rename and ACL revalidation; no directory durability guarantee.
- `console.c`: no-echo UTF-8 password input and event-driven interruption. True SIGHUP, asynchronous SIGTERM and general terminal emulation remain unsupported.
- Optional `shim-check.c` and `console-check.c` regression executables are built with `OTP_WINDOWS_SHIM_CHECKS=ON`; they are separate from ctest. Recorded native results and remaining verification are in `docs/windows-build.md`.

## Tests

`tests/CMakeLists.txt` registers everything with `add_test`. Gating:

- Needs a private D-Bus session (`tests/isolated-session.py`: Python 3, `dbus-daemon`, `glib-compile-schemas`): `review_flows`, `search_provider_settings`, `tray_session_*`, `gui_flows` (the last two also need Xvfb).
- Pure logic, no bus or display: `parse_uri*`, `otp_generation`, `otp_validation`, `common_utils`, `field_bounds_roundtrip`, `memlock_sizing`, `db_roundtrip`, `db_transaction`, `malformed_*`, `cli_hotp`, `dbus_signatures`, `tray_menu_model`, `settings_import`, `autostart_policy`.
- Portability caveat: a number of tests compile large parts of `src/common` (a few the whole set), including `common.c` with its `prctl`/`rlimit` code. The core/importers now build in the Windows CLI, but the hook does not wire the upstream test targets and rejects `BUILD_TESTING=ON`; the pure-logic ctest suite has not run on Windows. `otp_generation` compiles no common sources.
