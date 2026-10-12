#!/usr/bin/env bash
# Shared PE dependency checks; sourced by the staging and verification scripts.
fail() { printf 'FAIL: %s\n' "$*" >&2; exit 1; }

pe_imports() {
    local output
    output=$("$OBJDUMP" -p "$1") || return
    awk '/DLL Name:/ { sub(/^.*DLL Name:[[:space:]]*/, ""); sub(/\r$/, ""); print }' <<< "$output"
}

is_system_dll() {
    local name=${1,,}
    case "$name" in
        api-ms-*|ext-ms-*) return 0 ;;
    esac
    # Check the running OS, rather than silently accepting unknown imports.
    [[ -f "$SYSTEM_DIR/$1" ]]
}

require_msys() {
    [[ ${MSYSTEM:-} == UCRT64 ]] || fail 'Run inside the portable MSYS2 UCRT64 shell'
    command -v cygpath >/dev/null || fail 'cygpath is required'
    local windows_root=${SYSTEMROOT:-${SystemRoot:-}}
    [[ -n $windows_root ]] || fail 'SYSTEMROOT is required'
    SYSTEM_DIR=$(cygpath -u "$windows_root/System32")
    OBJDUMP=${OTP_OBJDUMP:-/ucrt64/bin/objdump.exe}
    [[ -x "$OBJDUMP" ]] || fail "Missing UCRT64 objdump: $OBJDUMP"
}
