# pulse-visualizer - Audio Visualizer

<video autoplay loop muted src="https://github.com/user-attachments/assets/8a65f8bc-7dfd-477d-88d7-20f65e7e54ad"></video>
<a href="https://repology.org/project/pulse-visualizer/versions">
    <img src="https://repology.org/badge/vertical-allrepos/pulse-visualizer.svg?allow_ignored=1"
         alt="Packaging status" align="right">
</a>

## What Even Is This?

Ever wanted to *see* your music? Pulse is a real-time audio visualizer inspired
by [MiniMeters](https://minimeters.app/). It turns whatever your system is
playing into eye-candy (or at least, that's the idea). Built in C++ with SDL3,
OpenGL, PulseAudio, and FFTW, Pulse tries to make your music look as cool as it
sounds. Unless your system sucks, then, well, good luck.

## Features

- Real-time, low-latency audio visualization
- CRT phosphor emulation with GPU compute shaders (glow, persistence, etc)
  - Curved screen, vignette, grain, chromatic aberration
  - Blur, light spread, and overdrive are configurable
  - Screen reflections
  - Rainbow beam
- Multiple visualizer styles:
  - Lissajous curve (spline interpolation, stretch modes)
  - Oscilloscope (gradient modes, pitch following, cycle limiting)
  - FFT/Constant-Q (mid/side, right/left, sharp/flat note key)
  - Spectrogram (log/linear scale, iterative reassignment)
  - Peak and LUFS meters (short-term, momentary, integrated)
  - VU meter (digital and analog)
- Dynamic tiled layout
- Settings menu (press `m` to open)
- Separate FFT and CQT threads for mid/side channels
- SIMD acceleration (AVX2) where supported
- Live config and theme hot-reloading
- Draggable splitters for custom layout
- Cross-platform audio backends: PulseAudio and PipeWire
- Hardware-accelerated rendering (OpenGL)
- A collection of ready-made themes (see `themes/`)
- Plugin support

## Installation

### Arch Linux (AUR)

Just grab it from the AUR as `pulse-visualizer-git`:

```bash
yay -S pulse-visualizer-git
```

### NixOS

Use the unstable channel for packages:

```bash
environment.systemPackages = [ pkgs.pulse-visualizer ];
```

### Other Distros

You can get the binary from the [Releases page](https://github.com/Beacroxx/pulse-visualizer/releases/latest).
There is an install script in the tarball,
make sure to run it as root before running the binary:

```bash
tar -xvf pulse-visualizer-<version>.tar.gz
cd pulse-visualizer-<version>
chmod +x install.sh
sudo ./install.sh
```

## Dependencies & Platform Support

- SDL3
- SDL3_image
- PulseAudio **or** PipeWire (0.3+)
- FFTW3
- FreeType2
- OpenGL
- YAML-CPP
- libebur128
- libcurl

**Works on:**

- Linux (PulseAudio or PipeWire)
- BSD (PulseAudio)
- Windows (WASAPI)

## Configuration

pulse-visualizer now has a settings menu! You can open it by pressing `m`.
You can also edit the config file directly.

For comprehensive configuration documentation, see [CONFIGURATION.md](docs/CONFIGURATION.md).

### Theming

You can pick a theme in the `Window` page in the settings menu.

You can also create your own themes by copying a template and editing the colors.
Theme files support a bunch of color and property keys
(see `_TEMPLATE.txt` for all options). All the main colors are required.
You can fine-tune visualizer-specific stuff too.
Theme and config changes reload live.

### Moving Visualizers

Visualizers can be rearranged directly in the main window using a swaywm-like
drag-and-drop interaction. Drag a visualizer and drop it on another visualizer's
center to replace it, or drop on the top/left/right/bottom edges of a
visualizer to create a split. The layout tree is adjusted automatically.
Only one instance of each visualizer ID may exist in the layout.

Note: resizing a visualizer does not automatically save the ratios to the
configuration file. To persist dimensional changes, open the configuration
menu and press the save button or press the `s` key.

## Plugin Support

Pulse has experimental plugin support.  
Plugins are `.so`/`.dll` files dropped into `~/.config/pulse-visualizer/plugins/` on linux
and `C:\Users\<username>\.config\pulse-visualizer\plugins` on Windows
and can draw using a small rendering API, handle SDL events, and access config/theme data.

See [PLUGINS.md](docs/PLUGINS.md) for details on the plugin lifecycle, required symbols, and the `PvAPI` interface.

## Usage

Just run:

```bash
pulse-visualizer
```

- Drag splitters to resize/rearrange visualizers
- Lissajous will enforce a square aspect ratio
- Hover FFT for real-time frequency, dB, and note readout
- All config and theme changes are live, no restart needed

## Building

If you want to build from source, you can do so with the following commands:

### Build dependencies

- C++20 compiler
- CMake 3.10+
- Ninja-build
- Development headers for: SDL3, SDL3_image, PulseAudio or PipeWire (0.3+), FFTW3, FreeType2, YAML-CPP, libebur128, libcurl

Fedora:
```sudo dnf5 install SDL3-devel SDL3_image-devel fftw3-devel freetype-devel yaml-cpp-devel ninja pipewire-devel libebur128-devel libcurl-devel```  
Debian:
```sudo apt install clang cmake libsdl3-dev libsdl3-image-dev libfftw3-dev libfreetype-dev libyaml-cpp-dev ninja libebur128-dev libcurl-dev```  

```bash
mkdir build
cd build
cmake -G Ninja -DCMAKE_BUILD_TYPE=Release ..
ninja
sudo ninja install
```

#### Nix

If you want to try out the latest version using Nix, there's a `flake.nix` with which you can use to:

```
nix run github:Audio-Solutions/pulse-visualizer
```

Or if you don't have flakes enabled already:

```bash
git clone https://github.com/Audio-Solutions/pulse-visualizer
nix-build
```

To install it as a system package in your NixOS flake setup, add to `flake.nix`:

```nix
inputs.pulse-visualizer.url = "github:Audio-Solutions/pulse-visualizer";
```

And then under `outputs`, use:

```nix
environment.systemPackages = [ pulse-visualizer.packages.${system}.default ];
```

Or:

```nix
nixpkgs.overlays = [ pulse-visualizer.overlays.default ];
environment.systemPackages = [ pkgs.pulse-visualizer ];
```

#### Note

Do not run the build as root. Use a regular user to build (`ninja`) and only
use `sudo ninja install` for the install step to avoid ownership and permission
issues.

## Contributing & Support

Want to help? PRs welcome! Please clang-format your code before submitting
(see the clang-format file). Sorry for the chaos.

See [CONTRIBUTORS](docs/CONTRIBUTORS.md) for a list of contributors to this project.

If you like pulse-visualizer, you can buy me a coffee at [ko-fi.com/beacrox](https://ko-fi.com/beacrox).

## License

This project is licensed under the GNU General Public License v3.0 -
see the [LICENSE](LICENSE) file for details.

## Acknowledgments

- [MiniMeters](https://minimeters.app/) for inspiration
- [Richard Andersson](https://richardandersson.net/?p=350) for the Phosphor effect
- [JetBrains](https://www.jetbrains.com/) and the [Nerd Fonts](https://www.nerdfonts.com/)
project for the JetBrains Mono Nerd Font
- SDL3, PulseAudio, PipeWire, FFTW, FreeType, libebur128, YAML-CPP, and everyone
else who made this possible

## Star History

[![Star History Chart](https://api.star-history.com/image?repos=Audio-Solutions/pulse-visualizer&type=date&legend=top-left)](https://www.star-history.com/?repos=Audio-Solutions%2Fpulse-visualizer&type=date&legend=top-left)
