#include <cstdint>
#include <atomic>
#include "Devices/Device.h"
#include "Applications/Synth/GrooveApp.h"
#include "Devices/Mosaico/AudioBackend.h"
#include "Devices/Mosaico/DevicePlatform.h"
#include "Devices/Mosaico/Diagnostics.h"
#include "Devices/MosaicoNative/TouchBackend.h"
#include "Devices/MosaicoNative/StorageBackend.h"
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

namespace {
constexpr char kTag[] = "MosaicoNative";
constexpr TickType_t kTick = pdMS_TO_TICKS(10);
constexpr std::int64_t kDrawPeriodUs = 33333;
constexpr char kAppRoot[] = "/nand/apps/mosaico-native/CardenzaSynth";
std::atomic<bool> flushRequested{false};

esp_err_t FileStatus(void*)
{
  return Device::Storage::IsMounted() ? ESP_OK : mosaico::storage::Error();
}

esp_err_t FlushSessionRpc(const esp_iris_rpc_request_t* request, std::uint8_t*, std::size_t,
                          std::size_t* responseSize, void*)
{
  *responseSize = 0;
  if (request->payload_size != 0) return ESP_ERR_INVALID_SIZE;
  flushRequested.store(true, std::memory_order_release);
  return ESP_OK;
}

void Runtime(void*)
{
  if (!Device::Init())
  {
    ESP_LOGE(kTag, "display/touch failed; Vibe Mode remains available");
    vTaskDelete(nullptr);
    return;
  }
  mosaico::audio::SetMuted(true);
  if (!mosaico::audio::Start() || !os::system::Begin())
  {
    ESP_LOGE(kTag, "application initialization failed; Vibe Mode remains available");
    vTaskDelete(nullptr);
    return;
  }
  // Settings and working project are restored by the existing system/app.
  // Begin never starts transport, and native has no automatic startup phrase.
  mosaico::audio::SetMuted(false);
  (void)mosaico::diagnostics::Init(false, false);
  ESP_ERROR_CHECK_WITHOUT_ABORT(esp_iris_rpc_register(0x4344, 2, FlushSessionRpc, nullptr));
  ESP_LOGI(kTag, "mosaico-native ready: 480x480, 10ms tick, 30fps draw target, storage=%u, heap=%u PSRAM=%u",
           Device::Storage::IsMounted(), static_cast<unsigned>(memory::FastFree()), static_cast<unsigned>(memory::SlowFree()));
  const std::int64_t started = esp_timer_get_time();
  std::int64_t nextDraw = started;
  TickType_t wake = xTaskGetTickCount();
  std::uint32_t frames = 0, late = 0, longest = 0, streak = 0;
  std::uint32_t healthTried = 0;
  bool healthy = false;
  for (;;)
  {
    const std::int64_t begin = esp_timer_get_time();
    const bool draw = begin >= nextDraw;
    if (draw)
    {
      do { nextDraw += kDrawPeriodUs; } while (nextDraw <= begin);
    }
    os::system::RunFrame(draw);
    if (flushRequested.exchange(false, std::memory_order_acq_rel))
      static_cast<apps::GrooveApp*>(os::system::ActiveApplication())->FlushSession();
    auto& application = *static_cast<apps::GrooveApp*>(os::system::ActiveApplication());
    const auto& controller = application.Ui();
    mosaico::diagnostics::PublishNativeUi(static_cast<std::uint8_t>(controller.State().scene), controller.CurrentPage(),
        static_cast<std::uint8_t>(controller.State().track),
        static_cast<std::uint8_t>(controller.TopModal() == nullptr ? groove::ModalKind::None : controller.TopModal()->kind), controller.State().dirty,
        application.Sequencer().Playing(), static_cast<std::uint32_t>(application.Sequencer().PlayPulse()));
    const auto elapsed = static_cast<std::uint32_t>((esp_timer_get_time() - started) / 1000);
    const auto duration = static_cast<std::uint32_t>(esp_timer_get_time() - begin);
    if (duration > longest) longest = duration;
    mosaico::diagnostics::Publish(++frames, elapsed, late, longest);
    if (!healthy && elapsed >= 1500 && elapsed - healthTried >= 1000 && mosaico::display::Healthy() &&
        mosaico::touch::Healthy() && mosaico::audio::Ready() && mosaico::audio::GetDiagnostics().writtenBlocks > 0)
    {
      healthTried = elapsed;
      if (esp_iris_mark_healthy() == ESP_OK)
      {
        healthy = true;
        ESP_LOGI(kTag, "native application lifecycle healthy");
      }
    }
    if (xTaskDelayUntil(&wake, kTick) == pdTRUE) streak = 0;
    else
    {
      ++late;
      if (++streak >= 3)
      {
        streak = 0;
        vTaskDelay(1);
        wake = xTaskGetTickCount();
      }
    }
  }
}
}

extern "C" void app_main()
{
  ESP_ERROR_CHECK_WITHOUT_ABORT(esp_iris_boot_probe());
  // Export only this application's files. Other apps, retained settings and
  // installation bundles elsewhere on NAND are outside this logical volume.
  esp_iris_file_volume_config_t volume{};
  volume.id = "native";
  volume.base_path = kAppRoot;
  volume.capabilities = ESP_IRIS_FILE_VOLUME_READ | ESP_IRIS_FILE_VOLUME_LIST | ESP_IRIS_FILE_VOLUME_MTIME |
      ESP_IRIS_FILE_VOLUME_WRITE | ESP_IRIS_FILE_VOLUME_ATOMIC_REPLACE | ESP_IRIS_FILE_VOLUME_HASH;
  ESP_ERROR_CHECK_WITHOUT_ABORT(esp_iris_file_volume_register_deferred(&volume, FileStatus, nullptr));
  iris_ota_support_start();
  ESP_LOGI(kTag, "firmware=%s IDF=%s target=esp32s31", esp_app_get_description()->version, esp_get_idf_version());
  if (xTaskCreatePinnedToCore(Runtime, "native_runtime", 24576, nullptr, 5, nullptr, 0) != pdPASS)
    ESP_LOGE(kTag, "runtime task unavailable; management remains active");
}
