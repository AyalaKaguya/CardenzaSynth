#pragma once

#include <cstdint>

namespace mosaico::diagnostics {

inline constexpr std::uint16_t kServiceId = 0x4343;
inline constexpr std::uint16_t kStatusMethod = 1;
inline constexpr std::uint16_t kAudioTestMethod = 2;

// Register bounded public Iris RPCs during startup. With a startup test already
// scheduled, mark it busy here and call CompleteAudioTest once it finishes.
bool Init(bool startupAudioTestRunning = false, bool audioReplayEnabled = true);

// Called only by the system/UI owner. Counts are cumulative since that owner
// started; lateFrameCount counts missed frame deadlines, not a current streak.
// RPC readers see approximately simultaneous atomic counters, never UI objects.
void Publish(std::uint32_t frameCount, std::uint32_t elapsedMs,
             std::uint32_t lateFrameCount, std::uint32_t longestFrameMicros = 0);

// The RPC executor only queues a request. The UI owner consumes it, runs the
// existing short sequencer test, then completes it; no business calls cross tasks.
bool ConsumeAudioTestRequest();
void CompleteAudioTest(bool passed);
#if defined(CARDENZA_NATIVE_UI)
void PublishNativeUi(std::uint8_t scene, std::uint8_t page, std::uint8_t track,
                     std::uint8_t modal, bool dirty, bool playing, std::uint32_t pulse);
#endif

}  // namespace mosaico::diagnostics
