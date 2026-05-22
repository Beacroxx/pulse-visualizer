/*
 * Pulse Audio Visualizer
 * Copyright (C) 2025 Beacroxx
 * Copyright (C) 2025 Contributors (see CONTRIBUTORS.md)
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "include/audio_engine.hpp"
#include "include/common.hpp"
#include "include/config.hpp"
#include "include/config_window.hpp"
#include "include/dsp.hpp"
#include "include/graphics.hpp"
#include "include/plugin.hpp"
#include "include/sdl_window.hpp"
#include "include/spline.hpp"
#include "include/theme.hpp"
#include "include/visualizer_registry.hpp"
#include "include/window_manager.hpp"

#include <SDL3/SDL_main.h>
#include <chrono>
#include <thread>

#if not(_WIN32)
#include <errno.h>
#include <sys/ptrace.h>
#endif

#ifdef USE_UPDATER
#include "include/update_window.hpp"

#include <curl/curl.h>
#endif

namespace CmdlineArgs {
bool debug = false;
bool help = false;
#ifdef _WIN32
bool console = false;
#endif
} // namespace CmdlineArgs

std::string expandUserPath(const std::string& path) {
  if (!path.empty() && path[0] == '~') {
#ifdef _WIN32
    const char* home = getenv("USERPROFILE");
#else
    const char* home = getenv("HOME");
#endif
    if (home) {
      return std::string(home) + path.substr(1);
    }
  }
  return path;
}

// checks if a debugger is present
bool debuggerPresent() {
#if _WIN32
  return IsDebuggerPresent();
#else
  errno = 0;

  // try to trace, will fail if a debugger is present
  if (ptrace(PTRACE_TRACEME, 0, nullptr, nullptr) == -1) {
    return errno == EPERM;
  }

  // detach if we succeeded
  ptrace(PTRACE_DETACH, 0, nullptr, nullptr);
  return false;
#endif
}

void reconfigure() {
  Theme::load();

  // Cleanup fonts for all SDL windows
  logDebug("Cleaning up fonts for all SDL windows");
  Graphics::Font::cleanup();

  logDebug("Reordering windows");
  WindowManager::initialize();

  // Load fonts for each SDL window
  logDebug("Loading fonts for each SDL window");
  Graphics::Font::load();

  // Set window decorations
  logDebug("Setting window decorations");
  SDL_SetWindowBordered(SDLWindow::states["main"].win, Config::options.window.decorations);
  SDL_SetWindowAlwaysOnTop(SDLWindow::states["main"].win, Config::options.window.always_on_top);

  logDebug("Reconfiguring Audio...");
  AudioEngine::reconfigure();
  DSP::FFT::recreatePlans();
  DSP::ConstantQ::regenerate();
  DSP::Lowpass::reconfigure();
  DSP::LUFS::init();
}

// Thread synchronization
BoolSignal dataRdy {};

int main(int argc, char** argv) {
  for (int i = 0; i < argc; i++) {
    if (argv[i][0] == '-') {
      switch (argv[i][1]) {
      case '-':
        if (std::string(argv[i]) == "--debug") {
          CmdlineArgs::debug = true;
        } else if (std::string(argv[i]) == "--help") {
          CmdlineArgs::help = true;
#ifdef _WIN32
        } else if (std::string(argv[i]) == "--console") {
          CmdlineArgs::console = true;
#endif
        }
        break;
      case 'd':
        CmdlineArgs::debug = true;
        break;
      case 'h':
        CmdlineArgs::help = true;
        break;
#ifdef _WIN32
      case 'c':
        CmdlineArgs::console = true;
        break;
#endif
      }
    }
  }

  std::cout << "pulse-visualizer v" << VERSION_STRING << " commit " << VERSION_COMMIT << "\n";
  if (CmdlineArgs::help) {
    std::cout << "Usage: pulse-visualizer [options]\n";
    std::cout << "Options:\n";
    std::cout << "  -d, --debug       Enable debug mode\n";
    std::cout << "  -h, --help        Show this help message\n";
#ifdef _WIN32
    std::cout << "  -c, --console     Open console window (Windows only)\n";
#endif
    return 0;
  }

  // force debug mode on if a debugger is present
  if (debuggerPresent()) {
    std::cout << "Debugger detected, activating debug mode\n";
    CmdlineArgs::debug = true;
  }

#ifndef _WIN32
  sigset_t sigset;
  if (!CmdlineArgs::debug) {
    // Block Signals so they can be polled
    sigemptyset(&sigset);
    sigaddset(&sigset, SIGWINCH);
    sigaddset(&sigset, SIGINT);
    sigaddset(&sigset, SIGTERM);
    sigaddset(&sigset, SIGQUIT);
    sigprocmask(SIG_BLOCK, &sigset, nullptr);
  }
#endif

#ifdef _WIN32
  if (!CmdlineArgs::console && !CmdlineArgs::debug)
    FreeConsole();
#endif

  // Initialize DSP buffers
  DSP::bufferMid.resize(DSP::bufferSize);
  DSP::bufferSide.resize(DSP::bufferSize);
  DSP::bandpassed.resize(DSP::bufferSize);
  DSP::lowpassed.resize(DSP::bufferSize);

  // Setup configuration
  logDebug("Copying Files");
  Config::copyFiles();

  // Load plugins
  logDebug("Loading Plugins");
  Plugin::loadAll();

  logDebug("Loading Config");
  try {
    Config::load();
  } catch (const std::exception& e) {
    std::cerr << "ERROR: " << e.what() << std::endl;
  }

  // Setup theme
  logDebug("Loading theme");
  Theme::load();

  // Initialize SDL and OpenGL
  logDebug("Initializing SDL and OpenGL");
  SDLWindow::init();

  // Clear and display the base window
  logDebug("Clearing and displaying base window");
  SDLWindow::clear();
  SDLWindow::display();

  // Set window decorations
  logDebug("Setting initial window decorations");
  SDL_SetWindowBordered(SDLWindow::states["main"].win, Config::options.window.decorations);
  SDL_SetWindowAlwaysOnTop(SDLWindow::states["main"].win, Config::options.window.always_on_top);

  // Initialize audio and DSP components
  logDebug("Initializing audio and DSP components");
  AudioEngine::init();
  DSP::ConstantQ::init();
  DSP::ConstantQ::generate();
  DSP::FFT::init();
  DSP::Lowpass::init();
  DSP::LUFS::init();

  // Setup window management
  logDebug("Setting up window management");
  WindowManager::initialize();

  // Start DSP processing thread
  logDebug("Starting DSP processing thread");
  std::jthread DSPThread(DSP::Threads::mainThread);

  // Initialise libcurl
#ifdef USE_UPDATER
  logDebug("Initialising libcurl");
  if (curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK) {
    logDebug("Checking for updates");
    UpdaterWindow::check();
  }
#endif

  // Frame timing variables
  logDebug("Setting up frame timing variables");
  int frameCount = 0;

  // Start plugins
  logDebug("Starting plugins");
  Plugin::startAll();

  // Depth Buffer stuff
  glEnable(GL_DEPTH_TEST);
  glDepthFunc(GL_LEQUAL);
  glEnable(GL_DEPTH_TEST);
  glDepthMask(GL_TRUE);

  // Main application loop
  logDebug("Starting main application loop");
  try {
    while (1) {
      // Handle configuration reloading
      if (Config::reload()) {
        logDebug("Config reloaded");
        reconfigure();
        Plugin::notifyConfigReload();
      }

      // Handle theme reloading
      if (Theme::reload()) {
        // Cleanup fonts for all SDL windows
        logDebug("Cleaning up fonts for all SDL windows");
        Graphics::Font::cleanup();

        // Load fonts for each SDL window
        logDebug("Loading fonts for each SDL window");
        Graphics::Font::load();
      }

      // Process SDL events
      SDL_Event event;
      while (SDL_PollEvent(&event)) {
        SDLWindow::handleEvent(event);
        ConfigWindow::handleEvent(event);
#ifdef USE_UPDATER
        UpdaterWindow::handleEvent(event);
#endif
        WindowManager::handleEvent(event);
        Plugin::handleEvent(event);
      }

#ifndef _WIN32
      // Poll Signals
      struct timespec ts = {0, 0};
      int sig = sigtimedwait(&sigset, nullptr, &ts);
      if (sig == SIGINT || sig == SIGTERM || sig == SIGQUIT)
        break;
#endif
      if (SDLWindow::stopSource.stop_requested())
        break;

      glUseProgram(0);
      if (Config::broken)
        continue;

      WindowManager::updateBounds();

      // Wait for DSP data to be ready
      dataRdy.wait_for(100ms);

      // Render frame
      SDLWindow::clear();
      WindowManager::render();
      ConfigWindow::draw();
#ifdef USE_UPDATER
      UpdaterWindow::draw();
#endif
      Plugin::drawAll();
      SDLWindow::display();

      // FPS logging if enabled
      if (Config::options.debug.log_fps) {
        frameCount++;
        static auto fpsStart = std::chrono::steady_clock::now();
        static auto lastPrint = fpsStart;
        auto now = std::chrono::steady_clock::now();
        if (now - lastPrint >= 1s) {
          float fps = frameCount / std::chrono::duration<float>(now - fpsStart).count();
          std::cout << "FPS: " << static_cast<int>(fps) << std::endl;
          frameCount = 0;
          fpsStart = now;
          lastPrint = now;
        }
      }
    }
  } catch (const std::exception& e) {
    std::cerr << "ERROR: " << e.what() << std::endl;
  }

  // Cleanup
  logDebug("Cleaning up...");
  DSPThread.request_stop();
  WindowManager::cleanup();
  Graphics::Font::cleanup();
  AudioEngine::cleanup();
  DSP::FFT::cleanup();
  Config::cleanup();
  Theme::cleanup();
  SDLWindow::deinit();
  if (DSPThread.joinable())
    DSPThread.join();
  VisualizerRegistry::cleanup();
  Plugin::unloadAll();

#ifdef USE_UPDATER
  UpdaterWindow::cleanup();
  curl_global_cleanup();
#endif

  return 0;
}