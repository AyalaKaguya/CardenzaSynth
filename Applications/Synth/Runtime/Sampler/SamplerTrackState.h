#pragma once

#include <array>
#include <cstdint>
#include <cstddef>
#include <string>

namespace synth {

inline constexpr std::size_t kSamplerWaveformBinCount = 32;

enum class SamplerTrackPhase {
  Empty,
  Ready,
  Recording,
  DraftTrim,
  Naming,
  Browser,
  Error,
};

struct SamplerTrackUiState {
  SamplerTrackPhase phase = SamplerTrackPhase::Empty;
  std::string sampleName = "EMPTY";
  std::string sampleFileName;
  std::string statusText = "EMPTY";
  std::string draftName;
  std::uint32_t sampleRate = 44100;
  std::uint32_t frameCount = 0;
  std::uint32_t recordMaxFrameCount = 0;
  std::uint32_t trimStartFrame = 0;
  std::uint32_t trimEndFrame = 0;
  std::uint32_t previewCursorFrame = 0;
  std::array<float, kSamplerWaveformBinCount> waveform{};
  float inputLevel = 0.0f;
  float recordProgress = 0.0f;
  bool hasSample = false;
  bool hasDraft = false;
  bool recording = false;
  bool needsName = false;
  bool overwritePending = false;
  bool deleteConfirm = false;
  bool previewing = false;
#if defined(CARDENZA_NATIVE_UI)
  bool saving = false;
  float savingProgress = 0;
  bool waveformPending = false;
#endif
};

}  // namespace synth
