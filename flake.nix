{
  description = "aro, a tiling Wayland compositor";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

  outputs = { self, nixpkgs }:
    let
      systems = [ "x86_64-linux" "aarch64-linux" ];
      forAll = f: nixpkgs.lib.genAttrs systems (system: f nixpkgs.legacyPackages.${system});
    in
    {
      packages = forAll (pkgs: rec {
        aro = pkgs.callPackage ./nix/package.nix { };
        default = aro;
      });

      devShells = forAll (pkgs: {
        default = pkgs.mkShell {
          inputsFrom = [ self.packages.${pkgs.stdenv.hostPlatform.system}.aro ];
        };
      });

      overlays.default = final: prev: {
        aro = final.callPackage ./nix/package.nix { };
      };

      nixosModules.default = import ./nix/module.nix self;
    };
}
