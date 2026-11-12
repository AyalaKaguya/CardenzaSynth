#include "Devices/Mosaico/Diagnostics.h"

#include <atomic>
#include <cinttypes>
#include <cstdio>

#include "Devices/Device.h"
#include "Devices/Mosaico/AudioBackend.h"
#include "Devices/Mosaico/DevicePlatform.h"
#include "Library/Memory/Memory.h"
#include "esp_iris.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "sdkconfig.h"
#if defined(CARDENZA_NATIVE_UI)
#include "Devices/MosaicoNative/Microphone.h"
#include "Devices/MosaicoNative/StorageBackend.h"
#include "Devices/MosaicoNative/TouchBackend.h"
#endif

namespace {

constexpr char kTag[] = "CardenzaDiag";
enum class AudioTestState : std::uint32_t { Idle, Queued, Running, Passed, Failed };
std::atomic<AudioTestState> g_testState{AudioTestState::Idle};
std::atomic<std::uint32_t> g_testCount{0};
std::atomic<std::uint32_t> g_frameCount{0};
std::atomic<std::uint32_t> g_elapsedMs{0};
std::atomic<std::uint32_t> g_lateFrames{0};
std::atomic<std::uint32_t> g_longestFrameMicros{0};
bool g_registered = false;  // Init is called by the startup owner only.
bool g_audioReplayEnabled = true;
#if defined(CARDENZA_NATIVE_UI)
std::atomic<std::uint32_t> g_nativeUi{0};
std::atomic<std::uint32_t> g_nativePulse{0};
#endif

const char* TestStateName(AudioTestState state)
{
  switch (state)
  {
    case AudioTestState::Queued: return "queued";
    case AudioTestState::Running: return "running";
    case AudioTestState::Passed: return "passed";
    case AudioTestState::Failed: return "failed";
    default: return "idle";
  }
}

esp_err_t StatusRpc(const esp_iris_rpc_request_t* request, std::uint8_t* response,
                    std::size_t capacity, std::size_t* responseSize, void*)
{
  *responseSize = 0;
  if (request->payload_size != 0) return ESP_ERR_INVALID_SIZE;
  const auto audio = mosaico::audio::GetDiagnostics();
  const auto timing = Device::Audio::Timing();
#if CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
  constexpr const char* kRuntimeStats = "true";
#else
  constexpr const char* kRuntimeStats = "false";
#endif
  const int written = std::snprintf(reinterpret_cast<char*>(response), capacity,
      "{\"schema\":1,\"uptime_ms\":%" PRIu64 ",\"ui_ms\":%" PRIu32
      ",\"frames\":%" PRIu32 ",\"late_frames\":%" PRIu32 ",\"frame_max_us\":%" PRIu32
      ",\"display_healthy\":%s,\"audio_ready\":%s,\"volume\":%u,\"cpu_mhz\":%u,\"runtime_stats\":%s"
      ",\"heap_free\":%u,\"heap_min\":%u,\"heap_largest\":%u,\"psram_free\":%u,\"psram_min\":%u"
      ",\"audio_blocks\":%" PRIu32 ",\"written\":%" PRIu32 ",\"nonzero\":%" PRIu32 ",\"peak\":%" PRIu32
      ",\"queue_frames\":%" PRIu32 ",\"render_us\":%" PRIu32 ",\"render_max_us\":%" PRIu32
      ",\"overruns\":%" PRIu32 ",\"estimated_underruns\":%" PRIu32 ",\"write_failures\":%" PRIu32
      ",\"startup_muted\":%s,\"recording_muted\":%s,\"output_gain_q15\":%" PRIu32
      ",\"output_nonzero\":%" PRIu32 ",\"output_peak\":%" PRIu32
      ",\"audio_test\":\"%s\",\"tests_completed\":%" PRIu32 "}",
      static_cast<std::uint64_t>(esp_timer_get_time() / 1000), g_elapsedMs.load(std::memory_order_relaxed),
      g_frameCount.load(std::memory_order_relaxed), g_lateFrames.load(std::memory_order_relaxed),
      g_longestFrameMicros.load(std::memory_order_relaxed), mosaico::display::Healthy() ? "true" : "false",
      audio.ready ? "true" : "false", static_cast<unsigned>(Device::Audio::OutputVolume()),
      static_cast<unsigned>(CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ), kRuntimeStats,
      static_cast<unsigned>(memory::FastFree()), static_cast<unsigned>(memory::FastMinimum()),
      static_cast<unsigned>(memory::FastLargest()), static_cast<unsigned>(memory::SlowFree()),
      static_cast<unsigned>(memory::SlowMinimum()), timing.blocks, audio.writtenBlocks,
      audio.nonSilentBlocks, audio.peak, audio.queueFrames, timing.renderMicros,
      timing.longestRenderMicros, timing.overruns, timing.underruns, audio.writeFailures,
      audio.startupMuted ? "true" : "false", audio.recordingMuted ? "true" : "false",
      audio.outputGainQ15, audio.outputNonSilentBlocks, audio.outputPeak,
      TestStateName(g_testState.load(std::memory_order_acquire)), g_testCount.load(std::memory_order_relaxed));
  if (written < 0 || static_cast<std::size_t>(written) >= capacity) return ESP_ERR_INVALID_SIZE;
  *responseSize = static_cast<std::size_t>(written);
#if defined(CARDENZA_NATIVE_UI)
  const auto mic = mosaico::microphone::GetDiagnostics();
  const auto ui = g_nativeUi.load(std::memory_order_relaxed);
  const int extra = std::snprintf(reinterpret_cast<char*>(response) + written - 1, capacity - written + 1,
      ",\"scene\":%u,\"page\":%u,\"track\":%u,\"modal\":%u,\"dirty\":%s"
      ",\"playing\":%s,\"pulse\":%" PRIu32
      ",\"touch_ready\":%s,\"storage_ready\":%s,\"storage_error\":%d,\"volume_bytes\":%" PRIu64
      ",\"mic_recording\":%s,\"mic_frames\":%" PRIu32 ",\"mic_overflows\":%" PRIu32
      ",\"mic_failures\":%" PRIu32 ",\"mic_peak\":%" PRIu32 "}",
      static_cast<unsigned>(ui & 7u), static_cast<unsigned>((ui >> 3) & 31u),
      static_cast<unsigned>((ui >> 8) & 3u), static_cast<unsigned>((ui >> 10) & 15u), ui & (1u << 14) ? "true" : "false",
      ui & (1u << 15) ? "true" : "false", g_nativePulse.load(std::memory_order_relaxed),
      mosaico::touch::Healthy() ? "true" : "false", Device::Storage::IsMounted() ? "true" : "false",
      static_cast<int>(mosaico::storage::Error()), mosaico::storage::MountedVolumeBytes(),
      mic.recording ? "true" : "false", mic.capturedFrames, mic.overflows, mic.readFailures, mic.peak);
  if (extra < 0 || static_cast<std::size_t>(extra) >= capacity - written + 1) return ESP_ERR_INVALID_SIZE;
  *responseSize = static_cast<std::size_t>(written - 1 + extra);
#endif
  return ESP_OK;
}

esp_err_t AudioTestRpc(const esp_iris_rpc_request_t* request, std::uint8_t* response,
                       std::size_t capacity, std::size_t* responseSize, void*)
{
  constexpr char kAccepted[] = "{\"schema\":1,\"queued\":true}";
  *responseSize = 0;
  if (!g_audioReplayEnabled) return ESP_ERR_NOT_SUPPORTED;
  if (request->payload_size != 0 || capacity < sizeof(kAccepted)) return ESP_ERR_INVALID_SIZE;
  if (g_frameCount.load(std::memory_order_acquire) == 0 || !mosaico::audio::Ready()) return ESP_ERR_INVALID_STATE;
  AudioTestState previous = g_testState.load(std::memory_order_acquire);
  for (;;)
  {
    if (previous == AudioTestState::Queued || previous == AudioTestState::Running) return ESP_ERR_INVALID_STATE;
    if (g_testState.compare_exchange_weak(previous, AudioTestState::Queued, std::memory_order_acq_rel)) break;
  }
  const int written = std::snprintf(reinterpret_cast<char*>(response), capacity, "%s", kAccepted);
  *responseSize = static_cast<std::size_t>(written);
  return ESP_OK;
}

}  // namespace

namespace mosaico::diagnostics {

bool Init(bool startupAudioTestRunning, bool audioReplayEnabled)
{
  if (g_registered) return true;
  g_audioReplayEnabled = audioReplayEnabled;
  g_testState.store(startupAudioTestRunning ? AudioTestState::Running : AudioTestState::Idle, std::memory_order_release);
  esp_err_t result = esp_iris_rpc_register(kServiceId, kStatusMethod, StatusRpc, nullptr);
  if (result != ESP_OK)
  {
    ESP_LOGE(kTag, "status RPC registration failed: %s", esp_err_to_name(result));
    return false;
  }
  result = esp_iris_rpc_register(kServiceId, kAudioTestMethod, AudioTestRpc, nullptr);
  if (result != ESP_OK)
  {
    (void)esp_iris_rpc_unregister(kServiceId, kStatusMethod);
    ESP_LOGE(kTag, "audio-test RPC registration failed: %s", esp_err_to_name(result));
    return false;
  }
  g_registered = true;
  ESP_LOGI(kTag, "diagnostics JSON schema=1: Iris service=0x%04x, status=%u, queue-audio-test=%u (empty payloads)",
           static_cast<unsigned>(kServiceId), static_cast<unsigned>(kStatusMethod), static_cast<unsigned>(kAudioTestMethod));
  return true;
}

void Publish(std::uint32_t frameCount, std::uint32_t elapsedMs,
             std::uint32_t lateFrameCount, std::uint32_t longestFrameMicros)
{
  g_elapsedMs.store(elapsedMs, std::memory_order_relaxed);
  g_lateFrames.store(lateFrameCount, std::memory_order_relaxed);
  g_longestFrameMicros.store(longestFrameMicros, std::memory_order_relaxed);
  g_frameCount.store(frameCount, std::memory_order_release);
}

bool ConsumeAudioTestRequest()
{
  AudioTestState expected = AudioTestState::Queued;
  return g_testState.compare_exchange_strong(expected, AudioTestState::Running, std::memory_order_acq_rel);
}

void CompleteAudioTest(bool passed)
{
  AudioTestState expected = AudioTestState::Running;
  if (g_testState.compare_exchange_strong(expected, passed ? AudioTestState::Passed : AudioTestState::Failed,
                                        std::memory_order_acq_rel))
    g_testCount.fetch_add(1, std::memory_order_relaxed);
}

#if defined(CARDENZA_NATIVE_UI)
void PublishNativeUi(std::uint8_t scene, std::uint8_t page, std::uint8_t track,
                     std::uint8_t modal, bool dirty, bool playing, std::uint32_t pulse)
{
  g_nativeUi.store((scene & 7u) | ((page & 31u) << 3) | ((track & 3u) << 8) |
                  ((modal & 15u) << 10) | (dirty ? 1u << 14 : 0u) | (playing ? 1u << 15 : 0u), std::memory_order_relaxed);
  g_nativePulse.store(pulse, std::memory_order_relaxed);
}
#endif

}  // namespace mosaico::diagnostics
