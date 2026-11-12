#include "Devices/MosaicoNative/TouchBackend.h"
#include "Devices/Device.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>

#include "bsp/esp_mosaico.h"
#include "esp_iris.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

namespace {
constexpr char kTag[] = "NativeTouch";
constexpr unsigned kMaxPoints = 2;
constexpr unsigned kReleaseConfirmPolls = 2;
constexpr std::uint16_t kService = 0x4344;
QueueHandle_t events = nullptr;
esp_lcd_touch_handle_t touchHandle = nullptr;
std::atomic<bool> healthy{false};
std::atomic<bool> queueLost{false};
struct Point { bool down = false; std::uint8_t id = 0; std::int16_t x = 0; std::int16_t y = 0; };

void Enqueue(const Device::Touch::Event& event)
{
  if (xQueueSend(events, &event, 0) != pdTRUE) queueLost.store(true, std::memory_order_release);
}

void TouchTask(void*)
{
  std::array<Point, kMaxPoints> previous{};
  unsigned releaseMisses = 0;
  TickType_t wake = xTaskGetTickCount();
  std::int64_t logged = -10000000;
  for (;;)
  {
    std::array<esp_lcd_touch_point_data_t, kMaxPoints> points{};
    std::uint8_t count = 0;
    esp_err_t result = esp_lcd_touch_read_data(touchHandle);
    if (result == ESP_OK) result = esp_lcd_touch_get_data(touchHandle, points.data(), &count, points.size());
    std::array<Point, kMaxPoints> current{};
    if (result == ESP_OK)
    {
      healthy.store(true, std::memory_order_release);
      // The polling CST9220 driver can return an empty ACK frame between
      // valid reports. Match GSP's two-poll release confirmation so that a
      // transient empty read does not release a held ADD/SUB gesture.
      const bool held = std::any_of(previous.begin(), previous.end(), [](const Point& point) { return point.down; });
      if (count == 0 && held && ++releaseMisses < kReleaseConfirmPolls) current = previous;
      else if (count > 0 || !held) releaseMisses = 0;
      for (unsigned i = 0; i < count && i < current.size(); ++i)
      {
        current[i] = {true, points[i].track_id,
            static_cast<std::int16_t>((std::min)(points[i].x, std::uint16_t{479})),
            static_cast<std::int16_t>((std::min)(points[i].y, std::uint16_t{479}))};
        const auto found = std::find_if(previous.begin(), previous.end(), [&](const Point& old) { return old.down && old.id == current[i].id; });
        if (found == previous.end()) Enqueue({Device::Touch::Kind::Down, current[i].id, current[i].x, current[i].y});
        else if (found->x != current[i].x || found->y != current[i].y)
          Enqueue({Device::Touch::Kind::Move, current[i].id, current[i].x, current[i].y});
      }
    }
    else
    {
      releaseMisses = 0;
      healthy.store(false, std::memory_order_release);
      if (esp_timer_get_time() - logged >= 10000000)
      {
        ESP_LOGW(kTag, "read failed: %s; active gestures cancelled", esp_err_to_name(result));
        logged = esp_timer_get_time();
      }
    }
    for (const auto& old : previous)
    {
      if (!old.down) continue;
      const auto found = std::find_if(current.begin(), current.end(), [&](const Point& next) { return next.down && next.id == old.id; });
      if (found == current.end()) Enqueue({result == ESP_OK ? Device::Touch::Kind::Up : Device::Touch::Kind::Cancel, old.id, old.x, old.y});
    }
    previous = current;
    if (xTaskDelayUntil(&wake, pdMS_TO_TICKS(10)) != pdTRUE)
    {
      vTaskDelay(1);
      wake = xTaskGetTickCount();
    }
  }
}

// Native-only pointer replay for simulator-equivalent acceptance. It enters
// the same bounded queue as physical touch and never calls application code.
esp_err_t PointerRpc(const esp_iris_rpc_request_t* request, std::uint8_t*, std::size_t, std::size_t* responseSize, void*)
{
  *responseSize = 0;
  if (request->payload_size != 6) return ESP_ERR_INVALID_SIZE;
  const auto* bytes = request->payload;
  if (bytes[0] > 3 || bytes[1] < 128) return ESP_ERR_INVALID_ARG;
  const int x = bytes[2] | bytes[3] << 8;
  const int y = bytes[4] | bytes[5] << 8;
  if (x >= 480 || y >= 480) return ESP_ERR_INVALID_ARG;
  Enqueue({static_cast<Device::Touch::Kind>(bytes[0]), bytes[1], static_cast<std::int16_t>(x), static_cast<std::int16_t>(y)});
  return ESP_OK;
}
}

namespace mosaico::touch {
bool Init()
{
  if (events != nullptr) return touchHandle != nullptr;
  events = xQueueCreate(32, sizeof(Device::Touch::Event));
  if (events == nullptr) return false;
  esp_err_t result = bsp_touch_new(BSP_DISPLAY_ROTATE_0, &touchHandle);
  if (result != ESP_OK)
  {
    ESP_LOGE(kTag, "initialization failed: %s", esp_err_to_name(result));
    return false;
  }
  if (xTaskCreatePinnedToCore(TouchTask, "native_touch", 4096, nullptr, 4, nullptr, 0) != pdPASS) return false;
  result = esp_iris_rpc_register(kService, 1, PointerRpc, nullptr);
  if (result != ESP_OK) ESP_LOGW(kTag, "pointer replay registration: %s", esp_err_to_name(result));
  healthy.store(true, std::memory_order_release);
  ESP_LOGI(kTag, "two-point touch ready; pointer queue RPC=0x4344/1");
  return true;
}
bool Healthy() { return healthy.load(std::memory_order_acquire); }
}

namespace Device::Touch {
bool Poll(Event& event)
{
  if (events == nullptr) return false;
  if (queueLost.exchange(false, std::memory_order_acq_rel))
  {
    xQueueReset(events);
    event = {Kind::Cancel, 255, 0, 0};
    return true;
  }
  return xQueueReceive(events, &event, 0) == pdTRUE;
}
void CancelAll() { queueLost.store(true, std::memory_order_release); }
}
