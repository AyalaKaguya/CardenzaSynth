#include "Devices/Mosaico/ScreenCapture.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "Devices/Device.h"
#include "Devices/Mosaico/DevicePlatform.h"
#include "bsp/esp_mosaico.h"
#include "esp_heap_caps.h"
#include "esp_iris.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

namespace {

constexpr std::size_t kPixelBytes = sizeof(Device::Display::Pixel);
constexpr std::size_t kLogicalBytes = Device::Display::kPixelCount * kPixelBytes;
constexpr std::uint32_t kStride = BSP_LCD_H_RES * kPixelBytes;
constexpr std::uint32_t kTotalBytes = kStride * BSP_LCD_V_RES;
constexpr int kLeft = (BSP_LCD_H_RES - Device::Display::kWidth) / 2;
constexpr int kTop = (BSP_LCD_V_RES - Device::Display::kHeight) / 2;
constexpr std::uint32_t kFrameWaitMs = 2000;

// The small synchronization state outlives Iris registration. All pixel-buffer
// access, including end/timeout cleanup, is under lock. The display owner only
// tries the lock, so a screenshot never holds up rendering or a DMA transfer.
SemaphoreHandle_t captureLock = nullptr;
SemaphoreHandle_t frameReady = nullptr;
std::atomic<bool> registered{false};
std::atomic<bool> pending{false};
Device::Display::Pixel* snapshot = nullptr;
bool snapshotReady = false;
std::uint32_t generation = 0;

void ReleaseLocked()
{
  pending.store(false, std::memory_order_release);
  heap_caps_free(snapshot);
  snapshot = nullptr;
  snapshotReady = false;
  ++generation;
}

esp_err_t Begin(const esp_iris_media_desc_t* requested, esp_iris_media_desc_t* actual,
                std::uint32_t* totalSize, void*)
{
  if (requested == nullptr || actual == nullptr || totalSize == nullptr) return ESP_ERR_INVALID_ARG;
  if (requested->format != 0 && requested->format != ESP_IRIS_PIXEL_FORMAT_RGB565) return ESP_ERR_NOT_SUPPORTED;
  if (!mosaico::display::Healthy()) return ESP_ERR_INVALID_STATE;
  if (xSemaphoreTake(captureLock, portMAX_DELAY) != pdTRUE) return ESP_ERR_TIMEOUT;
  if (snapshot != nullptr)
  {
    xSemaphoreGive(captureLock);
    return ESP_ERR_INVALID_STATE;
  }
  snapshot = static_cast<Device::Display::Pixel*>(heap_caps_malloc(kLogicalBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (snapshot == nullptr)
  {
    xSemaphoreGive(captureLock);
    return ESP_ERR_NO_MEM;
  }
  while (xSemaphoreTake(frameReady, 0) == pdTRUE) {}
  const std::uint32_t ticket = ++generation;
  snapshotReady = false;
  pending.store(true, std::memory_order_release);
  xSemaphoreGive(captureLock);

  (void)xSemaphoreTake(frameReady, pdMS_TO_TICKS(kFrameWaitMs));
  if (xSemaphoreTake(captureLock, portMAX_DELAY) != pdTRUE) return ESP_ERR_TIMEOUT;
  const bool complete = ticket == generation && snapshot != nullptr && snapshotReady && mosaico::display::Healthy();
  if (!complete)
  {
    // A cancelled/expired request cannot free a subsequent capture. The UI's
    // memcpy uses this same lock, so timeout never frees an in-progress copy.
    if (ticket == generation) ReleaseLocked();
    xSemaphoreGive(captureLock);
    return ESP_ERR_TIMEOUT;
  }
  *actual = {};
  actual->width = BSP_LCD_H_RES;
  actual->height = BSP_LCD_V_RES;
  actual->stride = kStride;
  actual->format = ESP_IRIS_PIXEL_FORMAT_RGB565;
  *totalSize = kTotalBytes;
  xSemaphoreGive(captureLock);
  return ESP_OK;
}

esp_err_t Read(std::uint32_t offset, std::uint8_t* out, std::size_t capacity, std::size_t* outSize, void*)
{
  if (outSize != nullptr) *outSize = 0;
  if (out == nullptr || outSize == nullptr || capacity == 0 || offset >= kTotalBytes) return ESP_ERR_INVALID_ARG;
  if (xSemaphoreTake(captureLock, portMAX_DELAY) != pdTRUE) return ESP_ERR_TIMEOUT;
  if (snapshot == nullptr || !snapshotReady)
  {
    xSemaphoreGive(captureLock);
    return ESP_ERR_INVALID_STATE;
  }
  const std::size_t count = (std::min)(capacity, static_cast<std::size_t>(kTotalBytes - offset));
  std::memset(out, 0, count);
  const std::size_t end = offset + count;
  const auto* pixels = reinterpret_cast<const std::uint8_t*>(snapshot);
  // S31 RGB565 is little-endian in RAM, as required by Iris. The black border
  // mirrors the physical display's clear; this is no LCD readback or touch feed.
  for (int row = 0; row < Device::Display::kHeight; ++row)
  {
    const std::size_t first = static_cast<std::size_t>(kTop + row) * kStride + kLeft * kPixelBytes;
    const std::size_t last = first + Device::Display::kWidth * kPixelBytes;
    const std::size_t from = (std::max)(static_cast<std::size_t>(offset), first);
    const std::size_t until = (std::min)(end, last);
    if (from < until)
      std::memcpy(out + from - offset, pixels + static_cast<std::size_t>(row) * Device::Display::kWidth * kPixelBytes + from - first, until - from);
  }
  *outSize = count;
  xSemaphoreGive(captureLock);
  return ESP_OK;
}

void End(void*)
{
  if (xSemaphoreTake(captureLock, portMAX_DELAY) != pdTRUE) return;
  ReleaseLocked();
  // Wake an early cancellation without leaving its begin blocked for 2 s.
  xSemaphoreGive(frameReady);
  xSemaphoreGive(captureLock);
}

}  // namespace

namespace mosaico::capture {

esp_err_t Init()
{
  if (registered.load(std::memory_order_acquire)) return ESP_OK;
  captureLock = xSemaphoreCreateMutex();
  frameReady = xSemaphoreCreateBinary();
  esp_err_t result = ESP_ERR_NO_MEM;
  if (captureLock != nullptr && frameReady != nullptr)
  {
    esp_iris_screen_backend_t backend{};
    backend.begin = Begin;
    backend.read = Read;
    backend.end = End;
    result = esp_iris_screen_register(&backend);
  }
  if (result == ESP_OK) registered.store(true, std::memory_order_release);
  else
  {
    if (frameReady != nullptr) vSemaphoreDelete(frameReady);
    if (captureLock != nullptr) vSemaphoreDelete(captureLock);
    frameReady = nullptr;
    captureLock = nullptr;
  }
  return result;
}

void FramePresented(const Device::Display::FrameBufferView& frame)
{
  if (!registered.load(std::memory_order_acquire) || !pending.load(std::memory_order_acquire)) return;
  if (frame.pixels == nullptr || frame.pixelCount != Device::Display::kPixelCount ||
      frame.width != Device::Display::kWidth || frame.height != Device::Display::kHeight) return;
  if (xSemaphoreTake(captureLock, 0) != pdTRUE) return;
  if (pending.load(std::memory_order_relaxed) && snapshot != nullptr)
  {
    std::memcpy(snapshot, frame.pixels, kLogicalBytes);
    snapshotReady = true;
    pending.store(false, std::memory_order_release);
    xSemaphoreGive(frameReady);
  }
  xSemaphoreGive(captureLock);
}

}  // namespace mosaico::capture
