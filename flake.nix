{
  description = "Real-time audio visualizer inspired by MiniMeters";
  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    flake-parts.url = "github:hercules-ci/flake-parts";
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
          mkPulseVisualizer = pkgs.clangStdenv.mkDerivation {
            pname = "pulse-visualizer";
            version = "1.3.9";
            src = ./.;

            nativeBuildInputs = [
              pkgs.cmake
              pkgs.ninja
              pkgs.pkg-config
            ];

            buildInputs = [
              pkgs.sdl3
              pkgs.sdl3-image
              pkgs.libpulseaudio
              pkgs.pipewire
              pkgs.fftwFloat
              pkgs.freetype
              pkgs.libGL
              pkgs.curl
              pkgs.yaml-cpp
              pkgs.libebur128
            ];

            strictDeps = true;
            enableParallelBuilding = true;

            postPatch = ''
              substituteInPlace CMakeLists.txt \
                --replace-fail " -march=native" "" \
                --replace-fail " -mtune=native" "" \
                --replace-fail "-Wl,-s" "" \
                --replace-fail " -s" ""
            '';

            meta = {
              description = "Real-time audio visualizer inspired by MiniMeters";
              homepage = "https://github.com/Audio-Solutions/pulse-visualizer";
              license = lib.licenses.gpl3;
              maintainers = with lib.maintainers; [ miyu ];
              platforms = lib.platforms.linux;
              badPlatforms = lib.platforms.darwin;
            };
          };
        in
        {
          packages.default = mkPulseVisualizer;
          packages.pulse-visualizer = mkPulseVisualizer;

          devShells.default = pkgs.mkShell {
            inputsFrom = [ mkPulseVisualizer ];
          };

          apps.default = {
            type = "app";
            program = "${self'.packages.default}/bin/pulse-visualizer";
          };
        };

      flake.overlays.default = final: prev: {
        pulse-visualizer = self.packages.${final.system}.default;
      };
    };
}
