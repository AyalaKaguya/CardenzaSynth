#include "Devices/MosaicoNative/Microphone.h"
#include "Devices/Mosaico/AudioBackend.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <mutex>

#include "bsp/esp_mosaico.h"
#include "esp_codec_dev.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

namespace {
constexpr char kTag[] = "NativeMicrophone";
constexpr std::uint32_t kSampleRate = 44100;
constexpr std::size_t kFramesPerRead = 256;
// Existing NAND sync/stream contention has held the UI for about 750 ms.
// Keep roughly twice that measured interval, allocated only while recording;
// overflow remains an explicit bounded failure rather than losing old samples.
constexpr std::size_t kRingFrames = 65536;  // 128 KiB PSRAM, about 1.486 s at 44.1 kHz.
constexpr std::size_t kWireChannels = 2;
constexpr std::size_t kPrimeFrames = 1536; // Flush at least the BSP's six 240-frame DMA descriptors.
constexpr std::int64_t kControlWaitMicros = 1500000;
constexpr float kInputGainDb = 24.0f;
#if defined(CONFIG_CARDENZA_NATIVE_MIC_SLOT)
constexpr std::size_t kInputSlot = CONFIG_CARDENZA_NATIVE_MIC_SLOT;
#else
constexpr std::size_t kInputSlot = 0;  // ES8311 normal-I2S ADC output uses the left slot.
#endif
static_assert(kInputSlot < kWireChannels);

// Only the output owner creates/opens/closes the codec. The input task borrows
// it while readerActive is true. readerGate covers the handoff into a read;
// the output owner never waits for a blocking read and continues rendering.
esp_codec_dev_handle_t inputCodec = nullptr;
bool codecOpen = false;             // Output owner only.
bool failedOpen = false;            // One attempt per UI start request.
std::int64_t nextCloseAttempt = 0;  // Output owner; failed cleanup is retried at most once a second.
std::mutex readerGate;
bool readerActive = false;          // readerGate.
bool workerIdle = true;             // readerGate; includes the interval between reads.
std::atomic<bool> requested{false};
std::atomic<bool> enabled{false};
std::atomic<bool> ready{false};
std::atomic<bool> recordingMute{false};
std::atomic<bool> overflow{false};
std::mutex requestMutex;            // Serializes system-owner API calls.
std::mutex transitionMutex;         // Publishes start resources and codec cleanup atomically.
std::mutex ringMutex;
std::int16_t* ring = nullptr;       // ringMutex, freed only after the reader is idle.
std::int16_t* wire = nullptr;       // Persistent internal scratch while a recording is active.
std::size_t head = 0;
std::size_t count = 0;
TaskHandle_t readerTask = nullptr;  // Created once, sleeps when not recording.
std::atomic<std::uint32_t> capturedFrames{0};
std::atomic<std::uint32_t> overflowCount{0};
std::atomic<std::uint32_t> readFailures{0};
std::atomic<std::uint32_t> inputPeak{0};

void FreeBuffers()
{
  std::lock_guard<std::mutex> lock(ringMutex);
  heap_caps_free(wire);
  heap_caps_free(ring);
  wire = nullptr;
  ring = nullptr;
  head = count = 0;
}

void CaptureTask(void*)
{
  for (;;)
  {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    {
      std::lock_guard<std::mutex> gate(readerGate);
      if (!enabled.load(std::memory_order_acquire)) continue;
      workerIdle = false;
    }
    std::size_t primed = 0;
    for (;;)
    {
      {
        std::lock_guard<std::mutex> gate(readerGate);
        if (!enabled.load(std::memory_order_acquire))
        {
          workerIdle = true;
          break;
        }
        readerActive = true;
      }
      // codec read waits up to one second in esp_codec_dev. Stop requests do
      // not close the device until this borrow has ended under readerGate.
      const int result = esp_codec_dev_read(inputCodec, wire,
          static_cast<int>(kFramesPerRead * kWireChannels * sizeof(std::int16_t)));
      if (result != ESP_CODEC_DEV_OK)
      {
        overflow.store(true, std::memory_order_release);
        readFailures.fetch_add(1, std::memory_order_relaxed);
        requested.store(false, std::memory_order_release);
      }
      else if (enabled.load(std::memory_order_acquire))
      {
        if (primed < kPrimeFrames)
        {
          primed += kFramesPerRead;
          if (primed >= kPrimeFrames) ready.store(true, std::memory_order_release);
        }
        else
        {
          std::lock_guard<std::mutex> lock(ringMutex);
          if (ring != nullptr && count <= kRingFrames - kFramesPerRead)
          {
            std::uint32_t peak = 0;
            for (std::size_t frame = 0; frame < kFramesPerRead; ++frame)
            {
              const std::int16_t sample = wire[kWireChannels * frame + kInputSlot];
              ring[(head + count + frame) % kRingFrames] = sample;
              const std::int32_t value = sample;
              peak = (std::max)(peak, static_cast<std::uint32_t>(value < 0 ? -value : value));
            }
            count += kFramesPerRead;
            capturedFrames.fetch_add(kFramesPerRead, std::memory_order_relaxed);
            if (peak > inputPeak.load(std::memory_order_relaxed)) inputPeak.store(peak, std::memory_order_relaxed);
          }
          else
          {
            overflow.store(true, std::memory_order_release);
            overflowCount.fetch_add(1, std::memory_order_relaxed);
            requested.store(false, std::memory_order_release);
          }
        }
      }
      {
        std::lock_guard<std::mutex> gate(readerGate);
        readerActive = false;
        if (result != ESP_CODEC_DEV_OK || !requested.load(std::memory_order_acquire))
          enabled.store(false, std::memory_order_release);
      }
    }
  }
}
}  // namespace

namespace mosaico::microphone {

void ServiceControl(bool outputReady)
{
  std::lock_guard<std::mutex> transition(transitionMutex);
  if (requested.load(std::memory_order_acquire))
  {
    if (codecOpen || failedOpen || !outputReady) return;
    if (inputCodec == nullptr) inputCodec = bsp_audio_codec_microphone_init();
    esp_codec_dev_sample_info_t format{};
    format.sample_rate = kSampleRate;
    format.bits_per_sample = 16;
    format.channel = kWireChannels;  // Match the output's existing I2S wire format.
    int result = inputCodec == nullptr ? ESP_CODEC_DEV_NOT_FOUND : esp_codec_dev_open(inputCodec, &format);
    if (result == ESP_CODEC_DEV_OK) result = esp_codec_dev_set_in_gain(inputCodec, kInputGainDb);
    if (result != ESP_CODEC_DEV_OK)
    {
      // open can fail after setting its internal input-open flag. Close before
      // releasing anything, and retain ownership if that rollback also fails.
      if (inputCodec != nullptr && esp_codec_dev_close(inputCodec) != ESP_CODEC_DEV_OK)
      {
        codecOpen = true;
        nextCloseAttempt = esp_timer_get_time() + 1000000;
      }
      failedOpen = true;
      requested.store(false, std::memory_order_release);
      ESP_LOGE(kTag, "microphone open failed (%d)", result);
      return;
    }
    codecOpen = true;
    nextCloseAttempt = 0;
    {
      std::lock_guard<std::mutex> gate(readerGate);
      enabled.store(true, std::memory_order_release);
    }
    xTaskNotifyGive(readerTask);
    ESP_LOGI(kTag, "input starting: %u Hz, mono from I2S slot %u, gain %.0f dB, ring=%u frames",
             kSampleRate, static_cast<unsigned>(kInputSlot), static_cast<double>(kInputGainDb),
             static_cast<unsigned>(kRingFrames));
    return;
  }

  bool idle;
  {
    std::lock_guard<std::mutex> gate(readerGate);
    enabled.store(false, std::memory_order_release);
    idle = !readerActive && workerIdle;
  }
  if (!idle) return;
  if (codecOpen)
  {
    if (esp_timer_get_time() < nextCloseAttempt) return;
    const int result = esp_codec_dev_close(inputCodec);
    if (result != ESP_CODEC_DEV_OK)
    {
      // A borrowed codec or failed close is retained. No reader buffer is freed
      // and output stays muted until the next successful control attempt.
      nextCloseAttempt = esp_timer_get_time() + 1000000;
      ESP_LOGE(kTag, "microphone close failed (%d); resources retained", result);
      return;
    }
    codecOpen = false;
    ESP_LOGI(kTag, "input stopped: captured=%u overflow=%u read_failures=%u peak=%u",
             static_cast<unsigned>(capturedFrames.load()), static_cast<unsigned>(overflowCount.load()),
             static_cast<unsigned>(readFailures.load()), static_cast<unsigned>(inputPeak.load()));
  }
  if (recordingMute.load(std::memory_order_acquire)) FreeBuffers();
  failedOpen = false;
  ready.store(false, std::memory_order_release);
  recordingMute.store(false, std::memory_order_release);
}

Device::Audio::InputStatus Start()
{
  std::lock_guard<std::mutex> request(requestMutex);
  if (requested.load(std::memory_order_acquire) && ready.load(std::memory_order_acquire)) return Device::Audio::InputStatus::Ready;
  if (!audio::Ready() || recordingMute.load(std::memory_order_acquire)) return Device::Audio::InputStatus::NotReady;
  {
    std::lock_guard<std::mutex> lock(ringMutex);
    ring = static_cast<std::int16_t*>(heap_caps_malloc(kRingFrames * sizeof(std::int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    wire = static_cast<std::int16_t*>(heap_caps_malloc(kFramesPerRead * kWireChannels * sizeof(std::int16_t),
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT | MALLOC_CAP_DMA));
    head = count = 0;
  }
  if (ring == nullptr || wire == nullptr)
  {
    FreeBuffers();
    return Device::Audio::InputStatus::NotReady;
  }
  if (readerTask == nullptr && xTaskCreate(CaptureTask, "sample_input", 6144, nullptr, 6, &readerTask) != pdPASS)
  {
    readerTask = nullptr;
    FreeBuffers();
    return Device::Audio::InputStatus::NotReady;
  }
  {
    std::lock_guard<std::mutex> transition(transitionMutex);
    overflow.store(false, std::memory_order_release);
    inputPeak.store(0, std::memory_order_relaxed);
    ready.store(false, std::memory_order_release);
    recordingMute.store(true, std::memory_order_release);
    requested.store(true, std::memory_order_release);
  }
  const std::int64_t deadline = esp_timer_get_time() + kControlWaitMicros;
  while (esp_timer_get_time() < deadline)
  {
    if (ready.load(std::memory_order_acquire)) return Device::Audio::InputStatus::Ready;
    if (!requested.load(std::memory_order_acquire)) break;
    vTaskDelay(1);
  }
  requested.store(false, std::memory_order_release);
  ESP_LOGW(kTag, "input did not become ready; output owner will finish cleanup");
  return Device::Audio::InputStatus::NotReady;
}

void Stop()
{
  std::lock_guard<std::mutex> request(requestMutex);
  requested.store(false, std::memory_order_release);
  ready.store(false, std::memory_order_release);
  const std::int64_t deadline = esp_timer_get_time() + kControlWaitMicros;
  while (recordingMute.load(std::memory_order_acquire) && esp_timer_get_time() < deadline) vTaskDelay(1);
  if (recordingMute.load(std::memory_order_acquire)) ESP_LOGW(kTag, "input cleanup pending; buffers retained");
}

Device::Audio::InputStatus Read(std::int16_t* destination, std::size_t frameCount,
                                std::size_t& framesRead, std::uint32_t timeoutMs)
{
  framesRead = 0;
  if (destination == nullptr && frameCount != 0) return Device::Audio::InputStatus::InvalidArgument;
  if (frameCount == 0) return Device::Audio::InputStatus::Ready;
  const std::int64_t deadline = esp_timer_get_time() + static_cast<std::int64_t>(timeoutMs) * 1000;
  for (;;)
  {
    if (overflow.load(std::memory_order_acquire)) return Device::Audio::InputStatus::Overflow;
    {
      std::lock_guard<std::mutex> lock(ringMutex);
      framesRead = (std::min)(count, frameCount);
      for (std::size_t frame = 0; frame < framesRead; ++frame) destination[frame] = ring[(head + frame) % kRingFrames];
      head = (head + framesRead) % kRingFrames;
      count -= framesRead;
    }
    if (framesRead != 0) return Device::Audio::InputStatus::Ready;
    if (!ready.load(std::memory_order_acquire)) return Device::Audio::InputStatus::NotReady;
    if (timeoutMs == 0 || esp_timer_get_time() >= deadline) return Device::Audio::InputStatus::Ready;
    vTaskDelay(1);
  }
}

bool OutputMuted() { return recordingMute.load(std::memory_order_acquire); }
Diagnostics GetDiagnostics()
{
  return {requested.load(std::memory_order_acquire) && ready.load(std::memory_order_acquire),
          capturedFrames.load(), overflowCount.load(), readFailures.load(), inputPeak.load()};
}
}  // namespace mosaico::microphone
