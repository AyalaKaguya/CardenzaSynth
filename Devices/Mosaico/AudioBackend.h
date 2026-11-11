#pragma once

#include <cstdint>

namespace mosaico::audio {

// Call after Device::Init has completed the BSP's shared power/display setup;
// those lazy BSP initializers are not safe to race on two tasks. One output task
// then owns the cached ES8311 speaker and I2S transport. Start is idempotent;
// readiness is separate because audio initialization happens on that task.
bool Start();
bool Ready();
// This affects only device output, after the synth render. It never stops the clock
// or prevents pending synth events from being consumed.
void SetMuted(bool muted);

struct Diagnostics {
  bool ready = false;
  std::uint32_t writtenBlocks = 0;
  std::uint32_t nonSilentBlocks = 0;  // synth output before device volume/mute
  std::uint32_t writeFailures = 0;
  std::uint32_t peak = 0;             // largest absolute pre-volume PCM value, 0..32768
  std::uint32_t queueFrames = 0;      // BSP/IDF default DMA capacity
};

// Together with Device::Audio::Timing(), lets an explicitly enabled note test
// distinguish real synth output, a running task, and successful codec writes.
Diagnostics GetDiagnostics();

}  // namespace mosaico::audio
