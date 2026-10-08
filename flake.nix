{
  description = "Paint.NET on Linux with Wine";

  # Older system nixpkgs may not have the Wine version Paint.NET needs.
  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

  outputs =
    { self, nixpkgs }:
    let
      system = "x86_64-linux";
      pkgs = import nixpkgs {
        inherit system;
        config.allowUnfreePredicate = pkg: nixpkgs.lib.getName pkg == "paintdotnet";
      };
    in
    {
      packages.${system} = rec {
        paintdotnet = pkgs.callPackage ./package.nix { };
        default = paintdotnet;
      };

      overlays.default = final: _prev: {
        paintdotnet = final.callPackage ./package.nix { };
      };

      nixosModules.default =
        { lib, ... }:
        {
          imports = [ ./module.nix ];
          programs.paintdotnet.package = lib.mkDefault (
            self.packages.${system}.paintdotnet.override { withMesaFallback = false; }
          );
        };

      homeManagerModules.default =
        { lib, ... }:
        {
          imports = [ ./hm-module.nix ];
          programs.paintdotnet.package = lib.mkDefault self.packages.${system}.paintdotnet;
        };

      formatter.${system} = pkgs.nixfmt;
    };
}
