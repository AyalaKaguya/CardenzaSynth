#include "Library/Synth/Audio/SynthAudioRuntime.h"
#include "Library/Memory/Memory.h"
#include "Library/Synth/Audio/RenderLoad.h"
#include "Library/Synth/Core/Engines/Dx7/Dx7FactoryPresets.h"
#include "Library/Synth/Core/Engines/Juno/JunoFactoryPresets.h"
#include "Library/Synth/Core/Engines/Drums/DrumsSynthEngine.h"
#include "Library/Synth/Audio/Amy/FactoryParameters.h"
#include "Library/Synth/Audio/Fx/SynthFxDsp.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <limits>
#include <thread>
#include <tuple>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(_WIN32)
#include <unistd.h>
#endif

#if !defined(_WIN32)
#include <pthread.h>
#endif

#if defined(ESP_PLATFORM)
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#endif

#if defined(ESP_PLATFORM)
#include <driver/uart.h>
#endif

#if defined(ESP_PLATFORM)
#include <soc/uart_reg.h>
#endif

#if defined(ESP_PLATFORM)
#include <esp_task.h>
#endif

namespace amy_embed {
#if defined(ESP_PLATFORM)
// New IDF/libstdc++ exposes the float classification overload in std only.
// AMY's C header is embedded in this namespace and calls isnan unqualified.
using std::isnan;
#endif
extern "C" {
#include "amy.h"
// AMY's voice table (patches.c): the first oscillator of every allocated voice.
extern uint16_t* voice_to_base_osc;
// Where oscillator state and the event pool's blocks go (AmyEmbeddedStubs.c, amy_config_t's hooks).
void* cardenza_amy_osc_malloc(uint32_t size);
void* cardenza_amy_pool_malloc(uint32_t size);

// What AMY does to an oscillator that has gone silent (amy.c).
void reset_modosc(struct mod_synthinfo* pmsynth);
void reset_osc_state(struct synthinfo* psynth);
// The block being rendered, per core and bus (amy.c; one bus here, EnsureAmyInitialized).
extern SAMPLE** fbl[AMY_MAX_CORES];
extern SAMPLE core_max[AMY_MAX_CORES];
// One oscillator's block, as amy_render does it for each, per core and bus (amy.c).
extern SAMPLE** per_osc_fb[AMY_MAX_CORES];
SAMPLE render_osc_wave(uint16_t osc, uint8_t core, SAMPLE* buf);
void mix_with_pan(SAMPLE* stereo_dest, SAMPLE* mono_src, float pan_start, float pan_end, float level);
}
}  // namespace amy_embed

using amy_embed::amy_add_event;
using amy_embed::amy_config_t;
using amy_embed::amy_default_config;
using amy_embed::amy_default_event;
using amy_embed::amy_event;
using amy_embed::amy_simple_fill_buffer;
using amy_embed::amy_start;
using amy_embed::amy_stop;
using amy_embed::amy_update;
using amy_embed::output_sample_type;
using amy_embed::s8_23;  // what AMY's SAMPLE stands for

namespace synth::audio {

namespace {

constexpr std::size_t kAmyFramesPerBlock = AMY_BLOCK_SIZE;
constexpr std::size_t kAmyChannelCount = AMY_NCHANS;
constexpr std::size_t kSamplerVoiceCount = 4;  // then a new note takes a voice, a let-go one first
constexpr int kAmyMaxSynths = 8;
// Four tracks of the most voices (VoiceCountFor), and oscillators for four Juno tracks of them (6 a
// voice, 144) with room to spare: AMY gives each voice a contiguous run, and engines switched back and
// forth leave gaps. Oscillator state is allocated on first use (internal RAM, then PSRAM).
constexpr int kAmyMaxVoices = 24;
constexpr int kAmyMaxOscs = 192;
// The output stage, the same on every path: the mix times kOutputGain, held under the ceiling by a
// limiter that sees each block before it plays. Mix 1.0 is where AMY's own soft clip sits, so the
// tracks have about 9 dB above the ceiling before that clip could colour them.
constexpr float kOutputGain = 3.0f;
constexpr float kOutputCeiling = 0.97f;
// Recovers about 30 dB per second, slow enough not to follow the waveform of a bass note.
constexpr float kLimiterReleasePerFrame = 0.00008f;
constexpr std::size_t kLimiterShortestAttackFrames = 4;
constexpr float kSamplerVoiceGain = 0.3f;
constexpr float kDx7MaxAmyVelocity = 88.0f / 127.0f;
constexpr float kTwoPi = 6.28318530717958647692f;

struct SamplerParams {
  int sampleStart = 0;
  int sampleEnd = 127;
  int sampleLoop = 0;
  int sampleTune = 0;
  int sampleRoot = 60;
  int sampleGain = 96;
  int attack = 4;
  int release = 18;
  int cutoff = 96;
  int resonance = 18;
  int drive = 0;
  int level = 96;
  float filterG = 0.5f;
  float filterDamping = 1.0f;
  float filterInverse = 1.0f / 1.5f;
  // Worked out when something changes, not per sample (UpdateSamplerDerived).
  std::uint32_t revision = 1;
  std::uint32_t derivedRevision = 0;
  float attackStep = 0.0f;
  float releaseStep = 0.0f;
  float outputGain = 0.0f;
  float driveGain = 1.0f;
  std::uint64_t startCursor = 0;
  std::uint64_t endCursor = 0;
  SharedSampleBuffer sampleFrames;
  std::uint32_t sampleRate = kSampleRate;
};

struct SamplerVoice {
  bool active = false;
  bool releasing = false;
  std::uint8_t note = 0;
  float velocity = 0.0f;
  float envelope = 0.0f;
  // Frames in 32.32 fixed point: this core has no double-precision unit.
  std::uint64_t sampleCursor = 0;
  std::uint64_t cursorStep = 0;
  std::uint32_t revision = 0;
  StreamCursor stream{};
  float filterLow = 0.0f;
  float filterBand = 0.0f;
  std::uint64_t serial = 0;
};

struct EngineState {
  SynthEngineKind kind = SynthEngineKind::None;
  bool configured = false;
  int synthId = 0;
  int patchNumber = -1;
  float presetGain = 1.0f;  // Evens out the factory presets (PresetLevels.inc).
  bool orphanVoiceSeen = false;
  int noteOffset = 0;
  std::array<std::uint8_t, 156> dx7{};
  std::array<std::uint8_t, 18> juno{};
  std::array<bool, 128> activeNotes{};
  std::array<int, 8> drumLevels{{100, 100, 100, 100, 100, 100, 100, 100}};
  int drumMaster = 100;
  SamplerParams sampler{};
  std::array<SamplerVoice, kSamplerVoiceCount> samplerVoices{};
  std::uint64_t nextSamplerVoiceSerial = 1;
};

enum class PendingEventType {
  EnsureEngine,
  SetPreset,
  SetParameter,
  SetSamplerFrames,
  NoteOn,
  NoteOff,
  AllNotesOff,
  SetTrackGain,
  SetTrackMuted,
  SetMasterFx,
};

struct PendingEvent {
  PendingEventType type = PendingEventType::EnsureEngine;
  std::size_t engineIndex = 0;
  SynthEngineKind kind = SynthEngineKind::None;
  std::uint8_t note = 0;
  std::uint8_t velocity = 0;
  int value = 0;
  std::string text;
  SharedSampleBuffer frames;
  std::uint32_t sampleRate = kSampleRate;
  float gain = 1.0f;
};

// Track mixer. AMY renders every voice's root oscillator (with its chained or FM-operator
// oscillators) into a private buffer, decides from that buffer whether the voice has fallen
// silent, and then offers it to amy_external_render_hook before mixing it into the shared output.
// The hook scales each buffer by its track's gain there: post-filter and post-envelope, so the
// gain composes with every engine level (patch levels, Juno vca.level, Drums master.level) and
// survives preset loads, and it never feeds AMY's silence detection, so notes held through a
// mute or a closed fader are still playing when the gain comes back. At unity the hook leaves
// the buffer untouched, which keeps the output bit-identical to rendering without the mixer.
constexpr std::int32_t kTrackGainUnityQ16 = 1 << 16;
// Applied gain moves by at most 0.25 per AMY block, interpolated per frame: a full 0 <-> 1
// swing takes four blocks (~23 ms).
constexpr float kTrackGainStepPerBlock = 0.25f;
constexpr float kTrackGainStepPerFrame = kTrackGainStepPerBlock / static_cast<float>(kAmyFramesPerBlock);
constexpr float kMeterDecaySeconds = 0.3f;
constexpr float kMeterFloor = 0.0001f;
constexpr float kActivityHeldFraction = 0.5f;

// Render-thread mixer state, applied while draining events and rendering.
struct TrackMixState {
  float gain = 1.0f;
  bool muted = false;
  float amyGain = 1.0f;     // Gain ramping towards the target on the AMY voices.
  std::int32_t amyBlockStartQ16 = kTrackGainUnityQ16;  // Gain at the first and last frame of
  std::int32_t amyBlockEndQ16 = kTrackGainUnityQ16;    // the AMY block being rendered.
  float samplerGain = 1.0f; // Gain ramping towards the target on the sampler voices.
  float activity = 0.0f;
  float heldLevel = 0.0f;   // Velocity-normalized level of the latest note-on.
};

// UI-visible mixer state: requested values plus the published meters.
struct PublishedTrackMix {
  std::atomic<float> gain{1.0f};
  std::atomic<bool> muted{false};
  std::atomic<float> activity{0.0f};
};

std::once_flag g_amyInitFlag;
bool g_amyInitialized = false;
std::array<EngineState, kSynthTrackCount> g_engines{};
std::array<std::int16_t, kAmyFramesPerBlock * kAmyChannelCount> g_amyBlock{};
std::size_t g_amyBlockCursor = kAmyFramesPerBlock;
// In PSRAM: when the render thread falls behind the queue grows, and from internal RAM it took the
// last of it (some 30 KB of waiting notes in the stress test's "worst", 2026-11-04), after which AMY
// could not set up an engine and the board restarted.
std::deque<PendingEvent, memory::SlowAllocator<PendingEvent>> g_pendingEvents;
std::mutex g_pendingEventMutex;
std::mutex g_renderMutex;
std::array<TrackMixState, kSynthTrackCount> g_trackMix{};
// Owning track + 1 of every AMY oscillator (0: none), rebuilt whenever AMY allocates or
// releases voices.
std::array<std::uint8_t, kAmyMaxOscs> g_amyOscTrack{};
// The first oscillator of each voice. It holds the voice's filter and envelope and renders the others
// through them: any of those rendered on its own plays raw, whatever the patch says.
std::array<bool, kAmyMaxOscs> g_amyVoiceStart{};
std::array<PublishedTrackMix, kSynthTrackCount> g_publishedMix;
float g_outputPeakState = 0.0f;
std::atomic<float> g_outputPeak{0.0f};

// Effects (SynthFx.h, DSP in Fx/SynthFxDsp.h). An AMY track with an active insert or send leaves
// AMY's shared mix: the render hook adds its voices, after the track gain ramp, into a private
// float bus (cheaper on the ESP32-S3 than AMY's own stereo mix) that runs through the track's
// insert chain and sends right after each AMY block. Without memory for its bus, a track that
// only sends stays in AMY's mix and the hook feeds the sends from it directly. Sampler tracks with
// effects render into their bus per chunk. The unrouted AMY output, routed tracks and the bus
// returns then meet in a float mix, go through the master chain and the output soft clipper.
// Until effects are used, and again once nothing is routed, the master chain is neutral and the
// bus tails have died out, rendering takes exactly the paths above, bit for bit.
//
// Memory is allocated on the render thread when a setting first needs it and is then kept:
// FxCore (~4.6 KB) on first use, 1 KB per track with effects, 1 KB per chorus, 16 KB for the delay
// line on the first delay send, 4.75 KB for the reverb on the first reverb send.
#if defined(ESP_PLATFORM)
constexpr float kAmyOutputShift = 0.5f;  // amy_fill_buffer drops one bit on ESP32.
#else
constexpr float kAmyOutputShift = 1.0f;
#endif
// AMY's interleaved output to the float mix, the same level on every platform.
constexpr float kAmyBlockToMix = 1.0f / 65536.0f / kAmyOutputShift;
static_assert(kAmyFramesPerBlock <= fx::dsp::kMaxBlockFrames, "effects process whole AMY blocks");

struct TrackFxParams {
  fx::dsp::TrackInsertSettings inserts{};
  int delaySend = 0;
  int reverbSend = 0;
};

struct FxCore {
  std::array<float, kAmyFramesPerBlock> mix{};  // Float mix of the current AMY block (or chunk).
  std::array<float, kAmyFramesPerBlock> delaySend{};
  std::array<float, kAmyFramesPerBlock> reverbSend{};
  std::array<fx::dsp::TrackInserts, kSynthTrackCount> inserts{};
  std::array<float, kSynthTrackCount> delayGain{};  // Send gains as applied, ramping per block.
  std::array<float, kSynthTrackCount> reverbGain{};
  fx::dsp::SendBuses buses;
  fx::dsp::MasterChain master;
};

// What the render hook does with one track's AMY voices during the block being rendered.
struct FxHookTrack {
  float* bus = nullptr;         // Inserts: the voices join this bus (mix units), not AMY's mix.
  float* delaySend = nullptr;   // Sends only: the voices stay in AMY's mix and also feed the sends.
  float* reverbSend = nullptr;
  float delayGain = 0.0f, delayStep = 0.0f;    // Per-frame send gains, AMY-to-mix scale included.
  float reverbGain = 0.0f, reverbStep = 0.0f;
};

constexpr std::array<int, fx::kMasterParamCount> DefaultMasterFx()
{
  std::array<int, fx::kMasterParamCount> values{};
  for (std::size_t index = 0; index < values.size(); ++index) values[index] = fx::kMasterParams[index].defaultValue;
  return values;
}

// Render-thread effects state.
struct FxState {
  std::array<TrackFxParams, kSynthTrackCount> tracks{};
  std::array<int, fx::kMasterParamCount> master = DefaultMasterFx();
  std::unique_ptr<FxCore> core;
  std::array<std::unique_ptr<float[]>, kSynthTrackCount> bus;  // Tracks with inserts (or sampler tracks with effects).
  std::array<bool, kSynthTrackCount> routed{};        // Any effect: the float path is needed.
  std::array<bool, kSynthTrackCount> insertRouted{};  // Through the track's bus and inserts.
  bool floatPath = false;
  // Output stage of the effects path: 0 = the AMY-only path's linear 0.75 gain, 1 = the soft clipper
  // of the sampler mix. It crossfades over one AMY block when the path starts or ends next to the
  // AMY-only path, so switching paths does not click.
  bool blockPrepared = false;   // core->mix holds the AMY block at g_amyBlockCursor.
  bool dirty = false;          // Settings changed: allocations may be due.
  float memoryTempo = 0.0f;    // The tempo the allocations were last checked for.
  std::array<float, fx::kBusCount> returnMeter{};
};

FxState g_fx;
// Render hook plan for the AMY block being rendered (all null outside the effects path).
std::array<FxHookTrack, kSynthTrackCount> g_fxHook{};
float g_outputGain = kOutputGain;  // The limiter's current gain.
float g_fxVoiceScale = 0.0f;  // AMY voice sample (S8.23) to mix units, for the block being rendered.

template <std::size_t... Index>
std::array<std::atomic<int>, sizeof...(Index)> MakePublishedMasterFx(std::index_sequence<Index...>)
{
  return {{std::atomic<int>{fx::kMasterParams[Index].defaultValue}...}};
}

// UI-visible effects state.
std::array<std::atomic<int>, fx::kMasterParamCount> g_publishedMasterFx = MakePublishedMasterFx(std::make_index_sequence<fx::kMasterParamCount>{});
std::atomic<float> g_fxTempo{120.0f};
std::array<std::atomic<float>, fx::kBusCount> g_fxReturnLevel{};
std::array<std::atomic<bool>, fx::kBusCount> g_fxBusFailed{};

struct AmyShutdown {
  ~AmyShutdown()
  {
    if (g_amyInitialized)
    {
      amy_stop();
    }
  }
};

AmyShutdown g_amyShutdown;

float Clamp01(float value)
{
  return std::clamp(value, 0.0f, 1.0f);
}

std::int16_t FloatToInt16(float sample)
{
  return static_cast<std::int16_t>(std::clamp(sample, -1.0f, 1.0f) * 32767.0f);
}

float Normalize(int value, int minimum, int maximum)
{
  if (maximum <= minimum)
  {
    return 0.0f;
  }
  return Clamp01(static_cast<float>(value - minimum) / static_cast<float>(maximum - minimum));
}

std::size_t SamplerStartFrame(const SamplerParams& params)
{
  if (!params.sampleFrames || params.sampleFrames->frameCount == 0)
  {
    return 0;
  }

  const std::size_t lastFrame = params.sampleFrames->frameCount - 1;
  return (std::min)(lastFrame,
                    static_cast<std::size_t>((static_cast<std::uint64_t>(lastFrame) * static_cast<std::uint64_t>(std::clamp(params.sampleStart, 0, 127))) /
                                             127u));
}

std::size_t SamplerEndFrame(const SamplerParams& params, std::size_t startFrame)
{
  if (!params.sampleFrames || params.sampleFrames->frameCount == 0)
  {
    return 0;
  }

  const std::size_t frameCount = params.sampleFrames->frameCount;
  const std::size_t endFrame = static_cast<std::size_t>((static_cast<std::uint64_t>(frameCount) * static_cast<std::uint64_t>(std::clamp(params.sampleEnd, 0, 127))) /
                                                        127u);
  return std::clamp(endFrame, startFrame + 1, frameCount);
}

bool SamplerHasSample(const SamplerParams& params)
{
  return params.sampleFrames && params.sampleFrames->frameCount != 0 && (params.sampleFrames->frames || params.sampleFrames->stream);
}

void UpdateSamplerFilter(SamplerParams& params)
{
  const float cutoff = 80.0f + std::pow(Normalize(params.cutoff, 0, 127), 2.2f) * 12000.0f;
  params.filterG = std::tan(0.5f * kTwoPi * cutoff / static_cast<float>(kSampleRate));
  params.filterDamping = 1.0f / (0.7071f + Normalize(params.resonance, 0, 127) * 7.0f);
  params.filterInverse = 1.0f / (1.0f + params.filterG * (params.filterG + params.filterDamping));
}

bool UsesAmy(SynthEngineKind kind)
{
  return kind == SynthEngineKind::Dx7 || kind == SynthEngineKind::Juno || kind == SynthEngineKind::Drums;
}

int VoiceCountFor(SynthEngineKind kind)
{
  switch (kind)
  {
    // When every voice of a track is busy a new note takes one, a let-go one first (AMY): fine by the
    // user (2026-11-04), and fewer voices are less to render. Raised to 6 / 8 / 8 for a day, which only
    // gave the budget more to fade.
    case SynthEngineKind::Dx7:
      return 4;
    case SynthEngineKind::Juno:
    case SynthEngineKind::Drums:
      return 6;
    default:
      return 0;
  }
}

int DefaultPatchFor(SynthEngineKind kind)
{
  switch (kind)
  {
    case SynthEngineKind::Dx7:
      return 138;
    case SynthEngineKind::Juno:
      return 32;
    default:
      return -1;
  }
}

int MapDx7Preset(std::string_view presetId)
{
  if (presetId == "dx7.piano1") return 135;
  if (presetId == "dx7.piano3") return 137;
  if (presetId == "dx7.epiano1") return 138;
  if (presetId == "dx7.brass1") return 128;
  if (presetId == "dx7.organ1") return 150;
  if (presetId == "dx7.bell") return 219;
  return 138;
}

int MapJunoPreset(std::string_view presetId)
{
  if (const JunoFactoryPresetDefinition* preset = FindJunoFactoryPreset(presetId)) return preset->patchNumber;
  if (presetId == "juno.saw") return 32;
  if (presetId == "juno.pwm") return 47;
  if (presetId == "juno.hoover") return 34;
  if (presetId == "juno.strings") return 64;
  if (presetId == "juno.brass") return 80;
  if (presetId == "juno.bass") return 31;
  if (presetId == "juno.pad") return 74;
  return 32;
}

// Called by AMY (on the rendering core) for each sounding voice block, before it is mixed.
// The buses and sends are shared by the voices of both cores. Each voice adds to them for microseconds;
// the other core never holds this while waiting for anything.
// Not std::atomic_flag: with PSRAM, ESP-IDF builds everything with -mdisable-hardware-atomics, and GCC
// has no library call for test_and_set, so it became a plain load and store. A release falling between
// the other core's load and store was overwritten with "taken" and both cores spun for ever: the device
// froze on PLAY (2026-11-04). exchange() is a library call there, atomic across the cores.
std::atomic<bool> g_fxBusLock{false};
class FxBusLock {
public:
  FxBusLock() { while (g_fxBusLock.exchange(true, std::memory_order_acquire)) {} }
  ~FxBusLock() { g_fxBusLock.store(false, std::memory_order_release); }
};

SYNTH_FX_HOT std::uint8_t ApplyTrackGainToAmyVoice(std::uint16_t osc, std::int32_t* buffer, std::uint16_t length)
{
  if (osc >= g_amyOscTrack.size() || g_amyOscTrack[osc] == 0)
  {
    return 0;
  }

  const std::size_t track = g_amyOscTrack[osc] - 1u;
  const TrackMixState& mix = g_trackMix[track];


  const std::int32_t startQ16 = mix.amyBlockStartQ16;
  const std::int32_t endQ16 = mix.amyBlockEndQ16;
  if (startQ16 == 0 && endQ16 == 0)
  {
    return 1;  // "Handled": AMY skips mixing this voice block.
  }
  const FxHookTrack& fx = g_fxHook[track];
  if (float* bus = fx.bus)
  {
    // Through the track's inserts: the voice joins the track bus instead of AMY's mix, with the
    // gain ramp applied in float.
    const float inverseLength = length == kAmyFramesPerBlock ? 1.0f / static_cast<float>(kAmyFramesPerBlock) : 1.0f / static_cast<float>(length);
    const float scale = g_fxVoiceScale * (1.0f / static_cast<float>(kTrackGainUnityQ16));
    float gain = static_cast<float>(startQ16) * scale;
    const FxBusLock lock;
    if (startQ16 == endQ16)
    {
      for (std::uint16_t index = 0; index < length; ++index) bus[index] += static_cast<float>(buffer[index]) * gain;
      return 1;
    }
    const float step = static_cast<float>(endQ16 - startQ16) * scale * inverseLength;
    for (std::uint16_t index = 0; index < length; ++index)
    {
      bus[index] += static_cast<float>(buffer[index]) * gain;
      gain += step;
    }
    return 1;
  }

  if (startQ16 == endQ16)
  {
    if (startQ16 != kTrackGainUnityQ16)
    {
      for (std::uint16_t index = 0; index < length; ++index)
      {
        buffer[index] = static_cast<std::int32_t>((static_cast<std::int64_t>(buffer[index]) * startQ16) >> 16);
      }
    }
  }
  else
  {
    const std::int32_t deltaQ16 = endQ16 - startQ16;
    for (std::uint16_t index = 0; index < length; ++index)
    {
      const std::int64_t gainQ16 = startQ16 + (deltaQ16 * static_cast<std::int32_t>(index)) / static_cast<std::int32_t>(length);
      buffer[index] = static_cast<std::int32_t>((static_cast<std::int64_t>(buffer[index]) * gainQ16) >> 16);
    }
  }
  // Sends only: the (post-fader) voice also feeds the delay and reverb sends.
  if (fx.delaySend != nullptr && fx.reverbSend != nullptr)
  {
    const FxBusLock lock;
    float delayGain = fx.delayGain, reverbGain = fx.reverbGain;
    if (fx.delayStep == 0.0f && fx.reverbStep == 0.0f)
    {
      for (std::uint16_t index = 0; index < length; ++index)
      {
        const float sample = static_cast<float>(buffer[index]);
        fx.delaySend[index] += sample * delayGain;
        fx.reverbSend[index] += sample * reverbGain;
      }
      return 0;
    }
    for (std::uint16_t index = 0; index < length; ++index)
    {
      const float sample = static_cast<float>(buffer[index]);
      fx.delaySend[index] += sample * delayGain;
      fx.reverbSend[index] += sample * reverbGain;
      delayGain += fx.delayStep;
      reverbGain += fx.reverbStep;
    }
  }
  else if (float* send = fx.delaySend != nullptr ? fx.delaySend : fx.reverbSend)
  {
    const FxBusLock lock;
    const bool delay = fx.delaySend != nullptr;
    float gain = delay ? fx.delayGain : fx.reverbGain;
    const float step = delay ? fx.delayStep : fx.reverbStep;
    if (step == 0.0f)
    {
      for (std::uint16_t index = 0; index < length; ++index) send[index] += static_cast<float>(buffer[index]) * gain;
      return 0;
    }
    for (std::uint16_t index = 0; index < length; ++index)
    {
      send[index] += static_cast<float>(buffer[index]) * gain;
      gain += step;
    }
  }
  return 0;
}

void EnsureAmyInitialized()
{
  std::call_once(g_amyInitFlag, []() {
    amy_config_t config = amy_default_config();
    config.audio = AMY_AUDIO_IS_NONE;
    config.midi = AMY_MIDI_IS_NONE;
    config.features.default_synths = 0;
    config.features.startup_bleep = 0;
    config.features.audio_in = 0;
    config.features.custom = 0;
    config.features.partials = 0;
    config.features.reverb = 0;
    config.features.echo = 0;
    config.features.chorus = 0;
  #if defined(ESP_PLATFORM)
    // Not AMY's own tasks: RenderAmyVoicesNoLock shares the voices between the cores.
    config.platform.multicore = 0;
    config.platform.multithread = 0;
  #else
    config.platform.multicore = 0;
    config.platform.multithread = 0;
  #endif
    // CARDENZA sequences in SynthSequencer; AMY's own tag table would only cost internal RAM.
    config.max_sequencer_tags = 1;
  #if defined(ESP_PLATFORM)
    // The render and mix blocks and AMY's tables stay in internal RAM (ram_caps_synth is AMY's name for
    // the oscillator tables). Each oscillator's state and the event pool's blocks do not use these caps:
    // they go through our allocators (internal RAM while there is room, PSRAM after,
    // AmyEmbeddedStubs.c); an oscillator's state is given back when its voice is released.
    constexpr std::uint32_t kAmyFastCaps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    config.ram_caps_synth = kAmyFastCaps;
    config.ram_caps_block = kAmyFastCaps;
    config.ram_caps_fbl = kAmyFastCaps;
    config.ram_caps_events = kAmyFastCaps;
    config.ram_caps_oscs = kAmyFastCaps;
  #endif
    config.amy_external_osc_alloc_hook = amy_embed::cardenza_amy_osc_malloc;
    config.amy_external_delta_alloc_hook = amy_embed::cardenza_amy_pool_malloc;
    config.max_synths = kAmyMaxSynths;
    config.max_voices = kAmyMaxVoices;
    config.max_oscs = kAmyMaxOscs;
    // The tracks' effects are ours (SynthFx): AMY mixes every voice into one stereo bus.
    config.max_buses = 1;
    config.amy_external_render_hook = ApplyTrackGainToAmyVoice;
    amy_start(config);
    g_amyInitialized = true;
  });
}

// What was given up, by what it was: a let-go tail (hardly heard), a note still held, a note left out
// before it was heard, a held note whose voice a newer note took (the track has no free voice).
std::atomic<std::uint32_t> g_lostTails{0};
std::atomic<std::uint32_t> g_lostHeld{0};
std::atomic<std::uint32_t> g_lostUnheard{0};
std::atomic<std::uint32_t> g_lostStolen{0};

// A voice whose key was let go: AMY's note-off sets the note-off clock and unsets the note-on clock
// (amy.c play_delta, algo_note_off). Comparing the two clocks instead never found one (the note-on clock
// unset is the largest value), so from 1e9ce9b on the budget faded held notes and never a tail: notes a
// listener missed (2026-11-04).
bool VoiceReleased(const amy_embed::synthinfo* voice)
{
  constexpr std::uint32_t kUnset = std::numeric_limits<std::uint32_t>::max();
  return voice->note_off_clock != kUnset && (voice->note_on_clock == kUnset || voice->note_off_clock >= voice->note_on_clock);
}

void SendAmyNoteEventNoLock(const EngineState& engine, std::uint8_t note, float velocity)
{
  if (!g_amyInitialized)
  {
    return;
  }

  // AMY has no note 0: its note-on is ignored, and its note-off wipes the voice table without releasing
  // a voice, which leaves everything sounding on the track stuck for ever.
  const int amyNote = (std::min)(static_cast<int>(note) + engine.noteOffset, 127);
  if (amyNote <= 0)
  {
    return;
  }

  if (velocity > 0.0f && engine.kind != SynthEngineKind::Drums)
  {
    // Every voice of the track holding a note: AMY gives this one the voice of a held note.
    std::array<std::uint16_t, MAX_VOICES_PER_INSTRUMENT> voices{};
    const int count = amy_embed::instrument_get_num_voices(engine.synthId, voices.data());
    int held = 0;
    for (int index = 0; index < count; ++index)
    {
      const std::uint16_t base = amy_embed::voice_to_base_osc[voices[static_cast<std::size_t>(index)]];
      const auto* voice = base < kAmyMaxOscs ? amy_embed::synth[base] : nullptr;
      if (voice != nullptr && voice->status == SYNTH_AUDIBLE && !VoiceReleased(voice)) ++held;
    }
    if (count > 0 && held >= count) g_lostStolen.fetch_add(1, std::memory_order_relaxed);
  }
  amy_event event = amy_default_event();
  event.synth = static_cast<uint16_t>(engine.synthId);
  event.midi_note = static_cast<uint16_t>(amyNote);
  event.velocity = velocity;
  if (engine.kind == SynthEngineKind::Drums)
  {
    // AMY no longer maps GM drum notes to its tiny PCM kit (_SYNTH_FLAGS_MIDI_DRUMS is gone): each pad
    // names the sample its GM note had there (808 kick, snare, closed and open hat, clap, tom, cowbell,
    // maraca) at the sample's own pitch, which is also the note its release finds.
    constexpr std::array<std::uint16_t, kDrumNotes.size()> kPcmPresets{{1, 2, 6, 7, 9, 8, 10, 0}};
    const auto pad = std::find(kDrumNotes.begin(), kDrumNotes.end(), note);
    if (pad == kDrumNotes.end()) return;
    event.preset = kPcmPresets[static_cast<std::size_t>(pad - kDrumNotes.begin())];
    event.midi_note = amy_embed::pcm_map[event.preset].midinote;
    event.wave = PCM;
    event.feedback = 0;
  }
  amy_add_event(&event);
}

void ReleaseAllAmyNotesNoLock(EngineState& engine)
{
  if (engine.kind == SynthEngineKind::Drums)
  {
    // Drum key releases preserve one-shot tails. Explicit stop must still reach
    // every allocated voice, including sounds whose performance key was released.
    if (std::any_of(engine.activeNotes.begin(), engine.activeNotes.end(), [](bool active) { return active; }))
    {
      auto event = amy_default_event();
      event.synth = static_cast<std::uint8_t>(engine.synthId);
      event.velocity = 0;
      amy_add_event(&event);
    }
    engine.activeNotes.fill(false);
    return;
  }
  for (std::size_t note = 0; note < engine.activeNotes.size(); ++note)
  {
    if (!engine.activeNotes[note])
    {
      continue;
    }

    SendAmyNoteEventNoLock(engine, static_cast<std::uint8_t>(note), 0.0f);
    engine.activeNotes[note] = false;
  }
}

void ReleaseSamplerNoteNoLock(EngineState& engine, std::uint8_t note)
{
  for (SamplerVoice& voice : engine.samplerVoices)
  {
    if (voice.active && voice.note == note)
    {
      voice.releasing = true;
    }
  }
}

amy_event SynthEvent(const EngineState& engine, int oscillator)
{
  amy_event event = amy_default_event();
  event.synth = static_cast<std::uint8_t>(engine.synthId);
  event.osc = static_cast<std::uint16_t>(oscillator);
  return event;
}

void ApplyJunoParameterNoLock(EngineState& engine, std::string_view id, int value)
{
  const int offset = factory::JunoOffset(id);
  const auto setFlag = [&](std::size_t byte, unsigned flag) {
    engine.juno[byte] = static_cast<std::uint8_t>((engine.juno[byte] & ~flag) | (value ? flag : 0u));
  };
  if (offset >= 0) engine.juno[offset] = static_cast<std::uint8_t>(std::clamp(value, 0, 127));
  else if (id == "dco.saw") setFlag(16, 16u);
  else if (id == "dco.pulse") setFlag(16, 8u);
  else if (id == "dco.range") engine.juno[16] = (engine.juno[16] & ~7u) | (1u << std::clamp(value, 0, 2));
  else if (id == "dco.pwm_mode") setFlag(17, 1u);
  else if (id == "vcf.polarity") setFlag(17, 2u);
  else if (id == "vca.mode") setFlag(17, 4u);
  else return;
  const auto& p = engine.juno;
  const auto normalized = [&](int i) { return p[i] / 127.0f; };
  const std::string_view group = id.substr(0, 4);
  // vca.mode swaps the envelope that drives the VCF (gate mode: ENV -> VCF, gate -> VCA).
  const bool vcaMode = id == "vca.mode";
  if (group == "lfo.")
  {
    auto event = SynthEvent(engine, 1);
    event.freq_coefs[amy_embed::COEF_CONST] = 0.6f * std::exp2(0.04f * p[0]) - 0.1f;
    event.eg0_times[0] = static_cast<std::uint32_t>(18.0f * std::exp2(0.066f * p[1]) - 13.0f);
    event.eg0_values[0] = 1;
    event.eg0_times[1] = 10000;
    event.eg0_values[1] = 0;
    amy_add_event(&event);
    return;
  }
  if (group == "dco.")
  {
    const float baseFrequency = static_cast<float>(ZERO_LOGFREQ_IN_HZ) * ((p[16] & 1) ? 0.5f : ((p[16] & 4) ? 2.0f : 1.0f));
    for (int osc = 2; osc <= 5; ++osc)
    {
      auto event = SynthEvent(engine, osc);
      event.amp_coefs[amy_embed::COEF_CONST] = osc == 2 ? float((p[16] & 8) != 0) :
          osc == 3 ? float((p[16] & 16) != 0) : normalized(osc == 4 ? 15 : 4);
      if (osc < 5)
      {
        event.freq_coefs[amy_embed::COEF_CONST] = baseFrequency * (osc == 4 ? 0.5f : 1.0f);
        // DCO LFO depth (amy/juno.py _freq_coef_string).
        event.freq_coefs[amy_embed::COEF_MOD] = 0.03f * normalized(2);
      }
      if (osc == 2)
      {
        const bool manual = (p[17] & 1) != 0;
        event.duty_coefs[amy_embed::COEF_CONST] = 0.5f + (manual ? 0.5f * normalized(3) : 0.0f);
        event.duty_coefs[amy_embed::COEF_MOD] = manual ? 0.0f : 0.5f * normalized(3);
      }
      amy_add_event(&event);
    }
    return;
  }
  if (group == "vcf." || vcaMode)
  {
    auto event = SynthEvent(engine, 0);
    // AMY runs one biquad twice for 24 dB, which squares its peak: at the ROM's 0.7 * 2^(4 x) a full
    // resonance is +42 dB (it overflows to silence, and a lead at 3/4 whistles 30 dB over its own
    // note). This law peaks at +20 dB, a Juno's ladder more than the ROM's arithmetic.
    event.resonance = 0.7f * std::exp2(2.2f * normalized(6));
    event.filter_freq_coefs[amy_embed::COEF_CONST] = 13.0f * std::exp2(0.0938f * p[5] + 0.75f * normalized(9));
    event.filter_freq_coefs[amy_embed::COEF_NOTE] = normalized(9);
    const float amount = 11.0f * ((p[17] & 2) ? -1.0f : 1.0f) * normalized(7);
    event.filter_freq_coefs[amy_embed::COEF_EG0] = (p[17] & 4) ? 0.0f : amount;
    event.filter_freq_coefs[amy_embed::COEF_EG1] = (p[17] & 4) ? amount : 0.0f;
    event.filter_freq_coefs[amy_embed::COEF_MOD] = 1.25f * normalized(8);
    amy_add_event(&event);
  }
  if (id == "vca.level" || group == "env." || vcaMode)
  {
    auto event = SynthEvent(engine, 0);
    event.amp_coefs[amy_embed::COEF_CONST] = normalized(10);
    // Gate mode (amy/juno.py update_env): the VCA follows a gate on envelope 0 and the ADSR moves
    // to envelope 1, where only the VCF reads it.
    const bool gate = (p[17] & 4) != 0;
    auto* times = gate ? event.eg1_times : event.eg0_times;
    auto* levels = gate ? event.eg1_values : event.eg0_values;
    times[0] = 6 + 8 * p[11]; levels[0] = 1;
    times[1] = static_cast<std::uint32_t>(80 * std::exp2(0.085f * p[12]) - 80); levels[1] = normalized(13);
    times[2] = static_cast<std::uint32_t>(70 * std::exp2(0.066f * p[14]) - 70); levels[2] = 0;
    if (gate)
    {
      event.eg0_times[0] = 0; event.eg0_values[0] = 1;
      event.eg0_times[1] = 0; event.eg0_values[1] = 1;
      event.eg0_times[2] = 0; event.eg0_values[2] = 0;
    }
    amy_add_event(&event);
  }
}

// A Juno patch from our own bytes, over AMY's ROM string: the same mapping except the resonance law
// above, and it lets a factory patch differ from the ROM.
void VoiceJunoPatchNoLock(EngineState& engine)
{
  const auto& p = engine.juno;
  ApplyJunoParameterNoLock(engine, "lfo.rate", p[0]);
  ApplyJunoParameterNoLock(engine, "dco.lfo", p[2]);
  ApplyJunoParameterNoLock(engine, "vcf.resonance", p[6]);
  ApplyJunoParameterNoLock(engine, "vca.level", p[10]);
}

float Dx7Level(int value)
{
  return std::exp2((value - 99.0f) / 8.0f);
}

void Dx7OperatorEnvelope(amy_event& event, const std::uint8_t* op)
{
  int current = op[7];
  event.eg0_times[0] = 0;
  event.eg0_values[0] = Dx7Level(current);
  for (int i = 0; i < 4; ++i)
  {
    const int target = op[4 + i];
    float seconds;
    if (target > current)
    {
      const float timeConstant = 0.008f * std::exp2((65.0f - op[i]) / 6.0f);
      seconds = timeConstant * std::log((109.0f - (std::max)(34, current)) / (109.0f - (std::max)(34, target)));
    }
    else
    {
      const int difference = (i == 3 && target == current) ? 60 : current - target;
      seconds = difference / (0.5f + 0.5f * std::exp2(op[i] / 6.0f));
    }
    event.eg0_times[i + 1] = static_cast<std::uint32_t>((std::max)(0.0f, seconds * 1000.0f));
    event.eg0_values[i + 1] = Dx7Level(target);
    current = target;
  }
}

void ApplyDx7ParameterNoLock(EngineState& engine, std::string_view id, int value)
{
  const int offset = factory::Dx7Offset(id);
  if (offset < 0) return;
  if (id == "global.transpose")
  {
    if (engine.noteOffset != value) ReleaseAllAmyNotesNoLock(engine);
    engine.noteOffset = std::clamp(value, -24, 24);
    engine.dx7[offset] = static_cast<std::uint8_t>(engine.noteOffset + 24);
    return;
  }
  int raw = value;
  if (id == "global.algorithm") --raw;
  if (id.size() > 4 && id.substr(4) == "detune") raw += 7;
  int maximum = 99;
  if (id == "global.lfo_wave") maximum = 5;
  else if (id == "global.lfo_pms") maximum = 7;
  else if (offset < 126 && offset % 21 == 17) maximum = 1;  // Operator mode: 0 ratio, 1 fixed.
  engine.dx7[offset] = static_cast<std::uint8_t>(std::clamp(raw, 0, maximum));
  const auto& p = engine.dx7;
  if (id == "global.algorithm" || id == "global.feedback" || id == "global.lfo_pitch" || id == "global.lfo_pms")
  {
    auto event = SynthEvent(engine, 0);
    event.algorithm = static_cast<std::uint8_t>(p[134] + 1);
    event.feedback = 0.00125f * std::exp2(float(p[135]));
    event.freq_coefs[amy_embed::COEF_MOD] = p[143] == 0 ? 0.0f :
        0.6f * std::pow(1.7f, p[143] - 1.0f) * p[139] / 1188.0f;
    amy_add_event(&event);
  }
  else if (id == "global.lfo_speed")
  {
    const int speed = p[137];
    auto event = SynthEvent(engine, 1);
    event.freq_coefs[amy_embed::COEF_CONST] = speed == 0 ? 0.064f : speed <= 64 ? speed / 6.0f :
        speed <= 85 ? speed - 64.0f * 5.0f / 6.0f : 31.67f + (speed - 85.0f) * 1.33f;
    amy_add_event(&event);
  }
  else if (id == "global.lfo_wave")
  {
    // DX7 order TRI, SAW DOWN, SAW UP, SQUARE, SINE, S/H, as amy/fm.py lfo_wave.
    constexpr std::array<std::uint16_t, 6> kWaves{{TRIANGLE, SAW_DOWN, SAW_UP, PULSE, SINE, NOISE}};
    auto event = SynthEvent(engine, 1);
    event.wave = kWaves[p[142]];
    amy_add_event(&event);
  }
  else
  {
    const bool all = id == "global.lfo_amp";
    for (int index = 0; index < 6; ++index)
    {
      if (!all && offset / 21 != index) continue;
      const auto* op = p.data() + index * 21;
      auto event = SynthEvent(engine, index + 2);
      if (all || offset % 21 == 16 || offset % 21 == 14)
      {
        event.amp_coefs[amy_embed::COEF_CONST] = 2.0f * Dx7Level(op[16]);
        // AMY's DX7 converter supports modulation sensitivity as an on/off switch.
        event.amp_coefs[amy_embed::COEF_MOD] = op[14] ? Dx7Level(p[140]) : 0.0f;
      }
      else if (offset % 21 >= 17)
      {
        const float fine = op[19] + (op[20] - 7.0f) / 8.0f;
        if (op[17] == 0) event.ratio = (op[18] == 0 ? 0.5f : float(op[18])) * (1.0f + fine / 100.0f);
        else
        {
          event.freq_coefs[amy_embed::COEF_CONST] = std::pow(10.0f, (op[18] & 3) + fine / 100.0f);
          // A fixed operator must drop its ratio: AMY stores log2(ratio) and treats NaN as unset.
          event.ratio = -1.0f;
        }
      }
      else Dx7OperatorEnvelope(event, op);
      amy_add_event(&event);
    }
  }
}

TrackMixState& TrackMixFor(const EngineState& engine)
{
  return g_trackMix[static_cast<std::size_t>(&engine - g_engines.data())];
}

float EffectiveTrackGain(const TrackMixState& mix)
{
  return mix.muted ? 0.0f : mix.gain;
}

float ApproachGain(float current, float target, float step)
{
  return current < target ? (std::min)(target, current + step) : (std::max)(target, current - step);
}

std::int32_t GainToQ16(float gain)
{
  return static_cast<std::int32_t>(std::lround(gain * static_cast<float>(kTrackGainUnityQ16)));
}

// Runs before every AMY block: ramps each AMY track one step towards its effective gain.
void StepAmyTrackGainsNoLock()
{
  for (std::size_t index = 0; index < g_engines.size(); ++index)
  {
    const EngineState& engine = g_engines[index];
    TrackMixState& mix = g_trackMix[index];
    const float target = EffectiveTrackGain(mix);
    mix.amyGain = engine.configured && UsesAmy(engine.kind) ? ApproachGain(mix.amyGain, target, kTrackGainStepPerBlock) : target;
    mix.amyBlockStartQ16 = mix.amyBlockEndQ16;
    mix.amyBlockEndQ16 = GainToQ16(Clamp01(mix.amyGain) * engine.presetGain);
  }
}

void RebuildAmyOscTrackMapNoLock()
{
  g_amyOscTrack.fill(0);
  g_amyVoiceStart.fill(false);
  if (!g_amyInitialized)
  {
    return;
  }
  for (std::size_t track = 0; track < g_engines.size(); ++track)
  {
    const EngineState& engine = g_engines[track];
    if (!engine.configured || !UsesAmy(engine.kind))
    {
      continue;
    }

    std::array<std::uint16_t, MAX_VOICES_PER_INSTRUMENT> voices{};
    const int voiceCount = amy_embed::instrument_get_num_voices(engine.synthId, voices.data());
    if (voiceCount <= 0)
    {
      continue;
    }
    const int oscsPerVoice = amy_embed::instrument_get_oscs_per_voice(engine.synthId);
    for (int voice = 0; voice < voiceCount; ++voice)
    {
      const int base = amy_embed::voice_to_base_osc[voices[voice]];
      if (base < static_cast<int>(g_amyVoiceStart.size())) g_amyVoiceStart[static_cast<std::size_t>(base)] = true;
      for (int osc = base; osc < base + oscsPerVoice && osc < static_cast<int>(g_amyOscTrack.size()); ++osc)
      {
        g_amyOscTrack[static_cast<std::size_t>(osc)] = static_cast<std::uint8_t>(track + 1);
      }
    }
  }
}

void BumpTrackActivityNoLock(std::size_t engineIndex, float level)
{
  TrackMixState& mix = g_trackMix[engineIndex];
  mix.heldLevel = Clamp01(level);
  mix.activity = (std::max)(mix.activity, mix.heldLevel * EffectiveTrackGain(mix));
}

// In dB, by AMY patch number: Juno 0..127, DX7 128..255. The patches are decades apart in level (a
// resonant Juno lead against a soft DX7 pad is 40 dB), which no single output gain can even out.
constexpr std::int8_t kPresetLevels[256] = {
#include "Library/Synth/Audio/Amy/PresetLevels.inc"
};

float PresetGainFor(const EngineState& engine)
{
  if (engine.kind != SynthEngineKind::Juno && engine.kind != SynthEngineKind::Dx7) return 1.0f;
  const int patch = engine.patchNumber < 0 ? DefaultPatchFor(engine.kind) : engine.patchNumber;
  return std::pow(10.0f, static_cast<float>(kPresetLevels[std::clamp(patch, 0, 255)]) / 20.0f);
}

// AMY parses a ROM patch's text once for every voice it builds (each number in software double
// precision): 18-22 ms for an engine's voices on the board, which held up the audio for that long
// whenever a preset or an engine changed while playing (the stress test, 2026-11-04). Each track keeps
// its patch parsed in a memory patch of its own (1024 + track), and the voices are copied from it.
constexpr int kFirstMemoryPatch = 1024;
std::array<int, kSynthTrackCount> g_cachedAmyPatch{-1, -1, -1, -1};

int CachedAmyPatchNoLock(const EngineState& engine, int patch)
{
  const auto track = static_cast<std::size_t>(engine.synthId - 1);
  if (track >= kSynthTrackCount || patch < 0 || patch >= kFirstMemoryPatch) return patch;
  const int slot = kFirstMemoryPatch + static_cast<int>(track);
  if (g_cachedAmyPatch[track] != patch)
  {
    const char* text = amy_embed::amy_builtin_patch_command(static_cast<std::uint16_t>(patch));
    if (text == nullptr) return patch;
    amy_embed::patches_reset_patch(slot);
    amy_event store = amy_default_event();
    store.patch_number = static_cast<std::uint16_t>(slot);
    amy_embed::patches_store_patch(&store, const_cast<char*>(text));
    g_cachedAmyPatch[track] = patch;
  }
  return slot;
}

void ConfigureAmySynthNoLock(EngineState& engine)
{
  if (!UsesAmy(engine.kind))
  {
    return;
  }

  EnsureAmyInitialized();
  ReleaseAllAmyNotesNoLock(engine);
  engine.presetGain = PresetGainFor(engine);

  amy_event event = amy_default_event();
  event.synth = static_cast<uint16_t>(engine.synthId);
  event.num_voices = static_cast<uint16_t>(VoiceCountFor(engine.kind));
  if (engine.kind == SynthEngineKind::Drums)
  {
    event.oscs_per_voice = 1;
    event.wave = PCM;
    // No synth flags: AMY's IGNORE_NOTE_OFFS also suppresses explicit stop, and each pad names its
    // sample itself (SendAmyNoteEventNoLock).
    event.synth_flags = 0;
    event.amp_coefs[amy_embed::COEF_CONST] = 5.0f * Normalize(engine.drumMaster, 0, 127);
  }
  else
  {
    event.patch_number = static_cast<uint16_t>(CachedAmyPatchNoLock(engine, engine.patchNumber < 0 ? DefaultPatchFor(engine.kind) : engine.patchNumber));
  }
  amy_add_event(&event);
  if (engine.kind == SynthEngineKind::Juno) VoiceJunoPatchNoLock(engine);

  // AMY's Juno ROM models its HPF with global EQ. In this four-track rack that
  // would recolor every track and make project sound depend on preset load order.
  // Keep the shared master flat; per-track HPF remains unsupported in this version.
  event = amy_default_event();
  event.eq_l = event.eq_m = event.eq_h = 0.0f;
  amy_add_event(&event);

  // Fresh voices are silent: start them at the track gain without a ramp.
  TrackMixState& mix = TrackMixFor(engine);
  mix.amyGain = EffectiveTrackGain(mix);
  mix.amyBlockStartQ16 = mix.amyBlockEndQ16 = GainToQ16(Clamp01(mix.amyGain) * engine.presetGain);
  RebuildAmyOscTrackMapNoLock();
}

void LoadDx7FactoryPresetNoLock(EngineState& engine, const Dx7FactoryPresetDefinition& preset)
{
  engine.patchNumber = preset.patchNumber;
  ConfigureAmySynthNoLock(engine);
}

SamplerVoice& AllocateSamplerVoice(EngineState& engine)
{
  auto iterator = std::find_if(engine.samplerVoices.begin(), engine.samplerVoices.end(), [](const SamplerVoice& voice) {
    return !voice.active;
  });

  if (iterator == engine.samplerVoices.end())
  {
    // No free voice: the oldest let-go one, a held note only when every voice holds one.
    iterator = std::min_element(engine.samplerVoices.begin(), engine.samplerVoices.end(), [](const SamplerVoice& left, const SamplerVoice& right) {
      return left.releasing != right.releasing ? left.releasing : left.serial < right.serial;
    });
    if (!iterator->releasing) g_lostStolen.fetch_add(1, std::memory_order_relaxed);
  }

  *iterator = SamplerVoice{};
  iterator->active = true;
  iterator->serial = engine.nextSamplerVoiceSerial++;
  return *iterator;
}

void UpdateSamplerDerived(SamplerParams& params)
{
  params.derivedRevision = params.revision;
  params.attackStep = 1.0f / (std::max)(16.0f, (0.002f + Normalize(params.attack, 0, 127) * 0.25f) * static_cast<float>(kSampleRate));
  params.releaseStep = 1.0f / (std::max)(16.0f, (0.01f + Normalize(params.release, 0, 127) * 1.2f) * static_cast<float>(kSampleRate));
  params.outputGain = Normalize(params.sampleGain, 0, 127) * Normalize(params.level, 0, 127);
  params.driveGain = 1.0f + Normalize(params.drive, 0, 127) * 4.0f;
  const std::size_t start = SamplerStartFrame(params);
  params.startCursor = static_cast<std::uint64_t>(start) << 32;
  params.endCursor = static_cast<std::uint64_t>(SamplerEndFrame(params, start)) << 32;
}

// One voice for `frames` frames (at most a block), added into `out` at kSamplerVoiceGain. Its state
// stays in locals over the block: called once per frame and voice it took 355 cycles a frame on the
// board, five times what reading and interpolating the sample costs, most of it the call and the state
// going to memory and back (the stress test, 2026-11-04).
void RenderSamplerVoiceBlock(SamplerParams& params, SamplerVoice& voice, float* out, std::size_t frames)
{
  if (!voice.active || !SamplerHasSample(params))
  {
    voice.active = false;
    return;
  }
  if (params.derivedRevision != params.revision) UpdateSamplerDerived(params);
  if (voice.revision != params.revision)
  {
    voice.revision = params.revision;
    const double sourceRate = static_cast<double>(params.sampleRate == 0 ? kSampleRate : params.sampleRate);
    const double pitchRatio = std::pow(2.0, static_cast<double>(static_cast<int>(voice.note) - params.sampleRoot + params.sampleTune) / 12.0);
    voice.cursorStep = static_cast<std::uint64_t>((std::max)(0.0001, (sourceRate / static_cast<double>(kSampleRate)) * pitchRatio) * 4294967296.0);
  }
  const SampleBuffer& buffer = *params.sampleFrames;
  const std::int16_t* const memory = buffer.frames.get();
  const SampleStream* const stream = buffer.stream.get();
  const std::uint64_t startCursor = params.startCursor;
  const std::uint64_t endCursor = params.endCursor;
  const std::uint64_t step = voice.cursorStep;
  const bool loop = params.sampleLoop != 0;
  const std::size_t last = static_cast<std::size_t>(endCursor >> 32) - 1;
  const bool releasing = voice.releasing;
  const float attackStep = params.attackStep;
  const float releaseStep = params.releaseStep;
  const float g = params.filterG;
  const float filterInverse = params.filterInverse;
  const float drive = params.driveGain;
  const float level = (0.2f + 0.8f * voice.velocity) * params.outputGain * kSamplerVoiceGain;
  std::uint64_t cursor = voice.sampleCursor;
  float envelope = voice.envelope;
  float low = voice.filterLow;
  float band = voice.filterBand;
  for (std::size_t frame = 0; frame < frames; ++frame)
  {
    if (releasing)
    {
      envelope -= releaseStep;
      if (envelope <= 0.0f)
      {
        voice = SamplerVoice{};
        return;
      }
    }
    else
    {
      envelope = (std::min)(1.0f, envelope + attackStep);
    }
    if (cursor < startCursor) cursor = startCursor;
    if (cursor >= endCursor && !loop)
    {
      voice = SamplerVoice{};
      return;
    }

    const std::size_t index0 = (std::min)(last, static_cast<std::size_t>(cursor >> 32));
    const std::size_t index1 = (std::min)(last, index0 + 1);
    const float fraction = static_cast<float>(static_cast<std::uint32_t>(cursor)) * (1.0f / 4294967296.0f);
    const float sample0 = static_cast<float>(stream != nullptr ? stream->Read(voice.stream, index0) : memory[index0]) * (1.0f / 32768.0f);
    const float sample1 = static_cast<float>(stream != nullptr ? stream->Read(voice.stream, index1) : memory[index1]) * (1.0f / 32768.0f);
    float sample = sample0 + (sample1 - sample0) * fraction;

    // Two-integrator state-variable lowpass, with bounded Q for predictable headroom.
    const float bandNext = (band + g * (sample - low)) * filterInverse;
    const float lowNext = low + g * bandNext;
    band = 2.0f * bandNext - band;
    low = 2.0f * lowNext - low;
    sample = lowNext;

    // A soft clip, x (27 + x^2) / (27 + 9 x^2) on [-3, 3], not a tanh per sample. The chip's FPU has no
    // division (a float divide is a call into ROM), so the reciprocal of the denominator, which stays
    // within [27, 108], comes from a linear first guess and four Newton steps: exact to float precision.
    sample = (std::clamp)(sample * drive, -3.0f, 3.0f);
    const float squared = sample * sample;
    const float denominator = 27.0f + 9.0f * squared;
    float reciprocal = 0.036133695f - 0.000267657f * denominator;
    for (int newton = 0; newton < 4; ++newton) reciprocal *= 2.0f - denominator * reciprocal;
    out[frame] += sample * (27.0f + squared) * reciprocal * envelope * level;

    cursor += step;
    if (loop && cursor >= endCursor)
    {
      const std::uint64_t span = (std::max)(std::uint64_t{1} << 32, endCursor - startCursor);
      cursor = startCursor + (cursor - startCursor) % span;
    }
  }
  voice.sampleCursor = cursor;
  voice.envelope = envelope;
  voice.filterLow = low;
  voice.filterBand = band;
}

// A sampler track's voices for `frames` frames (at most a block), added into `out` at the track's
// level, which moves toward `target` frame by frame.
std::array<float, kAmyFramesPerBlock> g_samplerVoiceMix{};

void RenderSamplerTrackNoLock(EngineState& engine, TrackMixState& trackMix, float target, float* out, std::size_t frames)
{
  float* voices = g_samplerVoiceMix.data();
  std::fill_n(voices, frames, 0.0f);
  for (SamplerVoice& voice : engine.samplerVoices)
    if (voice.active) RenderSamplerVoiceBlock(engine.sampler, voice, voices, frames);
  for (std::size_t frame = 0; frame < frames; ++frame)
  {
    if (trackMix.samplerGain != target) trackMix.samplerGain = ApproachGain(trackMix.samplerGain, target, kTrackGainStepPerFrame);
    out[frame] += voices[frame] * trackMix.samplerGain;
  }
}

void ResetEngineNoLock(EngineState& engine, std::size_t engineIndex, SynthEngineKind kind)
{
  if (engine.configured && UsesAmy(engine.kind))
  {
    ReleaseAllAmyNotesNoLock(engine);
    amy_event release = amy_default_event();
    release.synth = static_cast<std::uint8_t>(engine.synthId);
    release.num_voices = 0;
    amy_add_event(&release);
  }

  engine = EngineState{};
  engine.kind = kind;
  engine.configured = kind != SynthEngineKind::None;
  engine.synthId = static_cast<int>(engineIndex) + 1;
  engine.patchNumber = DefaultPatchFor(kind);
  UpdateSamplerFilter(engine.sampler);
  RebuildAmyOscTrackMapNoLock();
}

bool MasterFxNeutral(const std::array<int, fx::kMasterParamCount>& master)
{
  return master[static_cast<std::size_t>(fx::MasterParam::EqLow)] == fx::kFilterCenter &&
         master[static_cast<std::size_t>(fx::MasterParam::EqHigh)] == fx::kFilterCenter &&
         master[static_cast<std::size_t>(fx::MasterParam::Compressor)] == 0 &&
         master[static_cast<std::size_t>(fx::MasterParam::Tape)] == 0;
}

int MasterFxValue(fx::MasterParam param)
{
  return g_fx.master[static_cast<std::size_t>(param)];
}

// The track's settings ask for effects (sends count even before their bus exists).
bool TrackFxRequested(std::size_t track)
{
  const TrackFxParams& params = g_fx.tracks[track];
  return !fx::dsp::IsNeutral(params.inserts) || params.delaySend > 0 || params.reverbSend > 0;
}

// Back to the defaults; the engine re-sends every non-default value right after.
void ResetTrackFxNoLock(std::size_t track, int chorus)
{
  g_fx.tracks[track] = TrackFxParams{};
  g_fx.tracks[track].inserts.chorus = std::clamp(chorus, 0, 3);
  g_fx.dirty = true;
}

void SetTrackFxParameterNoLock(std::size_t track, std::string_view id, int value)
{
  TrackFxParams& params = g_fx.tracks[track];
  const int clamped = std::clamp(value, 0, 127);
  if (id == "fx.drive") params.inserts.drive = clamped;
  else if (id == "fx.crush") params.inserts.crush = clamped;
  else if (id == "fx.filter") params.inserts.filter = clamped;
  else if (id == "fx.resonance") params.inserts.resonance = clamped;
  else if (id == "fx.chorus") params.inserts.chorus = std::clamp(value, 0, 3);
  else if (id == "fx.delay") params.delaySend = clamped;
  else if (id == "fx.reverb") params.reverbSend = clamped;
  else return;
  g_fx.dirty = true;
}

// Allocates what the current settings need (render thread, only when settings changed). A bus
// whose memory is missing stays silent and reports itself unavailable; the next settings change
// tries again.
// SuspendEffects calls not yet resumed (render lock).
int g_fxSuspended = 0;

// The effects run per sample and stay in internal RAM. They can do without their memory, the small
// plain allocations elsewhere cannot: no piece of it below this much internal RAM free (the device's
// nothrow new keeps the same floor for blocks of 4 KB and more; the choruses and buses are 1 KB each
// and took the heap down to 4 KB on the board, 2026-11-03).
bool FxRoom(std::size_t bytes)
{
  return memory::FastFree() >= bytes + 20 * 1024;
}

void UpdateFxNoLock()
{
  if (g_fxSuspended > 0) return;
  // A slower tempo can ask for a delay time only the long line holds.
  const float tempo = g_fxTempo.load(std::memory_order_relaxed);
  if (tempo != g_fx.memoryTempo)
  {
    g_fx.memoryTempo = tempo;
    g_fx.dirty = true;
  }
  if (!g_fx.dirty)
  {
    return;
  }
  g_fx.dirty = false;

  bool anyTrack = false;
  bool delaySends = false;
  bool reverbSends = false;
  for (std::size_t track = 0; track < g_fx.tracks.size(); ++track)
  {
    anyTrack = anyTrack || TrackFxRequested(track);
    delaySends = delaySends || g_fx.tracks[track].delaySend > 0;
    reverbSends = reverbSends || g_fx.tracks[track].reverbSend > 0;
  }
  const bool delayWanted = delaySends && MasterFxValue(fx::MasterParam::DelayLevel) > 0;
  const bool reverbWanted = reverbSends && MasterFxValue(fx::MasterParam::ReverbLevel) > 0;
  if (!g_fx.core)
  {
    if (!anyTrack && MasterFxNeutral(g_fx.master))
    {
      return;
    }
    if (FxRoom(sizeof(FxCore))) g_fx.core.reset(new (std::nothrow) FxCore());
    if (!g_fx.core)
    {
      // Without the core no effect can run: the tracks stay on AMY's mix, the buses silent.
      g_fxBusFailed[fx::kBusDelay].store(delayWanted, std::memory_order_relaxed);
      g_fxBusFailed[fx::kBusReverb].store(reverbWanted, std::memory_order_relaxed);
      return;
    }
  }

  FxCore& core = *g_fx.core;
  if (delayWanted)
  {
    // The long line only when the delay time needs it: it costs twice the memory.
    const bool longLine = fx::dsp::DelaySeconds(MasterFxValue(fx::MasterParam::DelayTime), g_fxTempo.load(std::memory_order_relaxed)) >
                          static_cast<float>(fx::dsp::kDelayLineFrames - 8) / fx::dsp::kBusRate;
    if (!core.buses.DelayAllocated() || longLine) g_fxBusFailed[fx::kBusDelay].store(!core.buses.AllocateDelay(longLine), std::memory_order_relaxed);
  }
  if (reverbWanted && !core.buses.ReverbAllocated())
  {
    g_fxBusFailed[fx::kBusReverb].store(!core.buses.AllocateReverb(), std::memory_order_relaxed);
  }
  for (std::size_t track = 0; track < g_fx.tracks.size(); ++track)
  {
    if (g_fx.tracks[track].inserts.chorus > 0 && (core.inserts[track].HasChorus() || FxRoom(fx::dsp::TrackInserts::kChorusMemoryBytes)))
    {
      (void)core.inserts[track].AllocateChorus();  // Without its line the chorus stays bypassed.
    }
    // Every track with effects renders into its bus; without the memory an AMY track still gets its
    // sends (from AMY's mix), only its inserts are lost.
    const EngineState& engine = g_engines[track];
    const bool needsBus = engine.configured && (UsesAmy(engine.kind) || engine.kind == SynthEngineKind::Sampler) &&
                          TrackFxRequested(track);
    if (needsBus && !g_fx.bus[track] && FxRoom(kAmyFramesPerBlock * sizeof(float)))
    {
      g_fx.bus[track].reset(new (std::nothrow) float[kAmyFramesPerBlock]);
    }
  }
}

// A track goes through its effects while its settings ask for them or its inserts and sends are
// still fading out, through its bus; an AMY track without a bus keeps only its sends, fed from
// AMY's mix.
void UpdateTrackRoutingNoLock()
{
  if (!g_fx.core)
  {
    g_fx.routed.fill(false);
    g_fx.insertRouted.fill(false);
    return;
  }
  const FxCore& core = *g_fx.core;
  for (std::size_t track = 0; track < g_engines.size(); ++track)
  {
    const EngineState& engine = g_engines[track];
    const TrackFxParams& params = g_fx.tracks[track];
    const bool insertsWanted = !fx::dsp::IsNeutral(params.inserts) || !core.inserts[track].Settled();
    const bool sendsWanted = (params.delaySend > 0 && core.buses.DelayAllocated()) || core.delayGain[track] != 0.0f ||
                             (params.reverbSend > 0 && core.buses.ReverbAllocated()) || core.reverbGain[track] != 0.0f;
    const bool hasBus = g_fx.bus[track] != nullptr;
    bool inserts = false;
    bool sends = false;
    if (engine.configured && engine.kind == SynthEngineKind::Sampler)
    {
      inserts = hasBus && (insertsWanted || sendsWanted);
    }
    else if (engine.configured && UsesAmy(engine.kind))
    {
      inserts = hasBus && (insertsWanted || sendsWanted);
      sends = !inserts && sendsWanted;
    }
    g_fx.insertRouted[track] = inserts;
    g_fx.routed[track] = inserts || sends;
  }
}

bool FxFloatPathWantedNoLock()
{
  if (!g_fx.core)
  {
    return false;
  }
  const FxCore& core = *g_fx.core;
  return std::any_of(g_fx.routed.begin(), g_fx.routed.end(), [](bool routed) { return routed; }) ||
         !MasterFxNeutral(g_fx.master) || !core.master.Bypassed() ||
         (core.buses.DelayAllocated() && !core.buses.DelayIdle()) || (core.buses.ReverbAllocated() && !core.buses.ReverbIdle());
}

float DelaySendTarget(std::size_t track)
{
  return g_fx.core->buses.DelayAllocated() ? fx::dsp::KnobGain(g_fx.tracks[track].delaySend) : 0.0f;
}

float ReverbSendTarget(std::size_t track)
{
  return g_fx.core->buses.ReverbAllocated() ? fx::dsp::KnobGain(g_fx.tracks[track].reverbSend) : 0.0f;
}

// Runs a routed track's samples through its inserts and adds them to the mix and, post-fader
// (the track gain is already in the samples), to the sends, in one pass.
SYNTH_FX_HOT void MixRoutedTrackNoLock(std::size_t track, float* samples, float* mix, float* delaySend, float* reverbSend, std::size_t frames)
{
  FxCore& core = *g_fx.core;
  core.inserts[track].Process(samples, frames, g_fx.tracks[track].inserts);
  const float delayFrom = core.delayGain[track];
  const float delayTo = DelaySendTarget(track);
  const float reverbFrom = core.reverbGain[track];
  const float reverbTo = ReverbSendTarget(track);
  core.delayGain[track] = delayTo;
  core.reverbGain[track] = reverbTo;
  if (delayFrom == 0.0f && delayTo == 0.0f && reverbFrom == 0.0f && reverbTo == 0.0f)
  {
    for (std::size_t index = 0; index < frames; ++index) mix[index] += samples[index];
    return;
  }
  if (delayFrom == delayTo && reverbFrom == reverbTo)
  {
    for (std::size_t index = 0; index < frames; ++index)
    {
      const float sample = samples[index];
      mix[index] += sample;
      delaySend[index] += sample * delayTo;
      reverbSend[index] += sample * reverbTo;
    }
    return;
  }
  const float inverse = frames == kAmyFramesPerBlock ? 1.0f / static_cast<float>(kAmyFramesPerBlock) : 1.0f / static_cast<float>(frames);
  const float delayStep = (delayTo - delayFrom) * inverse;
  const float reverbStep = (reverbTo - reverbFrom) * inverse;
  float delayGain = delayFrom, reverbGain = reverbFrom;
  for (std::size_t index = 0; index < frames; ++index)
  {
    const float sample = samples[index];
    delayGain += delayStep;
    reverbGain += reverbStep;
    mix[index] += sample;
    delaySend[index] += sample * delayGain;
    reverbSend[index] += sample * reverbGain;
  }
}

// The queue's bound: a note that would wait behind this many events is too late to play (counted as
// a note not heard). Everything else is kept, note-offs too, so nothing that sounds is left on.
constexpr std::size_t kMaxPendingEvents = 384;
std::atomic<std::uint32_t> g_droppedNoteOns{0};

void QueuePendingEvent(PendingEvent event)
{
  std::lock_guard<std::mutex> lock(g_pendingEventMutex);
  if (event.type == PendingEventType::NoteOn && g_pendingEvents.size() >= kMaxPendingEvents)
  {
    g_droppedNoteOns.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  g_pendingEvents.push_back(std::move(event));
}

void ApplyPendingEvent(const PendingEvent& event)
{
  if (event.engineIndex >= g_engines.size())
  {
    return;
  }

  EngineState& engine = g_engines[event.engineIndex];
  switch (event.type)
  {
    case PendingEventType::EnsureEngine:
      if (!engine.configured || engine.kind != event.kind)
      {
        const bool kindChanged = engine.kind != event.kind;
        ResetEngineNoLock(engine, event.engineIndex, event.kind);
        if (UsesAmy(event.kind))
        {
          ConfigureAmySynthNoLock(engine);
        }
        if (kindChanged)
        {
          ResetTrackFxNoLock(event.engineIndex, 0);
          if (g_fx.core)
          {
            // New voices: the old engine's insert state and send ramps must not leak into them.
            g_fx.core->inserts[event.engineIndex].Reset();
            g_fx.core->delayGain[event.engineIndex] = 0.0f;
            g_fx.core->reverbGain[event.engineIndex] = 0.0f;
          }
        }
      }
      break;
    case PendingEventType::SetPreset:
      if (!engine.configured)
      {
        return;
      }

      if (engine.kind == SynthEngineKind::Dx7)
      {
        if (engine.noteOffset != 0)
        {
          ReleaseAllAmyNotesNoLock(engine);
          engine.noteOffset = 0;
        }
        if (const Dx7FactoryPresetDefinition* preset = FindDx7FactoryPreset(event.text))
        {
          LoadDx7FactoryPresetNoLock(engine, *preset);
        }
        else
        {
          engine.patchNumber = MapDx7Preset(event.text);
          ConfigureAmySynthNoLock(engine);
        }
        std::copy_n(factory::kDx7[engine.patchNumber - 128], engine.dx7.size(), engine.dx7.begin());
        engine.noteOffset = factory::Dx7Value(engine.dx7.data(), "global.transpose");
      }
      else if (engine.kind == SynthEngineKind::Juno)
      {
        if (engine.noteOffset != 0)
        {
          ReleaseAllAmyNotesNoLock(engine);
          engine.noteOffset = 0;
        }
        engine.patchNumber = MapJunoPreset(event.text);
        std::copy_n(factory::kJuno[engine.patchNumber], engine.juno.size(), engine.juno.begin());
        ConfigureAmySynthNoLock(engine);
      }
      else if (engine.kind == SynthEngineKind::Drums && event.text == "drums.808")
      {
        engine.drumLevels.fill(100);
        engine.drumMaster = 100;
        ConfigureAmySynthNoLock(engine);
      }
      // Every preset starts from neutral effects, except the Juno chorus stored in the patch.
      ResetTrackFxNoLock(event.engineIndex, engine.kind == SynthEngineKind::Juno ?
          factory::JunoValue(factory::kJuno[std::clamp(engine.patchNumber, 0, 127)], "fx.chorus") : 0);
      break;
    case PendingEventType::SetParameter:
      if (fx::IsTrackParameter(event.text))
      {
        // Track effects belong to the track, not to the engine state (wiped on engine switches).
        SetTrackFxParameterNoLock(event.engineIndex, event.text, event.value);
        break;
      }
      switch (engine.kind)
      {
        case SynthEngineKind::Dx7:
          ApplyDx7ParameterNoLock(engine, event.text, event.value);
          break;
        case SynthEngineKind::Juno:
          ApplyJunoParameterNoLock(engine, event.text, event.value);
          break;
        case SynthEngineKind::Drums:
          if (event.text == "master.level")
          {
            engine.drumMaster = std::clamp(event.value, 0, 127);
            auto update = SynthEvent(engine, 0);
            update.amp_coefs[amy_embed::COEF_CONST] = 5.0f * Normalize(engine.drumMaster, 0, 127);
            amy_add_event(&update);
          }
          else
          {
            const auto parameter = std::find(kDrumParameterIds.begin(), kDrumParameterIds.end(), event.text);
            if (parameter != kDrumParameterIds.end()) engine.drumLevels[parameter - kDrumParameterIds.begin()] = std::clamp(event.value, 0, 127);
          }
          break;
        case SynthEngineKind::Sampler:
          if (event.text == "sample.start") engine.sampler.sampleStart = event.value;
          else if (event.text == "sample.end") engine.sampler.sampleEnd = event.value;
          else if (event.text == "sample.loop") engine.sampler.sampleLoop = event.value;
          else if (event.text == "sample.tune") engine.sampler.sampleTune = event.value;
          else if (event.text == "sample.root") engine.sampler.sampleRoot = event.value;
          else if (event.text == "sample.gain") engine.sampler.sampleGain = event.value;
          else if (event.text == "env.attack") engine.sampler.attack = event.value;
          else if (event.text == "env.release") engine.sampler.release = event.value;
          else if (event.text == "filter.cutoff") { engine.sampler.cutoff = event.value; UpdateSamplerFilter(engine.sampler); }
          else if (event.text == "filter.resonance") { engine.sampler.resonance = event.value; UpdateSamplerFilter(engine.sampler); }
          else if (event.text == "amp.level") engine.sampler.level = event.value;
          ++engine.sampler.revision;
          break;
        case SynthEngineKind::None:
        default:
          break;
      }
      break;
    case PendingEventType::SetSamplerFrames:
      engine.sampler.sampleFrames = event.frames;
      engine.sampler.sampleRate = event.sampleRate == 0 ? kSampleRate : event.sampleRate;
      ++engine.sampler.revision;
      for (SamplerVoice& voice : engine.samplerVoices)
      {
        voice = SamplerVoice{};
      }
      // Sampler cleanup can arrive after a track switches to AMY and plays a
      // note. Only sampler voices are reset here; keep AMY's release tracking.
      break;
    case PendingEventType::NoteOn:
      if (!engine.configured || engine.kind == SynthEngineKind::None)
      {
        return;
      }
      // A muted track drops new notes. Velocity-0 note-ons act as releases, so they still pass.
      if (g_trackMix[event.engineIndex].muted && event.velocity != 0)
      {
        return;
      }

      if (UsesAmy(engine.kind))
      {
        EnsureAmyInitialized();
        float velocity = std::clamp(static_cast<float>(event.velocity) / 127.0f, 0.0f, 1.0f);
        if (engine.kind == SynthEngineKind::Drums)
        {
          const auto drum = std::find(kDrumNotes.begin(), kDrumNotes.end(), event.note);
          if (drum == kDrumNotes.end() || event.velocity == 0) return;
          velocity *= Normalize(engine.drumLevels[drum - kDrumNotes.begin()], 0, 127);
          if (velocity == 0) return;
          if (event.note == 42 && engine.activeNotes[46])
          {
            SendAmyNoteEventNoLock(engine, 46, 0.0f);
            engine.activeNotes[46] = false;
          }
        }
        if (engine.activeNotes[event.note])
        {
          SendAmyNoteEventNoLock(engine, event.note, 0.0f);
        }
        const float meterLevel = velocity;
        if (engine.kind == SynthEngineKind::Dx7)
        {
          velocity = (std::min)(velocity, kDx7MaxAmyVelocity);
        }
        SendAmyNoteEventNoLock(engine, event.note, velocity);
        engine.activeNotes[event.note] = true;
        BumpTrackActivityNoLock(event.engineIndex, meterLevel);
        return;
      }

      {
        if (!SamplerHasSample(engine.sampler)) return;
        ReleaseSamplerNoteNoLock(engine, event.note);
        SamplerVoice& voice = AllocateSamplerVoice(engine);
        voice.note = event.note;
        voice.velocity = std::clamp(static_cast<float>(event.velocity) / 127.0f, 0.0f, 1.0f);
        voice.sampleCursor = static_cast<std::uint64_t>(SamplerStartFrame(engine.sampler)) << 32;
        BumpTrackActivityNoLock(event.engineIndex, voice.velocity);
      }
      break;
    case PendingEventType::NoteOff:
      if (engine.kind == SynthEngineKind::Drums) return;
      if (UsesAmy(engine.kind))
      {
        SendAmyNoteEventNoLock(engine, event.note, 0.0f);
        engine.activeNotes[event.note] = false;
        return;
      }

      ReleaseSamplerNoteNoLock(engine, event.note);
      break;
    case PendingEventType::AllNotesOff:
      if (UsesAmy(engine.kind))
      {
        ReleaseAllAmyNotesNoLock(engine);
        return;
      }

      for (SamplerVoice& voice : engine.samplerVoices)
      {
        voice.releasing = true;
      }
      break;
    case PendingEventType::SetTrackGain:
      // Applied by the ramps in StepAmyTrackGainsNoLock and the sampler mix loop.
      g_trackMix[event.engineIndex].gain = event.gain;
      break;
    case PendingEventType::SetTrackMuted:
      g_trackMix[event.engineIndex].muted = event.value != 0;
      break;
    case PendingEventType::SetMasterFx:
      if (event.note < fx::kMasterParamCount)
      {
        g_fx.master[event.note] = event.value;
        g_fx.dirty = true;
      }
      break;
  }
}

std::int64_t EventClockMicros()
{
#if defined(ESP_PLATFORM)
  return esp_timer_get_time();
#else
  return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
#endif
}

// Per block the render thread applies events for at most this long; the rest, in order, the next block.
// A project loading queues its engines, presets and parameters at once, and applying them all in one
// block ran the output dry (the stress test, 2026-11-04); a note behind them is a block late instead.
// The board only: hosts have no output to run dry, and tests count on an event applying by the next
// block however slow the machine.
#if defined(ESP_PLATFORM)
constexpr std::int64_t kEventBlockMicros = 1500;
#else
constexpr std::int64_t kEventBlockMicros = std::numeric_limits<std::int64_t>::max();
#endif

// How far the output is ahead of the speaker (the device queues six blocks), from how long each block
// took to render: a heavy event goes only when the queue holds more than it takes, and a block that
// renders in less than its length wins back the difference. The board only: elsewhere every event goes.
#if defined(CARDENZA_AUDIO_QUEUE_FRAMES)
constexpr float kOutputQueueMicros = 1000000.0f * static_cast<float>(CARDENZA_AUDIO_QUEUE_FRAMES) / static_cast<float>(kSampleRate);
#else
constexpr float kOutputQueueMicros = 6.0f * 1000000.0f * static_cast<float>(kAmyFramesPerBlock) / static_cast<float>(kSampleRate);
#endif
constexpr float kHeavyEventMicros = 25000.0f;
float g_outputAheadMicros = 0.0f;
// Since when a heavy event has waited for the output to get ahead (render clock, 0 for none). While
// it waits the voice budget is smaller (kHeavyWaitBudgetShare), so the blocks render faster and the
// output gains the lead: an overloaded device never got it, the engine switch waited for good and
// every later note of its track queued behind it (the stress test's "worst", 2026-11-04). After
// kHeavyEventMaxWaitMicros it goes anyway: one late block rather than a track that never changes.
std::int64_t g_heavyWaitingSince = 0;
constexpr std::int64_t kHeavyEventMaxWaitMicros = 300000;

bool HeavyEventFitsNoLock()
{
#if defined(ESP_PLATFORM)
  return g_outputAheadMicros >= kHeavyEventMicros ||
         (g_heavyWaitingSince != 0 && EventClockMicros() - g_heavyWaitingSince >= kHeavyEventMaxWaitMicros);
#else
  return true;
#endif
}

// While a project is built the output holds still: the audio task fades out its last block and then
// writes silence without the render lock, so the building thread applies its events (some 20 ms
// each, heavy) at once and nothing runs dry; the first block after fades in.
std::atomic<int> g_holdRequests{0};
std::atomic<bool> g_outputHeld{false};

// An engine set up or a preset loaded: AMY builds every voice of it again, 18-22 ms on the board (the
// stress test, 2026-11-04), more than three blocks.
bool HeavyEvent(const PendingEvent& event)
{
  return event.type == PendingEventType::EnsureEngine || event.type == PendingEventType::SetPreset;
}

// Those that are there now, in order, one at a time (a deque of its own would allocate, even an empty
// one, and a failed allocation here ends the program), until `budgetMicros` is used up. True when some
// are left. Not `heavyAllowed`: a heavy event waits, and the later events of its engine with it (the
// others go on, in their order).
bool DrainPendingEvents(std::int64_t budgetMicros = std::numeric_limits<std::int64_t>::max(), bool heavyAllowed = true)
{
  std::size_t count = 0;
  {
    std::lock_guard<std::mutex> lock(g_pendingEventMutex);
    count = g_pendingEvents.size();
  }
  const std::int64_t start = EventClockMicros();
  std::uint32_t waiting = 0;  // engines whose events wait behind a heavy one
  std::size_t index = 0;      // in the queue: those before it wait
  bool heavyWaits = false;    // a heavy event was left waiting in this pass
  for (; count > 0; --count)
  {
    PendingEvent event;
    {
      std::lock_guard<std::mutex> lock(g_pendingEventMutex);
      if (index >= g_pendingEvents.size()) break;
      const PendingEvent& next = g_pendingEvents[index];
      const std::uint32_t engine = 1u << (next.engineIndex & 31u);
      if ((waiting & engine) != 0 || (!heavyAllowed && HeavyEvent(next)))
      {
        if (HeavyEvent(next))
        {
          heavyWaits = true;
          if (g_heavyWaitingSince == 0) g_heavyWaitingSince = (std::max<std::int64_t>)(1, EventClockMicros());
        }
        waiting |= engine;
        ++index;
        continue;
      }
      event = std::move(g_pendingEvents[index]);
      g_pendingEvents.erase(g_pendingEvents.begin() + static_cast<std::ptrdiff_t>(index));
    }
    if (HeavyEvent(event)) g_heavyWaitingSince = 0;
    ApplyPendingEvent(event);
#if defined(ESP_PLATFORM)
    // One heavy event a call: the output has to win its time back before the next. (Hosts have no
    // output to run dry, and tests count on what they queued sounding in the next block.)
    if (HeavyEvent(event)) heavyAllowed = false;
#endif
    if (count > 1 && EventClockMicros() - start >= budgetMicros) return true;
  }
  // The whole queue seen: no heavy event waits any more (gone with its engine, say), so the budget is
  // whole again.
  if (!heavyWaits) g_heavyWaitingSince = 0;
  return waiting != 0;
}

bool HasAmyEnginesNoLock()
{
  for (const EngineState& engine : g_engines)
  {
    if (engine.configured && UsesAmy(engine.kind))
    {
      return true;
    }
  }

  return false;
}

bool HasActiveSamplerVoices(const EngineState& engine)
{
  return engine.configured && engine.kind == SynthEngineKind::Sampler &&
         std::any_of(engine.samplerVoices.begin(), engine.samplerVoices.end(), [](const SamplerVoice& voice) { return voice.active; });
}

// Tells the card streams, once per block, where every voice is and where its notes start.
void PublishSampleStreamsNoLock()
{
  static_assert(kSynthTrackCount * kSamplerVoiceCount <= kMaxStreamReaders, "every sampler voice needs a stream reader");
  for (std::size_t engineIndex = 0; engineIndex < g_engines.size(); ++engineIndex)
  {
    const EngineState& engine = g_engines[engineIndex];
    const SampleStream* stream = engine.sampler.sampleFrames ? engine.sampler.sampleFrames->stream.get() : nullptr;
    if (stream != nullptr) stream->PublishStart(SamplerStartFrame(engine.sampler));
    for (std::size_t voiceIndex = 0; voiceIndex < engine.samplerVoices.size(); ++voiceIndex)
    {
      const SamplerVoice& voice = engine.samplerVoices[voiceIndex];
      SampleStream::PublishReader(engineIndex * kSamplerVoiceCount + voiceIndex, voice.active ? stream : nullptr,
                                  static_cast<std::size_t>(voice.sampleCursor >> 32));
    }
  }
}

bool HasActiveSamplerVoicesNoLock()
{
  return std::any_of(g_engines.begin(), g_engines.end(), HasActiveSamplerVoices);
}

// Silent sampler tracks take a new gain at once; only sounding voices need the ramp.
void SettleIdleSamplerGainsNoLock()
{
  for (std::size_t index = 0; index < g_engines.size(); ++index)
  {
    if (!HasActiveSamplerVoices(g_engines[index]))
    {
      g_trackMix[index].samplerGain = EffectiveTrackGain(g_trackMix[index]);
    }
  }
}

bool HasHeldNotes(const EngineState& engine)
{
  if (!engine.configured)
  {
    return false;
  }

  switch (engine.kind)
  {
    case SynthEngineKind::Dx7:
    case SynthEngineKind::Juno:
      return std::any_of(engine.activeNotes.begin(), engine.activeNotes.end(), [](bool active) { return active; });
    case SynthEngineKind::Sampler:
      return std::any_of(engine.samplerVoices.begin(), engine.samplerVoices.end(), [](const SamplerVoice& voice) {
        return voice.active && !voice.releasing;
      });
    default:
      // Drum pads are one-shots: their meter follows the note-on bump only.
      return false;
  }
}

void UpdateMetersNoLock(const std::int16_t* destination, std::size_t frameCount)
{
  const float decay = std::exp(-static_cast<float>(frameCount) / (kMeterDecaySeconds * static_cast<float>(kSampleRate)));

  int peak = 0;
  for (std::size_t index = 0; index < frameCount; ++index)
  {
    peak = (std::max)(peak, std::abs(static_cast<int>(destination[index])));
  }
  g_outputPeakState = (std::max)((std::min)(1.0f, static_cast<float>(peak) / 32767.0f), g_outputPeakState * decay);
  if (g_outputPeakState < kMeterFloor)
  {
    g_outputPeakState = 0.0f;
  }
  g_outputPeak.store(g_outputPeakState, std::memory_order_relaxed);

  for (std::size_t index = 0; index < g_engines.size(); ++index)
  {
    TrackMixState& mix = g_trackMix[index];
    mix.activity *= decay;
    if (HasHeldNotes(g_engines[index]))
    {
      mix.activity = (std::max)(mix.activity, mix.heldLevel * kActivityHeldFraction * EffectiveTrackGain(mix));
    }
    if (mix.activity < kMeterFloor)
    {
      mix.activity = 0.0f;
    }
    g_publishedMix[index].activity.store(mix.activity, std::memory_order_relaxed);
  }
}

void UpdateFxMetersNoLock(std::size_t frameCount, float delayPeak, float reverbPeak)
{
  const std::array<float, fx::kBusCount> peaks{{delayPeak, reverbPeak}};
  if (std::all_of(g_fx.returnMeter.begin(), g_fx.returnMeter.end(), [](float level) { return level == 0.0f; }) &&
      delayPeak == 0.0f && reverbPeak == 0.0f)
  {
    return;
  }
  static const float kBlockDecay = std::exp(-static_cast<float>(kAmyFramesPerBlock) / (kMeterDecaySeconds * static_cast<float>(kSampleRate)));
  const float decay = frameCount == kAmyFramesPerBlock ? kBlockDecay
                                                       : std::exp(-static_cast<float>(frameCount) / (kMeterDecaySeconds * static_cast<float>(kSampleRate)));
  for (std::size_t bus = 0; bus < fx::kBusCount; ++bus)
  {
    float& level = g_fx.returnMeter[bus];
    level = (std::max)((std::min)(1.0f, peaks[bus]), level * decay);
    if (level < kMeterFloor)
    {
      level = 0.0f;
    }
    g_fxReturnLevel[bus].store(level, std::memory_order_relaxed);
  }
}

// Scale from one AMY voice sample (S8.23) to the float mix (see kAmyBlockToMix), mirroring the gains
// a voice gets on its way out of AMY: mix_with_pan multiplies it by the centre pan gain (AMY's
// approximate sqrt(0.5), 0.7314) and amy_fill_buffer by MUL4_SS(0.1, volume), both through
// MUL8_SS, which keeps only gain >> 12; the S8.23 result then becomes int16 with 15 fraction bits.
float AmyVoiceToMixScale()
{
  static constexpr float kOne = 8388608.0f;  // 1.0 in S8.23 (static: MSVC asks a lambda to capture a local)
  const auto toSample = [](float value) { return static_cast<std::int32_t>(value * kOne); };
  const std::int32_t volume = ((toSample(0.1f) >> 10) * (toSample(amy_embed::amy_global.volume[0]) >> 9)) >> 4;
  const std::int32_t pan = toSample(amy_embed::dsps_sqrtf_f32_ansi(0.5f));
  return static_cast<float>(volume >> 12) / 2048.0f * static_cast<float>(pan >> 12) / 2048.0f / kOne;
}

// Fills the float mix of the current AMY block from AMY's output plus the AMY tracks with inserts,
// each through its chain, and adds their sends (the send-only tracks' share arrived through the
// render hook). Without a fresh AMY render (joining mid-block) the sends start empty.
SYNTH_FX_HOT void PrepareFxAmyBlockNoLock(const std::array<FxHookTrack, kSynthTrackCount>& plan, bool rendered)
{
  FxCore& core = *g_fx.core;
  float* mix = core.mix.data();
  // With every AMY track on its own bus, AMY's mix is silent: skip reading it.
  bool amyMixUsed = !rendered;
  for (std::size_t track = 0; track < plan.size() && !amyMixUsed; ++track)
  {
    amyMixUsed = g_engines[track].configured && UsesAmy(g_engines[track].kind) && plan[track].bus == nullptr;
  }
  if (amyMixUsed)
  {
    const std::int16_t* block = g_amyBlock.data();
    for (std::size_t frame = 0; frame < kAmyFramesPerBlock; ++frame)
    {
      mix[frame] = static_cast<float>(static_cast<std::int32_t>(block[2 * frame]) + static_cast<std::int32_t>(block[2 * frame + 1])) * kAmyBlockToMix;
    }
  }
  else
  {
    core.mix.fill(0.0f);
  }
  static_assert(kAmyChannelCount == 2, "the float mix reads interleaved stereo AMY blocks");
  if (!rendered)
  {
    core.delaySend.fill(0.0f);
    core.reverbSend.fill(0.0f);
  }
  for (std::size_t track = 0; track < plan.size(); ++track)
  {
    if (plan[track].bus != nullptr)
    {
      MixRoutedTrackNoLock(track, plan[track].bus, mix, core.delaySend.data(), core.reverbSend.data(), kAmyFramesPerBlock);
    }
  }
  g_fx.blockPrepared = true;
}

// Plans what the render hook does with every AMY track in the next block: tracks with inserts
// render into their bus, tracks that only send feed the sends from AMY's mix (their send ramps
// advance here, one block at a time).
std::array<FxHookTrack, kSynthTrackCount> PlanFxAmyBlockNoLock()
{
  FxCore& core = *g_fx.core;
  UpdateTrackRoutingNoLock();
  std::array<FxHookTrack, kSynthTrackCount> plan{};
  core.delaySend.fill(0.0f);
  core.reverbSend.fill(0.0f);
  g_fxVoiceScale = AmyVoiceToMixScale();
  constexpr float kInverseBlock = 1.0f / static_cast<float>(kAmyFramesPerBlock);
  for (std::size_t track = 0; track < plan.size(); ++track)
  {
    if (!g_fx.routed[track] || !UsesAmy(g_engines[track].kind))
    {
      continue;
    }
    FxHookTrack& entry = plan[track];
    if (g_fx.insertRouted[track])
    {
      entry.bus = g_fx.bus[track].get();
      std::fill_n(entry.bus, kAmyFramesPerBlock, 0.0f);
      continue;
    }
    const float delayFrom = core.delayGain[track];
    const float delayTo = DelaySendTarget(track);
    const float reverbFrom = core.reverbGain[track];
    const float reverbTo = ReverbSendTarget(track);
    core.delayGain[track] = delayTo;
    core.reverbGain[track] = reverbTo;
    if (delayFrom != 0.0f || delayTo != 0.0f)
    {
      entry.delaySend = core.delaySend.data();
      entry.delayGain = delayFrom * g_fxVoiceScale;
      entry.delayStep = (delayTo - delayFrom) * g_fxVoiceScale * kInverseBlock;
    }
    if (reverbFrom != 0.0f || reverbTo != 0.0f)
    {
      entry.reverbSend = core.reverbSend.data();
      entry.reverbGain = reverbFrom * g_fxVoiceScale;
      entry.reverbStep = (reverbTo - reverbFrom) * g_fxVoiceScale * kInverseBlock;
    }
  }
  return plan;
}

// The voices share two cores: this thread and a helper on the other one. Each sounding voice is a run of
// oscillators that one core renders whole: its first oscillator holds the filter and the envelope and
// renders the others, which would sound raw on their own. Both cores take runs from the front of the
// list until none is left, heaviest first, so whichever core is free renders the next voice and they
// finish close together. Fixed halves by estimated weight left one core idle for most of a block while
// the other, which also runs the effects afterwards, went over the budget (DEMO-2 on the board: 4.3 ms
// against 1.6 ms, 2026-11-04). Below this much weight the helper is not woken.
constexpr std::uint32_t kNoSplitBelowWeight = 4;

int VoiceWeight(std::size_t track)
{
  // Roughly what a voice of each engine costs.
  switch (g_engines[track].kind)
  {
    case SynthEngineKind::Dx7: return 8;
    case SynthEngineKind::Juno: return 3;
    default: return 1;
  }
}

bool VoiceFading(std::uint16_t osc);

// A voice's first oscillator, sounding and not on its way out.
bool VoiceSounding(std::uint16_t osc)
{
  return g_amyVoiceStart[osc] && amy_embed::synth[osc] != nullptr && amy_embed::synth[osc]->status == SYNTH_AUDIBLE && !VoiceFading(osc);
}

struct VoiceRun {
  std::uint16_t from = 0;
  std::uint16_t to = 0;
  int weight = 0;
  std::uint8_t kind = 0;   // SynthEngineKind of its track, None for oscillators of no track
  std::uint8_t track = 0;  // kSynthTrackCount for none
};
std::array<VoiceRun, kAmyMaxOscs> g_voiceRuns{};
std::size_t g_voiceRunCount = 0;
std::atomic<std::size_t> g_nextVoiceRun{0};

// The voices' share of the block. The voices that sound are as many as the tracks' voice counts allow
// (AMY steals the oldest let-go voice of a track that wants another), which is more than both cores
// can render in a block once release tails pile up (DEMO-2: 12 to 16 voices; a voice costs about 0.65 ms
// on the board, 2026-11-04). Rendering longer than a block runs the DMA queue dry: clicks, and the
// sequencer's notes late. So the voices are kept within the budget before that happens: what a voice of
// each engine costs is learnt from the renders themselves, and when the next block would not fit, the
// least needed voices fade out over this one (VoiceFadeVictim), a let-go tail before a held note.
// The board only: hosts render at whatever speed the machine running the tests has.
constexpr float kBlockMicros = 1000000.0f * static_cast<float>(kAmyFramesPerBlock) / static_cast<float>(kSampleRate);
constexpr float kVoiceBudgetShare = 0.92f;
constexpr float kHeavyWaitBudgetShare = 0.6f;  // while a heavy event waits for the lead (g_heavyWaitingSince)
// The helper shares core 0 with the UI: it renders voices for at most this long a block and leaves the
// rest to the audio core, so the UI keeps more than half of its core however much plays (with no cap,
// four DX7 tracks took 80% of it and the UI dropped a frame in four, the stress test, 2026-11-04).
constexpr float kHelperShareMicros = 0.45f * kBlockMicros;
// How long after the audio core the helper starts on the voices (woken through a task notification).
constexpr float kHelperStartMicros = 150.0f;
constexpr std::size_t kKindCount = 5;  // SynthEngineKind None..Drums
std::array<float, kKindCount> g_runCostMicros{50.0f, 700.0f, 650.0f, 200.0f, 150.0f};
std::array<std::array<float, kKindCount>, AMY_MAX_CORES> g_runCostSum{};
std::array<std::array<std::uint16_t, kKindCount>, AMY_MAX_CORES> g_runCostCount{};
float g_overheadMicros = 1500.0f;  // the rest of a block: deltas, effects, output
// The costs above are a model (the helper starts late, a voice cannot be split between the cores, the
// events' cost comes in bursts): how long the blocks really take corrects it. While they take longer
// than the budget's share on average the voices' allowance shrinks, and it grows back when there is
// room, never above the model's. (Without it four DX7 tracks with effects averaged 5.6 ms a block
// against a 4.9 ms target, the stress test 2026-11-04.)
constexpr float kBudgetScaleGain = 0.02f;  // per block, times how far off the average is
constexpr float kBudgetScaleFloor = 0.2f;
float g_budgetScale = 1.0f;
float g_renderAverageMicros = 0.0f;
std::int64_t g_voicePhaseMicros = 0;
// A voice marked here fades out over its next block and then stops (whole: every oscillator of it),
// unless the oscillator has started another note since.
std::array<bool, kAmyMaxOscs> g_voiceFading{};
std::array<std::uint32_t, kAmyMaxOscs> g_voiceFadingClock{};
std::array<float, kAmyMaxOscs> g_voicePeak{};  // the last block's peak of each voice, by its first oscillator
std::atomic<std::uint32_t> g_fadedVoices{0};


std::int64_t VoiceClockMicros()
{
#if defined(ESP_PLATFORM)
  return esp_timer_get_time();
#else
  return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
#endif
}

bool VoiceFading(std::uint16_t osc)
{
  return g_voiceFading[osc] && amy_embed::synth[osc] != nullptr && amy_embed::synth[osc]->note_on_clock == g_voiceFadingClock[osc];
}

void MarkVoiceFading(std::uint16_t osc)
{
  g_voiceFading[osc] = true;
  g_voiceFadingClock[osc] = amy_embed::synth[osc]->note_on_clock;
}

// The runs of the next block, heaviest first; their total weight.
std::uint32_t PlanVoiceRunsNoLock()
{
  const auto oscs = static_cast<std::uint16_t>(amy_embed::amy_global.config.max_oscs);
  const std::uint16_t known = (std::min)(oscs, static_cast<std::uint16_t>(g_amyOscTrack.size()));
  std::uint32_t total = 0;
  g_voiceRunCount = 0;
  for (std::uint16_t from = 0; from < oscs;)
  {
    // To the next voice's first oscillator (whatever precedes the first voice is a run of its own).
    std::uint16_t to = from + 1;
    while (to < known && !g_amyVoiceStart[to]) ++to;
    if (to >= known) to = oscs;
    bool audible = false;
    for (std::uint16_t osc = from; osc < to && !audible; ++osc)
      audible = amy_embed::synth[osc] != nullptr && amy_embed::synth[osc]->status == SYNTH_AUDIBLE;
    if (audible)
    {
      const bool tracked = from < known && g_amyOscTrack[from] != 0;
      const std::size_t track = tracked ? g_amyOscTrack[from] - 1u : kSynthTrackCount;
      const int weight = tracked ? VoiceWeight(track) : 1;
      const auto kind = tracked ? static_cast<std::uint8_t>(g_engines[track].kind) : std::uint8_t{0};
      g_voiceRuns[g_voiceRunCount++] = VoiceRun{from, to, weight, static_cast<std::uint8_t>(kind < kKindCount ? kind : 0),
                                                static_cast<std::uint8_t>(track)};
      total += static_cast<std::uint32_t>(weight);
    }
    from = to;
  }
  // Heaviest first, equal weights in their order. An insertion sort: std::stable_sort takes a buffer
  // from the heap, and this runs every block under the render lock (a few dozen runs at most).
  for (std::size_t index = 1; index < g_voiceRunCount; ++index)
  {
    const VoiceRun run = g_voiceRuns[index];
    std::size_t to = index;
    for (; to > 0 && g_voiceRuns[to - 1].weight < run.weight; --to) g_voiceRuns[to] = g_voiceRuns[to - 1];
    g_voiceRuns[to] = run;
  }
  g_nextVoiceRun.store(0, std::memory_order_relaxed);
  return total;
}

// All of the voice: what its first oscillator leaves behind would sound raw and never end.
void StopVoiceRunNoLock(const VoiceRun& run)
{
  for (std::uint16_t osc = run.from; osc < run.to; ++osc)
  {
    if (amy_embed::synth[osc] == nullptr) continue;
    amy_embed::synth[osc]->status = SYNTH_OFF;
    amy_embed::reset_modosc(amy_embed::msynth[osc]);
    amy_embed::reset_osc_state(amy_embed::synth[osc]);
  }
}

// A voice whose note started this block: not one sample of it has been heard.
bool VoiceUnheardNoLock(std::uint16_t osc)
{
  const auto* voice = amy_embed::synth[osc];
  return voice != nullptr && voice->status == SYNTH_AUDIBLE &&
         voice->note_on_clock == static_cast<std::uint32_t>(amy_embed::amy_global.total_blocks * AMY_BLOCK_SIZE);
}

// What amy_render does for each oscillator, for one run; a fading run goes to silence over the block and
// stops.
SAMPLE RenderVoiceRunNoLock(const VoiceRun& run, std::uint8_t core)
{
  const bool fading = VoiceFading(run.from);
  g_voiceFading[run.from] = false;
  SAMPLE highest = 0;
  for (std::uint16_t osc = run.from; osc < run.to; ++osc)
  {
    auto* voice = amy_embed::synth[osc];
    if (voice == nullptr || voice->status != SYNTH_AUDIBLE) continue;
    const std::uint16_t bus = voice->bus;
    SAMPLE* buffer = amy_embed::per_osc_fb[core][bus];
    std::memset(buffer, 0, AMY_BLOCK_SIZE * sizeof(SAMPLE));
    const SAMPLE peak = amy_embed::render_osc_wave(osc, core, buffer);
    if (fading)
      for (std::size_t frame = 0; frame < AMY_BLOCK_SIZE; ++frame)
        buffer[frame] = static_cast<SAMPLE>(static_cast<std::int64_t>(buffer[frame]) * static_cast<std::int64_t>(AMY_BLOCK_SIZE - frame) /
                                            static_cast<std::int64_t>(AMY_BLOCK_SIZE));
    if (voice->status != SYNTH_AUDIBLE)
    {
      amy_embed::reset_modosc(amy_embed::msynth[osc]);
      amy_embed::reset_osc_state(voice);
    }
    const auto hook = amy_embed::amy_global.config.amy_external_render_hook;
    const std::uint8_t handled = hook != nullptr ? hook(osc, buffer, AMY_BLOCK_SIZE) : 0;
    if (handled == 0)
    {
      // The instrument's level (AMY's iV), on the audio only, as amy_render applies it.
      constexpr std::uint16_t kNoVoice = UINT16_MAX;  // AMY_UNSET for a uint16_t (_Generic is C only)
      const std::uint16_t owner = amy_embed::osc_to_voice != nullptr ? amy_embed::osc_to_voice[osc] : kNoVoice;
      const float level = owner != kNoVoice ? amy_embed::instrument_level_for_voice(owner) : 1.0f;
      amy_embed::mix_with_pan(amy_embed::fbl[core][bus], buffer, amy_embed::msynth[osc]->last_pan, amy_embed::msynth[osc]->pan, level);
    }
    highest = (std::max)(highest, peak);
  }
  g_voicePeak[run.from] = static_cast<float>(highest < 0 ? -highest : highest);
  if (fading) StopVoiceRunNoLock(run);
  return highest;
}

// The voice to fade when there is not enough time. A track gives way only to itself: the one with the
// most voices sounding (then a melodic one before drums, then the dearer one) loses one of its own,
// a let-go tail first (the longest let go), then the quietest held note (last block's peak at the
// track's level), the oldest. A track playing fewer keeps all of its voices: one track's notes never
// push out another's (the user, 2026-11-04; oldest first across the tracks once emptied one track
// while the others played on). -1 when nothing sounds.
int VoiceFadeVictimNoLock()
{
  // The candidates are this block's runs (every sounding voice starts one, PlanVoiceRunsNoLock): at
  // most a voice each, not every oscillator. Ranked in single precision and integers: the chip has no
  // double-precision hardware, and this runs once for every voice faded in a block already too heavy.
  std::array<int, kSynthTrackCount> sounding{};
  std::array<float, kSynthTrackCount> cost{};
  for (std::size_t index = 0; index < g_voiceRunCount; ++index)
  {
    const VoiceRun& run = g_voiceRuns[index];
    if (run.from >= g_amyOscTrack.size() || !VoiceSounding(run.from)) continue;
    const std::size_t track = (g_amyOscTrack[run.from] - 1u) % kSynthTrackCount;
    ++sounding[track];
    cost[track] += g_runCostMicros[run.kind];
  }
  using Rank = std::tuple<int, int, float, int, float, std::int64_t>;
  int victim = -1;
  Rank victimRank{};
  for (std::size_t index = 0; index < g_voiceRunCount; ++index)
  {
    const std::uint16_t osc = g_voiceRuns[index].from;
    if (osc >= g_amyOscTrack.size() || !VoiceSounding(osc)) continue;
    const auto* voice = amy_embed::synth[osc];
    const std::size_t track = (g_amyOscTrack[osc] - 1u) % kSynthTrackCount;
    const bool drums = g_engines[track].kind == SynthEngineKind::Drums;
    const bool released = VoiceReleased(voice);
    const float level = static_cast<float>(g_voicePeak[osc]) * EffectiveTrackGain(g_trackMix[track]);
    const Rank rank{sounding[track], drums ? 0 : 1, cost[track], released ? 1 : 0, -level,
                    -static_cast<std::int64_t>(released ? voice->note_off_clock : voice->note_on_clock)};
    if (victim >= 0 && rank <= victimRank) continue;
    victim = static_cast<int>(osc);
    victimRank = rank;
  }
  return victim;
}

void CountGivenUpNoLock(std::uint16_t osc)
{
  const auto* voice = amy_embed::synth[osc];
  (voice != nullptr && VoiceReleased(voice) ? g_lostTails : g_lostHeld).fetch_add(1, std::memory_order_relaxed);
}

// Before rendering: if the voices that will still sound next block would not fit in the budget (the
// helper takes up to its share, the audio core the rest before its effects), fade the least needed
// ones out over this block, from the track with the most voices (VoiceFadeVictimNoLock).
void KeepVoicesWithinBudgetNoLock()
{
#if defined(ESP_PLATFORM)
  float total = 0.0f;
  int remaining = 0;
  for (std::size_t index = 0; index < g_voiceRunCount; ++index)
  {
    const VoiceRun& run = g_voiceRuns[index];
    if (VoiceFading(run.from)) continue;
    total += g_runCostMicros[run.kind];
    ++remaining;
  }
  // The audio core renders its part of the voices while the helper renders the rest, then the
  // effects: its part has to fit in what the effects leave. Shared evenly the helper takes as much as
  // the audio core less its late start, never more than its share. (Counting the helper's whole share
  // on top of the audio core's let four DX7 tracks with effects run 6.4 ms a block, the stress test
  // 2026-11-04.)
  const float share = g_heavyWaitingSince != 0 ? kHeavyWaitBudgetShare : kVoiceBudgetShare;
  const float mainAllowed = (share * kBlockMicros - g_overheadMicros) * g_budgetScale;
  const float helperAllowed = (std::min)(kHelperShareMicros, (std::max)(0.0f, mainAllowed - kHelperStartMicros));
  const float allowed = AMY_CORES > 1 ? mainAllowed + helperAllowed : mainAllowed;
  // As many as it takes: four tracks of eight-note pads change chords in one block, and the new
  // notes alone took 16 ms with at most eight fades a block (the stress test, 2026-11-04). The last
  // voice stays: when even that does not fit, the effects take the block, and RenderLoad is the net.
  // From the track with the most voices, its let-go tails first (VoiceFadeVictimNoLock).
  // (Holding the held notes back until the output was about to run dry ran it dry: 2026-11-04.)
  for (int faded = 0; faded < kAmyMaxVoices && total > allowed && remaining > 1; ++faded)
  {
    const int victim = VoiceFadeVictimNoLock();
    if (victim < 0) break;
    CountGivenUpNoLock(static_cast<std::uint16_t>(victim));
    MarkVoiceFading(static_cast<std::uint16_t>(victim));
    g_fadedVoices.fetch_add(1, std::memory_order_relaxed);
    --remaining;
    for (std::size_t index = 0; index < g_voiceRunCount; ++index)
      if (g_voiceRuns[index].from == victim) total -= g_runCostMicros[g_voiceRuns[index].kind];
  }

  // This block: the voices fading still render it. When it would not fit either (a chord change on
  // four tracks of pads started twenty voices in one block, which took 17 ms, the stress test
  // 2026-11-04), new voices not heard yet are left out, the busiest track's: never having sounded,
  // they stop without a click, as a voice taken by a newer note of the same track would.
  const float mainModel = kVoiceBudgetShare * kBlockMicros - g_overheadMicros;
  const float blockAllowed = mainModel + (AMY_CORES > 1 ? (std::min)(kHelperShareMicros, (std::max)(0.0f, mainModel - kHelperStartMicros)) : 0.0f);
  float thisBlock = 0.0f;
  std::array<int, kSynthTrackCount> sounding{};
  for (std::size_t index = 0; index < g_voiceRunCount; ++index)
  {
    const VoiceRun& run = g_voiceRuns[index];
    thisBlock += g_runCostMicros[run.kind];
    if (run.track < kSynthTrackCount) ++sounding[run.track];
  }
  // Only what the queue cannot take: a block longer than its length by more than the output is ahead
  // (less a block's margin) would run it dry.
  while (thisBlock - blockAllowed > g_outputAheadMicros - kBlockMicros)
  {
    // Only the track with the most voices gives up a note of its own (as VoiceFadeVictimNoLock): with
    // none new there, the block runs long and RenderLoad fades one of that track's next.
    std::size_t busiest = kSynthTrackCount;
    for (std::size_t track = 0; track < kSynthTrackCount; ++track)
    {
      const auto rank = [&](std::size_t of) { return std::make_tuple(sounding[of], g_engines[of].kind == SynthEngineKind::Drums ? 0 : 1); };
      if (sounding[track] > 0 && (busiest == kSynthTrackCount || rank(track) > rank(busiest))) busiest = track;
    }
    int chosen = -1;
    for (std::size_t index = 0; index < g_voiceRunCount && chosen < 0; ++index)
      if (g_voiceRuns[index].track == busiest && VoiceUnheardNoLock(g_voiceRuns[index].from)) chosen = static_cast<int>(index);
    if (chosen < 0) break;
    const VoiceRun& run = g_voiceRuns[static_cast<std::size_t>(chosen)];
    StopVoiceRunNoLock(run);
    g_voiceFading[run.from] = false;
    g_fadedVoices.fetch_add(1, std::memory_order_relaxed);
    g_lostUnheard.fetch_add(1, std::memory_order_relaxed);
    thisBlock -= g_runCostMicros[run.kind];
    --sounding[run.track];
  }
#endif
}

// What a voice of each engine cost this block, into the running estimate (about a tenth of a second).
void LearnVoiceCostsNoLock()
{
  for (std::size_t core = 0; core < AMY_MAX_CORES; ++core)
  {
    for (std::size_t kind = 0; kind < kKindCount; ++kind)
    {
      if (g_runCostCount[core][kind] != 0)
      {
        const float average = g_runCostSum[core][kind] / static_cast<float>(g_runCostCount[core][kind]);
        g_runCostMicros[kind] += 0.05f * (average - g_runCostMicros[kind]);
      }
      g_runCostSum[core][kind] = 0.0f;
      g_runCostCount[core][kind] = 0;
    }
  }
}

// One core's share: runs from the front until none is left (the helper, core 1 in AMY's numbering,
// stops taking more after kHelperShareMicros). AMY's chorus (rendered by amy_render on core 0) is off
// here (EnsureAmyInitialized).
void RenderClaimedRunsNoLock(std::uint8_t core)
{
  for (int bus = 0; bus <= amy_embed::amy_global.highest_bus; ++bus) std::fill_n(amy_embed::fbl[core][bus], AMY_BLOCK_SIZE * AMY_NCHANS, 0);
  SAMPLE highest = 0;
  const std::int64_t started = VoiceClockMicros();
  for (;;)
  {
#if defined(ESP_PLATFORM)
    if (core != 0 && static_cast<float>(VoiceClockMicros() - started) >= kHelperShareMicros) break;
#endif
    const std::size_t index = g_nextVoiceRun.fetch_add(1, std::memory_order_relaxed);
    if (index >= g_voiceRunCount) break;
    const VoiceRun& run = g_voiceRuns[index];
    const std::int64_t begin = VoiceClockMicros();
    highest = (std::max)(highest, RenderVoiceRunNoLock(run, core));
    g_runCostSum[core][run.kind] += static_cast<float>(VoiceClockMicros() - begin);
    ++g_runCostCount[core][run.kind];
  }
  amy_embed::core_max[core] = highest;
}

#if defined(AMY_DUALCORE) && !defined(__EMSCRIPTEN__)
#if defined(ESP_PLATFORM)
constexpr std::uint32_t kHelperStackBytes = 5120;  // Measured: under 2 KB.
constexpr UBaseType_t kHelperPriority = 6;  // above the UI on that core: sound first
constexpr BaseType_t kHelperCore = 0;
TaskHandle_t g_helperTask = nullptr;
TaskHandle_t g_helperCaller = nullptr;

void AmyHelperTask(void*)
{
  for (;;)
  {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    RenderClaimedRunsNoLock(1);
    xTaskNotifyGive(g_helperCaller);
  }
}

bool StartHelper()
{
  if (g_helperTask == nullptr && xTaskCreatePinnedToCore(AmyHelperTask, "synth_audio2", kHelperStackBytes, nullptr, kHelperPriority, &g_helperTask, kHelperCore) != pdPASS)
  {
    g_helperTask = nullptr;
    return false;
  }
  g_helperCaller = xTaskGetCurrentTaskHandle();
  xTaskNotifyGive(g_helperTask);
  return true;
}

void WaitForHelper()
{
  ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
}
#else
std::mutex g_helperMutex;
std::condition_variable g_helperWake;
bool g_helperStarted = false;
bool g_helperWork = false;

bool StartHelper()
{
  {
    std::lock_guard<std::mutex> lock(g_helperMutex);
    g_helperWork = true;
  }
  if (!g_helperStarted)
  {
    g_helperStarted = true;
    std::thread([] {
      for (;;)
      {
        std::unique_lock<std::mutex> lock(g_helperMutex);
        g_helperWake.wait(lock, [] { return g_helperWork; });
        lock.unlock();
        RenderClaimedRunsNoLock(1);
        lock.lock();
        g_helperWork = false;
        g_helperWake.notify_all();
      }
    }).detach();
  }
  g_helperWake.notify_all();
  return true;
}

void WaitForHelper()
{
  std::unique_lock<std::mutex> lock(g_helperMutex);
  g_helperWake.wait(lock, [] { return !g_helperWork; });
}
#endif

void RenderAmyVoicesNoLock()
{
  const std::int64_t start = VoiceClockMicros();
  const std::uint32_t total = PlanVoiceRunsNoLock();
  KeepVoicesWithinBudgetNoLock();
  const bool shared = total >= kNoSplitBelowWeight && g_voiceRunCount > 1 && StartHelper();
  RenderClaimedRunsNoLock(0);


  if (shared) WaitForHelper();
  // Whatever the second core left in its buffer last time must not sound again.
  else amy_embed::amy_render(0, 0, 1);
  LearnVoiceCostsNoLock();
  g_voicePhaseMicros = VoiceClockMicros() - start;
}
#else
// One core here: the same runs, every one on it.
void RenderAmyVoicesNoLock()
{
  (void)PlanVoiceRunsNoLock();
  RenderClaimedRunsNoLock(0);
  LearnVoiceCostsNoLock();
}
#endif

void FetchAmyBlockNoLock(bool effects = false)
{
  EnsureAmyInitialized();
  StepAmyTrackGainsNoLock();
  std::array<FxHookTrack, kSynthTrackCount> plan{};
  if (effects)
  {
    plan = PlanFxAmyBlockNoLock();
    g_fxHook = plan;
  }
  amy_embed::amy_execute_deltas();
  RenderAmyVoicesNoLock();
  const output_sample_type* block = amy_embed::amy_fill_buffer();
  if (block != nullptr)
  {
    std::memcpy(g_amyBlock.data(), block, sizeof(std::int16_t) * g_amyBlock.size());
  }
  else
  {
    std::fill(g_amyBlock.begin(), g_amyBlock.end(), 0);
  }
  g_amyBlockCursor = 0;
  if (effects)
  {
    g_fxHook = {};
    PrepareFxAmyBlockNoLock(plan, true);
  }
  else
  {
    g_fx.blockPrepared = false;
  }
}

// Sampler tracks on the effects path: the legacy per-frame gain ramp and voice mix, with routed
// tracks summed into their bus first and then run through their inserts.
void RenderFxSamplerTracksNoLock(float* mix, float* delaySend, float* reverbSend, std::size_t frames,
                                 const std::array<float, kSynthTrackCount>& targets)
{
  for (std::size_t track = 0; track < g_engines.size(); ++track)
  {
    EngineState& engine = g_engines[track];
    if (!engine.configured || engine.kind != SynthEngineKind::Sampler)
    {
      continue;
    }
    TrackMixState& trackMix = g_trackMix[track];
    const bool routed = g_fx.insertRouted[track];
    if (!routed && !HasActiveSamplerVoices(engine))
    {
      for (std::size_t frame = 0; frame < frames; ++frame)
      {
        if (trackMix.samplerGain != targets[track])
        {
          trackMix.samplerGain = ApproachGain(trackMix.samplerGain, targets[track], kTrackGainStepPerFrame);
        }
      }
      continue;
    }

    float* out = routed ? g_fx.bus[track].get() : mix;
    if (routed)
    {
      std::fill_n(out, frames, 0.0f);
    }
    RenderSamplerTrackNoLock(engine, trackMix, targets[track], out, frames);
    if (routed)
    {
      MixRoutedTrackNoLock(track, out, mix, delaySend, reverbSend, frames);
    }
  }
}

// Every path ends here. The block is already rendered, so the limiter knows in advance the first
// sample that would go over and the gain the loudest one needs, and is there in time instead of
// clipping and reacting afterwards: polyphony gets a little quieter rather than distorted, for one
// pass over the block.
std::atomic<LineInput> g_lineInput{nullptr};

// The level rider in front of the limiter. When the limiter has to hold a dense mix well below the
// output gain block after block, every drum hit pumps everything else down and the sustained tracks
// disappear under the loud one (DEMO-2 on the board: 2 dB on average, dips of 6 dB, 2026-11-04). Then the
// whole mix comes down slowly instead (2 dB a second while the limiter takes more than 1.5 dB), and goes
// back up slowly (0.5 dB a second) once the limiter is nearly idle again, never below -6 dB, at once in
// silence. The limiter is left with the peaks;
// the balance between the tracks does not change, and a sparse mix never moves it.
float g_levelRider = 1.0f;
constexpr float kRiderFloor = 0.5f;
constexpr float kRiderDownPerFrame = 5.2e-6f;  // -2 dB a second
constexpr float kRiderUpPerFrame = 1.3e-6f;    // +0.5 dB a second
constexpr float kRiderSilence = 0.001f;        // the block's loudest output below -60 dB

SYNTH_FX_HOT void LimitOutputNoLock(const float* mix, std::int16_t* out, std::size_t frames)
{
  float gain = g_outputGain;
  const float wouldClip = kOutputCeiling / gain;
  float peak = 0.0f;
  std::size_t firstOver = frames;
  for (std::size_t offset = 0; offset < frames; ++offset)
  {
    const float level = std::fabs(mix[offset]);
    if (level > peak)
    {
      peak = level;
      if (level > wouldClip && firstOver == frames) firstOver = offset;
    }
  }
  // Silent even at full gain: the rider and the limiter are back where they rest, which nobody hears.
  const bool silent = peak * kOutputGain < kRiderSilence;
  if (silent) g_levelRider = 1.0f;
  const float target = kOutputGain * g_levelRider;
  float next = silent ? kOutputGain : (std::min)(target, gain + gain * kLimiterReleasePerFrame * static_cast<float>(frames));
  if (peak * next > kOutputCeiling)
  {
    next = kOutputCeiling / peak;
  }
  if (!silent && next < target * 0.84f)  // the limiter holds it more than 1.5 dB down
    g_levelRider = (std::max)(kRiderFloor, g_levelRider * (1.0f - kRiderDownPerFrame * static_cast<float>(frames)));
  else if (!silent && next > target * 0.97f)  // a quarter of a decibel
    g_levelRider = (std::min)(1.0f, g_levelRider * (1.0f + kRiderUpPerFrame * static_cast<float>(frames)));
  const std::size_t ramp = next < gain ? (std::min)(frames, (std::max)(firstOver, kLimiterShortestAttackFrames)) : frames;
  const float step = ramp > 0 ? (next - gain) / static_cast<float>(ramp) : 0.0f;
  for (std::size_t offset = 0; offset < ramp; ++offset)
  {
    gain += step;
    out[offset] = FloatToInt16(mix[offset] * gain);
  }
  for (std::size_t offset = ramp; offset < frames; ++offset)
  {
    out[offset] = FloatToInt16(mix[offset] * next);
  }
  g_outputGain = next;
}

// Every render path ends here: the line input joins the mix (at unity on the output: the output gain is
// taken out first), then the output gain and the limiter. Nothing after this clips.
void WriteOutputNoLock(const float* mix, std::int16_t* out, std::size_t frames)
{
  const LineInput line = g_lineInput.load(std::memory_order_relaxed);
  if (line == nullptr)
  {
    LimitOutputNoLock(mix, out, frames);
    return;
  }
  constexpr std::size_t kChunk = 256;
  constexpr float kLineToMix = 0.5f / 32768.0f / kOutputGain;  // both channels, then to mix units
  std::array<std::int16_t, 2 * kChunk> stereo;
  std::array<float, kChunk> joined;
  for (std::size_t done = 0; done < frames;)
  {
    const std::size_t count = (std::min)(frames - done, kChunk);
    const std::size_t got = (std::min)(line(stereo.data(), count), count);
    for (std::size_t frame = 0; frame < count; ++frame)
    {
      const float input = frame < got ? (static_cast<float>(stereo[2 * frame]) + static_cast<float>(stereo[2 * frame + 1])) * kLineToMix : 0.0f;
      joined[frame] = mix[done + frame] + input;
    }
    LimitOutputNoLock(joined.data(), out + done, count);
    done += count;
  }
}

void RenderFxFramesNoLock(std::int16_t* destination, std::size_t frameCount, bool hasAmyEngines)
{
  FxCore& core = *g_fx.core;
  std::array<float, kSynthTrackCount> samplerTargets{};
  for (std::size_t index = 0; index < g_engines.size(); ++index)
  {
    samplerTargets[index] = EffectiveTrackGain(g_trackMix[index]);
  }
  const auto value = [](fx::MasterParam param) { return MasterFxValue(param); };
  fx::dsp::SendBusSettings buses;
  buses.delaySeconds = fx::dsp::DelaySeconds(value(fx::MasterParam::DelayTime), g_fxTempo.load(std::memory_order_relaxed));
  buses.delayFeedback = value(fx::MasterParam::DelayFeedback);
  buses.delayTone = value(fx::MasterParam::DelayTone);
  buses.delayLevel = value(fx::MasterParam::DelayLevel);
  buses.reverbSize = value(fx::MasterParam::ReverbSize);
  buses.reverbDamp = value(fx::MasterParam::ReverbDamp);
  buses.reverbLowCut = value(fx::MasterParam::ReverbLowCut);
  buses.reverbLevel = value(fx::MasterParam::ReverbLevel);

  std::size_t frameIndex = 0;
  while (frameIndex < frameCount)
  {
    std::size_t base = 0;
    std::size_t frames = 0;
    if (hasAmyEngines)
    {
      if (g_amyBlockCursor >= kAmyFramesPerBlock)
      {
        FetchAmyBlockNoLock(true);
      }
      else if (!g_fx.blockPrepared)
      {
        // Joined in the middle of a block rendered without effects: AMY already mixed every voice.
        PrepareFxAmyBlockNoLock({}, false);
      }
      base = g_amyBlockCursor;
      frames = (std::min)(frameCount - frameIndex, kAmyFramesPerBlock - base);
    }
    else
    {
      frames = (std::min)(frameCount - frameIndex, kAmyFramesPerBlock);
      std::fill_n(core.mix.data(), frames, 0.0f);
      std::fill_n(core.delaySend.data(), frames, 0.0f);
      std::fill_n(core.reverbSend.data(), frames, 0.0f);
      g_fx.blockPrepared = false;
    }

    float* mix = core.mix.data() + base;
    float* delaySend = core.delaySend.data() + base;
    float* reverbSend = core.reverbSend.data() + base;
    UpdateTrackRoutingNoLock();
    RenderFxSamplerTracksNoLock(mix, delaySend, reverbSend, frames, samplerTargets);
    core.buses.Process(delaySend, reverbSend, mix, frames, buses);
    core.master.Process(mix, frames, value(fx::MasterParam::EqLow), value(fx::MasterParam::EqHigh), value(fx::MasterParam::Compressor),
                        value(fx::MasterParam::Tape));
    WriteOutputNoLock(mix, destination + frameIndex, frames);


    UpdateFxMetersNoLock(frames, core.buses.DelayPeak(), core.buses.ReverbPeak());

    if (hasAmyEngines)
    {
      g_amyBlockCursor += frames;
    }
    frameIndex += frames;
  }
}

void RenderAmyOnlyFramesNoLock(std::int16_t* destination, std::size_t frameCount)
{
  std::array<float, kAmyFramesPerBlock> mix;
  std::size_t frameIndex = 0;
  while (frameIndex < frameCount)
  {
    if (g_amyBlockCursor >= kAmyFramesPerBlock)
    {
      FetchAmyBlockNoLock();
    }

    const std::size_t framesAvailable = (std::min)(frameCount - frameIndex, kAmyFramesPerBlock - g_amyBlockCursor);
    for (std::size_t offset = 0; offset < framesAvailable; ++offset)
    {
      const std::size_t sampleIndex = (g_amyBlockCursor + offset) * kAmyChannelCount;
      mix[offset] = static_cast<float>(static_cast<std::int32_t>(g_amyBlock[sampleIndex]) +
                                       static_cast<std::int32_t>(g_amyBlock[sampleIndex + 1])) * kAmyBlockToMix;
    }
    WriteOutputNoLock(mix.data(), destination + frameIndex, framesAvailable);

    g_amyBlockCursor += framesAvailable;
    frameIndex += framesAvailable;
  }
}

std::size_t RenderFramesNoLock(std::int16_t* destination, std::size_t frameCount)
{
  if (destination == nullptr)
  {
    return 0;
  }

  (void)DrainPendingEvents(kEventBlockMicros, HeavyEventFitsNoLock());
  UpdateFxNoLock();

  SettleIdleSamplerGainsNoLock();

  const bool hasAmyEngines = HasAmyEnginesNoLock();
  const bool hasActiveSamplerVoices = HasActiveSamplerVoicesNoLock();
  PublishSampleStreamsNoLock();
  // The effects path starts as soon as anything needs it and ends on an AMY block boundary once
  // nothing is routed, the master chain is neutral and the delay and reverb tails have died out.
  UpdateTrackRoutingNoLock();
  if (FxFloatPathWantedNoLock())
  {
    g_fx.floatPath = true;
  }
  else if (g_fx.floatPath && (!hasAmyEngines || g_amyBlockCursor >= kAmyFramesPerBlock))
  {
    g_fx.floatPath = false;
    g_fx.blockPrepared = false;
  }
  if (g_fx.floatPath)
  {
    RenderFxFramesNoLock(destination, frameCount, hasAmyEngines);
    UpdateMetersNoLock(destination, frameCount);
    return frameCount;
  }
  UpdateFxMetersNoLock(frameCount, 0.0f, 0.0f);

  if (!hasAmyEngines && !hasActiveSamplerVoices)
  {
    std::fill(destination, destination + frameCount, 0);
    UpdateMetersNoLock(destination, frameCount);
    return frameCount;
  }

  if (hasAmyEngines && !hasActiveSamplerVoices)
  {
    RenderAmyOnlyFramesNoLock(destination, frameCount);
    UpdateMetersNoLock(destination, frameCount);
    return frameCount;
  }

  std::array<float, kSynthTrackCount> samplerTargets{};
  for (std::size_t index = 0; index < g_engines.size(); ++index)
  {
    samplerTargets[index] = EffectiveTrackGain(g_trackMix[index]);
  }

  std::array<float, kAmyFramesPerBlock> mixed;
  std::size_t mixedFrames = 0;
  for (std::size_t frameIndex = 0; frameIndex < frameCount; ++frameIndex)
  {
    // The sampler tracks first, a block at a time, into the frames about to be mixed.
    if (mixedFrames == 0)
    {
      const std::size_t chunk = (std::min)(mixed.size(), frameCount - frameIndex);
      std::fill_n(mixed.data(), chunk, 0.0f);
      // Muted voices keep running silently, so held notes resume on unmute.
      for (std::size_t index = 0; index < g_engines.size(); ++index)
        if (g_engines[index].configured && g_engines[index].kind == SynthEngineKind::Sampler)
          RenderSamplerTrackNoLock(g_engines[index], g_trackMix[index], samplerTargets[index], mixed.data(), chunk);
    }
    float mixedSample = mixed[mixedFrames];

    if (hasAmyEngines)
    {
      if (g_amyBlockCursor >= kAmyFramesPerBlock)
      {
        FetchAmyBlockNoLock();
      }

      const std::size_t sampleIndex = g_amyBlockCursor * kAmyChannelCount;
      mixedSample += (static_cast<float>(g_amyBlock[sampleIndex]) + static_cast<float>(g_amyBlock[sampleIndex + 1])) * kAmyBlockToMix;
      ++g_amyBlockCursor;
    }

    mixed[mixedFrames++] = mixedSample;
    if (mixedFrames == mixed.size() || frameIndex + 1 == frameCount)
    {
      WriteOutputNoLock(mixed.data(), destination + frameIndex + 1 - mixedFrames, mixedFrames);
      mixedFrames = 0;
    }
  }

  UpdateMetersNoLock(destination, frameCount);
  return frameCount;
}

}  // namespace

void EnsureEngine(std::size_t engineIndex, SynthEngineKind kind)
{
  if (engineIndex >= g_engines.size())
  {
    return;
  }

  PendingEvent event;
  event.type = PendingEventType::EnsureEngine;
  event.engineIndex = engineIndex;
  event.kind = kind;
  QueuePendingEvent(std::move(event));
}

void SetEnginePreset(std::size_t engineIndex, std::string_view presetId)
{
  if (engineIndex >= g_engines.size())
  {
    return;
  }

  PendingEvent event;
  event.type = PendingEventType::SetPreset;
  event.engineIndex = engineIndex;
  event.text = presetId;
  QueuePendingEvent(std::move(event));
}

void SetEngineParameter(std::size_t engineIndex, std::string_view parameterId, int value)
{
  if (engineIndex >= g_engines.size())
  {
    return;
  }

  PendingEvent event;
  event.type = PendingEventType::SetParameter;
  event.engineIndex = engineIndex;
  event.value = value;
  event.text = parameterId;
  QueuePendingEvent(std::move(event));
}

void SetSamplerFrames(std::size_t engineIndex, SharedSampleBuffer frames, std::uint32_t sampleRate)
{
  if (engineIndex >= g_engines.size())
  {
    return;
  }

  PendingEvent event;
  event.type = PendingEventType::SetSamplerFrames;
  event.engineIndex = engineIndex;
  event.frames = std::move(frames);
  event.sampleRate = sampleRate;
  QueuePendingEvent(std::move(event));
}

void NoteOn(std::size_t engineIndex, std::uint8_t note, std::uint8_t velocity)
{
  if (engineIndex >= g_engines.size() || note > 127)
  {
    return;
  }

  PendingEvent event;
  event.type = PendingEventType::NoteOn;
  event.engineIndex = engineIndex;
  event.note = note;
  event.velocity = velocity;
  QueuePendingEvent(std::move(event));
}

void NoteOff(std::size_t engineIndex, std::uint8_t note)
{
  if (engineIndex >= g_engines.size() || note > 127)
  {
    return;
  }

  PendingEvent event;
  event.type = PendingEventType::NoteOff;
  event.engineIndex = engineIndex;
  event.note = note;
  QueuePendingEvent(std::move(event));
}

void AllNotesOff(std::size_t engineIndex)
{
  if (engineIndex >= g_engines.size())
  {
    return;
  }

  PendingEvent event;
  event.type = PendingEventType::AllNotesOff;
  event.engineIndex = engineIndex;
  QueuePendingEvent(std::move(event));
}

void SetTrackGain(std::size_t engineIndex, float gain)
{
  if (engineIndex >= g_engines.size())
  {
    return;
  }

  // NaN and negative values close the fader.
  gain = gain > 0.0f ? (std::min)(gain, 1.0f) : 0.0f;
  PendingEvent event;
  event.type = PendingEventType::SetTrackGain;
  event.engineIndex = engineIndex;
  event.gain = gain;
  // Publish and queue under one lock, so concurrent setters cannot reorder the two.
  std::lock_guard<std::mutex> lock(g_pendingEventMutex);
  if (g_publishedMix[engineIndex].gain.exchange(gain, std::memory_order_relaxed) != gain)
  {
    g_pendingEvents.push_back(std::move(event));
  }
}

float TrackGain(std::size_t engineIndex)
{
  return engineIndex < g_publishedMix.size() ? g_publishedMix[engineIndex].gain.load(std::memory_order_relaxed) : 1.0f;
}

void SetTrackMuted(std::size_t engineIndex, bool muted)
{
  if (engineIndex >= g_engines.size())
  {
    return;
  }

  PendingEvent event;
  event.type = PendingEventType::SetTrackMuted;
  event.engineIndex = engineIndex;
  event.value = muted ? 1 : 0;
  std::lock_guard<std::mutex> lock(g_pendingEventMutex);
  if (g_publishedMix[engineIndex].muted.exchange(muted, std::memory_order_relaxed) != muted)
  {
    g_pendingEvents.push_back(std::move(event));
  }
}

bool TrackMuted(std::size_t engineIndex)
{
  return engineIndex < g_publishedMix.size() && g_publishedMix[engineIndex].muted.load(std::memory_order_relaxed);
}

float TrackActivity(std::size_t engineIndex)
{
  return engineIndex < g_publishedMix.size() ? g_publishedMix[engineIndex].activity.load(std::memory_order_relaxed) : 0.0f;
}

float OutputPeak()
{
  return g_outputPeak.load(std::memory_order_relaxed);
}

void SetMasterFx(fx::MasterParam param, int value)
{
  const auto index = static_cast<std::size_t>(param);
  if (index >= fx::kMasterParamCount)
  {
    return;
  }
  value = std::clamp(value, fx::kMasterParams[index].minValue, fx::kMasterParams[index].maxValue);
  PendingEvent event;
  event.type = PendingEventType::SetMasterFx;
  event.note = static_cast<std::uint8_t>(index);
  event.value = value;
  // Publish and queue under one lock, so concurrent setters cannot reorder the two.
  std::lock_guard<std::mutex> lock(g_pendingEventMutex);
  if (g_publishedMasterFx[index].exchange(value, std::memory_order_relaxed) != value)
  {
    g_pendingEvents.push_back(std::move(event));
  }
}

int MasterFx(fx::MasterParam param)
{
  const auto index = static_cast<std::size_t>(param);
  return index < fx::kMasterParamCount ? g_publishedMasterFx[index].load(std::memory_order_relaxed) : 0;
}

void SetTempo(float bpm)
{
  if (bpm > 0.0f && std::isfinite(bpm))
  {
    g_fxTempo.store(bpm, std::memory_order_relaxed);
  }
}

float FxReturnLevel(std::size_t bus)
{
  return bus < fx::kBusCount ? g_fxReturnLevel[bus].load(std::memory_order_relaxed) : 0.0f;
}

bool FxBusAvailable(std::size_t bus)
{
  return bus < fx::kBusCount && !g_fxBusFailed[bus].load(std::memory_order_relaxed);
}

void SuspendEffects()
{
  std::lock_guard<std::mutex> lock(g_renderMutex);

  if (g_fxSuspended++ > 0) return;
  // The float path and the voice hook point into the core: both end first.
  g_fx.floatPath = false;
  g_fx.blockPrepared = false;
  g_fxHook = {};
  g_fx.routed.fill(false);
  g_fx.insertRouted.fill(false);
  for (auto& bus : g_fx.bus) bus.reset();
  g_fx.core.reset();
}

void ResumeEffects()
{

  // The new project's events first, a batch at a time while the effects still rest, so the audio task
  // renders between the batches (all of them under one hold of the lock ran the output dry when a
  // project loaded).
  for (bool more = true; more;)
  {
    std::lock_guard<std::mutex> lock(g_renderMutex);
    more = DrainPendingEvents(kEventBlockMicros);
  }
  std::lock_guard<std::mutex> lock(g_renderMutex);
  if (g_fxSuspended == 0 || --g_fxSuspended > 0) return;
  // The memory is taken here and now, with the settings already sent, so whatever the caller does
  // next sees the heap as it is (from the render thread it came a moment later, while the next
  // project was being built).
  g_fx.dirty = true;
  DrainPendingEvents();
  UpdateFxNoLock();
}

void WarmUp()
{
#if defined(ESP_PLATFORM)
  // While the output holds (a project being built) what the engine just queued is applied here and
  // now; otherwise the audio task applies it, the heavy events when the output can spare their time.
  // Rendering a block here instead held the lock for 20-25 ms (from the UI's core, beside the helper),
  // ran the output dry and clicked at every project load (the stress test, 2026-11-04).
  if (!g_outputHeld.load(std::memory_order_acquire)) return;
  for (bool more = true; more;)
  {
    std::lock_guard<std::mutex> lock(g_renderMutex);
    more = DrainPendingEvents(kEventBlockMicros);
  }
#else
  std::array<std::int16_t, kAmyFramesPerBlock> scratch{};
  std::lock_guard<std::mutex> lock(g_renderMutex);
  (void)RenderFramesNoLock(scratch.data(), scratch.size());
#endif
}

namespace {

// If it still does not fit (the budget, KeepVoicesWithinBudgetNoLock, guessed wrong): the sound must
// never fall behind time, because that is what makes everything slow down and break up. Rendering that
// keeps taking longer than it lasts sheds a voice (RenderLoad): the one the budget would choose, faded.
RenderLoad g_renderLoad;
std::atomic<std::uint32_t> g_shedVoices{0};

std::int64_t RenderClockMicros()
{
#if defined(ESP_PLATFORM)
  return esp_timer_get_time();
#else
  return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
#endif
}

// When rendering still cannot keep up (RenderLoad, a safety net behind the budget): the same voice the
// budget would choose, faded out over the next block.
bool ShedVoiceNoLock()
{
  // The output has lost 12 ms to the speaker (RenderLoad): a held note may go too, tails first.
  const int victim = VoiceFadeVictimNoLock();
  if (victim < 0) return false;
  CountGivenUpNoLock(static_cast<std::uint16_t>(victim));
  MarkVoiceFading(static_cast<std::uint16_t>(victim));
  g_shedVoices.fetch_add(1, std::memory_order_relaxed);
  return true;
}

std::array<std::array<std::atomic<std::uint32_t>, 4>, kSynthTrackCount> g_heldNotes{};
std::atomic<std::uint32_t> g_orphanVoices{0};
std::uint32_t g_orphanVoiceCheckBlocks = 0;

void PublishHeldNotesNoLock()
{
  for (std::size_t track = 0; track < g_engines.size(); ++track)
  {
    const EngineState& engine = g_engines[track];
    std::array<std::uint32_t, 4> held{};
    if (engine.configured && engine.kind != SynthEngineKind::Drums)
    {
      if (UsesAmy(engine.kind))
      {
        for (std::size_t note = 0; note < engine.activeNotes.size(); ++note)
          if (engine.activeNotes[note]) held[note / 32] |= 1u << (note % 32);
      }
      else
      {
        for (const SamplerVoice& voice : engine.samplerVoices)
          if (voice.active && !voice.releasing) held[voice.note / 32] |= 1u << (voice.note % 32);
      }
    }
    for (std::size_t word = 0; word < held.size(); ++word) g_heldNotes[track][word].store(held[word], std::memory_order_relaxed);
  }
}

// A track that holds no note has no voice in its held part: if AMY has one (its voice table and ours
// disagree), it gets a note-off from here. About three times a second is plenty for a safety net.
void ReleaseOrphanVoicesNoLock()
{
  if (++g_orphanVoiceCheckBlocks < 64 || !g_amyInitialized) return;
  g_orphanVoiceCheckBlocks = 0;
  for (EngineState& engine : g_engines)
  {
    if (!engine.configured || !UsesAmy(engine.kind) || engine.kind == SynthEngineKind::Drums) continue;
    if (std::any_of(engine.activeNotes.begin(), engine.activeNotes.end(), [](bool active) { return active; }))
    {
      engine.orphanVoiceSeen = false;
      continue;
    }
    std::array<std::uint16_t, MAX_VOICES_PER_INSTRUMENT> voices{};
    const int voiceCount = amy_embed::instrument_get_num_voices(engine.synthId, voices.data());
    const auto held = [](const amy_embed::synthinfo* root) {
      return root != nullptr && root->status == SYNTH_AUDIBLE && root->note_off_clock == std::numeric_limits<std::uint32_t>::max();
    };
    bool orphan = false;
    for (int voice = 0; voice < voiceCount && !orphan; ++voice) orphan = held(amy_embed::synth[amy_embed::voice_to_base_osc[voices[voice]]]);
    // Second look: a note-off just sent may still be on its way through AMY's queue.
    const bool confirmed = orphan && engine.orphanVoiceSeen;
    engine.orphanVoiceSeen = orphan && !confirmed;
    if (!confirmed) continue;
    g_orphanVoices.fetch_add(1, std::memory_order_relaxed);
    // Through the voice table for the notes it knows, and to the voices themselves for the ones it
    // has lost: a table that disagrees with the voices is how they got here.
    amy_event event = amy_default_event();
    event.synth = static_cast<std::uint8_t>(engine.synthId);
    event.velocity = 0;
    amy_add_event(&event);
    for (int voice = 0; voice < voiceCount; ++voice)
    {
      const std::uint16_t base = amy_embed::voice_to_base_osc[voices[voice]];
      if (!held(amy_embed::synth[base])) continue;
      // AMY's events no longer name a voice: a note-off to the voice's first oscillator is what a
      // voice's note-off was (its filter and envelope live there).
      amy_event release = amy_default_event();
      release.osc = base;
      release.velocity = 0;
      amy_add_event(&release);
    }
  }
}

}  // namespace

std::size_t RenderFrames(std::int16_t* destination, std::size_t frameCount)
{
#if defined(ESP_PLATFORM)
  const std::int64_t asked = RenderClockMicros();
  if (g_outputHeld.load(std::memory_order_acquire) && g_holdRequests.load(std::memory_order_acquire) > 0)
  {
    // A project is being built: silence, without the render lock; it costs nothing, so the queue fills.
    std::fill_n(destination, frameCount, std::int16_t{0});
    g_outputAheadMicros = kOutputQueueMicros;
    return frameCount;
  }
#endif
  std::lock_guard<std::mutex> lock(g_renderMutex);

  const std::int64_t start = RenderClockMicros();
  const std::size_t rendered = RenderFramesNoLock(destination, frameCount);


  const std::int64_t elapsed = RenderClockMicros() - start;
  const std::int64_t budget = static_cast<std::int64_t>(frameCount) * 1000000 / kSampleRate;
  // Everything but the voices, for the voice budget: one block per call is the case that tells.
  if (g_voicePhaseMicros > 0 && frameCount == kAmyFramesPerBlock)
    g_overheadMicros += 0.05f * (static_cast<float>(elapsed - g_voicePhaseMicros) - g_overheadMicros);
  g_voicePhaseMicros = 0;
  if (frameCount == kAmyFramesPerBlock)
  {
    constexpr float kTarget = kVoiceBudgetShare * kBlockMicros;
    g_renderAverageMicros += 0.05f * (static_cast<float>(elapsed) - g_renderAverageMicros);
    g_budgetScale = (std::clamp)(g_budgetScale + kBudgetScaleGain * (kTarget - g_renderAverageMicros) / kTarget, kBudgetScaleFloor, 1.0f);
  }
  if (g_renderLoad.Add(elapsed, budget)) (void)ShedVoiceNoLock();
  PublishHeldNotesNoLock();
  ReleaseOrphanVoicesNoLock();

#if defined(ESP_PLATFORM)
  // What the output gained or lost on the speaker with this block, waiting for the lock included.
  g_outputAheadMicros = (std::clamp)(g_outputAheadMicros + static_cast<float>(budget - (RenderClockMicros() - asked)), 0.0f, kOutputQueueMicros);
  // Into the hold: this block fades out, the next ones are silence. Out of it: the first fades in.
  const bool holdWanted = g_holdRequests.load(std::memory_order_acquire) > 0;
  const bool held = g_outputHeld.load(std::memory_order_relaxed);
  if (holdWanted != held && rendered > 0)
  {
    for (std::size_t frame = 0; frame < rendered; ++frame)
    {
      const std::int32_t weight = static_cast<std::int32_t>(holdWanted ? rendered - frame : frame);
      destination[frame] = static_cast<std::int16_t>(static_cast<std::int32_t>(destination[frame]) * weight / static_cast<std::int32_t>(rendered));
    }
    g_outputHeld.store(holdWanted, std::memory_order_release);
  }
#endif
  return rendered;
}

void HoldOutput()
{
#if defined(ESP_PLATFORM)
  if (g_holdRequests.fetch_add(1, std::memory_order_acq_rel) > 0) return;
  // The audio task fades out within a block or two; without one running (paused for the microphone,
  // not started yet) this gives up after a while and the building goes on as it would have.
  for (int waited = 0; waited < 8 && !g_outputHeld.load(std::memory_order_acquire); ++waited)
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
#endif
}

void ReleaseOutput()
{
#if defined(ESP_PLATFORM)
  g_holdRequests.fetch_sub(1, std::memory_order_acq_rel);
#endif
}

void SetLineInput(LineInput input)
{
  g_lineInput.store(input, std::memory_order_relaxed);
}

bool ShedVoice()
{
  std::lock_guard<std::mutex> lock(g_renderMutex);
  return ShedVoiceNoLock();
}

std::uint32_t ShedVoiceCount()
{
  return g_shedVoices.load(std::memory_order_relaxed);
}

std::uint32_t FadedVoiceCount()
{
  return g_fadedVoices.load(std::memory_order_relaxed);
}

RenderModel CurrentRenderModel()
{
  RenderModel model;
  model.dx7Micros = g_runCostMicros[static_cast<std::size_t>(SynthEngineKind::Dx7)];
  model.junoMicros = g_runCostMicros[static_cast<std::size_t>(SynthEngineKind::Juno)];
  model.drumsMicros = g_runCostMicros[static_cast<std::size_t>(SynthEngineKind::Drums)];
  model.overheadMicros = g_overheadMicros;
  model.budgetScale = g_budgetScale;
  return model;
}

GivenUpVoices GivenUpVoiceCounts()
{
  return {g_lostTails.load(std::memory_order_relaxed), g_lostHeld.load(std::memory_order_relaxed),
          g_lostUnheard.load(std::memory_order_relaxed) + g_droppedNoteOns.load(std::memory_order_relaxed), g_lostStolen.load(std::memory_order_relaxed)};
}

NoteSet HeldNotes(std::size_t engineIndex)
{
  NoteSet held{};
  if (engineIndex >= g_heldNotes.size()) return held;
  for (std::size_t word = 0; word < held.size(); ++word) held[word] = g_heldNotes[engineIndex][word].load(std::memory_order_relaxed);
  return held;
}

std::uint32_t ReleasedOrphanVoiceCount()
{
  return g_orphanVoices.load(std::memory_order_relaxed);
}

}  // namespace synth::audio
