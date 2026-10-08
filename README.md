# paint.nix

Run [Paint.NET](https://www.getpaint.net/) on x86_64 Linux with Nix using the
experimental
[Paint.NET-on-Wine](https://github.com/paintdotnet/Paint.NET-on-Wine) release.
The package includes Wine, DXVK, a dark Windows 10-style theme, Selawik fonts,
and a desktop entry. A small Wine shim fixes floating palette dragging under
GNOME/XWayland.

The pinned packages currently use Wine 11.18 and DXVK 3.1.1. Paint.NET requires
Wine 11.15 or newer and working Vulkan drivers.

## Install on NixOS

Add the input to your existing flake:

```nix
inputs.paint-nix.url = "github:0mega24/paint.nix";
```

Include `paint-nix` in your `outputs` arguments, then add these entries to your
NixOS configuration's `modules` list:

```nix
paint-nix.nixosModules.default
{ programs.paintdotnet.enable = true; }
```

Rebuild NixOS, then run `paintdotnet` or select Paint.NET in your app launcher.
The module installs the package and its desktop files and enables
`hardware.graphics` for DXVK. Your machine still needs the appropriate GPU
configuration.

### Other installation methods

| Method | Configuration |
|---|---|
| Home Manager | Add `paint-nix.homeManagerModules.default` to `imports` and set `programs.paintdotnet.enable = true`. Enable Vulkan drivers separately in the NixOS configuration. |
| Package only | Add `paint-nix.packages.x86_64-linux.default` to `environment.systemPackages` or `home.packages`. |
| Run without installing | `nix run github:0mega24/paint.nix` |
| Overlay | Add `paint-nix.overlays.default` to `nixpkgs.overlays` and install `pkgs.paintdotnet`. |

### Run on other Linux distributions

On distributions other than NixOS, host Vulkan drivers may need libraries
outside the Nix store. The package includes Mesa to avoid depending on those
libraries. If `/run/opengl-driver` is absent, the launcher points
`VK_DRIVER_FILES` at the bundled drivers. These include Intel, AMD, NVIDIA
nouveau, and lavapipe for software rendering. The NixOS module omits Mesa
from the package and uses the system graphics configuration.

For NVIDIA's proprietary driver, launch with
[nixGL's Vulkan wrapper](https://github.com/nix-community/nixGL):

```sh
nix run --impure github:nix-community/nixGL#nixVulkanNvidia -- nix run github:0mega24/paint.nix
```

The launcher leaves driver selection to you when `VK_DRIVER_FILES`,
`VK_ICD_FILENAMES`, or `VK_ADD_DRIVER_FILES` has a nonempty value.

A low hard limit for open files can cause `Too many open files` errors.
The launcher warns when `ulimit -Hn` is below 65536. On systems that use
PAM's `pam_limits`, add the following to
`/etc/security/limits.d/90-nofile.conf`, then log out and back in:

```text
*  soft  nofile  65536
*  hard  nofile  524288
```

Check `ulimit -Hn` in the new session to confirm the limit took effect.

The module and package outputs use this flake's pinned nixpkgs. The overlay
uses your nixpkgs, which must provide `wineWow64Packages.unstableFull` at
version 11.15 or newer. The drag shim also uses private Wine APIs; check their
compatibility before switching Wine versions. The definitions are in
[dragfix/wine-dragfix.c](dragfix/wine-dragfix.c); compare them with the matching
Wine version's `include/ntuser.h`.

Paint.NET is freeware and the package is marked unfree. This flake permits
`paintdotnet` in its own nixpkgs configuration. Overlay users must permit it in
their configuration, for example:

```nix
nixpkgs.config.allowUnfreePredicate = pkg:
  nixpkgs.lib.getName pkg == "paintdotnet";
```

## Launch and stored data

On first launch, the launcher creates a Wine prefix, sets it to Windows 11,
and installs DXVK, font substitutions, ClearType settings, and the theme.
Later launches repeat prefix setup when Wine, DXVK, fonts, or theme settings
change. Locks serialize setup when more than one launcher starts at once.

With the default environment, files live here:

| Contents | Location |
|---|---|
| Wine prefix | `~/.local/share/paint.nix/prefix` |
| App files, settings, and cache | `~/.local/share/paint.nix/app` |

Paint.NET runs in portable mode and writes settings beside its executable.
The launcher copies the executable into the writable app directory and links
other packaged files from the Nix store. Regular settings files survive app
updates. Updates currently delete all symlinks in that directory, including
user-created links.

Open an image from the command line with `paintdotnet /path/to/image.png`.
Existing file arguments are converted to Windows paths before launching Wine.

### Environment variables

| Variable | Default | Use |
|---|---|---|
| `PAINTDOTNET_HOME` | `${XDG_DATA_HOME:-$HOME/.local/share}/paint.nix` | Change the app's data directory. |
| `PAINTDOTNET_PREFIX` | `$PAINTDOTNET_HOME/prefix` | Use a different Wine prefix. |
| `PAINTDOTNET_THEME` | `win10dark` | Set to `none` to skip theme installation in a new prefix. It does not undo an already installed theme. |
| `PAINTDOTNET_DRAGFIX` | `1` | Set to `0` to disable the palette drag shim. |
| `PAINTDOTNET_MESA_FALLBACK` | `1` | Set to `0` to disable automatic selection of the bundled Mesa drivers. |
| `WINE_DRAGFIX_LOG` | Unset | Append drag diagnostics to the specified file. |
| `WINEDEBUG` | `-all` | Override Wine's logging settings. |
| `DXVK_LOG_LEVEL` | `error` | Override DXVK's logging level. |

For example, record palette drag diagnostics with:

```sh
WINE_DRAGFIX_LOG="$HOME/paintdotnet-drag.log" paintdotnet
```

## Updates

Paint.NET-on-Wine builds expire 12 weeks after their build date. The packaged
release, `5.200.9775.2252`, expires on 2026-12-29.

From this repository, run:

```sh
scripts/update.sh
nix build .#default
```

The update script needs `curl`, `jq`, and Nix. It fetches the latest release,
prefetches its archive, and changes the version and Paint.NET source hash in
`package.nix`. It does not update `flake.lock` or the expiry date in these docs.
Check the new release's expiry, update the docs, and test before committing.

## Testing status and limitations

The original setup was tested on Ubuntu 26.04 with GNOME on Wayland, XWayland,
Intel Meteor Lake graphics, and Wine 11.19. Palette dragging and snapping
worked there. The Nix package builds, and its NixOS and Home Manager modules
evaluate successfully.

The latest cleanup has passed isolated launcher and shim tests. It has not
been tested interactively on a real NixOS host or with the packaged Wine 11.18.
Interactive testing should cover all four palettes, snapping and release,
open/save, different DPI scales, and monitors with negative origins.

The existing upstream notes list slower rendering, failures with some GIF,
palette PNG, and CMYK JPEG files, possible PNG save failures, unavailable
printing and scanning, and an initially oversized Tools palette. Check
[upstream issues](https://github.com/paintdotnet/Paint.NET-on-Wine/issues) when
investigating application problems.

## Development and licenses

Build with `nix build .#default` and format Nix sources with `nix fmt`.
The theme sources are under [theme/](theme/). Rebuilding them requires Python
with Pillow, Inkscape, Wine's resource compiler, and mingw-w64.

| Component | License |
|---|---|
| Paint.NET | Freeware; see the packaged `License.txt`. |
| Theme derived from Wine's aero theme | LGPL-2.1-or-later |
| Selawik | OFL-1.1 |
