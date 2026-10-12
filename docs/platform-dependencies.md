# Platform dependency audit

Every Linux/POSIX/desktop-specific coupling found in the tree (upstream 5.2.2), with the Windows candidate and the kind of seam it needs. Produced by two read-only audits (not a proof of completeness; re-grep after every upstream merge) (`src/common` + CLI + build; `src/gui` + search provider), then spot-checked against the code. Line numbers drift; re-run `git grep` before relying on them.

**Reading the table.** Everything in the "Windows candidate" column is a hypothesis. Nothing here has been built or run on Windows. "Seam": **shim** = a Windows replacement for an existing include/symbol, no call-site edit; **port** = an explicit interface in `src/platform/` plus a small call-site edit; **none** = works as is (GLib/portable) or is excluded.

## 1. `src/common` and the CLI (compiled into every target)

| Where | Linux API | Purpose | Windows candidate | Seam |
|---|---|---|---|---|
| `common.c:100,112`; `cli/main.c:231` | `getrlimit/setrlimit(RLIMIT_MEMLOCK)` | size libgcrypt's secure-memory pool; the CLI aborts if the query fails | no direct mapping; derive a pool size from a Windows policy (working-set limits / `VirtualLock`) | port |
| `common.c:126,130` | `prctl(PR_SET_DUMPABLE)`, `RLIMIT_CORE` | stop crash dumps containing secrets | Windows dump/WER policy; `SetErrorMode` alone is not shown to be equivalent | port |
| `common.c:185`, `db-common.c:179`, `authpro.c:274`; `gui/*`, `search-provider.c:212` | `explicit_bzero` (with `_DEFAULT_SOURCE`) | wipe passwords and keys | compat function over `SecureZeroMemory`; must not be optimised away | shim |
| `parse-uri.c:234`, `settings-import-export.c:136` | `strnlen` | bound input length | normally available in the MinGW CRT (not verified); otherwise a bounded shim | shim |
| `cli/exec-action.c:475` | `getline`, `ssize_t` | read a DB path from stdin | small line-reader shim | shim |
| `common.c:777-792` | `open(O_NOFOLLOW\|O_CLOEXEC)`, `fstat`, `S_ISREG` | open an import file once, refuse symlinks and non-regular files | `CreateFileW` + reparse-point/type checks; keep UTF-8 → UTF-16 conversion | port |
| `aegis.c:102`, `authpro.c:64`, `twofas.c:89`, `freeotp.c:46` | `/proc/self/fd/%d` | feed path-based parsers the already-opened (and already checked) fd | **no equivalent.** Pass the opened handle/stream to the parsers instead of reopening a path. Needs real edits in four upstream files | port |
| `db-common.c:260,1665` | POSIX `read/write/close` on fds | binary DB I/O with short-I/O handling | CRT fd adapter (binary mode) or Win32 handle I/O | shim |
| `db-common.c:376,451,544` | `flock(LOCK_EX\|LOCK_NB)`, `LOCK_UN` | serialise writers; the code relies on two separately opened fds in one process excluding each other (`db-common.c:358`) | `LockFileEx`/`UnlockFileEx`; verify that same-process exclusion, lock range, sharing flags and error mapping | shim |
| `db-common.c:311,313,339` | literal `/` temp path, `g_mkstemp_full`, `g_rename` | atomic replace | native path building; `ReplaceFileW`/`MoveFileExW` | port |
| `db-common.c:286,323` | `fsync` of file and parent directory | durability | `FlushFileBuffers`; no established equivalent for directory flush | port |
| `db-common.c:408,2097,2108`; `freeotp.c:72`, `aegis.c:592` | `0700/0600`, `umask`, `g_chmod`, `G_FILE_CREATE_PRIVATE` | private files and lock dirs | per-user DACLs at creation; CRT mode bits are not access control | port |
| `cli/exec-action.c:127,147` | POSIX mode check on `--password-file` | reject symlinks, non-regular files, group/other access | handle-based reparse/type checks + DACL validation | port |
| `cli/exec-action.c:506-565` | `isatty`, `termios`, `sigaction`, `raise` | no-echo password prompt, restore terminal on SIGINT/TERM/HUP | `GetConsoleMode/SetConsoleMode`, console-control handler; separate path for pipes/files | port |
| `cli/exec-action.c:442` | `strchr(arg, G_DIR_SEPARATOR)` | DB path vs. configured name | must also recognise `/` and drive-relative paths on Windows | port |
| `aegis.c:387-391` | `libuuid` (`uuid_generate_random`, `uuid_unparse_lower`) | Aegis export slot UUID | `g_uuid_string_random()` (already used at `secret-schema.c:103`). The call site uses the binary API, so the shim must adapt it | shim |
| `secret-schema.c` (whole file); GUI `otpclient-application.c` | `libsecret` | per-database password storage, probe, legacy migration | Credential Manager (storage) and/or DPAPI (encryption) are different building blocks; a shim must preserve attributes, async callbacks, cancellation, errors and secret-buffer ownership | shim |
| `gsettings-common.c:70,79`, `settings-import-export.c:52,157,223` | GSettings default backend + installed schema; partial `GKeyFile` fallback | preferences, DB list, settings import | GLib's registry backend is the candidate, see `docs/windows-port.md`; not yet tested here | port |
| `common.c:697`, `db-common.c:407`, `exec-action.c:361` | `g_get_user_{config,data}_dir`, `g_get_home_dir` | config and lock locations | same GLib calls | none |
| `cli/get-data.c:198-206`, `gui/otp-entry.c:489-576` | `time_t` → `long` for libcotp's timestamp argument | TOTP at a given time | on 64-bit Windows `long` is 32 bits, so there is a 2038 limit through libcotp's API. The CLI already rejects values above `LONG_MAX` | port |
| `db-common.c:1690,1747` | legacy (v1/v2) native struct layout | read old DBs | v3 uses explicit big-endian; verify struct padding for v1/v2 on Windows | none (verify) |
| `db-common.c:1409`, `db-common.h:26` | Argon2id 128 MiB × 4 lanes | key derivation | same libgcrypt KDF; check allocation failure and secure-memory behaviour on Windows | none (verify) |
| `cli/main.c:42`, `version.h.in:10` | gettext, `LOCALE_DIR` | translations | gettext on Windows, relocatable locale dir | port |
| `common.c:1`, `aegis.c:2`, `exec-action.c:1` | `_GNU_SOURCE`, `_DEFAULT_SOURCE` | expose non-ISO declarations | these macros do not supply missing APIs on Windows | shim |
| `macros.h:3`, `cli/main.c:109` | `__attribute__((unused))`, `g_autoptr` | warnings, cleanup | fine with GCC/Clang (MinGW, clang); **not** with MSVC | none if MinGW/Clang |

## 2. `src/gui`

| Where | Linux API | Purpose | Windows candidate | Seam |
|---|---|---|---|---|
| `otpclient-application.c:3,1480-1482` | `<glib-unix.h>` (unconditional) and `g_unix_signal_add` for SIGINT/SIGTERM/SIGHUP | quit cleanly on signals | guard the include; `g_unix_signal_add` has no Windows form, so use `SetConsoleCtrlHandler` or another portable mechanism | port |
| `otpclient-application.c:366,384,424,860,1006-1064`; `otpclient-window.c:835`; `settings-dialog.c:186` | `libsecret` store/lookup/clear and a round-trip probe | remember DB password | as above | shim |
| `otpclient-application.c:1432`; `otpclient-window.c:5277`; `settings-dialog.c:932-965` | GSettings, `changed` signals | preferences; cross-consumer sync | needs a backend with change notification, including across processes | port |
| `otpclient-application.c:605-611` | `/doc/`, `/run/user/%u/doc/`, `getuid` | detect Flatpak document-portal paths | no Windows analogue | port (or drop) |
| `gui-misc.c:152`, `otpclient-window.c:3967`, `otpclient-application.c:431` | exact string equality on paths as DB identity and secret key | identify a DB | needs a Windows path-identity policy (case, separators, drive/UNC, aliases) | port |
| `tray.c:15,606,709,1020` | StatusNotifierItem/Watcher, `com.canonical.dbusmenu`, `getpid` | tray icon and menu | `Shell_NotifyIconW` with a native menu/event bridge | port |
| `autostart.c:646,844,860` | XDG `autostart/*.desktop` | login-time launch | `HKCU\…\Run` or Startup shortcut | port |
| `autostart.c:662-669,836` | `INSTALL_BINDIR`, bare `otpclient`, desktop-entry quoting | the command to launch | runtime `.exe` location and Windows quoting | port |
| `autostart.c:52,614` | Flatpak Background portal | background permission | exclude on Windows | port |
| `lock-app.c:13-15,417,422` | `org.gnome/cinnamon/freedesktop.ScreenSaver` signals (`:417`) and `com.canonical.Unity.Session.Locked` (`:422`) | lock when the session locks | `WTSRegisterSessionNotification` | port |
| `lock-app.c:445,451` | system D-Bus logind `PrepareForSleep` | lock before suspend | `WM_POWERBROADCAST` | port |
| `lock-app.c:393` | GLib monotonic time | in-app inactivity | same | none |
| `otpclient-window.c:1893`; `gui-misc.c:257` | `g_application_send_notification` | notifications | GIO backend on Windows or a native toast adapter (unverified) | port |
| `webcam-scanner.c:50-81` | zbar video processor (V4L, `/dev/video0` per comment at `:42`) | webcam QR scan | zbar's video backend on Windows is unverified; Media Foundation capture feeding a decoder is another option | port |
| `qrcode-parser.c`, `qr-display-dialog.c:17`, `google-migration.c:92` | zbar, GdkPixbuf, libqrencode, protobuf-c | QR decode/encode, Google migration | same libraries (packaged in MSYS2, see `docs/windows-port.md`) | none |
| `main.c:12` | gettext, `LOCALE_DIR` | translations | relocatable locale dir | port |
| `otpclient-application.c:159` | `GApplication` default flags | single-instance activation | behaviour on Windows to be verified | port |
| `gui-misc.c:46-52` | `g_get_user_*_dir`, GKeyFile `otpclient.cfg` | config file | same GLib calls | none |
| `export-dialog.c:133`, `import-dialog.c:229`, `settings-dialog.c:593`, `otpclient-window.c:3182` | `GtkFileDialog`, native paths | file pickers | same APIs; check Unicode/UNC | none |
| `otpclient-window.c:1867` | GdkClipboard | copy/clear | same APIs; ownership and delayed clear to be verified | none |
| `otpclient-application.c:354`, `settings-dialog.c:562,629,646,655` | `__attribute__((unused))` | warnings | fine with GCC/Clang | none |

## 3. Search provider and D-Bus data

`src/search-provider/*` (KRunner object registered at `search-provider.c:2135`, GNOME SearchProvider2 at `:2147`, bus names at `:2213,2216`, Klipper clipboard at `:1324`, Wayland/X11 env and `wl-copy`/`xclip`/`xsel` at `:1340,1477-1479`, notification portal at `:1600`, direct `org.freedesktop.Notifications` at `:1650`), and `data/dbus-1/services/*.in`, `data/krunner/*`, `data/gnome-shell/*`: **excluded from the Windows build** (`-DBUILD_SEARCH_PROVIDER=OFF`). Note the cross-target dependency below: disabling the provider does not remove the `uuid`/`libsecret` requirement.

## 4. Build system and CI

| Where | Item | Note |
|---|---|---|
| `CMakeLists.txt:101-108` | `PkgConfig REQUIRED`; `uuid` and `libsecret` unconditional | `uuid` and `libsecret` stay required even with the GUI and provider off. They must be shimmed or made optional on Windows |
| `CMakeLists.txt:21,26` | GNU-style warnings, `-O3`, `_FORTIFY_SOURCE=3` | fine with MinGW GCC/Clang; `_FORTIFY_SOURCE` depends on the libc; MSVC would need different flags |
| `CMakeLists.txt:35,44,77,93` | `-fPIE/-pie`, probed hardening flags, ELF linker flags, sanitizers | ELF flags are already Linux-gated; PIE has only a compiler guard; sanitizers are not probed |
| `CMakeLists.txt:111-116` | GUI dependency set | `BUILD_GUI=OFF` allows a core/CLI spike without deciding the GUI strategy |
| `CMakeLists.txt:153-253` | desktop file, man pages, completions, metainfo, `glib-compile-schemas`, `gtk4-update-icon-cache` | the icon-cache command is unconditional, including for CLI-only builds |
| `src/gui/CMakeLists.txt:26`, `src/cli/CMakeLists.txt:6-9`, `src/search-provider/CMakeLists.txt`, `tests/CMakeLists.txt` | common sources compiled into every target | compat settings must reach all targets, tests included; even parser tests inherit OS-coupled `common.c` |
| `tests/CMakeLists.txt:50-57,143` | integration tests are gated on Python, `dbus-daemon`, `glib-compile-schemas`, and `Xvfb` for GUI tests | a missing prerequisite omits those registrations with only a status message; absence is not a pass. Schema-dependent unit tests such as `settings_import` stay registered and can skip themselves |
| `.ci/*.sh`, `.circleci/config.yml` | Bash, Make, `/usr`, source builds of libcotp (and libbaseencode, which nothing references) | Linux only; a Windows job needs its own scripts |

## Hypotheses that need real verification

- **libsecret → Credential Manager/DPAPI.** Storage and encryption are different things; a replacement must preserve attribute identity, async callbacks, cancellation, error reporting and secret-buffer ownership.
- **libuuid → `g_uuid_string_random`.** Plausible but not drop-in: the call site uses the binary API, so the shim has to adapt it. Check UUID version and lowercase output.
- **`flock` → `LockFileEx`.** The code explicitly requires that two separately opened descriptors in one process exclude each other (`db-common.c:358`). Verify lock range, sharing flags, close behaviour and error mapping.
- **GSettings on Windows.** No persistent Windows backend is selected anywhere in the repo, and the tests use the `memory` backend (`tests/test_settings_import.c`), which proves nothing about persistence. GLib's registry backend is the candidate; it is untested with this schema.
- **Secure memory.** Pool sizing, page-lock guarantees and the 128 MiB Argon2id cost have not been looked at on Windows.
