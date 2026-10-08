{
  lib,
  stdenv,
  stdenvNoCC,
  fetchurl,
  unzip,
  imagemagick,
  makeDesktopItem,
  copyDesktopItems,
  libx11,
  wineWow64Packages,
  dxvk,
  # Paint.NET-on-Wine needs Wine >= 11.15 (WoW64 build: no 32-bit host libraries needed).
  winePackage ? wineWow64Packages.unstableFull,
}:

assert lib.assertMsg (lib.versionAtLeast winePackage.version "11.15")
  "paint.nix: Paint.NET-on-Wine needs Wine >= 11.15, got ${winePackage.version}";

let
  version = "5.200.9775.2252";

  # Builds expire 12 weeks after their build date (this one: 2026-12-29); run scripts/update.sh.
  src = fetchurl {
    url = "https://github.com/paintdotnet/Paint.NET-on-Wine/releases/download/v${version}/paint.net.${version}.portable.x64.wine.EXPERIMENTAL.zip";
    hash = "sha256-Cs4FtDlxOfUCCZq0KeMCk478Ier320XQeLfiBOU3LG8="; # paintdotnet
  };

  # Microsoft's open (OFL) Segoe UI-metric-compatible font, substituted for Segoe UI in the prefix.
  selawik = fetchurl {
    url = "https://github.com/microsoft/Selawik/releases/download/1.01/Selawik_Release.zip";
    hash = "sha256-P2LFHgXjtaHmJBz5KjcfC+LqEYOqh7MHGLvUCDKo1CM=";
  };

  # LD_PRELOAD shim that lets Paint.NET's floating palettes be dragged under GNOME/XWayland.
  dragfix = stdenv.mkDerivation {
    pname = "wine-dragfix";
    version = "1";
    src = ./dragfix;
    buildInputs = [ libx11 ];
    buildPhase = ''
      runHook preBuild
      $CC -shared -fPIC -O2 -Wall -o wine-dragfix.so wine-dragfix.c -ldl -lpthread
      runHook postBuild
    '';
    installPhase = ''
      runHook preInstall
      install -Dm755 wine-dragfix.so $out/lib/wine-dragfix.so
      runHook postInstall
    '';
  };
in
stdenvNoCC.mkDerivation {
  pname = "paintdotnet";
  inherit version src;

  nativeBuildInputs = [
    unzip
    imagemagick
    copyDesktopItems
  ];

  unpackPhase = ''
    runHook preUnpack
    mkdir app fonts
    unzip -q $src -d app
    unzip -q -j ${selawik} '*.ttf' -d fonts
    runHook postUnpack
  '';

  dontConfigure = true;
  dontBuild = true;
  # Windows binaries: nothing to strip or patch
  dontStrip = true;
  dontPatchELF = true;

  installPhase = ''
    runHook preInstall

    share=$out/share/paintdotnet
    mkdir -p $share $out/bin $out/lib

    cp -r app $share/app
    rm -f $share/app/*.sh # upstream install/launch scripts; the launcher replaces them

    install -Dm644 -t $share/fonts fonts/*.ttf
    install -Dm644 ${./theme/win10dark.msstyles} $share/theme/win10dark.msstyles
    install -Dm644 ${./theme/apply-theme.exe} $share/theme/apply-theme.exe
    install -Dm755 ${dragfix}/lib/wine-dragfix.so $out/lib/wine-dragfix.so

    # icons from the app's .ico (one PNG per frame size)
    magick app/paintdotnet.ico icon-%d.png
    for f in icon-*.png; do
      size=$(magick identify -format '%w' "$f")
      install -Dm644 "$f" $out/share/icons/hicolor/''${size}x''${size}/apps/paintdotnet.png
    done

    substitute ${./launcher.sh} $out/bin/paintdotnet \
      --subst-var-by bash ${stdenv.shell} \
      --subst-var-by wine ${winePackage} \
      --subst-var-by dxvk ${dxvk.bin} \
      --subst-var-by app $share/app \
      --subst-var-by theme $share/theme \
      --subst-var-by fonts $share/fonts \
      --subst-var-by dragfix $out/lib/wine-dragfix.so \
      --subst-var-by version ${version}
    chmod +x $out/bin/paintdotnet

    runHook postInstall
  '';

  desktopItems = [
    (makeDesktopItem {
      name = "paintdotnet";
      desktopName = "Paint.NET";
      genericName = "Image Editor";
      comment = "Paint.NET ${version} (experimental Wine build)";
      exec = "paintdotnet %F";
      icon = "paintdotnet";
      categories = [
        "Graphics"
        "RasterGraphics"
        "2DGraphics"
      ];
      mimeTypes = [
        "image/png"
        "image/jpeg"
        "image/bmp"
        "image/gif"
        "image/tiff"
        "image/webp"
        "image/avif"
        "image/jxl"
        "image/x-tga"
        "image/vnd-ms.dds"
      ];
      startupWMClass = "paintdotnet.exe";
    })
  ];

  passthru = {
    inherit winePackage dragfix;
  };

  meta = {
    description = "Paint.NET image editor for Linux via the official Paint.NET-on-Wine build";
    homepage = "https://github.com/paintdotnet/Paint.NET-on-Wine";
    license = lib.licenses.unfree; # Paint.NET is freeware; see app/License.txt
    sourceProvenance = [ lib.sourceTypes.binaryNativeCode ];
    platforms = [ "x86_64-linux" ];
    mainProgram = "paintdotnet";
  };
}
