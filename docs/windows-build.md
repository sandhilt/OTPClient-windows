# Windows CLI cross-build (2026-10-12)

## Current result: core + CLI linked

The Windows build produces `src/cli/otpclient-cli.exe`, a `pei-x86-64` console
executable using UCRT, with all `src/common/*.c` compiled into the existing CLI
target. The latest recorded clean build also linked `otp-windows-shim-check.exe`
and `otp-windows-console-check.exe` without compiler/linker warnings. Architecture
selection lives in `cmake/windows-ucrt.cmake`; scope is x64 first. No upstream
source/build files or DB v3 format were changed. The supplied native MSYS2 logs
now record a successful GUI build with the hook and a five-second startup with
the memory settings backend, but with a missing-schema warning. Portable GUI
staging scripts now exist; their execution and visual result remain unverified
(see the portable GUI section below and `docs/windows-port.md`).

Native Windows 11 results are recorded in the branch history and the verification
matrix below; they were obtained through WSL interop and native console harnesses,
not by running a Linux binary. Security stubs and unverified properties remain;
this is not a production-ready Windows port.

## Shell setup and build recipe

Run from the repository root on x86_64 Linux/WSL with Nix. `direnv allow` enters
the default Linux shell; use `nix develop .#windows` explicitly for the cross
build. Nothing needs to be installed on the host.

```sh
nix build .#devShells.x86_64-linux.windows --no-link
nix develop .#windows
cmake -S . -B /tmp/otp-win-build -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$PWD/cmake/windows-ucrt.cmake" \
  -DOTP_WINDOWS_GETTEXT_ROOT="$OTP_WINDOWS_GETTEXT_ROOT" \
  -DBUILD_GUI=OFF -DBUILD_SEARCH_PROVIDER=OFF -DBUILD_TESTING=OFF \
  -DOTP_WINDOWS_SHIM_CHECKS=ON
cmake --build /tmp/otp-win-build --clean-first -j8
```

The shell includes cross GLib/GIO, libgcrypt, Jansson, libcotp, gettext,
`libgpg-error.dev` and `pcre2.dev`, and exports `OTP_WINDOWS_GETTEXT_ROOT`.
Use its current pkg-config environment; do not mix native libraries into cross
lookup or reuse old exported environments/store paths. Recorded versions were
GCC 16.2.0, GLib 2.88.3, libgcrypt 1.12.4, libcotp 4.1.0 and Jansson 2.15.0.
Optional missing `bash-completion.pc` metadata is non-fatal; upstream falls back
to a relative install directory.

The toolchain loads `cmake/windows.cmake` via `CMAKE_PROJECT_INCLUDE`, skips
compiler probes and uses a deferred hook after the upstream targets exist.
It prepends compat headers, force-includes `compat/otp-windows.h`, attaches
`compat.c`, `console.c`, `security.c` and `private-file.c`, links gettext and
OS `advapi32`, and removes PIE/glibc FORTIFY options. Metadata-only
`cmake/windows/pkgconfig/{uuid,libsecret-1}.pc` satisfies the unconditional
checks; UUID symbols come from the shim and Secret Service is unavailable.

The hook now accepts CLI, GUI or both; search provider, upstream tests and Flatpak
must remain off. The CLI recipe above is unchanged. GUI additionally force-includes
`compat/otp-windows-gui.h` (which includes the common header) and adds
`gui-compat.c`; Unix signal registration and document-portal UID are explicit
stubs. For native MSYS2 UCRT64, set `CMAKE_PROJECT_INCLUDE` explicitly and use
the native compiler rather than `windows-ucrt.cmake`. Select the fork's UUID and
libsecret metadata even when real MSYS2 libsecret is installed; it remains unavailable
through milestone 1. `OTP_WINDOWS_SHIM_CHECKS=ON` builds two separate regression executables,
not ctest targets. A Windows installer is not implemented: the upstream install
rules still include man pages, completions, metainfo, schemas and an unconditional
GTK icon-cache command. Build and stage the executables rather than treating
`cmake --install` as a tested Windows packaging recipe.

### Windows-only dependency overrides

All overrides extend `pkgsCross.ucrt64` in `flake.nix`; native Linux packages
and nixpkgs files are unchanged.

| Package | Cross-build issue | Override |
|---|---|---|
| Jansson | ELF fixup expects `dev/lib/cmake/jansson/janssonTargets-release.cmake`; Windows installs targets in `out/cmake`, DLLs in `out/bin`, pkg-config points to `out/lib` and `dev/include` | empty `postFixup`; OTPClient uses pkg-config |
| libgcrypt | MinGW assembler rejects `-Wa,--noexecstack` in MPI assembly | `--disable-asm`, use C fallbacks |
| libcotp | MinGW linker rejects ELF-only `-z relro/noexecstack` | patch `if(NOT APPLE)` to `if(NOT APPLE AND NOT WIN32)` |
| libcotp DLL | upstream install lacks a `RUNTIME DESTINATION` | appended `postInstall` finds `libcotp.dll`, installs it into `$out/bin` and fails if absent |

The shell build was recorded as successful before the later libcotp DLL-install
override. Clean application/check builds used already available cross packages;
that later package override's rebuild was not confirmed in the recorded review.
The staging recipe below checks for the DLL rather than assuming installation.
The two libcotp fixes are upstream-PR candidates (`docs/upstream-sync.md`).

### Stage runtime DLLs

In the same Windows Nix shell, resolve the current dependency closure and follow
PE imports recursively. Do not copy Windows system DLLs or UCRT API sets from
an unrelated installation. This stages all three executables:

```sh
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
cp /tmp/otp-win-build/otp-windows-console-check.exe "$stage/"
queue=("$stage/otpclient-cli.exe" "$stage/otp-windows-shim-check.exe" "$stage/otp-windows-console-check.exe")
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

printf 'Stage directory: %s\n' "$stage"
```

The recorded x64 stage contains these 13 runtime DLLs (checked against the staged
PE imports; regenerate the list when dependencies/toolchain change):

```text
libcotp.dll
libffi-8.dll
libgcrypt-20.dll
libgio-2.0-0.dll
libglib-2.0-0.dll
libgmodule-2.0-0.dll
libgobject-2.0-0.dll
libgpg-error-0.dll
libiconv-2.dll
libintl-8.dll
libjansson.dll
libmcfgthread-2.dll
libpcre2-8-0.dll
```

## Run natively on Windows

From WSL, copy the stage to a Windows directory on NTFS, then execute the PE
files directly through interop. They run as native Windows processes; Wine is
not involved. Use Windows paths for CLI arguments. For example, after staging:

```sh
mkdir -p /mnt/c/otp-test/bin
cp "$stage"/*.exe "$stage"/*.dll /mnt/c/otp-test/bin/
cd /mnt/c/otp-test/bin
./otpclient-cli.exe --version
./otpclient-cli.exe --help
./otpclient-cli.exe --list-types
./otp-windows-shim-check.exe --tap
./otp-windows-console-check.exe --tap
```

For interactive password/keyboard tests, open a native PowerShell console in
`C:\otp-test\bin`; a WSL pipe/PTY is not the same as a Win32 console input handle.
The hidden-console regression executable creates its own child consoles.

```powershell
$failed = $false
1..3 | ForEach-Object {
    .\otp-windows-console-check.exe --tap
    if ($LASTEXITCODE -ne 0) { $failed = $true }
    .\otp-windows-shim-check.exe --tap
    if ($LASTEXITCODE -ne 0) { $failed = $true }
}
if ($failed) { throw "Windows regression checks failed; review all TAP runs" }
.\otp-windows-shim-check.exe --tap -p /windows/private-inheritable-acl
if ($LASTEXITCODE -ne 0) { throw "Inheritable ACL regression failed" }
```

Use disposable databases and dummy passwords. The shim helper creates a fixed
`smoke-only-password` file with the private Windows adapter. Unix `chmod` does
not set a Windows DACL. A native equivalent of the earlier CLI smoke flow is:

```powershell
New-Item -ItemType Directory -Force 'C:\otp-test\export' | Out-Null
.\otp-windows-shim-check.exe --make-password-file 'C:\otp-test\password.txt'
@'
otpauth://totp/Example:alice@example.com?secret=JBSWY3DPEHPK3PXP&issuer=Example&algorithm=SHA1&digits=6&period=30
otpauth://hotp/AcmeCorp:bob@acme.com?secret=JBSWY3DPEHPK3PXP&issuer=AcmeCorp&algorithm=SHA1&digits=6&counter=0
'@ | Set-Content -Encoding ascii 'C:\otp-test\uris.txt'
'x' | .\otpclient-cli.exe --import -t freeotpplus_plain -f 'C:\otp-test\uris.txt' -d 'C:\otp-test\test.db' -p 'C:\otp-test\password.txt'
.\otpclient-cli.exe --list -d 'C:\otp-test\test.db' -p 'C:\otp-test\password.txt'
.\otpclient-cli.exe --show -d 'C:\otp-test\test.db' -p 'C:\otp-test\password.txt' -i Example -a alice@example.com -m
.\otpclient-cli.exe --export -t freeotpplus_plain -d 'C:\otp-test\test.db' -p 'C:\otp-test\password.txt' -o 'C:\otp-test\export'
.\otpclient-cli.exe --export -t aegis_plain -d 'C:\otp-test\test.db' -p 'C:\otp-test\password.txt' -o 'C:\otp-test\export'
```

Check each exit code and the resulting `test.db`, `freeotpplus-exports.txt` and
`aegis_exports.json`; these commands do not assert every smoke step automatically.
For a real-console prompt omit `-p`, enter a correct/wrong dummy password and
check no echo, typeahead flushing and exact mode restoration. Press Ctrl+C
without Enter, then repeat with Ctrl+Break and partial input; generated-control
results below do not establish physical-key behaviour.

The `-p` refusal still says "chmod 600" (`src/cli/exec-action.c:149`); a Windows
message would need a separate upstream edit. For a file you own, the documented
cmd.exe workaround removes inherited grants and replaces your explicit grant
(replace `<file>` with the actual quoted path):

```bat
icacls <file> /inheritance:r /grant:r %USERNAME%:F
```

Inspect the resulting DACL: this command does not remove explicit grants to
other principals, which must also be removed if present. PowerShell uses
`icacls $file /inheritance:r /grant:r "$($env:USERNAME):F"`.
For an inheritable Builtin Users read ACE repro on Windows pt-BR, use the numeric
SID `*S-1-5-32-545`, not the localized name "Users":

```bat
icacls C:\otp-test\export /grant "*S-1-5-32-545:(OI)(CI)R"
```

The regression test itself uses `WinBuiltinUsersSid` and is locale-independent.

## Verification matrix

These are recorded results from 2026-10-12 and branch commits
`4a35c44`, `e9bd41a`, `2910320` and `1983335`, not new runtime execution of
this documentation update. Later fixes supersede the earlier failures; they do
not expand coverage to the remaining gaps.

| Area | Evidence / result | Scope |
|---|---|---|
| Core + CLI + both check executables | clean cross compile/link, no compiler/linker warnings; x64 UCRT PE imports and staged DLLs inspected | built/inspected, not the upstream Windows ctest suite |
| Earlier CLI smoke | Wine `--version`, `--help`, import/list/show/export recorded in `4a35c44`; native Windows 11 import/list/show and plain exports recorded in `e9bd41a` | CLI smoke, not every importer/exporter or settings persistence |
| Password file | native user-only ACL accepted; inherited/Users-readable grants refused; shim test also checks Everyone read/write and NULL DACL | conservative mode audit; second-user access denial unverified |
| Private rename/inheritable ACL | 5/5 targeted runs; full shim suite 3/3 cases, exit 0; 10/10 CLI exports into a directory with explicit `(OI)(CI)` Users read ACE | exact filename, no `.otp-private-*` leftovers, user-only protected DACL; old binary not rerun as baseline |
| Latest console and shim checks | console 4/4 cases and shim 3/3 cases in three runs each, exit 0, native Windows 11 | generated hidden-console input/control events and adapter tests |
| CLI console Ctrl+C/Ctrl+Break | abort without Enter in under 25 ms, exit 3, console mode restored | Ctrl+C injected by `GenerateConsoleCtrlEvent`, not a physical keypress |
| Earlier password prompts and latest line checks | prompt display/no echo, modes 0x1F7 → 0x1F3 → 0x1F7, typeahead flush and wrong-password rejection recorded; latest backspace, non-ASCII input, successive prompts and piped stdin pass | native harness/generated input; full encrypted import/export prompt cycle still unverified |
| Import fd adapters | native shim tests pass seek/tell SET/CUR/END and initialized JSON errors after descriptor close | full encrypted Authenticator Pro round-trip still unverified |
| DB locks | `LockFileEx` shim compiles; unsupported error mapping inspected | same-process/two-process exclusion and unsupported-filesystem fallback unverified |
| ABI | `_WIN64` selects matching CRT stat struct/function; recorded Win64 layout probe: WCHAR 2 bytes, `FILE_RENAME_INFO` 24 bytes, offsets 0/8/16/20 | 32-bit toolchain/dependency build unverified |
| Settings / GUI | common settings code compiles; no persistent backend selected; GUI excluded | registry persistence, cross-process notifications and GUI unverified |
| Linux regression | initial build history records 36/36 ctest; later review built GUI/CLI/provider/tests, registered 36, passed 21 and explicitly excluded 15 session tests | later full-suite rerun pending; excluded tests are not passes |

### Native rename follow-up

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


The recorded native retest passed on Windows 11/NTFS through WSL interop. The
previous binary was not re-run as a baseline: this confirms the repaired path's
behaviour, not the exact cause of the old failure or its ACE correlation.

### Console interruption and checks

The previous synchronous-read cancellation design hung natively: pending Ctrl+C
and Ctrl+Break timed out without Enter (3/3 each, 15 seconds), and Enter after the
generated control also failed to release a read. Why `CancelSynchronousIo`
failed and why Enter stopped releasing it were not verified. Source inspection
established a persistent interrupt-latch defect; a 12-thread snapshot did not
identify a kernel/CRT/lock cause or support a retry-Sleep explanation.

The current `console.c` removes that cancellation path. It waits on a manual-reset
interrupt event plus the console input handle, calls the installed leaf C handler
before latching the signal, and returns `EINTR` once after wiping the line and
consuming latches. SIG_DFL/SIG_IGN fall through to CRT/OS; registrations/restoration
reset latches and remove the control callback when neither signal has a real
handler. An SRW lock protects actions/line state, released while waiting and
after every input record. Missing dynamically resolved
`ReadConsoleInputExW(CONSOLE_READ_NOWAIT)` returns `ENOSYS`, with no blocking
fallback; it closes the empty-queue race between peek and consumption. Microsoft
documents [dynamic Kernel32 lookup and NOWAIT](https://learn.microsoft.com/en-us/windows/console/readconsoleinputex)
(Windows 7 minimum for that API); this does not establish the port's OS support floor.

Key-down Unicode records are assembled manually with repeats, UTF-8 backspace,
surrogate pairs and U+FFFD for unpaired surrogates; other records are ignored.
Enter emits LF, with no CR/LF left for the next prompt. The secure line buffer
checks `gcry_is_secure` and fails with `ENOMEM` instead of an ordinary-heap
fallback. Served/deleted/discarded bytes, old allocations and scratch records are
wiped. `TCSAFLUSH` discards OS input and buffered suffixes (including after the
CLI's 511-byte limit), preserving pending signals. No typed input is echoed or
logged. Non-console and zero-length reads retain CRT semantics.

`console-check.c` compiles its wait handshake only into the console-check target,
not the CLI or shim checks. Hidden child consoles verify a read stays pending
for 100 ms without Enter before generating controls; successful interrupted reads
must finish within 2 seconds. Children are bounded to 8 seconds and timed-out
children are terminated/reaped; nonfatal parent assertions continue the TAP run.
Injected ^C key records did not generate controls in the native harness;
`GenerateConsoleCtrlEvent` exercises the callback but not a physical key.

The four native-passing cases are:

- `/windows/console-interrupt`: 20 cycles each of pending Ctrl+C/Ctrl+Break,
  early and combined controls, restored `raise(SIGINT)`, SIG_IGN; noise/partial
  Unicode, Enter after EINTR with the same handlers, mode restoration/delegation.
- `/windows/console-lines`: 20 prompt cycles, no screen/cursor changes,
  backspace/repeats, BMP/supplementary/unpaired UTF-16, typeahead, secure-buffer
  growth, 511-byte truncation then flushing and another prompt.
- `/windows/console-default`: default Ctrl+C/Ctrl+Break exits compared with CRT
  `_read` child baselines.
- `/windows/nonconsole-read`: pipe/file CRLF and Ctrl+Z translation, binary bytes,
  NUL EOF and zero-length reads.

The three shim cases cover private creation/replacement/cancelled close and
DACL-derived modes; inheritable ACL/Unicode/long-path rename and failure cleanup;
and fd seeking/initialized JSON errors. They do not test actual second-user access.

## Stubs and remaining gaps

Every incomplete semantic is marked `WINDOWS-STUB` in the platform code:

- Secret Service returns `G_IO_ERROR_NOT_SUPPORTED` (or cancellation), with
  GTask async flow. No credentials are stored; no native backend is selected.
- `prctl(PR_SET_DUMPABLE)` and `setrlimit` fail with `ENOSYS`, causing upstream
  warnings. Crash dumps are not prevented.
- `getrlimit(RLIMIT_MEMLOCK)` supplies a fixed 64 MiB budget, not a measured
  page-lock limit. The unchanged sizing code reserves 1 MiB headroom, so this
  budget yields a 63 MiB pool; libgcrypt secure allocation remains in use.
  Windows page-lock guarantees and Argon2id allocation-failure behaviour are unverified.
- `umask`/`g_chmod` are no-ops. Private GIO exports install/verify a DACL, while
  DB/lock/temp files and copy/relocation paths still rely on inherited ACLs.
- The password-file audit is conservative, not a full AccessCheck/group or
  conditional-ACE evaluator. Unknown ACL/ACE forms reject; deny ACEs do not
  cancel foreign grants. Current user, SYSTEM and Administrators are trusted.
  Owner bits retain CRT read-only state. Query failure returns `EACCES`; the
  upstream mode check is never bypassed. Complex private ACLs may be refused.
- File `fsync` maps to `_commit`; parent-directory open at `db-common.c:283`
  fails before flush and warns at `:347`. Import can perform several replacements
  and emit multiple warnings. Do not return success to hide missing durability.
  Private exports flush their held handle, but parent-directory fsync/crash
  durability is still unverified. Failed cleanup can leave a user-private artifact.
- Console support is limited to one reader/thread and no-echo UTF-8 password
  lines, not cooked history/cursor editing, code-page output or binary-console
  CRLF emulation. POSIX masks/flags, true SIGHUP (aliased to SIGBREAK), asynchronous
  SIGTERM, close/logoff/shutdown cleanup, debugger interception and console
  reattachment remain unsupported. Handlers must be leaf functions with no CRT
  I/O or shim reentry. Physical keyboard Ctrl+C remains unverified.

Still required: read plaintext exports as a second non-administrator account;
exercise FAT/exFAT explicit refusal and other filesystems/minifilters; test crash
durability and lock exclusion/fallback; verify persistent GSettings and relocatable
locales, a matching 32-bit toolchain/GLib, and complete legacy v1/v2 DB fixtures.
Administrators' ownership/privilege overrides are outside ordinary-user privacy.

For a full encrypted Authenticator Pro round-trip and successive real CLI prompts,
use a disposable backup with known dummy encryption passwords:

```powershell
.\otpclient-cli.exe --import -t authpro_encrypted -f 'C:\otp-test\dummy-authpro.bin' -d 'C:\otp-test\roundtrip.db'
# Supply the DB password, then the backup's dummy encryption password.
.\otpclient-cli.exe --list -d 'C:\otp-test\roundtrip.db'
.\otpclient-cli.exe --export -t authpro_encrypted -d 'C:\otp-test\roundtrip.db' -o 'C:\otp-test\export'
# Supply the DB password, then a new dummy export encryption password.
# Re-import that export into another disposable DB and compare tokens.
```

## Linux regression and Wine

Run the full Linux suite separately in the default shell:

```sh
nix develop .#default --command bash -c 'cmake -S . -B /tmp/otp-linux-build -G Ninja && cmake --build /tmp/otp-linux-build -j8 && ctest --test-dir /tmp/otp-linux-build --output-on-failure'
```

Later restricted reviews could not bind the private D-Bus socket
(`Failed to bind socket ...: Operation not permitted` in
`tests/isolated-session.py:45`). They passed 21 non-session tests with
`-E '^(review_flows|search_provider_settings|tray_session_.*|gui_flows)$'` and
explicitly excluded 15; this is not 36/36. Rerun the full suite outside those
socket restrictions. Similarly, early Nix attempts stopped before shell entry
at `/nix/var/nix/daemon-socket/socket: Operation not permitted`; those attempts
were not builds. Shell evaluation alone was superseded by actual cross builds.

Wine is a quick headless check after DLL staging, not proof of native ACL
enforcement or console behaviour:

```sh
export WINEPREFIX
WINEPREFIX=$(mktemp -d /tmp/otp-wineprefix.XXXXXX)
export WINEDEBUG=-all GSETTINGS_BACKEND=memory
# Optional settings smoke: supply a compiled schema, using Windows path syntax.
# export GSETTINGS_SCHEMA_DIR=$(winepath -w /tmp/otp-linux-build/tests/schemas)
wine "$stage/otpclient-cli.exe" --version
wine "$stage/otpclient-cli.exe" --help
wine "$stage/otpclient-cli.exe" --list-types
wine "$stage/otp-windows-shim-check.exe" --tap
```

An earlier Wine smoke passed; later restricted attempts stopped before executable
load with `sock_init: ERROR in sock_check_pollhup()` / `wineserver: bind: Operation
not permitted`. Those blocked invocations are not application failures or new
verification. For Wine import/export, convert each CLI path with `winepath -w`
and create the password file with the shim helper. If Wine cannot persist/report
the protected DACL or persistent-ACL volume flag, the private adapter must fail;
do not disable its checks to obtain a pass.

## Portable GUI zip: staging and runtime (2026-10-12)

The supplied `51-gui-build-with-hook.log` reports a successful native UCRT64
compile/link. `80-gui-start.log` and its stderr record a process still alive
at five seconds (`timeout_rc=124`), with `GSETTINGS_BACKEND=memory` and
`Settings schema 'com.github.paolostivanin.OTPClient' is not installed`.
This is prior-run evidence, not a new execution of the packaging scripts or
proof of a usable window. This packaging change was checked by reading sources,
package files, PE headers/import tables and logs, plus Bash syntax checks.
Windows programs, PowerShell, Nix, CMake builds and visual checks were not run.

`packaging/windows/stage-gui.sh` runs offline inside the portable MSYS2 UCRT64
shell. Its only destination is its staging-directory argument, default
`/c/Users/hilt/AppData/Local/otp-dev/stage/otpclient`. Inputs are the repository,
`build-gui-hook/src/gui/otpclient.exe`, optional CLI from that build or
`build-cli/src/cli/otpclient-cli.exe`, `prefix/bin`, and `/ucrt64`. Environment
overrides are `OTP_DEV_ROOT`, `OTP_SOURCE_ROOT`, `OTP_GUI_BUILD`, `OTP_CLI_BUILD`,
`OTP_PREFIX_ROOT`, `OTP_UCRT_ROOT`, and `OTP_OBJDUMP`. Use the matching UCRT64
pkg-config environment when choosing a different runtime root.

The destination must be empty or carry `.otpclient-stage`; input overlap is
refused. Reruns replace generated `bin/`, `lib/`, `share/` and launcher/metadata
files while preserving `config/` and `logs/`. It does not install packages, call
`cmake --install`, modify the source data, or change persistent environment
variables. Close staged processes before restaging. An interrupted stage is
incomplete and must be rerun; the marker alone is not verification.

```text
otpclient/
  .otpclient-stage
  otpclient.cmd, otpclient.ps1, capture-window.ps1
  check-stage.sh, runtime-common.sh       maintenance checks need MSYS2
  dll-manifest.tsv, icon-names.txt, pixbuf-loaders-dir.txt
  bin/
    otpclient.exe, otpclient-cli.exe      CLI optional
    gsettings.exe, gdk-pixbuf-query-loaders.exe
    *.dll                               recursively resolved non-system imports
    gio-querymodules.exe                only if GIO extensions exist
  lib/
    gdk-pixbuf-2.0/<ABI>/loaders/*.dll
    gdk-pixbuf-2.0/<ABI>/loaders.cache
    gio/modules/                        optional DLLs and giomodule.cache
  share/
    glib-2.0/schemas/*.gschema.xml, gschemas.compiled
    icons/hicolor/, icons/Adwaita/
    gtk-4.0/, mime/, libwmf/              present runtime data trees
    locale/<language>/LC_MESSAGES/*.mo   available selected catalogs
  config/glib-2.0/settings/keyfile        created only with the keyfile option
  logs/                                 check diagnostics, exclude from release zip
```

The script seeds `objdump -p` traversal with both app executables, runtime
helpers, every installed pixbuf loader and any GIO extension DLLs. Dependencies
come only from `prefix/bin` and UCRT64 `bin`; conflicting same-named DLLs fail.
Windows `System32` DLLs and API-set imports remain OS-provided. Unknown imports
fail rather than silently producing an incomplete zip. `dll-manifest.tsv`
records each import/source. This covers PE imports plus those explicitly seeded
plugins; arbitrary late-loaded codecs/media/TLS modules are not proven by an
import table. Video/webcam integration remains outside milestone 1.

The project's unchanged `data/*.gschema.xml` and GTK4 settings/inspector schemas
are copied and compiled with `glib-compile-schemas --strict`; GTK Demo schemas,
developer codegen/GDB data, desktop/service files and Linux search-provider
metadata are excluded. The app's UI/CSS resource is already embedded by
`src/gui/CMakeLists.txt` / `gui.gresource.xml`; it needs no loose `.ui` file.
GTK and libadwaita DLLs carry their built-in widget resources/styles. GTK emoji
data and MIME metadata are copied when available. Fonts come from Windows;
the minimal theme is not a replacement for the complete desktop icon set.

The icon selection is extracted from C strings and `src/gui/ui/*.ui`, including
dialogs and the UI files in `gui.gresource.xml`, plus twelve common GTK widget
fallbacks. The two unchanged project SVGs go in hicolor. Matching Adwaita SVG /
symbolic PNG files and RTL variants retain their original relative paths. The
staged Adwaita `index.theme` inherits only hicolor, dropping the unstaged
AdwaitaLegacy theme. No source theme or stale package `icon-theme.cache` is
copied. GTK can scan these directories without a cache; additional toolkit or
file-chooser icons still need visual verification.

### Loader relocation and process environment

The launcher runs the bundled `gdk-pixbuf-query-loaders.exe` on the staged DLLs,
with a 15-second tool timeout, at staging initialization and again on **every
launch**, including after moving/extracting the zip. It writes UTF-8 without a
BOM, removes build-directory comments and normalizes module entries to absolute
forward-slash paths inside the **current** stage. It rejects external/missing
paths, stderr diagnostics or a count differing from the staged modules, then
replaces the cache. GIO extension caches, when applicable, are regenerated too.
This intentionally avoids relying on relative-cache support in a particular
pixbuf build; the supplied older query output actually contains relative paths,
and upstream supports them when built with `GDK_PIXBUF_RELOCATABLE`
([source](https://github.com/GNOME/gdk-pixbuf/blob/master/gdk-pixbuf/gdk-pixbuf-io.c)).
The chosen first-launch regeneration still works with an absolute-only build.
The stage must be writable; read-only media are not supported by this launcher.
Simultaneous first launches and UNC/Unicode paths have not been tested.

`otpclient.cmd` invokes Windows PowerShell with a command-local policy override;
`otpclient.ps1` sets these variables only for its process and child app:

| Variable | Value / purpose |
|---|---|
| `GSETTINGS_SCHEMA_DIR` | `<stage>/share/glib-2.0/schemas` |
| `GSETTINGS_BACKEND` | `registry` initially; `keyfile` is an explicit option |
| `GDK_PIXBUF_MODULE_FILE` | regenerated `<stage>/lib/gdk-pixbuf-2.0/<ABI>/loaders.cache` |
| `GDK_PIXBUF_MODULEDIR` | staged loader directory, for the query helper |
| `XDG_DATA_DIRS` | `<stage>/share`; Windows separates multiple directories with `;` |
| `GIO_MODULE_DIR` | `<stage>/lib/gio/modules` |
| `PATH` | only `<stage>/bin` and Windows OS directories; no MSYS2 runtime on the app path |
| `XDG_CONFIG_HOME` | `<stage>/config` only when choosing `keyfile` |

Registry is the first probe, **not a settled storage policy**. GLib's Windows
registry backend uses `HKCU\Software\GSettings` by default
([GLib API](https://docs.gtk.org/gio-win32/func.registry_settings_backend_new.html)).
Settings therefore remain per-user and do not travel with the zip.

For a portable keyfile, change the single default line
`[string]$SettingsBackend = 'registry'` to `'keyfile'` in the staged launcher,
or call `otpclient.ps1 -SettingsBackend keyfile` (also accepted via the `.cmd`).
This sets `XDG_CONFIG_HOME=<stage>/config`, so the backend file is
`<stage>/config/glib-2.0/settings/keyfile`; it also relocates other application
config files. GLib chooses that filename internally
([backend source](https://github.com/GNOME/glib/blob/main/gio/gkeyfilesettingsbackend.c));
Windows GLib honors `XDG_CONFIG_HOME`
([directory source](https://github.com/GNOME/glib/blob/main/glib/gutils.c)).
If roaming storage is preferred, replace that assignment with
`Join-Path $env:APPDATA 'OTPClient'`; the keyfile then lives at
`%APPDATA%\OTPClient\glib-2.0\settings\keyfile`. The database/data directory is
not relocated by this packaging change. Backend switching does not migrate
settings or make those storage locations equivalent. Persistence, notifications
and settings export/import require separate native tests.

The current recorded PE has subsystem **3 (console)**. Upstream GUI CMake uses
ordinary `add_executable`, with no `WIN32` or `-mwindows`. The fork-only deferred
hook now exposes `OTP_WINDOWS_GUI_SUBSYSTEM=ON` by default and sets the GUI target's
`WIN32_EXECUTABLE` property (MinGW: `-mwindows`). Reconfigure/relink to obtain
subsystem **2 (GUI)**; set the option OFF for console diagnostics. CLI and check
targets retain consoles. The launcher reads the PE subsystem and uses
`ProcessStartInfo.CreateNoWindow` for subsystem 3, suppressing its console
without hiding the GTK window. The `.cmd` host itself can briefly flash when
double-clicked; native suppression/normal GUI presentation remains unverified.
No upstream CMake edit is needed or proposed.

### Translations and remaining differences

`po/` contains only two `.pot` templates, with no `.po` / `.mo` files. There is
no translation compilation/install rule in root or GUI CMake. The currently
exported `build-gui-hook/version.h` defines `LOCALE_DIR` as `/locale` (the
`SHARE_INSTALL_PREFIX` template input was unset). Staging copies optional app
catalogs found under the build/prefix `share/locale`, and available GTK40,
libadwaita, GLib and pixbuf catalogs. Copying app catalogs does **not** make
`main.c`'s configured `bindtextdomain` path relocatable. App translations are
absent in this build, and their future relocation needs a separate fork-only
adapter; no upstream edit is proposed here. Toolkit catalog lookup also needs
native relocation verification.

The existing signal, hardening and secret-service stubs remain. A compiled
schema does not implement registry/keyfile parity, crash-dump prevention,
memlock sizing, ACL guarantees, directory durability, Windows notifications,
session lock, autostart, tray or camera capture. Schema defaults are preserved,
including a search-provider setting whose provider is not built on Windows.
Some desktop-integration UI rows still remain. Milestone 1 is not complete
solely because startup diagnostics pass.

### Manager: exact next native sequence

First transfer the **working-tree** `packaging/windows/`, modified
`cmake/windows.cmake` and documentation to the exported
`C:\Users\hilt\AppData\Local\otp-dev\src\repo`. `git archive HEAD` alone omits
these uncommitted/new files. Copy the supplied `90-stage-gui.sh` and
`91-check-stage.sh` wrappers to `otp-dev\scripts`, alongside the existing
`51-gui-build-with-hook.sh`. Close existing OTPClient instances. No package
installation/network step is needed for the already provisioned tree.

In native Windows PowerShell, stop on the first nonzero exit:

```powershell
$ot = 'C:\Users\hilt\AppData\Local\otp-dev'
$runner = Join-Path $ot 'run-msys2.ps1'
& $runner -Script '/c/Users/hilt/AppData/Local/otp-dev/scripts/51-gui-build-with-hook.sh'
if ($LASTEXITCODE -ne 0) { throw 'GUI rebuild failed' }
& $runner -Script '/c/Users/hilt/AppData/Local/otp-dev/scripts/90-stage-gui.sh'
if ($LASTEXITCODE -ne 0) { throw 'GUI staging failed' }
& $runner -Script '/c/Users/hilt/AppData/Local/otp-dev/scripts/91-check-stage.sh'
if ($LASTEXITCODE -ne 0) { throw 'Registry stage checks failed' }
# Optional keyfile comparison after registry; this creates stage/config.
& $runner -Command 'bash /c/Users/hilt/AppData/Local/otp-dev/scripts/91-check-stage.sh keyfile'
if ($LASTEXITCODE -ne 0) { throw 'Keyfile stage checks failed' }
# Use a separate PowerShell process so the capture is bounded and returns a status.
& powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$ot\stage\otpclient\capture-window.ps1" `
    -StageDir "$ot\stage\otpclient" -OutputPath "$ot\logs\92-otpclient-window.png" -SettingsBackend registry
if ($LASTEXITCODE -ne 0) { throw 'Capture or graceful shutdown failed' }
```

The wrappers copy their repository scripts from the exported source before
running, validate Bash syntax and log PASS/FAIL plus duration to
`otp-dev/logs/90-stage-gui.log` and `91-check-stage.log`. Script 90 records
source hashes; script 91 keeps detailed check artifacts in the stage's `logs/`.
Run the PS parser before the sequence if reviewing in isolation:

```powershell
foreach ($name in 'otpclient.ps1', 'capture-window.ps1') {
    $tokens = $null; $errors = $null
    [System.Management.Automation.Language.Parser]::ParseFile(
        "$ot\src\repo\packaging\windows\$name", [ref]$tokens, [ref]$errors) | Out-Null
    if ($errors.Count) { $errors | Format-List; throw "PowerShell parse failed: $name" }
}
```

`check-stage.sh [stage] [registry|keyfile] [--static-only]` prints separate
PASS/FAIL lines for PE/`ldd` closure (rejecting build-tree fallback), schema
lookup, backend read, launcher/cache initialization and a six-second startup.
It greps stderr for missing schemas/icons and image-loader errors. Expected
hardening/signal warnings are explicitly reported as remaining limitations.
The timed probe terminates the process (`124` / `137` expected); this is not a
clean-close or clipboard-cleanup test. No DISPLAY, Xvfb or visual inspection is
required for the checker. An unavailable interactive Windows desktop can still
prevent actual GTK startup; use `--static-only` to skip that step, which prints
SKIP and never claims a complete runtime pass. Its schema/cache helpers still
execute natively. Do not run the full startup probe against an existing app
instance: early activation exits are reported as failures.

`capture-window.ps1` was prepared but deliberately not run. It refuses existing
OTPClient processes, launches the staged app, waits up to 30 seconds for its
visible main-window handle, requests foreground, allows two seconds for paint,
and saves the window rectangle via System.Drawing / CopyFromScreen. It closes
only the newly launched process, waits five seconds and reports a failure if
forced termination is needed. It refuses to overwrite an existing PNG. Use a
new output filename on reruns. Screen occlusion, off-screen content, DPI/GPU
rendering and remote-desktop behavior need the owner's visual review; a saved
PNG is not itself approval of the UI.

Before releasing a zip, the manager must also exercise ordinary launch/close,
settings persistence across launches and notifications across processes,
password/unlock, database reopen, import/export, QR image/display and clipboard
behavior. Test an extracted copy in a different directory (including a space
or Unicode name), launched through its `.cmd`/`.ps1` without MSYS2 on PATH.
After review, create the zip from a **fresh dedicated stage**, not one containing
`config/` or `logs/` from these probes. Windows dependencies' licensing/notice
assembly and signing are not implemented by this staging script; this is a
milestone probe directory, not a release-approved distribution.

Read-only validation of the existing native tree for this change found 16
pixbuf loader DLLs, 85 non-system DLLs in the import closure of app/CLI/helpers/
loaders, no unresolved imports or differing same-named DLLs, and all 32 selected
icon names in Adwaita. Those are static snapshot counts, not staged/runtime
results. All 15 relevant Bash files (packaging helpers, numbered wrappers and
prior run2 shell helpers) pass individual `bash -n` checks. PowerShell syntax,
staging idempotence in MSYS2, actual schema/backend/cache runtime, subsystem-2
relink, console suppression, GUI function and screenshot remain **UNVERIFIED**.
