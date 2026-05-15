{
  description = "Real-time audio visualizer inspired by MiniMeters";
  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    flake-parts.url = "github:hercules-ci/flake-parts";
    flake-compat.url = "github:NixOS/flake-compat";
    gitignore = {
      url = "github:hercules-ci/gitignore.nix";
      inputs.nixpkgs.follows = "nixpkgs";
    };
  };
  outputs =
    inputs@{ flake-parts, gitignore, self, ... }:
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
          inherit (gitignore.lib) gitignoreSource;
        in
        {
          packages.default = pkgs.callPackage ./package.nix {
            inherit version;
            src = lib.cleanSourceWith {
              # Ignore many files that gitignoreSource doesn't ignore, see:
              # https://github.com/hercules-ci/gitignore.nix/issues/9#issuecomment-635458762
              filter = path: type:
                let
                  rel = lib.removePrefix (toString ./. + "/") (toString path);
                in
                !(builtins.elem rel [
                  # Nix files
                  "flake.nix"
                  "flake.lock"
                  "default.nix"
                  "shell.nix"
                  "package.nix"
                  # vcpkg Microsoft files
                  "vcpkg.json"
                  "vcpkg-configuration.json"
                  # Git files
                  ".github"
                  ".git"
                  # Other files that shouldn't affect Nix build
                  "pkg/install.sh"
                  "pkg/uninstall.sh"
                  ".clang-format"
                ]);
              src = gitignoreSource ./.;
            };
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
