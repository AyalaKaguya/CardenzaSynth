#include "Applications/Synth/GrooveApp.h"
#include "Applications/Synth/StressTest.h"

#include "Library/Memory/Memory.h"
#include "Library/Synth/Audio/SampleStream.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <memory>
#include <atomic>
#include <new>
#include <set>
#include <utility>

#if defined(ESP_PLATFORM)
#include <dirent.h>
#else
#include <filesystem>
#endif

#include "Applications/Synth/Runtime/ProjectStorage.h"
#include "Applications/Synth/Runtime/SynthStorage.h"
#include "Applications/Synth/UI/Groove/Render/Gfx.h"
#include "Applications/Synth/UI/Groove/Render/GrooveRenderer.h"
#include "Devices/Device.h"
#include "Devices/LedLight.h"
#include "Library/Synth/Audio/SynthAudioRuntime.h"
#include "OS/MIDI/MIDI.h"
#include "OS/Storage/DeviceSettings.h"
#include "OS/Storage/Settings.h"
#include "OS/Storage/Storage.h"

namespace apps {

namespace {

// The autosave (SessionStore): written once the keys have rested this long, none held; while keys keep
// coming, or a sample plays from the card, this long at most after the first change it does not have.
constexpr std::uint32_t kSessionQuietMs = 1500;
constexpr std::uint32_t kSessionLongestWaitMs = 60000;
// After a failed autosave: tried again this soon, and once it keeps failing (kSessionFailuresBeforeSlow
// in a row) this seldom.
constexpr std::uint32_t kSessionRetryMs = 2000;
constexpr std::uint32_t kSessionFailedRetryMs = 30000;
constexpr std::uint32_t kSessionFailuresBeforeSlow = 3;
// A write that took this long says so in the log (the system loop's, or the background task's).
constexpr std::uint32_t kSessionSlowSaveMs = 30;
constexpr std::uint32_t kSessionSlowCardMs = 500;
constexpr std::uint32_t kCardCheckMs = 2000;
constexpr std::uint32_t kCardProbeMs = 10000;
constexpr std::uint32_t kOrphanCheckMs = 200;
constexpr std::uint32_t kOrphanConfirmMs = 50;
#if defined(ESP_PLATFORM)
// What the autosave's snapshot and its file need at the same time (many small plain allocations, and
// the file's buffers, which the card's DMA needs in internal RAM).
constexpr std::size_t kSessionHeapBytes = 12 * 1024;
constexpr std::size_t kSessionBlockBytes = 5 * 1024;
constexpr std::size_t kProjectSaveHeapBytes = 8 * 1024;
#endif
// Engines: what has to stay free when one is built, for everything that allocates on the side (the UI,
// AMY's voices, the autosave). A kind's size is learnt from what the heap gets back when an engine of
// it is destroyed (the boot replaces all four defaults with the working state); before that it is
// estimated, from what each takes on the host (more than on the device: 64-bit pointers). How much the
// heap shrinks while one is built does not work: it counts what other tasks and first-time setup take
// meanwhile, which made a drum engine 46 KB at boot on the board and refused every load (2026-11-03).
constexpr std::size_t kEngineHeapFloorBytes = 16 * 1024;
// The largest single block an engine takes: its parameters and presets are kept in small blocks
// (PagedSynthEngine), the largest is a sampler's 1 KB on the host.
constexpr std::size_t kEngineBlockBytes = 2 * 1024;
// Something else given back at the same moment: a size beyond this many estimates is not believed.
constexpr std::size_t kEngineSizeTrustFactor = 3;

std::size_t EngineEstimateBytes(synth::SynthEngineKind kind)
{
  // With PSRAM an engine's parameters and presets are there, and what it keeps in internal RAM is a
  // few hundred bytes (260 to 1750 measured, 2026-11-04). Its voices' oscillator state is AMY's,
  // which yields internal RAM to everything else (AmyEmbeddedStubs.c) and is given back with the
  // voices, at the render thread's pace: a measure taken while it is given back once read a Juno at
  // 15 KB and refused every project after it. The estimate also bounds what a measure may claim.
  if (memory::HasSlow()) return kind == synth::SynthEngineKind::None ? 0 : 2 * 1024;
  switch (kind)
  {
    case synth::SynthEngineKind::Dx7: return 14 * 1024;
    case synth::SynthEngineKind::Juno: return 6 * 1024;
    case synth::SynthEngineKind::Sampler: return 6 * 1024;
    case synth::SynthEngineKind::Drums: return 4 * 1024;
    case synth::SynthEngineKind::None:
    default: return 0;
  }
}

std::size_t KindIndex(synth::SynthEngineKind kind)
{
  const auto index = static_cast<std::size_t>(kind);
  return index < 5 ? index : 0;
}

os::storage::Settings& AppSettings()
{
  return os::storage::AppSettings(synth::storage::kAppId);
}

std::unique_ptr<app::Application> CreateGrooveApplication()
{
  return std::make_unique<GrooveApp>();
}

app::ApplicationInfo BuildGrooveInfo()
{
  app::ApplicationInfo info;
  info.id = app::MakeApplicationId("Cardenza", "Synth");
  info.name = "Groovebox";
  info.author = "Cardenza";
  info.factory = &CreateGrooveApplication;
  return info;
}

bool HasWavExtension(const std::string& name)
{
  if (name.size() < 5) return false;
  std::string extension = name.substr(name.size() - 4);
  std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return extension == ".wav";
}

// The autosave's writer (below, by SaveSessionToCard).
void ForgetCardSession();

}  // namespace

void RegisterGrooveApplication()
{
  static const bool registered = app::RegisterApplication(BuildGrooveInfo());
  (void)registered;
}

GrooveApp::GrooveApp() : controller_(*this) {}

GrooveApp::~GrooveApp()
{
  // Silence the engines and hand the sample memory back before the next app starts.
  sampler_.KeepTakesOnCard();
  sequencer_.ArpReleaseAll();
  sequencer_.Stop();
  for (std::size_t track = 0; track < synth::kSynthTrackCount; ++track)
  {
    synth::audio::AllNotesOff(track);
    sampler_.OnTrackEngineChanged(track, synth::SynthEngineKind::None);
  }
}

std::uint32_t GrooveApp::NowMs() const
{
  if (clock_ != nullptr) return clock_();
  using namespace std::chrono;
  return static_cast<std::uint32_t>(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

void GrooveApp::Setup(const std::vector<std::string>& args)
{
  // Explicit volatile hosts (the minimal Mosaico port) have no storage or keys.
  // Keep the normal instrument screen reachable without dismissing a card prompt.
  const bool volatileSession = std::find(args.begin(), args.end(), "--volatile-session") != args.end();
  if (initialized_) return;
  os::midi::Initialize();
  // What a computer plays over USB is not heard for now: the device is its microphone only (the user,
  // 2026-11-06; mixed in, it also came back in what the computer recorded). The channel stays, the
  // device drops what nobody reads; synth::audio::SetLineInput(&Device::Audio::ReadComputerAudio)
  // would mix it in at unity.
  synth::audio::SetLineInput(nullptr);
  sequencer_.SetMidiSink(&GrooveApp::SendSequencerMidi, this);
  for (std::size_t track = 0; track < synth::kSynthTrackCount; ++track)
  {
    rack_.AttachEngine(track, kDefaultKinds[track]);
    sampler_.OnTrackEngineChanged(track, kDefaultKinds[track]);
  }
  storageReady_ = synth::storage::EnsureLayout();
  ApplyMix();
  ApplyMasterFx(synth::ProjectData::DefaultMasterFx());
  // Volume, key LEDs and screen are device settings: the system applied them at boot.
  RestorePlaySettings();
  // The autosave is on the card: the working state comes back with it.
  (void)RestoreSessionState();
  cardWasMounted_ = os::storage::CardMounted();
  if (cardWasMounted_)
  {
    const int take = sampler_.RestoreTakes();
    if (take >= 0) controller_.ResumeTake(static_cast<std::size_t>(take));
  }
  // Without a card nothing is kept: said in a notice that waits for ENTER or ESC, so it is still there
  // when the boot animation ends.
  if (os::storage::CardMissingThisRun() && !volatileSession) controller_.ShowNoCardNotice();
  initialized_ = true;
}

void GrooveApp::Suspend()
{
  // A lesson's practice project is never kept: the user's working state comes back first.
  ExitGuide();
  StorePlaySettings();
  sampler_.KeepTakesOnCard();
  (void)SaveSessionState();
}

void GrooveApp::RestorePlaySettings()
{
  const auto& settings = AppSettings();
  groove::ProjectUiSettings ui;
  groove::KeyboardSettings& keys = ui.keyboard;
  const groove::KeyboardSettings defaults{};
  keys.layout = static_cast<groove::KeyboardLayout>(
      settings.GetInt("keys.layout", static_cast<int>(defaults.layout), 0, static_cast<int>(groove::kLayoutNames.size()) - 1));
  keys.scale = static_cast<std::size_t>(settings.GetInt("keys.scale", static_cast<int>(defaults.scale), 0, static_cast<int>(groove::kScales.size()) - 1));
  keys.root = settings.GetInt("keys.root", defaults.root, 0, 11);
  keys.octave = settings.GetInt("keys.octave", defaults.octave, -3, 3);
  keys.lock = static_cast<groove::ScaleLock>(
      settings.GetInt("keys.lock", static_cast<int>(defaults.lock), 0, static_cast<int>(groove::kScaleLockNames.size()) - 1));
  keys.multiSelect = settings.GetInt("keys.select", defaults.multiSelect ? 1 : 0, 0, 1) != 0;
  keys.velocity = static_cast<std::uint8_t>(settings.GetInt("keys.velocity", defaults.velocity, 1, 127));
  // The keyboard belongs to the project now; the values older firmware kept in the app settings
  // only start a session that has no project of its own (the session file overrides them).
  controller_.Restore(ui, "");
  storedMultiSelect_ = keys.multiSelect;
}

void GrooveApp::StorePlaySettings()
{
  // Polled every tick. The system writes the file a second after the last change, and when the app
  // is suspended.
  const bool multiSelect = controller_.Settings().keyboard.multiSelect;
  if (multiSelect == storedMultiSelect_) return;
  AppSettings().SetInt("keys.select", multiSelect ? 1 : 0);
  storedMultiSelect_ = multiSelect;
}

void GrooveApp::ApplyPlaySettings(const synth::ProjectData& project, groove::ProjectUiSettings& settings)
{
  if (!project.hasPlay) return;
  groove::KeyboardSettings& keys = settings.keyboard;
  keys.octave = std::clamp(project.octave, -3, 3);
  keys.root = std::clamp(project.root, 0, 11);
  keys.scale = std::min(project.scale, groove::kScales.size() - 1);
  keys.layout = static_cast<groove::KeyboardLayout>(std::clamp(project.layout, 0, static_cast<int>(groove::kLayoutNames.size()) - 1));
  keys.lock = static_cast<groove::ScaleLock>(std::clamp(project.lock, 0, static_cast<int>(groove::kScaleLockNames.size()) - 1));
  keys.velocity = static_cast<std::uint8_t>(std::clamp(project.velocity, 1, 127));
  for (std::size_t track = 0; track < synth::kSynthTrackCount; ++track)
  {
    const auto& play = project.play[track];
    settings.chords[track].chord = static_cast<groove::ChordType>(std::clamp(play.chord, 0, static_cast<int>(groove::kChordTypeCount) - 1));
    settings.chords[track].voicing = static_cast<std::uint8_t>(std::clamp(play.voicing, 0, static_cast<int>(groove::kVoicingNames.size()) - 1));
    sequencer_.SetArpSettings(track, play.arp);
  }
}

synth::SessionInfo GrooveApp::NextSessionInfo()
{
  const auto& view = controller_.State();
  synth::SessionInfo info;
  info.name = view.projectName;
  info.dirty = view.dirty;
  info.generation = ++sessionGeneration_;
  if (info.generation == 0) info.generation = ++sessionGeneration_;  // 0 means none
  return info;
}

bool GrooveApp::RestoreSessionState()
{
  // No card, no autosave. And none on it: nothing counted.
  synth::SessionInfo info;
  if (!os::storage::CardMounted() || !synth::SessionStore::ReadInfo(info)) return false;
  if (!synth::SessionStore::BeginRestore())
  {
    std::printf("Groove: autosave skipped: the last starts with it did not finish\n");
    return false;
  }
  synth::ProjectData project;
  std::string message;
  const bool found = synth::SessionStore::Load(info, project);
  sessionGeneration_ = std::max(sessionGeneration_, info.generation);
  const bool restored = found && ReplaceSession(info.name, project, false, message, true);
  std::printf("Groove: autosave generation %u: %s\n", static_cast<unsigned>(info.generation),
              restored ? "restored" : found ? message.c_str() : "unreadable");
  if (restored)
  {
    groove::ProjectUiSettings ui = controller_.Settings();
    // Every project opens on the first track, whichever it was left on.
    ui.activeTrack = synth::kDefaultActiveTrack;
    ApplyPlaySettings(project, ui);
    // The session comes back with its unsaved state, so loading another project still asks first.
    controller_.Restore(ui, synth::IsProjectNameValid(info.name) ? info.name : std::string(), info.dirty);
  }
  synth::SessionStore::FinishRestore();
  return restored;
}

// Nothing that could be heard, and no finger on a key.
bool GrooveApp::Quiet(std::uint32_t now) const
{
  return now - lastKeyAt_ >= kSessionQuietMs && keysHeld_ == 0 && !sequencer_.Playing() && sampler_.RecordingTrack() < 0;
}

// The card comes and goes on its own: what waits for it gets it. A run that started without one has
// nothing to look for.
void GrooveApp::CheckCard(std::uint32_t now)
{
  if (os::storage::CardMissingThisRun() || now - lastCardCheckAt_ < kCardCheckMs) return;
  lastCardCheckAt_ = now;
  // Looking for a card that is not there takes a moment: only for something that waits for it (a
  // sample, the autosave), and only when nobody is playing.
  const bool waiting = sessionChanged_ ||
                       std::any_of(pendingSamples_.begin(), pendingSamples_.end(), [](const std::string& name) { return !name.empty(); });
  if (!cardWasMounted_ && waiting && Quiet(now) && now - lastCardProbeAt_ >= kCardProbeMs)
  {
    lastCardProbeAt_ = now;
    (void)os::storage::CardPresent();
  }
  const bool mounted = os::storage::CardMounted();
  if (mounted == cardWasMounted_) return;
  cardWasMounted_ = mounted;
  if (!mounted) return;
  // It may be another card: the autosave is written whole the next time.
  ForgetCardSession();
  storageReady_ = synth::storage::EnsureLayout();
  if (!storageReady_) return;
  synth::audio::RetrySampleStreams();
  ApplyPendingSamples();
  const int take = sampler_.RestoreTakes();
  if (take >= 0) controller_.ResumeTake(static_cast<std::size_t>(take));
}

// The samples the autosave names, for when it came back before the card did.
void GrooveApp::ApplyPendingSamples()
{
  for (std::size_t track = 0; track < synth::kSynthTrackCount; ++track)
  {
    if (pendingSamples_[track].empty()) continue;
    const std::string name = std::move(pendingSamples_[track]);
    pendingSamples_[track].clear();
    if (rack_.Slot(track).kind != synth::SynthEngineKind::Sampler || !sampler_.BuildUiState(track).sampleFileName.empty()) continue;
    synth::SamplerManager::PreparedSample sample;
    if (!synth::SamplerManager::PrepareSample(name, sample)) continue;
    (void)sampler_.ApplySample(track, sample);
    // The engine already carries the saved trim.
    (void)sampler_.ConsumeTrimReset(track);
  }
}

namespace {

// The autosave on its way to the card: taken in the system loop, written on the device's background
// task (a project file takes the card 100 ms to seconds), while the music plays on. Its files have one
// writer at a time: the background task, or the system loop writing it now (SaveSessionState), which
// first drops a snapshot still waiting so that nothing older lands after it.
struct CardSessionWrite {
  std::mutex files;
  std::mutex mutex;
  std::unique_ptr<synth::ProjectData> project;  // under mutex: waiting to be written, null when none
  synth::SessionInfo info;                       // under mutex
  bool writing = false;                          // under mutex
  int result = 0;                                // under mutex: 1 written, 2 unchanged, -1 failed, 0 nothing new
  std::uint32_t tookMs = 0;                      // under mutex
  // What the card holds, as ProjectFingerprint with the name and the unsaved mark; 0 = unknown. Only
  // the background task (under files) and a card coming back (under files) touch it.
  std::uint32_t written = 0;
};
CardSessionWrite g_cardSession;

std::uint32_t SessionFingerprint(const synth::SessionInfo& info, const synth::ProjectData& project)
{
  std::uint32_t hash = synth::ProjectFingerprint(project);
  for (const char c : info.name + (info.dirty ? "\n1" : "\n0")) hash = (hash ^ static_cast<unsigned char>(c)) * 16777619u;
  return hash | 1u;
}

void WriteCardSession()
{
  std::lock_guard<std::mutex> files(g_cardSession.files);
  std::unique_ptr<synth::ProjectData> project;
  synth::SessionInfo info;
  {
    std::lock_guard<std::mutex> lock(g_cardSession.mutex);
    if (!g_cardSession.project) return;
    project = std::move(g_cardSession.project);
    info = g_cardSession.info;
    g_cardSession.writing = true;
  }
  const auto started = std::chrono::steady_clock::now();
  // A key that changed nothing writes nothing.
  const std::uint32_t fingerprint = SessionFingerprint(info, *project);
  const bool unchanged = fingerprint == g_cardSession.written;
  const bool written = unchanged || synth::SessionStore::Save(info, *project);
  g_cardSession.written = written ? fingerprint : 0;
  project.reset();
  const auto took = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
  std::lock_guard<std::mutex> lock(g_cardSession.mutex);
  g_cardSession.writing = false;
  g_cardSession.result = !written ? -1 : unchanged ? 2 : 1;
  g_cardSession.tookMs = static_cast<std::uint32_t>(took);
}

bool CardSessionBusy()
{
  std::lock_guard<std::mutex> lock(g_cardSession.mutex);
  return g_cardSession.project != nullptr || g_cardSession.writing;
}

// The card was out or another one is in: what it holds is not known.
void ForgetCardSession()
{
  std::lock_guard<std::mutex> files(g_cardSession.files);
  g_cardSession.written = 0;
}

}  // namespace

bool GrooveApp::SaveSessionState()
{
  // The card may have been out: asked for now (the device limits how often it probes).
  storageReady_ = os::storage::CardPresent() && synth::storage::EnsureLayout();
  synth::ProjectData project;
  std::string message;
  if (!storageReady_ || !CaptureProject(controller_.Settings(), project, message, true)) return false;
  const synth::SessionInfo info = NextSessionInfo();
  const std::uint32_t started = NowMs();
  bool written = false;
  {
    // An older snapshot still waiting is not written after this one; one being written finishes first.
    {
      std::lock_guard<std::mutex> lock(g_cardSession.mutex);
      g_cardSession.project.reset();
    }
    std::lock_guard<std::mutex> files(g_cardSession.files);
    written = synth::SessionStore::Save(info, project);
    g_cardSession.written = 0;
    std::lock_guard<std::mutex> lock(g_cardSession.mutex);
    g_cardSession.result = 0;
  }
  const std::uint32_t took = NowMs() - started;
  if (written)
  {
    sessionChanged_ = false;
    sessionFailures_ = 0;
    autosaveFailureShown_ = false;
  }
  else
  {
    // Whatever the card had is not known now (a write waiting or failing was dropped above): tried
    // again once the changes settle.
    if (!sessionChanged_) sessionChangedAt_ = started;
    sessionChanged_ = true;
    storageReady_ = false;
    os::storage::ReportCardIoError();
  }
  if (!written || took >= kSessionSlowSaveMs)
    std::printf("Groove: autosave %s in %u ms, generation %u\n", written ? "written" : "failed", static_cast<unsigned>(took),
                static_cast<unsigned>(info.generation));
  return written;
}

bool GrooveApp::SaveSessionToCard()
{
  // A snapshot: the pattern pages are shared until either side changes one (copy on write).
  std::unique_ptr<synth::ProjectData> project(new (std::nothrow) synth::ProjectData);
  std::string message;
  if (!project || !CaptureProject(controller_.Settings(), *project, message, true)) return false;
  const synth::SessionInfo info = NextSessionInfo();
  {
    std::lock_guard<std::mutex> lock(g_cardSession.mutex);
    g_cardSession.project = std::move(project);
    g_cardSession.info = info;
    g_cardSession.result = 0;
  }
  // The snapshot has every change so far; the ones after it start a wait of their own.
  savingSince_ = sessionChangedAt_;
  sessionChanged_ = false;
  if (!Device::Background::Run(&WriteCardSession)) WriteCardSession();
  return true;
}

void GrooveApp::SessionWriteFailed(std::uint32_t since, const char* why)
{
  // The changes are still not on the card since the first of them: the wait does not start again (a
  // write failing again and again must not put the next try off for good).
  const std::uint32_t now = NowMs();
  if (!sessionChanged_ || now - since > now - sessionChangedAt_) sessionChangedAt_ = since;
  sessionChanged_ = true;
  ++sessionFailures_;
  if (!autosaveFailureShown_) controller_.ReportAutosaveProblem(why);
  autosaveFailureShown_ = true;
}

void GrooveApp::CheckCardSession(std::uint32_t now)
{
  (void)now;
  int result = 0;
  std::uint32_t took = 0;
  {
    std::lock_guard<std::mutex> lock(g_cardSession.mutex);
    result = g_cardSession.result;
    took = g_cardSession.tookMs;
    g_cardSession.result = 0;
  }
  if (result == 0) return;
  if (result < 0 || took >= kSessionSlowCardMs || sessionFailures_ != 0)
    std::printf("Groove: autosave %s in %u ms, generation %u%s\n", result < 0 ? "failed" : result == 2 ? "unchanged" : "written",
                static_cast<unsigned>(took), static_cast<unsigned>(sessionGeneration_), sequencer_.Playing() ? ", playing" : "");
  if (result > 0)
  {
    sessionFailures_ = 0;
    autosaveFailureShown_ = false;
    return;
  }
  // The card looks at itself from here, the system loop (it may have stopped answering), and the
  // folders are made again before the next try.
  os::storage::ReportCardIoError();
  storageReady_ = false;
  SessionWriteFailed(savingSince_, "AUTOSAVE FAILED: SD CARD ERROR");
}

void GrooveApp::FlushSession()
{
  if (guide_ || !sessionChanged_ || sampler_.RecordingTrack() >= 0) return;
  (void)SaveSessionState();
}

void GrooveApp::KeepSessionCurrent(std::uint32_t now)
{
  CheckCardSession(now);
  // The guide's practice project is not the working state, and a recording keeps its own file. A run
  // without a card keeps nothing.
  if (guide_ || sampler_.RecordingTrack() >= 0 || !sessionChanged_ || os::storage::CardMissingThisRun()) return;
  // Settled: the keys have rested a moment, none held, playing or not (the card is written on the
  // background task). Keys that keep coming, or a sample played from the card (writing slows its
  // reading), put it off only so long.
  const bool overdue = now - sessionChangedAt_ >= kSessionLongestWaitMs;
  const bool settled = now - lastKeyAt_ >= kSessionQuietMs && keysHeld_ == 0 && !os::storage::CardStreaming();
  if (!settled && !overdue) return;
  if (sessionFailures_ != 0 &&
      now - lastSessionTryAt_ < (sessionFailures_ < kSessionFailuresBeforeSlow ? kSessionRetryMs : kSessionFailedRetryMs))
    return;
  if (CardSessionBusy()) return;
#if defined(ESP_PLATFORM)
  // A plain allocation that fails ends the program. Later is fine.
  if (memory::FastFree() < kSessionHeapBytes || memory::FastLargest() < kSessionBlockBytes) return;
#endif
  lastSessionTryAt_ = now;
  // Out, or it stopped answering: CheckCard looks for it while the changes wait.
  if (!os::storage::CardMounted())
  {
    SessionWriteFailed(sessionChangedAt_, "AUTOSAVE FAILED: NO SD CARD");
    return;
  }
  if (!storageReady_) storageReady_ = synth::storage::EnsureLayout();
  if (!storageReady_)
  {
    os::storage::ReportCardIoError();
    SessionWriteFailed(sessionChangedAt_, "AUTOSAVE FAILED: SD CARD ERROR");
    return;
  }
  if (!SaveSessionToCard()) SessionWriteFailed(sessionChangedAt_, "AUTOSAVE FAILED: NO MEMORY");
}

void GrooveApp::Tick()
{
#if defined(CARDENZA_STRESS_TEST)
  StressTestTick(*this);
#endif
  // The log waits while a sample plays from the card (OS/Storage SetCardStreaming).
  os::storage::SetCardStreaming(synth::audio::SampleStreamsReading());
  ReadComputerMidi();
  sampler_.Tick();
  rack_.DispatchPendingMessages();
  SyncSamplerTrim();
  // The delay time follows the sequencer tempo.
  const std::uint16_t bpm = sequencer_.Tempo();
  if (bpm != fxTempo_)
  {
    fxTempo_ = bpm;
    synth::audio::SetTempo(static_cast<float>(bpm));
  }
  const std::uint32_t now = NowMs();
  ServiceGuide(now);
  controller_.Tick(now);
  animator_.Update(controller_, now);
  StorePlaySettings();
  CheckCard(now);
  KeepSessionCurrent(now);
  EndOrphanNotes(now);
}

// The safety net under every path that starts a note: the audio side's notes against those someone
// is sounding (pads, steps, arpeggiator, sample preview). A note nobody claims gets a second look once
// any note-off on its way has arrived, and if it is still there, its note-off from here.
void GrooveApp::EndOrphanNotes(std::uint32_t now)
{
  // Keys going down and up keep the audio side a moment behind; it can wait until they rest.
  if (now - lastKeyAt_ < kOrphanCheckMs) return;
  if (now - lastOrphanCheckAt_ < (orphanCandidates_ ? kOrphanConfirmMs : kOrphanCheckMs)) return;
  lastOrphanCheckAt_ = now;
  const std::uint32_t shed = synth::audio::ShedVoiceCount();
  if (shed != loggedShedVoices_) printf("Groove: too much to render: %u voices shed so far\n", static_cast<unsigned>(shed));
  loggedShedVoices_ = shed;
  const bool confirming = orphanCandidates_;
  orphanCandidates_ = false;
  for (std::size_t track = 0; track < synth::kSynthTrackCount; ++track)
  {
    const synth::audio::NoteSet held = synth::audio::HeldNotes(track);
    synth::audio::NoteSet owned{};
    controller_.AddHeldPadNotes(track, owned);
    sequencer_.AddSoundingNotes(track, owned);
    for (std::size_t word = 0; word < owned.size(); ++word) owned[word] |= computerNotes_[track][word];
    // A sample preview plays the sample's root note, whichever that is.
    const bool preview = sampler_.Previewing(track);
    for (std::size_t word = 0; word < held.size(); ++word)
    {
      const std::uint32_t orphans = preview ? 0u : held[word] & ~owned[word];
      for (std::size_t bit = 0; confirming && bit < 32; ++bit)
      {
        if ((orphans & orphanNotes_[track][word] & (1u << bit)) == 0) continue;
        synth::audio::NoteOff(track, static_cast<std::uint8_t>(word * 32 + bit));
        ++endedOrphanNotes_;
        printf("Groove: ended a note nobody held: track %u note %u\n", static_cast<unsigned>(track), static_cast<unsigned>(word * 32 + bit));
      }
      orphanNotes_[track][word] = confirming ? 0u : orphans;
      orphanCandidates_ = orphanCandidates_ || (!confirming && orphans != 0);
    }
  }
}

void GrooveApp::SyncSamplerTrim()
{
  // The sampler engine's start / end parameters are the trim window of the loaded sample or take.
  for (std::size_t track = 0; track < synth::kSynthTrackCount; ++track)
  {
    const auto& slot = rack_.Slot(track);
    if (slot.kind != synth::SynthEngineKind::Sampler || slot.engine == nullptr) continue;
    if (sampler_.ConsumeTrimReset(track))
    {
      slot.engine->SetParameterValue("sample.start", 0);
      slot.engine->SetParameterValue("sample.end", 127);
    }
    synth::SynthParameterInfo start, end, loop;
    if (slot.engine->GetParameterInfo("sample.start", start) && slot.engine->GetParameterInfo("sample.end", end))
      sampler_.SetDraftTrimWindow(track, start.value, end.value);
    if (slot.engine->GetParameterInfo("sample.loop", loop)) sampler_.SetDraftLoopEnabled(track, loop.value != 0);
  }
}

void GrooveApp::UpdateLeds()
{
  groove::LedFrame frame{};
  controller_.ComputeLeds(frame);
  std::array<std::uint8_t, groove::kKeyCount> levels{};
  animator_.DecorateLeds(controller_, frame, levels);
  animator_.SmoothLeds(levels);
  // The eight edge keys (ESC TAB FN CTRL, ADD SUB OK PLAY) never go above the dimmest glow: their
  // state shows on the screen's edge strips, the keys only say they are there. Except one the guide
  // asks for: it is full like the other keys of its hint, or it would not stand out.
  const std::uint64_t hint = controller_.GuideHint();
  for (std::size_t row = 0; row < groove::kRows; ++row)
    for (const std::size_t column : {std::size_t{0}, groove::kColumns - 1})
    {
      const std::size_t index = groove::KeyIndex(row, column);
      if ((hint >> index) & 1u) continue;
      levels[index] = std::min(levels[index], groove::kLedEdgeMax);
    }
  for (std::size_t row = 0; row < groove::kRows; ++row)
    for (std::size_t column = 0; column < groove::kColumns; ++column)
      Device::LED::SetKey(row, column, levels[groove::KeyIndex(row, column)]);
  Device::LED::SetEnabled(true);
}

void GrooveApp::RenderFrame()
{
  const auto start = std::chrono::steady_clock::now();
  groove::Gfx gfx(Device::Display::MutableFrameBuffer());
  groove::RenderFrame(gfx, controller_, animator_);
#if defined(ESP_PLATFORM)
  // Frame budget report, looked at every ~12 s (600 frames at 50 fps): printed when notes were lost, a
  // frame took more than a frame's time or the internal RAM's low mark fell by 4 KB, otherwise every
  // 25 reports (5 minutes). Printed every time, an idle device wrote the log on the card every 5 s.
  constexpr std::uint32_t kSlowFrameUs = 20000;
  constexpr std::uint32_t kQuietReports = 25;
  constexpr std::uint32_t kFastMinimumStep = 4096;
  const auto elapsed = static_cast<std::uint32_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count());
  renderTotalUs_ += elapsed;
  renderMaxUs_ = std::max(renderMaxUs_, elapsed);
  if (++renderFrames_ >= 600)
  {
    // Notes lost so far: held ones faded and ones left out (only when the output was about to run dry),
    // held ones whose voice a newer note took (the track's voices all held), let-go tails cut short.
    const synth::audio::GivenUpVoices lost = synth::audio::GivenUpVoiceCounts();
    const std::uint32_t lostNotes = lost.held + lost.unheard + lost.stolen + lost.tails;
    const auto fastMinimum = static_cast<std::uint32_t>(memory::FastMinimum());
    const bool report = ++quietRenderReports_ >= kQuietReports || lostNotes != loggedLostNotes_ || renderMaxUs_ > kSlowFrameUs ||
                        loggedFastMinimum_ == 0 || fastMinimum + kFastMinimumStep <= loggedFastMinimum_;
    if (!report)
    {
      renderFrames_ = 0;
      renderTotalUs_ = 0;
      renderMaxUs_ = 0;
      return;
    }
    quietRenderReports_ = 0;
    loggedLostNotes_ = lostNotes;
    loggedFastMinimum_ = fastMinimum;
    std::printf("Groove: render avg=%luus max=%luus heap=%u min=%u largest=%u psram=%u min=%u lost held=%u unheard=%u stolen=%u tails=%u\n",
                static_cast<unsigned long>(renderTotalUs_ / renderFrames_), static_cast<unsigned long>(renderMaxUs_),
                static_cast<unsigned>(memory::FastFree()), static_cast<unsigned>(memory::FastMinimum()),
                static_cast<unsigned>(memory::FastLargest()), static_cast<unsigned>(memory::SlowFree()),
                static_cast<unsigned>(memory::SlowMinimum()), static_cast<unsigned>(lost.held), static_cast<unsigned>(lost.unheard),
                static_cast<unsigned>(lost.stolen), static_cast<unsigned>(lost.tails));
    // The key LED levels this frame (0..f per key, rows top to bottom), to check the lights from
    // the serial console.
    char leds[4 * 15 + 1] = {};
    for (std::size_t row = 0; row < 4; ++row)
    {
      for (std::size_t column = 0; column < 14; ++column)
        leds[row * 15 + column] = "0123456789abcdef"[Device::LED::Brightness(row, column) & 0x0F];
      leds[row * 15 + 14] = row == 3 ? '\0' : ' ';
    }
    std::printf("Groove: leds %s master=%u\n", leds, static_cast<unsigned>(Device::LED::GlobalBrightness()));
    renderFrames_ = 0;
    renderTotalUs_ = 0;
    renderMaxUs_ = 0;
  }
#else
  (void)start;
#endif
}

void GrooveApp::HandleMatrixKeyEvent(std::size_t row, std::size_t column, bool pressed)
{
  const std::uint32_t now = NowMs();
  controller_.KeyEvent(row, column, pressed, now);
  if (!sessionChanged_) sessionChangedAt_ = now;
  sessionChanged_ = true;
  lastKeyAt_ = now;
  keysHeld_ = pressed ? keysHeld_ + 1 : std::max(0, keysHeld_ - 1);
  // The lesson page follows every key, but not the ones it pressed itself.
  if (guide_ && !guideInjecting_)
  {
    char text[8];
    std::snprintf(text, sizeof(text), "GK%02x%c", static_cast<unsigned>(groove::KeyIndex(row, column)), pressed ? '1' : '0');
    SendGuide(text);
  }
}

// ---------------------------------------------------------------------------------------------
// Engines and notes

synth::ISynthEngine* GrooveApp::Engine(std::size_t track)
{
  return track < synth::kSynthTrackCount ? rack_.Slot(track).engine.get() : nullptr;
}

const synth::ISynthEngine* GrooveApp::Engine(std::size_t track) const
{
  return track < synth::kSynthTrackCount ? rack_.Slot(track).engine.get() : nullptr;
}

synth::SynthEngineKind GrooveApp::EngineKind(std::size_t track) const
{
  return track < synth::kSynthTrackCount ? rack_.Slot(track).kind : synth::SynthEngineKind::None;
}

bool GrooveApp::SwitchEngine(std::size_t track, synth::SynthEngineKind kind, std::string& error)
{
  if (track >= synth::kSynthTrackCount) return false;
  auto& slot = rack_.Slot(track);
  if (slot.kind == kind) return true;
  const auto sample = sampler_.BuildUiState(track);
  if (sample.recording || sample.hasDraft)
  {
    error = "SAVE SAMPLE FIRST";
    return false;
  }
  auto engine = BuildEngine(kind, 0);
  if (!engine)
  {
    error = "NO MEMORY";
    return false;
  }
  if (!slot.engine || !engine->CanReplace(*slot.engine))
  {
    DropEngine(engine, kind);
    error = "ENGINE UNAVAILABLE";
    return false;
  }
  synth::audio::AllNotesOff(track);
  sampler_.OnTrackEngineChanged(track, synth::SynthEngineKind::None);
  engine->ReplaceBinding(*slot.engine);
  std::swap(slot.engine, engine);
  DropEngine(engine, slot.kind);
  slot.kind = kind;
  sampler_.OnTrackEngineChanged(track, kind);
  return true;
}

void GrooveApp::NoteOn(std::size_t track, std::uint8_t note, std::uint8_t velocity)
{
  if (track >= synth::kSynthTrackCount) return;
  os::midi::Send(midi::MidiPacket::NoteOn(0, note, velocity), rack_.PortForTrack(track));
  SendComputerMidi(0x90, track, note, velocity);
}

void GrooveApp::NoteOff(std::size_t track, std::uint8_t note)
{
  if (track >= synth::kSynthTrackCount) return;
  os::midi::Send(midi::MidiPacket::NoteOff(0, note, 0), rack_.PortForTrack(track));
  SendComputerMidi(0x80, track, note, 0);
}

// What the device plays goes out to a computer on USB too: track n on MIDI channel n.
void GrooveApp::SendComputerMidi(std::uint8_t status, std::size_t track, std::uint8_t data1, std::uint8_t data2)
{
  const std::uint8_t packet[4] = {static_cast<std::uint8_t>(status >> 4), static_cast<std::uint8_t>(status | (track & 0x0F)), data1, data2};
  (void)Device::USB::WriteMidi(packet);
}

// A computer plays the device over USB: MIDI channels 1-4 are tracks 1-4, any other channel the current
// track. Its notes are owned here (EndOrphanNotes) and end when it disconnects.
void GrooveApp::ReadComputerMidi()
{
  const bool connected = Device::USB::Connected();
  if (!connected && computerConnected_) ReleaseComputerNotes();
  computerConnected_ = connected;
  std::uint8_t packet[4];
  for (int count = 0; count < 64 && Device::USB::ReadMidi(packet); ++count)
  {
    const std::uint8_t status = packet[1] & 0xF0;
    const std::uint8_t channel = packet[1] & 0x0F;
    const std::size_t track = channel < synth::kSynthTrackCount ? channel : controller_.State().track;
    const std::uint8_t data1 = packet[2] & 0x7F;
    const std::uint8_t data2 = packet[3] & 0x7F;
    auto& held = computerNotes_[track][data1 / 32];
    const std::uint32_t bit = 1u << (data1 % 32);
    if (status == 0x90 && data2 != 0)
    {
      synth::audio::NoteOn(track, data1, data2);
      held |= bit;
    }
    else if (status == 0x80 || status == 0x90)
    {
      synth::audio::NoteOff(track, data1);
      held &= ~bit;
    }
    else if (status == 0xB0 && (data1 == 120 || data1 == 123))
    {
      ReleaseComputerNotes(track);
    }
    else if (status == 0xB0)
    {
      os::midi::Send(midi::MidiPacket::ControlChange(0, data1, data2), rack_.PortForTrack(track));
    }
  }
}

void GrooveApp::ReleaseComputerNotes(std::size_t onlyTrack)
{
  for (std::size_t track = 0; track < synth::kSynthTrackCount; ++track)
  {
    if (onlyTrack < synth::kSynthTrackCount && track != onlyTrack) continue;
    for (std::size_t word = 0; word < computerNotes_[track].size(); ++word)
      for (std::size_t bit = 0; bit < 32; ++bit)
        if (computerNotes_[track][word] & (1u << bit)) synth::audio::NoteOff(track, static_cast<std::uint8_t>(word * 32 + bit));
    computerNotes_[track] = {};
  }
}

void GrooveApp::Panic()
{
  sequencer_.Stop();
  sequencer_.SetRecordArmed(false);
  for (std::size_t track = 0; track < synth::kSynthTrackCount; ++track) synth::audio::AllNotesOff(track);
}

void GrooveApp::SendSequencerMidi(void* context, std::size_t trackIndex, const midi::MidiPacket& packet)
{
  (void)context;
  if (trackIndex >= synth::kSynthTrackCount) return;
  // Bypass the UI queue: the audio runtime synchronizes stable track indices.
  if (packet.status == midi::MidiStatus::NoteOn && packet.data[2] != 0)
  {
    synth::audio::NoteOn(trackIndex, packet.data[1], packet.data[2]);
    SendComputerMidi(0x90, trackIndex, packet.data[1], packet.data[2]);
  }
  else if (packet.status == midi::MidiStatus::NoteOff || packet.status == midi::MidiStatus::NoteOn)
  {
    synth::audio::NoteOff(trackIndex, packet.data[1]);
    SendComputerMidi(0x80, trackIndex, packet.data[1], 0);
  }
  else if (packet.status == midi::MidiStatus::ControlChange && (packet.data[1] == 120 || packet.data[1] == 123))
    synth::audio::AllNotesOff(trackIndex);
}

// ---------------------------------------------------------------------------------------------
// Sampler

void GrooveApp::PreviewSample(std::size_t track)
{
  std::uint8_t note = 60;
  if (const auto* engine = Engine(track))
  {
    synth::SynthParameterInfo root;
    if (engine->GetParameterInfo("sample.root", root)) note = static_cast<std::uint8_t>(std::clamp(root.value, 0, 127));
  }
  (void)sampler_.PreviewCurrentSample(track, note);
}

std::vector<std::string> GrooveApp::ListSamples() const
{
  std::set<std::string> names;
  const std::string directory = synth::storage::SampleDirectory();
  if (!synth::storage::EnsureLayout() || directory.empty()) return {};
#if defined(ESP_PLATFORM)
  if (DIR* handle = opendir(directory.c_str()))
  {
    while (const dirent* entry = readdir(handle))
      if (entry->d_name[0] != '\0' && HasWavExtension(entry->d_name)) names.insert(entry->d_name);
    closedir(handle);
  }
#else
  namespace fs = std::filesystem;
  std::error_code error;
  for (const fs::directory_entry& entry : fs::directory_iterator(directory, error))
  {
    if (error) break;
    if (!entry.is_regular_file(error)) continue;
    const std::string name = entry.path().filename().string();
    if (HasWavExtension(name)) names.insert(name);
  }
#endif
  return std::vector<std::string>(names.begin(), names.end());
}

// ---------------------------------------------------------------------------------------------
// Mixer

void GrooveApp::ApplyMix()
{
  const bool anySolo = std::any_of(soloed_.begin(), soloed_.end(), [](bool soloed) { return soloed; });
  for (std::size_t track = 0; track < synth::kSynthTrackCount; ++track)
  {
    synth::audio::SetTrackGain(track, static_cast<float>(levels_[track]) / 100.0f);
    synth::audio::SetTrackMuted(track, muted_[track] || (anySolo && !soloed_[track]));
  }
}

void GrooveApp::SetTrackLevel(std::size_t track, std::uint8_t level)
{
  if (track >= synth::kSynthTrackCount) return;
  levels_[track] = std::min<std::uint8_t>(level, 100);
  ApplyMix();
}

std::uint8_t GrooveApp::TrackLevel(std::size_t track) const
{
  return track < synth::kSynthTrackCount ? levels_[track] : 0;
}

void GrooveApp::SetTrackMuted(std::size_t track, bool muted)
{
  if (track >= synth::kSynthTrackCount) return;
  muted_[track] = muted;
  ApplyMix();
}

bool GrooveApp::TrackMuted(std::size_t track) const
{
  return track < synth::kSynthTrackCount && muted_[track];
}

void GrooveApp::SetTrackSoloed(std::size_t track, bool soloed)
{
  if (track >= synth::kSynthTrackCount) return;
  soloed_[track] = soloed;
  ApplyMix();
}

bool GrooveApp::TrackSoloed(std::size_t track) const
{
  return track < synth::kSynthTrackCount && soloed_[track];
}

bool GrooveApp::TrackAudible(std::size_t track) const
{
  if (track >= synth::kSynthTrackCount || muted_[track]) return false;
  const bool anySolo = std::any_of(soloed_.begin(), soloed_.end(), [](bool soloed) { return soloed; });
  return !anySolo || soloed_[track];
}

void GrooveApp::SetMasterVolume(std::uint8_t percent)
{
  // A device setting (Settings/system.cfg) on the system's perceptual curve.
  os::storage::SetOutputVolume(percent);
}

std::uint8_t GrooveApp::MasterVolume() const
{
  return os::storage::OutputVolume();
}

float GrooveApp::TrackActivity(std::size_t track) const
{
  return synth::audio::TrackActivity(track);
}

float GrooveApp::OutputPeak() const
{
  return synth::audio::OutputPeak();
}

void GrooveApp::SetMasterFx(std::size_t param, int value)
{
  if (param >= synth::fx::kMasterParamCount) return;
  synth::audio::SetMasterFx(static_cast<synth::fx::MasterParam>(param), value);
}

int GrooveApp::MasterFx(std::size_t param) const
{
  return param < synth::fx::kMasterParamCount ? synth::audio::MasterFx(static_cast<synth::fx::MasterParam>(param)) : 0;
}

float GrooveApp::FxReturnLevel(std::size_t bus) const
{
  return synth::audio::FxReturnLevel(bus);
}

bool GrooveApp::FxBusAvailable(std::size_t bus) const
{
  return synth::audio::FxBusAvailable(bus);
}

void GrooveApp::ApplyMasterFx(const std::array<std::uint8_t, synth::fx::kMasterParamCount>& values)
{
  for (std::size_t param = 0; param < values.size(); ++param) SetMasterFx(param, values[param]);
}

// ---------------------------------------------------------------------------------------------
// Projects

std::vector<std::string> GrooveApp::ListProjects() const
{
  if (!os::storage::CardPresent()) return {};
  return synth::ListProjects();
}

bool GrooveApp::CaptureProject(const groove::ProjectUiSettings& settings, synth::ProjectData& project, std::string& message, bool forSession) const
{
  for (std::size_t track = 0; track < synth::kSynthTrackCount; ++track)
  {
    const auto sample = sampler_.BuildUiState(track);
    if (!forSession && (sample.recording || sample.hasDraft))
    {
      message = "SAVE SAMPLE FIRST";
      return false;
    }
    const auto& slot = rack_.Slot(track);
    if (!slot.engine)
    {
      message = "ENGINE OFFLINE";
      return false;
    }
    project.kinds[track] = slot.kind;
    project.engines[track] = forSession ? slot.engine->CaptureChanges() : slot.engine->CaptureState();
    project.samples[track] = sample.sampleFileName;
    // Still waiting for the card it is on.
    if (forSession && sample.sampleFileName.empty() && slot.kind == synth::SynthEngineKind::Sampler) project.samples[track] = pendingSamples_[track];
  }
  project.sequence = sequencer_.CaptureState();
  if (!project.sequence)
  {
    message = "NO MEMORY";
    return false;
  }
  project.activeTrack = settings.activeTrack;
  // The play settings belong to the project: the keyboard, each track's chord and arpeggiator.
  project.hasPlay = true;
  project.octave = settings.keyboard.octave;
  project.root = settings.keyboard.root;
  project.scale = settings.keyboard.scale;
  project.layout = static_cast<int>(settings.keyboard.layout);
  project.lock = static_cast<int>(settings.keyboard.lock);
  project.velocity = settings.keyboard.velocity;
  for (std::size_t track = 0; track < synth::kSynthTrackCount; ++track)
  {
    project.play[track].chord = static_cast<int>(settings.chords[track].chord);
    project.play[track].voicing = settings.chords[track].voicing;
    project.play[track].arp = sequencer_.GetArpSettings(track);
  }
  // Volume, key LEDs and screen are device settings; their old fields are still written.
  project.ledBrightness = os::storage::KeyLedBrightness();
  project.backlight = os::storage::ScreenBacklight();
  project.mixer.master = os::storage::OutputVolume();
  project.mixer.level = levels_;
  project.mixer.muted = muted_;
  project.mixer.soloed = soloed_;
  for (std::size_t param = 0; param < synth::fx::kMasterParamCount; ++param)
    project.masterFx[param] = static_cast<std::uint8_t>(MasterFx(param));
  return true;
}

groove::SaveResult GrooveApp::SaveProject(const std::string& name, bool overwrite, const groove::ProjectUiSettings& settings, std::string& message)
{
  if (!synth::IsProjectNameValid(name))
  {
    message = "INVALID NAME";
    return groove::SaveResult::Failed;
  }
  // No card (the run started without one, or it is out): nothing to save to.
  if (!os::storage::CardPresent())
  {
    std::printf("Groove: save %s: no card\n", name.c_str());
    message = "NO SD CARD";
    return groove::SaveResult::Failed;
  }
#if defined(ESP_PLATFORM)
  // The file gets every parameter of every track, copied first in many small blocks, and a plain
  // allocation that fails ends the program: about 64 bytes each, and room for the rest. Plain
  // allocations spill into PSRAM when internal RAM is full.
  std::size_t needed = kProjectSaveHeapBytes;
  for (std::size_t track = 0; track < synth::kSynthTrackCount; ++track)
    if (const synth::ISynthEngine* engine = Engine(track)) needed += engine->ListParameters({0, 0}).totalCount * 64;
  if (memory::FastFree() + memory::SlowFree() < needed)
  {
    message = "NO MEMORY";
    return groove::SaveResult::Failed;
  }
#endif
  synth::ProjectData project;
  if (!CaptureProject(settings, project, message)) return groove::SaveResult::Failed;
  const auto result = synth::SaveProjectFile(name, project, overwrite);
  std::printf("Groove: save %s: result %d\n", name.c_str(), static_cast<int>(result));
  storageReady_ = result != synth::ProjectResult::StorageError;
  if (!storageReady_) os::storage::ReportCardIoError();
  switch (result)
  {
    case synth::ProjectResult::Ok: return groove::SaveResult::Ok;
    case synth::ProjectResult::Exists: return groove::SaveResult::Exists;
    case synth::ProjectResult::Invalid: message = "INVALID PROJECT"; return groove::SaveResult::Failed;
    case synth::ProjectResult::StorageError:
    default: message = "SD CARD ERROR"; return groove::SaveResult::Failed;
  }
}

GrooveApp::HeapRoom GrooveApp::Heap() const
{
  HeapRoom room;
  if (heapProbe_ != nullptr)
  {
    heapProbe_(room.freeBytes, room.largestBlock);
    return room;
  }
  // Internal RAM: an engine is many small plain blocks there (its parameter list and presets are in
  // PSRAM, which samples and patterns check for themselves).
  room.freeBytes = memory::FastFree();
  room.largestBlock = memory::FastLargest();
  return room;
}

std::size_t GrooveApp::EngineBytes(synth::SynthEngineKind kind) const
{
  const std::size_t measured = engineBytes_[KindIndex(kind)];
  return measured != 0 ? measured : EngineEstimateBytes(kind);
}

std::size_t GrooveApp::EngineBlockBytes(synth::SynthEngineKind kind) const
{
  return kind == synth::SynthEngineKind::None ? 0 : kEngineBlockBytes;
}

void GrooveApp::DropEngine(std::unique_ptr<synth::ISynthEngine>& engine, synth::SynthEngineKind kind)
{
  if (!engine) return;
  const std::size_t before = Heap().freeBytes;
  engine.reset();
  const std::size_t after = Heap().freeBytes;
  if (after <= before || kind == synth::SynthEngineKind::None) return;
  const std::size_t bytes = after - before;
  const std::size_t index = KindIndex(kind);
  if (bytes > kEngineSizeTrustFactor * EngineEstimateBytes(kind))
  {
    std::printf("Groove: a %s engine gave back %u bytes, not believed\n", synth::ToString(kind).c_str(), static_cast<unsigned>(bytes));
    return;
  }
  // The largest seen: one with its user presets read, or a project's sound, is larger than a new one.
  if (bytes > engineBytes_[index])
  {
    engineBytes_[index] = bytes;
    std::printf("Groove: a %s engine takes %u bytes\n", synth::ToString(kind).c_str(), static_cast<unsigned>(bytes));
  }
}

std::unique_ptr<synth::ISynthEngine> GrooveApp::BuildEngine(synth::SynthEngineKind kind, std::size_t alsoNeeded, bool force)
{
  const HeapRoom room = Heap();
  if (!force && (room.freeBytes < EngineBytes(kind) + alsoNeeded + kEngineHeapFloorBytes || room.largestBlock < EngineBlockBytes(kind)))
  {
    std::printf("Groove: no room for a %s engine: %u free, largest %u, needs %u + %u\n", synth::ToString(kind).c_str(),
                static_cast<unsigned>(room.freeBytes), static_cast<unsigned>(room.largestBlock), static_cast<unsigned>(EngineBytes(kind)),
                static_cast<unsigned>(alsoNeeded + kEngineHeapFloorBytes));
    return nullptr;
  }
  auto engine = synth::CreateSynthEngine(kind);
  if (!engine) return nullptr;
  (void)engine->RefreshPresetCatalog();
  return engine;
}

bool GrooveApp::ReplaceSession(const std::string& name, synth::ProjectData& project, bool createNew, std::string& message, bool workingState)
{
  (void)name;
  for (std::size_t track = 0; track < synth::kSynthTrackCount; ++track)
  {
    const auto sample = sampler_.BuildUiState(track);
    if (sample.recording || sample.hasDraft)
    {
      message = "SAVE SAMPLE FIRST";
      return false;
    }
    if (!rack_.Slot(track).engine)
    {
      message = "ENGINE OFFLINE";
      return false;
    }
  }
  // The output holds still while the project is built: building it holds the audio for 20 ms an
  // engine or preset, which ran the output dry (synth::audio::HoldOutput).
  const synth::audio::OutputHold outputHold;
  // The effects give their memory back while the project is checked and built (nothing plays then),
  // and take what the new one asks for at the end, before the next thing is checked.
  synth::audio::SuspendEffects();
  struct ResumeEffectsAtEnd {
    ~ResumeEffectsAtEnd() { synth::audio::ResumeEffects(); }
  } resumeEffects;

  // Everything is checked first, while the current session plays on untouched: a sound this firmware
  // does not know, a sample that is not there, or a project that does not fit leaves it as it was.
  // Then the tracks are replaced one at a time, so at most one engine is built beside the four that
  // play. (Four new ones beside the four old needed some 30 KB more than the device has: loading a
  // project, or NEW, ended the program, 2026-11-03.)
  // Memory first: the most the swap needs on top of what is used now, track by track (a track's new
  // engine is built before its old one goes).
  long long growth = 0, need = 0;
  std::size_t block = 0;
  for (std::size_t track = 0; track < synth::kSynthTrackCount; ++track)
  {
    const auto kind = project.kinds[track];
    need = std::max(need, growth + static_cast<long long>(EngineBytes(kind)));
    growth += static_cast<long long>(EngineBytes(kind)) - static_cast<long long>(EngineBytes(rack_.Slot(track).kind));
    block = std::max(block, EngineBlockBytes(kind));
  }
  const HeapRoom room = Heap();
  if (!workingState && (room.freeBytes < static_cast<std::size_t>(need) + kEngineHeapFloorBytes || room.largestBlock < block))
  {
    std::printf("Groove: project does not fit: %u free, largest %u, needs %u + %u, block %u\n", static_cast<unsigned>(room.freeBytes),
                static_cast<unsigned>(room.largestBlock), static_cast<unsigned>(need), static_cast<unsigned>(kEngineHeapFloorBytes),
                static_cast<unsigned>(block));
    message = "NO MEMORY";
    return false;
  }
  // The sounds: a track that keeps its kind is checked by the engine playing it; another kind by an
  // engine built for the check alone, one at a time.
  if (!createNew)
  {
    std::unique_ptr<synth::ISynthEngine> probe;
    for (std::size_t track = 0; track < synth::kSynthTrackCount; ++track)
    {
      const auto kind = project.kinds[track];
      const synth::ISynthEngine* checker = rack_.Slot(track).engine.get();
      if (rack_.Slot(track).kind != kind)
      {
        if (!probe || probe->Descriptor().kind != kind)
        {
          if (probe) DropEngine(probe, probe->Descriptor().kind);
          probe = BuildEngine(kind, 0, workingState);
          if (!probe)
          {
            message = "NO MEMORY";
            return false;
          }
        }
        checker = probe.get();
      }
      // The working state does not fall over a sound this firmware no longer knows: that track starts
      // from the engine's first one.
      if (!checker->CanRestore(project.engines[track]) && !workingState)
      {
        message = "INVALID SOUND";
        return false;
      }
    }
    if (probe) DropEngine(probe, probe->Descriptor().kind);
  }
  std::array<synth::SamplerManager::PreparedSample, synth::kSynthTrackCount> samples;
  std::array<std::string, synth::kSynthTrackCount> pending;
  for (std::size_t track = 0; track < synth::kSynthTrackCount; ++track)
  {
    if (project.samples[track].empty()) continue;
    bool shared = false;
    for (std::size_t previous = 0; previous < track; ++previous)
      if (project.samples[previous] == project.samples[track])
      {
        samples[track] = samples[previous];
        shared = true;
        break;
      }
    if (!shared && !synth::SamplerManager::PrepareSample(project.samples[track], samples[track]))
    {
      // The working state does not fall over a file that is not there: the card may just not be in.
      if (workingState)
      {
        pending[track] = project.samples[track];
        project.samples[track].clear();
        continue;
      }
      message = "SAMPLE MISSING";
      return false;
    }
  }

  // Nothing that can fail for a reason other than memory remains, and memory was checked: swap the
  // engines track by track. The arpeggiator goes first so no arp note fires while they change.
  sequencer_.ArpReleaseAll();
  sequencer_.Stop();
  sequencer_.SetRecordArmed(false);
  sequencer_.ClearSelectedSteps();
  for (std::size_t track = 0; track < synth::kSynthTrackCount; ++track) sampler_.OnTrackEngineChanged(track, synth::SynthEngineKind::None);
  for (std::size_t track = 0; track < synth::kSynthTrackCount; ++track)
  {
    auto& slot = rack_.Slot(track);
    auto engine = BuildEngine(project.kinds[track], 0, true);
    if (engine && !createNew && !engine->RestoreState(project.engines[track]) && !workingState)
      std::printf("Groove: track %u lost its sound while loading\n", static_cast<unsigned>(track + 1));
    // Its values are in the engine now: they go back to the heap before the next track is built.
    project.engines[track].values.clear();
    if (!engine || !engine->CanReplace(*slot.engine))
    {
      std::printf("Groove: track %u kept its engine while loading\n", static_cast<unsigned>(track + 1));
      sampler_.OnTrackEngineChanged(track, slot.kind);
      if (engine) DropEngine(engine, project.kinds[track]);
      continue;
    }
    synth::audio::AllNotesOff(track);
    engine->ReplaceBinding(*slot.engine);
    std::swap(slot.engine, engine);
    DropEngine(engine, slot.kind);
    slot.kind = project.kinds[track];
    sampler_.OnTrackEngineChanged(track, slot.kind);
    if (!project.samples[track].empty()) (void)sampler_.ApplySample(track, samples[track]);
    // The engine already carries the project's saved trim.
    (void)sampler_.ConsumeTrimReset(track);
  }
  sequencer_.RestoreState(*project.sequence);
  // Mixer levels, mute, solo and the master bus belong to the project (the play settings too, applied
  // by ApplyPlaySettings); volume, key LEDs and screen do not.
  levels_ = project.mixer.level;
  muted_ = project.mixer.muted;
  soloed_ = project.mixer.soloed;
  ApplyMix();
  ApplyMasterFx(project.masterFx);
  pendingSamples_ = std::move(pending);
  return true;
}

bool GrooveApp::MakeRoom(std::string& message)
{
  message = "NO MEMORY";
  // The patterns can only wait on the card. And not during a lesson: the autosave holds the user's
  // working state then, which the practice project must not replace.
  if (!os::storage::CardMounted() || guide_)
  {
    std::printf("Groove: no room made: %s\n", guide_ ? "the autosave holds the working state" : "no card for the autosave");
    return false;
  }
  // Silent while the autosave is written.
  const synth::audio::OutputHold outputHold;
  // The effects first: the autosave needs room of its own.
  const std::size_t before = Heap().freeBytes;
  synth::audio::SuspendEffects();
  bool saved = true;
#if defined(ESP_PLATFORM)
  // The autosave is many small plain allocations as well.
  saved = memory::FastFree() >= kSessionHeapBytes && memory::FastLargest() >= kSessionBlockBytes;
#endif
  saved = saved && SaveSessionState();
  if (!saved)
  {
    std::printf("Groove: no room made: the autosave could not be written (%u free)\n", static_cast<unsigned>(Heap().freeBytes));
    synth::audio::ResumeEffects();
    return false;
  }
  sequencer_.Reset();
  std::printf("Groove: room made: the patterns wait in the autosave, the effects pause: %u free (was %u), largest %u\n",
              static_cast<unsigned>(Heap().freeBytes), static_cast<unsigned>(before), static_cast<unsigned>(Heap().largestBlock));
  return true;
}

bool GrooveApp::RestoreSessionPatterns()
{
  if (!synth::SessionStore::BeginRestore()) return false;
  synth::ProjectData project;
  synth::SessionInfo info;
  const bool restored = synth::SessionStore::Load(info, project) && project.sequence && sequencer_.RestoreState(*project.sequence);
  synth::SessionStore::FinishRestore();
  if (!restored) std::printf("Groove: the patterns did not come back from the autosave\n");
  return restored;
}

bool GrooveApp::LoadProject(const std::string& name, groove::ProjectUiSettings& settings, std::string& message)
{
  if (!os::storage::CardPresent())
  {
    message = "NO SD CARD";
    return false;
  }
  const HeapRoom before = Heap();
  std::printf("Groove: loading %s (%u free, largest %u)\n", name.c_str(), static_cast<unsigned>(before.freeBytes), static_cast<unsigned>(before.largestBlock));
  synth::ProjectData project;
  auto result = synth::LoadProjectFile(name, project);
  bool cleared = false;
  if (result == synth::ProjectResult::NoMemory)
  {
    project = synth::ProjectData{};
    if (!MakeRoom(message)) return false;
    cleared = true;
    result = synth::LoadProjectFile(name, project);
  }
  bool loaded = false;
  if (result == synth::ProjectResult::Ok)
  {
    loaded = ReplaceSession(name, project, false, message);
    // NO MEMORY changed nothing yet: the room the current patterns and the effects hold may be what
    // is missing.
    if (!loaded && !cleared && message == "NO MEMORY" && MakeRoom(message))
    {
      cleared = true;
      loaded = ReplaceSession(name, project, false, message);
    }
  }
  else if (result == synth::ProjectResult::NoMemory) message = "NO MEMORY";
  else if (result == synth::ProjectResult::StorageError) message = "SD CARD ERROR";
  else message = "INVALID PROJECT";
  std::printf("Groove: load %s: %s (%u free, largest %u)\n", name.c_str(), loaded ? "ok" : message.c_str(),
              static_cast<unsigned>(Heap().freeBytes), static_cast<unsigned>(Heap().largestBlock));
  if (!loaded)
  {
    // What was cleared for it comes back from the autosave (the engines did not change).
    if (cleared)
    {
      project = synth::ProjectData{};
      (void)RestoreSessionPatterns();
      synth::audio::ResumeEffects();
    }
    return false;
  }
  if (cleared) synth::audio::ResumeEffects();
  settings = controller_.Settings();
  settings.activeTrack = synth::kDefaultActiveTrack;
  ApplyPlaySettings(project, settings);
  return true;
}

bool GrooveApp::NewProject(groove::ProjectUiSettings& settings, std::string& message)
{
  synth::ProjectData project;
  project.sequence.reset(new (memory::slow) synth::SynthSequencer::ProjectState);
  if (!project.sequence)
  {
    message = "NO MEMORY";
    return false;
  }
  project.kinds = kDefaultKinds;
  bool created = ReplaceSession("", project, true, message);
  bool cleared = false;
  // As for a load: NO MEMORY changed nothing, and the current patterns and the effects may hold the
  // room.
  if (!created && message == "NO MEMORY" && MakeRoom(message))
  {
    cleared = true;
    created = ReplaceSession("", project, true, message);
  }
  std::printf("Groove: new project: %s (%u free, largest %u)\n", created ? "ok" : message.c_str(), static_cast<unsigned>(Heap().freeBytes),
              static_cast<unsigned>(Heap().largestBlock));
  if (cleared)
  {
    if (!created) (void)RestoreSessionPatterns();
    synth::audio::ResumeEffects();
  }
  if (!created) return false;
  // A new project starts on the first track with the default play settings:
  // C major, no chords, every arpeggiator off. SELECT is a preference and stays.
  const bool multiSelect = controller_.Settings().keyboard.multiSelect;
  settings = groove::ProjectUiSettings{};
  settings.keyboard.multiSelect = multiSelect;
  for (std::size_t track = 0; track < synth::kSynthTrackCount; ++track) sequencer_.SetArpSettings(track, synth::ArpSettings{});
  return true;
}

// ---------------------------------------------------------------------------------------------
// System

// Device settings: os::storage keeps them in Settings/system.cfg for every app.
void GrooveApp::SetLedBrightness(std::uint8_t level)
{
  os::storage::SetKeyLedBrightness(level);
}

std::uint8_t GrooveApp::LedBrightness() const
{
  return os::storage::KeyLedBrightness();
}

void GrooveApp::SetBacklight(std::uint8_t level)
{
  os::storage::SetScreenBacklight(level);
}

std::uint8_t GrooveApp::Backlight() const
{
  return os::storage::ScreenBacklight();
}

void GrooveApp::SetLogging(bool on)
{
  os::storage::SetLogEnabled(on);
  std::printf("log: %s\n", on ? "on" : "off");
}

bool GrooveApp::Logging() const
{
  return os::storage::LogEnabled();
}

groove::SystemInfo GrooveApp::QuerySystem() const
{
  groove::SystemInfo info;
  info.storageReady = storageReady_ && os::storage::CardMounted();
#if defined(ESP_PLATFORM)
  info.freeHeap = memory::FastFree();
  info.minimumFreeHeap = memory::FastMinimum();
  info.freePsram = memory::SlowFree();
  info.totalPsram = memory::SlowTotal();
#endif
  return info;
}

}  // namespace apps
