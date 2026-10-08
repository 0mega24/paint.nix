#!@bash@
# paintdotnet -- run Paint.NET-on-Wine (packaged by paint.nix)
#
# Environment:
#   PAINTDOTNET_HOME      data dir (default: $XDG_DATA_HOME/paint.nix)
#   PAINTDOTNET_PREFIX    Wine prefix (default: $PAINTDOTNET_HOME/prefix)
#   PAINTDOTNET_THEME     "win10dark" (default) or "none" to keep Wine's stock theme
#   PAINTDOTNET_DRAGFIX   "1" (default) or "0" to disable the palette drag fix
#   WINE_DRAGFIX_LOG      file to log palette drags to (debugging)
set -euo pipefail

WINE_BIN=@wine@/bin
DXVK=@dxvk@
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
export PATH="$WINE_BIN:$PATH"

log() { echo "paintdotnet: $*" >&2; }

# Paint.NET runs in portable mode and writes its settings (PaintDotNet.AppSettings.json)
# and AppCache/ next to the exe, so it needs a writable app dir: symlinks into the store,
# with the exe itself copied so .NET resolves the app base to this dir. Files Paint.NET
# creates are real files and survive version updates.
sync_app() {
    mkdir -p "$APP_DIR"
    [ "$(cat "$APP_DIR/.store-path" 2>/dev/null || true)" = "$APP_STORE" ] && return
    log "linking Paint.NET $VERSION into $APP_DIR"
    find "$APP_DIR" -type l -delete
    rm -f "$APP_DIR/paintdotnet.exe"
    cp -rsn --no-preserve=mode "$APP_STORE"/. "$APP_DIR"/
    rm -f "$APP_DIR/paintdotnet.exe"
    cp --no-preserve=mode "$APP_STORE/paintdotnet.exe" "$APP_DIR/paintdotnet.exe"
    echo "$APP_STORE" > "$APP_DIR/.store-path"
}

reg() { wine reg add "$@" /f >/dev/null; }

# One-time (and on package change) prefix setup, mirroring upstream install.sh plus extras.
setup_prefix() {
    local sys="$WINEPREFIX/drive_c/windows" dll pair

    log "setting up Wine prefix at $WINEPREFIX"
    mkdir -p "$(dirname "$WINEPREFIX")"
    # mshtml off so prefix creation doesn't ask for Wine Gecko; mscoree must stay
    # enabled, Wine's loader needs it for .NET (including self-contained) apps.
    WINEDLLOVERRIDES="mshtml=" wine wineboot --init
    wineserver -w

    # Paint.NET requires Windows 10 21H2+; win11 reports build 22000
    wine winecfg -v win11
    # ClearType UI text
    reg 'HKCU\Control Panel\Desktop' /v FontSmoothingType /t REG_DWORD /d 2

    # DXVK: without it Paint.NET crashes at startup (null ID3D11Device5)
    for dll in d3d8 d3d9 d3d10core d3d11 dxgi; do
        install -m644 "$DXVK/x64/$dll.dll" "$sys/system32/$dll.dll"
        install -m644 "$DXVK/x32/$dll.dll" "$sys/syswow64/$dll.dll"
        reg 'HKCU\Software\Wine\DllOverrides' /v "$dll" /d native
    done

    # Selawik stands in for Segoe UI; fall back to Tahoma/DejaVu for missing glyphs
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

    # Windows 10 style (dark) visual style, applied like winecfg does
    if [ "$THEME" = win10dark ]; then
        mkdir -p "$sys/resources/themes/win10dark"
        install -m644 "$THEME_DIR/win10dark.msstyles" "$sys/resources/themes/win10dark/win10dark.msstyles"
        wine "$THEME_DIR/apply-theme.exe" 'C:\windows\resources\themes\win10dark\win10dark.msstyles' Blue NormalSize >/dev/null
    fi
    wineserver -w
}

sync_app

setup_id="1 $DXVK $THEME_DIR $FONTS_DIR $THEME"
marker="$WINEPREFIX/.paint.nix-setup"
if [ "$(cat "$marker" 2>/dev/null || true)" != "$setup_id" ]; then
    setup_prefix
    echo "$setup_id" > "$marker"
fi

# file arguments -> Windows paths
args=()
for arg in "$@"; do
    if [ -e "$arg" ]; then args+=("$(wine winepath -w "$arg" 2>/dev/null)"); else args+=("$arg"); fi
done

export WINEDLLOVERRIDES="mshtml=;d3dcompiler_47=n${WINEDLLOVERRIDES:+;$WINEDLLOVERRIDES}"
export DXVK_LOG_LEVEL="${DXVK_LOG_LEVEL:-error}"
if [ "${PAINTDOTNET_DRAGFIX:-1}" = 1 ]; then
    export LD_PRELOAD="$DRAGFIX${LD_PRELOAD:+:$LD_PRELOAD}"
fi

cd "$APP_DIR"
exec wine "$APP_DIR/paintdotnet.exe" "${args[@]}"
