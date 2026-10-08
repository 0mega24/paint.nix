#!/usr/bin/env bash
# Bump package.nix to the latest Paint.NET-on-Wine release (builds expire after 12 weeks).
# Needs curl, jq and nix.
set -euo pipefail
cd "$(dirname "$0")/.."

tag=$(curl -fsSL https://api.github.com/repos/paintdotnet/Paint.NET-on-Wine/releases/latest | jq -r .tag_name)
version=${tag#v}
current=$(sed -nE 's/^  version = "([^"]+)";/\1/p' package.nix)
if [ "$version" = "$current" ]; then
    echo "already at $version"
    exit 0
fi

url="https://github.com/paintdotnet/Paint.NET-on-Wine/releases/download/$tag/paint.net.$version.portable.x64.wine.EXPERIMENTAL.zip"
hash=$(nix --extra-experimental-features nix-command store prefetch-file --json "$url" | jq -r .hash)

sed -i -E \
    -e "s|^  version = \"[^\"]+\";|  version = \"$version\";|" \
    -e "s|hash = \"[^\"]+\"; # paintdotnet|hash = \"$hash\"; # paintdotnet|" \
    package.nix
echo "updated $current -> $version ($hash)"
