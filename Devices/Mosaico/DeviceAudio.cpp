#include "Devices/Mosaico/AudioBackend.h"
#include "Devices/Device.h"
#include "Devices/DeviceAudio.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cinttypes>
#include <cstdint>
#include <mutex>

#include "bsp/esp_mosaico.h"
#include "esp_attr.h"
#include "esp_codec_dev.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace {

constexpr char kLogTag[] = "MosaicoAudio";
constexpr std::uint32_t kSampleRate = device::kDefaultSynthAudioSampleRate;
constexpr std::size_t kBlockFrames = 256;
constexpr std::int64_t kBlockMicros = 1000000LL * kBlockFrames / kSampleRate;
constexpr std::int64_t kRetryMicros = 1000000;
constexpr std::int64_t kLogIntervalMicros = 300000000;
constexpr std::int64_t kWriteWaitedMicros = 300;
constexpr std::uint32_t kTaskStackBytes = 12 * 1024;
constexpr UBaseType_t kTaskPriority = 6;
constexpr BaseType_t kTaskCore = 1;
constexpr std::int32_t kUnityGain = 32768;

// Small, fixed internal scratch buffers; the codec copies from these into the
// BSP-owned DMA buffers. No output-block allocation can spill into PSRAM.
DRAM_ATTR std::array<std::int16_t, kBlockFrames> g_mono{};
DRAM_ATTR std::array<std::int16_t, kBlockFrames * 2> g_stereo{};
std::atomic<device::SynthAudioRenderCallback> g_renderCallback{nullptr};
std::atomic<std::uint8_t> g_outputVolume{Device::Audio::kDefaultOutputVolume};
std::atomic<bool> g_muted{false};
std::atomic<bool> g_outputReady{false};
std::mutex g_startMutex;
TaskHandle_t g_task = nullptr;  // protected by g_startMutex; never deleted while the app runs

std::atomic<std::uint32_t> g_blocks{0};
std::atomic<std::uint32_t> g_renderMicros{0};
std::atomic<std::uint32_t> g_overruns{0};
std::atomic<std::uint32_t> g_underruns{0};
std::atomic<std::uint32_t> g_longestRenderMicros{0};
std::atomic<std::uint32_t> g_writtenBlocks{0};
std::atomic<std::uint32_t> g_nonSilentBlocks{0};
std::atomic<std::uint32_t> g_writeFailures{0};
std::atomic<std::uint32_t> g_peak{0};
std::atomic<std::uint32_t> g_queueFrames{0};

// Called once, by the output task. The BSP has no public teardown for a failed
// partial I2S allocation, so do not repeatedly initialize it after such a failure.
esp_codec_dev_handle_t InitializeSpeaker()
{
  i2s_std_config_t config{};
  config.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(kSampleRate);
  config.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
  config.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);
  config.gpio_cfg.mclk = BSP_AUDIO_I2S_MCLK;
  config.gpio_cfg.bclk = BSP_AUDIO_I2S_SCLK;
  config.gpio_cfg.ws = BSP_AUDIO_I2S_LRCLK;
  config.gpio_cfg.dout = BSP_AUDIO_I2S_SDOUT;
  config.gpio_cfg.din = BSP_AUDIO_I2S_DSIN;
  const esp_err_t result = bsp_audio_init(&config);
  if (result != ESP_OK)
  {
    ESP_LOGE(kLogTag, "BSP audio initialization failed: %s; synth events will still be drained", esp_err_to_name(result));
    return nullptr;
  }
  // speaker_init calls bsp_audio_init(NULL), which returns immediately once the
  // BSP data interface exists: it cannot replace our 44.1 kHz setup with 48 kHz.
  const auto speaker = bsp_audio_codec_speaker_init();
  if (speaker == nullptr) ESP_LOGE(kLogTag, "BSP ES8311 speaker unavailable");
  return speaker;
}

bool OpenSpeaker(esp_codec_dev_handle_t speaker)
{
  esp_codec_dev_sample_info_t format{};
  format.sample_rate = kSampleRate;
  format.bits_per_sample = 16;
  format.channel = 2;
  int result = esp_codec_dev_open(speaker, &format);
  if (result == ESP_CODEC_DEV_OK)
  {
    // Device volume is linear software scaling below, like the existing hosts.
    // Unity codec volume avoids applying that same setting twice.
    result = esp_codec_dev_set_out_vol(speaker, 100);
    if (result == ESP_CODEC_DEV_OK) result = esp_codec_dev_set_out_mute(speaker, false);
  }
  if (result != ESP_CODEC_DEV_OK)
  {
    (void)esp_codec_dev_set_out_mute(speaker, true);
    (void)esp_codec_dev_close(speaker);
    return false;
  }
  g_outputReady.store(true, std::memory_order_release);
  ESP_LOGI(kLogTag, "ES8311 output ready: %" PRIu32 " Hz, stereo int16, %u-frame synth blocks, DMA=%" PRIu32 " frames",
           kSampleRate, static_cast<unsigned>(kBlockFrames), g_queueFrames.load(std::memory_order_relaxed));
  return true;
}

void RenderBlock()
{
  const auto callback = g_renderCallback.load(std::memory_order_acquire);
  const std::int64_t begin = esp_timer_get_time();
  const std::size_t frames = callback == nullptr ? 0 : (std::min)(callback(g_mono.data(), g_mono.size()), g_mono.size());
  std::fill(g_mono.begin() + frames, g_mono.end(), 0);
  const auto elapsed = static_cast<std::uint32_t>(esp_timer_get_time() - begin);
  g_blocks.fetch_add(1, std::memory_order_relaxed);
  g_renderMicros.fetch_add(elapsed, std::memory_order_relaxed);
  if (elapsed > kBlockMicros) g_overruns.fetch_add(1, std::memory_order_relaxed);
  if (elapsed > g_longestRenderMicros.load(std::memory_order_relaxed))
    g_longestRenderMicros.store(elapsed, std::memory_order_relaxed);

  std::uint32_t peak = 0;
  for (const auto sample : g_mono)
  {
    const std::int32_t value = sample;
    peak = (std::max)(peak, static_cast<std::uint32_t>(value < 0 ? -value : value));
  }
  if (peak != 0) g_nonSilentBlocks.fetch_add(1, std::memory_order_relaxed);
  if (peak > g_peak.load(std::memory_order_relaxed)) g_peak.store(peak, std::memory_order_relaxed);
}

void PrepareStereo(std::int32_t& previousGain)
{
  const std::int32_t target = g_muted.load(std::memory_order_relaxed) ? 0 :
      (g_outputVolume.load(std::memory_order_relaxed) * kUnityGain + 50) / 100;
  for (std::size_t frame = 0; frame < kBlockFrames; ++frame)
  {
    const std::int32_t gain = previousGain + (target - previousGain) * static_cast<std::int32_t>(frame + 1) /
        static_cast<std::int32_t>(kBlockFrames);
    const auto sample = static_cast<std::int16_t>((static_cast<std::int32_t>(g_mono[frame]) * gain) / kUnityGain);
    g_stereo[2 * frame] = sample;
    g_stereo[2 * frame + 1] = sample;
  }
  previousGain = target;
}

// When output is unavailable there is no blocking I2S write to pace rendering.
// Continue consuming synth events at real time, with bounded catch-up and a yield.
void PaceWithoutOutput(std::int64_t& started, std::uint64_t& frames)
{
  const std::int64_t now = esp_timer_get_time();
  std::int64_t due = started + static_cast<std::int64_t>(frames * 1000000ULL / kSampleRate);
  if (started == 0 || now - due > 2 * kBlockMicros)
  {
    started = now;
    frames = 0;
    due = now;
  }
  while (esp_timer_get_time() < due) vTaskDelay(1);
  frames += kBlockFrames;
}

void AudioTask(void*)
{
  const i2s_chan_config_t defaults = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  const std::uint32_t dmaFrames = defaults.dma_desc_num * defaults.dma_frame_num;
  const std::int64_t queueMicros = 1000000LL * dmaFrames / kSampleRate;
  g_queueFrames.store(dmaFrames, std::memory_order_relaxed);
#if defined(CARDENZA_AUDIO_QUEUE_FRAMES)
  if (dmaFrames != CARDENZA_AUDIO_QUEUE_FRAMES)
    ESP_LOGW(kLogTag, "BSP DMA queue has %" PRIu32 " frames; synth queue policy expects %u",
             dmaFrames, static_cast<unsigned>(CARDENZA_AUDIO_QUEUE_FRAMES));
#endif
  const auto speaker = InitializeSpeaker();
  std::int64_t retryAt = 0;
  std::int64_t logAt = esp_timer_get_time() + kLogIntervalMicros;
  std::int64_t lastWriteEnd = 0;
  std::int64_t queuedMicros = 0;
  std::int64_t drainStarted = 0;
  std::uint64_t drainFrames = 0;
  std::int32_t previousGain = 0;
  std::uint32_t openFailures = 0;

  for (;;)
  {
    const std::int64_t now = esp_timer_get_time();
    if (!g_outputReady.load(std::memory_order_relaxed) && speaker != nullptr && now >= retryAt)
    {
      if (!OpenSpeaker(speaker))
      {
        if ((openFailures++ % 64) == 0) ESP_LOGE(kLogTag, "ES8311 open failed; retrying once a second");
        retryAt = esp_timer_get_time() + kRetryMicros;
      }
    }

    const bool ready = g_outputReady.load(std::memory_order_relaxed);
    if (!ready) PaceWithoutOutput(drainStarted, drainFrames);
    else drainStarted = 0;
    RenderBlock();
    PrepareStereo(previousGain);

    if (ready)
    {
      const std::int64_t writeBegin = esp_timer_get_time();
      if (lastWriteEnd != 0)
      {
        queuedMicros -= writeBegin - lastWriteEnd;
        if (queuedMicros < 0)
        {
          g_underruns.fetch_add(1, std::memory_order_relaxed);
          queuedMicros = 0;
        }
      }
      const int result = esp_codec_dev_write(speaker, g_stereo.data(), static_cast<int>(g_stereo.size() * sizeof(g_stereo[0])));
      const std::int64_t writeEnd = esp_timer_get_time();
      if (result == ESP_CODEC_DEV_OK)
      {
        g_writtenBlocks.fetch_add(1, std::memory_order_relaxed);
        queuedMicros = writeEnd - writeBegin > kWriteWaitedMicros ? queueMicros :
            (std::min)(queueMicros, queuedMicros + kBlockMicros);
        lastWriteEnd = writeEnd;
      }
      else
      {
        g_outputReady.store(false, std::memory_order_release);
        const auto failures = g_writeFailures.fetch_add(1, std::memory_order_relaxed) + 1;
        if ((failures % 64) == 1) ESP_LOGE(kLogTag, "ES8311 write failed (%d), failures=%" PRIu32, result, failures);
        (void)esp_codec_dev_set_out_mute(speaker, true);
        (void)esp_codec_dev_close(speaker);
        retryAt = writeEnd + kRetryMicros;
        lastWriteEnd = 0;
        queuedMicros = 0;
      }
    }
    if (esp_timer_get_time() >= logAt)
    {
      // The BSP hides its I2S handle/callbacks. Queue starvation below is a timing
      // estimate, not a hardware interrupt counter; log that distinction explicitly.
      ESP_LOGI(kLogTag, "audio blocks=%" PRIu32 " written=%" PRIu32 " nonzero=%" PRIu32
               " max_render=%" PRIu32 "us overruns=%" PRIu32 " estimated_underruns=%" PRIu32 " failures=%" PRIu32,
               g_blocks.load(), g_writtenBlocks.load(), g_nonSilentBlocks.load(), g_longestRenderMicros.load(),
               g_overruns.load(), g_underruns.load(), g_writeFailures.load());
      logAt = esp_timer_get_time() + kLogIntervalMicros;
    }
  }
}

}  // namespace

namespace mosaico::audio {

bool Start()
{
  std::lock_guard<std::mutex> lock(g_startMutex);
  if (g_task != nullptr) return true;
  BaseType_t result;
#if portNUM_PROCESSORS > 1
  result = xTaskCreatePinnedToCore(AudioTask, "synth_audio", kTaskStackBytes, nullptr, kTaskPriority, &g_task, kTaskCore);
#else
  result = xTaskCreate(AudioTask, "synth_audio", kTaskStackBytes, nullptr, kTaskPriority, &g_task);
#endif
  if (result == pdPASS) return true;
  g_task = nullptr;
  ESP_LOGE(kLogTag, "unable to create audio task");
  return false;
}

bool Ready() { return g_outputReady.load(std::memory_order_acquire); }
void SetMuted(bool muted) { g_muted.store(muted, std::memory_order_relaxed); }

Diagnostics GetDiagnostics()
{
  return {Ready(), g_writtenBlocks.load(), g_nonSilentBlocks.load(), g_writeFailures.load(), g_peak.load(), g_queueFrames.load()};
}

}  // namespace mosaico::audio

namespace Device::Audio {

void SetOutputVolume(std::uint8_t percent) { g_outputVolume.store((std::min)(percent, std::uint8_t{100}), std::memory_order_relaxed); }
std::uint8_t OutputVolume() { return g_outputVolume.load(std::memory_order_relaxed); }
InputStreamInfo InputInfo() { return {}; }
InputStatus StartInput() { return InputStatus::NotReady; }
void StopInput() {}
InputStatus ReadInputFrames(std::int16_t* destination, std::size_t frameCount, std::size_t& framesRead, std::uint32_t)
{
  framesRead = 0;
  return destination == nullptr && frameCount != 0 ? InputStatus::InvalidArgument : InputStatus::NotReady;
}
std::size_t ReadComputerAudio(std::int16_t*, std::size_t) { return 0; }
RenderTiming Timing()
{
  return {g_blocks.load(), g_renderMicros.load(), g_overruns.load(), g_underruns.load(), g_longestRenderMicros.load()};
}

}  // namespace Device::Audio

namespace device {

void SetSynthAudioRenderProvider(SynthAudioRenderProvider provider)
{
  if (provider.sampleRate != kSampleRate)
  {
    ESP_LOGE(kLogTag, "unsupported render sample rate: %" PRIu32 " (expected %" PRIu32 ")", provider.sampleRate, kSampleRate);
    g_renderCallback.store(nullptr, std::memory_order_release);
    return;
  }
  g_renderCallback.store(provider.renderFrames, std::memory_order_release);
}

SynthAudioPipelineBinding ConnectSynthAudioPipeline(std::size_t engineIndex)
{
  SynthAudioPipelineBinding binding;
  binding.engineIndex = engineIndex;
  binding.backendName = "mosaico-bsp-es8311";
  binding.hasNativeOutput = mosaico::audio::Start();
  binding.connected = binding.hasNativeOutput && mosaico::audio::Ready();
  return binding;
}

}  // namespace device
