#include "audio_processing.hpp"
#include "config.hpp"
#include "graphics.hpp"
#include "theme.hpp"
#include "visualizers.hpp"

#include <GL/glew.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <string>
#include <vector>

// Computed caches (no theme colors needed - using centralized access)

// Static working buffers and phosphor context
static Graphics::Phosphor::PhosphorContext* fftPhosphorContext = nullptr;

// Computed values from config (cached for performance)
float FFTVisualizer::logMinFreq = 0.0f;
float FFTVisualizer::logMaxFreq = 0.0f;
float FFTVisualizer::logFreqRange = 0.0f;
float FFTVisualizer::dbRange = 0.0f;
const char** FFTVisualizer::noteNames = nullptr;
size_t FFTVisualizer::lastConfigVersion = 0;

// Static member definitions for FFT points
std::vector<std::pair<float, float>> FFTVisualizer::fftPoints;
std::vector<std::pair<float, float>> FFTVisualizer::alternateFFTPoints;

void FFTVisualizer::updateCaches() {
  if (Config::getVersion() != lastConfigVersion) {
    const auto& fcfg = Config::values().fft;

    // Set note names based on key mode
    static const char* noteNamesSharp[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    static const char* noteNamesFlat[] = {"C", "Db", "D", "Eb", "E", "F", "Gb", "G", "Ab", "A", "Bb", "B"};
    noteNames = (fcfg.note_key_mode == "sharp") ? noteNamesSharp : noteNamesFlat;

    lastConfigVersion = Config::getVersion();

    // Pre-compute logarithmic values
    logMinFreq = log(fcfg.min_freq);
    logMaxFreq = log(fcfg.max_freq);
    logFreqRange = logMaxFreq - logMinFreq;
    dbRange = fcfg.max_db - fcfg.min_db;
  }
}

void FFTVisualizer::draw(const AudioData& audioData, int) {
  // Set up the viewport for the FFT visualizer
  Graphics::setupViewport(position, 0, width, audioData.windowHeight, audioData.windowHeight);

  // Update cached values if needed
  updateCaches();

  const auto& fcfg = Config::values().fft;
  const auto& phos = Config::values().phosphor;
  const auto& colors = Theme::ThemeManager::colors();

  // Initialize phosphor context if needed
  if (fcfg.enable_phosphor && !fftPhosphorContext) {
    fftPhosphorContext = Graphics::Phosphor::createPhosphorContext("fft");
  } else if (!fcfg.enable_phosphor && fftPhosphorContext) {
    Graphics::Phosphor::destroyPhosphorContext(fftPhosphorContext);
    fftPhosphorContext = nullptr;
  }

  // Draw frequency scale lines and labels first (in the back) for non-phosphor mode
  if (!fcfg.enable_phosphor) {
    auto drawFreqLine = [&](float freq) {
      if (freq < fcfg.min_freq || freq > fcfg.max_freq)
        return;
      float logX = (log(freq) - logMinFreq) / logFreqRange;
      float x = logX * width;
      Graphics::drawAntialiasedLine(x, -audioData.windowHeight, x, audioData.windowHeight * 2, colors.grid, 1.0f);
    };

    // Draw log-decade lines: 1,2,3,...9,10 per decade, from 100Hz up to maxFreq
    // Add 10-100Hz decade
    for (int mult = 1; mult < 10; ++mult) {
      float freq = 10.0f * mult;
      drawFreqLine(freq);
    }
    drawFreqLine(100.0f);
    for (int decade = 2; decade <= 5; ++decade) {
      float base = powf(10.0f, decade);
      for (int mult = 1; mult < 10; ++mult) {
        float freq = base * mult;
        drawFreqLine(freq);
      }
    }
    for (int freq = 10000; freq <= static_cast<int>(fcfg.max_freq); freq *= 10) {
      drawFreqLine(freq);
    }

    // Draw frequency labels for 100Hz, 1kHz, 10kHz
    auto drawFreqLabel = [&](float freq, const char* label) {
      float logX = (log(freq) - logMinFreq) / logFreqRange;
      float x = logX * width;
      float y = 8.0f;

      // Draw background rectangle
      float textWidth = 40.0f;
      float textHeight = 12.0f;
      float padding = 4.0f;

      Graphics::drawFilledRect(x - textWidth / 2 - padding, y - textHeight / 2 - padding, textWidth + padding * 2,
                               textHeight + padding * 2, colors.background);

      // Draw the text
      Graphics::drawText(label, x - textWidth / 2, y, 10.0f, colors.grid, Config::values().font.c_str());
    };
    drawFreqLabel(100.0f, "100 Hz");
    drawFreqLabel(1000.0f, "1 kHz");
    drawFreqLabel(10000.0f, "10 kHz");
  }

  // Check if we should use CQT or FFT data
  bool useCQT = fcfg.enable_cqt && !audioData.cqtMagnitudesMid.empty() && !audioData.cqtMagnitudesSide.empty();
  bool useFFT = !audioData.fftMagnitudesMid.empty() && !audioData.fftMagnitudesSide.empty();

  if (useCQT || useFFT) {
    // Determine which data to use based on mode
    const std::vector<float>* mainMagnitudes = nullptr;
    const std::vector<float>* alternateMagnitudes = nullptr;
    const std::vector<float>* frequencies = nullptr;

    // FFT size for frequency calculations (only used in FFT mode)
    int fftSize = 0;
    if (useFFT) {
      fftSize = (audioData.fftMagnitudesMid.size() - 1) * 2;
    }

    // Temporary vectors for computed LEFTRIGHT mode data
    std::vector<float> leftMagnitudes, rightMagnitudes;

    // Force mono mode when phosphor is enabled
    bool isLeftRightMode = (fcfg.stereo_mode == "leftright") && !fcfg.enable_phosphor;
    bool showAlternate = !fcfg.enable_phosphor; // Hide alternate channel in phosphor mode

    if (useCQT) {
      // Use CQT data
      frequencies = &audioData.cqtFrequencies;

      if (isLeftRightMode) {
        // LEFTRIGHT mode: Main = Mid - Side (Left), Alternate = Mid + Side (Right)
        leftMagnitudes.resize(audioData.smoothedCqtMagnitudesMid.size());
        rightMagnitudes.resize(audioData.smoothedCqtMagnitudesMid.size());

        for (size_t i = 0; i < audioData.smoothedCqtMagnitudesMid.size(); ++i) {
          leftMagnitudes[i] = audioData.smoothedCqtMagnitudesMid[i] - audioData.smoothedCqtMagnitudesSide[i];
          rightMagnitudes[i] = audioData.smoothedCqtMagnitudesMid[i] + audioData.smoothedCqtMagnitudesSide[i];
        }

        mainMagnitudes = &leftMagnitudes;
        alternateMagnitudes = &rightMagnitudes;
      } else {
        // MIDSIDE mode: Main = Mid, Alternate = Side (or mono when phosphor enabled)
        mainMagnitudes = &audioData.smoothedCqtMagnitudesMid;
        alternateMagnitudes = &audioData.smoothedCqtMagnitudesSide;
      }
    } else {
      // Use FFT data (original logic)
      if (isLeftRightMode) {
        // LEFTRIGHT mode: Main = Mid - Side (Left), Alternate = Mid + Side (Right)
        leftMagnitudes.resize(audioData.smoothedMagnitudesMid.size());
        rightMagnitudes.resize(audioData.smoothedMagnitudesMid.size());

        for (size_t i = 0; i < audioData.smoothedMagnitudesMid.size(); ++i) {
          leftMagnitudes[i] = audioData.smoothedMagnitudesMid[i] - audioData.smoothedMagnitudesSide[i];
          rightMagnitudes[i] = audioData.smoothedMagnitudesMid[i] + audioData.smoothedMagnitudesSide[i];
        }

        mainMagnitudes = &leftMagnitudes;
        alternateMagnitudes = &rightMagnitudes;
      } else {
        // MIDSIDE mode: Main = Mid, Alternate = Side (or mono when phosphor enabled)
        mainMagnitudes = &audioData.smoothedMagnitudesMid;
        alternateMagnitudes = &audioData.smoothedMagnitudesSide;
      }
    }

    // Pre-compute constants for magnitude processing
    const float freqScale = useCQT ? 1.0f : (audioData.sampleRate / fftSize);
    const float invLogFreqRange = 1.0f / logFreqRange;
    const float invDbRange = 1.0f / dbRange;
    const float gainBase = 1.0f / (440.0f * 2.0f); // Pre-compute for slope correction

    // Clear and prepare static class member vectors
    alternateFFTPoints.clear();
    fftPoints.clear();

    if (showAlternate) {
      alternateFFTPoints.reserve(alternateMagnitudes->size());
    }
    fftPoints.reserve(mainMagnitudes->size());

    // Generate alternate FFT points only when showing alternate channel
    if (showAlternate) {
      size_t startBin = useCQT ? 0 : 1; // CQT starts from bin 0, FFT from bin 1
      for (size_t bin = startBin; bin < alternateMagnitudes->size(); ++bin) {
        float freq;
        if (useCQT) {
          freq = (*frequencies)[bin];
        } else {
          freq = static_cast<float>(bin) * freqScale;
        }

        if (freq < fcfg.min_freq || freq > fcfg.max_freq)
          continue;
        float logX = (log(freq) - logMinFreq) * invLogFreqRange;
        float x = logX * width;
        float magnitude = (*alternateMagnitudes)[bin];

        // Apply slope correction with pre-computed values
        float slope_k = fcfg.slope_correction_db / 20.0f / log10f(2.0f);
        float gain = powf(freq * gainBase, slope_k);
        magnitude *= gain;

        // Convert to dB using proper scaling
        float dB = 20.0f * log10f(magnitude + 1e-9f);

        // Map dB to screen coordinates using cached display limits
        float y = (dB - fcfg.min_db) * invDbRange * audioData.windowHeight;
        alternateFFTPoints.push_back({x, y});
      }
    }

    // Draw Main spectrum using pre-computed smoothed values
    size_t startBin = useCQT ? 0 : 1; // CQT starts from bin 0, FFT from bin 1
    for (size_t bin = startBin; bin < mainMagnitudes->size(); ++bin) {
      float freq;
      if (useCQT) {
        freq = (*frequencies)[bin];
      } else {
        freq = static_cast<float>(bin) * freqScale;
      }

      if (freq < fcfg.min_freq || freq > fcfg.max_freq)
        continue;
      float logX = (log(freq) - logMinFreq) * invLogFreqRange;
      float x = logX * width;
      float magnitude = (*mainMagnitudes)[bin];

      // Apply slope correction with pre-computed values
      float slope_k = fcfg.slope_correction_db / 20.0f / log10f(2.0f);
      float gain = powf(freq * gainBase, slope_k);
      magnitude *= gain;

      // Convert to dB using proper scaling
      float dB = 20.0f * log10f(magnitude + 1e-9f);

      // Map dB to screen coordinates using cached display limits
      float y = (dB - fcfg.min_db) * invDbRange * audioData.windowHeight;
      fftPoints.push_back({x, y});
    }

    if (fcfg.enable_phosphor && fftPhosphorContext && !fftPoints.empty()) {
      // --- Phosphor rendering using Graphics::Phosphor high-level interface ---

      // Prepare intensity and dwell time data for FFT spectrum
      std::vector<float> intensityLinear;
      std::vector<float> dwellTimes;

      intensityLinear.reserve(fftPoints.size());
      dwellTimes.reserve(fftPoints.size());

      // Normalize beam energy over area
      float ref = 400.0f * (useCQT ? 300.0f : 1.0f);
      float beamEnergy = phos.beam_energy / ref * (width * (useCQT ? audioData.windowHeight : 1.0f));

      // Apply beam multiplier
      beamEnergy *= fcfg.beam_multiplier;

      // Normalize beam energy over fps
      beamEnergy = beamEnergy * audioData.getAudioDeltaTime() / 0.016f;

      for (size_t i = 1; i < fftPoints.size(); ++i) {
        const auto& p1 = fftPoints[i - 1];
        const auto& p2 = fftPoints[i];

        float dx = p2.first - p1.first;
        float dy = p2.second - p1.second;
        float segLen = std::max(sqrtf(dx * dx + dy * dy), 1e-12f);
        float deltaT = 1.0f / audioData.sampleRate;

        float intensity;
        if (useCQT) {
          intensity = beamEnergy * (deltaT / segLen) * 2.0f;
        } else {
          intensity = beamEnergy * (deltaT * sqrtf(dx)) / 2.0f;
        }

        intensityLinear.push_back(intensity);
        dwellTimes.push_back(deltaT);
      }

      // Render phosphor splines using high-level interface
      GLuint phosphorTexture = Graphics::Phosphor::renderPhosphorSplines(
          fftPhosphorContext, fftPoints, intensityLinear, dwellTimes, width, audioData.windowHeight,
          audioData.getAudioDeltaTime(), 1.0f, colors.background, colors.spectrum);

      // Draw the phosphor result
      if (phosphorTexture) {
        Graphics::Phosphor::drawPhosphorResult(phosphorTexture, width, audioData.windowHeight);
      }

      // Draw the Alternate FFT curve with reduced opacity for phosphor mode
      if (!alternateFFTPoints.empty()) {
        constexpr float alternateFFTAlpha = 0.15f; // Reduced for phosphor mode
        float alternateFFTColorArray[4] = {
            colors.spectrum[0] * alternateFFTAlpha + colors.background[0] * (1.0f - alternateFFTAlpha),
            colors.spectrum[1] * alternateFFTAlpha + colors.background[1] * (1.0f - alternateFFTAlpha),
            colors.spectrum[2] * alternateFFTAlpha + colors.background[2] * (1.0f - alternateFFTAlpha),
            colors.spectrum[3] * alternateFFTAlpha + colors.background[3] * (1.0f - alternateFFTAlpha)};
        Graphics::drawAntialiasedLines(alternateFFTPoints, alternateFFTColorArray, 1.0f);
      }
    } else {
      // Standard non-phosphor rendering
      // Draw the Alternate FFT curve using centralized colors
      constexpr float alternateFFTAlpha = 0.3f;
      float alternateFFTColorArray[4] = {
          colors.spectrum[0] * alternateFFTAlpha + colors.background[0] * (1.0f - alternateFFTAlpha),
          colors.spectrum[1] * alternateFFTAlpha + colors.background[1] * (1.0f - alternateFFTAlpha),
          colors.spectrum[2] * alternateFFTAlpha + colors.background[2] * (1.0f - alternateFFTAlpha),
          colors.spectrum[3] * alternateFFTAlpha + colors.background[3] * (1.0f - alternateFFTAlpha)};
      Graphics::drawAntialiasedLines(alternateFFTPoints, alternateFFTColorArray, 2.0f);

      // Draw the Main FFT curve using centralized colors
      Graphics::drawAntialiasedLines(fftPoints, colors.spectrum, 2.0f);
    }
  }

  // Draw crosshair and show mouse position info when hovering
  const auto& audio = Config::values().audio;
  bool isSilent = !audioData.hasValidPeak || audioData.peakDb < audio.silence_threshold;

  if (!isSilent && hovering) {
    // Convert absolute mouse coordinates to relative coordinates within the visualizer
    float relativeMouseX = mouseX - position;
    float relativeMouseY = mouseY;

    // Only draw cursor lines if mouse is within the visualizer bounds
    if (relativeMouseX >= 0 && relativeMouseX <= width && relativeMouseY >= 0 &&
        relativeMouseY <= audioData.windowHeight) {
      // Draw crosshair lines relative to visualizer position
      Graphics::drawAntialiasedLine(relativeMouseX, 0, relativeMouseX, audioData.windowHeight, colors.spectrum, 2.0f);
      Graphics::drawAntialiasedLine(0, relativeMouseY, width, relativeMouseY, colors.spectrum, 2.0f);
    }

    // Calculate frequency and dB from mouse position (use relative X coordinate)
    float frequency, actualDB;
    calculateFrequencyAndDB(relativeMouseX, mouseY, audioData.windowHeight, frequency, actualDB);

    // Convert frequency to note
    std::string noteName;
    int octave, cents;
    freqToNote(frequency, noteName, octave, cents);

    char overlay[128];
    snprintf(overlay, sizeof(overlay), "%6.2f dB  |  %7.2f Hz  |  %s%d %+d Cents", actualDB, frequency,
             noteName.c_str(), octave, cents);
    float overlayX = 10.0f;
    float overlayY = audioData.windowHeight - 20.0f;

    // Draw overlay text using centralized colors
    Graphics::drawText(overlay, overlayX, overlayY, 14.0f, colors.text, Config::values().font.c_str());
  } else if (!isSilent) {
    // Show pitch detector data when not hovering
    char overlay[128];
    snprintf(overlay, sizeof(overlay), "%6.2f dB  |  %7.2f Hz  |  %s%d %+d Cents", audioData.peakDb, audioData.peakFreq,
             audioData.peakNote.c_str(), audioData.peakOctave, audioData.peakCents);
    float overlayX = 10.0f;
    float overlayY = audioData.windowHeight - 20.0f;

    // Draw overlay text using centralized colors
    Graphics::drawText(overlay, overlayX, overlayY, 14.0f, colors.text, Config::values().font.c_str());
  }
}

int FFTVisualizer::getPosition() const { return position; }
void FFTVisualizer::setPosition(int pos) { position = pos; }
int FFTVisualizer::getWidth() const { return width; }
void FFTVisualizer::setWidth(int w) { width = w; }
int FFTVisualizer::getRightEdge(const AudioData&) const { return position + width; }

// Mouse tracking methods
void FFTVisualizer::updateMousePosition(float mouseX, float mouseY) {
  this->mouseX = mouseX;
  this->mouseY = mouseY;
}

void FFTVisualizer::setHovering(bool hovering) { this->hovering = hovering; }

bool FFTVisualizer::isHovering() const { return hovering; }

void FFTVisualizer::calculateFrequencyAndDB(float x, float y, float windowHeight, float& frequency,
                                            float& actualDB) const {
  const auto& fcfg = Config::values().fft;

  // Convert X coordinate to frequency
  float logX = x / width;
  frequency = exp(logX * logFreqRange + logMinFreq);

  // Convert Y coordinate to displayed dB (slope-corrected)
  float displayed_dB = (y / windowHeight) * dbRange + fcfg.min_db;

  // Remove slope correction to get actual dB
  const float gainBase = 1.0f / (440.0f * 2.0f);
  float slope_k = fcfg.slope_correction_db / 20.0f / log10f(2.0f);
  float slope_correction_dB = 20.0f * slope_k * log10f(frequency * gainBase);
  actualDB = displayed_dB - slope_correction_dB;
}

void FFTVisualizer::freqToNote(float freq, std::string& noteName, int& octave, int& cents) const {
  if (freq <= 0.0f || !noteNames) {
    noteName = "-";
    octave = 0;
    cents = 0;
    return;
  }

  // Calculate MIDI note number (A4 = 440Hz = MIDI 69)
  float midi = 69.0f + 12.0f * log2f(freq / 440.0f);

  // Handle out of range values
  if (midi < 0.0f || midi > 127.0f) {
    noteName = "-";
    octave = 0;
    cents = 0;
    return;
  }

  int midiInt = (int)roundf(midi);
  int noteIdx = (midiInt + 1200) % 12; // +1200 for negative safety
  octave = midiInt / 12 - 1;
  noteName = noteNames[noteIdx];
  cents = (int)roundf((midi - midiInt) * 100.0f);
}

void FFTVisualizer::invalidatePhosphorContext() {
  if (fftPhosphorContext) {
    // Force the phosphor context to recreate its textures by destroying and recreating it
    Graphics::Phosphor::destroyPhosphorContext(fftPhosphorContext);
    fftPhosphorContext = nullptr;
  }
}