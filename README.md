# paint.nix

[Paint.NET](https://www.getpaint.net/) on NixOS, using the official, experimental
[Paint.NET-on-Wine](https://github.com/paintdotnet/Paint.NET-on-Wine) build, with fixes that make it
pleasant on a Linux desktop:

- **Wine 11.18 (WoW64) + DXVK 3.1.1** from a pinned nixos-unstable. Paint.NET needs Wine ≥ 11.15
  and DXVK, or it crashes at startup.
- **Floating palettes can be dragged** (Tools, Colors, Layers, History). Without the fix, they stay
  locked to their canvas corners under GNOME/XWayland. See [How the drag fix works](#how-the-drag-fix-works).
- **Windows 10 style (dark) window frames and controls** instead of Wine's stock theme, plus Microsoft's
  open-source Selawik font standing in for Segoe UI.
- A desktop entry and icons, so it shows up in rofi, fuzzel, GNOME, KDE and so on.

## Usage

```nix
# flake.nix
{
  inputs.paint-nix.url = "github:0mega24/paint.nix";

  outputs = { nixpkgs, paint-nix, ... }: {
    nixosConfigurations.myhost = nixpkgs.lib.nixosSystem {
      modules = [
        paint-nix.nixosModules.default
        { programs.paintdotnet.enable = true; }
      ];
    };
  };
}
```

The NixOS module installs the package, links its desktop entry and icons, and enables
`hardware.graphics` (DXVK needs Vulkan drivers).

You can also install it in other ways:

- **Home Manager:** import `paint-nix.homeManagerModules.default` and set `programs.paintdotnet.enable = true;`.
  Vulkan drivers still need `hardware.graphics.enable = true;` in the NixOS config.
- **Package:** add `paint-nix.packages.x86_64-linux.default` to `environment.systemPackages` or `home.packages`.
- **Try it:** `nix run github:0mega24/paint.nix`.
- **Overlay:** `paint-nix.overlays.default` builds against *your* nixpkgs instead. It needs
  `wineWow64Packages.unstableFull` ≥ 11.15 there, and `allowUnfree` for `paintdotnet`.

Paint.NET is freeware, not open source, so the package is marked `unfree`. The flake's own outputs
allow it for you.

## First launch

`paintdotnet` (or the "Paint.NET" app entry) sets everything up on first run, and again whenever
the package changes:

| | Default location |
|---|---|
| Wine prefix | `~/.local/share/paint.nix/prefix` |
| Writable app dir (settings, palette positions) | `~/.local/share/paint.nix/app` |

The prefix is set to Windows 11 with ClearType, DXVK DLLs and overrides, the Selawik fonts and
substitutes, and the Windows 10 Dark theme.

Paint.NET runs in portable mode and writes its settings next to its exe. The app dir is therefore a
writable symlink farm into the Nix store, and settings survive updates.

### Environment variables

| Variable | Default | Effect |
|---|---|---|
| `PAINTDOTNET_HOME` | `$XDG_DATA_HOME/paint.nix` | data dir |
| `PAINTDOTNET_PREFIX` | `$PAINTDOTNET_HOME/prefix` | Wine prefix |
| `PAINTDOTNET_THEME` | `win10dark` | `none` keeps Wine's stock theme (applies to a new prefix) |
| `PAINTDOTNET_DRAGFIX` | `1` | `0` disables the palette drag fix |
| `WINE_DRAGFIX_LOG` | unset | file to log palette drags to |

## Updating (every 12 weeks)

Paint.NET-on-Wine builds **expire 12 weeks after their build date**. The current one,
`5.200.9775.2252`, expires on 2026-12-29. To bump it:

```sh
scripts/update.sh   # needs curl, jq, nix
```

## How the drag fix works

`wine-dragfix.so` is an `LD_PRELOAD` shim loaded only into Paint.NET's Wine process.

**The problem.** When you drag a window by a Wine-drawn title bar, winex11 hands the move to the
window manager with `_NET_WM_MOVERESIZE`. It then polls `XQueryPointer` until the button is released.
Under XWayland the compositor owns the pointer during that drag, so XWayland reports the button as
already up and the pointer as frozen. winex11 ends the "move" before the window has moved, and
Paint.NET never gets `WM_MOVING`. Its snap manager, which works out positions from the cursor,
then puts the palette back in its corner.

**What the shim does** while a window-manager-driven move is in progress (no pointer grabs, no
synthetic X events):

- reports the button as still held, so winex11's move loop stays alive;
- reads where the compositor has moved the window, and works out the real pointer delta from it,
  ignoring positions the app snapped the window back to;
- feeds Wine matching absolute mouse-move input, so `GetCursorPos` follows the drag;
- sends `WM_MOVING`, as Windows' move loop would;
- ends the drag when XWayland gets the pointer back (`EnterNotify`, `ButtonRelease` or `MotionNotify`),
  with a 30 s timeout as a safety net.

Earlier attempts and why they failed:

| Attempt | Result |
|---|---|
| Hide `_NET_WM_MOVERESIZE` so Wine runs its own move loop | stuttery drags, lost button releases, and a leftover X pointer grab that blocked clicks in other apps |
| Wine's Wayland driver | Paint.NET is disconnected by the compositor (`wl_surface already has a buffer committed`) |
| Wine virtual desktop | one big window, palettes hidden behind it |

## The theme

Wine only loads XP-format (`PACKTHEM_VERSION` 3) `.msstyles` files, so Windows 10's own theme can't
be used. `theme/win10dark.msstyles` is Wine's LGPL aero theme restyled by `theme/win10theme.py`:

- window frames and caption buttons are redrawn flat;
- all controls are recolored to Windows 10 dark with square corners;
- system colors are set to Windows 10 dark, and fonts to Segoe UI.

`theme/apply-theme.exe` applies it the way winecfg does. To rebuild the theme (needs Python with
Pillow, Inkscape, Wine's `wrc`, and mingw-w64), download Wine's theme source and run the generator:

```sh
mkdir aero-src
curl -L "https://gitlab.winehq.org/wine/wine/-/archive/wine-11.19/wine-wine-11.19.tar.gz?path=dlls/aero.msstyles" \
  | tar -xz --strip-components=3 -C aero-src
python3 theme/win10theme.py aero-src out
theme/build.sh out/win10dark.rc theme/win10dark.msstyles
```

## Known issues

These come from Paint.NET-on-Wine upstream:

- Rendering is slower than on Windows.
- Some GIF, palette PNG and CMYK JPEG files won't open.
- Saving PNGs can fail.
- Printing and scanning don't work.
- The Tools window can start out double width (Wine metrics).

Report Paint.NET problems [upstream](https://github.com/paintdotnet/Paint.NET-on-Wine/issues).

## Licenses

| Component | License |
|---|---|
| Paint.NET | freeware (see its `License.txt`) |
| Theme (derived from Wine's aero theme) | LGPL-2.1-or-later |
| Selawik | OFL-1.1 |
