#!/usr/bin/env bash
# No DISPLAY/Xvfb needed: static checks and a timed Win32 startup probe.
set -Eeuo pipefail
SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
source "$SCRIPT_DIR/runtime-common.sh"
require_msys
[[ $# -le 3 ]] || fail 'Usage: check-stage.sh [stage] [registry|keyfile] [--static-only]'
STAGE=$(realpath -e -- "${1:-${OTP_DEV_ROOT:-/c/Users/hilt/AppData/Local/otp-dev}/stage/otpclient}")
BACKEND=${2:-registry}
[[ $BACKEND == registry || $BACKEND == keyfile ]] || fail 'Select registry or keyfile'
[[ -z ${3:-} || ${3:-} == --static-only ]] || fail 'Unknown check option'
[[ -f "$STAGE/.otpclient-stage" ]] || fail 'Not an OTPClient staging directory'
mkdir -p "$STAGE/logs"
LOGS="$STAGE/logs"
POWERSHELL="$SYSTEM_DIR/WindowsPowerShell/v1.0/powershell.exe"
LDD=$(command -v ldd) || fail 'ldd is required'
TIMEOUT=$(command -v timeout) || fail 'timeout is required'
failures=0
bad() { echo "FAIL: $*"; failures=$((failures + 1)); }
pass() { echo "PASS: $*"; }

# Use only this stage and the OS at runtime; no UCRT64/prefix bin directories.
export PATH="$STAGE/bin:/usr/bin:$SYSTEM_DIR"
export GSETTINGS_SCHEMA_DIR
GSETTINGS_SCHEMA_DIR=$(cygpath -m "$STAGE/share/glib-2.0/schemas")
export GSETTINGS_BACKEND="$BACKEND"
export XDG_DATA_DIRS
XDG_DATA_DIRS=$(cygpath -m "$STAGE/share")
export GIO_MODULE_DIR
GIO_MODULE_DIR=$(cygpath -m "$STAGE/lib/gio/modules")
if [[ $BACKEND == keyfile ]]; then
    export XDG_CONFIG_HOME
    XDG_CONFIG_HOME=$(cygpath -m "$STAGE/config")
fi

images=()
while IFS= read -r -d '' file; do images+=("$file"); done < <(
    find "$STAGE/bin" "$STAGE/lib" -type f \( -iname '*.exe' -o -iname '*.dll' \) -print0)
[[ ${#images[@]} -gt 0 && -f "$STAGE/bin/otpclient.exe" ]] || fail 'No staged executables'
declare -A staged_dlls=() checked_paths=()
for image in "${images[@]}"; do
    if [[ ${image%/*} == "$STAGE/bin" ]]; then
        name=${image##*/}
        staged_dlls[${name,,}]=1
    fi
done
stage_lower=${STAGE,,}; system_lower=${SYSTEM_DIR,,}
winsxs_lower=${system_lower%/*}/winsxs
: > "$LOGS/ldd.txt"
before=$failures
closure_started=$SECONDS
for image in "${images[@]}"; do
    imports=$(pe_imports "$image") || { bad "Cannot read imports: $image"; continue; }
    while IFS= read -r name; do
        [[ -n $name ]] || continue
        is_system_dll "$name" && continue
        [[ -n ${staged_dlls[${name,,}]:-} ]] || bad "Missing DLL $name imported by ${image#"$STAGE/"}"
    done <<< "$imports"
    printf '\n%s\n' "${image#"$STAGE/"}" >> "$LOGS/ldd.txt"
    # MSYS ldd uses the native loader/debug machinery. Keep the deliberate
    # crash executable strictly opt-in, even if ldd normally stops before main.
    if [[ ${image##*/} == otp-windows-crash-probe.exe ]]; then
        printf 'SKIP: deliberate crash probe; PE imports checked, no ldd\n' >> "$LOGS/ldd.txt"
        continue
    fi
    if output=$("$LDD" "$image" 2>&1); then
        printf '%s\n' "$output" >> "$LOGS/ldd.txt"
    else
        printf '%s\n' "$output" >> "$LOGS/ldd.txt"
        bad "ldd failed for ${image#"$STAGE/"} (see logs/ldd.txt)"
    fi
    if grep -qiE 'not found|No such file|cannot find' <<< "$output"; then
        grep -iE 'not found|No such file|cannot find' <<< "$output" || true
        bad "ldd reports missing DLLs for ${image#"$STAGE/"}"
    fi
    # Reject fallback to the build dependency tree, even if ldd resolves it.
    while IFS= read -r resolved; do
        resolved=${resolved%$'\r'}
        # ldd recursively repeats the same DLL paths for almost every image.
        # Validate each distinct spelling once, retaining every ldd invocation
        # and its diagnostics. Avoid thousands of MSYS child processes.
        [[ -z ${checked_paths[$resolved]:-} ]] || continue
        checked_paths[$resolved]=1
        resolved=$(cygpath -u "$resolved")
        resolved=$(realpath -m -- "$resolved")
        resolved_lower=${resolved,,}
        if [[ $resolved_lower != "$stage_lower/"* && $resolved_lower != "$system_lower/"* &&
              $resolved_lower != "$winsxs_lower/"* ]]; then
            bad "External DLL resolved by ldd: $resolved"
        fi
    done < <(sed -nE 's/^.* => (.*) \(0x[[:xdigit:]]+\).*$/\1/p' <<< "$output")
done
echo "INFO: PE/ldd duration=$((SECONDS - closure_started))s; ${#checked_paths[@]} distinct resolved paths"
(( failures != before )) || pass "PE imports and ldd closure (${#images[@]} images)"

if "$STAGE/bin/gsettings.exe" list-schemas > "$LOGS/schemas.txt" 2> "$LOGS/schemas.stderr" &&
    grep -qx 'com.github.paolostivanin.OTPClient' < <(tr -d '\r' < "$LOGS/schemas.txt"); then
    pass 'GSettings schema found'
else
    bad 'GSettings schema not found (see logs/schemas.stderr)'
fi
# Read a key to actually select a backend; list-schemas alone does not do so.
if "$STAGE/bin/gsettings.exe" get com.github.paolostivanin.OTPClient dark-theme \
    > "$LOGS/backend.txt" 2> "$LOGS/backend.stderr" &&
    ! grep -qiE 'failed|unable|not found|not supported|memory' "$LOGS/backend.stderr"; then
    pass "$BACKEND backend read (persistence and changed notifications still unverified)"
else
    bad "$BACKEND backend initialization/read (see logs/backend.stderr)"
fi

if "$POWERSHELL" -NoProfile -NonInteractive -ExecutionPolicy Bypass \
    -File "$(cygpath -m "$STAGE/otpclient.ps1")" -SettingsBackend "$BACKEND" -InitializeOnly \
    > "$LOGS/initialize.stdout" 2> "$LOGS/initialize.stderr"; then
    pass 'Launcher runtime initialization'
else
    bad 'Launcher runtime initialization (see logs/initialize.stderr)'
fi
loaders=$(tr -d '\r\n' < "$STAGE/pixbuf-loaders-dir.txt")
[[ $loaders =~ ^lib/gdk-pixbuf-2\.0/[0-9.]+/loaders$ ]] || fail 'Invalid loader directory'
CACHE="$STAGE/${loaders%/loaders}/loaders.cache"
before=$failures
count=0
if [[ -s $CACHE ]]; then
    # The launcher normalizes module entries to unescaped forward-slash paths.
    while IFS= read -r entry; do
        count=$((count + 1))
        path=${entry#\"}; path=${path%\"}
        path=$(cygpath -u "$path")
        path=$(realpath -m -- "$path")
        [[ ${path,,} == "${STAGE,,}/"* && -f $path ]] || bad "Loader outside stage or missing: $entry"
    done < <(tr -d '\r' < "$CACHE" | grep -iE '^".*\.dll"$' || true)
    (( count > 0 )) || bad 'Cache contains no loader entries'
    expected=$(find "$STAGE/$loaders" -type f -iname '*.dll' | wc -l)
    (( count == expected )) || bad "Loader count differs: cache=$count staged=$expected"
else
    bad 'Missing loaders cache'
fi
(( failures != before )) || pass "Loaders cache confined to stage ($count modules)"

if [[ ${3:-} == --static-only ]]; then
    echo 'SKIP: 6-second startup (--static-only); runtime suite incomplete'
else
    export GDK_PIXBUF_MODULE_FILE
    GDK_PIXBUF_MODULE_FILE=$(cygpath -m "$CACHE")
    export GDK_PIXBUF_MODULEDIR
    GDK_PIXBUF_MODULEDIR=$(cygpath -m "$STAGE/$loaders")
    # GNU timeout on MSYS2 supervises the native process. Do not equate an early
    # clean exit (e.g. activation of an existing instance) with a startup pass.
    started=$SECONDS
    set +e
    (cd "$STAGE" && "$TIMEOUT" --kill-after=2s 6s "$STAGE/bin/otpclient.exe") \
        > "$LOGS/startup.stdout" 2> "$LOGS/startup.stderr"
    rc=$?
    set -e
    elapsed=$((SECONDS - started))
    if (( (rc == 124 || rc == 137) && elapsed >= 6 )); then
        pass 'GUI stayed alive for 6 seconds (timeout termination; appearance unverified)'
    else
        bad "GUI exited early or timeout failed: rc=$rc elapsed=${elapsed}s"
    fi
    if grep -qiE "schema.*(not installed|not found)|No GSettings schemas|[Mm]issing.*icon|[Ff]ailed.*icon|[Cc]ould not.*icon|[Uu]nable.*icon|[Ii]con.*(not found|missing)|[Ll]oader.*(error|failed|cannot|unable)|[Cc]annot.*(pixbuf|loader)|[Ff]ailed.*(pixbuf|loader)|[Uu]nable.*(pixbuf|loader)|Unrecognized image|Couldn't recognize.*image" \
        "$LOGS/startup.stderr"; then
        cat "$LOGS/startup.stderr"
        bad 'Schema/icon/pixbuf startup diagnostics'
    else
        pass 'No schema/icon/pixbuf startup diagnostics'
    fi
    if grep -qiE 'core dumps|RLIMIT_CORE' "$LOGS/startup.stderr"; then
        bad 'Crash protection installation failed (see logs/startup.stderr)'
    fi
    if grep -qiE 'Windows locale initialization failed|Cannot (resolve Windows executable-relative locale directory|bind Windows application message catalogs)' "$LOGS/startup.stderr"; then
        bad 'Application locale initialization/binding failed (see logs/startup.stderr)'
    fi
    if grep -qi 'WINDOWS-STUB' "$LOGS/startup.stderr"; then
        echo 'INFO: remaining Windows stubs (see logs/startup.stderr)'
    fi
fi
if (( failures )); then fail "$failures stage check(s) failed; logs: $LOGS"; fi
if [[ ${3:-} == --static-only ]]; then
    pass 'Static stage checks only'
else
    pass 'Stage checks (visual review, persistence, functional GUI tests still pending)'
fi
