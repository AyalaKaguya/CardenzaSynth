#include "Devices/Device.h"
#include "Devices/LedFrameBuffer.h"
#include "Devices/Mosaico/DevicePlatform.h"
#include "Devices/Mosaico/ScreenCapture.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>

#include "bsp/esp_mosaico.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

namespace {

constexpr char kTag[] = "CardenzaMosaico";
constexpr int kTransferLines = 8;  // Fits the BSP's maximum LCD DMA transaction.
constexpr int kImageLeft = (BSP_LCD_H_RES - Device::Display::kWidth) / 2;
constexpr int kImageTop = (BSP_LCD_V_RES - Device::Display::kHeight) / 2;
constexpr std::size_t kTransferPixels = BSP_LCD_H_RES * kTransferLines;
constexpr std::uint32_t kTransferTimeoutMs = 1000;
static_assert(Device::Display::kWidth <= BSP_LCD_H_RES && Device::Display::kHeight <= BSP_LCD_V_RES);
static_assert(kImageLeft % 4 == 0 && Device::Display::kWidth % 4 == 0,
              "CO5300 horizontal origin and transfer width must be multiples of four");

// The system/UI task owns the logical buffer. Only the persistent internal DMA
// buffer is submitted to the panel; it is never reused before color completion.
Device::Display::Pixel* framePixels = nullptr;
Device::Display::Pixel* transferPixels = nullptr;
esp_lcd_panel_handle_t panel = nullptr;
SemaphoreHandle_t displayMutex = nullptr;
SemaphoreHandle_t transferDone = nullptr;
bool callbacksInstalled = false;
bool initAttempted = false;  // System task only; BSP partial initialization is not retried.
bool displayFault = false;
std::atomic<bool> displayReady{false};
std::atomic<bool> displayHealthy{false};
std::atomic<std::uint8_t> backlightLevel{Device::Display::kDefaultBacklight};

Device::Keyboard::MatrixKeyState keyboardState{};
Device::LED::detail::LedFrameBuffer keyboardLedFrames;

using BackgroundJob = void (*)();
constexpr std::size_t kBackgroundJobCount = 4;
std::array<BackgroundJob, kBackgroundJobCount> backgroundJobs{};
portMUX_TYPE backgroundLock = portMUX_INITIALIZER_UNLOCKED;
TaskHandle_t backgroundTask = nullptr;  // Created/queried only by the system task.

bool IRAM_ATTR ColorTransferDone(esp_lcd_panel_io_handle_t, esp_lcd_panel_io_event_data_t*, void* context)
{
  BaseType_t wake = pdFALSE;
  xSemaphoreGiveFromISR(static_cast<SemaphoreHandle_t>(context), &wake);
  return wake == pdTRUE;
}

bool SendTransfer(int left, int top, int width, int height)
{
  const esp_err_t result = esp_lcd_panel_draw_bitmap(panel, left, top, left + width, top + height, transferPixels);
  if (result == ESP_OK && xSemaphoreTake(transferDone, pdMS_TO_TICKS(kTransferTimeoutMs)) == pdTRUE) return true;

  // A failed/late transfer may still borrow transferPixels. Stop presenting and
  // retain both it and its callback context for the rest of this boot.
  displayFault = true;
  displayHealthy.store(false, std::memory_order_release);
  ESP_LOGE(kTag, "LCD transfer stopped: %s", result == ESP_OK ? "completion timeout" : esp_err_to_name(result));
  return false;
}

bool ClearPhysicalDisplay()
{
  std::fill_n(transferPixels, kTransferPixels, Device::Display::Pixel{0});
  for (int y = 0; y < BSP_LCD_V_RES; y += kTransferLines)
  {
    if (!SendTransfer(0, y, BSP_LCD_H_RES, std::min(kTransferLines, BSP_LCD_V_RES - y))) return false;
  }
  return true;
}

void BackgroundWorker(void*)
{
  for (;;)
  {
    (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    std::array<BackgroundJob, kBackgroundJobCount> jobs{};
    portENTER_CRITICAL(&backgroundLock);
    jobs = backgroundJobs;
    portEXIT_CRITICAL(&backgroundLock);
    for (const BackgroundJob job : jobs) if (job != nullptr) job();
  }
}

}  // namespace

namespace mosaico::display {
bool Healthy() { return displayHealthy.load(std::memory_order_acquire); }
}  // namespace mosaico::display

namespace Device {

namespace Display {

FrameBufferView CurrentFrameBuffer() { return FrameBufferView{framePixels, kPixelCount, kWidth, kHeight}; }
MutableFrameBufferView MutableFrameBuffer() { return MutableFrameBufferView{framePixels, kPixelCount, kWidth, kHeight}; }

void Clear(Pixel color)
{
  if (framePixels != nullptr) std::fill_n(framePixels, kPixelCount, color);
}

void Present()
{
  if (!displayReady.load(std::memory_order_acquire)) return;
  if (xSemaphoreTake(displayMutex, pdMS_TO_TICKS(kTransferTimeoutMs)) != pdTRUE)
  {
    displayHealthy.store(false, std::memory_order_release);
    return;
  }
  if (!displayFault)
  {
    for (int row = 0; row < kHeight; row += kTransferLines)
    {
      const int lines = std::min(kTransferLines, kHeight - row);
      const std::size_t count = static_cast<std::size_t>(lines) * kWidth;
      const Pixel* source = framePixels + static_cast<std::size_t>(row) * kWidth;
      for (std::size_t pixel = 0; pixel < count; ++pixel)
      {
        const Pixel value = source[pixel];
        transferPixels[pixel] = static_cast<Pixel>((value << 8) | (value >> 8));
      }
      if (!SendTransfer(kImageLeft, kImageTop + row, kWidth, lines)) break;
    }
    if (!displayFault)
    {
      displayHealthy.store(true, std::memory_order_release);
      mosaico::capture::FramePresented(CurrentFrameBuffer());
    }
  }
  xSemaphoreGive(displayMutex);
}

void SetBacklight(std::uint8_t level)
{
  backlightLevel.store(level, std::memory_order_relaxed);
  if (!displayReady.load(std::memory_order_acquire)) return;
  if (xSemaphoreTake(displayMutex, pdMS_TO_TICKS(kTransferTimeoutMs)) != pdTRUE) return;
  const esp_err_t result = displayFault ? ESP_ERR_INVALID_STATE : bsp_display_brightness_set((level * 100 + 127) / 255);
  xSemaphoreGive(displayMutex);
  if (result != ESP_OK) ESP_LOGW(kTag, "LCD brightness: %s", esp_err_to_name(result));
}

std::uint8_t Backlight() { return backlightLevel.load(std::memory_order_relaxed); }

}  // namespace Display

namespace Keyboard {
const MatrixKeyState& State() { return keyboardState; }
}  // namespace Keyboard

namespace Buttons {
bool HomePressed() { return false; }
}  // namespace Buttons

// Keep LED state round-trippable for the application without claiming physical
// keyboard LEDs. External keyboard input and its feedback are a later backend.
namespace LED {
void BeginFrame() { keyboardLedFrames.BeginFrame(); }
void Present() { (void)keyboardLedFrames.Present(); }
std::uint8_t Brightness(std::size_t row, std::size_t column)
{
  return row < kRows && column < kColumns ? keyboardLedFrames.Current().levels[row * kColumns + column] : kBrightnessOff;
}
void SetKey(std::size_t row, std::size_t column, std::uint8_t brightness) { (void)keyboardLedFrames.SetKey(row, column, brightness); }
void Clear() { (void)keyboardLedFrames.Fill(kBrightnessOff); }
void Fill(std::uint8_t brightness) { (void)keyboardLedFrames.Fill(brightness); }
void SetGlobalBrightness(std::uint8_t brightness) { (void)keyboardLedFrames.SetMaster(brightness); }
std::uint8_t GlobalBrightness() { return keyboardLedFrames.Current().master; }
void SetEnabled(bool enabled) { (void)keyboardLedFrames.SetEnabled(enabled); }
bool Enabled() { return keyboardLedFrames.Current().enabled; }
}  // namespace LED

// ESP-Iris retains USB ownership. This first backend exposes no MIDI, guide
// channel, touch keyboard or computer-audio endpoint.
namespace USB {
bool Connected() { return false; }
bool ReadMidi(std::uint8_t[4]) { return false; }
bool WriteMidi(const std::uint8_t[4]) { return false; }
bool ReadAppMessage(AppMessage& message) { message.length = 0; return false; }
bool WriteAppMessage(const char*, std::size_t) { return false; }
}  // namespace USB

namespace Storage {
std::string RootPath() { return "/nand/apps"; }
bool Mount() { return false; }
bool IsMounted() { return false; }
void CheckAfterIoError() {}
std::uint64_t CapacityBytes() { return 0; }
std::uint64_t FreeBytes() { return 0; }
}  // namespace Storage

// Console logging remains available; there is no pretend persistent log ring
// while this backend deliberately has no filesystem mounted.
namespace Log {
std::size_t Take(char*, std::size_t, std::size_t& lost) { lost = 0; return 0; }
std::size_t Pending() { return 0; }
void Write(std::string_view line)
{
  if (!line.empty()) (void)std::fwrite(line.data(), 1, line.size(), stdout);
  if (line.empty() || line.back() != '\n') (void)std::fputc('\n', stdout);
}
}  // namespace Log

namespace Background {
bool Run(void (*job)())
{
  if (job == nullptr) return false;
  bool registered = false;
  portENTER_CRITICAL(&backgroundLock);
  for (const BackgroundJob existing : backgroundJobs) if (existing == job) registered = true;
  if (!registered)
  {
    for (BackgroundJob& slot : backgroundJobs)
    {
      if (slot == nullptr) { slot = job; registered = true; break; }
    }
  }
  portEXIT_CRITICAL(&backgroundLock);
  if (!registered) return false;
  if (backgroundTask == nullptr && xTaskCreate(BackgroundWorker, "cc_background", 4096, nullptr, 3, &backgroundTask) != pdPASS)
  {
    backgroundTask = nullptr;
    return false;
  }
  xTaskNotifyGive(backgroundTask);
  return true;
}
}  // namespace Background

std::string Name() { return "ESP-Mosaico"; }
std::string BackendName() { return "mosaico-bsp"; }

bool Init()
{
  if (displayReady.load(std::memory_order_acquire)) return !displayFault;
  if (initAttempted || displayFault) return false;
  initAttempted = true;
  if (displayMutex == nullptr) displayMutex = xSemaphoreCreateMutex();
  if (transferDone == nullptr) transferDone = xSemaphoreCreateBinary();
  if (framePixels == nullptr)
  {
    framePixels = static_cast<Display::Pixel*>(heap_caps_calloc(Display::kPixelCount, sizeof(Display::Pixel), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (framePixels == nullptr)
      framePixels = static_cast<Display::Pixel*>(heap_caps_calloc(Display::kPixelCount, sizeof(Display::Pixel), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  }
  if (transferPixels == nullptr)
    transferPixels = static_cast<Display::Pixel*>(heap_caps_malloc(kTransferPixels * sizeof(Display::Pixel), MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT));
  if (displayMutex == nullptr || transferDone == nullptr || framePixels == nullptr || transferPixels == nullptr)
  {
    ESP_LOGE(kTag, "insufficient memory for display buffers/synchronization");
    return false;
  }

  esp_err_t result = bsp_power_init();
  const bsp_display_config_t config = BSP_DISPLAY_DEFAULT_CONFIG();
  if (result == ESP_OK) result = bsp_display_new(&config, &panel);
  esp_lcd_panel_io_handle_t io = result == ESP_OK ? bsp_display_get_panel_io() : nullptr;
  if (result == ESP_OK && (panel == nullptr || io == nullptr)) result = ESP_ERR_INVALID_STATE;
  if (result == ESP_OK && !callbacksInstalled)
  {
    esp_lcd_panel_io_callbacks_t callbacks{};
    callbacks.on_color_trans_done = ColorTransferDone;
    result = esp_lcd_panel_io_register_event_callbacks(io, &callbacks, transferDone);
    callbacksInstalled = result == ESP_OK;
  }
  if (result == ESP_OK) result = bsp_display_brightness_set((Display::Backlight() * 100 + 127) / 255);
  if (result != ESP_OK)
  {
    ESP_LOGE(kTag, "BSP display initialization: %s", esp_err_to_name(result));
    return false;
  }
  if (!ClearPhysicalDisplay()) return false;
  displayReady.store(true, std::memory_order_release);
  const esp_err_t captureResult = mosaico::capture::Init();
  if (captureResult != ESP_OK) ESP_LOGW(kTag, "Iris screenshot unavailable: %s", esp_err_to_name(captureResult));
  ESP_LOGI(kTag, "display %dx%d, native %dx%d image at (%d,%d); input/storage unavailable",
           BSP_LCD_H_RES, BSP_LCD_V_RES, Display::kWidth, Display::kHeight, kImageLeft, kImageTop);
  return true;
}

void Start() {}

}  // namespace Device
