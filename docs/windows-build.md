# Windows CLI cross-build (2026-10-12)

## Current result: core + CLI linked

The exported Windows environment produced
`/tmp/otp-win-build/src/cli/otpclient-cli.exe`, a `pei-x86-64` Windows console
executable using UCRT, with all `src/common/*.c` compiled into the CLI. The final
build had no compiler/linker warnings. No upstream source/build files or database
format were changed. The architecture is selected only
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

### Shim review fixes (2026-10-12)

#### Native Windows rename regression follow-up

Native Windows 11 reported a garbage filename suffix and errors 3/123 when the
parent had an explicit inheritable Users read ACE. The confirmed buffer issue
was `offsetof(FILE_RENAME_INFO, FileName) + name_bytes`, with no UTF-16 NUL.
That is the suspected cause, **not a proven explanation of the ACE correlation**:
[Microsoft's FILE_RENAME_INFO page](https://learn.microsoft.com/en-us/windows/win32/api/winbase/ns-winbase-file_rename_info)
calls `FileName` NUL-terminated but also says a terminator is not required under
`FileNameLength`. The revised buffer uses `sizeof(FILE_RENAME_INFO) + name_bytes`,
zeroed padding and an explicit NUL; `FileNameLength` still excludes NUL. The
character count comes from `GetFullPathNameW`, with checked capacity/byte limits,
rather than `wcslen`. The temporary uses that same absolute path.

The installed MinGW header has the conditional Flags/ReplaceIfExists union.
A cross-compiled layout probe confirmed 2-byte WCHAR, 24-byte struct and offsets
0/8/16/20 for ReplaceIfExists/RootDirectory/FileNameLength/FileName on this Win64
toolchain. Zero initialization, `ReplaceIfExists = TRUE` and
`RootDirectory = NULL` match `FileRenameInfo`. Creation already requested `DELETE`, `READ_CONTROL` and
`GENERIC_WRITE`, with share mode 0 and `CREATE_NEW`. `FileRenameInfoEx` with
REPLACE_IF_EXISTS/POSIX_SEMANTICS was considered but not adopted: replacing open
destinations is outside this fix and its native behavior has not been tested.
The held object's protected user-only DACL/owner is verified before writes and
again after rename. ACL/ACE/SID bounds are checked. Failures request deletion
on that handle; cleanup errors are included in the returned error. If post-rename
verification fails, the renamed object is discarded (the old target has already
been replaced); no successful export is reported. OS cleanup failure can still
leave an artifact, and parent-directory durability remains unverified.

`/windows/private-inheritable-acl` installs and confirms an explicit Users
`(OI)(CI)` read ACE using `WinBuiltinUsersSid`, `SetEntriesInAclW` and
`SetNamedSecurityInfoW`. In each of a normal directory and an extended-length
path over `MAX_PATH`, 20 distinct names are created and replaced (80 successful
closes expected), including accented/CJK/supplementary Unicode. Win32 enumeration
checks the exact sole filename and absence of temporaries; reads check exact
bytes; an independent security query checks owner, protected DACL and its sole
non-inherited full-access user ACE. A rename onto an existing directory must
fail and remove its temporary in both paths.

This revision cross-built CLI and shim checks in `/tmp/otp-win-build2` without
compiler/linker warnings. **No Windows executable could be run in this sandbox.**
Native NTFS retest is required to confirm the cause/fix, ACL retention, long-path
handling and failure cleanup. Second-user access denial, other filesystems,
minifilters and crash durability remain gaps. Rebuild using the exported shell
recipe below, substituting `/tmp/otp-win-build2` for `/tmp/otp-win-build` (no Nix).
Copy the new executable into a Windows folder with the staged runtime DLLs and
run in PowerShell:

```powershell
.\otp-windows-shim-check.exe --tap -p /windows/private-inheritable-acl
if ($LASTEXITCODE -ne 0) { throw "Inheritable ACL regression failed" }
.\otp-windows-shim-check.exe --tap
if ($LASTEXITCODE -ne 0) { throw "Windows shim checks failed" }
```

**Native verification (2026-10-12, Windows 11 via WSL interop):** the rebuilt
`otp-windows-shim-check.exe` passed `/windows/private-inheritable-acl` 5/5 runs and
the full suite (3/3) with exit 0. A manual repro (directory with an explicit
`(OI)(CI)` Users read ACE, 10 private exports via `otpclient-cli.exe`) gave rc=0
every time, with the exact file name, no `.otp-private-*` leftovers and an
owner-only DACL on the exported file. The previous binary was not re-run as a
baseline, so this shows the fix works but does not re-demonstrate the old failure.
Still unverified: second-user access denial, other filesystems, crash durability.

The current revision rebuilt the CLI and `otp-windows-shim-check.exe` from clean
objects with no compiler/linker warnings. The native Linux GUI, CLI, provider and
tests compiled; all 36 tests were registered and the 21 non-session tests passed.
The 15 isolated D-Bus/GUI session tests were explicitly excluded, not counted as
passes. No Nix, Wine or D-Bus execution was attempted in this revision.

| Review item | Implementation / result |
|---|---|
| 1. Private exports | `private-file.c` intercepts private `g_file_replace`. A same-directory `CREATE_NEW` temporary receives the current token user's SID as owner and a protected DACL with one allow ACE for that SID at creation. Before any plaintext write, the handle is checked for persistent ACL support, owner and the actual protected user-only DACL. Writes stay on that exclusively held handle; close flushes and renames that object using `FileRenameInfo`, retaining its ACL. Unsupported filesystems, private backups/etags/extra flags and ACL failures return errors. There is no fallback to CRT privacy. |
| 2. JSON error | `otp_json_load_file` initializes the whole `json_error_t`, reports failed seeks and supplies a fallback message for parser failures lacking text. |
| 3. Encrypted AuthPro seek | The fd-backed `GFileInputStream` now implements `tell`, `can_seek`, and SET/CUR/END seek, including errors and cancellation. Closing/finalizing releases only its duplicated descriptor. Full encrypted AuthPro import still needs runtime validation. |
| 4. Unsupported locks | `ERROR_NOT_SUPPORTED`, `ERROR_INVALID_FUNCTION`, and `ERROR_CALL_NOT_IMPLEMENTED` map to `ENOSYS`, which `db_lock_take` recognizes for fallback. Contention remains `EWOULDBLOCK`. |
| 5. TCSAFLUSH | Separate NOW/DRAIN/FLUSH constants; FLUSH calls `FlushConsoleInputBuffer`, NOW/DRAIN retain mode-setting behavior, invalid actions return `EINVAL`. Console output draining has no analogue on this input handle. |
| 6. Password-file modes | `otp_fstat` audits the opened file's owner/DACL; only current user, SYSTEM and Administrators may have grants. Foreign read/metadata grants set `044`, write/control grants set `066`; NULL/unknown ACLs reject. Owner permission bits retain the CRT read-only attribute. Failure to query the token/ACL returns `EACCES`. The upstream mode check remains active. |
| 7. ABI | `sys/stat.h` uses the same `_WIN64` choice as the installed MinGW GLib: `_stat64`/`_fstat64` on Win64, `_stat32`/`_fstat32` otherwise. Internal size queries use the matching CRT function. No target architecture literal was added to C sources. A 32-bit dependency build remains unverified. |
| 8. Directory fsync | Investigated and retained: `db-common.c:280` opens the parent with `g_open`; `_wopen` rejects directory opens before `_commit` can run. Returning success would hide unimplemented durability. Each successful database replacement can warn; an import can perform multiple replacements. No Linux or upstream caller changes. |
| 9. libcotp DLL | Only the Windows libcotp `overrideAttrs` changed in `flake.nix`: appended `postInstall` locates the built `libcotp.dll` and installs it to `$out/bin/libcotp.dll`, failing if absent. Its existing ELF-flag patch is unchanged. This package change was not evaluated or built: no Nix daemon access. |
| 10. Reproduction | Commands below include the optional regression executable, recursive DLL staging, password-file creation through the private adapter, import/list/show/export and Linux exclusions. |

The added `security.{c,h}`, `private-file.c` and `shim-check.c` stay under
`src/platform/windows`; target wiring stays in `cmake/windows.cmake`. Windows
links the OS `advapi32` library. No upstream source or root build file was edited.

The regression executable checks private creation/replacement/cancelled close, DACL-derived
password-file modes (user-only, Everyone read/write, NULL DACL), stream seeking
and a populated JSON error after descriptor close. It has only been **compiled**.
Run it under Wine for API smoke coverage, then on native Windows/NTFS and as a
second ordinary user for actual access denial. Wine is not proof of Windows ACL
enforcement. Administrators' ability to take ownership/use privileges is outside
the ordinary-user DACL guarantee.

API basis: Microsoft's [new-object security descriptors](https://learn.microsoft.com/en-us/windows/win32/secauthz/security-descriptors-for-new-objects)
describe explicit protected DACLs at creation; [FILE_RENAME_INFO](https://learn.microsoft.com/en-us/windows/win32/api/winbase/ns-winbase-file_rename_info)
accepts an absolute destination and replacement of an existing file. GIO's
[GFileInputStream](https://docs.gtk.org/gio/class.FileInputStream.html) supplies the
seek virtual functions used by the adapter. These references do not constitute
runtime verification of this implementation.

### Console password interruption redesign (2026-10-12)

The previous `CancelSynchronousIo` design failed the manager's native Windows 11
tests. `tap-run-140626.txt` records `wait=258` (timeout) for the interrupt child;
`child-scenarios.txt` records early=77 in 53 ms and ignored=0, but pending int/break
children timed out. `variants-out.txt` records 3/3 timeouts each for Ctrl+C and
Ctrl+Break without Enter (15 seconds). `harness-out.txt` records that Enter after
the generated control also failed to release the new build, while prompt display,
no echo, modes 0x1F7/0x1F3/0x1F7, typeahead flushing and wrong-password rejection
passed. Non-console and existing shim checks passed in those logs. These are
manager-supplied results for the previous revision, not execution of this redesign.

Source reasoning: the previous wrapper could inspect its latch only before or
after the blocking CRT `_read`; the callback depended on cancelling that call
and kept an active-read retry loop until the wrapper retired it. The interrupt
latch was never consumed by a read, so later reads with the same installed actions
would also return EINTR indefinitely. That persistent-latch defect is established
by source inspection. **Why CancelSynchronousIo failed, and the exact cause of
Enter no longer releasing the already pending read, are NOT VERIFIED.** The
12-thread snapshot in `int-threads.txt` has no Sleep waiter and does not support
the retry-loop explanation. It does not identify a kernel/CRT or lock stack.
The redesign removes the entire synchronous-read cancellation path rather than
claiming a verified explanation of those native hangs.

`console.c` now waits on a manual-reset interrupt event and the console input
handle. Its control callback calls the installed C handler first (so the CLI's
`password_signal` is visible), latches interruption, calls `SetEvent`, and
returns TRUE. There are no duplicated thread handles, cancellation calls or
retry sleeps. SIG_DFL/SIG_IGN fall through to the CRT/OS; the callback is removed
when neither signal has a real handler. SIGHUP retains its SIGBREAK alias.
Actions, line state and event reset share an SRW lock; the lock is released
during `WaitForMultipleObjects`, and after each consumed record.

The compatibility header still redirects `read` without upstream edits.
`GetConsoleMode` selects the console branch, including when no handler is
installed; files, pipes, NUL and zero-length calls stay on plain CRT `_read`.
The console branch never calls `ReadFile`, `ReadConsole` or CRT `_read`.
[ReadConsoleInput](https://learn.microsoft.com/en-us/windows/console/readconsoleinput)
documents that any unread input record signals the console handle.
[SetConsoleMode](https://learn.microsoft.com/en-us/windows/console/setconsolemode)
documents that line/echo processing applies to ReadFile/ReadConsole, requiring
manual line assembly when consuming records.

After readiness, the wrapper peeks one record, then consumes it with dynamically
resolved `ReadConsoleInputExW(CONSOLE_READ_NOWAIT)`. This is a deliberate
deviation from a Peek/ReadConsoleInputW pair: the queue could become empty between
those calls (for example around Ctrl+Break), leaving ReadConsoleInputW blocked.
[Microsoft documents NOWAIT](https://learn.microsoft.com/en-us/windows/console/readconsoleinputex)
as returning even for an empty queue, with dynamic lookup from Kernel32.dll and
Windows 7 as the minimum client. This closes the race without relying on I/O
cancellation. Missing ExW produces ENOSYS, with no blocking fallback. This is a
documented API choice, not a measured runtime result.

Only key-down records with nonzero Unicode characters enter the line buffer;
mouse/focus/resize, key-up and zero-character records are consumed and ignored.
Character/backspace repeat counts are handled. In line mode, Enter completes a
line and backspace removes one UTF-8 code point (or a pending high surrogate).
UTF-16 surrogate pairs become UTF-8; unpaired surrogates become U+FFFD.
Enter emits LF, matching the CLI's text-mode CRT CRLF translation; no extra CR
or LF is retained for the next prompt. Complete lines are served across
`get_pwd`'s one-byte reads. The buffer grows in libgcrypt secure memory, explicitly
checks `gcry_is_secure`, and returns ENOMEM rather than using ordinary heap
storage. Served bytes, deleted characters, old growth allocations, input-record
scratch storage and discarded lines are wiped with `SecureZeroMemory`. The
wrapper never writes typed input to the console or a log.

On interruption, the wrapper wipes the partial/ready line, consumes all latches
for its reader thread, resets the manual-reset event and returns -1/EINTR once.
An ordinary Enter on the next call can complete a fresh line with the same
handlers installed. Registration/restoration resets its action's latch.
`tcsetattr(TCSAFLUSH)` now discards both OS input and buffered line bytes,
including an unread suffix after the CLI's 511-byte limit. It preserves pending
signals, including a signal before the first read. This prevents stale bytes or
event state carrying into successive import/export password prompts. Native
confirmation of this behavior, including Enter after Ctrl+C, is still pending.

`WINDOWS-STUB` limits are explicit in the source: one console reader/thread,
no cooked history/cursor editing or binary-console CRLF/code-page emulation;
this is the CLI's no-echo UTF-8 password path. POSIX masks/flags, true SIGHUP,
asynchronous SIGTERM, close/logoff/shutdown, debugger interception and console
reattachment remain unsupported. C handlers must be leaf handlers with no CRT
I/O or shim reentry. Other programs consuming the same console may remove input;
NOWAIT prevents such removal from turning consumption into a blocking read.

`OTP_WINDOWS_SHIM_CHECKS=ON` builds `otp-windows-console-check.exe`. A wait
handshake is compiled only into that target; the CLI and existing shim executable
have no test hook. Hidden child consoles isolate generated events from the parent.
The sender waits for an empty-queue read handshake, verifies that the read stays
pending for 100 ms without Enter, then uses
[GenerateConsoleCtrlEvent](https://learn.microsoft.com/en-us/windows/console/generateconsolectrlevent).
Injected ^C key records did not generate controls in the native harness, so they
cannot test this callback. Each successful interrupted read must complete within
2 seconds of generation. The parent bounds every child to 8 seconds, terminates
and reaps a timed-out child, reports scenario/wait/exit/timing and continues;
nonfatal parent assertions allow the remaining TAP tests to run.

The four test cases (assertions only; **not executed here**) are:

- `/windows/console-interrupt`: 20 cycles each of pending Ctrl+C/Ctrl+Break,
  early controls, both early controls together, restored `raise(SIGINT)`,
  plus SIG_IGN. Pending reads include noise, empty lines and partial Unicode.
  Every EINTR is followed by ordinary Enter with the same handlers, and checks
  the flag, exact mode restoration and delegation after restoration.
- `/windows/console-lines`: 20 prompt cycles checking no cursor/screen changes,
  ignored input records, backspace/repeats, BMP/supplementary UTF-8, unpaired
  surrogate handling, typeahead, secure-buffer growth and 511-byte truncation
  followed by flushing and another prompt.
- `/windows/console-default`: compares no-handler Ctrl+C/Ctrl+Break exit behavior
  against child processes reading through plain CRT `_read`.
- `/windows/nonconsole-read`: file/pipe CRLF and Ctrl+Z translation, binary file
  bytes, NUL EOF and zero-length calls.

**Verified here: cross-compilation/linking only.** The CLI and both check
executables built from clean objects in `/tmp/otp-win-build4` with no compiler or
linker warnings. Configure reports optional bash-completion pkg-config metadata
missing; configuration succeeds. Logs: `/tmp/otp-win-build4-configure.log` and
`/tmp/otp-win-build4-build.log`. No .exe, Nix, Wine, Linux tests or native console
tests were executed. Windows 11 regression results for this redesign are pending.
The committed private-file/security implementations and upstream files were not
edited. No commit was made.

Changed paths (line references for this revision):

| File | Source entry points |
|---|---|
| `src/platform/windows/console.c` | `:67` consume/reset; `:84` callback; `:179` secure buffer; `:222` editing/UTF-8; `:264` event-driven read |
| `src/platform/windows/console-check.c` | `:28` pending-read sender; `:147` line/prompt cases; `:340` bounded child wait; `:485` nonfatal TAP |
| `src/platform/windows/compat.c` | `:143` flush OS and shim input |
| `src/platform/windows/compat/otp-windows.h` | `:28` read/flush declarations; `:48` read redirect |
| `cmake/windows.cmake` | `:33` CLI source; `:59` console-check-only handshake definition |
| `docs/windows-build.md` | `:132` previous failure evidence, redesign and pending native verification |

Rebuild from the repository root using the manager's exported environment:

```sh
source /tmp/claude-1000/-home-sandhilt-workspace-OTPClient-windows/42d068b6-91af-47fd-bf85-ce9c99ea1974/scratchpad/win-env.sh
export PKG_CONFIG_PATH=/nix/store/k9ybcll1npa94kppyyn11zg817a5n810-libgpg-error-x86_64-w64-mingw32-1.61-dev/lib/pkgconfig:/nix/store/634njvdl9j4y0781l28r40bcqwfbhr0w-pcre2-x86_64-w64-mingw32-10.48-dev/lib/pkgconfig:$PKG_CONFIG_PATH
export OTP_WINDOWS_GETTEXT_ROOT=/nix/store/ywigw24851mjsyanpykx4ps3c3i5nclb-gettext-x86_64-w64-mingw32-1.0
cmake -S . -B /tmp/otp-win-build4 -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$PWD/cmake/windows-ucrt.cmake" \
  -DOTP_WINDOWS_GETTEXT_ROOT="$OTP_WINDOWS_GETTEXT_ROOT" \
  -DBUILD_GUI=OFF -DBUILD_SEARCH_PROVIDER=OFF -DBUILD_TESTING=OFF \
  -DOTP_WINDOWS_SHIM_CHECKS=ON
cmake --build /tmp/otp-win-build4 --clean-first -j8
```

Stage with the known runtime DLLs, copying only these new executables:

```sh
console_stage=$(mktemp -d /tmp/otp-win-console-stage4.XXXXXX)
cp /tmp/otp-win-stage.jaUV3P/*.dll "$console_stage/"
cp /tmp/otp-win-build4/src/cli/otpclient-cli.exe \
   /tmp/otp-win-build4/otp-windows-shim-check.exe \
   /tmp/otp-win-build4/otp-windows-console-check.exe "$console_stage/"
printf 'Copy this directory to Windows: %s\n' "$console_stage"
```

Copy that directory to Windows and open PowerShell there. These native commands
have **not** been run for the redesign:

```powershell
$failed = $false
1..3 | ForEach-Object {
    .\otp-windows-console-check.exe --tap
    if ($LASTEXITCODE -ne 0) { $failed = $true }
    .\otp-windows-shim-check.exe --tap
    if ($LASTEXITCODE -ne 0) { $failed = $true }
}
if ($failed) { throw "Windows regression checks failed; review all TAP runs" }

# Disposable existing DB; omit -p so this exercises the real console prompt.
.\otpclient-cli.exe --list -d 'C:\otp-test\test.db'
# Press Ctrl+C WITHOUT Enter; expected prompt cleanup followed by exit 3.
$LASTEXITCODE
# Repeat with Ctrl+Break, partial dummy input, and normal Enter.
.\otpclient-cli.exe --list -d 'C:\otp-test\test.db'
$LASTEXITCODE
```

Repeat the manager's real-console harness against the new stage, including
prompt display, no echo, exact console modes, typeahead, correct/wrong passwords,
pipes/files/NUL, Ctrl+C/Break without Enter and with Enter after the control,
and successive DB/import/export prompts in the same process. Hidden tests
exercise the shim and do not prove the complete CLI workflow or real keyboard
behavior. For an encrypted import/export prompt cycle, use a disposable
Authenticator Pro backup with a known dummy password:

```powershell
.\otpclient-cli.exe --import -t authpro_encrypted -f 'C:\otp-test\dummy-authpro.bin' -d 'C:\otp-test\roundtrip.db'
# Supply the DB password, then the backup's dummy encryption password.
New-Item -ItemType Directory -Force -Path 'C:\otp-test\export' | Out-Null
.\otpclient-cli.exe --export -t authpro_encrypted -d 'C:\otp-test\roundtrip.db' -o 'C:\otp-test\export'
# Supply the DB password, then a new dummy export encryption password.
```

### Exact build commands used

Run in Bash from the repository root. Source each environment in the same shell
as its commands. No Nix command was run for this attempt.

```sh
source /tmp/claude-1000/-home-sandhilt-workspace-OTPClient-windows/42d068b6-91af-47fd-bf85-ce9c99ea1974/scratchpad/win-env.sh
export PKG_CONFIG_PATH=/nix/store/k9ybcll1npa94kppyyn11zg817a5n810-libgpg-error-x86_64-w64-mingw32-1.61-dev/lib/pkgconfig:/nix/store/634njvdl9j4y0781l28r40bcqwfbhr0w-pcre2-x86_64-w64-mingw32-10.48-dev/lib/pkgconfig:$PKG_CONFIG_PATH
cmake -S . -B /tmp/otp-win-build -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$PWD/cmake/windows-ucrt.cmake" \
  -DOTP_WINDOWS_GETTEXT_ROOT=/nix/store/ywigw24851mjsyanpykx4ps3c3i5nclb-gettext-x86_64-w64-mingw32-1.0 \
  -DBUILD_GUI=OFF -DBUILD_SEARCH_PROVIDER=OFF -DBUILD_TESTING=OFF \
  -DOTP_WINDOWS_SHIM_CHECKS=ON
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
- `umask`/`g_chmod` remain no-ops. Private GIO exports now install/verify a DACL;
  other creation paths (including database/lock/temp files) still rely on inherited
  ACLs. A system-wide equivalent of Unix creation modes is not implemented.
- The password-file DACL audit is deliberately conservative, not a complete
  AccessCheck/group-membership/conditional-ACE evaluator. Unknown ACEs and
  foreign grants that deny ACEs might cancel are rejected. Legitimately private
  complex ACLs can therefore fail the mode check; it is never bypassed.
- File `fsync` maps to `_commit`; parent-directory opening/flushing is unsupported
  and upstream warns. Private exports also flush before handle-based replacement,
  with no parent-directory durability guarantee. Crash-safe replacement/durability
  is unverified. A failed temporary cleanup can leave a user-private partial file.
- CRT signal dispositions now have a console-control cancellation adapter for
  SIGINT/SIGBREAK (SIGHUP alias); see the console interruption follow-up above.
  POSIX masks/flags, true SIGHUP, SIGTERM interruption and close/logoff/shutdown
  cleanup remain unsupported. Console interruption and echo restoration have
  compiled automated coverage but still require native execution.

The lock shim is implemented, not stubbed. Same-process exclusion and other
Windows-specific security/atomicity properties require runtime validation.
This is a compilation spike, not a production-ready Windows port.

### Linux regression and Wine

```sh
source /tmp/claude-1000/-home-sandhilt-workspace-OTPClient-windows/42d068b6-91af-47fd-bf85-ce9c99ea1974/scratchpad/linux-env.sh
cmake -S . -B /tmp/otp-linux-build -G Ninja && \
  cmake --build /tmp/otp-linux-build -j8 && \
  ctest --test-dir /tmp/otp-linux-build --output-on-failure \
    -E '^(review_flows|search_provider_settings|tray_session_.*|gui_flows)$'
```

Linux GUI, CLI, search provider and tests compiled. All 36 tests were registered:
this revision passed 21 and explicitly excluded 15. The previous attempt's 15
session failures occurred before their cases could execute because private D-Bus socket
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

## Manager verification commands

Run outside the sandbox, from the repository root. This rebuilds the changed
libcotp package; the old exported `win-env.sh` cannot verify its `postInstall`.
The native build also provides the compiled GSettings schema used by the smoke.

```sh
nix build .#devShells.x86_64-linux.windows --no-link
nix develop .#default --command bash -c 'cmake -S . -B /tmp/otp-linux-build -G Ninja && cmake --build /tmp/otp-linux-build -j8 && ctest --test-dir /tmp/otp-linux-build --output-on-failure'
nix develop .#windows --command bash <<'BASH'
set -euo pipefail
cmake -S . -B /tmp/otp-win-build -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$PWD/cmake/windows-ucrt.cmake" \
  -DOTP_WINDOWS_GETTEXT_ROOT="$OTP_WINDOWS_GETTEXT_ROOT" \
  -DBUILD_GUI=OFF -DBUILD_SEARCH_PROVIDER=OFF -DBUILD_TESTING=OFF \
  -DOTP_WINDOWS_SHIM_CHECKS=ON
cmake --build /tmp/otp-win-build --clean-first -j8

# Find the cross packages' runtime DLLs in their bin directories, including
# propagated dependencies. Resolve the new shell closure, not old store paths.
read -r -a roots <<< "$buildInputs ${propagatedBuildInputs:-}"
declare -A dll_sources
while IFS= read -r prefix; do
  [[ -d "$prefix/bin" ]] || continue
  while IFS= read -r -d '' dll; do
    name=${dll##*/}
    key=${name,,}
    if [[ -n "${dll_sources[$key]:-}" ]]; then
      cmp -s "${dll_sources[$key]}" "$dll" || {
        echo "Conflicting runtime DLL: $name" >&2; exit 1;
      }
    fi
    dll_sources[$key]=$dll
  done < <(find -L "$prefix/bin" -maxdepth 1 -type f -iname '*.dll' -print0)
done < <(nix-store --query --requisites "${roots[@]}")
[[ -n "${dll_sources[libcotp.dll]:-}" ]] || {
  echo 'libcotp.dll is still missing from the cross package bin output' >&2; exit 1;
}

# This is x86_64-w64-mingw32-objdump for the current toolchain. Read it from
# CMake so DLL staging follows the selected target, rather than a native tool.
objdump=$(sed -n 's/^CMAKE_OBJDUMP:FILEPATH=//p' /tmp/otp-win-build/CMakeCache.txt)
[[ -x "$objdump" ]]
stage=$(mktemp -d /tmp/otp-win-stage.XXXXXX)
cp /tmp/otp-win-build/src/cli/otpclient-cli.exe "$stage/"
cp /tmp/otp-win-build/otp-windows-shim-check.exe "$stage/"
queue=("$stage/otpclient-cli.exe" "$stage/otp-windows-shim-check.exe")
declare -A staged
for ((i=0; i<${#queue[@]}; i++)); do
  image=${queue[$i]}
  while IFS= read -r name; do
    key=${name,,}
    [[ -z "${staged[$key]:-}" ]] || continue
    if [[ -n "${dll_sources[$key]:-}" ]]; then
      cp "${dll_sources[$key]}" "$stage/$name"
      staged[$key]=1
      queue+=("$stage/$name")
    else
      # Windows/Wine provides these system imports and UCRT API sets.
      case "$key" in
        api-ms-*|ext-ms-*|ucrtbase.dll|msvcrt.dll|kernel32.dll|ntdll.dll|advapi32.dll|\
        user32.dll|gdi32.dll|shell32.dll|shlwapi.dll|ole32.dll|oleaut32.dll|\
        ws2_32.dll|bcrypt.dll|crypt32.dll|secur32.dll|iphlpapi.dll|dnsapi.dll|\
        version.dll|winmm.dll|winspool.drv|setupapi.dll|cfgmgr32.dll|comdlg32.dll|comctl32.dll)
          staged[$key]=1 ;;
        *) echo "Missing runtime DLL: $name (imported by $image)" >&2; exit 1 ;;
      esac
    fi
  done < <("$objdump" -p "$image" | awk '/DLL Name:/ { print $3 }')
done

export WINEPREFIX
WINEPREFIX=$(mktemp -d /tmp/otp-wineprefix.XXXXXX)
export WINEDEBUG=-all GSETTINGS_BACKEND=memory
export GSETTINGS_SCHEMA_DIR
GSETTINGS_SCHEMA_DIR=$(winepath -w /tmp/otp-linux-build/tests/schemas)
wine "$stage/otp-windows-shim-check.exe"
wine "$stage/otpclient-cli.exe" --version
wine "$stage/otpclient-cli.exe" --help
wine "$stage/otpclient-cli.exe" --list-types

# Dummy test credentials only. Unix chmod does not set the required Windows
# DACL. Create this password file through the tested private Windows adapter.
mkdir -p "$stage/export"
pw=$(winepath -w "$stage/password.txt")
db=$(winepath -w "$stage/test.db")
uris=$(winepath -w "$stage/uris.txt")
out=$(winepath -w "$stage/export")
wine "$stage/otp-windows-shim-check.exe" --make-password-file "$pw"
cat > "$stage/uris.txt" <<'URI'
otpauth://totp/Example:alice@example.com?secret=JBSWY3DPEHPK3PXP&issuer=Example&algorithm=SHA1&digits=6&period=30
otpauth://hotp/AcmeCorp:bob@acme.com?secret=JBSWY3DPEHPK3PXP&issuer=AcmeCorp&algorithm=SHA1&digits=6&counter=0
URI
printf 'x\n' | wine "$stage/otpclient-cli.exe" --import -t freeotpplus_plain -f "$uris" -d "$db" -p "$pw"
test -s "$stage/test.db"
wine "$stage/otpclient-cli.exe" --list -d "$db" -p "$pw"
wine "$stage/otpclient-cli.exe" --show -d "$db" -p "$pw" -i Example -a alice@example.com -m
wine "$stage/otpclient-cli.exe" --export -t freeotpplus_plain -d "$db" -p "$pw" -o "$out"
test -s "$stage/export/freeotpplus-exports.txt"
wine "$stage/otpclient-cli.exe" --export -t aegis_plain -d "$db" -p "$pw" -o "$out"
test -s "$stage/export/aegis_exports.json"
printf 'dummy-import-password\n' | wine "$stage/otpclient-cli.exe" --export -t authpro_encrypted -d "$db" -p "$pw" -o "$out"
test -s "$stage/export/authpro_encrypted.bin"
authpro=$(winepath -w "$stage/export/authpro_encrypted.bin")
roundtrip=$(winepath -w "$stage/authpro-roundtrip.db")
printf 'dummy-import-password\n' | wine "$stage/otpclient-cli.exe" --import -t authpro_encrypted -f "$authpro" -d "$roundtrip" -p "$pw"
wine "$stage/otpclient-cli.exe" --list -d "$roundtrip" -p "$pw"
printf 'Review test artifacts in %s (Wine prefix %s)\n' "$stage" "$WINEPREFIX"
BASH
```

If Wine cannot persist/report the protected DACL or persistent ACL volume flag,
the private adapter must fail. Do not disable the checks to make this smoke pass.
Native Windows/NTFS verification remains required: run the regression executable,
try reading both plaintext exports from a second non-administrator account,
exercise an encrypted Authenticator Pro backup with a known dummy password,
check exclusive locks across two processes/fallback on an unsupported filesystem,
and paste/type pending console input around the password prompt to validate flush
and echo restoration. Test FAT/exFAT exports for explicit refusal. Test a 32-bit
toolchain with matching 32-bit GLib to verify the ABI branch. Re-run the complete
36-test Linux suite outside the sandbox as shown above.

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
