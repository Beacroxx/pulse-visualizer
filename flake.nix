{
  description = "Real-time audio visualizer inspired by MiniMeters";
  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    flake-parts.url = "github:hercules-ci/flake-parts";
    flake-compat.url = "github:NixOS/flake-compat";
  };
  outputs =
    inputs@{ flake-parts, self, ... }:
    flake-parts.lib.mkFlake { inherit inputs; } {
      systems = [
        "x86_64-linux"
        "aarch64-linux"
      ];
      perSystem =
        {
          pkgs,
          lib,
          self',
          ...
        }:
        let
          # https://discourse.nixos.org/t/passing-git-commit-hash-and-tag-to-build-with-flakes/11355/2
          versionRev = if (self ? rev) then (builtins.substring 0 7 self.rev) else "dirty";
          # TODO: Set version prefix only once somewhere, and read that here
          # and in ./CMakeLists.txt
          version = "1.3.9-${versionRev}-flake";
        in
        {
          packages.default = pkgs.callPackage ./package.nix {
            inherit version;
            src = ./.;
          };
          packages.pulse-visualizer = self'.packages.default;
          packages.pulse-visualizer-with-clang = self'.packages.default.override {
            stdenv = pkgs.clangStdenv;
          };

          devShells.default = pkgs.mkShell {
            inputsFrom = [ self'.packages.default ];
          };

          apps.default = {
            type = "app";
            program = pkgs.lib.getExe self'.packages.default;
          };
        };

      flake.overlays.default = final: prev: {
        pulse-visualizer = self.packages.${final.system}.default;
      };
    };
}
