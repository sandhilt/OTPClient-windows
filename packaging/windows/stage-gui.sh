#!/usr/bin/env bash
# Offline staging for the portable UCRT64 build; owns only the destination tree.
set -Eeuo pipefail
SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
source "$SCRIPT_DIR/runtime-common.sh"
require_msys
trap 'rc=$?; if (( rc == 0 )); then echo "PASS: stage-gui"; else echo "FAIL: stage-gui rc=$rc"; fi' EXIT

DEV=${OTP_DEV_ROOT:-/c/Users/hilt/AppData/Local/otp-dev}
REPO=$(cd -- "${OTP_SOURCE_ROOT:-$SCRIPT_DIR/../..}" && pwd)
UCRT=${OTP_UCRT_ROOT:-/ucrt64}
PREFIX=${OTP_PREFIX_ROOT:-$DEV/prefix}
GUI_BUILD=${OTP_GUI_BUILD:-$DEV/build-gui-hook}
CLI_BUILD=${OTP_CLI_BUILD:-$DEV/build-cli}
export PATH="$PREFIX/bin:$UCRT/bin:/usr/bin:$PATH"
[[ $# -le 1 ]] || fail 'Usage: stage-gui.sh [staging-directory]'
STAGE=$(realpath -m -- "${1:-$DEV/stage/otpclient}")
[[ ! -L ${1:-$DEV/stage/otpclient} && "$STAGE" != / ]] || fail 'Unsafe staging directory'
# Do not erase or write into any input tree, or an ancestor of one.
for input in "$REPO" "$UCRT" "$PREFIX" "$GUI_BUILD" "$CLI_BUILD"; do
    input=$(realpath -m -- "$input")
    [[ "$input" != "$STAGE" && "$input" != "$STAGE/"* && "$STAGE" != "$input/"* ]] ||
        fail "Staging directory overlaps input: $input"
done
[[ -f "$GUI_BUILD/src/gui/otpclient.exe" ]] || fail "Missing GUI build: $GUI_BUILD"
if [[ -d "$STAGE" ]]; then
    [[ -z $(find "$STAGE" -type l -print -quit) ]] || fail 'Stage must not contain symlinks'
fi
if [[ -d "$STAGE" && ! -f "$STAGE/.otpclient-stage" ]]; then
    [[ -z $(find "$STAGE" -mindepth 1 -maxdepth 1 -print -quit) ]] ||
        fail 'Destination is not empty and is not an OTPClient stage'
fi
mkdir -p -- "$STAGE"
printf 'OTPClient portable UCRT64 stage v1\n' > "$STAGE/.otpclient-stage"
# Preserve config/ and logs/ across reruns. Everything below these three paths
# is generated packaging output; removing it also removes obsolete dependencies.
rm -rf -- "$STAGE/bin" "$STAGE/lib" "$STAGE/share"
mkdir -p "$STAGE/bin" "$STAGE/lib/gio/modules" "$STAGE/share/glib-2.0/schemas"
cp -- "$GUI_BUILD/src/gui/otpclient.exe" "$STAGE/bin/"
if [[ -f "$GUI_BUILD/src/cli/otpclient-cli.exe" ]]; then
    cp -- "$GUI_BUILD/src/cli/otpclient-cli.exe" "$STAGE/bin/"
elif [[ -f "$CLI_BUILD/src/cli/otpclient-cli.exe" ]]; then
    cp -- "$CLI_BUILD/src/cli/otpclient-cli.exe" "$STAGE/bin/"
fi
for tool in gdk-pixbuf-query-loaders gsettings; do
    [[ -f "$UCRT/bin/$tool.exe" ]] || fail "Missing runtime tool: $tool"
    cp -- "$UCRT/bin/$tool.exe" "$STAGE/bin/"
done

# Keep the package's loader ABI directory, discovered from its metadata.
PIXBUF_ABI=$("$UCRT/bin/pkgconf.exe" --variable=gdk_pixbuf_binary_version gdk-pixbuf-2.0)
PIXBUF_ABI=${PIXBUF_ABI//$'\r'/}
[[ $PIXBUF_ABI =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || fail 'Invalid pixbuf loader ABI'
LOADERS="lib/gdk-pixbuf-2.0/$PIXBUF_ABI/loaders"
mkdir -p "$STAGE/$LOADERS"
shopt -s nullglob
loader_files=("$UCRT/$LOADERS/"*.dll)
(( ${#loader_files[@]} > 0 )) || fail 'No pixbuf loaders installed'
cp -- "${loader_files[@]}" "$STAGE/$LOADERS/"
printf '%s\n' "$LOADERS" > "$STAGE/pixbuf-loaders-dir.txt"

# Optional dynamically loaded GIO extensions also need their own PE closure.
gio_files=("$UCRT/lib/gio/modules/"*.dll)
if (( ${#gio_files[@]} )); then
    mkdir -p "$STAGE/lib/gio/modules"
    cp -- "${gio_files[@]}" "$STAGE/lib/gio/modules/"
    [[ -f "$UCRT/bin/gio-querymodules.exe" ]] || fail 'Missing gio-querymodules'
    cp -- "$UCRT/bin/gio-querymodules.exe" "$STAGE/bin/"
fi

declare -A dll_sources=() visited=()
for dir in "$PREFIX/bin" "$UCRT/bin"; do
    for dll in "$dir/"*.dll; do
        key=${dll##*/}; key=${key,,}
        if [[ -n ${dll_sources[$key]:-} ]]; then
            cmp -s -- "${dll_sources[$key]}" "$dll" || fail "Conflicting runtime DLL: $key"
        else
            dll_sources[$key]=$dll
        fi
    done
done
queue=("$STAGE/bin/"*.exe "$STAGE/$LOADERS/"*.dll "$STAGE/lib/gio/modules/"*.dll)
printf 'image\timport\tsource\n' > "$STAGE/dll-manifest.tsv"
for ((i=0; i<${#queue[@]}; i++)); do
    image=${queue[$i]}
    imports=$(pe_imports "$image") || fail "Cannot read PE imports: $image"
    while IFS= read -r name; do
        [[ -n $name ]] || continue
        [[ $name != */* && $name != *\\* ]] || fail "Invalid import: $name"
        if is_system_dll "$name"; then
            printf '%s\t%s\tWindows system\n' "${image#"$STAGE/"}" "$name" >> "$STAGE/dll-manifest.tsv"
            continue
        fi
        key=${name,,}
        dll=${dll_sources[$key]:-}
        [[ -n $dll ]] || fail "Unresolved import $name (from $image)"
        printf '%s\t%s\t%s\n' "${image#"$STAGE/"}" "$name" "$dll" >> "$STAGE/dll-manifest.tsv"
        [[ -z ${visited[$key]:-} ]] || continue
        visited[$key]=1
        cp -- "$dll" "$STAGE/bin/$name"
        queue+=("$STAGE/bin/$name")
    done <<< "$imports"
done

# GTK's settings schemas are runtime data; omit the demonstration program.
schemas=("$UCRT/share/glib-2.0/schemas/"org.gtk.gtk4.*.gschema.xml)
(( ${#schemas[@]} > 0 )) || fail 'GTK4 schemas are missing'
cp -- "${schemas[@]}" "$STAGE/share/glib-2.0/schemas/"
project_schemas=("$REPO/data/"*.gschema.xml)
(( ${#project_schemas[@]} > 0 )) || fail 'Project schemas are missing'
cp -- "${project_schemas[@]}" "$STAGE/share/glib-2.0/schemas/"
[[ -x "$UCRT/bin/glib-compile-schemas.exe" ]] || fail 'Missing glib-compile-schemas'
"$UCRT/bin/glib-compile-schemas.exe" --strict "$(cygpath -m "$STAGE/share/glib-2.0/schemas")"
[[ -s "$STAGE/share/glib-2.0/schemas/gschemas.compiled" ]] || fail 'Schema compilation produced no output'
for data_dir in gtk-4.0 mime libwmf; do
    if [[ -d "$UCRT/share/$data_dir" ]]; then
        cp -aL -- "$UCRT/share/$data_dir" "$STAGE/share/"
    fi
done

# Keep hicolor metadata, the two application icons and the referenced Adwaita
# symbolic icons. Include GTK widget fallbacks and RTL variants of each name.
for theme in hicolor Adwaita; do
    [[ -f "$UCRT/share/icons/$theme/index.theme" ]] || fail "Missing $theme icon theme"
    mkdir -p "$STAGE/share/icons/$theme"
    cp -- "$UCRT/share/icons/$theme/index.theme" "$STAGE/share/icons/$theme/"
done
sed -i 's/^Inherits=.*/Inherits=hicolor/' "$STAGE/share/icons/Adwaita/index.theme"
mkdir -p "$STAGE/share/icons/hicolor/scalable/apps"
cp -- "$REPO/data/icons/com.github.paolostivanin.OTPClient.svg" \
    "$REPO/data/icons/com.github.paolostivanin.OTPClient-symbolic.svg" \
    "$STAGE/share/icons/hicolor/scalable/apps/"
{ grep -rhoE '"[a-z0-9-]+-symbolic"|>[a-z0-9-]+-symbolic<' \
      "$REPO/src/gui" --include='*.c' --include='*.ui' | tr -d '\"><'
  printf '%s\n' image-missing-symbolic pan-start-symbolic pan-end-symbolic \
      pan-down-symbolic pan-up-symbolic window-close-symbolic window-minimize-symbolic \
      window-maximize-symbolic window-restore-symbolic go-next-symbolic \
      go-previous-symbolic object-select-symbolic
} | sort -u > "$STAGE/icon-names.txt"
while IFS= read -r name; do
    files=()
    while IFS= read -r -d '' icon; do files+=("$icon"); done < <(
        find "$UCRT/share/icons/Adwaita" -type f \
            \( -name "$name.svg" -o -name "$name.symbolic.png" \
            -o -name "$name-rtl.svg" -o -name "$name-rtl.symbolic.png" \) -print0)
    (( ${#files[@]} > 0 )) || fail "Missing Adwaita icon: $name"
    for icon in "${files[@]}"; do
        rel=${icon#"$UCRT/share/icons/Adwaita/"}
        mkdir -p -- "$STAGE/share/icons/Adwaita/$(dirname -- "$rel")"
        cp -- "$icon" "$STAGE/share/icons/Adwaita/$rel"
    done
done < "$STAGE/icon-names.txt"

# No .po -> .mo rules exist upstream. Copy built catalogs if supplied, plus
# the toolkit catalogs; do not pretend that copied app catalogs fix LOCALE_DIR.
for locale_root in "$UCRT/share/locale" "$PREFIX/share/locale" \
    "$GUI_BUILD/share/locale" "$CLI_BUILD/share/locale"; do
    [[ -d $locale_root ]] || continue
    while IFS= read -r -d '' catalog; do
        rel=${catalog#"$locale_root/"}
        mkdir -p -- "$STAGE/share/locale/$(dirname -- "$rel")"
        cp -- "$catalog" "$STAGE/share/locale/$rel"
    done < <(find "$locale_root" -type f \( -name OTPClient.mo -o -name otpclient.mo \
        -o -name otpclient-cli.mo -o -name gtk40.mo -o -name libadwaita.mo \
        -o -name glib20.mo -o -name gdk-pixbuf.mo \) -print0)
done
for file in otpclient.cmd otpclient.ps1 capture-window.ps1 check-stage.sh runtime-common.sh; do
    cp -- "$SCRIPT_DIR/$file" "$STAGE/$file"
done
POWERSHELL="$SYSTEM_DIR/WindowsPowerShell/v1.0/powershell.exe"
[[ -x $POWERSHELL ]] || fail 'Windows PowerShell is missing'
# The same initialization runs again on every launch after extraction/moving.
"$POWERSHELL" -NoProfile -NonInteractive -ExecutionPolicy Bypass \
    -File "$(cygpath -m "$STAGE/otpclient.ps1")" -InitializeOnly
"$OBJDUMP" -p "$STAGE/bin/otpclient.exe" | grep -E 'Subsystem[[:space:]]' || true
echo "PASS: staged ${#visited[@]} non-system DLLs in $STAGE"
echo 'App catalog relocation and GUI appearance still require native verification.'
