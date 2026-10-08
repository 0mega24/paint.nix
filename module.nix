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
    enable = lib.mkEnableOption "Paint.NET with Wine";

    package = lib.mkOption {
      type = lib.types.package;
      default = pkgs.callPackage ./package.nix { withMesaFallback = false; };
      defaultText = lib.literalExpression "paint.nix's paintdotnet package";
      description = "Package used to run Paint.NET.";
    };
  };

  config = lib.mkIf cfg.enable {
    environment.systemPackages = [ cfg.package ];

    environment.pathsToLink = [
      "/share/applications"
      "/share/icons"
    ];

    # DXVK needs Vulkan drivers available to Wine.
    hardware.graphics.enable = lib.mkDefault true;
  };
}
