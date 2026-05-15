{
  lib,
  stdenv,
  version,
  src,

  # nativeBuildInputs
  cmake,
  ninja,
  pkg-config,

  # buildInputs
  curl,
  fftwFloat,
  freetype,
  libebur128,
  libGL,
  libpulseaudio,
  pipewire,
  sdl3,
  sdl3-image,
  yaml-cpp,
}:

stdenv.mkDerivation (finalAttrs: {
  pname = "pulse-visualizer";
  inherit version src;

  nativeBuildInputs = [
    cmake
    ninja
    pkg-config
  ];

  buildInputs = [
    curl
    fftwFloat
    freetype
    libebur128
    libGL
    libpulseaudio
    pipewire
    sdl3
    sdl3-image
    yaml-cpp
  ];

  strictDeps = true;
  enableParallelBuilding = true;

  meta = {
    description = "Real-time audio visualizer inspired by MiniMeters";
    homepage = "https://github.com/Audio-Solutions/pulse-visualizer";
    license = lib.licenses.gpl3;
    maintainers = with lib.maintainers; [ miyu ];
    platforms = lib.platforms.x86_64;
    badPlatforms = lib.platforms.darwin;
    mainProgram = "pulse-visualizer";
  };
})
