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

#pragma once

#define _USE_MATH_DEFINES

#include <glad/gl.h>
// ^ glad needs to be included first
#include <SDL3/SDL.h>
#include <SDL3/SDL_opengl.h>
#include <algorithm>
#include <any>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <complex>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <ebur128.h>
#include <fftw3.h>
#include <filesystem>
#include <format>
#include <fstream>
#include <ft2build.h>
#include <functional>
#include <iostream>
#include <mutex>
#include <numeric>
#include <optional>
#include <ranges>
#include <signal.h>
#include <source_location>
#include <span>
#include <sstream>
#include <string_view>
#include <sys/stat.h>
#include <sys/types.h>
#include <thread>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>
#include <yaml-cpp/yaml.h>
#include FT_FREETYPE_H

using namespace std::literals;

namespace CmdlineArgs {
extern bool debug;
extern bool help;
#ifdef _WIN32
extern bool console;
#endif
} // namespace CmdlineArgs

constexpr const char* constexpr_basename(const char* p) noexcept {
  const char* b = p;
  const char* s = nullptr;
  while (*b) {
    if (*b == '/' || *b == '\\')
      s = b;
    ++b;
  }
  return s ? s + 1 : p;
}

constexpr const char* constexpr_trim_ret(const char* p) noexcept {
  if (!p || !*p)
    return p;
  const char* open = nullptr;
  const char* cur = p;

  while (*cur && *cur != '(')
    ++cur;
  if (!*cur)
    return p;
  open = cur;

  while (open > p && (open[-1] != ' ' && open[-1] != '>'))
    --open;
  return open < cur ? open : p;
}

/**
 * @brief Constructs a std::runtime_error with a formatted error message including source location information.
 * @param fmt The format string, compatible with std::format.
 * @param args Arguments to be formatted into the format string.
 * @param loc The source location where the error occurred (defaults to current location).
 * @return std::runtime_error The constructed runtime error with the formatted message.
 */
template <typename... Args>
std::runtime_error makeErrorAt(std::source_location loc, std::format_string<Args...> fmt, Args&&... args) {
  return std::runtime_error(std::format("{} | {}:{} in {}", std::format(fmt, std::forward<Args>(args)...),
                                        constexpr_basename(loc.file_name()), loc.line(),
                                        constexpr_trim_ret(loc.function_name())));
}

/**
 * @brief Prints a formatted warning message to std::clog including source location information.
 * @param loc The source location where the warning occurred (defaults to current location).
 * @param fmt The format string, compatible with std::format.
 * @param args Arguments to be formatted into the format string.
 */
template <typename... Args> void logWarnAt(std::source_location loc, std::format_string<Args...> fmt, Args&&... args) {
  std::clog << "WARN: " << std::format(fmt, std::forward<Args>(args)...) << " | " << constexpr_basename(loc.file_name())
            << ":" << loc.line() << " in " << constexpr_trim_ret(loc.function_name()) << "\n";
}

/**
 * @brief Logs a formatted debug message string to stderr.
 * @param fmt The format string, compatible with std::format.
 * @param args Arguments to be formatted into the format string.
 */
template <typename... Args> void logDebug(std::format_string<Args...> fmt, Args&&... args) {
  if (!CmdlineArgs::debug)
    return;

  std::clog << "DEBUG: " << std::format(fmt, std::forward<Args>(args)...) << '\n';
}

#ifdef HAVE_AVX2
#include <immintrin.h>

#ifndef HAVE_MM256_LOG10_PS

/**
 * @brief Computes log10 for 8 float values using AVX2
 * @param x Vector of 8 float values
 * @return Vector with log10 applied to each element
 */
static inline __m256 _mm256_log10_ps(__m256 x) {
  float vals[8];
  _mm256_storeu_ps(vals, x);
  for (int i = 0; i < 8; ++i)
    vals[i] = log10f(vals[i]);
  return _mm256_loadu_ps(vals);
}

#endif

#ifndef HAVE_MM256_POW_PS

/**
 * @brief Computes power function for 8 float values using AVX2
 * @param a Base values vector
 * @param b Exponent values vector
 * @return Vector with pow(a, b) applied to each element
 */
static inline __m256 _mm256_pow_ps(__m256 a, __m256 b) {
  float va[8], vb[8], vr[8];
  _mm256_storeu_ps(va, a);
  _mm256_storeu_ps(vb, b);
  for (int i = 0; i < 8; ++i)
    vr[i] = powf(va[i], vb[i]);
  return _mm256_loadu_ps(vr);
}

#endif

/**
 * @brief Reduces 8 float values to a single sum using AVX2
 * @param v Vector of 8 float values
 * @return Sum of all elements in the vector
 */
inline float avx2_reduce_add_ps(__m256 v) {
  __m128 vLow = _mm256_castps256_ps128(v);
  __m128 vHigh = _mm256_extractf128_ps(v, 1);
  vLow = _mm_add_ps(vLow, vHigh);
  vLow = _mm_hadd_ps(vLow, vLow);
  vLow = _mm_hadd_ps(vLow, vLow);
  return _mm_cvtss_f32(vLow);
}
#endif

#ifdef __linux__
#include <poll.h>
#include <sys/inotify.h>
#include <unistd.h>
#endif

#ifdef _WIN32
#define NOMINMAX
#include <comdef.h>
#include <cstddef>
// clang-format off
#include <propsys.h>
#include <functiondiscoverykeys_devpkey.h>
// clang-format on

typedef ptrdiff_t ssize_t;
#endif

#if HAVE_PULSEAUDIO
#include <pulse/context.h>
#include <pulse/error.h>
#include <pulse/introspect.h>
#include <pulse/mainloop.h>
#include <pulse/simple.h>
#endif

#if HAVE_PIPEWIRE
#include <pipewire/keys.h>
#include <pipewire/pipewire.h>
#include <pipewire/thread-loop.h>
#include <spa/param/audio/format-utils.h>
#include <spa/pod/builder.h>
#endif

#if HAVE_WASAPI

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <assert.h>
#include <audioclient.h>
#include <initguid.h>
#include <mmdeviceapi.h>

#endif

#ifndef FLT_EPSILON
#define FLT_EPSILON 1e-6f
#endif

class BoolSignal {
public:
  void set() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      flag_ = true;
    }
    cv_.notify_one();
  }

  bool wait_for(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (!cv_.wait_for(lock, timeout, [this] { return flag_; }))
      return false;
    flag_ = false;
    return true;
  }

  void wait() {
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait(lock, [this] { return flag_; });
    flag_ = false;
  }

  std::mutex mutex_;
  std::condition_variable cv_;
  bool flag_ = false;
};

// Thread synchronization variables for DSP data processing
extern BoolSignal dataRdy;

/**
 * @brief Expands a path starting with '~' to the user's home directory
 * @param path The path to expand
 * @return The expanded path with home directory substituted
 */
std::string expandUserPath(const std::string& path);

/**
 * @brief Reconfigures the program with new config data
 */
void reconfigure();

/**
 * @brief convert GLenum to human readable error string.
 * @param err The GLenum to convert.
 * @return Pointer to the human readable error string.
 */
const char* glErrorString(GLenum err);

/**
 * @brief Helper struct for creating a visitor for std::visit with multiple lambdas.
 * @tparam Ts Lambda types to inherit from.
 */
template <class... Ts> struct Visitor : Ts... {
  using Ts::operator()...;
};

template <class... Ts> Visitor(Ts...) -> Visitor<Ts...>;

/**
 * @brief Concept that requires a visitor to be invocable with every alternative type of a variant.
 * @tparam Visitor The visitor type.
 * @tparam Variant The variant type whose alternatives must all be visitable by `Visitor`.
 */
template <class Visitor, class Variant>
concept visitable = []<std::size_t... I>(std::index_sequence<I...>) {
  return (std::invocable<Visitor, std::variant_alternative_t<I, Variant>&> && ...);
}(std::make_index_sequence<std::variant_size_v<Variant>> {});