#pragma once

#include <cstdint>

namespace mosaico::audio {

// Call after Device::Init has completed the BSP's shared power/display setup;
// those lazy BSP initializers are not safe to race on two tasks. One output task
// then owns the cached ES8311 speaker and I2S transport. Start is idempotent;
// readiness is separate because audio initialization happens on that task.
bool Start();
bool Ready();
// Hold device output while the application restores its settings/session. This
// startup gate is separate from the user's volume and the recording gate.
void SetStartupMuted(bool muted);

struct Diagnostics {
  bool ready = false;
  std::uint32_t writtenBlocks = 0;
  std::uint32_t nonSilentBlocks = 0;  // synth output before device volume/mute
  std::uint32_t writeFailures = 0;
  std::uint32_t peak = 0;             // largest absolute pre-volume PCM value, 0..32768
  std::uint32_t queueFrames = 0;      // BSP/IDF default DMA capacity
  bool startupMuted = false;
  bool recordingMuted = false;
  std::uint32_t outputGainQ15 = 0;    // gain at the end of the last successful codec write; 32768 = unity
  std::uint32_t outputNonSilentBlocks = 0;  // successful writes with nonzero post-volume PCM
  std::uint32_t outputPeak = 0;       // post-volume peak of the last successful write; zero if unavailable
};

// Distinguishes rendered synth PCM from the PCM actually passed to successful
// codec writes. These counters do not measure the speaker's acoustic output.
Diagnostics GetDiagnostics();

}  // namespace mosaico::audio
