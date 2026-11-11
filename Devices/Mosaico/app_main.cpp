#include <cstdint>
#include <cstdio>

#include "Applications/Synth/GrooveApp.h"
#include "Devices/Device.h"
#include "Devices/Mosaico/AudioBackend.h"
#include "Devices/Mosaico/DevicePlatform.h"
#include "Devices/Mosaico/Diagnostics.h"
#include "Library/Memory/Memory.h"
#include "OS/System/System.h"
#include "esp_app_desc.h"
#include "esp_iris.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
extern "C" {
#include "iris_ota_support.h"
}
#include "sdkconfig.h"

namespace {
constexpr char kTag[] = "CardenzaMosaico";
constexpr std::uint32_t kRuntimeStackBytes = 20 * 1024;
constexpr UBaseType_t kRuntimePriority = 5;
constexpr TickType_t kRuntimeTick = pdMS_TO_TICKS(10);
static_assert(kRuntimeTick > 0, "The system needs a 10 ms scheduler tick");

void LogHeap(const char* stage)
{
  ESP_LOGI(kTag, "%s: internal=%u largest=%u PSRAM=%u/%u", stage,
           static_cast<unsigned>(memory::FastFree()), static_cast<unsigned>(memory::FastLargest()),
           static_cast<unsigned>(memory::SlowFree()), static_cast<unsigned>(memory::SlowTotal()));
}

// Runs on the UI/system owner, through the existing sequencer and MIDI sink.
// No synthetic matrix keys, independent oscillator or new audio owner is used.
class StartupAudioTest {
public:
  bool Passed() const { return done_ && passed_; }
  bool Done() const { return done_; }
  void Tick(apps::GrooveApp& app, std::uint32_t elapsedMs)
  {
    if (done_) return;
    if (!started_)
    {
      if (elapsedMs < 2000 || !mosaico::audio::Ready()) return;
      auto& sequence = app.Sequencer();
      sequence.SetTempo(120);
      sequence.SetTrackStepCount(kTrack, 8);
      constexpr std::uint8_t notes[] = {60, 64, 67};
      for (std::size_t i = 0; i < 3; ++i)
      {
        if (!sequence.SetStepNotes(kTrack, i * 2, &notes[i], 1, 80))
        {
          sequence.Stop();
          ESP_LOGE(kTag, "audio test: could not create test pattern");
          done_ = true;
          return;
        }
      }
      before_ = mosaico::audio::GetDiagnostics();
      sequence.Play();
      startedAt_ = elapsedMs;
      started_ = true;
      ESP_LOGI(kTag, "audio test: short Juno phrase through sequencer/MIDI/synth/BSP, volume=%u%%",
               static_cast<unsigned>(Device::Audio::OutputVolume()));
      return;
    }
    if (elapsedMs - startedAt_ < 2500) return;
    auto& sequence = app.Sequencer();
    sequence.Stop();
    constexpr std::uint8_t notes[] = {60, 64, 67};
    for (std::size_t i = 0; i < 3; ++i) (void)sequence.RemoveNote(kTrack, 0, i * 2, notes[i]);
    sequence.SetTrackStepCount(kTrack, 16);
    const auto after = mosaico::audio::GetDiagnostics();
    const bool rendered = after.nonSilentBlocks > before_.nonSilentBlocks;
    const bool written = after.writtenBlocks > before_.writtenBlocks;
    const bool ok = rendered && written && after.writeFailures == before_.writeFailures && app.EndedOrphanNotes() == 0;
    passed_ = ok;
    ESP_LOGI(kTag, "audio test: software pipeline %s, rendered=%u written=%u failures=%u peak=%u orphan=%u",
             ok ? "PASS" : "FAIL", static_cast<unsigned>(after.nonSilentBlocks - before_.nonSilentBlocks),
             static_cast<unsigned>(after.writtenBlocks - before_.writtenBlocks),
             static_cast<unsigned>(after.writeFailures - before_.writeFailures),
             static_cast<unsigned>(after.peak), static_cast<unsigned>(app.EndedOrphanNotes()));
    done_ = true;
  }
private:
  static constexpr std::size_t kTrack = 1;  // The default Juno track.
  mosaico::audio::Diagnostics before_{};
  std::uint32_t startedAt_ = 0;
  bool started_ = false;
  bool done_ = false;
  bool passed_ = false;
};

void RuntimeTask(void*)
{
  LogHeap("before boot");
  // BSP power initialization has a ramp and no cross-core lazy-init lock.
  // Finish it before the audio task can enter codec/power initialization.
  if (!Device::Init())
  {
    ESP_LOGE(kTag, "display/device initialization failed; management remains active");
    vTaskDelete(nullptr);
    return;
  }
  mosaico::audio::SetMuted(true);
  if (!mosaico::audio::Start())
  {
    ESP_LOGE(kTag, "audio task could not start; Vibe Mode return remains available");
    vTaskDelete(nullptr);
    return;
  }
  os::system::BootOptions options;
  options.applicationArgs.emplace_back("--volatile-session");
  if (!os::system::Begin(options))
  {
    ESP_LOGE(kTag, "system boot failed; Vibe Mode return remains available");
    vTaskDelete(nullptr);
    return;
  }
  auto& application = *static_cast<apps::GrooveApp*>(os::system::ActiveApplication());
  application.SetMasterVolume(CONFIG_CARDENZA_MOSAICO_TEST_VOLUME);
  mosaico::audio::SetMuted(false);
  LogHeap("after boot");
  ESP_LOGI(kTag, "system ready: original %dx%d UI at 1:1, 10ms ticks/20ms draw target; keyboard/storage not connected",
           Device::Display::kWidth, Device::Display::kHeight);

  const std::int64_t bootAt = esp_timer_get_time();
  TickType_t wake = xTaskGetTickCount();
  unsigned lateFrames = 0;
  std::uint32_t totalLateFrames = 0;
  std::uint32_t longestFrameMicros = 0;
  bool healthy = false;
  std::uint32_t lastHealthAttempt = 0;
  StartupAudioTest audioTest;
#if CONFIG_CARDENZA_MOSAICO_AUDIO_TEST
  bool audioTestActive = true;
#else
  bool audioTestActive = false;
#endif
  (void)mosaico::diagnostics::Init(audioTestActive);
  for (std::uint32_t frame = 0;; ++frame)
  {
    const std::int64_t frameBegin = esp_timer_get_time();
    os::system::RunFrame((frame & 1u) == 0);
    const auto elapsedMs = static_cast<std::uint32_t>((esp_timer_get_time() - bootAt) / 1000);
    if (mosaico::diagnostics::ConsumeAudioTestRequest())
    {
      audioTest = StartupAudioTest{};
      audioTestActive = true;
    }
    if (audioTestActive)
    {
      audioTest.Tick(*static_cast<apps::GrooveApp*>(os::system::ActiveApplication()), elapsedMs);
      if (audioTest.Done())
      {
        mosaico::diagnostics::CompleteAudioTest(audioTest.Passed());
        audioTestActive = false;
      }
    }
#if CONFIG_CARDENZA_MOSAICO_AUDIO_TEST
    const bool audioHealthy = audioTest.Passed();
#else
    const bool audioHealthy = true;
#endif
    // Accept only after application frames and the real output task are running.
    // Physical screen/speaker acceptance is a separate device test.
    if (!healthy && audioHealthy && elapsedMs >= 1500 && elapsedMs - lastHealthAttempt >= 1000 &&
        mosaico::display::Healthy() && mosaico::audio::Ready() && mosaico::audio::GetDiagnostics().writtenBlocks != 0)
    {
      lastHealthAttempt = elapsedMs;
      const esp_err_t result = esp_iris_mark_healthy();
      if (result == ESP_OK)
      {
        healthy = true;
        ESP_LOGI(kTag, "application lifecycle healthy; retained Vibe Mode is available");
      }
    }
    const auto frameMicros = static_cast<std::uint32_t>(esp_timer_get_time() - frameBegin);
    if (frameMicros > longestFrameMicros) longestFrameMicros = frameMicros;
    mosaico::diagnostics::Publish(frame + 1, elapsedMs, totalLateFrames, longestFrameMicros);
    if (xTaskDelayUntil(&wake, kRuntimeTick) == pdTRUE) lateFrames = 0;
    else
    {
      ++totalLateFrames;
      if (++lateFrames >= 3)
      {
        lateFrames = 0;
        vTaskDelay(1);
        wake = xTaskGetTickCount();
      }
    }
  }
}
}  // namespace

extern "C" void app_main()
{
  ESP_ERROR_CHECK_WITHOUT_ABORT(esp_iris_boot_probe());
  // Starts management before the application so hardware failures do not hide
  // the public request to return to retained Vibe Mode. Never erase NVS here.
  iris_ota_support_start();
  ESP_LOGI(kTag, "firmware %s, ESP-IDF %s, target ESP32-S31", esp_app_get_description()->version, esp_get_idf_version());
  if (xTaskCreatePinnedToCore(RuntimeTask, "cc_runtime", kRuntimeStackBytes, nullptr, kRuntimePriority, nullptr, 0) != pdPASS)
    ESP_LOGE(kTag, "system task allocation failed; management remains active");
}
