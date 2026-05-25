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

#include "include/sdl_window.hpp"

#include "include/config.hpp"
#include "include/config_window.hpp"
#include "include/graphics.hpp"
#include "include/theme.hpp"
#include "include/window_manager.hpp"

#include <SDL3_image/SDL_image.h>

namespace SDLWindow {
// Map of window states by group
std::unordered_map<std::string, State> states;

// Global buffer handles
GLuint vertexBuffer = 0;
GLuint vertexColorBuffer = 0;

std::stop_source stopSource;

SDL_Surface* icon = nullptr;

void deinit() {
  // Free any created system cursors before quitting SDL
  freeCursors();

  for (auto& [group, state] : states) {
    SDL_DestroyWindow(state.win);
    SDL_GL_DestroyContext(state.glContext);
  }

  SDL_DestroySurface(icon);

  glDeleteBuffers(1, &vertexBuffer);
  glDeleteBuffers(1, &vertexColorBuffer);

  SDL_Quit();
}

void init() {
#ifdef __linux
  // Prefer wayland natively if enabled in the config
  if (Config::options.window.wayland)
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "wayland,x11");
  else
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "x11");
#endif

  // Initialize SDL video subsystem
  if (!SDL_Init(SDL_INIT_VIDEO)) {
    throw makeErrorAt(std::source_location::current(), "SDL init failed: {}", SDL_GetError());
  }

  // Configure OpenGL context attributes
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 6);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_COMPATIBILITY);
  SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
  SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 8);
  SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
  SDL_GL_SetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT, 1);

  // Disable compositor bypass to enable transparency in DE's like KDE
  SDL_SetHint(SDL_HINT_VIDEO_X11_NET_WM_BYPASS_COMPOSITOR, "0");

  // Create Base SDL window
  createWindow("main", "Pulse " VERSION_FULL, Config::options.window.default_width,
               Config::options.window.default_height);

  // Check if window creation was successful
  if (states.find("main") == states.end()) {
    throw makeErrorAt(std::source_location::current(), "Failed to create main window");
  }

  // Make the OpenGL context current before initializing GLAD
  if (!selectWindow("main")) {
    throw makeErrorAt(std::source_location::current(), "Failed to select main window");
  }

  // Initialize GLAD
  int version = gladLoadGL((GLADloadfunc)SDL_GL_GetProcAddress);
  if (version == 0) {
    const GLubyte* glVersion = glGetString(GL_VERSION);
    if (glVersion)
      throw makeErrorAt(std::source_location::current(), "GLAD initialization failed. OpenGL Version {}",
                        reinterpret_cast<const char*>(glVersion));
    else
      throw makeErrorAt(std::source_location::current(),
                        "GLAD initialization failed. No valid OpenGL context available");
  }

  logDebug("OpenGL Loaded via GLAD: {}.{}", GLAD_VERSION_MAJOR(version), GLAD_VERSION_MINOR(version));

  Graphics::Font::load();

  glGenBuffers(1, &vertexBuffer);
  glGenBuffers(1, &vertexColorBuffer);

  // Configure OpenGL rendering state
  glEnable(GL_BLEND);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
  glEnable(GL_LINE_SMOOTH);
  glHint(GL_LINE_SMOOTH_HINT, GL_NICEST);
  glLineWidth(2.0f);

  // Initialise window size
  SDL_GetWindowSize(states["main"].win, &states["main"].windowSizes.first, &states["main"].windowSizes.second);

  // Create system cursors
  initCursors();
}

// Cursors

void initCursors() {
  for (size_t i = 0; i < g_cursors.size(); ++i) {
    if (!g_cursors[i])
      g_cursors[i] = SDL_CreateSystemCursor(cursor_map[i]);
  }
}

void freeCursors() {
  for (auto& c : g_cursors) {
    if (c) {
      SDL_DestroyCursor(c);
      c = nullptr;
    }
  }
}

void setCursor(CursorType type) {
  auto idx = static_cast<size_t>(type);
  if (idx < g_cursors.size() && g_cursors[idx])
    SDL_SetCursor(g_cursors[idx]);
}

void handleEvent(SDL_Event& event) {
  std::string group = "";
  auto isThisWindow = [&event](auto const& p) { return event.window.windowID == std::get<1>(p).winID; };
  if (auto it = std::ranges::find_if(SDLWindow::states, isThisWindow); it == SDLWindow::states.end())
    return;
  else
    group = it->first;

  switch (event.type) {

  case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
    if (group != "main")
      return;
  case SDL_EVENT_QUIT:
    stopSource.request_stop();
    return;

  case SDL_EVENT_KEY_DOWN:
    switch (event.key.key) {
    case SDLK_Q:
    case SDLK_ESCAPE:
      if (group == "main")
        stopSource.request_stop();
      return;
    case SDLK_M:
      ConfigWindow::toggle();
      break;
    case SDLK_S:
      Config::save();
    default:
      break;
    }
    break;

  case SDL_EVENT_MOUSE_MOTION:
    states[group].mousePos.first = event.motion.x;
    states[group].mousePos.second = states[group].windowSizes.second - event.motion.y;
    break;

  case SDL_EVENT_WINDOW_MOUSE_ENTER:
    states[group].focused = true;
    break;

  case SDL_EVENT_WINDOW_MOUSE_LEAVE:
    states[group].focused = false;
    break;

  case SDL_EVENT_WINDOW_RESIZED:
    states[group].windowSizes.first = event.window.data1;
    states[group].windowSizes.second = event.window.data2;
    WindowManager::boundsDirty.store(true);
    break;

  default:
    break;
  }
}

void display() {
  for (auto& [_, state] : states) {
    SDL_GL_MakeCurrent(state.win, state.glContext);
    SDL_GL_SwapWindow(state.win);
  }
}

void clear() {
  // Clear with current theme background color
  float* c = Theme::colors.background;
  for (auto& [_, state] : states) {
    SDL_GL_MakeCurrent(state.win, state.glContext);
    glClearColor(c[0], c[1], c[2], c[3]);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
  }
}

void createWindow(const std::string& group, const std::string& title, int width, int height, uint32_t flags) {
  // Create SDL window with OpenGL support
  logDebug("Creating window: {}", title);
  SDL_Window* win = SDL_CreateWindow(title.c_str(), width, height, SDL_WINDOW_OPENGL | flags);
  if (!win) {
    throw makeErrorAt(std::source_location::current(), "Failed to create Window: {}", SDL_GetError());
  }

  // Create OpenGL context
  SDL_GLContext glContext;
  if (states.empty()) {
    logDebug("Creating OpenGL context");
    glContext = SDL_GL_CreateContext(win);
    if (!glContext) {
      SDL_DestroyWindow(win);
      throw makeErrorAt(std::source_location::current(), "Failed to create OpenGL Context: {}", SDL_GetError());
    }
  } else {
    logDebug("Using shared OpenGL context");
    glContext = states["main"].glContext;
  }

  // Fix nvidia bug
  SDL_GL_SetSwapInterval(0);

  if (!icon)
    icon = IMG_Load((Config::getInstallDir() + "/icons/icon.ico").c_str());

  if (!icon)
    logWarnAt(std::source_location::current(), "Failed to load window icon from {}/icons/icon.ico",
              Config::getInstallDir());
  else
    SDL_SetWindowIcon(win, icon);

  states[group] = {win, SDL_GetWindowID(win), glContext, std::make_pair(width, height), std::make_pair(0, 0), false};
}

bool destroyWindow(const std::string& group) {
  if (states.find(group) == states.end())
    return false;

  logDebug("Destroying window: {}", group);
  SDL_DestroyWindow(states[group].win);
  if (group == "main")
    SDL_GL_DestroyContext(states[group].glContext);
  states.erase(group);
  selectWindow("main");
  return true;
}

bool selectWindow(const std::string& group) {
  if (states.find(group) == states.end())
    return false;
  SDL_GL_MakeCurrent(states[group].win, states[group].glContext);
  return true;
}

std::optional<std::pair<int, int>> getWindowSize(const std::string& group) {
  auto it = states.find(group);
  if (it == states.end()) {
    return std::nullopt;
  }
  return it->second.windowSizes;
}

std::optional<std::pair<int, int>> getCursorPos(const std::string& group) {
  auto it = states.find(group);
  if (it == states.end()) {
    return std::nullopt;
  }
  return it->second.mousePos;
}

void layer(float z) {
  glMatrixMode(GL_MODELVIEW);
  glPushMatrix();
  glLoadIdentity();
  glTranslatef(0.0f, 0.0f, z);
}

void layer() {
  glMatrixMode(GL_MODELVIEW);
  glPopMatrix();
}

} // namespace SDLWindow