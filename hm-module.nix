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

  # home.packages links share/applications and share/icons into the profile, which
  # Home Manager puts on XDG_DATA_DIRS, so launchers (rofi, fuzzel, ...) find it.
  # Vulkan drivers still need hardware.graphics.enable in the NixOS config.
  config = lib.mkIf cfg.enable {
    home.packages = [ cfg.package ];
  };
}
