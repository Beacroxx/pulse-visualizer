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

#include "include/config.hpp"
#include "include/dsp.hpp"

namespace AudioEngine {

Type toType(std::string str) {
  std::transform(str.begin(), str.end(), str.begin(), ::tolower);

  if (str == "pulseaudio" || str == "pulse" || str == "pa")
    return Type::PULSEAUDIO;
  else if (str == "pipewire" || str == "pw")
    return Type::PIPEWIRE;
  else if (str == "wasapi" || str == "windows")
    return Type::WASAPI;
  return Type::AUTO;
}

namespace Pulseaudio {
#if HAVE_PULSEAUDIO

// PulseAudio stream and state
pa_simple* paStream;
bool running = false;
bool initialized = false;

// Available audio sources and default sink
std::vector<DeviceInfo> availableSources;
std::string defaultSink;

void sourceInfoCallback(pa_context* ctx, const pa_source_info* info, int eol, void*) {
  if (eol || !info)
    return;

  // Extract device information from PulseAudio
  DeviceInfo dev;
  dev.index = info->index;
  dev.name = info->name;
  dev.desc = info->description;
  dev.isMonitor = info->monitor_of_sink != PA_INVALID_INDEX;
  availableSources.push_back(dev);
}

std::string find(std::string dev) {
  if (dev.empty()) {
    if (defaultSink.empty())
      return "default";

    dev = defaultSink + ".monitor";
  }

  // Search for matching device
  for (const auto& source : availableSources) {
    if (source.name.find(dev) != std::string::npos ||
        source.name.find(dev.substr(0, dev.find(".monitor"))) != std::string::npos) {
      return source.name;
    }
  }

  logWarnAt(std::source_location::current(), "PulseAudio device '{}' not found. Using system default", dev);
  return "default";
}

void cleanup() {
  if (paStream) {
    pa_simple_free(paStream);
    paStream = nullptr;
  }
  initialized = false;
  running = false;
}

void init() {
  if (initialized)
    cleanup();

  availableSources.clear();
  defaultSink.clear();

  // Create PulseAudio mainloop and context
  pa_mainloop* ml = pa_mainloop_new();
  if (!ml)
    throw makeErrorAt(std::source_location::current(), "Failed to create PulseAudio mainloop");

  pa_context* ctx = pa_context_new(pa_mainloop_get_api(ml), "pulse-device-enum");
  if (!ctx) {
    pa_mainloop_free(ml);
    throw makeErrorAt(std::source_location::current(), "Failed to create PulseAudio context");
  }

  pa_context_set_state_callback(ctx, [](pa_context*, void*) {}, nullptr);

  if (pa_context_connect(ctx, nullptr, PA_CONTEXT_NOFLAGS, nullptr) < 0) {
    pa_context_unref(ctx);
    pa_mainloop_free(ml);
    throw makeErrorAt(std::source_location::current(), "Failed to connect PulseAudio context");
  }

  // Wait for context to be ready and enumerate devices
  bool done = false;
  do {
    if (pa_mainloop_iterate(ml, 1, nullptr) < 0)
      break;

    pa_context_state_t state = pa_context_get_state(ctx);
    if (state == PA_CONTEXT_READY) {
      // Get server info to find default sink
      pa_operation* opServer = pa_context_get_server_info(
          ctx,
          [](pa_context* ctx, const pa_server_info* info, void*) {
            if (!info)
              return;

            if (info->default_sink_name)
              defaultSink = info->default_sink_name;
          },
          nullptr);

      if (opServer) {
        while (pa_operation_get_state(opServer) == PA_OPERATION_RUNNING)
          pa_mainloop_iterate(ml, 1, nullptr);
        pa_operation_unref(opServer);
      }

      // Get source list
      pa_operation* opSources = pa_context_get_source_info_list(ctx, sourceInfoCallback, nullptr);
      if (opSources) {
        while (pa_operation_get_state(opSources) == PA_OPERATION_RUNNING)
          pa_mainloop_iterate(ml, 1, nullptr);
        pa_operation_unref(opSources);
      }
      done = true;
    } else if (state == PA_CONTEXT_FAILED || state == PA_CONTEXT_TERMINATED) {
      done = true;
    }
  } while (!done);

  pa_context_disconnect(ctx);
  pa_context_unref(ctx);
  pa_mainloop_free(ml);

  // Configure audio format
  pa_sample_spec sampleSpec;
  sampleSpec.format = PA_SAMPLE_FLOAT32LE;
  sampleSpec.rate = Config::options.audio.sample_rate;
  sampleSpec.channels = 2;

  // Calculate buffer size based on FPS
  uint32_t samples = 0;
  try {
    samples = static_cast<uint32_t>(sampleSpec.rate / Config::options.window.fps_limit);
  } catch (...) {
    samples = 512;
  }

  const uint32_t bytesPerSample = sampleSpec.channels * sizeof(float);
  const uint32_t bufferSize = samples * bytesPerSample;

  // Configure buffer attributes
  pa_buffer_attr attr = {.maxlength = static_cast<uint32_t>(-1),
                         .tlength = static_cast<uint32_t>(-1),
                         .prebuf = static_cast<uint32_t>(-1),
                         .minreq = static_cast<uint32_t>(-1),
                         .fragsize = bufferSize};

  std::string dev;
  if (Config::options.audio.device == "default") {
    dev = defaultSink + ".monitor";
  } else {
    dev = find(Config::options.audio.device);
  }

  // Create PulseAudio stream
  int err = 0;
  paStream = pa_simple_new(nullptr, "Pulse Visualizer", PA_STREAM_RECORD, dev.c_str(), "Pulse Audio Visualizer",
                           &sampleSpec, nullptr, &attr, &err);

  if (!paStream)
    throw makeErrorAt(std::source_location::current(), "Failed to create PulseAudio stream: {}", pa_strerror(err));

  initialized = true;
  running = true;

  if (dev.empty())
    return;

  logDebug("Connected to PulseAudio device '{}'", dev);
}

bool read(float* buffer, const size_t& samples) {
  if (!initialized || !paStream)
    return true;

  // Read audio data from PulseAudio
  int err;
  if (pa_simple_read(paStream, buffer, samples * 2 * sizeof(float), &err) < 0) {
    logWarnAt(std::source_location::current(), "Failed to read from PulseAudio stream: {}", pa_strerror(err));
    running = false;
    return false;
  }

  return true;
}

bool reconfigure() {
  static std::string lastDevice = "";
  static uint32_t lastSampleRate = 0;
  static size_t lastFPS = 0;

  // Check if reconfiguration is needed
  if (lastDevice == Config::options.audio.device && lastFPS == Config::options.window.fps_limit &&
      lastSampleRate == Config::options.audio.sample_rate) [[likely]]
    return false;

  lastSampleRate = Config::options.audio.sample_rate;
  lastFPS = Config::options.window.fps_limit;
  lastDevice = Config::options.audio.device;

  init();

  return true;
}

std::vector<std::string> enumerate() {
  std::vector<std::string> result;
  result.push_back("default");
  for (const auto& device : availableSources) {
    result.push_back(device.name);
  }
  return result;
}

#else
void init() { throw makeErrorAt(std::source_location::current(), "PulseAudio unavailable"); }
bool read(float*, size_t) { return false; }
bool reconfigure(const std::string&, uint32_t, size_t) { return false; }
std::vector<std::string> enumerate() { return {}; }
#endif
} // namespace Pulseaudio

namespace PipeWire {
#if HAVE_PIPEWIRE

// PipeWire handles and state
struct pw_thread_loop* loop;
struct pw_context* ctx;
struct pw_core* core;
struct pw_stream* stream;
struct spa_hook streamListener;
struct pw_registry* registry;
struct spa_hook registryListener;
BoolSignal readSig {};

bool initialized = false;
bool running = false;

std::vector<DeviceInfo> availableDevices;

void registryEventGlobal(void*, uint32_t id, uint32_t perm, const char* type, uint32_t vers,
                         const struct spa_dict* props) {
  if (strcmp(type, PW_TYPE_INTERFACE_Node) == 0) {
    const char* name = spa_dict_lookup(props, PW_KEY_NODE_NAME);
    const char* desc = spa_dict_lookup(props, PW_KEY_NODE_DESCRIPTION);
    const char* mediaClass = spa_dict_lookup(props, PW_KEY_MEDIA_CLASS);

    // Add audio sources and sinks to device list
    if (mediaClass && (strcmp(mediaClass, "Audio/Source") == 0 || strcmp(mediaClass, "Audio/Sink") == 0)) {
      DeviceInfo dev;
      dev.id = id;
      dev.name = name ? name : "";
      dev.desc = desc ? desc : "";
      dev.mediaClass = mediaClass;
      availableDevices.push_back(dev);
    }
  }
}

void registryEventGlobalRemove(void*, uint32_t id) {}

void onStreamStateChanged(void*, enum pw_stream_state old, enum pw_stream_state state, const char* err) {
  switch (state) {
  case PW_STREAM_STATE_ERROR:
    logWarnAt(std::source_location::current(), "PipeWire stream error: {}", err ? err : "unknown");
  case PW_STREAM_STATE_UNCONNECTED:
    running = false;
    break;
  case PW_STREAM_STATE_STREAMING:
    running = true;
    break;
  case PW_STREAM_STATE_PAUSED:
    pw_stream_set_active(stream, true);
  default:
    break;
  }
}

void onProcess(void*) {
  struct pw_buffer* b;
  struct spa_buffer* buf;

  if ((b = pw_stream_dequeue_buffer(stream)) == nullptr)
    return;

  buf = b->buffer;

  if (buf->datas[0].data == nullptr || buf->datas[0].chunk->size == 0) {
    pw_stream_queue_buffer(stream, b);
    return;
  }

  // Process audio data
  float* samples = static_cast<float*>(buf->datas[0].data);
  size_t n_samples = buf->datas[0].chunk->size / sizeof(float);

  if (n_samples > buf->datas[0].maxsize / sizeof(float))
    n_samples = buf->datas[0].maxsize / sizeof(float);

  float gain = powf(10.0f, Config::options.audio.gain_db / 20.0f);

  // Process samples with SIMD optimization if available
#ifdef HAVE_AVX2
  size_t simd_samples = n_samples & ~7;
  __m256i left_idx = _mm256_setr_epi32(0, 2, 4, 6, 0, 0, 0, 0);
  __m256i right_idx = _mm256_setr_epi32(1, 3, 5, 7, 0, 0, 0, 0);

  for (size_t i = 0; i < simd_samples; i += 8) {
    __m256 data = _mm256_loadu_ps(&samples[i]);

    __m256 left = _mm256_permutevar8x32_ps(data, left_idx);
    __m256 right = _mm256_permutevar8x32_ps(data, right_idx);

    __m256 vGain = _mm256_set1_ps(gain);
    left = _mm256_mul_ps(left, vGain);
    right = _mm256_mul_ps(right, vGain);

    __m256 mid = _mm256_mul_ps(_mm256_add_ps(left, right), _mm256_set1_ps(0.5f));
    __m256 side = _mm256_mul_ps(_mm256_sub_ps(left, right), _mm256_set1_ps(0.5f));

    _mm_storeu_ps(&DSP::bufferMid[DSP::writePos], _mm256_castps256_ps128(mid));
    _mm_storeu_ps(&DSP::bufferSide[DSP::writePos], _mm256_castps256_ps128(side));
    DSP::writePos = (DSP::writePos + 4) % DSP::bufferSize;
  }

  for (size_t i = simd_samples; i + 1 < n_samples; i += 2) {
#else
  for (size_t i = 0; i + 1 < n_samples; i += 2) {
#endif
    float left = samples[i] * gain;
    float right = samples[i + 1] * gain;
    DSP::bufferMid[DSP::writePos] = (left + right) / 2.0f;
    DSP::bufferSide[DSP::writePos] = (left - right) / 2.0f;
    DSP::writePos = (DSP::writePos + 1) % DSP::bufferSize;
  }

  pw_stream_queue_buffer(stream, b);

  readSig.set();
}

std::pair<std::string, uint32_t> find(std::string dev) {
  if (dev.empty()) {
    return std::make_pair("default", PW_ID_ANY);
  }

  // Search for matching device
  for (const auto& device : availableDevices) {
    if (device.name.find(dev) != std::string::npos ||
        device.name.find(dev.substr(0, dev.find(".monitor"))) != std::string::npos) {
      return std::make_pair(device.name, device.id);
    }
  }

  logWarnAt(std::source_location::current(), "PipeWire device '{}' not found. Using system default", dev);
  return std::make_pair("default", PW_ID_ANY);
}

void cleanup() {
  if (loop) {
    pw_thread_loop_lock(loop);

    if (registry) {
      spa_hook_remove(&registryListener);
      pw_proxy_destroy((struct pw_proxy*)registry);
      registry = nullptr;
    }

    if (stream) {
      pw_stream_destroy(stream);
      stream = nullptr;
    }

    pw_thread_loop_unlock(loop);
    pw_thread_loop_stop(loop);
  }

  if (core) {
    pw_core_disconnect(core);
    core = nullptr;
  }

  if (ctx) {
    pw_context_destroy(ctx);
    ctx = nullptr;
  }

  if (loop) {
    pw_thread_loop_destroy(loop);
    loop = nullptr;
  }

  pw_deinit();

  initialized = false;
  running = false;
}

void init() {
  if (initialized)
    cleanup();

  pw_init(nullptr, nullptr);

  // Create PipeWire thread loop
  loop = pw_thread_loop_new("pulse-visualizer", nullptr);
  if (!loop)
    throw makeErrorAt(std::source_location::current(), "Failed to create PipeWire thread");

  // Create PipeWire context
  ctx = pw_context_new(pw_thread_loop_get_loop(loop), nullptr, 0);
  if (!ctx)
    throw makeErrorAt(std::source_location::current(), "Failed to create PipeWire context");

  // Connect to PipeWire core
  core = pw_context_connect(ctx, nullptr, 0);
  if (!core)
    throw makeErrorAt(std::source_location::current(), "Failed to connect to PipeWire");

  if (pw_thread_loop_start(loop) < 0)
    throw makeErrorAt(std::source_location::current(), "Failed to start PipeWire thread");

  // Setup registry to enumerate devices
  availableDevices.clear();
  static const struct pw_registry_events registryEvents = {
      .version = PW_VERSION_REGISTRY_EVENTS, .global = registryEventGlobal, .global_remove = registryEventGlobalRemove};

  pw_thread_loop_lock(loop);
  registry = pw_core_get_registry(core, PW_VERSION_REGISTRY, 0);
  pw_registry_add_listener(registry, &registryListener, &registryEvents, nullptr);
  pw_thread_loop_unlock(loop);

  // Wait for device enumeration
  constexpr auto TIMEOUT = std::chrono::milliseconds(300);
  auto start = std::chrono::steady_clock::now();
  while (availableDevices.empty() && std::chrono::steady_clock::now() - start < TIMEOUT)
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

  // Configure audio format
  const struct spa_pod* params[1];
  struct spa_audio_info_raw info = {};
  uint8_t buffer[1024];
  struct spa_pod_builder b;

  info.format = SPA_AUDIO_FORMAT_F32;
  info.channels = 2;
  info.rate = Config::options.audio.sample_rate;

  spa_pod_builder_init(&b, buffer, sizeof(buffer));
  params[0] = spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat, &info);

  // Setup stream events
  static const struct pw_stream_events streamEvents = {
      .version = PW_VERSION_STREAM_EVENTS, .state_changed = onStreamStateChanged, .process = onProcess};

  pw_thread_loop_lock(loop);

  // Calculate buffer size
  size_t samples = info.rate / Config::options.window.fps_limit;

  std::string nodeLatency = std::to_string(samples) + "/" + std::to_string(info.rate);

  // Create PipeWire stream
  stream = pw_stream_new_simple(pw_thread_loop_get_loop(loop), "Pulse Visualizer",
                                pw_properties_new(PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Capture",
                                                  PW_KEY_MEDIA_ROLE, "Music", PW_KEY_STREAM_CAPTURE_SINK, "true",
                                                  PW_KEY_NODE_LATENCY, nodeLatency.c_str(), nullptr),
                                &streamEvents, nullptr);

  if (!stream)
    throw makeErrorAt(std::source_location::current(), "Failed to create PipeWire stream");

  // Connect stream to device
  uint32_t devId;
  std::string dev;
  if (Config::options.audio.device == "default") {
    devId = PW_ID_ANY;
    dev = "default";
  } else {
    std::tie(dev, devId) = find(Config::options.audio.device);
  }

  if (pw_stream_connect(stream, PW_DIRECTION_INPUT, devId,
                        static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS |
                                                     PW_STREAM_FLAG_DONT_RECONNECT),
                        params, 1) < 0) {
    pw_thread_loop_unlock(loop);
    throw makeErrorAt(std::source_location::current(), "Failed to connect PipeWire stream");
  }

  pw_thread_loop_unlock(loop);

  // Wait for stream to be ready
  start = std::chrono::steady_clock::now();
  while (!running && std::chrono::steady_clock::now() - start < TIMEOUT)
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

  logDebug("Connected to PipeWire device '{}'", dev);

  initialized = true;
}

bool read(float*, const size_t&) {
  // Wait for up to 100ms
  return readSig.wait_for(100ms);
}

bool reconfigure() {
  static std::string lastDevice = "";
  static uint32_t lastSampleRate = 0;
  static size_t lastFPS = 0;

  // Check if reconfiguration is needed
  if (lastDevice == Config::options.audio.device && lastFPS == Config::options.window.fps_limit &&
      lastSampleRate == Config::options.audio.sample_rate) [[likely]]
    return false;

  lastSampleRate = Config::options.audio.sample_rate;
  lastFPS = Config::options.window.fps_limit;
  lastDevice = Config::options.audio.device;

  init();

  return true;
}

std::vector<std::string> enumerate() {
  std::vector<std::string> result;
  result.push_back("default");
  for (const auto& device : availableDevices) {
    result.push_back(device.name);
  }
  return result;
}

#else
void init() { throw makeErrorAt(std::source_location::current(), "PipeWire unavailable"); }
bool read(float*, size_t) { return false; }
bool reconfigure(const std::string&, uint32_t, size_t) { return false; }
std::vector<std::string> enumerate() { return {}; }
#endif
} // namespace PipeWire

namespace WASAPI {
#if HAVE_WASAPI

IMMDevice* currentDevice = NULL;
WAVEFORMATEX* wfx = NULL;
IAudioClient* audioClient = NULL;
IAudioCaptureClient* captureClient = NULL;
HANDLE captureEvent = NULL;
UINT32 bufferFrameCount;

bool initialized = false;
bool running = false;
BoolSignal readSig {};

std::thread wasapiThread;

void threadFunc() {
  HRESULT hr;
  running = true;

  while (running) {
    // Wait for audio data (no busy wait now)
    DWORD waitResult = WaitForSingleObject(captureEvent, 50);

    if (waitResult == WAIT_TIMEOUT) {
      continue;
    }

    UINT32 packetSize = 0;
    hr = captureClient->GetNextPacketSize(&packetSize);
    assert(SUCCEEDED(hr));

    while (packetSize > 0) {
      BYTE* pData;
      UINT32 numFrames;
      DWORD flags;

      hr = captureClient->GetBuffer(&pData, &numFrames, &flags, NULL, NULL);
      if (SUCCEEDED(hr)) {
        // Process audio data
        float* samples = reinterpret_cast<float*>(pData);
        size_t n_samples = numFrames * 2;

        if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
          // If the buffer is silent, skip processing and just write zeros
          for (size_t i = 0; i + 1 < n_samples; i += 2) {
            DSP::bufferMid[DSP::writePos] = 0.f;
            DSP::bufferSide[DSP::writePos] = 0.f;
            DSP::writePos = (DSP::writePos + 1) % DSP::bufferSize;
          }
        } else {
          float gain = powf(10.0f, Config::options.audio.gain_db / 20.0f);

          // Process samples with SIMD optimization if available
#ifdef HAVE_AVX2
          size_t simd_samples = n_samples & ~7;
          __m256i left_idx = _mm256_setr_epi32(0, 2, 4, 6, 0, 0, 0, 0);
          __m256i right_idx = _mm256_setr_epi32(1, 3, 5, 7, 0, 0, 0, 0);

          for (size_t i = 0; i < simd_samples; i += 8) {
            __m256 data = _mm256_loadu_ps(&samples[i]);

            __m256 left = _mm256_permutevar8x32_ps(data, left_idx);
            __m256 right = _mm256_permutevar8x32_ps(data, right_idx);

            __m256 vGain = _mm256_set1_ps(gain);
            left = _mm256_mul_ps(left, vGain);
            right = _mm256_mul_ps(right, vGain);

            __m256 mid = _mm256_mul_ps(_mm256_add_ps(left, right), _mm256_set1_ps(0.5f));
            __m256 side = _mm256_mul_ps(_mm256_sub_ps(left, right), _mm256_set1_ps(0.5f));

            _mm_storeu_ps(&DSP::bufferMid[DSP::writePos], _mm256_castps256_ps128(mid));
            _mm_storeu_ps(&DSP::bufferSide[DSP::writePos], _mm256_castps256_ps128(side));
            DSP::writePos = (DSP::writePos + 4) % DSP::bufferSize;
          }

          for (size_t i = simd_samples; i + 1 < n_samples; i += 2) {
#else
          for (size_t i = 0; i + 1 < n_samples; i += 2) {
#endif
            float left = samples[i] * gain;
            float right = samples[i + 1] * gain;
            DSP::bufferMid[DSP::writePos] = (left + right) / 2.0f;
            DSP::bufferSide[DSP::writePos] = (left - right) / 2.0f;
            DSP::writePos = (DSP::writePos + 1) % DSP::bufferSize;
          }
        }

        captureClient->ReleaseBuffer(numFrames);
      }

      captureClient->GetNextPacketSize(&packetSize);

      readSig.set();
    }
  }
}

// convert wstring to string
std::string WideToUtf8(const std::wstring& wstr) {
  int size_needed = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, nullptr, 0, nullptr, nullptr);
  std::string result(size_needed, 0);
  WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, &result[0], size_needed, nullptr, nullptr);
  return result;
}

void selectDefault(IMMDeviceEnumerator* enumerator, IMMDevice** outDevice) {
  HRESULT hr;
  IMMDevice* device = nullptr;

  hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
  if (SUCCEEDED(hr)) {
    *outDevice = device;
    return;
  }

  throw makeErrorAt(std::source_location::current(), "Failed to get default audio endpoint: {}",
                    _com_error(hr).ErrorMessage());
}

void select(const std::string& targetName, IMMDevice** outDevice) {
  HRESULT hr;
  IMMDeviceEnumerator* enumerator = nullptr;
  IMMDeviceCollection* collection = nullptr;

  hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                        (void**)&enumerator);
  if (FAILED(hr)) {
    throw makeErrorAt(std::source_location::current(), "Failed to create device enumerator: {}",
                      _com_error(hr).ErrorMessage());
  }

  std::string targetNameCorrected = targetName;
  std::transform(targetNameCorrected.begin(), targetNameCorrected.end(), targetNameCorrected.begin(),
                 [](unsigned char c) { return std::tolower(c); });

  if (targetNameCorrected == "default") {
    selectDefault(enumerator, outDevice);
    enumerator->Release();
    return;
  }

  hr = enumerator->EnumAudioEndpoints(eAll, DEVICE_STATE_ACTIVE, &collection);
  if (FAILED(hr)) {
    enumerator->Release();
    throw makeErrorAt(std::source_location::current(), "Failed to enumerate audio endpoints: {}",
                      _com_error(hr).ErrorMessage());
  }

  UINT count;
  collection->GetCount(&count);

  for (UINT i = 0; i < count; ++i) {
    IMMDevice* device = nullptr;
    IPropertyStore* props = nullptr;

    hr = collection->Item(i, &device);
    if (FAILED(hr))
      continue;

    hr = device->OpenPropertyStore(STGM_READ, &props);
    if (FAILED(hr)) {
      device->Release();
      continue;
    }

    PROPVARIANT name;
    PropVariantInit(&name);
    hr = props->GetValue(PKEY_Device_FriendlyName, &name);
    if (SUCCEEDED(hr)) {
      std::wstring wname(name.pwszVal);
      std::string sname = WideToUtf8(wname);

      std::transform(sname.begin(), sname.end(), sname.begin(), [](unsigned char c) { return std::tolower(c); });

      if (sname.find(targetNameCorrected) != std::string::npos) {
        *outDevice = device;
        props->Release();
        PropVariantClear(&name);
        collection->Release();
        enumerator->Release();
        return;
      }
    }

    PropVariantClear(&name);
    props->Release();
    device->Release();
  }

  logWarnAt(std::source_location::current(), "Audio device '{}' not found. Selecting default device", targetName);

  selectDefault(enumerator, outDevice);

  collection->Release();
  enumerator->Release();
}

#define SAFE_RELEASE(punk)                                                                                             \
  if ((punk) != NULL) {                                                                                                \
    (punk)->Release();                                                                                                 \
    (punk) = NULL;                                                                                                     \
  }

void cleanup() {
  initialized = false;
  running = false;

  if (wasapiThread.joinable())
    wasapiThread.join();
  CoTaskMemFree(wfx);
  SAFE_RELEASE(currentDevice);
  SAFE_RELEASE(audioClient);
  SAFE_RELEASE(captureClient);
  CloseHandle(captureEvent);
}

void init() {
  if (initialized)
    cleanup();

  HRESULT hr;

  // Initializes the COM library
  hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
  if (!SUCCEEDED(hr) && hr != S_FALSE && hr != RPC_E_CHANGED_MODE)
    throw makeErrorAt(std::source_location::current(), "Failed to initialize COM: {}", _com_error(hr).ErrorMessage());

  // Try selecting the wanted audio device
  try {
    select(Config::options.audio.device, &currentDevice);
  } catch (const std::exception& e) {
    cleanup();
    throw makeErrorAt(std::source_location::current(), "Failed to select specified audio device: {}. Error: {}",
                      Config::options.audio.device, e.what());
  }

  EDataFlow flow;

  // Show selected device name for clarity
  do {
    IPropertyStore* props = nullptr;
    if (FAILED(currentDevice->OpenPropertyStore(STGM_READ, &props))) {
      continue;
    }

    std::stringstream ss;
    ss << "Selected device: ";

    PROPVARIANT name;
    PropVariantInit(&name);
    if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &name))) {
      std::wstring wname(name.pwszVal);
      std::string sname = WideToUtf8(wname);
      ss << sname;
    } else {
      ss << "N/A";
    }

    PropVariantClear(&name);
    props->Release();

    IMMEndpoint* endpoint = nullptr;
    if (SUCCEEDED(currentDevice->QueryInterface(__uuidof(IMMEndpoint), (void**)&endpoint))) {
      if (SUCCEEDED(endpoint->GetDataFlow(&flow))) {
        switch (flow) {
        case eRender:
          ss << " (render)";
          break;
        case eCapture:
          ss << " (capture)";
          break;
        case eAll:
          ss << " (all)";
          break;
        default:
          ss << " (unknown)";
          break;
        }
      }
      endpoint->Release();
    } else {
      ss << " (unknown)";
    }

    logDebug("{}", ss.str());
  } while (false);

  // Activate the current device
  hr = currentDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, NULL, (void**)&audioClient);
  if (!SUCCEEDED(hr)) {
    cleanup();
    throw makeErrorAt(std::source_location::current(), "Failed to activate current device: {}",
                      _com_error(hr).ErrorMessage());
  }

  // Get mix format
  hr = audioClient->GetMixFormat(&wfx);
  if (!SUCCEEDED(hr)) {
    cleanup();
    throw makeErrorAt(std::source_location::current(), "Failed to get mix format: {}", _com_error(hr).ErrorMessage());
  }

  // Check if the mix format is F32, otherwise we fail because we can't do anything about
  // Solution is to use exclusive mode but I'm not sure how that would work with output devices
  if (wfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
    const WAVEFORMATEXTENSIBLE* wfex = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(wfx);
    if (!(wfex->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT && wfx->wBitsPerSample == 32)) {
      cleanup();
      throw makeErrorAt(std::source_location::current(), "Device format is not Float32.");
    }
  } else if (!(wfx->wFormatTag == WAVE_FORMAT_IEEE_FLOAT && wfx->wBitsPerSample == 32)) {
    cleanup();
    throw makeErrorAt(std::source_location::current(), "Device format is not Float32.");
  }

  // I don't think WASAPI cares about ^2 sample counts
  // we don't have much influence over the buffer size anyways
  size_t targetSamples = wfx->nSamplesPerSec / Config::options.window.fps_limit;
  REFERENCE_TIME hnsBufferDuration = (REFERENCE_TIME)(1e7 * targetSamples / wfx->nSamplesPerSec);

  // Create the audio client
  // AUDCLNT_STREAMFLAGS_LOOPBACK is needed for output devices to work
  DWORD streamFlags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK;
  if (flow == eRender)
    streamFlags |= AUDCLNT_STREAMFLAGS_LOOPBACK;

  hr = audioClient->Initialize(AUDCLNT_SHAREMODE_SHARED, streamFlags, hnsBufferDuration, 0, wfx, NULL);
  if (!SUCCEEDED(hr)) {
    cleanup();
    throw makeErrorAt(std::source_location::current(), "Failed to initialize audio client: {}",
                      _com_error(hr).ErrorMessage());
  }

  // Get the size of the allocated buffer.
  hr = audioClient->GetBufferSize(&bufferFrameCount);
  if (!SUCCEEDED(hr)) {
    cleanup();
    throw makeErrorAt(std::source_location::current(), "Failed to get mix buffer size: {}",
                      _com_error(hr).ErrorMessage());
  }

  // Create the capture service
  hr = audioClient->GetService(__uuidof(IAudioCaptureClient), (void**)&captureClient);
  if (!SUCCEEDED(hr)) {
    cleanup();
    throw makeErrorAt(std::source_location::current(), "Failed to setup capture client: {}",
                      _com_error(hr).ErrorMessage());
  }

  // Create the capture event (doesn't make sense tbh)
  captureEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
  audioClient->SetEventHandle(captureEvent);

  audioClient->Start();
  // start thread
  wasapiThread = std::thread(threadFunc);

  logDebug("Connected to WASAPI");

  initialized = true;
}

bool read(float*, const size_t&) {
  // Wait for up to 100ms
  return readSig.wait_for(100ms);
}

bool reconfigure() {
  static std::string lastDevice = "";
  static uint32_t lastSampleRate = 0;
  static size_t lastFPS = 0;

  // Check if reconfiguration is needed
  if (lastDevice == Config::options.audio.device && lastFPS == Config::options.window.fps_limit &&
      lastSampleRate == Config::options.audio.sample_rate) [[likely]]
    return false;

  lastSampleRate = Config::options.audio.sample_rate;
  lastFPS = Config::options.window.fps_limit;
  lastDevice = Config::options.audio.device;

  init();

  return true;
}

std::vector<std::string> enumerate() {
  HRESULT hr;
  IMMDeviceEnumerator* enumerator = nullptr;
  IMMDeviceCollection* collection = nullptr;

  hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                        (void**)&enumerator);
  if (FAILED(hr)) {
    throw makeErrorAt(std::source_location::current(), "Failed to create device enumerator: {}",
                      _com_error(hr).ErrorMessage());
    // return {};
  }

  hr = enumerator->EnumAudioEndpoints(eAll, DEVICE_STATE_ACTIVE, &collection);
  if (FAILED(hr)) {
    enumerator->Release();
    throw makeErrorAt(std::source_location::current(), "Failed to enumerate audio endpoints: {}",
                      _com_error(hr).ErrorMessage());
    // return {};
  }

  UINT count;
  collection->GetCount(&count);

  std::vector<std::string> output;
  output.push_back("default");

  for (UINT i = 0; i < count; ++i) {
    IMMDevice* device = nullptr;
    IPropertyStore* props = nullptr;

    hr = collection->Item(i, &device);
    if (FAILED(hr))
      continue;

    hr = device->OpenPropertyStore(STGM_READ, &props);
    if (FAILED(hr)) {
      device->Release();
      continue;
    }

    PROPVARIANT name;
    PropVariantInit(&name);
    hr = props->GetValue(PKEY_Device_FriendlyName, &name);
    if (SUCCEEDED(hr)) {
      std::wstring wname(name.pwszVal);
      std::string sname = WideToUtf8(wname);

      output.push_back(sname);
    }

    PropVariantClear(&name);
    props->Release();
    device->Release();
  }

  return output;
}

#else
void init() { throw makeErrorAt(std::source_location::current(), "WASAPI unavailable"); }
bool read(float*, size_t) { return false; }
bool reconfigure(const std::string&, uint32_t, size_t) { return false; }
std::vector<std::string> enumerate() { return {}; }
#endif
} // namespace WASAPI

std::optional<Type> init() {
  Type engine = toType(Config::options.audio.engine);

  // Check for available backends
#if !HAVE_PULSEAUDIO
  if (engine == Type::PULSEAUDIO) {
    logWarnAt(std::source_location::current(), "pulse-visualizer not compiled with PulseAudio enabled, using auto.");
    engine = Type::AUTO;
  }
#endif

#if !HAVE_PIPEWIRE
  if (engine == Type::PIPEWIRE) {
    logWarnAt(std::source_location::current(), "pulse-visualizer not compiled with PipeWire enabled, using auto.");
    engine = Type::AUTO;
  }
#endif

#if !HAVE_WASAPI
  if (engine == Type::WASAPI) {
    logWarnAt(std::source_location::current(), "pulse-visualizer not compiled with WASAPI enabled, using auto.");
    engine = Type::AUTO;
  }
#endif

  // Initialize selected backend
  switch (engine) {
  case Type::AUTO:
#if HAVE_WASAPI
    WASAPI::init();
    return Type::WASAPI;
#endif

    // If both PipeWire and PulseAudio are available
#if HAVE_PIPEWIRE && HAVE_PULSEAUDIO
    try {
      PipeWire::init();
      Pulseaudio::cleanup();
      return Type::PIPEWIRE;
    } catch (const std::exception& e_pw) {
      try {
        Pulseaudio::init();
        PipeWire::cleanup();
        return Type::PULSEAUDIO;
      } catch (const std::exception& e_pa) {
        throw makeErrorAt(std::source_location::current(), "Auto Init failed.\nPipeWire: {}\nPulseAudio: {}",
                          e_pw.what(), e_pa.what());
      }
    }

    // Only PipeWire available
#elif HAVE_PIPEWIRE
    PipeWire::init();
    return Type::PIPEWIRE;

    // Only PulseAudio available
#elif HAVE_PULSEAUDIO
    Pulseaudio::init();
    return Type::PULSEAUDIO;
#endif
    break;
  case Type::WASAPI:
    WASAPI::init();
    return Type::WASAPI;

  case Type::PULSEAUDIO:
    Pulseaudio::init();
#if HAVE_PIPEWIRE
    PipeWire::cleanup();
#endif
    return Type::PULSEAUDIO;
    break;
  case Type::PIPEWIRE:
    PipeWire::init();
#if HAVE_PULSEAUDIO
    Pulseaudio::cleanup();
#endif
    return Type::PIPEWIRE;
    break;
  }

  return std::nullopt;
}

void cleanup() {
#if HAVE_PULSEAUDIO
  AudioEngine::Pulseaudio::cleanup();
#endif
#if HAVE_PIPEWIRE
  AudioEngine::PipeWire::cleanup();
#endif
#if HAVE_WASAPI
  AudioEngine::WASAPI::cleanup();
#endif
}

bool reconfigure() {
#if HAVE_PULSEAUDIO
  if (Pulseaudio::running) {
    return AudioEngine::Pulseaudio::reconfigure();
  }
#endif
#if HAVE_PIPEWIRE
  if (PipeWire::running) {
    return AudioEngine::PipeWire::reconfigure();
  }
#endif
#if HAVE_WASAPI
  if (WASAPI::running) {
    return AudioEngine::WASAPI::reconfigure();
  }
#endif
  return false;
}

bool read(float* buffer, const size_t& samples) {
#if HAVE_PULSEAUDIO
  if (Pulseaudio::running) {
    return AudioEngine::Pulseaudio::read(buffer, samples);
  }
#endif
#if HAVE_PIPEWIRE
  if (PipeWire::running) {
    return AudioEngine::PipeWire::read(buffer, samples);
  }
#endif
#if HAVE_WASAPI
  if (WASAPI::running) {
    return AudioEngine::WASAPI::read(buffer, samples);
  }
#endif
  return false;
}

std::vector<std::string> enumerate() {
#if HAVE_PULSEAUDIO
  if (Pulseaudio::running) {
    return AudioEngine::Pulseaudio::enumerate();
  }
#endif
#if HAVE_PIPEWIRE
  if (PipeWire::running) {
    return AudioEngine::PipeWire::enumerate();
  }
#endif
#if HAVE_WASAPI
  if (WASAPI::running) {
    return AudioEngine::WASAPI::enumerate();
  }
#endif
  return {};
}

} // namespace AudioEngine