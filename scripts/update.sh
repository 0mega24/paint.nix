#!/usr/bin/env bash
# Update the release version and source hash in package.nix.
set -euo pipefail
cd "$(dirname "$0")/.."

tag=$(curl -fsSL https://api.github.com/repos/paintdotnet/Paint.NET-on-Wine/releases/latest | jq -er '.tag_name | select(type == "string")')
if [[ ! "$tag" =~ ^v[0-9]+(\.[0-9]+)+$ ]]; then
    printf 'unexpected release tag: %s\n' "$tag" >&2
    exit 1
fi
version=${tag#v}
current=$(sed -nE 's/^  version = "([^"]+)";/\1/p' package.nix)
if [ "$version" = "$current" ]; then
    echo "already at $version"
    exit 0
fi

url="https://github.com/paintdotnet/Paint.NET-on-Wine/releases/download/$tag/paint.net.$version.portable.x64.wine.EXPERIMENTAL.zip"
hash=$(nix --extra-experimental-features nix-command store prefetch-file --json "$url" | jq -er '.hash | select(type == "string" and startswith("sha256-"))')

sed -i -E \
    -e "s|^  version = \"[^\"]+\";|  version = \"$version\";|" \
    -e "s|hash = \"[^\"]+\"; # paintdotnet|hash = \"$hash\"; # paintdotnet|" \
    package.nix
echo "updated $current -> $version ($hash)"
