{
  config,
  lib,
  pkgs,
  ...
}:

let
  cfg = config.programs.paintdotnet;
in
{
  options.programs.paintdotnet = {
    enable = lib.mkEnableOption "Paint.NET (via Wine)";

    package = lib.mkOption {
      type = lib.types.package;
      default = pkgs.callPackage ./package.nix { };
      defaultText = lib.literalExpression "paint.nix's paintdotnet package";
      description = "The Paint.NET package to install.";
    };
  };

  config = lib.mkIf cfg.enable {
    environment.systemPackages = [ cfg.package ];

    # Desktop entry + icons, so launchers (rofi, fuzzel, GNOME, KDE, ...) list Paint.NET
    environment.pathsToLink = [
      "/share/applications"
      "/share/icons"
    ];

    # DXVK renders through Vulkan; Wine finds the drivers via /run/opengl-driver.
    hardware.graphics.enable = lib.mkDefault true;
  };
}
