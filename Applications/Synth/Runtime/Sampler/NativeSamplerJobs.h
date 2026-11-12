#pragma once

#if defined(CARDENZA_NATIVE_UI)

#include <cstddef>
#include <array>
#include <cstdint>
#include <memory>
#include <string>

#include "Library/Synth/Audio/SynthAudioRuntime.h"
#include "Applications/Synth/Runtime/Sampler/SamplerTrackState.h"

namespace synth::native {

struct SampleSaveJob;
struct SampleSaveResult {
  bool done = false;
  bool saved = false;
  float progress = 0;
  audio::SharedSampleBuffer frames;
};

// The worker owns its source/path snapshot, never a SamplerManager or UI pointer.
// A job still in progress remains owned if the application is suspended/destroyed.
std::shared_ptr<SampleSaveJob> SaveSampleInBackground(std::string path,
    audio::SharedSampleBuffer source, std::size_t firstFrame, std::size_t frameCount,
    std::uint32_t sampleRate, bool overwrite);
SampleSaveResult PollSampleSave(const std::shared_ptr<SampleSaveJob>& job);

struct SampleWaveJob;
struct SampleWaveResult {
  bool done = false;
  bool ready = false;
  std::array<float, kSamplerWaveformBinCount> waveform{};
};
std::shared_ptr<SampleWaveJob> BuildSampleWaveInBackground(audio::SharedSampleBuffer source);
SampleWaveResult PollSampleWave(const std::shared_ptr<SampleWaveJob>& job);

}  // namespace synth::native

#endif
