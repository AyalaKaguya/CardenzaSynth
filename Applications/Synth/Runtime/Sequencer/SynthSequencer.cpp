#include "Applications/Synth/Runtime/Sequencer/SynthSequencer.h"

#include "Library/Memory/Memory.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <new>
#include <string>
#include <utility>

#if defined(ESP_PLATFORM)
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#endif

namespace synth {

namespace {

constexpr std::array<std::uint16_t, kHomePerformanceColumnCount> kLengthBuckets = {
    kSequencerPulsesPerStep,
    kSequencerPulsesPerStep * 2,
    kSequencerPulsesPerStep * 3,
    kSequencerPulsesPerStep * 4,
    kSequencerPulsesPerStep * 6,
    kSequencerPulsesPerStep * 8,
    kSequencerPulsesPerStep * 12,
    kSequencerPulsesPerStep * 16,
};

constexpr std::array<std::uint8_t, kHomePerformanceColumnCount> kVelocityBuckets = {
    24, 40, 56, 72, 88, 104, 120, 127,
};

// Relative length edits step through musically useful lengths, from a quarter step to a full pattern.
constexpr std::array<std::uint16_t, 16> kAdjustLengthTable = {
    6, 12, 18, 24, 36, 48, 72, 96, 144, 192, 288, 384, 768, 1536, 3072, kSequencerMaxSteps * kSequencerPulsesPerStep,
};

constexpr std::uint8_t kMinSwing = 50;
constexpr std::uint8_t kMaxSwing = 75;
// An odd sixteenth starts at swing% of an eighth note (48 pulses): this many pulses late at most.
constexpr std::uint16_t SwingDelayPulses(std::uint8_t swing)
{
  const int swung = (48 * static_cast<int>(swing < kMinSwing ? kMinSwing : (swing > kMaxSwing ? kMaxSwing : swing)) + 50) / 100;
  return static_cast<std::uint16_t>(swung > static_cast<int>(kSequencerPulsesPerStep) ? swung - static_cast<int>(kSequencerPulsesPerStep) : 0);
}
// The latest a note's last hit can start after the start of its own step: the latest microstep, the
// hardest swing and the widest spread of hits. That many steps back can still play on this pulse.
constexpr std::uint16_t MaxHitReachPulses()
{
  std::uint16_t widest = 0;
  for (int hits = 1; hits <= kSequencerMaxHits; ++hits)
  {
    const auto spread = static_cast<std::uint16_t>((hits - 1) * SequencerHitSpacingPulses(static_cast<std::uint8_t>(hits)));
    if (spread > widest) widest = spread;
  }
  return static_cast<std::uint16_t>((kSequencerMicrostepCount - 1) * kSequencerPulsesPerMicrostep + SwingDelayPulses(kMaxSwing) + widest);
}
constexpr std::size_t kHitReachSteps = MaxHitReachPulses() / kSequencerPulsesPerStep;
static_assert(MaxHitReachPulses() == 50 && kHitReachSteps == 2, "A note's hits reach at most two steps past its own.");
constexpr std::uint16_t kMinTempo = 30;
constexpr std::uint16_t kMaxTempo = 300;

constexpr std::size_t kClockIdleSleepMs = 2;
constexpr std::size_t kClockTaskStackWords = 4096;
// The beat does not depend on anything else that runs: on the core without the main audio task, above
// the UI and the audio helper there, and each pulse takes only a moment.
constexpr int kClockTaskPriority = 7;
constexpr int kClockTaskCore = 0;
// The clock catches up on late pulses, however many, so the tempo stays right; only further behind than
// this (the whole device was stopped) it restarts from now.
constexpr std::uint64_t kClockMaxLagMicros = 500000;

}  // namespace

namespace {

const SynthSequencer::PatternData kEmptyPattern{};
SynthSequencer::PatternData scratchPattern{};
const SynthSequencer::StepData kEmptyStep{};
SynthSequencer::StepData scratchStep{};
std::atomic<bool> patternMemoryFull{false};

bool StepHasNotes(const SynthSequencer::StepData& step)
{
  return std::any_of(step.notes.begin(), step.notes.end(), [](const auto& note) { return note.active; });
}
// Pattern steps live in PSRAM (Library/Memory/Memory.h): the clock reads one step at a time, the UI a
// page. Pages stop when PSRAM is down to this, which samples and snapshots share: about 1300 pages more
// than the 1024 every pattern of every track can have.
constexpr std::size_t kPatternSlowReserveBytes = 256 * 1024;
// Without PSRAM they come from internal RAM and stop here: file IO, the event queues, the effects, the
// autosave (12 KB) and building an engine need the rest (2026-11-03, when that was all the board used).
constexpr std::size_t kPatternHeapFloorBytes = 24 * 1024;
#if defined(CARDENZA_SEQUENCER_TEST_HOOKS)
int testPageBudget = -1;
#endif

std::size_t ClampedPatternCount(const SynthSequencer::TrackData& track)
{
  return std::clamp<std::size_t>(track.patternCount, 1, kSequencerMaxPatternCount);
}

bool ValidSongRow(const SongRow& row, const std::array<SynthSequencer::TrackData, kSynthTrackCount>& tracks)
{
  if (row.repeats < 1 || row.repeats > kSongMaxRepeats) return false;
  for (std::size_t track = 0; track < tracks.size(); ++track)
  {
    const int pattern = row.patterns[track];
    if (pattern < -1 || pattern >= static_cast<int>(ClampedPatternCount(tracks[track]))) return false;
  }
  return true;
}

}  // namespace

struct SynthSequencer::StepStore::Page {
  std::atomic<std::uint16_t> owners{1};
  std::array<StepData, kSequencerStepCount> steps{};
};

SynthSequencer::StepStore::StepStore(const StepStore& other)
    : pages_(other.pages_), populated_(other.populated_), populatedKnown_(other.populatedKnown_)
{
  for (Page* page : pages_)
    if (page != nullptr) page->owners.fetch_add(1, std::memory_order_relaxed);
}

SynthSequencer::StepStore& SynthSequencer::StepStore::operator=(const StepStore& other)
{
  if (this == &other) return *this;
  for (Page* page : other.pages_)
    if (page != nullptr) page->owners.fetch_add(1, std::memory_order_relaxed);
  for (Page* page : pages_) ReleasePage(page);
  pages_ = other.pages_;
  populated_ = other.populated_;
  populatedKnown_ = other.populatedKnown_;
  return *this;
}

SynthSequencer::StepStore::~StepStore()
{
  for (Page* page : pages_) ReleasePage(page);
}

void SynthSequencer::StepStore::ReleasePage(Page* page)
{
  if (page != nullptr && page->owners.fetch_sub(1, std::memory_order_acq_rel) == 1) delete page;
}

SynthSequencer::StepStore::Page* SynthSequencer::StepStore::NewPage()
{
#if defined(CARDENZA_SEQUENCER_TEST_HOOKS)
  if (testPageBudget == 0) return nullptr;
  if (testPageBudget > 0) --testPageBudget;
#endif
  if (!memory::SlowRoom(sizeof(Page), kPatternSlowReserveBytes, kPatternHeapFloorBytes)) return nullptr;
  return new (memory::slow) Page;
}

const SynthSequencer::StepData& SynthSequencer::StepStore::operator[](std::size_t step) const
{
  const std::size_t page = step / kSequencerStepCount;
  if (page >= pages_.size() || pages_[page] == nullptr) return kEmptyStep;
  return pages_[page]->steps[step % kSequencerStepCount];
}

SynthSequencer::StepData* SynthSequencer::StepStore::TryEdit(std::size_t step)
{
  const std::size_t index = step / kSequencerStepCount;
  if (index >= pages_.size()) return nullptr;
  Page*& page = pages_[index];
  if (page == nullptr)
  {
    page = NewPage();
    if (page == nullptr) return nullptr;
  }
  else if (page->owners.load(std::memory_order_acquire) > 1)
  {
    // A snapshot still reads this page: it keeps the old one, this store writes a copy.
    Page* copy = NewPage();
    if (copy == nullptr) return nullptr;
    copy->steps = page->steps;
    ReleasePage(page);
    page = copy;
  }
  populatedKnown_ = false;
  return &page->steps[step % kSequencerStepCount];
}

SynthSequencer::StepData& SynthSequencer::StepStore::Edit(std::size_t step)
{
  if (StepData* data = TryEdit(step)) return *data;
  patternMemoryFull.store(true, std::memory_order_relaxed);
  scratchStep = StepData{};
  return scratchStep;
}

std::size_t SynthSequencer::StepStore::NextInUse(std::size_t step) const
{
  for (std::size_t page = step / kSequencerStepCount; page < pages_.size(); ++page)
    if (pages_[page] != nullptr) return std::max(step, page * kSequencerStepCount);
  return size();
}

std::size_t SynthSequencer::StepStore::PopulatedSteps() const
{
  if (!populatedKnown_)
  {
    populated_ = 0;
    for (std::size_t step = NextInUse(0); step < size(); step = NextInUse(step + 1))
      if (StepHasNotes((*this)[step])) ++populated_;
    populatedKnown_ = true;
  }
  return populated_;
}

void SynthSequencer::StepStore::ReleaseEmptyPages()
{
  if (populatedKnown_) return;
  for (Page*& page : pages_)
  {
    if (page == nullptr) continue;
    if (std::any_of(page->steps.begin(), page->steps.end(), StepHasNotes)) continue;
    ReleasePage(page);
    page = nullptr;
  }
}

void SynthSequencer::StepStore::Clear()
{
  for (Page*& page : pages_)
  {
    ReleasePage(page);
    page = nullptr;
  }
  populated_ = 0;
  populatedKnown_ = true;
}

bool SynthSequencer::ConsumePatternMemoryFull()
{
  return patternMemoryFull.exchange(false, std::memory_order_relaxed);
}

#if defined(CARDENZA_SEQUENCER_TEST_HOOKS)
void SynthSequencer::SetPageBudgetForTest(int pages) { testPageBudget = pages; }
#endif

SynthSequencer::PatternBank& SynthSequencer::PatternBank::operator=(const PatternBank& other)
{
  if (this != &other) (void)CopyFrom(other);
  return *this;
}

SynthSequencer::PatternData& SynthSequencer::PatternBank::operator[](std::size_t index)
{
  if (Ensure(index)) return *patterns_[index];
  scratchPattern = PatternData{};
  return scratchPattern;
}

const SynthSequencer::PatternData& SynthSequencer::PatternBank::operator[](std::size_t index) const
{
  return Allocated(index) ? *patterns_[index] : kEmptyPattern;
}

bool SynthSequencer::PatternBank::Allocated(std::size_t index) const
{
  return index < patterns_.size() && patterns_[index] != nullptr;
}

bool SynthSequencer::PatternBank::Ensure(std::size_t index)
{
  if (index >= patterns_.size()) return false;
  if (!patterns_[index]) patterns_[index].reset(new (memory::slow) PatternData);
  return patterns_[index] != nullptr;
}

bool SynthSequencer::PatternBank::Reserve(const PatternBank& other)
{
  for (std::size_t index = 0; index < patterns_.size(); ++index)
    if (other.Allocated(index) && !Ensure(index)) return false;
  return true;
}

bool SynthSequencer::PatternBank::CopyFrom(const PatternBank& other)
{
  if (this == &other) return true;
  // A failed reserve only adds empty patterns, so the contents stay unchanged.
  if (!Reserve(other)) return false;
  for (std::size_t index = 0; index < patterns_.size(); ++index)
  {
    if (other.Allocated(index)) *patterns_[index] = *other.patterns_[index];
    else patterns_[index].reset();
  }
  return true;
}

void SynthSequencer::PatternBank::Release(std::size_t index)
{
  if (index < patterns_.size()) patterns_[index].reset();
}

void SynthSequencer::PatternBank::Swap(std::size_t left, std::size_t right)
{
  if (left < patterns_.size() && right < patterns_.size()) patterns_[left].swap(patterns_[right]);
}

void SynthSequencer::PatternBank::Clear()
{
  for (auto& pattern : patterns_) pattern.reset();
}

SynthSequencer::SynthSequencer()
{
  queuedPattern_.fill(kNoPattern);
  previousPassPattern_.fill(kNoPattern);
}

SynthSequencer::~SynthSequencer()
{
  // Stop() alone keeps a held arp running.
  ArpReleaseAll();
  Stop();
  StopClockWorker();
}

void SynthSequencer::SetMidiSink(MidiSink sink, void* context)
{
  std::lock_guard<std::mutex> lock(mutex_);
  midiSink_ = sink;
  midiSinkContext_ = context;
}

void SynthSequencer::SetActiveTrack(std::size_t trackIndex)
{
  std::lock_guard<std::mutex> outputLock(outputMutex_);
  std::array<PendingMidiEvent, kMaxPendingMidiEvents> events{};
  std::size_t eventCount = 0;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (trackIndex < tracks_.size())
    {
      if (trackIndex != activeTrack_)
      {
        StopAuditionLocked(events, eventCount);
        heldSteps_.fill(false);
        heldEditLanes_.fill(false);
        editLane_ = SequencerEditLane::Note;
        // Each track's arp runs on when another track becomes active.
      }
      activeTrack_ = trackIndex;
      ReleaseEmptyPagesLocked();
    }
  }
  EmitPendingMidi(events, eventCount);
}

void SynthSequencer::Play()
{
  EnsureClockWorkerStarted();
  std::lock_guard<std::mutex> outputLock(outputMutex_);
  std::lock_guard<std::mutex> lock(mutex_);
  if (playing_ || !clockWorkerRunning_.load())
  {
    return;
  }
  viewFollow_.fill(true);
  absolutePulse_ = 0;
  playbackStep_ = 0;
  ResetPlaybackPositionsLocked();
  // Pulse 0 is a bar line: the song, when switched on, takes over there from its start row.
  LeaveSongLocked();
  playing_ = true;
  // Running arps lock onto the transport grid from its first pulse.
  AlignArpsToTransportLocked(absolutePulse_);
}

void SynthSequencer::Stop()
{
  std::lock_guard<std::mutex> outputLock(outputMutex_);
  std::array<PendingMidiEvent, kMaxPendingMidiEvents> events{};
  std::size_t eventCount = 0;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    StopLocked(events, eventCount);
  }
  EmitPendingMidi(events, eventCount);
}

void SynthSequencer::StopLocked(std::array<PendingMidiEvent, kMaxPendingMidiEvents>& events, std::size_t& count)
{
  // Held takes end at the stop; queued patterns take over now that no loop is running.
  FinalizeAllHeldRecordingsLocked();
  ApplyQueuedPatternsLocked();
  suppressedNotes_.fill(SuppressedNote{});
  // The song position survives for Resume(); a pending jump does not.
  songJumpPending_ = false;
  playing_ = false;
  // The all-notes-off below silences the arp notes too; held arps run on, on their free clock.
  for (ArpVoice& arp : arps_) CutArpNoteLocked(arp, events, count);
  CollectAllNoteOffsLocked(events, count);
}

void SynthSequencer::TogglePlayback()
{
  if (Playing())
  {
    Stop();
    return;
  }
  Play();
}

bool SynthSequencer::Playing() const
{
  std::lock_guard<std::mutex> lock(mutex_);
  return playing_;
}

void SynthSequencer::Resume()
{
  EnsureClockWorkerStarted();
  std::lock_guard<std::mutex> outputLock(outputMutex_);
  std::lock_guard<std::mutex> lock(mutex_);
  const bool wasPlaying = playing_;
  playing_ = clockWorkerRunning_.load();
  if (playing_ && !wasPlaying) AlignArpsToTransportLocked(absolutePulse_);
}

std::uint16_t SynthSequencer::Tempo() const
{
  std::lock_guard<std::mutex> lock(mutex_);
  return bpm_;
}

void SynthSequencer::AdjustTempo(int delta)
{
  std::lock_guard<std::mutex> lock(mutex_);
  bpm_ = static_cast<std::uint16_t>(std::clamp(static_cast<int>(bpm_) + delta, 30, 300));
}

void SynthSequencer::ToggleTrackMuted()
{
  std::lock_guard<std::mutex> outputLock(outputMutex_);
  std::array<PendingMidiEvent, kMaxPendingMidiEvents> events{};
  std::size_t count = 0;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    TrackData& track = tracks_[activeTrack_];
    track.muted = !track.muted;
    if (track.muted) {
      for (PlaybackNote& note : playbackNotes_) {
        if (note.active && note.trackIndex == activeTrack_) {
          AddPendingMidi(events, count, note.trackIndex, midi::MidiPacket::NoteOff(0, note.note, 0));
          note = PlaybackNote{};
        }
      }
    }
  }
  EmitPendingMidi(events, count);
}

void SynthSequencer::ToggleTrackEnabled()
{
  // Reuse mute's note release without changing the stored mute preference.
  std::lock_guard<std::mutex> outputLock(outputMutex_);
  std::array<PendingMidiEvent, kMaxPendingMidiEvents> events{};
  std::size_t count = 0;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    TrackData& track = tracks_[activeTrack_];
    track.enabled = !track.enabled;
    if (!track.enabled) {
      for (PlaybackNote& note : playbackNotes_) {
        if (note.active && note.trackIndex == activeTrack_) {
          AddPendingMidi(events, count, note.trackIndex, midi::MidiPacket::NoteOff(0, note.note, 0));
          note = PlaybackNote{};
        }
      }
    }
  }
  EmitPendingMidi(events, count);
}

void SynthSequencer::Reset()
{
  ArpReleaseAll();
  Stop();
  ClearHeldSteps();
  std::lock_guard<std::mutex> lock(mutex_);
  // Clear one pattern at a time; a temporary full ProjectState would overflow a task stack.
  for (TrackData& track : tracks_) {
    track.patterns.Clear();
    track.patternCount = 1;
    track.activePattern = 0;
    track.defaultVelocity = 110;
    track.defaultLengthPulses = kSequencerPulsesPerStep;
    track.defaultHits = 1;
    track.enabled = true;
    track.muted = false;
    track.patternSteps = TrackData::DefaultPatternSteps();
  }
  viewPage_.fill(0);
  viewPattern_.fill(0);
  viewFollow_.fill(true);
  bpm_ = 120;
  swing_ = kMinSwing;
  recordArmed_ = false;
  queuedPattern_.fill(kNoPattern);
  heldRecordings_.fill(HeldRecording{});
  ResetSongLocked();
  songMode_ = false;
  absolutePulse_ = 0;
  playbackStep_ = 0;
  ResetPlaybackPositionsLocked();
}

std::unique_ptr<SynthSequencer::ProjectState> SynthSequencer::CaptureState() const
{
  auto state = std::unique_ptr<ProjectState>(new (memory::slow) ProjectState);
  if (!state) return nullptr;
  std::lock_guard<std::mutex> lock(mutex_);
  // Reserve first so the copy below cannot silently drop patterns when memory runs out.
  for (std::size_t index = 0; index < tracks_.size(); ++index)
    if (!state->tracks[index].patterns.Reserve(tracks_[index].patterns)) return nullptr;
  state->bpm = bpm_;
  state->swing = swing_;
  state->tracks = tracks_;
  state->song = song_;
  state->songLength = songLength_;
  state->songLoop = songLoop_;
  return state;
}

bool SynthSequencer::ValidateState(const ProjectState& state)
{
  if (state.bpm < kMinTempo || state.bpm > kMaxTempo || state.swing < kMinSwing || state.swing > kMaxSwing) return false;
  if (state.songLength > kSongMaxRows) return false;
  for (std::size_t row = 0; row < state.songLength; ++row)
    if (!ValidSongRow(state.song[row], state.tracks)) return false;
  for (const TrackData& track : state.tracks) {
    if (track.patternCount < 1 || track.patternCount > kSequencerMaxPatternCount ||
        track.activePattern >= track.patternCount || track.defaultVelocity == 0 || track.defaultVelocity > 127 ||
        track.defaultLengthPulses == 0 || track.defaultLengthPulses > kSequencerMaxSteps * kSequencerPulsesPerStep || track.defaultHits < 1 ||
        track.defaultHits > kSequencerMaxHits) return false;
    for (const std::uint16_t steps : track.patternSteps)
      if (steps < 1 || steps > kSequencerMaxSteps) return false;
    for (std::size_t patternIndex = 0; patternIndex < track.patterns.size(); ++patternIndex) {
      if (!track.patterns.Allocated(patternIndex)) continue;
      const PatternData& pattern = track.patterns[patternIndex];
      for (std::size_t stepIndex = pattern.steps.NextInUse(0); stepIndex < pattern.steps.size(); stepIndex = pattern.steps.NextInUse(stepIndex + 1)) {
        std::array<bool, 128> notes{};
        for (const NoteEvent& note : pattern.steps[stepIndex].notes) {
          if (!note.active) continue;
          if (note.note > 127 || note.velocity == 0 || note.velocity > 127 || note.microstep >= kSequencerMicrostepCount ||
              note.lengthPulses == 0 || note.lengthPulses > (kSequencerMaxSteps - stepIndex) * kSequencerPulsesPerStep - note.microstep * kSequencerPulsesPerMicrostep ||
              notes[note.note]) return false;
          notes[note.note] = true;
        }
      }
    }
  }
  return true;
}

bool SynthSequencer::RestoreState(const ProjectState& state)
{
  if (!ValidateState(state)) return false;
  ArpReleaseAll();
  Stop();
  ClearHeldSteps();
  std::lock_guard<std::mutex> lock(mutex_);
  for (std::size_t index = 0; index < tracks_.size(); ++index)
    if (!tracks_[index].patterns.Reserve(state.tracks[index].patterns)) return false;
  tracks_ = state.tracks;
  bpm_ = state.bpm;
  swing_ = state.swing;
  // The song mode switch is a performance control and stays as it is.
  ResetSongLocked();
  std::copy_n(state.song.begin(), state.songLength, song_.begin());
  songLength_ = state.songLength;
  songLoop_ = state.songLoop;
  queuedPattern_.fill(kNoPattern);
  heldRecordings_.fill(HeldRecording{});
  absolutePulse_ = 0;
  playbackStep_ = 0;
  ResetPlaybackPositionsLocked();
  return true;
}

void SynthSequencer::StepPattern(int delta)
{
  std::lock_guard<std::mutex> lock(mutex_);
  if (activeTrack_ >= tracks_.size() || delta == 0)
  {
    return;
  }

  TrackData& track = tracks_[activeTrack_];
  track.patternCount = std::clamp<std::size_t>(track.patternCount, 1, kSequencerMaxPatternCount);
  const int count = static_cast<int>(track.patternCount);
  const int next = (static_cast<int>(track.activePattern) + delta % count + count) % count;
  track.activePattern = static_cast<std::size_t>(next);
  queuedPattern_[activeTrack_] = kNoPattern;
  ReleaseEmptyPagesLocked();
}

void SynthSequencer::AdjustPatternCount(int delta)
{
  std::lock_guard<std::mutex> lock(mutex_);
  if (activeTrack_ >= tracks_.size() || delta == 0)
  {
    return;
  }

  TrackData& track = tracks_[activeTrack_];
  const int current = static_cast<int>(std::clamp<std::size_t>(track.patternCount, 1, kSequencerMaxPatternCount));
  const int next = std::clamp(current + delta, 1, static_cast<int>(kSequencerMaxPatternCount));
  for (int index = next; index < current; ++index)
  {
    track.patterns.Release(static_cast<std::size_t>(index));
    track.patternSteps[static_cast<std::size_t>(index)] = kSequencerStepCount;
    RemoveSongPatternLocked(activeTrack_, static_cast<std::size_t>(index), false);
  }
  for (int index = current; index < next; ++index)
    track.patternSteps[static_cast<std::size_t>(index)] = track.patternSteps[std::min<std::size_t>(track.activePattern, kSequencerMaxPatternCount - 1)];
  track.patternCount = static_cast<std::size_t>(next);
  if (track.activePattern >= track.patternCount)
  {
    track.activePattern = track.patternCount - 1;
  }
  if (queuedPattern_[activeTrack_] >= track.patternCount) queuedPattern_[activeTrack_] = kNoPattern;
  for (HeldRecording& recording : heldRecordings_)
    if (recording.active && recording.trackIndex == activeTrack_ && recording.pattern >= track.patternCount) recording = HeldRecording{};
  ReleaseEmptyPagesLocked();
}

bool SynthSequencer::SelectOrCreatePattern(std::size_t patternIndex)
{
  std::lock_guard<std::mutex> lock(mutex_);
  if (activeTrack_ >= tracks_.size() || patternIndex >= kSequencerMaxPatternCount)
  {
    return false;
  }

  TrackData& track = tracks_[activeTrack_];
  track.patternCount = std::clamp<std::size_t>(track.patternCount, 1, kSequencerMaxPatternCount);
  if (patternIndex > track.patternCount)
  {
    return false;
  }

  if (patternIndex == track.patternCount)
  {
    if (track.patternCount >= kSequencerMaxPatternCount)
    {
      return false;
    }
    track.patternSteps[patternIndex] = track.patternSteps[std::min(track.activePattern, track.patternCount - 1)];
    ++track.patternCount;
  }

  track.activePattern = patternIndex;
  queuedPattern_[activeTrack_] = kNoPattern;
  ReleaseEmptyPagesLocked();
  return true;
}

bool SynthSequencer::DeletePattern(std::size_t patternIndex)
{
  std::lock_guard<std::mutex> lock(mutex_);
  if (activeTrack_ >= tracks_.size() || patternIndex >= kSequencerMaxPatternCount)
  {
    return false;
  }

  TrackData& track = tracks_[activeTrack_];
  track.patternCount = std::clamp<std::size_t>(track.patternCount, 1, kSequencerMaxPatternCount);
  if (patternIndex >= track.patternCount)
  {
    return false;
  }

  std::size_t& queued = queuedPattern_[activeTrack_];
  if (queued == patternIndex || track.patternCount == 1) queued = kNoPattern;
  else if (queued != kNoPattern && queued > patternIndex) --queued;
  for (HeldRecording& recording : heldRecordings_)
  {
    if (!recording.active || recording.trackIndex != activeTrack_) continue;
    if (recording.pattern == patternIndex) recording = HeldRecording{};
    else if (recording.pattern > patternIndex && track.patternCount > 1) --recording.pattern;
  }

  if (track.patternCount == 1)
  {
    // The only pattern is cleared rather than removed, so song rows keep referring to it.
    track.patterns.Release(0);
    track.patternSteps[0] = kSequencerStepCount;
    track.activePattern = 0;
    ReleaseEmptyPagesLocked();
    return true;
  }

  RemoveSongPatternLocked(activeTrack_, patternIndex, true);
  for (std::size_t index = patternIndex; index + 1 < track.patternCount; ++index)
  {
    track.patterns.Swap(index, index + 1);
    track.patternSteps[index] = track.patternSteps[index + 1];
  }
  track.patterns.Release(track.patternCount - 1);
  track.patternSteps[track.patternCount - 1] = kSequencerStepCount;
  --track.patternCount;
  if (track.activePattern > patternIndex) --track.activePattern;
  track.activePattern = std::min(track.activePattern, track.patternCount - 1);
  ReleaseEmptyPagesLocked();
  return true;
}

bool SynthSequencer::CopyPattern(std::size_t sourcePattern, std::size_t destinationPattern)
{
  std::lock_guard<std::mutex> lock(mutex_);
  if (activeTrack_ >= tracks_.size() || sourcePattern >= kSequencerMaxPatternCount || destinationPattern >= kSequencerMaxPatternCount)
  {
    return false;
  }

  TrackData& track = tracks_[activeTrack_];
  track.patternCount = std::clamp<std::size_t>(track.patternCount, 1, kSequencerMaxPatternCount);
  if (sourcePattern >= track.patternCount || destinationPattern > track.patternCount || !track.patterns.Ensure(destinationPattern))
  {
    return false;
  }

  if (destinationPattern == track.patternCount)
  {
    if (track.patternCount >= kSequencerMaxPatternCount)
    {
      return false;
    }
    ++track.patternCount;
  }

  track.patterns[destinationPattern] = std::as_const(track.patterns)[sourcePattern];
  track.patternSteps[destinationPattern] = track.patternSteps[sourcePattern];
  track.activePattern = destinationPattern;
  queuedPattern_[activeTrack_] = kNoPattern;
  ReleaseEmptyPagesLocked();
  return true;
}

std::uint16_t SynthSequencer::PatternStepMask(std::size_t trackIndex, std::size_t patternIndex) const
{
  std::lock_guard<std::mutex> lock(mutex_);
  if (trackIndex >= tracks_.size() || patternIndex >= kSequencerMaxPatternCount) return 0;
  const TrackData& track = tracks_[trackIndex];
  if (patternIndex >= std::clamp<std::size_t>(track.patternCount, 1, kSequencerMaxPatternCount) || !track.patterns.Allocated(patternIndex)) return 0;
  const StepStore& steps = track.patterns[patternIndex].steps;
  const std::size_t length = std::clamp<std::size_t>(track.patternSteps[patternIndex], 1, kSequencerMaxSteps);
  std::uint16_t mask = 0;
  for (std::size_t step = steps.NextInUse(0); step < length; step = steps.NextInUse(step + 1))
    if (ActiveNoteCount(steps[step]) > 0)
    {
      const std::size_t bit = length <= kSequencerStepCount ? step : step * kSequencerStepCount / length;
      mask = static_cast<std::uint16_t>(mask | (1u << bit));
    }
  return mask;
}

bool SynthSequencer::PatternHasEvents(std::size_t patternIndex) const
{
  std::lock_guard<std::mutex> lock(mutex_);
  if (activeTrack_ >= tracks_.size() || patternIndex >= kSequencerMaxPatternCount)
  {
    return false;
  }

  const TrackData& track = tracks_[activeTrack_];
  const std::size_t patternCount = std::clamp<std::size_t>(track.patternCount, 1, kSequencerMaxPatternCount);
  if (patternIndex >= patternCount)
  {
    return false;
  }

  return track.patterns[patternIndex].steps.PopulatedSteps() != 0;
}

void SynthSequencer::PressStep(std::size_t stepIndex, const std::uint8_t* heldNotes, std::size_t heldNoteCount)
{
  if (stepIndex >= kSequencerMaxSteps)
  {
    return;
  }

  std::lock_guard<std::mutex> lock(mutex_);
  heldSteps_[stepIndex] = true;
  if (activeTrack_ >= tracks_.size() || heldNotes == nullptr || heldNoteCount == 0 || editLane_ != SequencerEditLane::Note)
  {
    ReleaseEmptyPagesLocked();
    return;
  }

  // The held notes as one group: any of them in the step takes them all out, else they all go in.
  TrackData& track = tracks_[activeTrack_];
  std::array<bool, 128> given{};
  const std::array<std::uint8_t, 128> velocity{};
  for (std::size_t index = 0; index < heldNoteCount; ++index)
    if (heldNotes[index] < given.size()) given[heldNotes[index]] = true;
  ToggleNotesInStepLocked(ActivePatternLocked(track).steps, stepIndex, given, velocity, track);
  ReleaseEmptyPagesLocked();
}

void SynthSequencer::ReleaseStep(std::size_t stepIndex)
{
  std::lock_guard<std::mutex> outputLock(outputMutex_);
  if (stepIndex >= kSequencerMaxSteps)
  {
    return;
  }

  std::array<PendingMidiEvent, kMaxPendingMidiEvents> events{};
  std::size_t eventCount = 0;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    heldSteps_[stepIndex] = false;
    if (!std::any_of(heldSteps_.begin(), heldSteps_.end(), [](bool held) { return held; }))
    {
      StopAuditionLocked(events, eventCount);
      heldEditLanes_.fill(false);
      editLane_ = SequencerEditLane::Note;
    }
    else
    {
      ReleaseEmptyPagesLocked();
    }
  }
  EmitPendingMidi(events, eventCount);
}

void SynthSequencer::ClearHeldSteps()
{
  std::lock_guard<std::mutex> outputLock(outputMutex_);
  std::array<PendingMidiEvent, kMaxPendingMidiEvents> events{};
  std::size_t eventCount = 0;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    StopAuditionLocked(events, eventCount);
    heldSteps_.fill(false);
    heldEditLanes_.fill(false);
    editLane_ = SequencerEditLane::Note;
  }
  EmitPendingMidi(events, eventCount);
}

void SynthSequencer::ClearStep(std::size_t stepIndex)
{
  if (stepIndex >= kSequencerMaxSteps)
  {
    return;
  }

  std::lock_guard<std::mutex> lock(mutex_);
  if (activeTrack_ >= tracks_.size())
  {
    return;
  }

  PatternData& pattern = ActivePatternLocked(tracks_[activeTrack_]);
  if (ActiveNoteCount(pattern.steps[stepIndex]) != 0) ClearStepLocked(pattern.steps.Edit(stepIndex));
  ReleaseEmptyPagesLocked();
}

void SynthSequencer::ClearHeldStepEvents()
{
  std::lock_guard<std::mutex> outputLock(outputMutex_);
  std::array<PendingMidiEvent, kMaxPendingMidiEvents> events{};
  std::size_t eventCount = 0;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (activeTrack_ >= tracks_.size())
    {
      return;
    }

    StopAuditionLocked(events, eventCount);
    PatternData& pattern = ActivePatternLocked(tracks_[activeTrack_]);
    for (std::size_t stepIndex = 0; stepIndex < heldSteps_.size(); ++stepIndex)
    {
      if (heldSteps_[stepIndex] && ActiveNoteCount(pattern.steps[stepIndex]) != 0)
      {
        ClearStepLocked(pattern.steps.Edit(stepIndex));
      }
    }
    ReleaseEmptyPagesLocked();
  }
  EmitPendingMidi(events, eventCount);
}

bool SynthSequencer::CopyStep(std::size_t sourceStep, std::size_t destinationStep)
{
  if (sourceStep >= kSequencerMaxSteps || destinationStep >= kSequencerMaxSteps)
  {
    return false;
  }

  std::lock_guard<std::mutex> lock(mutex_);
  if (activeTrack_ >= tracks_.size())
  {
    return false;
  }

  PatternData& pattern = ActivePatternLocked(tracks_[activeTrack_]);
  if (ActiveNoteCount(pattern.steps[sourceStep]) == 0)
  {
    return false;
  }

  const StepData source = pattern.steps[sourceStep];
  StepData& destination = pattern.steps.Edit(destinationStep);
  destination = source;
  for (NoteEvent& note : destination.notes)
  {
    if (note.active)
      note.lengthPulses = ClampLengthForStepMicrostep(destinationStep, note.microstep, note.lengthPulses);
  }
  ReleaseEmptyPagesLocked();
  return true;
}

std::size_t SynthSequencer::StepNoteEvents(std::size_t trackIndex, std::size_t stepIndex, std::array<StepNoteView, 8>& notes) const
{
  if (trackIndex >= kSynthTrackCount || stepIndex >= kSequencerMaxSteps) return 0;
  std::lock_guard<std::mutex> lock(mutex_);
  std::size_t count = 0;
  for (const NoteEvent& event : ActivePatternLocked(tracks_[trackIndex]).steps[stepIndex].notes)
  {
    if (!event.active || count >= notes.size()) continue;
    notes[count++] = StepNoteView{event.note, event.velocity, event.lengthPulses, event.microstep, event.Hits()};
  }
  return count;
}

bool SynthSequencer::SetStepNotes(std::size_t trackIndex, std::size_t stepIndex, const std::uint8_t* notes, std::size_t count,
                                  std::uint8_t velocity)
{
  if (trackIndex >= kSynthTrackCount || stepIndex >= kSequencerMaxSteps || (count > 0 && notes == nullptr)) return false;
  std::lock_guard<std::mutex> lock(mutex_);
  auto& steps = ActivePatternLocked(tracks_[trackIndex]).steps;
  if (count == 0)
  {
    // No notes: the step is erased, and a page left without notes is given back.
    if (ActiveNoteCount(steps[stepIndex]) != 0) ClearStepLocked(steps.Edit(stepIndex));
    steps.ReleaseEmptyPages();
    if (trackIndex == activeTrack_) ReleaseEmptyPagesLocked();
    return true;
  }
  StepData& step = steps.Edit(stepIndex);
  ClearStepLocked(step);
  std::size_t slot = 0;
  for (std::size_t index = 0; index < count && slot < step.notes.size(); ++index)
  {
    if (notes[index] > 127) continue;
    NoteEvent& event = step.notes[slot++];
    event.active = true;
    event.note = notes[index];
    event.velocity = std::clamp<std::uint8_t>(velocity, 1, 127);
    event.microstep = 0;
    event.lengthPulses = kSequencerPulsesPerStep;
    event.SetHits(1);
  }
  if (trackIndex == activeTrack_) ReleaseEmptyPagesLocked();
  return true;
}

bool SynthSequencer::ToggleStepNotes(std::size_t trackIndex, std::size_t stepIndex, const std::uint8_t* notes, std::size_t count,
                                     const std::uint8_t* velocities)
{
  if (trackIndex >= kSynthTrackCount || stepIndex >= kSequencerMaxSteps || notes == nullptr || count == 0) return false;
  std::lock_guard<std::mutex> lock(mutex_);
  TrackData& track = tracks_[trackIndex];
  std::array<bool, 128> given{};
  std::array<std::uint8_t, 128> velocity{};
  for (std::size_t index = 0; index < count; ++index)
  {
    if (notes[index] >= given.size()) continue;
    given[notes[index]] = true;
    velocity[notes[index]] = velocities != nullptr ? velocities[index] : 0;
  }
  ToggleNotesInStepLocked(ActivePatternLocked(track).steps, stepIndex, given, velocity, track);
  if (trackIndex == activeTrack_) ReleaseEmptyPagesLocked();
  return true;
}

void SynthSequencer::ToggleNotesInStepLocked(StepStore& steps, std::size_t stepIndex, const std::array<bool, 128>& given,
                                             const std::array<std::uint8_t, 128>& velocity, TrackData& track)
{
  const auto& current = steps[stepIndex].notes;
  const bool present = std::any_of(current.begin(), current.end(), [&](const NoteEvent& event) { return event.active && event.note < given.size() && given[event.note]; });
  if (present)
  {
    // Any of them there: they all come out, and none goes in.
    for (NoteEvent& event : steps.Edit(stepIndex).notes)
      if (event.active && event.note < given.size() && given[event.note]) event = NoteEvent{};
    steps.ReleaseEmptyPages();
    return;
  }
  StepData& step = steps.Edit(stepIndex);
  for (std::size_t note = 0; note < given.size(); ++note)
    if (given[note]) ToggleNoteInStepLocked(step, static_cast<std::uint8_t>(note), track, stepIndex, velocity[note]);
}

bool SynthSequencer::MoveNote(std::size_t trackIndex, std::size_t patternIndex, std::size_t stepIndex, std::uint8_t note, std::uint8_t fromMicrostep, std::uint8_t toMicrostep)
{
  if (trackIndex >= kSynthTrackCount || stepIndex >= kSequencerMaxSteps || toMicrostep >= kSequencerMicrostepCount) return false;
  std::lock_guard<std::mutex> lock(mutex_);
  TrackData& track = tracks_[trackIndex];
  if (patternIndex >= std::clamp<std::size_t>(track.patternCount, 1, kSequencerMaxPatternCount)) return false;
  auto& steps = track.patterns[patternIndex].steps;
  const auto& notes = steps[stepIndex].notes;
  const auto at = [&](std::uint8_t microstep) {
    return std::any_of(notes.begin(), notes.end(), [&](const NoteEvent& event) { return event.active && event.note == note && event.microstep == microstep; });
  };
  if (!at(fromMicrostep) || at(toMicrostep)) return false;
  for (NoteEvent& event : steps.Edit(stepIndex).notes)
  {
    if (!event.active || event.note != note || event.microstep != fromMicrostep) continue;
    event.microstep = toMicrostep;
    event.lengthPulses = ClampLengthForStepMicrostep(stepIndex, toMicrostep, event.lengthPulses);
    break;
  }
  return true;
}

bool SynthSequencer::RemoveNote(std::size_t trackIndex, std::size_t patternIndex, std::size_t stepIndex, std::uint8_t note)
{
  if (trackIndex >= kSynthTrackCount || stepIndex >= kSequencerMaxSteps) return false;
  std::lock_guard<std::mutex> lock(mutex_);
  TrackData& track = tracks_[trackIndex];
  if (patternIndex >= std::clamp<std::size_t>(track.patternCount, 1, kSequencerMaxPatternCount) || !track.patterns.Allocated(patternIndex)) return false;
  auto& steps = track.patterns[patternIndex].steps;
  const auto& held = steps[stepIndex].notes;
  if (std::none_of(held.begin(), held.end(), [&](const NoteEvent& event) { return event.active && event.note == note; })) return false;
  for (NoteEvent& event : steps.Edit(stepIndex).notes)
    if (event.active && event.note == note) event = NoteEvent{};
  steps.ReleaseEmptyPages();
  if (trackIndex == activeTrack_) ReleaseEmptyPagesLocked();
  return true;
}

bool SynthSequencer::InsertNote(std::size_t trackIndex, std::size_t patternIndex, std::size_t stepIndex, std::uint8_t microstep,
                                std::uint8_t note, std::uint8_t velocity, std::uint16_t lengthPulses)
{
  if (trackIndex >= kSynthTrackCount || stepIndex >= kSequencerMaxSteps || note > 127) return false;
  std::lock_guard<std::mutex> lock(mutex_);
  TrackData& track = tracks_[trackIndex];
  if (patternIndex >= std::clamp<std::size_t>(track.patternCount, 1, kSequencerMaxPatternCount)) return false;
  StepData& step = track.patterns[patternIndex].steps.Edit(stepIndex);
  NoteEvent* target = nullptr;
  for (NoteEvent& event : step.notes)
    if (event.active && event.note == note) { target = &event; break; }
  if (target == nullptr)
    for (NoteEvent& event : step.notes)
      if (!event.active) { target = &event; break; }
  if (target == nullptr) return false;
  target->active = true;
  target->note = note;
  target->velocity = std::clamp<std::uint8_t>(velocity, 1, 127);
  target->microstep = std::min<std::uint8_t>(microstep, kSequencerMicrostepCount - 1);
  target->lengthPulses = ClampLengthInPattern(track.patternSteps[patternIndex], stepIndex, target->microstep, std::max<std::uint16_t>(1, lengthPulses));
  target->SetHits(1);
  if (trackIndex == activeTrack_) ReleaseEmptyPagesLocked();
  return true;
}

std::uint64_t SynthSequencer::PlayPulse() const
{
  std::lock_guard<std::mutex> lock(mutex_);
  return playing_ ? absolutePulse_ : 0;
}

bool SynthSequencer::StepSelected(std::size_t stepIndex) const
{
  std::lock_guard<std::mutex> lock(mutex_);
  return stepIndex < heldSteps_.size() && heldSteps_[stepIndex];
}

bool SynthSequencer::StepHasEvents(std::size_t stepIndex) const
{
  if (stepIndex >= kSequencerMaxSteps)
  {
    return false;
  }

  std::lock_guard<std::mutex> lock(mutex_);
  if (activeTrack_ >= tracks_.size())
  {
    return false;
  }

  return ActiveNoteCount(ActivePatternLocked(tracks_[activeTrack_]).steps[stepIndex]) > 0;
}

std::size_t SynthSequencer::StepNotes(std::size_t stepIndex, std::array<std::uint8_t, 8>& notes) const
{
  static_assert(std::tuple_size<decltype(StepData::notes)>::value == 8, "StepNotes returns one pitch per note slot.");
  if (stepIndex >= kSequencerMaxSteps)
  {
    return 0;
  }

  std::lock_guard<std::mutex> lock(mutex_);
  if (activeTrack_ >= tracks_.size())
  {
    return 0;
  }

  // A 128-entry presence table orders the pitches without sorting a fixed buffer.
  std::array<bool, 128> present{};
  for (const NoteEvent& note : ActivePatternLocked(tracks_[activeTrack_]).steps[stepIndex].notes)
    if (note.active && note.note < present.size()) present[note.note] = true;
  std::size_t count = 0;
  for (std::size_t pitch = 0; pitch < present.size() && count < notes.size(); ++pitch)
    if (present[pitch]) notes[count++] = static_cast<std::uint8_t>(pitch);
  return count;
}

std::uint8_t SynthSequencer::SelectedNoteVelocity(std::uint8_t pitch) const
{
  std::lock_guard<std::mutex> lock(mutex_);
  const StepData* step = PrimaryHeldStepLocked();
  if (step != nullptr)
    for (const NoteEvent& note : step->notes)
      if (note.active && note.note == pitch) return note.velocity;
  return 0;
}

void SynthSequencer::QuantizeStep(std::size_t stepIndex)
{
  if (stepIndex >= kSequencerMaxSteps)
  {
    return;
  }

  std::lock_guard<std::mutex> lock(mutex_);
  if (activeTrack_ >= tracks_.size())
  {
    return;
  }

  PatternData& pattern = ActivePatternLocked(tracks_[activeTrack_]);
  if (ActiveNoteCount(pattern.steps[stepIndex]) != 0) QuantizeStepLocked(pattern.steps.Edit(stepIndex), stepIndex);
  ReleaseEmptyPagesLocked();
}

void SynthSequencer::QuantizeHeldSteps()
{
  std::lock_guard<std::mutex> lock(mutex_);
  if (activeTrack_ >= tracks_.size())
  {
    return;
  }

  PatternData& pattern = ActivePatternLocked(tracks_[activeTrack_]);
  for (std::size_t stepIndex = 0; stepIndex < heldSteps_.size(); ++stepIndex)
  {
    if (heldSteps_[stepIndex] && ActiveNoteCount(pattern.steps[stepIndex]) != 0)
    {
      QuantizeStepLocked(pattern.steps.Edit(stepIndex), stepIndex);
    }
  }
  ReleaseEmptyPagesLocked();
}

void SynthSequencer::QuantizeActivePattern()
{
  std::lock_guard<std::mutex> lock(mutex_);
  if (activeTrack_ >= tracks_.size())
  {
    return;
  }

  PatternData& pattern = ActivePatternLocked(tracks_[activeTrack_]);
  for (std::size_t stepIndex = pattern.steps.NextInUse(0); stepIndex < pattern.steps.size(); stepIndex = pattern.steps.NextInUse(stepIndex + 1))
  {
    if (ActiveNoteCount(pattern.steps[stepIndex]) != 0) QuantizeStepLocked(pattern.steps.Edit(stepIndex), stepIndex);
  }
  ReleaseEmptyPagesLocked();
}

void SynthSequencer::QuantizePattern(std::size_t patternIndex)
{
  std::lock_guard<std::mutex> lock(mutex_);
  if (activeTrack_ >= tracks_.size() || patternIndex >= tracks_[activeTrack_].patternCount ||
      patternIndex >= kSequencerMaxPatternCount)
  {
    return;
  }

  PatternData& pattern = tracks_[activeTrack_].patterns[patternIndex];
  for (std::size_t stepIndex = pattern.steps.NextInUse(0); stepIndex < pattern.steps.size(); stepIndex = pattern.steps.NextInUse(stepIndex + 1))
  {
    if (ActiveNoteCount(pattern.steps[stepIndex]) != 0) QuantizeStepLocked(pattern.steps.Edit(stepIndex), stepIndex);
  }
  ReleaseEmptyPagesLocked();
}

bool SynthSequencer::IsStepHeld(std::size_t stepIndex) const
{
  std::lock_guard<std::mutex> lock(mutex_);
  return stepIndex < heldSteps_.size() && heldSteps_[stepIndex];
}

bool SynthSequencer::HasHeldSteps() const
{
  std::lock_guard<std::mutex> lock(mutex_);
  return std::any_of(heldSteps_.begin(), heldSteps_.end(), [](bool held) { return held; });
}

bool SynthSequencer::PressEditLane(std::size_t row)
{
  std::lock_guard<std::mutex> outputLock(outputMutex_);
  if (row >= kKeyboardMatrixRows)
  {
    return false;
  }

  std::array<PendingMidiEvent, kMaxPendingMidiEvents> events{};
  std::size_t eventCount = 0;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!std::any_of(heldSteps_.begin(), heldSteps_.end(), [](bool held) { return held; }))
    {
      return false;
    }

    StopAuditionLocked(events, eventCount);
    heldEditLanes_[row] = true;
    switch (row)
    {
      case 0:
        editLane_ = SequencerEditLane::Note;
        ReleaseEmptyPagesLocked();
        break;
      case 1:
        editLane_ = SequencerEditLane::Length;
        ReleaseEmptyPagesLocked();
        break;
      case 2:
        editLane_ = SequencerEditLane::Velocity;
        ReleaseEmptyPagesLocked();
        break;
      case 3:
        editLane_ = SequencerEditLane::Micro;
        ReleaseEmptyPagesLocked();
        break;
      default:
        return false;
    }
  }
  EmitPendingMidi(events, eventCount);
  return true;
}

bool SynthSequencer::ReleaseEditLane(std::size_t row)
{
  std::lock_guard<std::mutex> outputLock(outputMutex_);
  if (row >= kKeyboardMatrixRows)
  {
    return false;
  }

  std::array<PendingMidiEvent, kMaxPendingMidiEvents> events{};
  std::size_t eventCount = 0;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    heldEditLanes_[row] = false;
    StopAuditionLocked(events, eventCount);
  }
  EmitPendingMidi(events, eventCount);
  return true;
}

bool SynthSequencer::HandlePerformanceKeyPress(std::size_t, std::size_t column, std::uint8_t note)
{
  if (column < kHomePerformanceFirstColumn || column > kHomePerformanceLastColumn)
  {
    return false;
  }

  const std::size_t bucket = column - kHomePerformanceFirstColumn;
  std::lock_guard<std::mutex> lock(mutex_);
  if (!std::any_of(heldSteps_.begin(), heldSteps_.end(), [](bool held) { return held; }) || activeTrack_ >= tracks_.size())
  {
    return false;
  }

  if (editLane_ == SequencerEditLane::Note)
  {
    TrackData& track = tracks_[activeTrack_];
    PatternData& pattern = ActivePatternLocked(track);
    for (std::size_t stepIndex = 0; stepIndex < heldSteps_.size(); ++stepIndex)
    {
      if (heldSteps_[stepIndex])
      {
        ToggleNoteInStepLocked(pattern.steps.Edit(stepIndex), note, track, stepIndex);
      }
    }
    ReleaseEmptyPagesLocked();
    return true;
  }

  if (editLane_ == SequencerEditLane::Length)
  {
    ApplyLengthToHeldStepsLocked(kLengthBuckets[bucket]);
    return true;
  }

  if (editLane_ == SequencerEditLane::Velocity)
  {
    ApplyVelocityToHeldStepsLocked(kVelocityBuckets[bucket]);
    return true;
  }

  if (editLane_ == SequencerEditLane::Micro)
  {
    if (bucket < kSequencerMicrostepCount)
    {
      ApplyMicrostepToHeldStepsLocked(static_cast<std::uint8_t>(bucket));
    }
    return true;
  }

  return false;
}

SequencerUiState SynthSequencer::BuildUiState(std::size_t activeTrack) const
{
  std::lock_guard<std::mutex> lock(mutex_);
  SequencerUiState state;
  state.visible = true;
  state.activeTrack = activeTrack;
  state.playing = playing_;
  state.recordArmed = recordArmed_;
  state.bpm = bpm_;
  state.swing = swing_;
  state.currentStep = playbackStep_;
  state.patternProgress = playing_ ? static_cast<float>(absolutePulse_ % (kSequencerStepCount * kSequencerPulsesPerStep)) /
                                         static_cast<float>(kSequencerStepCount * kSequencerPulsesPerStep)
                                   : 0.0f;
  state.editLane = editLane_;
  state.editLanePressed = heldEditLanes_;
  state.heldStepCount = static_cast<std::size_t>(std::count(heldSteps_.begin(), heldSteps_.end(), true));
  state.hasHeldSteps = state.heldStepCount != 0;
  for (std::size_t index = 0; index < tracks_.size(); ++index)
  {
    const auto& source = tracks_[index];
    auto& summary = state.tracks[index];
    summary.enabled = source.enabled;
    summary.muted = source.muted;
    summary.activePattern = source.activePattern;
    summary.patternCount = source.patternCount;
    summary.stepCount = TrackStepCountOf(source);
    summary.currentStep = std::min<std::size_t>(trackCurrentPulse_[index] / kSequencerPulsesPerStep, summary.stepCount - 1);
    summary.populatedStepCount = ActivePatternLocked(source).steps.PopulatedSteps();
    summary.queuedPattern = queuedPattern_[index] < source.patternCount ? queuedPattern_[index] : kNoPattern;
    summary.songSilent = playing_ && songSilent_[index];
  }
  state.songMode = songMode_;
  state.songLoop = songLoop_;
  state.songLength = songLength_;
  state.songPlaying = playing_ && songEngaged_;
  if (state.songPlaying)
  {
    state.songRow = songRow_;
    state.songBar = songBar_;
    // Pulses of the row processed so far: whole bars, plus the bar holding the last processed pulse.
    const std::uint64_t repeats = songRow_ < songLength_ ? song_[songRow_].repeats : 1;
    const std::uint64_t inBar = absolutePulse_ > 0 ? (absolutePulse_ - 1) % kSongBarPulses + 1 : 0;
    const std::uint64_t done = static_cast<std::uint64_t>(songBar_) * kSongBarPulses + inBar;
    state.songRowProgress = std::min(1.0f, static_cast<float>(done) / static_cast<float>(repeats * kSongBarPulses));
  }
  else
  {
    state.songRow = songLength_ > 0 ? std::min(songStartRow_, songLength_ - 1) : 0;
  }
  state.songCueRow = playing_ && songJumpPending_ && songJumpRow_ < songLength_ ? songJumpRow_ : kSongMaxRows;
  const TrackData* track = activeTrack < tracks_.size() ? &tracks_[activeTrack] : nullptr;
  if (track != nullptr)
  {
    // The playhead and progress follow the shown track's own loop length.
    const std::size_t loopPulses = state.tracks[activeTrack].stepCount * kSequencerPulsesPerStep;
    state.currentStep = state.tracks[activeTrack].currentStep;
    state.patternProgress = playing_ ? static_cast<float>(trackNextPulse_[activeTrack] % loopPulses) / static_cast<float>(loopPulses)
                                     : 0.0f;
    state.trackEnabled = track->enabled;
    state.trackMuted = track->muted;
    state.patternCount = std::clamp<std::size_t>(track->patternCount, 1, kSequencerMaxPatternCount);
    state.activePattern = std::min(track->activePattern, state.patternCount - 1);
    state.populatedStepCount = state.tracks[activeTrack].populatedStepCount;
    for (std::size_t index = 0; index < state.patternCount; ++index)
    {
      if (!track->patterns.Allocated(index)) continue;
      const std::size_t populated = track->patterns[index].steps.PopulatedSteps();
      state.patternStepCounts[index] = static_cast<std::uint16_t>(populated);
      state.patternHasEvents[index] = populated != 0;
    }
  }
  const StepData* primaryHeldStep = nullptr;
  if (track != nullptr)
  {
    state.viewPage = ViewPageLocked(activeTrack);
    state.viewPageCount = (TrackStepCountOf(*track) + kSequencerStepCount - 1) / kSequencerStepCount;
    state.firstViewStep = state.viewPage * kSequencerStepCount;
    state.viewFollow = viewFollow_[activeTrack];
  }
  for (std::size_t stepIndex = 0; stepIndex < heldSteps_.size(); ++stepIndex)
  {
    if (!heldSteps_[stepIndex]) continue;
    if (track != nullptr) primaryHeldStep = &ActivePatternLocked(*track).steps[stepIndex];
    state.primaryHeldStep = stepIndex;
    if (primaryHeldStep != nullptr)
      for (const NoteEvent& note : primaryHeldStep->notes)
        if (note.active && (state.primaryLowestNote < 0 || note.note < state.primaryLowestNote)) state.primaryLowestNote = note.note;
    break;
  }
  for (std::size_t cellIndex = 0; cellIndex < state.steps.size(); ++cellIndex)
  {
    const std::size_t stepIndex = state.firstViewStep + cellIndex;
    const StepData* step = track != nullptr ? &ActivePatternLocked(*track).steps[stepIndex] : nullptr;
    auto& cell = state.steps[cellIndex];
    if (step != nullptr)
      for (const NoteEvent& note : step->notes)
        if (note.active)
        {
          ++cell.noteCount;
          // Includes every editable value so UI feedback also catches chord,
          // length and timing changes without retaining a second pattern copy.
          cell.contentFingerprint = cell.contentFingerprint * 16777619u ^
              (static_cast<std::uint32_t>(note.note) | (static_cast<std::uint32_t>(note.velocity) << 7) |
               (static_cast<std::uint32_t>(note.lengthPulses) << 14) | (static_cast<std::uint32_t>(note.microstep) << 25) |
               (static_cast<std::uint32_t>(note.extraHits) << 28));
          if (cell.lowestNote < 0 || note.note < cell.lowestNote) cell.lowestNote = note.note;
          cell.velocity = std::max(cell.velocity, note.velocity);
          cell.hits = std::max(cell.hits, note.Hits());
        }
    cell.hasEvents = cell.noteCount > 0;
    cell.held = heldSteps_[stepIndex];
    cell.playing = playing_ && state.currentStep == stepIndex;
  }
  if (primaryHeldStep != nullptr)
  {
    state.noteCount = ActiveNoteCount(*primaryHeldStep);
    // A 128-note presence table gives stable pitch order without allocating a chord copy.
    std::array<bool, 128> pitches{};
    for (const NoteEvent& note : primaryHeldStep->notes)
      if (note.active && note.note < pitches.size()) pitches[note.note] = true;
    static constexpr std::array<const char*, 12> names{{"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"}};
    for (std::size_t pitch = 0; pitch < pitches.size(); ++pitch)
    {
      if (!pitches[pitch]) continue;
      if (!state.noteNames.empty()) state.noteNames += ' ';
      state.noteNames += names[pitch % names.size()];
      state.noteNames += std::to_string(static_cast<int>(pitch / names.size()) - 1);
    }
  }
  state.valueLabel = ValueLabelLocked(primaryHeldStep);
  PopulateEditValueStateLocked(state, primaryHeldStep);
  if (track != nullptr && editLane_ != SequencerEditLane::Note)
  {
    int firstValue = -1;
    for (std::size_t index = 0; index < heldSteps_.size(); ++index)
    {
      if (!heldSteps_[index]) continue;
      const StepData& step = ActivePatternLocked(*track).steps[index];
      for (std::size_t slot = 0; slot < step.notes.size(); ++slot)
      {
        const NoteEvent& note = step.notes[slot];
        if (!note.active) continue;
        const int value = editLane_ == SequencerEditLane::Length ? note.lengthPulses :
            editLane_ == SequencerEditLane::Velocity ? note.velocity : editLane_ == SequencerEditLane::Hits ? note.Hits() : note.microstep;
        if (firstValue < 0) firstValue = value;
        else if (value != firstValue) state.editValueMixed = true;
      }
    }
    if (state.editValueMixed) state.valueLabel = std::string(SequencerEditLaneLabel(editLane_)) + " MIXED";
  }
  const ArpVoice& arp = arps_[std::min(activeTrack, arps_.size() - 1)];
  state.arpActive = ArpActiveLocked(arp);
  state.arpTrack = static_cast<std::uint8_t>(ArpTrack(arp));
  state.arpNoteCount = static_cast<std::uint8_t>(arp.keyCount);
  for (std::size_t index = 0; index < arp.keyCount; ++index) state.arpNotes[index] = arp.keys[index].note;
  state.arpSounding = arp.soundingNote;
  state.arpTick = arp.tick;
  state.arpLastNote = arp.lastPlayed;
  return state;
}

void SynthSequencer::SetRecordArmed(bool armed)
{
  std::lock_guard<std::mutex> lock(mutex_);
  recordArmed_ = armed;
  if (!armed) FinalizeAllHeldRecordingsLocked();
}

bool SynthSequencer::RecordArmed() const
{
  std::lock_guard<std::mutex> lock(mutex_);
  return recordArmed_;
}

void SynthSequencer::RecordNoteOn(std::size_t trackIndex, std::uint8_t note, std::uint8_t velocity)
{
  if (velocity == 0)
  {
    RecordNoteOff(trackIndex, note);
    return;
  }
  if (trackIndex >= kSynthTrackCount || note > 127)
  {
    return;
  }

  std::lock_guard<std::mutex> lock(mutex_);
  RecordNoteOnLocked(trackIndex, note, velocity);
}

void SynthSequencer::RecordNoteOnLocked(std::size_t trackIndex, std::uint8_t note, std::uint8_t velocity)
{
  if (!playing_ || !recordArmed_ || trackIndex >= kSynthTrackCount || note > 127 || velocity == 0)
  {
    return;
  }
  if (songSilent_[trackIndex])
  {
    return;  // The playing song row leaves this track out: there is no pattern to record into.
  }

  const std::uint64_t now = RecordPulseLocked(trackIndex);
  // A repeated note-on for a held pitch closes the earlier take first.
  for (HeldRecording& recording : heldRecordings_)
    if (recording.active && recording.trackIndex == trackIndex && recording.note == note) FinalizeHeldRecordingLocked(recording, now);

  TrackData& track = tracks_[trackIndex];
  const std::size_t stepCount = TrackStepCountOf(track);
  const std::uint32_t loopPulses = static_cast<std::uint32_t>(stepCount * kSequencerPulsesPerStep);
  // Before the first pulse of a run the playhead sits on the pulse about to be processed.
  const std::uint32_t position = (trackStarted_[trackIndex] ? trackCurrentPulse_[trackIndex] : trackNextPulse_[trackIndex]) % loopPulses;

  // Pick the microstep slot whose (swung) playback time is nearest. Ties go to the later slot,
  // so rounding past microstep 5 lands on the next step and the last step wraps to step 0.
  std::size_t bestStep = 0;
  std::uint8_t bestMicrostep = 0;
  std::uint32_t bestDistance = loopPulses;
  bool bestAhead = false;
  std::int32_t bestOffset = 0;
  const std::size_t center = position / kSequencerPulsesPerStep;
  for (std::size_t stepIndex = 0; stepIndex < stepCount; ++stepIndex)
  {
    // Swing moves a slot by less than a step, so the nearest one is at most two steps away.
    const std::size_t away = (stepIndex + stepCount - center) % stepCount;
    if (away > 2 && away + 2 < stepCount) continue;
    for (std::uint8_t microstep = 0; microstep < kSequencerMicrostepCount; ++microstep)
    {
      const std::uint32_t slot = NoteOffsetPulsesLocked(stepIndex, microstep) % loopPulses;
      const std::uint32_t forward = (slot + loopPulses - position) % loopPulses;
      const std::uint32_t backward = (position + loopPulses - slot) % loopPulses;
      const bool ahead = forward <= backward;
      const std::uint32_t distance = ahead ? forward : backward;
      // Coincident slots (swing overlaps) keep the note in the step whose own grid holds it.
      const bool better = distance < bestDistance ||
          (distance == bestDistance && ahead && !bestAhead) ||
          (distance == bestDistance && ahead == bestAhead && microstep < bestMicrostep);
      if (!better) continue;
      bestDistance = distance;
      bestAhead = ahead;
      bestStep = stepIndex;
      bestMicrostep = microstep;
      bestOffset = ahead ? static_cast<std::int32_t>(forward) : -static_cast<std::int32_t>(backward);
    }
  }

  StepData& step = ActivePatternLocked(track).steps.Edit(bestStep);
  NoteEvent* target = nullptr;
  for (NoteEvent& event : step.notes)
    if (event.active && event.note == note) { target = &event; break; }
  if (target == nullptr)
    for (NoteEvent& event : step.notes)
      if (!event.active) { target = &event; break; }
  if (target == nullptr)
  {
    return;  // Eight notes per step: a full step drops the take.
  }

  target->active = true;
  target->note = note;
  target->velocity = std::min<std::uint8_t>(velocity, 127);
  target->microstep = bestMicrostep;
  target->lengthPulses = ClampLengthInPattern(stepCount, bestStep, bestMicrostep, kSequencerPulsesPerMicrostep);
  target->SetHits(1);  // what was played: one hit

  HeldRecording* held = nullptr;
  for (HeldRecording& recording : heldRecordings_)
    if (!recording.active) { held = &recording; break; }
  if (held == nullptr)
  {
    held = &heldRecordings_[0];
    for (HeldRecording& recording : heldRecordings_)
      if (recording.onPulse < held->onPulse) held = &recording;
    FinalizeHeldRecordingLocked(*held, now);
  }
  held->active = true;
  held->trackIndex = static_cast<std::uint8_t>(trackIndex);
  held->note = note;
  held->pattern = std::min(track.activePattern, track.patternCount - 1);
  held->step = static_cast<std::uint8_t>(bestStep);
  held->onPulse = now;
  held->startPulse = bestOffset >= 0 ? now + static_cast<std::uint64_t>(bestOffset) : now - std::min<std::uint64_t>(now, static_cast<std::uint64_t>(-bestOffset));

  // The performer already sounded this note: a slot just ahead of the playhead must not
  // trigger it again when playback reaches it in this pass.
  if (bestOffset >= 0 && held->startPulse >= absolutePulse_)
  {
    SuppressedNote* entry = nullptr;
    for (SuppressedNote& suppressed : suppressedNotes_)
      if (!suppressed.active || (suppressed.trackIndex == trackIndex && suppressed.note == note)) { entry = &suppressed; break; }
    if (entry == nullptr)
    {
      entry = &suppressedNotes_[0];
      for (SuppressedNote& suppressed : suppressedNotes_)
        if (suppressed.pulse < entry->pulse) entry = &suppressed;
    }
    *entry = SuppressedNote{held->startPulse, static_cast<std::uint8_t>(trackIndex), note, true};
  }
}

void SynthSequencer::RecordNoteOff(std::size_t trackIndex, std::uint8_t note)
{
  if (trackIndex >= kSynthTrackCount)
  {
    return;
  }

  std::lock_guard<std::mutex> lock(mutex_);
  RecordNoteOffLocked(trackIndex, note);
}

void SynthSequencer::RecordNoteOffLocked(std::size_t trackIndex, std::uint8_t note)
{
  const std::uint64_t now = RecordPulseLocked(trackIndex);
  for (HeldRecording& recording : heldRecordings_)
    if (recording.active && recording.trackIndex == trackIndex && recording.note == note) FinalizeHeldRecordingLocked(recording, now);
}

void SynthSequencer::SetArpSettings(std::size_t trackIndex, const ArpSettings& settings)
{
  if (trackIndex >= kSynthTrackCount) return;
  ArpSettings next = settings;
  if (static_cast<std::size_t>(next.mode) >= kArpModeCount) next.mode = ArpMode::Off;
  next.rate = std::min<std::uint8_t>(next.rate, kArpRateCount - 1);
  next.octaves = std::clamp<std::uint8_t>(next.octaves, 1, kArpMaxOctaves);
  next.gate = std::clamp(next.gate, kArpMinGate, kArpMaxGate);

  std::lock_guard<std::mutex> outputLock(outputMutex_);
  std::array<PendingMidiEvent, kMaxPendingMidiEvents> events{};
  std::size_t eventCount = 0;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    ArpVoice& arp = arps_[trackIndex];
    const ArpSettings previous = arp.settings;
    arp.settings = next;
    if (next.mode == ArpMode::Off)
    {
      ReleaseArpLocked(arp, events, eventCount);
    }
    else
    {
      if (previous.latch && !next.latch)
      {
        // Unlatching keeps only the keys still held.
        for (std::size_t index = arp.keyCount; index > 0; --index)
          if (!arp.keys[index - 1].down) RemoveArpKeyLocked(arp, index - 1);
        if (arp.keyCount == 0) ReleaseArpLocked(arp, events, eventCount);
      }
      // A new rate applies from the next note, which moves onto the new grid.
      if (next.rate != previous.rate) RescheduleArpLocked(arp);
    }
  }
  EmitPendingMidi(events, eventCount);
}

ArpSettings SynthSequencer::GetArpSettings(std::size_t trackIndex) const
{
  std::lock_guard<std::mutex> lock(mutex_);
  return trackIndex < kSynthTrackCount ? arps_[trackIndex].settings : ArpSettings{};
}

void SynthSequencer::ArpNoteOn(std::size_t trackIndex, std::uint8_t note, std::uint8_t velocity)
{
  if (velocity == 0)
  {
    ArpNoteOff(trackIndex, note);
    return;
  }
  if (trackIndex >= kSynthTrackCount || note > 127)
  {
    return;
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (arps_[trackIndex].settings.mode == ArpMode::Off) return;
  }

  // While stopped the arps run on the clock worker's free clock.
  EnsureClockWorkerStarted();
  std::lock_guard<std::mutex> outputLock(outputMutex_);
  std::array<PendingMidiEvent, kMaxPendingMidiEvents> events{};
  std::size_t eventCount = 0;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    ArpVoice& arp = arps_[trackIndex];
    if (arp.settings.mode == ArpMode::Off)
    {
      return;
    }

    const bool running = ArpActiveLocked(arp);
    // A press after every key let go of a latched set starts a new set; the arp keeps its timing.
    if (running && arp.settings.latch &&
        std::none_of(arp.keys.begin(), arp.keys.begin() + static_cast<std::ptrdiff_t>(arp.keyCount), [](const ArpKey& key) { return key.down; }))
    {
      arp.keyCount = 0;
      ResetArpOrderLocked(arp);
    }

    ArpKey* key = nullptr;
    for (std::size_t index = 0; index < arp.keyCount; ++index)
      if (arp.keys[index].note == note) key = &arp.keys[index];
    if (key == nullptr && arp.keyCount < arp.keys.size()) key = &arp.keys[arp.keyCount++];
    if (key == nullptr)
    {
      return;  // Sixteen notes are held already.
    }
    *key = ArpKey{note, std::min<std::uint8_t>(velocity, 127), true};

    if (!running)
    {
      // The first note sounds now. When no other arp keeps the free clock running, the clock
      // worker counts the first pulse from here.
      const bool clockRunning = std::any_of(arps_.begin(), arps_.end(),
                                            [&](const ArpVoice& other) { return &other != &arp && ArpClockLocked(other); });
      if (!playing_ && !clockRunning) clockStartMicros_ = NowMicros();
      FireArpNoteLocked(arp, events, eventCount);
      if (playing_) AlignArpToTransportLocked(arp, absolutePulse_);
      else arp.nextPulse = arp.pulse + ArpPeriodLocked(arp);
    }
  }
  EmitPendingMidi(events, eventCount);
}

void SynthSequencer::ArpNoteOff(std::size_t trackIndex, std::uint8_t note)
{
  if (trackIndex >= kSynthTrackCount)
  {
    return;
  }

  std::lock_guard<std::mutex> outputLock(outputMutex_);
  std::array<PendingMidiEvent, kMaxPendingMidiEvents> events{};
  std::size_t eventCount = 0;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    ArpVoice& arp = arps_[trackIndex];
    for (std::size_t index = 0; index < arp.keyCount; ++index)
    {
      if (arp.keys[index].note != note) continue;
      if (arp.settings.latch) arp.keys[index].down = false;
      else RemoveArpKeyLocked(arp, index);
      break;
    }
    // Letting go of the last key stops the arp and its note.
    if (arp.keyCount == 0) ReleaseArpLocked(arp, events, eventCount);
  }
  EmitPendingMidi(events, eventCount);
}

void SynthSequencer::ArpReleaseAll()
{
  std::lock_guard<std::mutex> outputLock(outputMutex_);
  std::array<PendingMidiEvent, kMaxPendingMidiEvents> events{};
  std::size_t eventCount = 0;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    ReleaseAllArpsLocked(events, eventCount);
  }
  EmitPendingMidi(events, eventCount);
}

void SynthSequencer::ArpRelease(std::size_t trackIndex)
{
  if (trackIndex >= kSynthTrackCount) return;
  std::lock_guard<std::mutex> outputLock(outputMutex_);
  std::array<PendingMidiEvent, kMaxPendingMidiEvents> events{};
  std::size_t eventCount = 0;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    ReleaseArpLocked(arps_[trackIndex], events, eventCount);
  }
  EmitPendingMidi(events, eventCount);
}

bool SynthSequencer::ArpActive() const
{
  std::lock_guard<std::mutex> lock(mutex_);
  return AnyArpActiveLocked();
}

bool SynthSequencer::ArpActive(std::size_t trackIndex) const
{
  std::lock_guard<std::mutex> lock(mutex_);
  return trackIndex < kSynthTrackCount && ArpActiveLocked(arps_[trackIndex]);
}

#if defined(CARDENZA_SEQUENCER_TEST_HOOKS)
void SynthSequencer::UseManualClockForTest()
{
  // A running flag without a worker: Play() works and EnsureClockWorkerStarted() starts nothing.
  clockWorkerRunning_.store(true);
}

void SynthSequencer::AdvancePulsesForTest(std::size_t pulses)
{
  for (std::size_t index = 0; index < pulses; ++index)
  {
    std::uint64_t startMicros = 0;
    if (ClockNeeded(startMicros)) ProcessPulse();
  }
}
#endif

bool SynthSequencer::QueuePattern(std::size_t trackIndex, std::size_t patternIndex)
{
  if (trackIndex >= kSynthTrackCount || patternIndex >= kSequencerMaxPatternCount)
  {
    return false;
  }

  std::lock_guard<std::mutex> lock(mutex_);
  TrackData& track = tracks_[trackIndex];
  track.patternCount = std::clamp<std::size_t>(track.patternCount, 1, kSequencerMaxPatternCount);
  track.activePattern = std::min(track.activePattern, track.patternCount - 1);
  if (patternIndex > track.patternCount)
  {
    return false;
  }

  if (patternIndex == track.patternCount)
  {
    if (track.patternCount >= kSequencerMaxPatternCount)
    {
      return false;
    }
    track.patternSteps[patternIndex] = track.patternSteps[track.activePattern];
    ++track.patternCount;
  }

  if (!playing_)
  {
    track.activePattern = patternIndex;
    queuedPattern_[trackIndex] = kNoPattern;
  }
  else
  {
    queuedPattern_[trackIndex] = patternIndex == track.activePattern ? kNoPattern : patternIndex;
  }
  if (trackIndex == activeTrack_) ReleaseEmptyPagesLocked();
  return true;
}

void SynthSequencer::SetSwing(std::uint8_t percent)
{
  std::lock_guard<std::mutex> lock(mutex_);
  swing_ = std::clamp(percent, kMinSwing, kMaxSwing);
}

std::uint8_t SynthSequencer::Swing() const
{
  std::lock_guard<std::mutex> lock(mutex_);
  return swing_;
}

void SynthSequencer::SetTrackStepCount(std::size_t trackIndex, std::size_t steps)
{
  if (trackIndex >= kSynthTrackCount)
  {
    return;
  }

  std::lock_guard<std::mutex> lock(mutex_);
  TrackData& track = tracks_[trackIndex];
  track.patternSteps[std::min(track.activePattern, ClampedPatternCount(track) - 1)] = static_cast<std::uint16_t>(std::clamp<std::size_t>(steps, 1, kSequencerMaxSteps));
  // A shorter loop keeps its phase; reaching pulse 0 runs the usual loop boundary.
  const std::uint16_t loopPulses = static_cast<std::uint16_t>(TrackStepCountOf(track) * kSequencerPulsesPerStep);
  trackNextPulse_[trackIndex] = static_cast<std::uint16_t>(trackNextPulse_[trackIndex] % loopPulses);
  trackCurrentPulse_[trackIndex] = static_cast<std::uint16_t>(trackCurrentPulse_[trackIndex] % loopPulses);
}

std::size_t SynthSequencer::TrackStepCount(std::size_t trackIndex) const
{
  std::lock_guard<std::mutex> lock(mutex_);
  return trackIndex < tracks_.size() ? TrackStepCountOf(tracks_[trackIndex]) : kSequencerStepCount;
}

std::size_t SynthSequencer::PatternStepCount(std::size_t trackIndex, std::size_t patternIndex) const
{
  std::lock_guard<std::mutex> lock(mutex_);
  if (trackIndex >= tracks_.size() || patternIndex >= kSequencerMaxPatternCount) return kSequencerStepCount;
  return std::clamp<std::size_t>(tracks_[trackIndex].patternSteps[patternIndex], 1, kSequencerMaxSteps);
}

std::size_t SynthSequencer::ViewPageLocked(std::size_t trackIndex) const
{
  const TrackData& track = tracks_[trackIndex];
  if (viewPattern_[trackIndex] != track.activePattern)
  {
    viewPattern_[trackIndex] = static_cast<std::uint8_t>(track.activePattern);
    viewPage_[trackIndex] = 0;
  }
  const std::size_t stepCount = TrackStepCountOf(track);
  const std::size_t pageCount = (stepCount + kSequencerStepCount - 1) / kSequencerStepCount;
  // The page stays put while steps are selected, so an edit never moves away under the fingers.
  if (playing_ && viewFollow_[trackIndex] && std::none_of(heldSteps_.begin(), heldSteps_.end(), [](bool held) { return held; }))
    viewPage_[trackIndex] = static_cast<std::uint8_t>(std::min<std::size_t>(trackCurrentPulse_[trackIndex] / kSequencerPulsesPerStep, stepCount - 1) / kSequencerStepCount);
  return std::min<std::size_t>(viewPage_[trackIndex], pageCount - 1);
}

void SynthSequencer::SetViewPage(std::size_t trackIndex, std::size_t page)
{
  if (trackIndex >= tracks_.size()) return;
  std::lock_guard<std::mutex> lock(mutex_);
  (void)ViewPageLocked(trackIndex);
  viewPage_[trackIndex] = static_cast<std::uint8_t>(std::min(page, kSequencerMaxStepPages - 1));
  viewFollow_[trackIndex] = false;
}

std::size_t SynthSequencer::ViewFirstStep(std::size_t trackIndex) const
{
  if (trackIndex >= tracks_.size()) return 0;
  std::lock_guard<std::mutex> lock(mutex_);
  return ViewPageLocked(trackIndex) * kSequencerStepCount;
}

void SynthSequencer::SetStepSelected(std::size_t stepIndex, bool selected)
{
  if (stepIndex >= kSequencerMaxSteps)
  {
    return;
  }

  std::lock_guard<std::mutex> outputLock(outputMutex_);
  std::array<PendingMidiEvent, kMaxPendingMidiEvents> events{};
  std::size_t eventCount = 0;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    heldSteps_[stepIndex] = selected;
    if (std::none_of(heldSteps_.begin(), heldSteps_.end(), [](bool held) { return held; }))
      ClearSelectionLocked(events, eventCount);
    else
      ReleaseEmptyPagesLocked();
  }
  EmitPendingMidi(events, eventCount);
}

void SynthSequencer::ClearSelectedSteps()
{
  ClearHeldSteps();
}

bool SynthSequencer::ToggleNoteOnSelectedSteps(std::uint8_t note, std::uint8_t velocity)
{
  return ToggleNotesOnSelectedSteps(&note, 1, velocity);
}

bool SynthSequencer::ToggleNotesOnSelectedSteps(const std::uint8_t* notes, std::size_t count, std::uint8_t velocity)
{
  if (notes == nullptr) return false;
  std::array<bool, 128> given{};
  std::array<std::uint8_t, 128> velocities{};
  bool any = false;
  for (std::size_t index = 0; index < count; ++index)
  {
    if (notes[index] >= given.size()) continue;
    given[notes[index]] = true;
    velocities[notes[index]] = velocity;
    any = true;
  }
  if (!any) return false;

  std::lock_guard<std::mutex> lock(mutex_);
  if (activeTrack_ >= tracks_.size() || std::none_of(heldSteps_.begin(), heldSteps_.end(), [](bool held) { return held; }))
  {
    return false;
  }

  // Each selected step decides for itself: one that holds any of the notes loses them all.
  TrackData& track = tracks_[activeTrack_];
  StepStore& steps = ActivePatternLocked(track).steps;
  for (std::size_t stepIndex = 0; stepIndex < heldSteps_.size(); ++stepIndex)
    if (heldSteps_[stepIndex]) ToggleNotesInStepLocked(steps, stepIndex, given, velocities, track);
  ReleaseEmptyPagesLocked();
  return true;
}

bool SynthSequencer::SelectedStepsHoldAny(const std::uint8_t* notes, std::size_t count, std::array<bool, 128>* others) const
{
  if (others != nullptr) others->fill(false);
  if (notes == nullptr || count == 0) return false;
  std::array<bool, 128> group{};
  for (std::size_t index = 0; index < count; ++index)
    if (notes[index] < group.size()) group[notes[index]] = true;

  std::lock_guard<std::mutex> lock(mutex_);
  if (activeTrack_ >= tracks_.size()) return false;
  const PatternData& pattern = ActivePatternLocked(tracks_[activeTrack_]);
  bool any = false;
  for (std::size_t stepIndex = 0; stepIndex < heldSteps_.size(); ++stepIndex)
  {
    if (!heldSteps_[stepIndex]) continue;
    const auto& events = pattern.steps[stepIndex].notes;
    const auto inGroup = [&](const NoteEvent& event) { return event.active && event.note < group.size() && group[event.note]; };
    if (std::none_of(events.begin(), events.end(), inGroup)) continue;
    any = true;
    if (others == nullptr) break;
    for (const NoteEvent& event : events)
      if (event.active && event.note < group.size() && !group[event.note]) (*others)[event.note] = true;
  }
  return any;
}

bool SynthSequencer::MoveNotesOnSelectedSteps(const std::uint8_t* from, const std::uint8_t* to, std::size_t count)
{
  if (from == nullptr || to == nullptr || count == 0) return false;
  // target[note]: where a note of the group goes (-1: not in the group); landing: what the group becomes.
  std::array<int, 128> target{};
  target.fill(-1);
  std::array<bool, 128> landing{};
  for (std::size_t index = 0; index < count; ++index)
  {
    if (from[index] >= target.size() || to[index] >= target.size()) return false;
    target[from[index]] = to[index];
    landing[to[index]] = true;
  }
  const auto inGroup = [&](const NoteEvent& event) { return event.active && event.note < target.size() && target[event.note] >= 0; };

  std::lock_guard<std::mutex> lock(mutex_);
  if (activeTrack_ >= tracks_.size()) return false;
  StepStore& steps = ActivePatternLocked(tracks_[activeTrack_]).steps;
  // All or nothing: no step may end up with the same pitch twice.
  bool any = false;
  for (std::size_t stepIndex = 0; stepIndex < heldSteps_.size(); ++stepIndex)
  {
    if (!heldSteps_[stepIndex]) continue;
    const auto& events = std::as_const(steps)[stepIndex].notes;
    if (std::none_of(events.begin(), events.end(), inGroup)) continue;
    any = true;
    const bool blocked = std::any_of(events.begin(), events.end(), [&](const NoteEvent& event) {
      return event.active && event.note < target.size() && target[event.note] < 0 && landing[event.note];
    });
    if (blocked) return false;
  }
  if (!any) return false;

  bool moved = false;
  for (std::size_t stepIndex = 0; stepIndex < heldSteps_.size(); ++stepIndex)
  {
    if (!heldSteps_[stepIndex]) continue;
    const auto& events = std::as_const(steps)[stepIndex].notes;
    if (std::none_of(events.begin(), events.end(), inGroup)) continue;
    for (NoteEvent& event : steps.Edit(stepIndex).notes)
    {
      if (!inGroup(event) || target[event.note] == event.note) continue;
      event.note = static_cast<std::uint8_t>(target[event.note]);
      moved = true;
    }
  }
  ReleaseEmptyPagesLocked();
  return moved;
}

void SynthSequencer::SetEditLane(SequencerEditLane lane)
{
  if (lane > SequencerEditLane::Hits)
  {
    return;
  }

  std::lock_guard<std::mutex> lock(mutex_);
  editLane_ = lane;
  ReleaseEmptyPagesLocked();
}

void SynthSequencer::CycleEditLane()
{
  std::lock_guard<std::mutex> lock(mutex_);
  switch (editLane_)
  {
    case SequencerEditLane::Note: editLane_ = SequencerEditLane::Length; break;
    case SequencerEditLane::Length: editLane_ = SequencerEditLane::Velocity; break;
    case SequencerEditLane::Velocity: editLane_ = SequencerEditLane::Micro; break;
    case SequencerEditLane::Micro: editLane_ = SequencerEditLane::Hits; break;
    case SequencerEditLane::Hits:
    default: editLane_ = SequencerEditLane::Note; break;
  }
  ReleaseEmptyPagesLocked();
}

bool SynthSequencer::AdjustSelectedSteps(int delta, bool coarse)
{
  if (delta == 0)
  {
    return false;
  }

  std::lock_guard<std::mutex> lock(mutex_);
  if (activeTrack_ >= tracks_.size() || std::none_of(heldSteps_.begin(), heldSteps_.end(), [](bool held) { return held; }))
  {
    return false;
  }

  TrackData& track = tracks_[activeTrack_];
  bool selectionHasNotes = false;
  const PatternData& readPattern = ActivePatternLocked(std::as_const(track));
  for (std::size_t stepIndex = 0; stepIndex < heldSteps_.size(); ++stepIndex)
    if (heldSteps_[stepIndex] && ActiveNoteCount(readPattern.steps[stepIndex]) != 0) selectionHasNotes = true;

  if (!selectionHasNotes)
  {
    // Empty steps edit the values new notes start with ("NEW LEN" / "NEW VEL" / "NEW HITS").
    if (editLane_ == SequencerEditLane::Length)
    {
      const std::uint16_t next = std::clamp<std::uint16_t>(AdjustedLength(track.defaultLengthPulses, delta, coarse), 1,
                                                           kSequencerMaxSteps * kSequencerPulsesPerStep);
      const bool changed = next != track.defaultLengthPulses;
      track.defaultLengthPulses = next;
      return changed;
    }
    if (editLane_ == SequencerEditLane::Velocity)
    {
      const auto next = static_cast<std::uint8_t>(std::clamp(static_cast<int>(track.defaultVelocity) + delta * (coarse ? 10 : 1), 1, 127));
      const bool changed = next != track.defaultVelocity;
      track.defaultVelocity = next;
      return changed;
    }
    if (editLane_ == SequencerEditLane::Hits)
    {
      const std::uint8_t next = AdjustedHits(track.defaultHits, delta, coarse);
      const bool changed = next != track.defaultHits;
      track.defaultHits = next;
      return changed;
    }
    return false;
  }

  PatternData& pattern = ActivePatternLocked(track);
  bool changed = false;
  for (std::size_t stepIndex = 0; stepIndex < heldSteps_.size(); ++stepIndex)
  {
    if (!heldSteps_[stepIndex] || ActiveNoteCount(pattern.steps[stepIndex]) == 0) continue;
    StepData& step = pattern.steps.Edit(stepIndex);
    // Every note of the step (a held pad edits its own notes alone: AdjustNotesOnSelectedSteps).
    std::array<std::size_t, 8> slots{};
    std::size_t slotCount = 0;
    for (std::size_t slot = 0; slot < step.notes.size(); ++slot)
      if (step.notes[slot].active) slots[slotCount++] = slot;
    changed = AdjustLaneLocked(step, stepIndex, slots, slotCount, delta, coarse) || changed;
  }
  ReleaseEmptyPagesLocked();
  return changed;
}

bool SynthSequencer::AdjustNotesOnSelectedSteps(const std::uint8_t* notes, std::size_t count, int delta, bool coarse)
{
  if (notes == nullptr || count == 0 || delta == 0) return false;
  std::array<bool, 128> group{};
  for (std::size_t index = 0; index < count; ++index)
    if (notes[index] < group.size()) group[notes[index]] = true;

  std::lock_guard<std::mutex> lock(mutex_);
  if (activeTrack_ >= tracks_.size()) return false;
  StepStore& steps = ActivePatternLocked(tracks_[activeTrack_]).steps;
  bool changed = false;
  for (std::size_t stepIndex = 0; stepIndex < heldSteps_.size(); ++stepIndex)
  {
    if (!heldSteps_[stepIndex]) continue;
    const auto& events = std::as_const(steps)[stepIndex].notes;
    const auto inGroup = [&](const NoteEvent& event) { return event.active && event.note < group.size() && group[event.note]; };
    if (std::none_of(events.begin(), events.end(), inGroup)) continue;
    StepData& step = steps.Edit(stepIndex);
    std::array<std::size_t, 8> slots{};
    std::size_t slotCount = 0;
    for (std::size_t slot = 0; slot < step.notes.size(); ++slot)
      if (inGroup(step.notes[slot])) slots[slotCount++] = slot;
    changed = AdjustLaneLocked(step, stepIndex, slots, slotCount, delta, coarse) || changed;
  }
  ReleaseEmptyPagesLocked();
  return changed;
}

std::string SynthSequencer::NotesLaneValue(const std::uint8_t* notes, std::size_t count) const
{
  std::lock_guard<std::mutex> lock(mutex_);
  if (notes == nullptr || activeTrack_ >= tracks_.size()) return {};
  const PatternData& pattern = ActivePatternLocked(tracks_[activeTrack_]);
  for (std::size_t stepIndex = 0; stepIndex < heldSteps_.size(); ++stepIndex)
  {
    if (!heldSteps_[stepIndex]) continue;
    for (const NoteEvent& event : pattern.steps[stepIndex].notes)
      if (event.active && std::find(notes, notes + count, event.note) != notes + count) return LaneValueLocked(event);
  }
  return {};
}

SequencerEditLane SynthSequencer::EditLane() const
{
  std::lock_guard<std::mutex> lock(mutex_);
  return editLane_;
}

std::uint16_t SynthSequencer::AdjustedLength(std::uint16_t current, int delta, bool coarse)
{
  // Steps through the length table relative to the current value; values between entries move to
  // the neighbouring entry first.
  const int entries = delta * (coarse ? 3 : 1);
  const int last = static_cast<int>(kAdjustLengthTable.size()) - 1;
  int index = -1;
  if (entries > 0)
  {
    for (int candidate = 0; candidate <= last; ++candidate)
      if (kAdjustLengthTable[static_cast<std::size_t>(candidate)] > current) { index = candidate; break; }
    if (index < 0) return current;
    index = std::min(last, index + entries - 1);
  }
  else
  {
    for (int candidate = last; candidate >= 0; --candidate)
      if (kAdjustLengthTable[static_cast<std::size_t>(candidate)] < current) { index = candidate; break; }
    if (index < 0) return current;
    index = std::max(0, index + entries + 1);
  }
  return kAdjustLengthTable[static_cast<std::size_t>(index)];
}

bool SynthSequencer::AdjustLaneLocked(StepData& step, std::size_t stepIndex, std::array<std::size_t, 8> slots, std::size_t slotCount,
                                      int delta, bool coarse)
{
  slotCount = std::min(slotCount, slots.size());
  // Pitch order, so a transposed chord never collides with itself. (An explicit insertion sort: GCC's
  // std::sort expansion trips -Warray-bounds on this fixed 8-item buffer on Xtensa.)
  for (std::size_t index = 1; index < slotCount; ++index)
  {
    const std::size_t slot = slots[index];
    std::size_t insertionIndex = index;
    while (insertionIndex > 0 && step.notes[slot].note < step.notes[slots[insertionIndex - 1]].note)
    {
      slots[insertionIndex] = slots[insertionIndex - 1];
      --insertionIndex;
    }
    slots[insertionIndex] = slot;
  }

  bool changed = false;
  switch (editLane_)
  {
    case SequencerEditLane::Note:
    {
      const int amount = delta * (coarse ? 12 : 1);
      for (std::size_t order = 0; order < slotCount; ++order)
      {
        // Raise from the top and lower from the bottom.
        NoteEvent& event = step.notes[slots[amount > 0 ? slotCount - 1 - order : order]];
        const auto target = static_cast<std::uint8_t>(std::clamp(static_cast<int>(event.note) + amount, 0, 127));
        if (target == event.note) continue;
        const bool collides = std::any_of(step.notes.begin(), step.notes.end(),
                                          [&](const NoteEvent& other) { return other.active && other.note == target; });
        if (collides) continue;
        event.note = target;
        changed = true;
      }
      break;
    }
    case SequencerEditLane::Length:
      for (std::size_t order = 0; order < slotCount; ++order)
      {
        NoteEvent& event = step.notes[slots[order]];
        const std::uint16_t target = ClampLengthForStepMicrostep(stepIndex, event.microstep, AdjustedLength(event.lengthPulses, delta, coarse));
        if ((delta > 0 && target <= event.lengthPulses) || (delta < 0 && target >= event.lengthPulses)) continue;
        event.lengthPulses = target;
        changed = true;
      }
      break;
    case SequencerEditLane::Velocity:
      for (std::size_t order = 0; order < slotCount; ++order)
      {
        NoteEvent& event = step.notes[slots[order]];
        const auto target = static_cast<std::uint8_t>(std::clamp(static_cast<int>(event.velocity) + delta * (coarse ? 10 : 1), 1, 127));
        if (target == event.velocity) continue;
        event.velocity = target;
        changed = true;
      }
      break;
    case SequencerEditLane::Micro:
      for (std::size_t order = 0; order < slotCount; ++order)
      {
        NoteEvent& event = step.notes[slots[order]];
        // Coarse moves to the nearest edge of the six microsteps.
        const auto target = static_cast<std::uint8_t>(std::clamp(static_cast<int>(event.microstep) + delta * (coarse ? kSequencerMicrostepCount : 1),
                                                                 0, kSequencerMicrostepCount - 1));
        if (target == event.microstep) continue;
        event.microstep = target;
        event.lengthPulses = ClampLengthForStepMicrostep(stepIndex, event.microstep, event.lengthPulses);
        changed = true;
      }
      break;
    case SequencerEditLane::Hits:
      for (std::size_t order = 0; order < slotCount; ++order)
      {
        NoteEvent& event = step.notes[slots[order]];
        const std::uint8_t target = AdjustedHits(event.Hits(), delta, coarse);
        if (target == event.Hits()) continue;
        event.SetHits(target);
        changed = true;
      }
      break;
    default:
      break;
  }
  return changed;
}

std::uint8_t SynthSequencer::AdjustedHits(std::uint8_t current, int delta, bool coarse)
{
  // 1..4, no wrap, like the other lanes; coarse goes to the end.
  return ClampSequencerHits(static_cast<int>(current) + delta * (coarse ? kSequencerMaxHits : 1));
}

void SynthSequencer::SetSongMode(bool enabled)
{
  // Playback picks the change up at the next bar line (HandleSongBarLocked).
  std::lock_guard<std::mutex> lock(mutex_);
  songMode_ = enabled;
}

bool SynthSequencer::SongMode() const
{
  std::lock_guard<std::mutex> lock(mutex_);
  return songMode_;
}

void SynthSequencer::SetSongLoop(bool loop)
{
  std::lock_guard<std::mutex> lock(mutex_);
  songLoop_ = loop;
}

bool SynthSequencer::SongLoop() const
{
  std::lock_guard<std::mutex> lock(mutex_);
  return songLoop_;
}

std::size_t SynthSequencer::SongLength() const
{
  std::lock_guard<std::mutex> lock(mutex_);
  return songLength_;
}

SongRow SynthSequencer::SongRowAt(std::size_t row) const
{
  std::lock_guard<std::mutex> lock(mutex_);
  return row < songLength_ ? song_[row] : SongRow{};
}

bool SynthSequencer::SetSongPattern(std::size_t row, std::size_t trackIndex, int pattern)
{
  if (trackIndex >= kSynthTrackCount || pattern < -1)
  {
    return false;
  }

  std::lock_guard<std::mutex> lock(mutex_);
  if (row >= songLength_ || pattern >= static_cast<int>(ClampedPatternCount(tracks_[trackIndex])))
  {
    return false;
  }
  song_[row].patterns[trackIndex] = static_cast<std::int8_t>(pattern);
  return true;
}

bool SynthSequencer::SetSongRepeats(std::size_t row, std::uint8_t repeats)
{
  if (repeats < 1 || repeats > kSongMaxRepeats)
  {
    return false;
  }

  std::lock_guard<std::mutex> lock(mutex_);
  if (row >= songLength_)
  {
    return false;
  }
  song_[row].repeats = repeats;
  return true;
}

bool SynthSequencer::InsertSongRow(std::size_t at, const SongRow& row)
{
  std::lock_guard<std::mutex> lock(mutex_);
  if (at > songLength_ || songLength_ >= kSongMaxRows || !ValidSongRow(row, tracks_))
  {
    return false;
  }

  for (std::size_t index = songLength_; index > at; --index) song_[index] = song_[index - 1];
  song_[at] = row;
  ++songLength_;
  // The playing row and a pending jump keep pointing at the same rows.
  if (songEngaged_ && at <= songRow_) ++songRow_;
  if (songJumpPending_ && at <= songJumpRow_) ++songJumpRow_;
  return true;
}

bool SynthSequencer::DeleteSongRow(std::size_t row)
{
  std::lock_guard<std::mutex> lock(mutex_);
  if (row >= songLength_)
  {
    return false;
  }

  for (std::size_t index = row; index + 1 < songLength_; ++index) song_[index] = song_[index + 1];
  song_[songLength_ - 1] = SongRow{};
  --songLength_;
  // A jump to the deleted row lands on the row that took its place.
  if (songJumpPending_ && row < songJumpRow_) --songJumpRow_;
  if (songEngaged_)
  {
    if (row < songRow_)
    {
      --songRow_;
    }
    else if (row == songRow_ && !songJumpPending_)
    {
      // The playing row ends at the next bar line; past the end, the song loops or stops there.
      songJumpPending_ = true;
      songJumpRow_ = row;
    }
  }
  return true;
}

void SynthSequencer::CueSongRow(std::size_t row)
{
  std::lock_guard<std::mutex> lock(mutex_);
  if (row >= songLength_)
  {
    return;
  }
  if (playing_ && songMode_)
  {
    // Lands on the next bar line, also when that bar line is where the song takes over.
    songJumpPending_ = true;
    songJumpRow_ = row;
    return;
  }
  songStartRow_ = row;
}

void SynthSequencer::SetTempo(std::uint16_t bpm)
{
  std::lock_guard<std::mutex> lock(mutex_);
  bpm_ = std::clamp(bpm, kMinTempo, kMaxTempo);
}

void SynthSequencer::SetTrackEnabled(std::size_t trackIndex, bool enabled)
{
  if (trackIndex >= kSynthTrackCount)
  {
    return;
  }

  std::lock_guard<std::mutex> outputLock(outputMutex_);
  std::array<PendingMidiEvent, kMaxPendingMidiEvents> events{};
  std::size_t eventCount = 0;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    tracks_[trackIndex].enabled = enabled;
    if (!enabled) ReleaseTrackNotesLocked(trackIndex, events, eventCount);
  }
  EmitPendingMidi(events, eventCount);
}

bool SynthSequencer::TrackEnabled(std::size_t trackIndex) const
{
  std::lock_guard<std::mutex> lock(mutex_);
  return trackIndex < tracks_.size() && tracks_[trackIndex].enabled;
}

bool SynthSequencer::ClearPatternEvents(std::size_t trackIndex, std::size_t patternIndex)
{
  if (trackIndex >= kSynthTrackCount || patternIndex >= kSequencerMaxPatternCount)
  {
    return false;
  }

  std::lock_guard<std::mutex> lock(mutex_);
  TrackData& track = tracks_[trackIndex];
  if (patternIndex >= std::clamp<std::size_t>(track.patternCount, 1, kSequencerMaxPatternCount))
  {
    return false;
  }

  // Releasing reads back as an empty pattern and keeps the slot and pattern count.
  track.patterns.Release(patternIndex);
  if (trackIndex == activeTrack_) ReleaseEmptyPagesLocked();
  return true;
}

std::size_t SynthSequencer::PatternCount(std::size_t trackIndex) const
{
  std::lock_guard<std::mutex> lock(mutex_);
  return trackIndex < tracks_.size() ? std::clamp<std::size_t>(tracks_[trackIndex].patternCount, 1, kSequencerMaxPatternCount) : 0;
}

std::size_t SynthSequencer::ActivePattern(std::size_t trackIndex) const
{
  std::lock_guard<std::mutex> lock(mutex_);
  if (trackIndex >= tracks_.size()) return 0;
  const TrackData& track = tracks_[trackIndex];
  return std::min(track.activePattern, std::clamp<std::size_t>(track.patternCount, 1, kSequencerMaxPatternCount) - 1);
}

void SynthSequencer::EnsureClockWorkerStarted()
{
  bool expected = false;
  if (!clockWorkerRunning_.compare_exchange_strong(expected, true))
  {
    return;
  }

  clockWorkerExited_.store(false);

#if defined(ESP_PLATFORM)
  TaskHandle_t taskHandle = nullptr;
  if (xTaskCreatePinnedToCore(&SynthSequencer::ClockTaskEntry,
                              "synth_seq",
                              kClockTaskStackWords,
                              this,
                              kClockTaskPriority,
                              &taskHandle,
                              kClockTaskCore) != pdPASS)
  {
    clockWorkerRunning_.store(false);
    clockWorkerExited_.store(true);
    return;
  }
#elif defined(__EMSCRIPTEN__)
  // No thread: the host drives the clock through PumpClock().
#else
  clockThread_ = std::thread([this]() { ClockLoop(); });
#endif
}

#if defined(__EMSCRIPTEN__)
void SynthSequencer::PumpClock(std::uint64_t nowMicros)
{
  // ClockLoop for a host without threads: wherever it would sleep, return to the host instead.
  // The arp's press time is on the wall clock, not the host's audio clock, so an arp that starts
  // from idle simply counts from this call.
  std::uint64_t startMicros = 0;
  if (!clockWorkerRunning_.load() || !ClockNeeded(startMicros))
  {
    pumpIdle_ = true;
    pumpNextPulseMicros_ = nowMicros;
    return;
  }
  if (pumpIdle_)
  {
    pumpIdle_ = false;
    pumpNextPulseMicros_ = nowMicros;
  }
  while (nowMicros >= pumpNextPulseMicros_ && ClockNeeded(startMicros))
  {
    ProcessPulse();
    {
      std::lock_guard<std::mutex> lock(mutex_);
      pumpNextPulseMicros_ += PulsePeriodMicrosLocked();
    }
    if (nowMicros > pumpNextPulseMicros_ + kClockMaxLagMicros) pumpNextPulseMicros_ = nowMicros;
  }
}
#endif

void SynthSequencer::StopClockWorker()
{
  clockWorkerRunning_.store(false);
#if defined(ESP_PLATFORM)
  while (!clockWorkerExited_.load()) vTaskDelay(1);
#else
  if (clockThread_.joinable())
  {
    clockThread_.join();
  }
#endif
}

#if defined(ESP_PLATFORM)
void SynthSequencer::ClockTaskEntry(void* context)
{
  static_cast<SynthSequencer*>(context)->ClockLoop();
  vTaskDelete(nullptr);
}
#endif

void SynthSequencer::ClockLoop()
{
#if defined(ESP_PLATFORM)
  esp_timer_create_args_t timerArgs{};
  timerArgs.callback = [](void* task) { xTaskNotifyGive(static_cast<TaskHandle_t>(task)); };
  timerArgs.arg = xTaskGetCurrentTaskHandle();
  timerArgs.name = "seq_pulse";
  esp_timer_handle_t timer = nullptr;
  if (esp_timer_create(&timerArgs, &timer) != ESP_OK) {
    clockWorkerRunning_.store(false);
    { std::lock_guard<std::mutex> lock(mutex_); playing_ = false; }
    clockWorkerExited_.store(true);
    return;
  }
  clockTimer_ = timer;
#endif
  std::uint64_t nextPulseMicros = NowMicros();
  bool idle = true;
  while (clockWorkerRunning_.load())
  {
    std::uint64_t startMicros = 0;
    if (!ClockNeeded(startMicros))
    {
      idle = true;
#if defined(ESP_PLATFORM)
      vTaskDelay(std::max<TickType_t>(1, pdMS_TO_TICKS(kClockIdleSleepMs)));
#else
      std::this_thread::sleep_for(std::chrono::milliseconds(kClockIdleSleepMs));
#endif
      nextPulseMicros = NowMicros();
      continue;
    }
    if (idle)
    {
      idle = false;
      // An arp started from idle counts from its key press, not from this (up to a tick late) wake-up.
      const std::uint64_t wake = NowMicros();
      if (startMicros != 0 && startMicros <= wake && wake - startMicros < kClockMaxLagMicros) nextPulseMicros = startMicros;
    }

    const std::uint64_t now = NowMicros();
    if (now < nextPulseMicros)
    {
      SleepUntilMicros(nextPulseMicros);
      continue;
    }

    const auto late = static_cast<std::uint32_t>((std::min)(now - nextPulseMicros, std::uint64_t{0xffffffffu}));
    clockPulses_.fetch_add(1, std::memory_order_relaxed);
    if (late > clockLatestMicros_.load(std::memory_order_relaxed)) clockLatestMicros_.store(late, std::memory_order_relaxed);
    if (late > kLatePulseMicros) clockLatePulses_.fetch_add(1, std::memory_order_relaxed);
    ProcessPulse();
    {
      std::lock_guard<std::mutex> lock(mutex_);
      nextPulseMicros += PulsePeriodMicrosLocked();
    }

    const std::uint64_t after = NowMicros();
    if (after > nextPulseMicros + kClockMaxLagMicros)
    {
      nextPulseMicros = after;
    }
  }
#if defined(ESP_PLATFORM)
  esp_timer_stop(timer);
  esp_timer_delete(timer);
  clockTimer_ = nullptr;
#endif
  clockWorkerExited_.store(true);
}

void SynthSequencer::AddSoundingNotes(std::size_t trackIndex, std::array<std::uint32_t, 4>& notes) const
{
  if (trackIndex >= kSynthTrackCount) return;
  const auto add = [&](std::uint8_t note) { notes[(note & 0x7F) / 32] |= 1u << ((note & 0x7F) % 32); };
  std::lock_guard<std::mutex> lock(mutex_);
  for (const PlaybackNote& note : playbackNotes_)
    if (note.active && note.trackIndex == trackIndex) add(note.note);
  if (auditionActive_ && auditionTrack_ == trackIndex) add(auditionNote_);
  if (arps_[trackIndex].soundingNote >= 0) add(static_cast<std::uint8_t>(arps_[trackIndex].soundingNote));
}

bool SynthSequencer::ClockNeeded(std::uint64_t& startMicros)
{
  std::lock_guard<std::mutex> lock(mutex_);
  startMicros = clockStartMicros_;
  clockStartMicros_ = 0;
  return playing_ || AnyArpClockLocked();
}

void SynthSequencer::ProcessPulse()
{
  std::lock_guard<std::mutex> outputLock(outputMutex_);
  std::array<PendingMidiEvent, kMaxPendingMidiEvents> events{};
  std::size_t eventCount = 0;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    const bool arpClock = AnyArpClockLocked();
    if (!playing_ && !arpClock)
    {
      return;
    }

    const std::uint64_t pulse = absolutePulse_;
    if (playing_) ProcessTransportPulseLocked(pulse, events, eventCount);
    if (arpClock)
      for (ArpVoice& arp : arps_)
        if (ArpClockLocked(arp)) ProcessArpPulseLocked(arp, pulse, events, eventCount);
  }
  EmitPendingMidi(events, eventCount);
}

void SynthSequencer::ProcessTransportPulseLocked(std::uint64_t pulse,
                                                 std::array<PendingMidiEvent, kMaxPendingMidiEvents>& events,
                                                 std::size_t& eventCount)
{
  playbackStep_ = static_cast<std::size_t>((pulse / kSequencerPulsesPerStep) % kSequencerMaxSteps);

  CollectDueNoteOffsLocked(pulse, events, eventCount);
  for (SuppressedNote& suppressed : suppressedNotes_)
    if (suppressed.active && suppressed.pulse < pulse) suppressed = SuppressedNote{};

  // Song rows change on bar lines of the global clock; the end of a song can stop playback here.
  const bool running = pulse % kSongBarPulses != 0 || HandleSongBarLocked(pulse, events, eventCount);
  for (std::size_t trackIndex = 0; running && trackIndex < tracks_.size(); ++trackIndex)
  {
    const TrackData& track = tracks_[trackIndex];
    const std::size_t stepCount = TrackStepCountOf(track);
    const std::uint16_t loopPulses = static_cast<std::uint16_t>(stepCount * kSequencerPulsesPerStep);
    std::uint16_t position = trackNextPulse_[trackIndex];
    if (position >= loopPulses) position = 0;
    // Muted and disabled tracks keep their position so they re-enter in time.
    if (position == 0 && trackStarted_[trackIndex]) HandleLoopBoundaryLocked(trackIndex, pulse);
    trackStarted_[trackIndex] = true;
    trackCurrentPulse_[trackIndex] = position;
    trackNextPulse_[trackIndex] = static_cast<std::uint16_t>((position + 1u) % loopPulses);

    if (!track.enabled || track.muted)
    {
      continue;
    }

    const PatternData& pattern = ActivePatternLocked(track);
    const std::size_t stepIndex = position / kSequencerPulsesPerStep;
    // A track the playing song row leaves out keeps its position but plays nothing of its own.
    if (!songSilent_[trackIndex])
    {
      // Microstep, swing and the later hits of HITS carry a step's notes into the following steps...
      for (std::size_t back = 0; back <= kHitReachSteps && back <= stepIndex; ++back)
        FireStepNotesLocked(trackIndex, pattern.steps[stepIndex - back], stepIndex - back, position, pulse, events, eventCount);
    }
    // ...and the last steps of the previous pass past the loop wrap (or past a song row line).
    const std::size_t previousPattern = previousPassPattern_[trackIndex];
    const std::uint16_t previousPulses = previousPassPulses_[trackIndex];
    if (previousPattern < kSequencerMaxPatternCount && previousPulses >= kSequencerPulsesPerStep && position <= MaxHitReachPulses())
    {
      const std::size_t lastStep = std::min<std::size_t>(previousPulses / kSequencerPulsesPerStep, kSequencerMaxSteps) - 1;
      for (std::size_t back = 0; back <= kHitReachSteps && back <= lastStep; ++back)
        FireStepNotesLocked(trackIndex, track.patterns[previousPattern].steps[lastStep - back], lastStep - back,
                            static_cast<std::uint32_t>(position) + previousPulses, pulse, events, eventCount);
    }
  }

  if (running) ++absolutePulse_;
}

std::uint64_t SynthSequencer::NowMicros() const
{
#if defined(ESP_PLATFORM)
  return static_cast<std::uint64_t>(esp_timer_get_time());
#else
  const auto now = std::chrono::steady_clock::now().time_since_epoch();
  return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(now).count());
#endif
}

void SynthSequencer::SleepUntilMicros(std::uint64_t targetMicros) const
{
#if defined(ESP_PLATFORM)
  const std::uint64_t now = NowMicros();
  if (now >= targetMicros || !clockWorkerRunning_.load()) return;
  ulTaskNotifyTake(pdTRUE, 0);
  if (esp_timer_start_once(static_cast<esp_timer_handle_t>(clockTimer_), targetMicros - now) == ESP_OK)
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
#else
  std::this_thread::sleep_until(std::chrono::steady_clock::time_point(std::chrono::microseconds(targetMicros)));
#endif
}

SynthSequencer::ClockLateness SynthSequencer::TakeClockLateness()
{
  ClockLateness lateness;
  lateness.pulses = clockPulses_.exchange(0, std::memory_order_relaxed);
  lateness.latestMicros = clockLatestMicros_.exchange(0, std::memory_order_relaxed);
  lateness.latePulses = clockLatePulses_.exchange(0, std::memory_order_relaxed);
  return lateness;
}

std::uint64_t SynthSequencer::PulsePeriodMicrosLocked() const
{
  return 60000000ULL / (static_cast<std::uint64_t>(std::max<std::uint16_t>(1, bpm_)) * kSequencerPulsesPerQuarter);
}

void SynthSequencer::AddPendingMidi(std::array<PendingMidiEvent, kMaxPendingMidiEvents>& events,
                                    std::size_t& count,
                                    std::size_t trackIndex,
                                    const midi::MidiPacket& packet) const
{
  if (count >= events.size())
  {
    return;
  }
  events[count++] = PendingMidiEvent{trackIndex, packet};
}

void SynthSequencer::EmitPendingMidi(const std::array<PendingMidiEvent, kMaxPendingMidiEvents>& events, std::size_t count) const
{
  MidiSink sink = nullptr;
  void* context = nullptr;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    sink = midiSink_;
    context = midiSinkContext_;
  }
  if (sink == nullptr)
  {
    return;
  }

  for (std::size_t index = 0; index < count; ++index)
  {
    sink(context, events[index].trackIndex, events[index].packet);
  }
}

bool SynthSequencer::ScheduleNoteOffLocked(std::size_t trackIndex, std::uint8_t note, std::uint64_t offPulse)
{
  for (PlaybackNote& activeNote : playbackNotes_)
  {
    if (activeNote.active && activeNote.trackIndex == trackIndex && activeNote.note == note)
    {
      activeNote.offPulse = offPulse;
      return true;
    }
  }

  for (PlaybackNote& activeNote : playbackNotes_)
  {
    if (!activeNote.active)
    {
      activeNote.active = true;
      activeNote.trackIndex = static_cast<std::uint8_t>(trackIndex);
      activeNote.note = note;
      activeNote.offPulse = offPulse;
      return true;
    }
  }
  // A full release schedule must reject the note-on rather than create an untracked voice.
  return false;
}

void SynthSequencer::CollectDueNoteOffsLocked(std::uint64_t pulse,
                                              std::array<PendingMidiEvent, kMaxPendingMidiEvents>& events,
                                              std::size_t& count)
{
  for (PlaybackNote& activeNote : playbackNotes_)
  {
    if (!activeNote.active || activeNote.offPulse > pulse)
    {
      continue;
    }
    AddPendingMidi(events, count, activeNote.trackIndex, midi::MidiPacket::NoteOff(0, activeNote.note, 0));
    activeNote = PlaybackNote{};
  }
}

void SynthSequencer::CollectAllNoteOffsLocked(std::array<PendingMidiEvent, kMaxPendingMidiEvents>& events, std::size_t& count)
{
  StopAuditionLocked(events, count);
  for (PlaybackNote& activeNote : playbackNotes_)
  {
    if (!activeNote.active)
    {
      continue;
    }
    AddPendingMidi(events, count, activeNote.trackIndex, midi::MidiPacket::NoteOff(0, activeNote.note, 0));
    activeNote = PlaybackNote{};
  }

  for (std::size_t trackIndex = 0; trackIndex < tracks_.size(); ++trackIndex)
  {
    AddPendingMidi(events, count, trackIndex, midi::MidiPacket::ControlChange(0, 120, 0));
  }
}

void SynthSequencer::StopAuditionLocked(std::array<PendingMidiEvent, kMaxPendingMidiEvents>& events, std::size_t& count)
{
  if (!auditionActive_)
  {
    return;
  }

  AddPendingMidi(events, count, auditionTrack_, midi::MidiPacket::NoteOff(0, auditionNote_, 0));
  auditionActive_ = false;
}

void SynthSequencer::ToggleNoteInStepLocked(StepData& step, std::uint8_t note, TrackData& track, std::size_t stepIndex, std::uint8_t velocity)
{
  if (note > 127) return;
  for (NoteEvent& event : step.notes)
  {
    if (event.active && event.note == note)
    {
      event = NoteEvent{};
      return;
    }
  }

  for (NoteEvent& event : step.notes)
  {
    if (!event.active)
    {
      event.active = true;
      event.note = note;
      event.velocity = velocity != 0 ? std::min<std::uint8_t>(velocity, 127) : track.defaultVelocity;
      event.microstep = 0;
      event.lengthPulses = ClampLengthForStepMicrostep(stepIndex, event.microstep, track.defaultLengthPulses);
      event.SetHits(track.defaultHits);
      return;
    }
  }
}

void SynthSequencer::ApplyLengthToHeldStepsLocked(std::uint16_t lengthPulses)
{
  if (activeTrack_ >= tracks_.size())
  {
    return;
  }

  TrackData& track = tracks_[activeTrack_];
  PatternData& pattern = ActivePatternLocked(track);
  track.defaultLengthPulses = lengthPulses;
  for (std::size_t stepIndex = 0; stepIndex < heldSteps_.size(); ++stepIndex)
  {
    if (!heldSteps_[stepIndex] || ActiveNoteCount(pattern.steps[stepIndex]) == 0)
    {
      continue;
    }

    for (NoteEvent& note : pattern.steps.Edit(stepIndex).notes)
    {
      if (note.active)
      {
        note.lengthPulses = ClampLengthForStepMicrostep(stepIndex, note.microstep, lengthPulses);
      }
    }
  }
}

void SynthSequencer::ApplyVelocityToHeldStepsLocked(std::uint8_t velocity)
{
  if (activeTrack_ >= tracks_.size())
  {
    return;
  }

  TrackData& track = tracks_[activeTrack_];
  PatternData& pattern = ActivePatternLocked(track);
  track.defaultVelocity = velocity;
  for (std::size_t stepIndex = 0; stepIndex < heldSteps_.size(); ++stepIndex)
  {
    if (!heldSteps_[stepIndex] || ActiveNoteCount(pattern.steps[stepIndex]) == 0)
    {
      continue;
    }

    for (NoteEvent& note : pattern.steps.Edit(stepIndex).notes)
    {
      if (note.active)
      {
        note.velocity = velocity;
      }
    }
  }
}

void SynthSequencer::ApplyMicrostepToHeldStepsLocked(std::uint8_t microstep)
{
  if (activeTrack_ >= tracks_.size())
  {
    return;
  }

  microstep = std::min<std::uint8_t>(microstep, kSequencerMicrostepCount - 1);
  TrackData& track = tracks_[activeTrack_];
  PatternData& pattern = ActivePatternLocked(track);
  for (std::size_t stepIndex = 0; stepIndex < heldSteps_.size(); ++stepIndex)
  {
    if (!heldSteps_[stepIndex] || ActiveNoteCount(pattern.steps[stepIndex]) == 0)
    {
      continue;
    }

    for (NoteEvent& note : pattern.steps.Edit(stepIndex).notes)
    {
      if (note.active)
      {
        note.microstep = microstep;
        note.lengthPulses = ClampLengthForStepMicrostep(stepIndex, note.microstep, note.lengthPulses);
      }
    }
  }
}

void SynthSequencer::ClearStepLocked(StepData& step)
{
  step = StepData{};
}

void SynthSequencer::QuantizeStepLocked(StepData& step, std::size_t stepIndex)
{
  for (NoteEvent& note : step.notes)
  {
    if (!note.active)
    {
      continue;
    }

    note.microstep = 0;
    note.lengthPulses = ClampLengthForStepMicrostep(stepIndex, note.microstep, note.lengthPulses);
  }
}

// Gives back the memory of the active pattern's pages that edits emptied.
void SynthSequencer::ReleaseEmptyPagesLocked()
{
  if (activeTrack_ < tracks_.size()) ActivePatternLocked(tracks_[activeTrack_]).steps.ReleaseEmptyPages();
}

SynthSequencer::PatternData& SynthSequencer::ActivePatternLocked(TrackData& track)
{
  track.patternCount = std::clamp<std::size_t>(track.patternCount, 1, kSequencerMaxPatternCount);
  if (track.activePattern >= track.patternCount)
  {
    track.activePattern = track.patternCount - 1;
  }
  return track.patterns[track.activePattern];
}

const SynthSequencer::PatternData& SynthSequencer::ActivePatternLocked(const TrackData& track) const
{
  const std::size_t patternCount = std::clamp<std::size_t>(track.patternCount, 1, kSequencerMaxPatternCount);
  const std::size_t activePattern = std::min(track.activePattern, patternCount - 1);
  return track.patterns[activePattern];
}

const SynthSequencer::StepData* SynthSequencer::PrimaryHeldStepLocked() const
{
  if (activeTrack_ >= tracks_.size())
  {
    return nullptr;
  }

  const PatternData& pattern = ActivePatternLocked(tracks_[activeTrack_]);
  for (std::size_t stepIndex = 0; stepIndex < heldSteps_.size(); ++stepIndex)
  {
    if (heldSteps_[stepIndex])
    {
      return &pattern.steps[stepIndex];
    }
  }
  return nullptr;
}

std::size_t SynthSequencer::ActiveNoteCount(const StepData& step) const
{
  std::size_t count = 0;
  for (const NoteEvent& note : step.notes)
  {
    if (note.active)
    {
      ++count;
    }
  }
  return count;
}

void SynthSequencer::PopulateEditValueStateLocked(SequencerUiState& state, const StepData* step) const
{
  const NoteEvent* note = nullptr;
  if (step != nullptr)
  {
    for (const NoteEvent& event : step->notes)
    {
      if (event.active)
      {
        note = &event;
        break;
      }
    }
  }
  NoteEvent defaults;
  if (note == nullptr && step != nullptr && activeTrack_ < tracks_.size() && editLane_ != SequencerEditLane::Micro)
  {
    defaults.lengthPulses = tracks_[activeTrack_].defaultLengthPulses;
    defaults.velocity = tracks_[activeTrack_].defaultVelocity;
    defaults.SetHits(tracks_[activeTrack_].defaultHits);
    note = &defaults;
  }

  switch (editLane_)
  {
    case SequencerEditLane::Length:
      state.editValueCount = kLengthBuckets.size();
      state.editValueIndex = 0;
      if (note != nullptr)
      {
        for (std::size_t index = 0; index < kLengthBuckets.size(); ++index)
        {
          if (note->lengthPulses <= kLengthBuckets[index])
          {
            state.editValueIndex = index;
            break;
          }
          state.editValueIndex = index;
        }
      }
      break;
    case SequencerEditLane::Velocity:
      state.editValueCount = kVelocityBuckets.size();
      state.editValueIndex = 0;
      if (note != nullptr)
      {
        std::uint8_t bestDistance = 0xFF;
        for (std::size_t index = 0; index < kVelocityBuckets.size(); ++index)
        {
          const std::uint8_t distance = static_cast<std::uint8_t>(std::abs(static_cast<int>(note->velocity) - static_cast<int>(kVelocityBuckets[index])));
          if (distance < bestDistance)
          {
            bestDistance = distance;
            state.editValueIndex = index;
          }
        }
      }
      break;
    case SequencerEditLane::Micro:
      state.editValueCount = note != nullptr ? kSequencerMicrostepCount : 0;
      state.editValueIndex = note != nullptr ? std::min<std::size_t>(note->microstep, kSequencerMicrostepCount - 1) : 0;
      break;
    case SequencerEditLane::Hits:
      state.editValueCount = kSequencerMaxHits;
      state.editValueIndex = note != nullptr ? static_cast<std::size_t>(note->Hits() - 1) : 0;
      break;
    case SequencerEditLane::Note:
    default:
      state.editValueCount = 0;
      state.editValueIndex = 0;
      break;
  }
}

std::uint16_t SynthSequencer::ClampLengthForStepMicrostep(std::size_t stepIndex, std::uint8_t microstep, std::uint16_t lengthPulses) const
{
  return ClampLengthInPattern(activeTrack_ < tracks_.size() ? TrackStepCountOf(tracks_[activeTrack_]) : kSequencerMaxSteps, stepIndex, microstep, lengthPulses);
}

// To the end of the pattern (a short one still has the 16 steps of its page).
std::uint16_t SynthSequencer::ClampLengthInPattern(std::size_t patternSteps, std::size_t stepIndex, std::uint8_t microstep, std::uint16_t lengthPulses)
{
  const std::uint16_t startPulse = static_cast<std::uint16_t>(stepIndex * kSequencerPulsesPerStep +
                                                             microstep * kSequencerPulsesPerMicrostep);
  const std::uint16_t patternPulses = static_cast<std::uint16_t>((std::max)(patternSteps, kSequencerStepCount) * kSequencerPulsesPerStep);
  const std::uint16_t remaining = startPulse < patternPulses ? static_cast<std::uint16_t>(patternPulses - startPulse) : 1;
  return std::max<std::uint16_t>(1, std::min(lengthPulses, remaining));
}

std::string SynthSequencer::ValueLabelLocked(const StepData* step) const
{
  if (step == nullptr || ActiveNoteCount(*step) == 0)
  {
    if (step != nullptr && activeTrack_ < tracks_.size())
    {
      if (editLane_ == SequencerEditLane::Length)
        return "NEW LEN " + std::to_string(tracks_[activeTrack_].defaultLengthPulses / kSequencerPulsesPerStep);
      if (editLane_ == SequencerEditLane::Velocity)
        return "NEW VEL " + std::to_string(tracks_[activeTrack_].defaultVelocity);
      if (editLane_ == SequencerEditLane::Micro) return "ADD NOTES FIRST";
      if (editLane_ == SequencerEditLane::Hits) return "NEW HITS " + std::to_string(tracks_[activeTrack_].defaultHits);
    }
    return SequencerEditLaneLabel(editLane_);
  }

  const NoteEvent* note = nullptr;
  for (const NoteEvent& event : step->notes)
  {
    if (event.active)
    {
      note = &event;
      break;
    }
  }
  if (note == nullptr)
  {
    return SequencerEditLaneLabel(editLane_);
  }

  if (editLane_ == SequencerEditLane::Note) return std::to_string(ActiveNoteCount(*step)) + " notes";
  return std::string(SequencerEditLaneLabel(editLane_)) + " " + LaneValueLocked(*note);
}

std::string SynthSequencer::LaneValueLocked(const NoteEvent& note) const
{
  switch (editLane_)
  {
    case SequencerEditLane::Note:
      return std::to_string(note.note);
    case SequencerEditLane::Length:
      return std::to_string((note.lengthPulses + kSequencerPulsesPerStep - 1) / kSequencerPulsesPerStep);
    case SequencerEditLane::Velocity:
      return std::to_string(note.velocity);
    case SequencerEditLane::Micro:
      return std::to_string(note.microstep + 1);
    case SequencerEditLane::Hits:
      return std::to_string(note.Hits());
    default:
      return {};
  }
}

std::size_t SynthSequencer::TrackStepCountOf(const TrackData& track)
{
  const std::size_t pattern = std::min(track.activePattern, ClampedPatternCount(track) - 1);
  return std::clamp<std::size_t>(track.patternSteps[pattern], 1, kSequencerMaxSteps);
}

std::uint16_t SynthSequencer::SwingDelayPulsesLocked() const
{
  // An odd sixteenth starts at swing% of an eighth note (48 pulses); 50% is straight.
  return SwingDelayPulses(swing_);
}

std::uint16_t SynthSequencer::NoteOffsetPulsesLocked(std::size_t stepIndex, std::uint8_t microstep) const
{
  return static_cast<std::uint16_t>(stepIndex * kSequencerPulsesPerStep + microstep * kSequencerPulsesPerMicrostep +
                                    (stepIndex % 2 == 1 ? SwingDelayPulsesLocked() : 0));
}

void SynthSequencer::FireStepNotesLocked(std::size_t trackIndex,
                                         const StepData& step,
                                         std::size_t stepIndex,
                                         std::uint32_t targetOffset,
                                         std::uint64_t pulse,
                                         std::array<PendingMidiEvent, kMaxPendingMidiEvents>& events,
                                         std::size_t& count)
{
  for (const NoteEvent& note : step.notes)
  {
    if (!note.active || note.microstep >= kSequencerMicrostepCount) continue;
    // The note's first hit is on its own offset (TIME and swing included); HITS adds more, a spacing
    // apart (whole pulses), each ended before the next one starts: a note-off due on this pulse was
    // sent before these note-ons.
    const std::uint32_t first = NoteOffsetPulsesLocked(stepIndex, note.microstep);
    if (targetOffset < first) continue;
    const std::uint8_t hits = note.Hits();
    const std::uint32_t spacing = SequencerHitSpacingPulses(hits);
    const std::uint32_t since = targetOffset - first;
    if (since % spacing != 0 || since / spacing >= hits) continue;
    const auto hit = static_cast<std::uint8_t>(since / spacing);
    if (hit == 0 && ConsumeSuppressedNoteLocked(trackIndex, note.note, pulse))
    {
      continue;
    }

    if (ScheduleNoteOffLocked(trackIndex, note.note, pulse + SequencerHitGatePulses(hits, hit, std::max<std::uint16_t>(1, note.lengthPulses))))
      AddPendingMidi(events, count, trackIndex, midi::MidiPacket::NoteOn(0, note.note, note.velocity));
  }
}

bool SynthSequencer::ConsumeSuppressedNoteLocked(std::size_t trackIndex, std::uint8_t note, std::uint64_t pulse)
{
  for (SuppressedNote& suppressed : suppressedNotes_)
  {
    if (suppressed.active && suppressed.pulse == pulse && suppressed.trackIndex == trackIndex && suppressed.note == note)
    {
      suppressed = SuppressedNote{};
      return true;
    }
  }
  return false;
}

void SynthSequencer::HandleLoopBoundaryLocked(std::size_t trackIndex, std::uint64_t pulse)
{
  TrackData& track = tracks_[trackIndex];
  // A live note held over the wrap ends with the pass it was recorded in.
  for (HeldRecording& recording : heldRecordings_)
    if (recording.active && recording.trackIndex == trackIndex && recording.startPulse < pulse) FinalizeHeldRecordingLocked(recording, pulse);

  const std::size_t patternCount = std::clamp<std::size_t>(track.patternCount, 1, kSequencerMaxPatternCount);
  // A pass the song left silent has nothing to spill.
  previousPassPattern_[trackIndex] = songSilent_[trackIndex] ? kNoPattern : std::min(track.activePattern, patternCount - 1);
  previousPassPulses_[trackIndex] = static_cast<std::uint16_t>(TrackStepCountOf(track) * kSequencerPulsesPerStep);
  const std::size_t queued = queuedPattern_[trackIndex];
  queuedPattern_[trackIndex] = kNoPattern;
  if (queued < patternCount && queued != track.activePattern)
  {
    track.activePattern = queued;
    if (trackIndex == activeTrack_) ReleaseEmptyPagesLocked();
  }
}

bool SynthSequencer::HandleSongBarLocked(std::uint64_t pulse,
                                         std::array<PendingMidiEvent, kMaxPendingMidiEvents>& events,
                                         std::size_t& count)
{
  const bool wanted = songMode_ && songLength_ > 0;
  if (!songEngaged_)
  {
    if (!wanted) return true;
    // The song takes over: at the cued jump if one is pending, else at the start row.
    songEngaged_ = true;
    const std::size_t row = songJumpPending_ ? songJumpRow_ : songStartRow_;
    songJumpPending_ = false;
    StartSongRowLocked(std::min(row, songLength_ - 1), pulse);
    return true;
  }
  if (!wanted)
  {
    // Back to pattern looping: tracks keep the row's patterns and their loop positions.
    LeaveSongLocked();
    return true;
  }

  ++songBar_;
  // The row length is read live, so lengthening or shortening the playing row is heard at once.
  const std::size_t repeats = songRow_ < songLength_ ? song_[songRow_].repeats : 1;
  if (!songJumpPending_ && songBar_ < repeats) return true;
  std::size_t next = songJumpPending_ ? songJumpRow_ : songRow_ + 1;
  songJumpPending_ = false;
  if (next >= songLength_)
  {
    if (!songLoop_)
    {
      // The song is over: stop on this bar line. The next Play() starts from the start row again.
      LeaveSongLocked();
      StopLocked(events, count);
      return false;
    }
    next = 0;
  }
  StartSongRowLocked(next, pulse);
  return true;
}

void SynthSequencer::StartSongRowLocked(std::size_t row, std::uint64_t pulse)
{
  songRow_ = row;
  songBar_ = 0;
  const SongRow& data = song_[row];
  for (std::size_t trackIndex = 0; trackIndex < tracks_.size(); ++trackIndex)
  {
    TrackData& track = tracks_[trackIndex];
    const std::size_t patternCount = ClampedPatternCount(track);
    const std::uint16_t loopPulses = static_cast<std::uint16_t>(TrackStepCountOf(track) * kSequencerPulsesPerStep);

    // What a loop wrap would do: takes held over the line end with the row they were recorded in,
    // and the last step of the cut pass may still spill swung or late notes past the line.
    for (HeldRecording& recording : heldRecordings_)
      if (recording.active && recording.trackIndex == trackIndex && recording.startPulse < pulse) FinalizeHeldRecordingLocked(recording, pulse);
    std::uint16_t ran = 0;
    if (trackStarted_[trackIndex])
    {
      const std::uint16_t next = static_cast<std::uint16_t>(trackNextPulse_[trackIndex] % loopPulses);
      ran = next == 0 ? loopPulses : next;
    }
    const bool spills = ran != 0 && ran % kSequencerPulsesPerStep == 0 && !songSilent_[trackIndex];
    previousPassPattern_[trackIndex] = spills ? std::min(track.activePattern, patternCount - 1) : kNoPattern;
    previousPassPulses_[trackIndex] = ran;

    queuedPattern_[trackIndex] = kNoPattern;
    const int pattern = data.patterns[trackIndex];
    songSilent_[trackIndex] = pattern < 0;
    if (pattern >= 0) track.activePattern = std::min(static_cast<std::size_t>(pattern), patternCount - 1);
    // Every track restarts its loop with the row; no wrap is processed for this restart.
    trackNextPulse_[trackIndex] = 0;
    trackCurrentPulse_[trackIndex] = 0;
    trackStarted_[trackIndex] = false;
    if (trackIndex == activeTrack_) ReleaseEmptyPagesLocked();
  }
}

void SynthSequencer::LeaveSongLocked()
{
  songEngaged_ = false;
  songJumpPending_ = false;
  songBar_ = 0;
  songSilent_.fill(false);
}

void SynthSequencer::RemoveSongPatternLocked(std::size_t trackIndex, std::size_t patternIndex, bool shift)
{
  if (trackIndex >= kSynthTrackCount)
  {
    return;
  }
  for (std::size_t row = 0; row < songLength_; ++row)
  {
    std::int8_t& pattern = song_[row].patterns[trackIndex];
    if (pattern < 0) continue;
    if (static_cast<std::size_t>(pattern) == patternIndex) pattern = -1;
    else if (shift && static_cast<std::size_t>(pattern) > patternIndex) --pattern;
  }
}

void SynthSequencer::ResetSongLocked()
{
  song_.fill(SongRow{});
  songLength_ = 0;
  songLoop_ = true;
  songStartRow_ = 0;
  songRow_ = 0;
  LeaveSongLocked();
}

void SynthSequencer::ResetPlaybackPositionsLocked()
{
  trackNextPulse_.fill(0);
  trackCurrentPulse_.fill(0);
  trackStarted_.fill(false);
  previousPassPattern_.fill(kNoPattern);
  previousPassPulses_.fill(0);
  suppressedNotes_.fill(SuppressedNote{});
}

void SynthSequencer::ApplyQueuedPatternsLocked()
{
  for (std::size_t trackIndex = 0; trackIndex < tracks_.size(); ++trackIndex)
  {
    const std::size_t queued = queuedPattern_[trackIndex];
    queuedPattern_[trackIndex] = kNoPattern;
    TrackData& track = tracks_[trackIndex];
    if (queued >= std::clamp<std::size_t>(track.patternCount, 1, kSequencerMaxPatternCount)) continue;
    track.activePattern = queued;
    if (trackIndex == activeTrack_) ReleaseEmptyPagesLocked();
  }
}

void SynthSequencer::FinalizeHeldRecordingLocked(HeldRecording& recording, std::uint64_t pulse)
{
  const HeldRecording finished = recording;
  recording = HeldRecording{};
  if (!finished.active || finished.trackIndex >= tracks_.size())
  {
    return;
  }

  TrackData& track = tracks_[finished.trackIndex];
  if (finished.pattern >= track.patternCount || finished.step >= kSequencerMaxSteps || !track.patterns.Allocated(finished.pattern))
  {
    return;
  }

  StepData& step = track.patterns[finished.pattern].steps.Edit(finished.step);
  for (NoteEvent& note : step.notes)
  {
    if (!note.active || note.note != finished.note) continue;
    const std::uint64_t elapsed = pulse > finished.onPulse ? pulse - finished.onPulse : 0;
    // At most to the end of the track's loop, and never shorter than one microstep.
    const std::uint32_t loopPulses = static_cast<std::uint32_t>(TrackStepCountOf(track) * kSequencerPulsesPerStep);
    const std::uint32_t slotPulse = static_cast<std::uint32_t>(finished.step * kSequencerPulsesPerStep + note.microstep * kSequencerPulsesPerMicrostep);
    const std::uint32_t maximum = std::max<std::uint32_t>(kSequencerPulsesPerMicrostep, slotPulse < loopPulses ? loopPulses - slotPulse : 0);
    const auto length = static_cast<std::uint16_t>(std::clamp<std::uint64_t>(elapsed, kSequencerPulsesPerMicrostep, maximum));
    note.lengthPulses = length;
    return;
  }
}

void SynthSequencer::FinalizeAllHeldRecordingsLocked()
{
  for (HeldRecording& recording : heldRecordings_)
    if (recording.active) FinalizeHeldRecordingLocked(recording, RecordPulseLocked(recording.trackIndex));
}

std::uint64_t SynthSequencer::RecordPulseLocked(std::size_t trackIndex) const
{
  // The most recently processed pulse; before a run's first pulse, the one about to play.
  const bool started = trackIndex < trackStarted_.size() && trackStarted_[trackIndex];
  return started && absolutePulse_ > 0 ? absolutePulse_ - 1 : absolutePulse_;
}

void SynthSequencer::ReleaseTrackNotesLocked(std::size_t trackIndex,
                                             std::array<PendingMidiEvent, kMaxPendingMidiEvents>& events,
                                             std::size_t& count)
{
  for (PlaybackNote& note : playbackNotes_)
  {
    if (note.active && note.trackIndex == trackIndex)
    {
      AddPendingMidi(events, count, note.trackIndex, midi::MidiPacket::NoteOff(0, note.note, 0));
      note = PlaybackNote{};
    }
  }
}

void SynthSequencer::ClearSelectionLocked(std::array<PendingMidiEvent, kMaxPendingMidiEvents>& events, std::size_t& count)
{
  StopAuditionLocked(events, count);
  heldSteps_.fill(false);
  heldEditLanes_.fill(false);
  editLane_ = SequencerEditLane::Note;
}

std::size_t SynthSequencer::ArpTrack(const ArpVoice& arp) const
{
  return static_cast<std::size_t>(&arp - arps_.data());
}

bool SynthSequencer::AnyArpActiveLocked() const
{
  return std::any_of(arps_.begin(), arps_.end(), [this](const ArpVoice& arp) { return ArpActiveLocked(arp); });
}

bool SynthSequencer::AnyArpClockLocked() const
{
  return std::any_of(arps_.begin(), arps_.end(), [this](const ArpVoice& arp) { return ArpClockLocked(arp); });
}

void SynthSequencer::ReleaseAllArpsLocked(std::array<PendingMidiEvent, kMaxPendingMidiEvents>& events, std::size_t& count)
{
  for (ArpVoice& arp : arps_) ReleaseArpLocked(arp, events, count);
}

void SynthSequencer::AlignArpsToTransportLocked(std::uint64_t transportPulse)
{
  for (ArpVoice& arp : arps_)
    if (ArpActiveLocked(arp)) AlignArpToTransportLocked(arp, transportPulse);
}

bool SynthSequencer::ArpActiveLocked(const ArpVoice& arp) const
{
  return arp.settings.mode != ArpMode::Off && arp.keyCount > 0;
}

bool SynthSequencer::ArpClockLocked(const ArpVoice& arp) const
{
  return ArpActiveLocked(arp) || arp.soundingNote >= 0;
}

std::uint16_t SynthSequencer::ArpPeriodLocked(const ArpVoice& arp) const
{
  return kArpRatePulses[std::min<std::size_t>(arp.settings.rate, kArpRateCount - 1)];
}

std::uint16_t SynthSequencer::ArpSwingLocked(std::uint64_t gridPulse, std::uint16_t period) const
{
  // Only a sixteenth grid has notes on odd sixteenths; swing delays them like sequenced steps.
  if (period % kSequencerPulsesPerStep != 0 || (gridPulse / kSequencerPulsesPerStep) % 2 == 0) return 0;
  return SwingDelayPulsesLocked();
}

bool SynthSequencer::ArpCandidateLocked(const ArpVoice& arp, std::size_t index, std::uint8_t& note, std::uint8_t& velocity) const
{
  if (arp.keyCount == 0) return false;
  const ArpKey& key = arp.keys[index % arp.keyCount];
  const std::size_t pitch = key.note + 12u * (index / arp.keyCount);
  if (pitch > 127) return false;
  note = static_cast<std::uint8_t>(pitch);
  velocity = key.velocity;
  return true;
}

bool SynthSequencer::NextArpNoteLocked(ArpVoice& arp, std::uint8_t& note, std::uint8_t& velocity)
{
  const std::size_t total = arp.keyCount * std::clamp<std::size_t>(arp.settings.octaves, 1, kArpMaxOctaves);
  if (total == 0) return false;
  const int last = arp.lastNote;

  // Bit set of the pitches the pattern holds, for RANDOM: a pitch two keys reach counts once.
  std::array<std::uint32_t, 4> present{};
  auto mark = [&present](std::uint8_t pitch) {
    std::uint32_t& word = present[pitch / 32u];
    const std::uint32_t bit = 1u << (pitch % 32u);
    const bool fresh = (word & bit) == 0;
    word |= bit;
    return fresh;
  };

  int chosen = -1;
  std::uint8_t chosenVelocity = 0;
  switch (arp.settings.mode)
  {
    case ArpMode::Up:
    case ArpMode::Down:
    case ArpMode::UpDown:
    {
      // Neighbours of the last pitch and the ends of the range; the first key reaching a pitch wins.
      int lowest = -1, highest = -1, above = -1, below = -1;
      std::uint8_t lowestVelocity = 0, highestVelocity = 0, aboveVelocity = 0, belowVelocity = 0;
      for (std::size_t index = 0; index < total; ++index)
      {
        std::uint8_t pitch = 0;
        std::uint8_t level = 0;
        if (!ArpCandidateLocked(arp, index, pitch, level)) continue;
        const int value = pitch;
        if (lowest < 0 || value < lowest) { lowest = value; lowestVelocity = level; }
        if (value > highest) { highest = value; highestVelocity = level; }
        if (value > last && (above < 0 || value < above)) { above = value; aboveVelocity = level; }
        if (value < last && value > below) { below = value; belowVelocity = level; }
      }
      if (lowest < 0) return false;
      const bool hasLast = last >= 0;
      if (arp.settings.mode == ArpMode::Up)
      {
        chosen = hasLast && above >= 0 ? above : lowest;
        chosenVelocity = hasLast && above >= 0 ? aboveVelocity : lowestVelocity;
      }
      else if (arp.settings.mode == ArpMode::Down)
      {
        chosen = hasLast && below >= 0 ? below : highest;
        chosenVelocity = hasLast && below >= 0 ? belowVelocity : highestVelocity;
      }
      else
      {
        // Turns at either end without playing the end note twice; a single pitch just repeats.
        if (!hasLast) arp.descending = false;
        else if (!arp.descending && above < 0 && below >= 0) arp.descending = true;
        else if (arp.descending && below < 0 && above >= 0) arp.descending = false;
        if (!hasLast) { chosen = lowest; chosenVelocity = lowestVelocity; }
        else if (!arp.descending) { chosen = above >= 0 ? above : lowest; chosenVelocity = above >= 0 ? aboveVelocity : lowestVelocity; }
        else { chosen = below >= 0 ? below : highest; chosenVelocity = below >= 0 ? belowVelocity : highestVelocity; }
      }
      break;
    }
    case ArpMode::Random:
    {
      std::size_t distinct = 0;
      bool lastPresent = false;
      for (std::size_t index = 0; index < total; ++index)
      {
        std::uint8_t pitch = 0;
        std::uint8_t level = 0;
        if (!ArpCandidateLocked(arp, index, pitch, level) || !mark(pitch)) continue;
        ++distinct;
        if (pitch == last) lastPresent = true;
      }
      if (distinct == 0) return false;
      // Never the same pitch twice in a row while there is a choice.
      const bool skipLast = distinct > 1 && lastPresent;
      arp.random ^= arp.random << 13;
      arp.random ^= arp.random >> 17;
      arp.random ^= arp.random << 5;
      std::size_t pick = arp.random % (distinct - (skipLast ? 1u : 0u));
      present.fill(0);
      for (std::size_t index = 0; index < total && chosen < 0; ++index)
      {
        std::uint8_t pitch = 0;
        std::uint8_t level = 0;
        if (!ArpCandidateLocked(arp, index, pitch, level) || !mark(pitch) || (skipLast && pitch == last)) continue;
        if (pick-- == 0) { chosen = pitch; chosenVelocity = level; }
      }
      break;
    }
    case ArpMode::Played:
    {
      for (std::size_t step = 0; step < total && chosen < 0; ++step)
      {
        const std::size_t index = (arp.orderIndex + step) % total;
        std::uint8_t pitch = 0;
        std::uint8_t level = 0;
        if (!ArpCandidateLocked(arp, index, pitch, level)) continue;
        chosen = pitch;
        chosenVelocity = level;
        arp.orderIndex = index + 1;
      }
      break;
    }
    case ArpMode::Off:
    default:
      return false;
  }
  if (chosen < 0) return false;
  note = static_cast<std::uint8_t>(chosen);
  velocity = chosenVelocity;
  arp.lastNote = chosen;
  return true;
}

void SynthSequencer::ResetArpOrderLocked(ArpVoice& arp)
{
  arp.lastNote = -1;
  arp.descending = false;
  arp.orderIndex = 0;
}

void SynthSequencer::RemoveArpKeyLocked(ArpVoice& arp, std::size_t index)
{
  if (index >= arp.keyCount) return;
  // ORDER keeps its place: the key after the removed one comes next.
  const std::size_t octave = arp.orderIndex / arp.keyCount;
  std::size_t within = arp.orderIndex % arp.keyCount;
  if (within > index) --within;
  for (std::size_t next = index; next + 1 < arp.keyCount; ++next) arp.keys[next] = arp.keys[next + 1];
  arp.keys[--arp.keyCount] = ArpKey{};
  arp.orderIndex = octave * arp.keyCount + within;
}

void SynthSequencer::FireArpNoteLocked(ArpVoice& arp, std::array<PendingMidiEvent, kMaxPendingMidiEvents>& events, std::size_t& count)
{
  std::uint8_t note = 0;
  std::uint8_t velocity = 0;
  if (!NextArpNoteLocked(arp, note, velocity)) return;
  CutArpNoteLocked(arp, events, count);
  AddPendingMidi(events, count, ArpTrack(arp), midi::MidiPacket::NoteOn(0, note, velocity));
  const std::uint32_t gate = static_cast<std::uint32_t>(ArpPeriodLocked(arp)) * std::clamp(arp.settings.gate, kArpMinGate, kArpMaxGate) / 100u;
  arp.soundingNote = note;
  arp.offPulse = arp.pulse + std::max<std::uint32_t>(1, gate);
  arp.lastOnPulse = arp.pulse;
  arp.hasLastOn = true;
  arp.lastPlayed = note;
  ++arp.tick;
  // Overdub: the arp's own note-off ends the take.
  RecordNoteOnLocked(ArpTrack(arp), note, velocity);
}

void SynthSequencer::CutArpNoteLocked(ArpVoice& arp, std::array<PendingMidiEvent, kMaxPendingMidiEvents>& events, std::size_t& count)
{
  if (arp.soundingNote < 0) return;
  const auto note = static_cast<std::uint8_t>(arp.soundingNote);
  AddPendingMidi(events, count, ArpTrack(arp), midi::MidiPacket::NoteOff(0, note, 0));
  RecordNoteOffLocked(ArpTrack(arp), note);
  arp.soundingNote = -1;
}

void SynthSequencer::ReleaseArpLocked(ArpVoice& arp, std::array<PendingMidiEvent, kMaxPendingMidiEvents>& events, std::size_t& count)
{
  CutArpNoteLocked(arp, events, count);
  arp.keys.fill(ArpKey{});
  arp.keyCount = 0;
  arp.hasLastOn = false;
  if (!playing_ && !AnyArpClockLocked()) clockStartMicros_ = 0;
  ResetArpOrderLocked(arp);
}

void SynthSequencer::AlignArpToTransportLocked(ArpVoice& arp, std::uint64_t transportPulse)
{
  const std::uint16_t period = ArpPeriodLocked(arp);
  const std::uint64_t sinceLast = arp.hasLastOn ? arp.pulse - arp.lastOnPulse : period;
  // Three grid lines always reach one far enough ahead: the third starts a period after now.
  std::uint64_t grid = transportPulse - transportPulse % period;
  for (int attempt = 0; attempt < 3; ++attempt, grid += period)
  {
    const std::uint64_t fire = grid + ArpSwingLocked(grid, period);
    if (fire < transportPulse) continue;
    const std::uint64_t ahead = fire - transportPulse;
    if (sinceLast + ahead < period / 2u) continue;
    arp.nextPulse = arp.pulse + ahead;
    return;
  }
  arp.nextPulse = arp.pulse + period;
}

void SynthSequencer::RescheduleArpLocked(ArpVoice& arp)
{
  if (!ArpActiveLocked(arp)) return;
  if (playing_)
  {
    AlignArpToTransportLocked(arp, absolutePulse_);
    return;
  }
  const std::uint64_t due = arp.hasLastOn ? arp.lastOnPulse + ArpPeriodLocked(arp) : arp.pulse;
  arp.nextPulse = std::max(due, arp.pulse);
}

void SynthSequencer::ProcessArpPulseLocked(ArpVoice& arp, std::uint64_t transportPulse,
                                           std::array<PendingMidiEvent, kMaxPendingMidiEvents>& events,
                                           std::size_t& count)
{
  const std::uint64_t now = arp.pulse;
  // A gate of a whole period ends the old note just before the next one starts.
  if (arp.soundingNote >= 0 && arp.offPulse <= now) CutArpNoteLocked(arp, events, count);
  if (ArpActiveLocked(arp) && arp.nextPulse <= now)
  {
    FireArpNoteLocked(arp, events, count);
    // While playing, the next note waits for the next grid line (the song may just have ended).
    if (playing_) AlignArpToTransportLocked(arp, transportPulse);
    else arp.nextPulse = now + ArpPeriodLocked(arp);
  }
  ++arp.pulse;
}

}  // namespace synth
