#include "Applications/Synth/Runtime/Sampler/NativeSamplerJobs.h"

#if defined(CARDENZA_NATIVE_UI)

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <thread>
#include <utility>

#include "Applications/Synth/Runtime/Sampler/SamplerWav.h"
#include "Devices/Device.h"
#include "Library/Synth/Core/SynthStorage.h"

namespace synth::native {

struct SampleSaveJob {
  std::string path;
  audio::SharedSampleBuffer source;
  std::size_t firstFrame = 0;
  std::size_t frameCount = 0;
  std::uint32_t sampleRate = 44100;
  bool overwrite = false;
  std::atomic<std::uint32_t> completedFrames{0};
  std::atomic<bool> done{false};
  bool saved = false;  // Published together with frames by done's release store.
  audio::SharedSampleBuffer frames;
};

struct SampleWaveJob {
  audio::SharedSampleBuffer source;
  std::atomic<bool> done{false};
  bool ready = false;
  bool started = false; // waveMutex, prevents a second host worker claiming this job.
  std::array<float, kSamplerWaveformBinCount> waveform{};
};

namespace {
std::mutex pendingMutex;
std::shared_ptr<SampleSaveJob> pending;
std::mutex waveMutex;
std::array<std::shared_ptr<SampleWaveJob>, kSynthTrackCount> waveJobs;

bool FileExists(const std::string& path)
{
  std::ifstream file(path, std::ios::binary);
  return file.good();
}

bool WriteSample(const std::shared_ptr<SampleSaveJob>& job)
{
  if (!storage::EnsureLayout() || !job->source || job->frameCount == 0 ||
      job->firstFrame > job->source->frameCount || job->frameCount > job->source->frameCount - job->firstFrame) return false;
  const std::string temporary = job->path + ".tmp";
  const std::string backup = job->path + ".bak";
  if (FileExists(backup) || (!job->overwrite && FileExists(job->path))) return false;
  {
    // The target is written forward from its final header. Rewinding an existing
    // LittleFS file would copy every unchanged PCM byte after the header.
    std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
    sampler::WriteMono16WavHeader(file, job->frameCount, job->sampleRate);
    std::array<std::int16_t, 512> chunk{};
    bool sourceRead = true;
    for (std::size_t offset = 0; file && offset < job->frameCount;)
    {
      const auto count = std::min(chunk.size(), job->frameCount - offset);
      if (job->source->stream)
      {
        if (!job->source->stream->ReadFrames(job->firstFrame + offset, chunk.data(), count)) { sourceRead = false; break; }
      }
      else std::copy_n(job->source->frames.get() + job->firstFrame + offset, count, chunk.data());
      file.write(reinterpret_cast<const char*>(chunk.data()), static_cast<std::streamsize>(count * sizeof(std::int16_t)));
      offset += count;
      job->completedFrames.store(static_cast<std::uint32_t>(offset), std::memory_order_relaxed);
      // The low-priority background task must leave the UI and management idle
      // time even when NAND accepts writes without blocking this iteration.
      if (offset % (8 * chunk.size()) == 0) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    file.flush();
    file.close();
    if (!sourceRead || !file) { (void)std::remove(temporary.c_str()); return false; }
  }
  {
    // Only the short replacement transaction owns the sample-file guard; the
    // PCM copy above must not lock out every other stream for its whole length.
    audio::SampleFileGuard guard(job->path);
    const bool original = FileExists(job->path);
    if (original && (!job->overwrite || std::rename(job->path.c_str(), backup.c_str()) != 0))
    {
      (void)std::remove(temporary.c_str());
      return false;
    }
    if (std::rename(temporary.c_str(), job->path.c_str()) != 0)
    {
      if (original) (void)std::rename(backup.c_str(), job->path.c_str());
      (void)std::remove(temporary.c_str());
      return false;
    }
    if (original) (void)std::remove(backup.c_str());
  }
  auto frames = std::make_shared<audio::SampleBuffer>();
  frames->frameCount = job->frameCount;
  frames->stream = audio::SampleStream::Open(job->path, sampler::kWavHeaderBytes, job->frameCount);
  if (!frames->stream) return false;
  job->frames = std::move(frames);
  return true;
}

void SaveWorker()
{
  std::shared_ptr<SampleSaveJob> job;
  {
    std::lock_guard<std::mutex> lock(pendingMutex);
    job = pending;
    if (!job || job->done.load(std::memory_order_acquire)) return;
  }
  const auto started = std::chrono::steady_clock::now();
  job->saved = WriteSample(job);
  job->source.reset();
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
  std::printf("sampler: native background save frames=%u elapsed=%lldms ok=%d\n",
              static_cast<unsigned>(job->frameCount), static_cast<long long>(elapsed), job->saved ? 1 : 0);
  job->done.store(true, std::memory_order_release);
}

void WaveWorker()
{
  for (;;)
  {
    std::shared_ptr<SampleWaveJob> job;
    {
      std::lock_guard<std::mutex> lock(waveMutex);
      for (auto& candidate : waveJobs)
      {
        if (candidate && !candidate->started && !candidate->done.load(std::memory_order_acquire))
        {
          candidate->started = true;
          job = candidate;
          break;
        }
      }
    }
    if (!job) return;
    bool ready = true;
    std::array<std::int16_t, 512> chunk{};
    std::array<std::uint16_t, kSamplerWaveformBinCount> peaks{};
    const std::size_t total = job->source->frameCount;
    for (std::size_t offset = 0; offset < total;)
    {
      const std::size_t count = std::min(chunk.size(), total - offset);
      if (!job->source->stream->ReadFrames(offset, chunk.data(), count)) { ready = false; break; }
      for (std::size_t index = 0; index < count; ++index)
      {
        const std::size_t bin = std::min((offset + index) * peaks.size() / total, peaks.size() - 1);
        peaks[bin] = std::max(peaks[bin], static_cast<std::uint16_t>(std::abs(static_cast<int>(chunk[index]))));
      }
      offset += count;
      if (offset % (8 * chunk.size()) == 0) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (ready)
      for (std::size_t index = 0; index < peaks.size(); ++index) job->waveform[index] = static_cast<float>(peaks[index]) / 32768.0f;
    job->ready = ready;
    job->source.reset();
    job->done.store(true, std::memory_order_release);
    {
      std::lock_guard<std::mutex> lock(waveMutex);
      for (auto& candidate : waveJobs) if (candidate == job) candidate.reset();
    }
  }
}
}

std::shared_ptr<SampleSaveJob> SaveSampleInBackground(std::string path,
    audio::SharedSampleBuffer source, std::size_t firstFrame, std::size_t frameCount,
    std::uint32_t sampleRate, bool overwrite)
{
  auto job = std::make_shared<SampleSaveJob>();
  job->path = std::move(path);
  job->source = std::move(source);
  job->firstFrame = firstFrame;
  job->frameCount = frameCount;
  job->sampleRate = sampleRate;
  job->overwrite = overwrite;
  {
    std::lock_guard<std::mutex> lock(pendingMutex);
    if (pending && !pending->done.load(std::memory_order_acquire)) return {};
    pending = job;
  }
  if (Device::Background::Run(SaveWorker)) return job;
#if !defined(ESP_PLATFORM)
  // The headless host has no background facade; keep the same async ownership
  // and publication contract so native UI tests exercise the actual save flow.
  std::thread(SaveWorker).detach();
  return job;
#else
  std::lock_guard<std::mutex> lock(pendingMutex);
  if (pending == job) pending.reset();
  return {};
#endif
}

SampleSaveResult PollSampleSave(const std::shared_ptr<SampleSaveJob>& job)
{
  if (!job) return {};
  SampleSaveResult result;
  result.progress = job->frameCount == 0 ? 0 : static_cast<float>(job->completedFrames.load(std::memory_order_relaxed)) / job->frameCount;
  result.done = job->done.load(std::memory_order_acquire);
  if (result.done)
  {
    result.saved = job->saved;
    result.frames = job->frames;
    std::lock_guard<std::mutex> lock(pendingMutex);
    if (pending == job) pending.reset();
  }
  return result;
}

std::shared_ptr<SampleWaveJob> BuildSampleWaveInBackground(audio::SharedSampleBuffer source)
{
  if (!source || !source->stream || source->frameCount == 0) return {};
  auto job = std::make_shared<SampleWaveJob>();
  job->source = std::move(source);
  bool queued = false;
  {
    std::lock_guard<std::mutex> lock(waveMutex);
    for (auto& candidate : waveJobs)
      if (!candidate) { candidate = job; queued = true; break; }
  }
  if (!queued) return {};
  if (Device::Background::Run(WaveWorker)) return job;
#if !defined(ESP_PLATFORM)
  std::thread(WaveWorker).detach();
  return job;
#else
  std::lock_guard<std::mutex> lock(waveMutex);
  for (auto& candidate : waveJobs) if (candidate == job) candidate.reset();
  return {};
#endif
}

SampleWaveResult PollSampleWave(const std::shared_ptr<SampleWaveJob>& job)
{
  SampleWaveResult result;
  if (!job) return result;
  result.done = job->done.load(std::memory_order_acquire);
  if (result.done)
  {
    result.ready = job->ready;
    result.waveform = job->waveform;
  }
  return result;
}

}  // namespace synth::native

#endif
