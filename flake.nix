{
  description = "Paint.NET on Linux via the official Paint.NET-on-Wine build";

  # Pinned separately from your system nixpkgs: Paint.NET needs Wine >= 11.15.
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

      # Builds against your nixpkgs instead (needs wineWow64Packages.unstableFull >= 11.15 there, and
      # allowUnfree for "paintdotnet").
      overlays.default = final: _prev: {
        paintdotnet = final.callPackage ./package.nix { };
      };

      nixosModules.default =
        { lib, ... }:
        {
          imports = [ ./module.nix ];
          programs.paintdotnet.package = lib.mkDefault self.packages.${system}.paintdotnet;
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
