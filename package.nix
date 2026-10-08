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
  coreutils,
  findutils,
  util-linux,
  wineWow64Packages,
  dxvk,
  # Avoid the older pkgs.wine that callPackage would inject for a "wine" argument.
  winePackage ? wineWow64Packages.unstableFull,
}:

assert lib.assertMsg (lib.versionAtLeast winePackage.version "11.15")
  "paint.nix: Paint.NET-on-Wine needs Wine >= 11.15, got ${winePackage.version}";

let
  version = "5.200.9775.2252";

  # This release expires on 2026-12-29. Update with scripts/update.sh.
  src = fetchurl {
    url = "https://github.com/paintdotnet/Paint.NET-on-Wine/releases/download/v${version}/paint.net.${version}.portable.x64.wine.EXPERIMENTAL.zip";
    hash = "sha256-Cs4FtDlxOfUCCZq0KeMCk478Ier320XQeLfiBOU3LG8="; # paintdotnet
  };

  selawik = fetchurl {
    url = "https://github.com/microsoft/Selawik/releases/download/1.01/Selawik_Release.zip";
    hash = "sha256-P2LFHgXjtaHmJBz5KjcfC+LqEYOqh7MHGLvUCDKo1CM=";
  };

  dragfix = stdenv.mkDerivation {
    pname = "wine-dragfix";
    version = "1";
    src = ./dragfix;
    buildInputs = [ libx11 ];
    buildPhase = ''
      runHook preBuild
      $CC -shared -fPIC -O2 -Wall -Wextra -o wine-dragfix.so wine-dragfix.c -ldl
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
  # The app contains Windows PE binaries, not ELF binaries.
  dontStrip = true;
  dontPatchELF = true;

  installPhase = ''
    runHook preInstall

    share=$out/share/paintdotnet
    mkdir -p $share $out/bin $out/lib

    cp -r app $share/app
    rm -f $share/app/*.sh

    install -Dm644 -t $share/fonts fonts/*.ttf
    install -Dm644 ${./theme/win10dark.msstyles} $share/theme/win10dark.msstyles
    install -Dm644 ${./theme/apply-theme.exe} $share/theme/apply-theme.exe
    install -Dm755 ${dragfix}/lib/wine-dragfix.so $out/lib/wine-dragfix.so

    magick app/paintdotnet.ico icon-%d.png
    for f in icon-*.png; do
      size=$(magick identify -format '%w' "$f")
      install -Dm644 "$f" $out/share/icons/hicolor/''${size}x''${size}/apps/paintdotnet.png
    done

    substitute ${./launcher.sh} $out/bin/paintdotnet \
      --subst-var-by bash ${stdenv.shell} \
      --subst-var-by wine ${winePackage} \
      --subst-var-by runtimePath ${
        lib.makeBinPath [
          coreutils
          findutils
          util-linux
        ]
      } \
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
    description = "Paint.NET image editor running under Wine";
    homepage = "https://github.com/paintdotnet/Paint.NET-on-Wine";
    license = lib.licenses.unfree;
    sourceProvenance = [ lib.sourceTypes.binaryNativeCode ];
    platforms = [ "x86_64-linux" ];
    mainProgram = "paintdotnet";
  };
}
