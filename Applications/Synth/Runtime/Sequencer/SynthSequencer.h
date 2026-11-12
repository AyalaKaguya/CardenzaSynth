#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <memory>
#include <string>

#if !defined(ESP_PLATFORM)
#include <thread>
#endif

#include "Applications/Synth/Runtime/Sequencer/SynthSequencerTypes.h"
#include "OS/Framework/Midi/MidiPacket.h"

namespace synth {

class SynthSequencer {
public:
  using MidiSink = void (*)(void* context, std::size_t trackIndex, const midi::MidiPacket& packet);

  SynthSequencer();
  ~SynthSequencer();

  void SetMidiSink(MidiSink sink, void* context);
  void SetActiveTrack(std::size_t trackIndex);

  void Play();
  void Resume();
  void Stop();
  void TogglePlayback();
  bool Playing() const;
#if defined(__EMSCRIPTEN__)
  // The web build has no threads, so no clock worker runs; the host pumps the clock instead.
  // Processes every pulse due by nowMicros on the host's clock: ClockLoop's steps (transport and
  // arpeggiator) with its sleeps replaced by returns. The web host passes audio time (frames
  // rendered so far) before each audio block, so pulses land on exact block positions whatever the
  // main thread does.
  void PumpClock(std::uint64_t nowMicros);
  void PumpClock() { PumpClock(NowMicros()); }
#endif
  void AdjustTempo(int delta);
  void ToggleTrackMuted();
  void ToggleTrackEnabled();
  void Reset();
  void StepPattern(int delta);
  void AdjustPatternCount(int delta);
  bool SelectOrCreatePattern(std::size_t patternIndex);
  bool DeletePattern(std::size_t patternIndex);
  bool CopyPattern(std::size_t sourcePattern, std::size_t destinationPattern);
  bool PatternHasEvents(std::size_t patternIndex) const;
  // Bit n set = step n of that track's pattern holds notes (0 for an empty or missing pattern). A
  // pattern longer than 16 steps is cut in 16 equal slices, a bit for each.
  std::uint16_t PatternStepMask(std::size_t trackIndex, std::size_t patternIndex) const;

  // A step pressed (it is held) with notes held: in the NOTE lane they go in or out as one group, all
  // or none (any of them there takes them all out).
  void PressStep(std::size_t stepIndex, const std::uint8_t* heldNotes, std::size_t heldNoteCount);
  void ReleaseStep(std::size_t stepIndex);
  void ClearHeldSteps();
  void ClearStep(std::size_t stepIndex);
  void ClearHeldStepEvents();
  bool CopyStep(std::size_t sourceStep, std::size_t destinationStep);
  bool StepHasEvents(std::size_t stepIndex) const;
  // Pitches of the active notes of `step` in the active track's active pattern, ascending.
  // Returns how many were written; 0 for an empty or out-of-range step.
  std::size_t StepNotes(std::size_t step, std::array<std::uint8_t, 8>& notes) const;
  // Every note of one step of a track's active pattern, with its timing (the piano roll).
  struct StepNoteView {
    std::uint8_t note = 0;
    std::uint8_t velocity = 0;
    std::uint16_t lengthPulses = 0;
    std::uint8_t microstep = 0;
    std::uint8_t hits = 1;
  };
  std::size_t StepNoteEvents(std::size_t track, std::size_t step, std::array<StepNoteView, 8>& notes) const;
  // Step recording: the step's notes become exactly these (one step long, on the grid, one hit).
  bool SetStepNotes(std::size_t track, std::size_t step, const std::uint8_t* notes, std::size_t count, std::uint8_t velocity);
  // Notes held on the pads and a step tapped (MatrixOS): all or none, a chord as one. When the step of
  // the track's active pattern holds any of them they all come out and none goes in, else they all go
  // in, with the track's new-note length and each its velocity from `velocities` (null, or 0 for a
  // note: the track's new-note velocity). The selection is left alone. False when nothing was given.
  bool ToggleStepNotes(std::size_t track, std::size_t step, const std::uint8_t* notes, std::size_t count,
                       const std::uint8_t* velocities = nullptr);
  // Adds one timed note to a pattern of a track (a note of the same pitch on that step is
  // replaced). False when the step already holds eight other notes or the pattern does not exist.
  bool RemoveNote(std::size_t track, std::size_t pattern, std::size_t step, std::uint8_t note);
  // Moves one note within its step, to another microstep. False when it is not there, or another of
  // the same pitch already is.
  bool MoveNote(std::size_t track, std::size_t pattern, std::size_t step, std::uint8_t note, std::uint8_t fromMicrostep, std::uint8_t toMicrostep);
  // InsertNote (above) writes a plain note, one hit: a take lays down what was played.
  bool InsertNote(std::size_t track, std::size_t pattern, std::size_t step, std::uint8_t microstep, std::uint8_t note,
                  std::uint8_t velocity, std::uint16_t lengthPulses);
  // Pulses processed since playback started (0 while stopped): where a played note sits in time.
  std::uint64_t PlayPulse() const;
  std::uint8_t SelectedNoteVelocity(std::uint8_t pitch) const;
  void QuantizeStep(std::size_t stepIndex);
  void QuantizeHeldSteps();
  void QuantizeActivePattern();
  void QuantizePattern(std::size_t patternIndex);
  bool IsStepHeld(std::size_t stepIndex) const;
  bool HasHeldSteps() const;

  bool PressEditLane(std::size_t row);
  bool ReleaseEditLane(std::size_t row);
  bool HandlePerformanceKeyPress(std::size_t row, std::size_t column, std::uint8_t note);

  SequencerUiState BuildUiState(std::size_t activeTrack) const;

  // Live overdub. Notes are written only while playing and armed, into the track's active
  // pattern at its playback position, quantized to the nearest microstep. The sequencer never
  // sounds the live note itself: the caller already played it.
  void SetRecordArmed(bool armed);
  bool RecordArmed() const;
  void RecordNoteOn(std::size_t track, std::uint8_t note, std::uint8_t velocity);
  void RecordNoteOff(std::size_t track, std::uint8_t note);

  // Live arpeggiators, one per track, each with its own settings and held notes; they run side by
  // side. The caller hands a track's arp every note a key press makes (instead of sounding them)
  // and the arp plays the held set through the MIDI sink, one note per rate period, like sequenced
  // notes. The first press while the arp is idle sounds at once; later notes follow the transport
  // grid while playing (swung like sequenced sixteenths at 1/16), else a free clock at the current
  // tempo. Stop() cuts the sounding notes but lets held or latched arps run on free.
  // While playing and armed, every arp note is overdubbed into its track's active pattern.
  // Settings are clamped; mode Off, or latch switched off with no key held, releases the arp.
  void SetArpSettings(std::size_t track, const ArpSettings& settings);
  ArpSettings GetArpSettings(std::size_t track) const;
  // Ignored while the track's mode is Off. With latch on, the first press after every key was
  // released replaces the latched set. Velocity 0 is a note-off.
  void ArpNoteOn(std::size_t track, std::uint8_t note, std::uint8_t velocity);
  void ArpNoteOff(std::size_t track, std::uint8_t note);
  // Drops held and latched notes and silences the sounding note of every arp (RestoreState() and
  // Reset() do this too), or of one track's arp.
  void ArpReleaseAll();
  void ArpRelease(std::size_t track);
  // Marks the notes the sequencer is sounding on a track (steps, audition, arpeggiator), one bit per
  // MIDI note.
  void AddSoundingNotes(std::size_t track, std::array<std::uint32_t, 4>& notes) const;
  // Mode on and notes held or latched: on any track, or on one.
  bool ArpActive() const;
  bool ArpActive(std::size_t track) const;

#if defined(CARDENZA_SEQUENCER_TEST_HOOKS)
  // Test only. Call first: the clock worker is never started, and AdvancePulsesForTest() runs
  // the clock by hand, one pulse per step, exactly when the worker would not idle.
  void UseManualClockForTest();
  void AdvancePulsesForTest(std::size_t pulses);
  // From now on at most this many new pages of steps can be allocated (-1: no limit).
  static void SetPageBudgetForTest(int pages);
#endif

  // Switches immediately while stopped; while playing, at the track's next loop boundary.
  // patternIndex == PatternCount(track) creates a new pattern slot. Queueing the active
  // pattern cancels a pending switch.
  bool QueuePattern(std::size_t track, std::size_t patternIndex);

  // 50 is straight, 75 the hardest swing: odd sixteenths are delayed.
  void SetSwing(std::uint8_t percent);
  std::uint8_t Swing() const;

  // Each pattern has its own length (1..256 steps), and a track loops at the length of the pattern
  // it plays. These act on the track's current pattern; later steps keep their data.
  void SetTrackStepCount(std::size_t track, std::size_t steps);
  std::size_t TrackStepCount(std::size_t track) const;
  std::size_t PatternStepCount(std::size_t track, std::size_t pattern) const;

  // The GRID shows one page (16 steps) of the current pattern. While playing it follows the
  // playhead until the page is flipped by hand; Play goes back to following.
  void SetViewPage(std::size_t track, std::size_t page);
  std::size_t ViewFirstStep(std::size_t track) const;

  // True once after a write found no memory for a new page of steps (the write was dropped).
  static bool ConsumePatternMemoryFull();

  // Selection editing without audition. The selection is the held-step set of the active track.
  void SetStepSelected(std::size_t step, bool selected);
  bool StepSelected(std::size_t step) const;
  void ClearSelectedSteps();
  // A note into the selected steps, or out of those that have it; a new one gets `velocity` (0: the
  // track's new-note velocity).
  bool ToggleNoteOnSelectedSteps(std::uint8_t note, std::uint8_t velocity = 0);
  // The notes of one pad (its chord) into the selected steps or out of them, as one: a selected step
  // that holds any of them loses them all and gains none, one that holds none gains them all, each at
  // `velocity` (0: the track's new-note velocity). False when no step is selected or no note given.
  bool ToggleNotesOnSelectedSteps(const std::uint8_t* notes, std::size_t count, std::uint8_t velocity = 0);
  // Whether a selected step holds any of these notes (a pad's, its chord as one: the pad is lit).
  // `others`, when given, gets every other note those steps hold: where a move of them must not land.
  bool SelectedStepsHoldAny(const std::uint8_t* notes, std::size_t count, std::array<bool, 128>* others = nullptr) const;
  // A held pad's notes moved (ADD / SUB): on each selected step that holds any of from[0..count),
  // from[i] becomes to[i], keeping its velocity, length and timing (a step holding some of them moves
  // those). Nothing changes when such a step already holds one of `to` besides them. True when a note
  // moved.
  bool MoveNotesOnSelectedSteps(const std::uint8_t* from, const std::uint8_t* to, std::size_t count);
  void SetEditLane(SequencerEditLane lane);
  SequencerEditLane EditLane() const;
  void CycleEditLane();
  // Relative edit of the current lane for every note of each selected step. Returns true when any
  // value changed.
  bool AdjustSelectedSteps(int delta, bool coarse);
  // The same edit for these notes only (a held pad's, its chord as one), on each selected step that
  // holds any of them: what ADD / SUB do while a lit pad is held. True when a value changed.
  bool AdjustNotesOnSelectedSteps(const std::uint8_t* notes, std::size_t count, int delta, bool coarse);
  // The current lane's value ("96" on VEL) of the first of these notes on the selected steps; empty
  // when none holds them.
  std::string NotesLaneValue(const std::uint8_t* notes, std::size_t count) const;

  // Song (arrangement) mode. With the switch on and at least one row, Play() starts at the cued
  // row. Each row applies at a bar line: every track switches to the row's pattern (-1 leaves the
  // track silent for the row without touching mute/enable), restarts at step 0, and drops a
  // queued pattern switch. A row lasts repeats bars of the global clock, whatever the tracks'
  // step counts. After the last row the song loops to row 0, or playback stops when looping is
  // off. Flipping the switch while playing takes effect at the next bar line; switching off keeps
  // the patterns of the row that was playing and lets every track loop on from where it is.
  // Edits to the playing row are heard the next time a row starts; its length is read at each
  // bar line. The switch is a performance control: projects do not store it.
  void SetSongMode(bool enabled);
  bool SongMode() const;
  void SetSongLoop(bool loop);
  bool SongLoop() const;
  std::size_t SongLength() const;
  // A default row (every track on pattern 0, one bar) when out of range.
  SongRow SongRowAt(std::size_t row) const;
  // pattern is -1..PatternCount(track) - 1; row < SongLength().
  bool SetSongPattern(std::size_t row, std::size_t track, int pattern);
  // 1..kSongMaxRepeats bars.
  bool SetSongRepeats(std::size_t row, std::uint8_t repeats);
  // at <= SongLength() < kSongMaxRows; every pattern index and the repeats must be valid.
  bool InsertSongRow(std::size_t at, const SongRow& row);
  // Deleting the playing row ends it at the next bar line; the row that takes its place follows.
  bool DeleteSongRow(std::size_t row);
  // Playing with song mode on: jump to the row at the next bar line (also where a song switched
  // on mid-play takes over). Otherwise: the start row for Play() and for switching song mode on
  // while playing; it stays until cued again and is clamped to the song. row < SongLength().
  void CueSongRow(std::size_t row);

  void SetTempo(std::uint16_t bpm);
  std::uint16_t Tempo() const;

  // How late the clock task ran its pulses (on the board it sleeps until each one is due): pulses
  // since the last call, the latest of them, and how many were more than kLatePulseMicros late (a note
  // that late is heard off the beat).
  static constexpr std::uint32_t kLatePulseMicros = 5000;
  struct ClockLateness {
    std::uint32_t pulses = 0;
    std::uint32_t latestMicros = 0;
    std::uint32_t latePulses = 0;
  };
  ClockLateness TakeClockLateness();
  void SetTrackEnabled(std::size_t track, bool enabled);
  bool TrackEnabled(std::size_t track) const;
  bool ClearPatternEvents(std::size_t track, std::size_t patternIndex);
  std::size_t PatternCount(std::size_t track) const;
  std::size_t ActivePattern(std::size_t track) const;

  // Project snapshots are heap-owned: the full four-track data exceeds the firmware task stack.
  struct NoteEvent {
    std::uint8_t note = 60;
    std::uint8_t velocity = 110;
    std::uint16_t lengthPulses = kSequencerPulsesPerStep;
    std::uint8_t microstep = 0;
    // One byte for whether the slot holds a note and for its HITS less one (0..3: it fires 1..4 times
    // in its step), so a page of steps stays 768 bytes. HITS less one: zero, what a cleared slot holds,
    // is a plain note (and 1..4 stored as is would need a third bit).
    bool active : 1;
    std::uint8_t extraHits : 2;

    constexpr NoteEvent() : active(false), extraHits(0) {}
    constexpr NoteEvent(std::uint8_t pitch, std::uint8_t noteVelocity, std::uint16_t length, std::uint8_t noteMicrostep, bool isActive,
                        std::uint8_t hits = 1)
        : note(pitch), velocity(noteVelocity), lengthPulses(length), microstep(noteMicrostep), active(isActive),
          extraHits(static_cast<std::uint8_t>(ClampSequencerHits(hits) - 1))
    {
    }
    constexpr std::uint8_t Hits() const { return static_cast<std::uint8_t>(extraHits + 1); }
    void SetHits(int hits) { extraHits = static_cast<std::uint8_t>(ClampSequencerHits(hits) - 1) & 3u; }
  };
  static_assert(kSequencerMaxHits - 1 <= 3, "HITS less one must fit NoteEvent::extraHits' two bits.");

  struct StepData {
    std::array<NoteEvent, 8> notes{};
  };
  // A page of steps (StepStore) is 16 of these in PSRAM: 768 bytes.
  static_assert(sizeof(NoteEvent) == 6, "A NoteEvent is six bytes: HITS lives in the spare bits of `active`'s byte.");
  static_assert(sizeof(StepData) == 48, "A step is 48 bytes, a page of 16 steps 768.");

  // The steps of a pattern, stored one GRID page at a time. A page costs its 768 bytes only once
  // something is written on it (256 steps flat would be 12 KB per pattern, and the board has no
  // PSRAM). Copies share their pages until one of them writes, so a project snapshot costs next to
  // nothing however long the patterns are. Reading never allocates.
  class StepStore {
  public:
    StepStore() = default;
    StepStore(const StepStore& other);
    StepStore& operator=(const StepStore& other);
    ~StepStore();

    static constexpr std::size_t size() { return kSequencerMaxSteps; }
    const StepData& operator[](std::size_t step) const;
    // The step to write. Null when there is no memory for its page.
    StepData* TryEdit(std::size_t step);
    // Same, but without memory the write lands in a discarded scratch step and
    // ConsumePatternMemoryFull() reports it.
    StepData& Edit(std::size_t step);
    // The first step at or after `step` on a page that exists, size() when none is left.
    std::size_t NextInUse(std::size_t step) const;
    // Steps holding at least one note (cached until the next edit).
    std::size_t PopulatedSteps() const;
    // Gives the memory of pages emptied by edits back. Invalidates references into this store.
    void ReleaseEmptyPages();
    void Clear();

  private:
    struct Page;
    static void ReleasePage(Page* page);
    static Page* NewPage();
    std::array<Page*, kSequencerMaxStepPages> pages_{};
    mutable std::uint16_t populated_ = 0;
    mutable bool populatedKnown_ = true;
  };

  struct PatternData {
    StepStore steps;
  };

  // Patterns are allocated on first write, and their steps page by page (StepStore): inline
  // banks for every track would cost internal RAM the firmware cannot spare. Unallocated patterns
  // read as empty.
  class PatternBank {
  public:
    PatternBank() = default;
    PatternBank(const PatternBank& other) { (void)CopyFrom(other); }
    PatternBank& operator=(const PatternBank& other);
    PatternBank(PatternBank&&) noexcept = default;
    PatternBank& operator=(PatternBank&&) noexcept = default;

    // Allocates on demand; if memory runs out, writes land in a discarded scratch pattern.
    PatternData& operator[](std::size_t index);
    const PatternData& operator[](std::size_t index) const;
    static constexpr std::size_t size() { return kSequencerMaxPatternCount; }

    bool Allocated(std::size_t index) const;
    bool Ensure(std::size_t index);
    // Allocates every pattern `other` holds, so a following copy cannot fail halfway.
    bool Reserve(const PatternBank& other);
    bool CopyFrom(const PatternBank& other);
    void Release(std::size_t index);
    void Swap(std::size_t left, std::size_t right);
    void Clear();

  private:
    std::array<std::unique_ptr<PatternData>, kSequencerMaxPatternCount> patterns_{};
  };

  struct TrackData {
    PatternBank patterns{};
    std::size_t patternCount = 1;
    std::size_t activePattern = 0;
    std::uint8_t defaultVelocity = 110;
    std::uint16_t defaultLengthPulses = kSequencerPulsesPerStep;
    // HITS a note written by hand starts with (NEW HITS, 1..kSequencerMaxHits); recording writes 1.
    std::uint8_t defaultHits = 1;
    bool enabled = true;
    bool muted = false;
    // Each pattern's own length, 1..kSequencerMaxSteps.
    std::array<std::uint16_t, kSequencerMaxPatternCount> patternSteps = DefaultPatternSteps();

    static constexpr std::array<std::uint16_t, kSequencerMaxPatternCount> DefaultPatternSteps()
    {
      std::array<std::uint16_t, kSequencerMaxPatternCount> steps{};
      for (auto& count : steps) count = kSequencerStepCount;
      return steps;
    }
  };

  struct ProjectState {
    std::uint16_t bpm = 120;
    std::array<TrackData, kSynthTrackCount> tracks{};
    std::uint8_t swing = 50;
    // Rows past songLength are ignored and come back as default rows.
    std::array<SongRow, kSongMaxRows> song{};
    std::size_t songLength = 0;
    bool songLoop = true;
  };
  std::unique_ptr<ProjectState> CaptureState() const;
  static bool ValidateState(const ProjectState& state);
  bool RestoreState(const ProjectState& state);

private:

  struct PlaybackNote {
    std::uint64_t offPulse = 0;
    std::uint8_t trackIndex = 0;
    std::uint8_t note = 0;
    bool active = false;
  };

  struct PendingMidiEvent {
    std::size_t trackIndex = 0;
    midi::MidiPacket packet;
  };

  // A live note whose length is still open until its note-off or the track's loop wrap.
  struct HeldRecording {
    std::uint64_t onPulse = 0;
    std::uint64_t startPulse = 0;
    std::size_t pattern = 0;
    std::uint8_t trackIndex = 0;
    std::uint8_t step = 0;
    std::uint8_t note = 0;
    bool active = false;
  };

  // A recorded note quantized ahead of the playhead must not sound again in the same pass.
  struct SuppressedNote {
    std::uint64_t pulse = 0;
    std::uint8_t trackIndex = 0;
    std::uint8_t note = 0;
    bool active = false;
  };

  static constexpr std::size_t kMaxPlaybackNotes = 96;
  static constexpr std::size_t kMaxPendingMidiEvents = 128;
  static constexpr std::size_t kMaxHeldRecordings = 16;
  static constexpr std::size_t kMaxSuppressedNotes = 16;
  static constexpr std::size_t kNoPattern = kSequencerMaxPatternCount;

  void EnsureClockWorkerStarted();
  void StopClockWorker();
#if defined(ESP_PLATFORM)
  static void ClockTaskEntry(void* context);
#endif
  void ClockLoop();
  // Whether the clock must run pulses (transport playing or arp in use). Also hands over, once,
  // when an arp started from idle wants its first pulse (0 = none).
  bool ClockNeeded(std::uint64_t& startMicros);
  void ProcessPulse();
  void ProcessTransportPulseLocked(std::uint64_t pulse,
                                   std::array<PendingMidiEvent, kMaxPendingMidiEvents>& events,
                                   std::size_t& count);
  std::uint64_t NowMicros() const;
  void SleepUntilMicros(std::uint64_t targetMicros) const;
  std::uint64_t PulsePeriodMicrosLocked() const;
  void AddPendingMidi(std::array<PendingMidiEvent, kMaxPendingMidiEvents>& events,
                      std::size_t& count,
                      std::size_t trackIndex,
                      const midi::MidiPacket& packet) const;
  void EmitPendingMidi(const std::array<PendingMidiEvent, kMaxPendingMidiEvents>& events, std::size_t count) const;
  bool ScheduleNoteOffLocked(std::size_t trackIndex, std::uint8_t note, std::uint64_t offPulse);
  void CollectDueNoteOffsLocked(std::uint64_t pulse,
                                std::array<PendingMidiEvent, kMaxPendingMidiEvents>& events,
                                std::size_t& count);
  void CollectAllNoteOffsLocked(std::array<PendingMidiEvent, kMaxPendingMidiEvents>& events, std::size_t& count);
  void StopAuditionLocked(std::array<PendingMidiEvent, kMaxPendingMidiEvents>& events, std::size_t& count);

  void ToggleNoteInStepLocked(StepData& step, std::uint8_t note, TrackData& track, std::size_t stepIndex, std::uint8_t velocity = 0);
  // A group of notes (given[note]: a pad's chord, the pads held) toggled in a step all or none: any of
  // them there and they all come out, none goes in; none there and they all go in, each at velocity[note].
  void ToggleNotesInStepLocked(StepStore& steps, std::size_t stepIndex, const std::array<bool, 128>& given,
                               const std::array<std::uint8_t, 128>& velocity, TrackData& track);
  void ApplyLengthToHeldStepsLocked(std::uint16_t lengthPulses);
  void ApplyVelocityToHeldStepsLocked(std::uint8_t velocity);
  void ApplyMicrostepToHeldStepsLocked(std::uint8_t microstep);
  void ClearStepLocked(StepData& step);
  void QuantizeStepLocked(StepData& step, std::size_t stepIndex);
  // Gives back the memory of the active pattern's pages that edits emptied.
  void ReleaseEmptyPagesLocked();
  void PopulateEditValueStateLocked(SequencerUiState& state, const StepData* step) const;
  PatternData& ActivePatternLocked(TrackData& track);
  const PatternData& ActivePatternLocked(const TrackData& track) const;
  const StepData* PrimaryHeldStepLocked() const;
  std::size_t ActiveNoteCount(const StepData& step) const;
  std::uint16_t ClampLengthForStepMicrostep(std::size_t stepIndex, std::uint8_t microstep, std::uint16_t lengthPulses) const;
  static std::uint16_t ClampLengthInPattern(std::size_t patternSteps, std::size_t stepIndex, std::uint8_t microstep, std::uint16_t lengthPulses);
  std::string ValueLabelLocked(const StepData* step) const;
  // The edit lanes, in one place: the current lane's edit of some notes of a step (its slots), which
  // every way of editing a lane goes through (AdjustSelectedSteps, AdjustNotesOnSelectedSteps), and a
  // note's value on that lane. A new lane is added here (and to CycleEditLane and its label). True
  // when a value changed.
  bool AdjustLaneLocked(StepData& step, std::size_t stepIndex, std::array<std::size_t, 8> slots, std::size_t slotCount, int delta,
                        bool coarse);
  std::string LaneValueLocked(const NoteEvent& note) const;
  static std::uint16_t AdjustedLength(std::uint16_t current, int delta, bool coarse);
  static std::uint8_t AdjustedHits(std::uint8_t current, int delta, bool coarse);

  static std::size_t TrackStepCountOf(const TrackData& track);
  std::size_t ViewPageLocked(std::size_t trackIndex) const;
  std::uint16_t SwingDelayPulsesLocked() const;
  std::uint16_t NoteOffsetPulsesLocked(std::size_t stepIndex, std::uint8_t microstep) const;
  void FireStepNotesLocked(std::size_t trackIndex,
                           const StepData& step,
                           std::size_t stepIndex,
                           std::uint32_t targetOffset,
                           std::uint64_t pulse,
                           std::array<PendingMidiEvent, kMaxPendingMidiEvents>& events,
                           std::size_t& count);
  bool ConsumeSuppressedNoteLocked(std::size_t trackIndex, std::uint8_t note, std::uint64_t pulse);
  void HandleLoopBoundaryLocked(std::size_t trackIndex, std::uint64_t pulse);
  void StopLocked(std::array<PendingMidiEvent, kMaxPendingMidiEvents>& events, std::size_t& count);
  // Runs on every bar line of the global clock; false when the song ended and playback stopped.
  bool HandleSongBarLocked(std::uint64_t pulse, std::array<PendingMidiEvent, kMaxPendingMidiEvents>& events, std::size_t& count);
  void StartSongRowLocked(std::size_t row, std::uint64_t pulse);
  void LeaveSongLocked();
  // Song references to a removed pattern become -1; later indices shift down when `shift`.
  void RemoveSongPatternLocked(std::size_t trackIndex, std::size_t patternIndex, bool shift);
  void ResetSongLocked();
  void ResetPlaybackPositionsLocked();
  void ApplyQueuedPatternsLocked();
  void RecordNoteOnLocked(std::size_t trackIndex, std::uint8_t note, std::uint8_t velocity);
  void RecordNoteOffLocked(std::size_t trackIndex, std::uint8_t note);
  void FinalizeHeldRecordingLocked(HeldRecording& recording, std::uint64_t pulse);
  void FinalizeAllHeldRecordingsLocked();
  std::uint64_t RecordPulseLocked(std::size_t trackIndex) const;
  void ReleaseTrackNotesLocked(std::size_t trackIndex,
                               std::array<PendingMidiEvent, kMaxPendingMidiEvents>& events,
                               std::size_t& count);
  void ClearSelectionLocked(std::array<PendingMidiEvent, kMaxPendingMidiEvents>& events, std::size_t& count);

  // Arpeggiators (one ArpVoice per track; ArpTrack() is its track).
  struct ArpVoice;
  std::size_t ArpTrack(const ArpVoice& arp) const;
  bool ArpActiveLocked(const ArpVoice& arp) const;
  bool AnyArpActiveLocked() const;
  // Active, or its last note still sounding: the clock must run.
  bool ArpClockLocked(const ArpVoice& arp) const;
  bool AnyArpClockLocked() const;
  std::uint16_t ArpPeriodLocked(const ArpVoice& arp) const;
  std::uint16_t ArpSwingLocked(std::uint64_t gridPulse, std::uint16_t period) const;
  // Candidate `index` of the pattern: held note index % count, raised by index / count octaves.
  bool ArpCandidateLocked(const ArpVoice& arp, std::size_t index, std::uint8_t& note, std::uint8_t& velocity) const;
  bool NextArpNoteLocked(ArpVoice& arp, std::uint8_t& note, std::uint8_t& velocity);
  void ResetArpOrderLocked(ArpVoice& arp);
  void RemoveArpKeyLocked(ArpVoice& arp, std::size_t index);
  // Plays the next arp note at arp.pulse; a still sounding arp note ends first.
  void FireArpNoteLocked(ArpVoice& arp, std::array<PendingMidiEvent, kMaxPendingMidiEvents>& events, std::size_t& count);
  void CutArpNoteLocked(ArpVoice& arp, std::array<PendingMidiEvent, kMaxPendingMidiEvents>& events, std::size_t& count);
  void ReleaseArpLocked(ArpVoice& arp, std::array<PendingMidiEvent, kMaxPendingMidiEvents>& events, std::size_t& count);
  void ReleaseAllArpsLocked(std::array<PendingMidiEvent, kMaxPendingMidiEvents>& events, std::size_t& count);
  // Schedules the next arp note on the first transport grid line at or after transportPulse
  // (which arp.pulse stands for) that lies at least half a period after the last arp note.
  void AlignArpToTransportLocked(ArpVoice& arp, std::uint64_t transportPulse);
  void AlignArpsToTransportLocked(std::uint64_t transportPulse);
  void RescheduleArpLocked(ArpVoice& arp);
  void ProcessArpPulseLocked(ArpVoice& arp, std::uint64_t transportPulse,
                             std::array<PendingMidiEvent, kMaxPendingMidiEvents>& events,
                             std::size_t& count);

  mutable std::mutex mutex_;
  // Lock before mutex_: MIDI batches and transport changes must keep generation order.
  mutable std::mutex outputMutex_;
  std::array<TrackData, kSynthTrackCount> tracks_{};
  std::array<bool, kSequencerMaxSteps> heldSteps_{};
  // The page of each track's pattern the UI state's steps come from (see ViewPageLocked).
  mutable std::array<std::uint8_t, kSynthTrackCount> viewPage_{};
  mutable std::array<std::uint8_t, kSynthTrackCount> viewPattern_{};
  std::array<bool, kSynthTrackCount> viewFollow_{{true, true, true, true}};
  std::array<bool, kKeyboardMatrixRows> heldEditLanes_{};
  std::array<PlaybackNote, kMaxPlaybackNotes> playbackNotes_{};
  std::size_t activeTrack_ = 0;
  SequencerEditLane editLane_ = SequencerEditLane::Note;
  bool playing_ = false;
  std::size_t playbackStep_ = 0;
  std::uint64_t absolutePulse_ = 0;
  std::uint16_t bpm_ = 120;
  std::uint8_t swing_ = 50;
  bool recordArmed_ = false;
  // Per-track loop position in pulses: the next pulse to process, and the last processed one.
  std::array<std::uint16_t, kSynthTrackCount> trackNextPulse_{};
  std::array<std::uint16_t, kSynthTrackCount> trackCurrentPulse_{};
  std::array<bool, kSynthTrackCount> trackStarted_{};
  std::array<std::size_t, kSynthTrackCount> queuedPattern_{};
  // Pattern of the previous pass: swung notes of its last step spill past the loop wrap.
  std::array<std::size_t, kSynthTrackCount> previousPassPattern_{};
  // Pulses the previous pass ran: the loop length, or less when a song row cut it short.
  std::array<std::uint16_t, kSynthTrackCount> previousPassPulses_{};
  // Song arrangement and its playback position.
  std::array<SongRow, kSongMaxRows> song_{};
  std::size_t songLength_ = 0;
  bool songLoop_ = true;
  bool songMode_ = false;
  // True while playback follows the song; changes only on bar lines.
  bool songEngaged_ = false;
  std::size_t songRow_ = 0;
  std::size_t songBar_ = 0;
  std::size_t songStartRow_ = 0;
  bool songJumpPending_ = false;
  // May equal songLength_: the jump then lands past the end (loop to row 0, or stop).
  std::size_t songJumpRow_ = 0;
  std::array<bool, kSynthTrackCount> songSilent_{};
  std::array<HeldRecording, kMaxHeldRecordings> heldRecordings_{};
  std::array<SuppressedNote, kMaxSuppressedNotes> suppressedNotes_{};
  bool auditionActive_ = false;
  std::size_t auditionTrack_ = 0;
  std::uint8_t auditionNote_ = 0;
  // Arpeggiator state, one voice per track. Keys are the held (or latched) notes in press order.
  struct ArpKey {
    std::uint8_t note = 0;
    std::uint8_t velocity = 0;
    bool down = false;  // still held; a latched note is kept with down == false
  };
  struct ArpVoice {
    ArpSettings settings{};
    std::array<ArpKey, kArpMaxNotes> keys{};
    std::size_t keyCount = 0;
    // Arp clock: counts the pulses processed while the arp is in use; during playback it advances
    // with absolutePulse_. pulse is the next pulse to process.
    std::uint64_t pulse = 0;
    std::uint64_t nextPulse = 0;
    std::uint64_t lastOnPulse = 0;
    bool hasLastOn = false;
    int soundingNote = -1;
    std::uint64_t offPulse = 0;
    // Pattern position: the last pitch (UP, DOWN, UP-DN, RANDOM) and the next index (ORDER).
    int lastNote = -1;
    bool descending = false;
    std::size_t orderIndex = 0;
    std::uint32_t random = 0x9E3779B9u;
    std::uint32_t tick = 0;
    int lastPlayed = -1;
  };
  std::array<ArpVoice, kSynthTrackCount> arps_{};
  // When an arp started from idle, the time its first pulse belongs to (0 = none).
  std::uint64_t clockStartMicros_ = 0;
  MidiSink midiSink_ = nullptr;
  void* midiSinkContext_ = nullptr;
  std::atomic<bool> clockWorkerRunning_{false};
  std::atomic<std::uint32_t> clockPulses_{0};
  std::atomic<std::uint32_t> clockLatestMicros_{0};
  std::atomic<std::uint32_t> clockLatePulses_{0};
  std::atomic<bool> clockWorkerExited_{true};

#if defined(ESP_PLATFORM)
  void* clockTimer_ = nullptr;
#else
  std::thread clockThread_;
#endif
#if defined(__EMSCRIPTEN__)
  // ClockLoop's locals, kept between PumpClock() calls.
  std::uint64_t pumpNextPulseMicros_ = 0;
  bool pumpIdle_ = true;
#endif
};

}  // namespace synth
