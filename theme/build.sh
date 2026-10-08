#!/usr/bin/env bash
# Usage: build.sh <resources.rc> <output.msstyles>
# WRC selects Wine's resource compiler; CROSS selects the mingw-w64 tool prefix.
set -euo pipefail
WRC="${WRC:-wrc}"
CROSS="${CROSS:-x86_64-w64-mingw32-}"
here=$(cd "$(dirname "$0")" && pwd)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
"$WRC" --nostdinc -I"$here/stub" -I"$(dirname "$1")" -o "$tmp/out.res" "$1"
"${CROSS}windres" -J res -i "$tmp/out.res" -O coff -o "$tmp/out_res.o"
"${CROSS}gcc" -shared -nostdlib -s -Wl,--entry=DllMain -o "$2" "$here/stub/dllmain.c" "$tmp/out_res.o"
