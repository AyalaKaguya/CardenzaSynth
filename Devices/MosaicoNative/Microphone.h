#pragma once

#include <cstddef>
#include <cstdint>
#include "Devices/Device.h"

namespace mosaico::microphone {

// UI/system owner calls Start/Stop/Read. Start and Stop wait at most 1500 ms;
// the output task alone performs codec control via ServiceControl. A stopped
// worker cannot retain a borrowed buffer that cleanup has freed.
Device::Audio::InputStatus Start();
void Stop();
Device::Audio::InputStatus Read(std::int16_t* destination, std::size_t frameCount,
                                std::size_t& framesRead, std::uint32_t timeoutMs);
// Call on the BSP output owner once each audio block, outside any render lock.
void ServiceControl(bool outputReady);
// A separate recording mute preserves the user's master mute and volume.
bool OutputMuted();

struct Diagnostics {
  bool recording = false;
  std::uint32_t capturedFrames = 0;
  std::uint32_t overflows = 0;
  std::uint32_t readFailures = 0;
  std::uint32_t peak = 0;
};
Diagnostics GetDiagnostics();

}  // namespace mosaico::microphone
