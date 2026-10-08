#!@bash@
set -euo pipefail

WINE_BIN=@wine@/bin
DXVK=@dxvk@
MESA_ICDS=@mesaIcds@
APP_STORE=@app@
THEME_DIR=@theme@
FONTS_DIR=@fonts@
DRAGFIX=@dragfix@
VERSION=@version@

DATA_DIR="${PAINTDOTNET_HOME:-${XDG_DATA_HOME:-$HOME/.local/share}/paint.nix}"
APP_DIR="$DATA_DIR/app"
THEME="${PAINTDOTNET_THEME:-win10dark}"
export WINEPREFIX="${PAINTDOTNET_PREFIX:-$DATA_DIR/prefix}"
export WINEDEBUG="${WINEDEBUG:--all}"
export PATH="$WINE_BIN:@runtimePath@:$PATH"

log() { printf 'paintdotnet: %s\n' "$*" >&2; }

# Use packaged drivers when the host has no NixOS graphics setup.
if [ -n "$MESA_ICDS" ] && [ ! -d /run/opengl-driver ] \
    && [ -z "${VK_DRIVER_FILES:-}${VK_ICD_FILENAMES:-}${VK_ADD_DRIVER_FILES:-}" ] \
    && [ "${PAINTDOTNET_MESA_FALLBACK:-1}" = 1 ]; then
    export VK_DRIVER_FILES="$MESA_ICDS"
fi

# Warn before prefix setup so a setup failure does not hide the limit.
nofile="$(ulimit -Hn)"
if [ "$nofile" != unlimited ] && [ "$nofile" -lt 65536 ]; then
    log "warning: hard open-file limit is $nofile; Paint.NET may report 'Too many open files'."
    log "see the README for how to set the hard limit to 524288"
fi

# Copy the exe so .NET uses this writable directory for portable-mode settings.
sync_app() {
    mkdir -p "$APP_DIR"
    if [ "$(cat "$APP_DIR/.store-path" 2>/dev/null || true)" = "$APP_STORE" ]; then
        return
    fi
    log "linking Paint.NET $VERSION into $APP_DIR"
    find "$APP_DIR" -type l -delete
    rm -f "$APP_DIR/paintdotnet.exe"
    cp -rsn --no-preserve=mode "$APP_STORE"/. "$APP_DIR"/
    rm -f "$APP_DIR/paintdotnet.exe"
    cp --no-preserve=mode "$APP_STORE/paintdotnet.exe" "$APP_DIR/paintdotnet.exe"
    echo "$APP_STORE" > "$APP_DIR/.store-path"
}

reg() { wine reg add "$@" /f >/dev/null; }

setup_prefix() {
    local sys="$WINEPREFIX/drive_c/windows" dll pair

    log "setting up Wine prefix at $WINEPREFIX"
    mkdir -p "$(dirname "$WINEPREFIX")"
    # Suppress Gecko installation, but leave mscoree enabled for the .NET loader.
    WINEDLLOVERRIDES="mshtml=" wine wineboot --init
    wineserver -w

    # win11 reports build 22000, above Paint.NET's Windows 10 21H2 minimum.
    wine winecfg -v win11
    reg 'HKCU\Control Panel\Desktop' /v FontSmoothingType /t REG_DWORD /d 2

    # Wine's builtin D3D11 leaves Paint.NET with a null ID3D11Device5 at startup.
    for dll in d3d8 d3d9 d3d10core d3d11 dxgi; do
        install -m644 "$DXVK/x64/$dll.dll" "$sys/system32/$dll.dll"
        install -m644 "$DXVK/x32/$dll.dll" "$sys/syswow64/$dll.dll"
        reg 'HKCU\Software\Wine\DllOverrides' /v "$dll" /d native
    done

    # Substitute Selawik for Segoe UI and register fallback fonts for missing glyphs.
    install -m644 "$FONTS_DIR"/*.ttf "$sys/Fonts/"
    for pair in "Selawik (TrueType)=selawk.ttf" "Selawik Bold (TrueType)=selawkb.ttf" \
                "Selawik Light (TrueType)=selawkl.ttf" "Selawik Semibold (TrueType)=selawksb.ttf" \
                "Selawik Semilight (TrueType)=selawksl.ttf"; do
        reg 'HKLM\Software\Microsoft\Windows NT\CurrentVersion\Fonts' /v "${pair%%=*}" /d "${pair#*=}"
    done
    reg 'HKLM\Software\Microsoft\Windows NT\CurrentVersion\FontSubstitutes' /v "Segoe UI" /d "Selawik"
    reg 'HKLM\Software\Microsoft\Windows NT\CurrentVersion\FontSubstitutes' /v "Segoe UI Semibold" /d "Selawik Semibold"
    reg 'HKLM\Software\Microsoft\Windows NT\CurrentVersion\FontSubstitutes' /v "Segoe UI Light" /d "Selawik Light"
    reg 'HKLM\Software\Microsoft\Windows NT\CurrentVersion\FontLink\SystemLink' /v Selawik /t REG_MULTI_SZ \
        /d 'tahoma.ttf,Tahoma\0DejaVuSans.ttf,DejaVu Sans\0NotoColorEmoji.ttf,Noto Color Emoji'

    if [ "$THEME" = win10dark ]; then
        mkdir -p "$sys/resources/themes/win10dark"
        install -m644 "$THEME_DIR/win10dark.msstyles" "$sys/resources/themes/win10dark/win10dark.msstyles"
        wine "$THEME_DIR/apply-theme.exe" 'C:\windows\resources\themes\win10dark\win10dark.msstyles' Blue NormalSize >/dev/null
    fi
    wineserver -w
}

# Take locks in app-then-prefix order; close them before launching the app.
mkdir -p "$DATA_DIR" "$WINEPREFIX"
exec {app_lock}>"$DATA_DIR/.app.lock"
flock "$app_lock"
exec {prefix_lock}>"$WINEPREFIX/.paint.nix.lock"
flock "$prefix_lock"

sync_app

setup_id="1 $WINE_BIN $DXVK $THEME_DIR $FONTS_DIR $THEME"
marker="$WINEPREFIX/.paint.nix-setup"
if [ "$(cat "$marker" 2>/dev/null || true)" != "$setup_id" ]; then
    setup_prefix
    echo "$setup_id" > "$marker"
fi
exec {prefix_lock}>&-
exec {app_lock}>&-

args=()
for arg in "$@"; do
    if [ -e "$arg" ]; then
        args+=("$(wine winepath -w "$arg" 2>/dev/null)")
    else
        args+=("$arg")
    fi
done

export WINEDLLOVERRIDES="mshtml=;d3dcompiler_47=n${WINEDLLOVERRIDES:+;$WINEDLLOVERRIDES}"
export DXVK_LOG_LEVEL="${DXVK_LOG_LEVEL:-error}"
if [ "${PAINTDOTNET_DRAGFIX:-1}" = 1 ]; then
    export LD_PRELOAD="$DRAGFIX${LD_PRELOAD:+:$LD_PRELOAD}"
fi

cd "$APP_DIR"
exec wine "$APP_DIR/paintdotnet.exe" "${args[@]}"
