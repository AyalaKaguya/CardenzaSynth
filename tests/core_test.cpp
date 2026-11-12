#include "Applications/Synth/Runtime/ProjectStorage.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cmath>
#include <iterator>
#include <string>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <thread>
#include <vector>

namespace {
std::filesystem::path projectDirectory;
struct Output {
  std::mutex mutex;
  std::condition_variable changed;
  std::vector<midi::MidiPacket> packets;
  std::vector<std::size_t> tracks;
  bool blockNoteOn = false;
  bool noteEntered = false;
};
void Sink(void* context, std::size_t track, const midi::MidiPacket& packet) {
  auto& output = *static_cast<Output*>(context);
  std::unique_lock<std::mutex> lock(output.mutex);
  if (packet.status == midi::MidiStatus::NoteOn) {
    output.noteEntered = true;
    output.changed.notify_all();
    output.changed.wait(lock, [&] { return !output.blockNoteOn; });
  }
  output.packets.push_back(packet);
  output.tracks.push_back(track);
}

void TestSequencerEditing() {
  using namespace synth;
  Output output;
  auto sequencer = std::make_unique<SynthSequencer>();
  sequencer->SetMidiSink(Sink, &output);
  // Duplicate keyboard pitches form one chord tone, even in non-pitch input order.
  const std::uint8_t chord[] = {67, 60, 64, 60, 255};
  sequencer->PressStep(0, chord, std::size(chord));
  auto ui = sequencer->BuildUiState(0);
  assert(ui.noteCount == 3 && ui.noteNames == "C4 E4 G4");
  // Grid labels use the lowest active pitch, independent of entry order.
  assert(ui.steps[0].lowestNote == 60 && ui.steps[0].noteCount == 3 && ui.steps[0].hasEvents);
  assert(ui.steps[1].lowestNote == -1 && ui.steps[1].noteCount == 0 && !ui.steps[1].hasEvents);
  assert(sequencer->BuildUiState(1).steps[0].lowestNote == -1);
  assert(ui.heldStepCount == 1 && ui.primaryHeldStep == 0);
  assert(ui.populatedStepCount == 1 && ui.patternHasEvents[0] && !ui.patternHasEvents[1]);
  assert(ui.tracks[0].populatedStepCount == 1 && ui.tracks[1].populatedStepCount == 0);

  assert(sequencer->PressEditLane(1));
  assert(sequencer->HandlePerformanceKeyPress(0, 8, 0)); // Sixteen-step length.
  sequencer->ReleaseEditLane(1);
  assert(sequencer->PressEditLane(3));
  assert(sequencer->HandlePerformanceKeyPress(0, 6, 0)); // Last microstep.
  sequencer->ClearHeldSteps();
  assert(sequencer->CopyStep(0, kSequencerStepCount - 1));
  const auto copiedGrid = sequencer->BuildUiState(0);
  assert(copiedGrid.steps.back().lowestNote == 60 && copiedGrid.steps.back().noteCount == 3);
  assert(!copiedGrid.steps[0].held && copiedGrid.steps[0].lowestNote == 60);
  auto state = sequencer->CaptureState();
  assert(state && SynthSequencer::ValidateState(*state));
  for (const auto& note : state->tracks[0].patterns[0].steps[kSequencerStepCount - 1].notes) {
    if (note.active) assert(note.microstep == 5 && note.lengthPulses == 4);
  }
  assert(state->tracks[0].patterns[0].steps[0].notes[0].lengthPulses == 364);

  sequencer->Reset();
  const std::uint8_t otherChord[] = {76, 72};
  const std::uint8_t untouched = 84;
  sequencer->PressStep(10, &untouched, 1);
  assert(sequencer->PressEditLane(3));
  assert(sequencer->HandlePerformanceKeyPress(0, 6, 0));
  sequencer->ClearHeldSteps();
  sequencer->PressStep(2, chord, std::size(chord));
  sequencer->PressStep(6, otherChord, std::size(otherChord));
  ui = sequencer->BuildUiState(0);
  assert(ui.heldStepCount == 2 && ui.primaryHeldStep == 2 && ui.populatedStepCount == 3);
  // The lowest note of each chord edited alone (a held pad's notes, AdjustNotesOnSelectedSteps).
  const std::uint8_t lowest[] = {60, 72};
  assert(sequencer->PressEditLane(2));
  assert(sequencer->AdjustNotesOnSelectedSteps(lowest, std::size(lowest), 17, false));
  state = sequencer->CaptureState();
  for (std::size_t step : {std::size_t{2}, std::size_t{6}}) {
    for (const auto& note : state->tracks[0].patterns[0].steps[step].notes) {
      if (note.active) assert(note.velocity == (note.note == 60 || note.note == 72 ? 127 : 110));
    }
  }
  assert(sequencer->PressEditLane(1) && sequencer->AdjustNotesOnSelectedSteps(lowest, std::size(lowest), 1, false));
  assert(sequencer->PressEditLane(3) && sequencer->AdjustNotesOnSelectedSteps(lowest, std::size(lowest), 1, false));
  // Notes edited apart show mixed values rather than claim one chord-wide value.
  sequencer->ClearHeldSteps();
  sequencer->PressStep(2, nullptr, 0);
  sequencer->PressStep(6, nullptr, 0);
  assert(sequencer->PressEditLane(1) && sequencer->BuildUiState(0).editValueMixed);
  assert(sequencer->PressEditLane(3) && sequencer->BuildUiState(0).editValueMixed);
  assert(sequencer->PressEditLane(2));
  ui = sequencer->BuildUiState(0);
  assert(ui.editValueMixed && ui.valueLabel == "VEL MIXED");
  assert(sequencer->HandlePerformanceKeyPress(0, 4, 0));
  assert(!sequencer->BuildUiState(0).editValueMixed);
  assert(sequencer->PressEditLane(1));
  assert(sequencer->HandlePerformanceKeyPress(0, 2, 0));
  assert(sequencer->PressEditLane(3));
  assert(sequencer->HandlePerformanceKeyPress(0, 4, 0));
  sequencer->QuantizeHeldSteps();
  state = sequencer->CaptureState();
  for (std::size_t step : {std::size_t{2}, std::size_t{6}}) {
    for (const auto& note : state->tracks[0].patterns[0].steps[step].notes) {
      if (note.active) assert(note.microstep == 0 && note.lengthPulses == 48 && note.velocity == 72);
    }
  }
  assert(state->tracks[0].patterns[0].steps[10].notes[0].microstep == 5);
  sequencer->ClearHeldStepEvents();
  assert(!sequencer->StepHasEvents(2) && !sequencer->StepHasEvents(6) && sequencer->StepHasEvents(10));
  sequencer->QuantizeActivePattern();
  state = sequencer->CaptureState();
  assert(state->tracks[0].patterns[0].steps[10].notes[0].microstep == 0);
  assert(SynthSequencer::ValidateState(*state));

  // An edit selection belongs to the track where it was started.
  sequencer->ClearHeldSteps();
  sequencer->PressStep(10, nullptr, 0);
  assert(sequencer->PressEditLane(0));
  sequencer->SetActiveTrack(1);
  assert(!sequencer->HasHeldSteps() && sequencer->BuildUiState(1).editLane == SequencerEditLane::Note);
  assert(!sequencer->HandlePerformanceKeyPress(0, 1, 61));
  assert(!sequencer->StepHasEvents(10));
  sequencer->SetActiveTrack(0);
  assert(sequencer->StepHasEvents(10));

  // Deleting before the selected pattern preserves the selected musical content.
  sequencer->Reset();
  for (std::size_t pattern = 0; pattern < 3; ++pattern) {
    assert(sequencer->SelectOrCreatePattern(pattern));
    const auto pitch = static_cast<std::uint8_t>(60 + pattern);
    sequencer->PressStep(0, &pitch, 1);
    sequencer->ReleaseStep(0);
  }
  assert(sequencer->SelectOrCreatePattern(1) && sequencer->DeletePattern(0));
  state = sequencer->CaptureState();
  assert(state->tracks[0].activePattern == 0 && state->tracks[0].patternCount == 2);
  assert(state->tracks[0].patterns[0].steps[0].notes[0].note == 61);
  assert(sequencer->CopyPattern(0, 2));
  assert(sequencer->BuildUiState(0).tracks[0].activePattern == 2);
  assert(sequencer->DeletePattern(1));
  assert(sequencer->BuildUiState(0).activePattern == 1);
  sequencer->ToggleTrackMuted();
  sequencer->ToggleTrackEnabled();
  ui = sequencer->BuildUiState(0);
  assert(ui.tracks[0].muted && !ui.tracks[0].enabled && ui.tracks[0].patternCount == 2);
  sequencer->PressStep(0, nullptr, 0);
  assert(sequencer->PressEditLane(0));
  sequencer->Stop();
  assert(output.packets.back().status == midi::MidiStatus::ControlChange);
  const auto stoppedCount = output.packets.size();
  sequencer->ReleaseEditLane(0);
  assert(output.packets.size() == stoppedCount); // A lane key sounds nothing.
  // Removing a filled trailing pattern and adding a new slot must not resurrect notes.
  assert(sequencer->PatternHasEvents(1));
  sequencer->AdjustPatternCount(-1);
  sequencer->AdjustPatternCount(1);
  assert(sequencer->SelectOrCreatePattern(1) && !sequencer->PatternHasEvents(1));
  assert(sequencer->PatternHasEvents(0));
}

// Logs every packet with the loop position of the pulse that produced it, and can freeze the
// clock thread inside the sink at one armed event. While frozen the sequencer sits exactly on
// that pulse, so mutex-only calls (recording, queueing, UI state) act at a known position.
struct PulseProbe {
  struct Event {
    std::size_t track = 0;
    midi::MidiStatus status = midi::MidiStatus::None;
    std::uint8_t note = 0;
    std::uint8_t velocity = 0;
    int position = -1;      // In the packet track's own loop.
    int barPosition = -1;   // In track 0's loop (16 steps in these tests).
    int songRow = -1;       // Song row and bar while playback follows the song, else -1.
    int songBar = -1;
  };
  synth::SynthSequencer* sequencer = nullptr;
  std::mutex mutex;
  std::condition_variable changed;
  std::vector<Event> events;
  bool armed = false;
  std::size_t armTrack = 0;
  midi::MidiStatus armStatus = midi::MidiStatus::NoteOn;
  std::uint8_t armNote = 0;
  bool frozen = false;
  Event frozenAt;
};

int ProcessedPosition(const synth::SequencerUiState& ui, std::size_t track) {
  if (!ui.playing) return -1;
  const int loop = static_cast<int>(ui.tracks[track].stepCount * synth::kSequencerPulsesPerStep);
  // patternProgress reports the next pulse; the packet came from the one before it.
  return (static_cast<int>(std::lround(ui.patternProgress * static_cast<float>(loop))) + loop - 1) % loop;
}

void ProbeSink(void* context, std::size_t track, const midi::MidiPacket& packet) {
  auto& probe = *static_cast<PulseProbe*>(context);
  PulseProbe::Event event{track, packet.status, packet.data[1], packet.data[2], -1, -1};
  if (probe.sequencer != nullptr && (packet.status == midi::MidiStatus::NoteOn || packet.status == midi::MidiStatus::NoteOff)) {
    event.position = ProcessedPosition(probe.sequencer->BuildUiState(track), track);
    const auto bar = probe.sequencer->BuildUiState(0);
    event.barPosition = ProcessedPosition(bar, 0);
    if (bar.songPlaying) {
      event.songRow = static_cast<int>(bar.songRow);
      event.songBar = static_cast<int>(bar.songBar);
    }
  }
  std::unique_lock<std::mutex> lock(probe.mutex);
  probe.events.push_back(event);
  if (probe.armed && track == probe.armTrack && packet.status == probe.armStatus && packet.data[1] == probe.armNote) {
    probe.armed = false;
    probe.frozen = true;
    probe.frozenAt = event;
    probe.changed.notify_all();
    probe.changed.wait(lock, [&] { return !probe.frozen; });
  }
}

void Arm(PulseProbe& probe, std::size_t track, midi::MidiStatus status, std::uint8_t note) {
  std::lock_guard<std::mutex> lock(probe.mutex);
  probe.armed = true;
  probe.armTrack = track;
  probe.armStatus = status;
  probe.armNote = note;
}

PulseProbe::Event WaitFrozen(PulseProbe& probe) {
  using namespace std::chrono_literals;
  std::unique_lock<std::mutex> lock(probe.mutex);
  assert(probe.changed.wait_for(lock, 10s, [&] { return probe.frozen; }));
  return probe.frozenAt;
}

void Release(PulseProbe& probe) {
  {
    std::lock_guard<std::mutex> lock(probe.mutex);
    probe.frozen = false;
  }
  probe.changed.notify_all();
}

struct Hit {
  int pass = 0;
  int position = -1;
  int barPosition = -1;
  std::uint8_t note = 0;
  std::uint8_t velocity = 0;
};

// Note events of one track; passes are counted from wraps of track 0's 16-step bar.
std::vector<Hit> Hits(PulseProbe& probe, std::size_t track, midi::MidiStatus status) {
  std::lock_guard<std::mutex> lock(probe.mutex);
  std::vector<Hit> hits;
  int pass = 1;
  int last = -1;
  for (const auto& event : probe.events) {
    if (event.barPosition < 0) continue;
    if (event.barPosition < last) ++pass;
    last = event.barPosition;
    if (event.track == track && event.status == status) hits.push_back({pass, event.position, event.barPosition, event.note, event.velocity});
  }
  return hits;
}

std::size_t CountHits(const std::vector<Hit>& hits, std::uint8_t note, int pass = 0, int position = -1) {
  return static_cast<std::size_t>(std::count_if(hits.begin(), hits.end(), [&](const Hit& hit) {
    return hit.note == note && (pass == 0 || hit.pass == pass) && (position < 0 || hit.position == position);
  }));
}

void SetNote(synth::SynthSequencer::ProjectState& state, std::size_t track, std::size_t pattern, std::size_t step,
             std::uint8_t pitch, std::uint8_t velocity, std::uint16_t length, std::uint8_t microstep) {
  auto& notes = state.tracks[track].patterns[pattern].steps.Edit(step).notes;
  for (auto& note : notes) {
    if (!note.active || note.note == pitch) {
      note = {pitch, velocity, length, microstep, true};
      return;
    }
  }
  assert(false && "step is full");
}

const synth::SynthSequencer::NoteEvent* FindNote(const synth::SynthSequencer::ProjectState& state, std::size_t track,
                                                 std::size_t pattern, std::size_t step, std::uint8_t pitch) {
  for (const auto& note : state.tracks[track].patterns[pattern].steps[step].notes)
    if (note.active && note.note == pitch) return &note;
  return nullptr;
}

std::vector<int> StepPitches(const synth::SynthSequencer& sequencer, std::size_t step, std::size_t track = 0) {
  const auto state = sequencer.CaptureState();
  const auto& source = state->tracks[track];
  std::vector<int> pitches;
  for (const auto& note : source.patterns[source.activePattern].steps[step].notes)
    if (note.active) pitches.push_back(note.note);
  std::sort(pitches.begin(), pitches.end());
  return pitches;
}

template <typename Field>
std::vector<int> StepValues(const synth::SynthSequencer& sequencer, std::size_t step, Field field) {
  const auto state = sequencer.CaptureState();
  const auto& source = state->tracks[0];
  std::vector<std::pair<int, int>> notes;
  for (const auto& note : source.patterns[source.activePattern].steps[step].notes)
    if (note.active) notes.emplace_back(note.note, static_cast<int>(field(note)));
  std::sort(notes.begin(), notes.end());
  std::vector<int> values;
  for (const auto& note : notes) values.push_back(note.second);
  return values;
}

void TestLiveRecording() {
  using namespace synth;
  using midi::MidiStatus;
  PulseProbe probe;  // Outlives the sequencer's final stop callback.
  auto sequencer = std::make_unique<SynthSequencer>();
  probe.sequencer = sequencer.get();
  sequencer->SetMidiSink(ProbeSink, &probe);

  auto state = sequencer->CaptureState();
  state->bpm = 300;
  // Track 3 markers: note-offs at chosen pulses give freeze points between microstep slots.
  const std::array<std::pair<std::uint8_t, std::uint16_t>, 6> markers{{{100, 53}, {101, 54}, {102, 90}, {103, 120}, {104, 300}, {105, 382}}};
  for (const auto& marker : markers) SetNote(*state, 3, 0, 0, marker.first, 64, marker.second, 0);
  SetNote(*state, 0, 0, 2, 60, 50, 24, 0);  // Overdub replaces this pitch.
  for (std::uint8_t pitch = 40; pitch < 48; ++pitch) SetNote(*state, 0, 0, 5, pitch, 64, 24, 0);  // A full step.
  state->tracks[1].patternSteps.fill(4);
  assert(sequencer->RestoreState(*state));

  sequencer->SetRecordArmed(true);
  assert(sequencer->RecordArmed() && sequencer->BuildUiState(0).recordArmed);
  sequencer->RecordNoteOn(0, 73, 100);  // Stopped: ignored.
  Arm(probe, 3, MidiStatus::NoteOff, 100);
  sequencer->Play();

  // Pulse 53 rounds back to step 2 microstep 1 (pulse 52); playback already passed it.
  assert(WaitFrozen(probe).position == 53);
  sequencer->RecordNoteOn(0, 60, 100);
  Arm(probe, 3, MidiStatus::NoteOff, 101);
  Release(probe);

  // Pulse 54 is a tie between microsteps 1 and 2: it rounds up, just ahead of the playhead.
  assert(WaitFrozen(probe).position == 54);
  sequencer->RecordNoteOn(0, 62, 90);
  Arm(probe, 3, MidiStatus::NoteOff, 102);
  Release(probe);

  assert(WaitFrozen(probe).position == 90);
  sequencer->RecordNoteOff(0, 60);
  sequencer->RecordNoteOff(0, 62);
  state = sequencer->CaptureState();
  const auto* replaced = FindNote(*state, 0, 0, 2, 60);
  assert(replaced && replaced->velocity == 100 && replaced->microstep == 1 && replaced->lengthPulses == 37);
  const auto* ahead = FindNote(*state, 0, 0, 2, 62);
  assert(ahead && ahead->velocity == 90 && ahead->microstep == 2 && ahead->lengthPulses == 36);
  assert(StepPitches(*sequencer, 2) == (std::vector<int>{60, 62}));
  Arm(probe, 3, MidiStatus::NoteOff, 103);
  Release(probe);

  assert(WaitFrozen(probe).position == 120);
  sequencer->RecordNoteOn(0, 70, 100);  // Step 5 already holds eight notes.
  sequencer->RecordNoteOff(0, 70);
  assert(StepPitches(*sequencer, 5).size() == 8);
  sequencer->SetRecordArmed(false);
  assert(!sequencer->BuildUiState(0).recordArmed);
  sequencer->RecordNoteOn(0, 72, 100);  // Disarmed: ignored.
  sequencer->RecordNoteOff(0, 72);
  sequencer->SetRecordArmed(true);
  Arm(probe, 3, MidiStatus::NoteOff, 104);
  Release(probe);

  // Held over the wrap: finalized at the loop end.
  assert(WaitFrozen(probe).position == 300);
  sequencer->RecordNoteOn(0, 67, 100);
  Arm(probe, 3, MidiStatus::NoteOff, 105);
  Release(probe);

  // Pulse 382 rounds past the last microstep into step 0 of the next pass; on the four-step
  // track 1 (position 94) it wraps to that track's own step 0.
  assert(WaitFrozen(probe).position == 382);
  sequencer->RecordNoteOn(0, 64, 80);
  sequencer->RecordNoteOn(1, 50, 100);
  Arm(probe, 3, MidiStatus::NoteOff, 100);
  Release(probe);

  const auto secondPass = WaitFrozen(probe);
  assert(secondPass.position == 53);
  sequencer->RecordNoteOff(0, 64);
  sequencer->RecordNoteOff(1, 50);
  sequencer->RecordNoteOff(0, 67);  // Already ended at the wrap.
  state = sequencer->CaptureState();
  const auto* held = FindNote(*state, 0, 0, 12, 67);
  assert(held && held->microstep == 3 && held->lengthPulses == 84);
  const auto* wrapped = FindNote(*state, 0, 0, 0, 64);
  assert(wrapped && wrapped->microstep == 0 && wrapped->velocity == 80 && wrapped->lengthPulses == 55);
  const auto* shortLoop = FindNote(*state, 1, 0, 0, 50);
  assert(shortLoop && shortLoop->microstep == 0 && shortLoop->lengthPulses == 55);
  assert(SynthSequencer::ValidateState(*state));
  Arm(probe, 0, MidiStatus::NoteOn, 64);
  Release(probe);

  const auto thirdPass = WaitFrozen(probe);
  assert(thirdPass.position == 0);
  Release(probe);
  sequencer->Stop();
  sequencer->SetMidiSink(nullptr, nullptr);

  const auto ons = Hits(probe, 0, MidiStatus::NoteOn);
  // Pass 1 only plays what was there before; live takes never sound from the sequencer.
  assert(CountHits(ons, 60, 1) == 1 && CountHits(ons, 60, 1, 48) == 1);
  for (std::uint8_t note : {62, 64, 67, 70, 72, 73}) assert(CountHits(ons, note, 1) == 0);
  // Pass 2 plays the takes, except the one quantized just ahead of the pass-2 playhead.
  assert(CountHits(ons, 60, 2) == 1 && CountHits(ons, 60, 2, 52) == 1);
  assert(CountHits(ons, 62, 2) == 1 && CountHits(ons, 62, 2, 56) == 1);
  assert(CountHits(ons, 67, 2) == 1 && CountHits(ons, 67, 2, 300) == 1);
  assert(CountHits(ons, 64, 2) == 0 && CountHits(ons, 64, 3, 0) == 1);
  assert(CountHits(ons, 70) == 0 && CountHits(ons, 72) == 0 && CountHits(ons, 73) == 0);
  const auto velocity = std::find_if(ons.begin(), ons.end(), [](const Hit& hit) { return hit.pass == 2 && hit.note == 60; });
  assert(velocity != ons.end() && velocity->velocity == 100);
  // Track 1's note skips its next loop start (bar pulse 0) and plays one 96-pulse loop later.
  const auto trackOneOns = Hits(probe, 1, MidiStatus::NoteOn);
  assert(!trackOneOns.empty() && trackOneOns.front().note == 50 && trackOneOns.front().pass == 2 &&
         trackOneOns.front().barPosition == 96 && trackOneOns.front().position == 0);
  sequencer.reset();
}

void TestSwingPlayback() {
  using namespace synth;
  using midi::MidiStatus;
  PulseProbe probe;
  auto sequencer = std::make_unique<SynthSequencer>();
  probe.sequencer = sequencer.get();
  sequencer->SetMidiSink(ProbeSink, &probe);

  sequencer->SetSwing(10);
  assert(sequencer->Swing() == 50);
  sequencer->SetSwing(99);
  assert(sequencer->Swing() == 75);
  sequencer->SetSwing(66);
  assert(sequencer->Swing() == 66 && sequencer->BuildUiState(0).swing == 66);

  auto state = sequencer->CaptureState();
  assert(state->swing == 66 && SynthSequencer::ValidateState(*state));
  state->swing = 49;
  assert(!SynthSequencer::ValidateState(*state));
  state->swing = 75;  // Odd sixteenths start round(48 * 75%) - 24 = 12 pulses late.
  state->bpm = 300;
  SetNote(*state, 1, 0, 0, 50, 100, 24, 0);
  SetNote(*state, 1, 0, 1, 51, 100, 24, 0);
  SetNote(*state, 1, 0, 1, 52, 100, 24, 2);
  SetNote(*state, 1, 0, 2, 55, 100, 24, 1);
  SetNote(*state, 1, 0, 3, 53, 100, 6, 0);
  SetNote(*state, 1, 0, 15, 54, 100, 4, 5);  // 360 + 20 + 12 = 392: past the 384-pulse wrap.
  assert(sequencer->RestoreState(*state) && sequencer->Swing() == 75);

  Arm(probe, 1, MidiStatus::NoteOff, 54);
  sequencer->Play();
  const auto off = WaitFrozen(probe);
  Release(probe);
  sequencer->Stop();
  sequencer->SetMidiSink(nullptr, nullptr);

  const auto ons = Hits(probe, 1, MidiStatus::NoteOn);
  const auto offs = Hits(probe, 1, MidiStatus::NoteOff);
  assert(CountHits(ons, 50, 1, 0) == 1 && CountHits(ons, 51, 1, 36) == 1 && CountHits(ons, 52, 1, 44) == 1);
  assert(CountHits(ons, 55, 1, 49) == 0 && CountHits(ons, 55, 1, 52) == 1);  // Even steps stay straight.
  assert(CountHits(ons, 53, 1, 84) == 1 && CountHits(offs, 53, 1, 90) == 1);  // Length is kept.
  assert(CountHits(offs, 51, 1, 60) == 1 && CountHits(offs, 52, 1, 68) == 1);
  // The swung last step sounds after the wrap of the pass it belongs to, not in the first pass.
  assert(CountHits(ons, 54, 1) == 0 && CountHits(ons, 54, 2, 8) == 1);
  assert(off.position == 12 && CountHits(offs, 54, 2, 12) == 1);
  sequencer.reset();
}

void TestQueuedPatterns() {
  using namespace synth;
  using midi::MidiStatus;
  PulseProbe probe;
  auto sequencer = std::make_unique<SynthSequencer>();
  probe.sequencer = sequencer.get();
  sequencer->SetMidiSink(ProbeSink, &probe);

  // Stopped: the switch (and creation) is immediate and leaves the active track alone.
  assert(sequencer->QueuePattern(1, 1));
  assert(sequencer->PatternCount(1) == 2 && sequencer->ActivePattern(1) == 1);
  auto ui = sequencer->BuildUiState(0);
  assert(ui.tracks[1].activePattern == 1 && ui.tracks[1].queuedPattern == kSequencerMaxPatternCount && ui.activePattern == 0);
  assert(!sequencer->QueuePattern(1, 3) && !sequencer->QueuePattern(4, 0) && !sequencer->QueuePattern(1, kSequencerMaxPatternCount));
  assert(sequencer->QueuePattern(1, 0) && sequencer->ActivePattern(1) == 0 && sequencer->PatternCount(1) == 2);

  auto state = sequencer->CaptureState();
  state->bpm = 300;
  state->tracks[2].patternSteps.fill(4);  // A 96-pulse loop.
  state->tracks[2].patternCount = 2;
  SetNote(*state, 2, 0, 0, 80, 100, 6, 0);
  SetNote(*state, 2, 1, 0, 81, 100, 6, 0);
  assert(sequencer->RestoreState(*state));

  Arm(probe, 2, MidiStatus::NoteOn, 80);
  sequencer->Play();
  assert(WaitFrozen(probe).barPosition == 0);
  assert(sequencer->QueuePattern(2, 1) && sequencer->ActivePattern(2) == 0);
  assert(sequencer->BuildUiState(0).tracks[2].queuedPattern == 1);
  assert(sequencer->QueuePattern(2, 0));  // Queueing the playing pattern cancels.
  assert(sequencer->BuildUiState(0).tracks[2].queuedPattern == kSequencerMaxPatternCount);
  assert(sequencer->QueuePattern(2, 2) && sequencer->PatternCount(2) == 3);  // Created now, played later.
  assert(sequencer->BuildUiState(0).tracks[2].queuedPattern == 2 && sequencer->ActivePattern(2) == 0);
  assert(sequencer->QueuePattern(2, 1));
  Arm(probe, 2, MidiStatus::NoteOn, 81);
  Release(probe);

  const auto switched = WaitFrozen(probe);
  assert(switched.position == 0 && switched.barPosition == 96);
  assert(sequencer->ActivePattern(2) == 1);
  ui = sequencer->BuildUiState(0);
  assert(ui.tracks[2].activePattern == 1 && ui.tracks[2].queuedPattern == kSequencerMaxPatternCount);
  Release(probe);
  const auto ons = Hits(probe, 2, MidiStatus::NoteOn);
  assert(CountHits(ons, 80) == 1 && ons.front().barPosition == 0);

  // Stopping hands a pending switch over immediately, whichever comes first.
  assert(sequencer->QueuePattern(2, 0));
  sequencer->Stop();
  assert(sequencer->ActivePattern(2) == 0 && sequencer->BuildUiState(0).tracks[2].queuedPattern == kSequencerMaxPatternCount);
  sequencer->SetMidiSink(nullptr, nullptr);
  sequencer.reset();
}

void TestTrackStepCounts() {
  using namespace synth;
  using midi::MidiStatus;
  PulseProbe probe;
  auto sequencer = std::make_unique<SynthSequencer>();
  probe.sequencer = sequencer.get();
  sequencer->SetMidiSink(ProbeSink, &probe);

  sequencer->SetTrackStepCount(1, 0);
  assert(sequencer->TrackStepCount(1) == 1);
  sequencer->SetTrackStepCount(1, 400);
  assert(sequencer->TrackStepCount(1) == kSequencerMaxSteps);
  sequencer->SetTrackStepCount(1, 3);
  assert(sequencer->TrackStepCount(1) == 3 && sequencer->TrackStepCount(0) == 16 && sequencer->TrackStepCount(9) == 16);
  assert(sequencer->BuildUiState(0).tracks[1].stepCount == 3);

  auto state = sequencer->CaptureState();
  assert(state->tracks[1].patternSteps[0] == 3 && SynthSequencer::ValidateState(*state));
  state->tracks[1].patternSteps.fill(0);
  assert(!SynthSequencer::ValidateState(*state));
  state->tracks[1].patternSteps.fill(257);
  assert(!SynthSequencer::ValidateState(*state));
  state->tracks[1].patternSteps.fill(3);
  state->bpm = 300;
  SetNote(*state, 1, 0, 0, 40, 100, 1, 0);
  SetNote(*state, 1, 0, 5, 41, 100, 1, 0);  // Beyond the loop: kept, never played.
  SetNote(*state, 0, 0, 15, 31, 100, 1, 5);
  assert(sequencer->RestoreState(*state) && sequencer->TrackStepCount(1) == 3);

  Arm(probe, 1, MidiStatus::NoteOn, 40);
  sequencer->Play();
  assert(WaitFrozen(probe).barPosition == 0);
  Arm(probe, 1, MidiStatus::NoteOn, 40);
  Release(probe);

  const auto second = WaitFrozen(probe);
  assert(second.position == 0 && second.barPosition == 72);
  auto own = sequencer->BuildUiState(1);
  assert(own.currentStep == 0 && own.steps[0].playing && !own.steps[3].playing);
  assert(std::lround(own.patternProgress * 72.0f) == 1);
  auto ui = sequencer->BuildUiState(0);
  assert(ui.currentStep == 3 && ui.steps[3].playing && ui.tracks[0].currentStep == 3 && ui.tracks[0].stepCount == 16);
  assert(ui.tracks[1].currentStep == 0 && ui.tracks[1].stepCount == 3);
  Arm(probe, 0, MidiStatus::NoteOn, 31);
  Release(probe);

  assert(WaitFrozen(probe).barPosition == 380);
  Release(probe);
  sequencer->Stop();
  sequencer->SetMidiSink(nullptr, nullptr);
  const auto ons = Hits(probe, 1, MidiStatus::NoteOn);
  assert(CountHits(ons, 41) == 0);
  std::vector<int> starts;
  for (const auto& hit : ons)
    if (hit.note == 40 && hit.pass == 1) starts.push_back(hit.barPosition);
  assert(starts == (std::vector<int>{0, 72, 144, 216, 288, 360}));
  state = sequencer->CaptureState();
  assert(state->tracks[1].patternSteps[0] == 3 && FindNote(*state, 1, 0, 5, 41) != nullptr);
  sequencer->Reset();
  assert(sequencer->TrackStepCount(1) == 16 && sequencer->Swing() == 50);
  sequencer.reset();
}

void TestSelectionEditing() {
  using namespace synth;
  Output output;
  auto sequencer = std::make_unique<SynthSequencer>();
  sequencer->SetMidiSink(Sink, &output);
  auto restore = [&](auto edit) {
    auto state = sequencer->CaptureState();
    state->tracks[0].patterns[0].steps.Clear();
    edit(*state);
    assert(sequencer->RestoreState(*state));
  };
  auto velocities = [&](std::size_t step) { return StepValues(*sequencer, step, [](const auto& note) { return note.velocity; }); };
  auto lengths = [&](std::size_t step) { return StepValues(*sequencer, step, [](const auto& note) { return note.lengthPulses; }); };
  auto microsteps = [&](std::size_t step) { return StepValues(*sequencer, step, [](const auto& note) { return note.microstep; }); };
  using Values = std::vector<int>;

  const std::uint8_t chord[] = {60, 64, 67};
  const std::uint8_t single = 62;
  sequencer->PressStep(0, chord, std::size(chord));
  sequencer->ReleaseStep(0);
  sequencer->PressStep(3, &single, 1);
  sequencer->ReleaseStep(3);
  {
    std::lock_guard<std::mutex> lock(output.mutex);
    output.packets.clear();
  }

  assert(!sequencer->AdjustSelectedSteps(1, false) && !sequencer->ToggleNoteOnSelectedSteps(72));
  sequencer->SetStepSelected(0, true);
  sequencer->SetStepSelected(3, true);
  assert(sequencer->IsStepHeld(0) && sequencer->IsStepHeld(3) && sequencer->BuildUiState(0).heldStepCount == 2);

  // Note lane: every note moves by semitones, coarse by octaves.
  assert(sequencer->AdjustSelectedSteps(1, false));
  assert(StepPitches(*sequencer, 0) == (Values{61, 65, 68}) && StepPitches(*sequencer, 3) == (Values{63}));
  assert(sequencer->AdjustSelectedSteps(-1, true));
  assert(StepPitches(*sequencer, 0) == (Values{49, 53, 56}) && StepPitches(*sequencer, 3) == (Values{51}));
  assert(!sequencer->AdjustSelectedSteps(0, true));
  assert(sequencer->ToggleNoteOnSelectedSteps(72));
  assert(StepPitches(*sequencer, 0) == (Values{49, 53, 56, 72}) && StepPitches(*sequencer, 3) == (Values{51, 72}));
  assert(sequencer->ToggleNoteOnSelectedSteps(72));
  assert(StepPitches(*sequencer, 0) == (Values{49, 53, 56}) && StepPitches(*sequencer, 3) == (Values{51}));

  auto ui = sequencer->BuildUiState(0);

  // Lane cycling.
  assert(sequencer->BuildUiState(0).editLane == SequencerEditLane::Note);
  sequencer->CycleEditLane();
  assert(sequencer->BuildUiState(0).editLane == SequencerEditLane::Length);
  sequencer->CycleEditLane();
  assert(sequencer->BuildUiState(0).editLane == SequencerEditLane::Velocity);
  sequencer->CycleEditLane();
  assert(sequencer->BuildUiState(0).editLane == SequencerEditLane::Micro);
  sequencer->CycleEditLane();
  assert(sequencer->BuildUiState(0).editLane == SequencerEditLane::Hits);
  sequencer->CycleEditLane();
  assert(sequencer->BuildUiState(0).editLane == SequencerEditLane::Note);

  // Length lane walks the length table; coarse moves three entries.
  sequencer->SetEditLane(SequencerEditLane::Length);
  assert(sequencer->AdjustSelectedSteps(1, false));
  assert(lengths(0) == (Values{36, 36, 36}) && lengths(3) == (Values{36}));
  assert(sequencer->AdjustSelectedSteps(1, true));
  assert(lengths(0) == (Values{96, 96, 96}) && sequencer->BuildUiState(0).valueLabel == "LEN 4");
  assert(sequencer->AdjustSelectedSteps(-1, false));
  assert(lengths(0) == (Values{72, 72, 72}));
  sequencer->SetStepSelected(0, false);
  assert(sequencer->AdjustSelectedSteps(1, false) && lengths(3) == (Values{96}));
  sequencer->SetStepSelected(0, true);
  ui = sequencer->BuildUiState(0);
  assert(ui.editValueMixed && ui.valueLabel == "LEN MIXED");
  assert(sequencer->AdjustSelectedSteps(-1, false));  // Relative to each note.
  assert(lengths(0) == (Values{48, 48, 48}) && lengths(3) == (Values{72}) && sequencer->BuildUiState(0).editValueMixed);

  // Velocity lane: +-1, coarse +-10, clamped to 1..127, relative across mixed values.
  sequencer->SetEditLane(SequencerEditLane::Velocity);
  assert(sequencer->AdjustSelectedSteps(1, false));
  assert(velocities(0) == (Values{111, 111, 111}) && velocities(3) == (Values{111}));
  ui = sequencer->BuildUiState(0);
  assert(!ui.editValueMixed && ui.valueLabel == "VEL 111");
  assert(sequencer->AdjustSelectedSteps(1, true) && velocities(0) == (Values{121, 121, 121}));
  assert(sequencer->AdjustSelectedSteps(1, true) && velocities(0) == (Values{127, 127, 127}));
  assert(!sequencer->AdjustSelectedSteps(1, false));
  assert(sequencer->AdjustSelectedSteps(-20, true) && velocities(0) == (Values{1, 1, 1}) && velocities(3) == (Values{1}));
  assert(!sequencer->AdjustSelectedSteps(-1, false));
  sequencer->SetStepSelected(3, false);
  assert(sequencer->AdjustSelectedSteps(5, false));
  sequencer->SetStepSelected(3, true);
  ui = sequencer->BuildUiState(0);
  assert(ui.editValueMixed && ui.valueLabel == "VEL MIXED");
  assert(sequencer->AdjustSelectedSteps(1, false) && velocities(0) == (Values{7, 7, 7}) && velocities(3) == (Values{2}));

  // Micro lane: +-1 within 0..5; coarse jumps to the edge.
  sequencer->SetEditLane(SequencerEditLane::Micro);
  assert(sequencer->AdjustSelectedSteps(1, false));
  assert(microsteps(0) == (Values{1, 1, 1}) && sequencer->BuildUiState(0).valueLabel == "TIME 2");
  assert(sequencer->AdjustSelectedSteps(1, true) && microsteps(0) == (Values{5, 5, 5}) && microsteps(3) == (Values{5}));
  assert(!sequencer->AdjustSelectedSteps(1, false));
  assert(sequencer->AdjustSelectedSteps(-1, true) && microsteps(0) == (Values{0, 0, 0}));

  // None of the selection edits auditioned anything.
  {
    std::lock_guard<std::mutex> lock(output.mutex);
    assert(output.packets.empty());
  }

  sequencer->ClearSelectedSteps();
  ui = sequencer->BuildUiState(0);
  assert(!ui.hasHeldSteps && ui.editLane == SequencerEditLane::Note);

  // Per-step clamping: microsteps shorten a note at the pattern end, lengths stop at the end,
  // and the table ends do not move.
  restore([](SynthSequencer::ProjectState& state) {
    SetNote(state, 0, 0, 15, 60, 100, 24, 0);
    SetNote(state, 0, 0, 0, 61, 100, 384, 0);
    SetNote(state, 0, 0, 1, 62, 100, 6, 0);
    SetNote(state, 0, 0, 2, 63, 100, 30, 0);
  });
  sequencer->SetStepSelected(15, true);
  sequencer->SetEditLane(SequencerEditLane::Length);
  assert(!sequencer->AdjustSelectedSteps(1, false) && lengths(15) == (Values{24}));
  sequencer->SetEditLane(SequencerEditLane::Micro);
  assert(sequencer->AdjustSelectedSteps(1, false) && microsteps(15) == (Values{1}) && lengths(15) == (Values{20}));
  sequencer->ClearSelectedSteps();
  sequencer->SetStepSelected(0, true);
  sequencer->SetEditLane(SequencerEditLane::Length);
  assert(!sequencer->AdjustSelectedSteps(1, true) && lengths(0) == (Values{384}));
  sequencer->SetStepSelected(0, false);
  sequencer->SetStepSelected(1, true);
  sequencer->SetEditLane(SequencerEditLane::Length);
  assert(!sequencer->AdjustSelectedSteps(-1, false) && lengths(1) == (Values{6}));
  assert(sequencer->AdjustSelectedSteps(1, false) && lengths(1) == (Values{12}));
  sequencer->SetStepSelected(1, false);
  sequencer->SetStepSelected(2, true);
  sequencer->SetEditLane(SequencerEditLane::Length);
  assert(sequencer->AdjustSelectedSteps(1, false) && lengths(2) == (Values{36}));  // 30 sits between 24 and 36.
  sequencer->ClearSelectedSteps();

  // Pitch clamping and collisions inside a transposed chord.
  restore([](SynthSequencer::ProjectState& state) {
    SetNote(state, 0, 0, 4, 127, 100, 24, 0);
    SetNote(state, 0, 0, 4, 120, 100, 24, 0);
    SetNote(state, 0, 0, 5, 60, 100, 24, 0);
    SetNote(state, 0, 0, 5, 61, 100, 24, 0);
    SetNote(state, 0, 0, 6, 1, 100, 24, 0);
  });
  sequencer->SetStepSelected(4, true);
  assert(!sequencer->AdjustSelectedSteps(1, true) && StepPitches(*sequencer, 4) == (Values{120, 127}));
  assert(sequencer->AdjustSelectedSteps(-1, true) && StepPitches(*sequencer, 4) == (Values{108, 115}));
  sequencer->ClearSelectedSteps();
  sequencer->SetStepSelected(5, true);
  assert(sequencer->AdjustSelectedSteps(1, false) && StepPitches(*sequencer, 5) == (Values{61, 62}));
  assert(sequencer->AdjustSelectedSteps(-1, false) && StepPitches(*sequencer, 5) == (Values{60, 61}));
  sequencer->ClearSelectedSteps();
  sequencer->SetStepSelected(6, true);
  assert(sequencer->AdjustSelectedSteps(-1, true) && StepPitches(*sequencer, 6) == (Values{0}));
  sequencer->ClearSelectedSteps();

  // Empty steps adjust the values new notes start with.
  sequencer->SetStepSelected(9, true);
  sequencer->SetEditLane(SequencerEditLane::Velocity);
  assert(sequencer->BuildUiState(0).valueLabel == "NEW VEL 110");
  assert(sequencer->AdjustSelectedSteps(1, false) && sequencer->BuildUiState(0).valueLabel == "NEW VEL 111");
  sequencer->SetEditLane(SequencerEditLane::Length);
  assert(sequencer->AdjustSelectedSteps(2, false) && sequencer->BuildUiState(0).valueLabel == "NEW LEN 2");
  sequencer->SetEditLane(SequencerEditLane::Micro);
  assert(!sequencer->AdjustSelectedSteps(1, false));
  sequencer->SetEditLane(SequencerEditLane::Note);
  assert(sequencer->ToggleNoteOnSelectedSteps(65));
  auto state = sequencer->CaptureState();
  const auto* added = FindNote(*state, 0, 0, 9, 65);
  assert(added && added->velocity == 111 && added->lengthPulses == 48);
  sequencer->SetEditLane(SequencerEditLane::Velocity);
  sequencer->SetStepSelected(9, false);  // Emptying the selection returns to the note lane.
  assert(!sequencer->HasHeldSteps() && sequencer->BuildUiState(0).editLane == SequencerEditLane::Note);
  sequencer.reset();
}

// A chord (or the notes of the pads held) goes into a step or out of it as one group: when the step
// holds any of its notes they all come out and none goes in; when it holds none they all go in.
void TestChordTogglesAsOne() {
  using namespace synth;
  Output output;
  auto sequencer = std::make_unique<SynthSequencer>();
  sequencer->SetMidiSink(Sink, &output);
  using Values = std::vector<int>;
  const std::uint8_t chord[] = {60, 64, 67};
  const std::uint8_t third = 64;

  // Pads held, a step tapped: an empty step takes the whole chord, and gives it back whole.
  assert(sequencer->ToggleStepNotes(0, 0, chord, std::size(chord)));
  assert(StepPitches(*sequencer, 0) == (Values{60, 64, 67}));
  assert(sequencer->ToggleStepNotes(0, 0, chord, std::size(chord)));
  assert(StepPitches(*sequencer, 0).empty());
  // A step that holds one of its notes (and another note): that one comes out, C and G do not go in,
  // the other note stays.
  const std::uint8_t mixed[] = {64, 72};
  assert(sequencer->ToggleStepNotes(0, 1, mixed, std::size(mixed)));
  assert(sequencer->ToggleStepNotes(0, 1, chord, std::size(chord)));
  assert(StepPitches(*sequencer, 1) == (Values{72}));

  // Steps selected, then a chord's pad: each selected step decides for itself.
  assert(sequencer->ToggleStepNotes(0, 2, &third, 1));  // step 2 holds E; step 3 is empty
  sequencer->SetStepSelected(2, true);
  sequencer->SetStepSelected(3, true);
  assert(sequencer->ToggleNotesOnSelectedSteps(chord, std::size(chord), 90));
  assert(StepPitches(*sequencer, 2).empty() && StepPitches(*sequencer, 3) == (Values{60, 64, 67}));
  assert(StepValues(*sequencer, 3, [](const auto& note) { return note.velocity; }) == (Values{90, 90, 90}));
  assert(sequencer->ToggleNotesOnSelectedSteps(chord, std::size(chord)));
  assert(StepPitches(*sequencer, 2) == (Values{60, 64, 67}) && StepPitches(*sequencer, 3).empty());
  sequencer->ClearSelectedSteps();
  assert(!sequencer->ToggleNotesOnSelectedSteps(chord, std::size(chord)));  // nothing selected

  // The older step keys (PressStep) take the held notes as one group too.
  sequencer->PressStep(4, &third, 1);
  sequencer->ReleaseStep(4);
  assert(StepPitches(*sequencer, 4) == (Values{64}));
  sequencer->PressStep(4, chord, std::size(chord));
  sequencer->ReleaseStep(4);
  assert(StepPitches(*sequencer, 4).empty());
  sequencer.reset();
}

// A held pad's notes on the selected steps (the SEQ note page): a move takes each of them to its new
// pitch and keeps its values, all or nothing; the other lanes edit those notes alone.
void TestHeldNotesEdit() {
  using namespace synth;
  Output output;
  auto sequencer = std::make_unique<SynthSequencer>();
  sequencer->SetMidiSink(Sink, &output);
  using Values = std::vector<int>;
  const std::uint8_t chord[] = {60, 64, 67};
  const std::uint8_t c = 60, d = 62;
  assert(sequencer->SetStepNotes(0, 0, chord, std::size(chord), 100));
  assert(sequencer->SetStepNotes(0, 1, &c, 1, 80));
  assert(sequencer->SetStepNotes(0, 2, &d, 1, 70));
  assert(!sequencer->SelectedStepsHoldAny(&c, 1));  // nothing selected
  for (const std::size_t step : {0, 1, 2}) sequencer->SetStepSelected(step, true);
  std::array<bool, 128> others{};
  assert(sequencer->SelectedStepsHoldAny(&c, 1, &others));
  assert(others[64] && others[67] && !others[60] && !others[62]);  // step 2 does not hold C: its D does not count
  const std::uint8_t e = 64, cSharp = 61;
  assert(!sequencer->MoveNotesOnSelectedSteps(&c, &e, 1));  // E is taken on step 0: nothing moves
  assert(StepPitches(*sequencer, 0) == (Values{60, 64, 67}) && StepPitches(*sequencer, 1) == (Values{60}));
  assert(sequencer->MoveNotesOnSelectedSteps(&c, &cSharp, 1));
  assert(StepPitches(*sequencer, 0) == (Values{61, 64, 67}) && StepPitches(*sequencer, 1) == (Values{61}) && StepPitches(*sequencer, 2) == (Values{62}));
  assert(StepValues(*sequencer, 1, [](const auto& note) { return note.velocity; }) == (Values{80}));
  // A group moves as one (no note lands on another of the group on the way).
  const std::uint8_t from[] = {61, 64}, to[] = {64, 65};
  assert(sequencer->MoveNotesOnSelectedSteps(from, to, 2));
  assert(StepPitches(*sequencer, 0) == (Values{64, 65, 67}) && StepPitches(*sequencer, 1) == (Values{64}));
  // VEL for the held note only, on each selected step that has it.
  sequencer->SetEditLane(SequencerEditLane::Velocity);
  assert(sequencer->EditLane() == SequencerEditLane::Velocity);
  const std::uint8_t g = 67;
  assert(sequencer->AdjustNotesOnSelectedSteps(&g, 1, -5, false));
  assert(StepValues(*sequencer, 0, [](const auto& note) { return note.velocity; }) == (Values{100, 100, 95}));
  assert(sequencer->NotesLaneValue(&g, 1) == "95");
  assert(!sequencer->AdjustNotesOnSelectedSteps(&cSharp, 1, 1, false));  // no step holds it
  sequencer->ClearSelectedSteps();
  sequencer.reset();
}

void TestSequencerSettings() {
  using namespace synth;
  Output output;
  auto sequencer = std::make_unique<SynthSequencer>();
  sequencer->SetMidiSink(Sink, &output);
  sequencer->SetTempo(10);
  assert(sequencer->BuildUiState(0).bpm == 30);
  sequencer->SetTempo(500);
  assert(sequencer->BuildUiState(0).bpm == 300);
  sequencer->SetTempo(128);
  assert(sequencer->BuildUiState(0).bpm == 128 && sequencer->CaptureState()->bpm == 128);

  sequencer->SetTrackEnabled(2, false);
  auto ui = sequencer->BuildUiState(0);
  assert(!sequencer->TrackEnabled(2) && !ui.tracks[2].enabled && ui.trackEnabled && sequencer->TrackEnabled(0));
  assert(!sequencer->BuildUiState(2).trackEnabled);
  sequencer->SetTrackEnabled(2, true);
  assert(sequencer->TrackEnabled(2) && !sequencer->TrackEnabled(7));
  sequencer->SetTrackEnabled(7, false);  // Out of range: ignored.

  assert(sequencer->PatternCount(1) == 1 && sequencer->ActivePattern(1) == 0 && sequencer->PatternCount(9) == 0);
  auto state = sequencer->CaptureState();
  state->tracks[1].patternCount = 3;
  state->tracks[1].activePattern = 2;
  SetNote(*state, 1, 1, 4, 70, 100, 24, 0);
  SetNote(*state, 1, 2, 4, 71, 100, 24, 0);
  assert(sequencer->RestoreState(*state));
  assert(sequencer->PatternCount(1) == 3 && sequencer->ActivePattern(1) == 2 && sequencer->BuildUiState(0).activePattern == 0);
  assert(sequencer->ClearPatternEvents(1, 1) && !sequencer->ClearPatternEvents(1, 3) && !sequencer->ClearPatternEvents(5, 0));
  state = sequencer->CaptureState();
  assert(state->tracks[1].patternCount == 3 && !FindNote(*state, 1, 1, 4, 70) && FindNote(*state, 1, 2, 4, 71));
  sequencer->SetActiveTrack(1);
  assert(!sequencer->PatternHasEvents(1) && sequencer->PatternHasEvents(2));
  sequencer.reset();
}

// Song mode, playback position of a note event: row, bar within the row, pulse within the bar.
using SongSpot = std::array<int, 3>;
using SongSpots = std::vector<SongSpot>;

SongSpots SongEvents(PulseProbe& probe, std::size_t track, midi::MidiStatus status, int note) {
  std::lock_guard<std::mutex> lock(probe.mutex);
  SongSpots spots;
  for (const auto& event : probe.events)
    if (event.track == track && event.status == status && event.note == note) spots.push_back(SongSpot{event.songRow, event.songBar, event.barPosition});
  return spots;
}

bool WaitStopped(const synth::SynthSequencer& sequencer) {
  using namespace std::chrono_literals;
  for (int attempt = 0; attempt < 2000 && sequencer.Playing(); ++attempt) std::this_thread::sleep_for(5ms);
  return !sequencer.Playing();
}

void TestSongEditing() {
  using namespace synth;
  using Values = std::vector<int>;
  Output output;
  auto sequencer = std::make_unique<SynthSequencer>();
  sequencer->SetMidiSink(Sink, &output);

  assert(!sequencer->SongMode() && sequencer->SongLoop() && sequencer->SongLength() == 0);
  auto ui = sequencer->BuildUiState(0);
  assert(!ui.songMode && !ui.songPlaying && ui.songLoop && ui.songLength == 0 && ui.songRow == 0 && ui.songBar == 0 &&
         ui.songRowProgress == 0.0f && ui.songCueRow == kSongMaxRows);
  const SongRow fresh{};
  assert(fresh.repeats == 1 && fresh.patterns == (std::array<std::int8_t, kSynthTrackCount>{{0, 0, 0, 0}}));
  assert(sequencer->SongRowAt(0) == fresh && sequencer->SongRowAt(kSongMaxRows + 5) == fresh);
  sequencer->SetSongMode(true);
  sequencer->SetSongLoop(false);
  ui = sequencer->BuildUiState(0);
  assert(sequencer->SongMode() && !sequencer->SongLoop() && ui.songMode && !ui.songLoop && !ui.songPlaying);
  sequencer->SetSongLoop(true);

  // Track 1 holds three patterns, track 2 two, tracks 0 and 3 one.
  sequencer->SetActiveTrack(1);
  assert(sequencer->SelectOrCreatePattern(1) && sequencer->SelectOrCreatePattern(2));
  sequencer->SetActiveTrack(2);
  assert(sequencer->SelectOrCreatePattern(1));
  sequencer->SetActiveTrack(0);

  const SongRow a{{{0, 2, 1, -1}}, 2};
  const SongRow b{{{-1, 1, 0, 0}}, 16};
  assert(!sequencer->InsertSongRow(1, a));  // Past the end.
  assert(sequencer->InsertSongRow(0, a) && sequencer->InsertSongRow(0, b) && sequencer->SongLength() == 2);
  assert(sequencer->SongRowAt(0) == b && sequencer->SongRowAt(1) == a && sequencer->SongRowAt(2) == fresh);
  for (const SongRow& invalid : {SongRow{{{1, 0, 0, 0}}, 1}, SongRow{{{0, 3, 0, 0}}, 1}, SongRow{{{0, 0, 2, 0}}, 1},
                                 SongRow{{{-2, 0, 0, 0}}, 1}, SongRow{{{0, 0, 0, 0}}, 0}, SongRow{{{0, 0, 0, 0}}, 17}})
    assert(!sequencer->InsertSongRow(0, invalid));
  assert(sequencer->SongLength() == 2 && sequencer->BuildUiState(0).songLength == 2);

  assert(sequencer->SetSongPattern(1, 1, -1) && sequencer->SongRowAt(1).patterns[1] == -1);
  assert(sequencer->SetSongPattern(1, 1, 2) && sequencer->SongRowAt(1) == a);
  assert(!sequencer->SetSongPattern(1, 1, 3) && !sequencer->SetSongPattern(1, 1, -2) && !sequencer->SetSongPattern(1, 0, 1));
  assert(!sequencer->SetSongPattern(2, 0, 0) && !sequencer->SetSongPattern(0, kSynthTrackCount, 0));
  assert(sequencer->SetSongRepeats(0, 1) && sequencer->SongRowAt(0).repeats == 1 && sequencer->SetSongRepeats(0, 16));
  assert(!sequencer->SetSongRepeats(0, 0) && !sequencer->SetSongRepeats(0, 17) && !sequencer->SetSongRepeats(2, 4));
  assert(sequencer->SongRowAt(0) == b && sequencer->SongRowAt(1) == a);

  // At most kSongMaxRows rows; deleting shifts later rows up.
  while (sequencer->SongLength() < kSongMaxRows) assert(sequencer->InsertSongRow(sequencer->SongLength(), fresh));
  assert(!sequencer->InsertSongRow(0, fresh) && !sequencer->InsertSongRow(kSongMaxRows, fresh));
  assert(!sequencer->DeleteSongRow(kSongMaxRows));
  assert(sequencer->DeleteSongRow(0) && sequencer->SongRowAt(0) == a && sequencer->SongLength() == kSongMaxRows - 1);
  while (sequencer->SongLength() > 1) assert(sequencer->DeleteSongRow(sequencer->SongLength() - 1));
  assert(sequencer->SongRowAt(0) == a && sequencer->SongRowAt(1) == fresh);
  assert(sequencer->InsertSongRow(0, b) && sequencer->InsertSongRow(2, fresh));

  // Stopped, a cue picks the row Play() starts from; it stays within the song.
  sequencer->CueSongRow(2);
  ui = sequencer->BuildUiState(0);
  assert(ui.songRow == 2 && ui.songCueRow == kSongMaxRows && !ui.songPlaying);
  sequencer->CueSongRow(3);  // Past the end: ignored.
  assert(sequencer->BuildUiState(0).songRow == 2);

  auto state = sequencer->CaptureState();
  assert(state->songLength == 3 && state->songLoop && state->song[0] == b && state->song[1] == a && state->song[2] == fresh &&
         state->song[3] == fresh && SynthSequencer::ValidateState(*state));
  assert(sequencer->DeleteSongRow(2) && sequencer->BuildUiState(0).songRow == 1);

  // Snapshots carry the song; rows past its length are ignored. The mode switch is not project data.
  state->songLoop = false;
  state->song[1].repeats = 3;
  state->song[40].repeats = 0;
  assert(SynthSequencer::ValidateState(*state));
  assert(sequencer->RestoreState(*state));
  assert(sequencer->SongLength() == 3 && !sequencer->SongLoop() && sequencer->SongRowAt(1).repeats == 3 && sequencer->SongMode());
  assert(sequencer->CaptureState()->song[40] == fresh && sequencer->BuildUiState(0).songRow == 0);
  auto rejects = [&](auto edit) {
    auto bad = sequencer->CaptureState();
    edit(*bad);
    assert(!SynthSequencer::ValidateState(*bad) && !sequencer->RestoreState(*bad));
    assert(sequencer->SongLength() == 3 && sequencer->SongRowAt(0) == b);
  };
  rejects([](SynthSequencer::ProjectState& bad) { bad.songLength = kSongMaxRows + 1; });
  rejects([](SynthSequencer::ProjectState& bad) { bad.song[2].repeats = 0; });
  rejects([](SynthSequencer::ProjectState& bad) { bad.song[2].repeats = 17; });
  rejects([](SynthSequencer::ProjectState& bad) { bad.song[0].patterns[1] = 3; });
  rejects([](SynthSequencer::ProjectState& bad) { bad.song[0].patterns[0] = -2; });
  rejects([](SynthSequencer::ProjectState& bad) { bad.song[1].patterns[3] = 1; });
  rejects([](SynthSequencer::ProjectState& bad) { bad.tracks[1].patternCount = 2; bad.tracks[1].activePattern = 0; });
  sequencer->Reset();
  ui = sequencer->BuildUiState(0);
  assert(sequencer->SongLength() == 0 && sequencer->SongLoop() && !sequencer->SongMode() && ui.songRow == 0 && !ui.songMode);

  // Pattern edits keep every song reference valid.
  sequencer->SetActiveTrack(1);
  assert(sequencer->SelectOrCreatePattern(1) && sequencer->SelectOrCreatePattern(2));
  sequencer->SetActiveTrack(2);
  assert(sequencer->SelectOrCreatePattern(1));
  assert(sequencer->InsertSongRow(0, SongRow{{{0, 0, 0, 0}}, 1}) && sequencer->InsertSongRow(1, SongRow{{{0, 1, 1, 0}}, 2}) &&
         sequencer->InsertSongRow(2, SongRow{{{0, 2, -1, 0}}, 3}));
  auto column = [&](std::size_t track) {
    Values patterns;
    for (std::size_t row = 0; row < sequencer->SongLength(); ++row) patterns.push_back(sequencer->SongRowAt(row).patterns[track]);
    return patterns;
  };
  sequencer->SetActiveTrack(1);
  assert(sequencer->DeletePattern(1));  // Later indices shift down; references to it go silent.
  assert(column(1) == (Values{0, -1, 1}) && column(2) == (Values{0, 1, -1}) && column(0) == (Values{0, 0, 0}));
  assert(sequencer->CopyPattern(0, 2) && sequencer->SelectOrCreatePattern(3) && sequencer->PatternCount(1) == 4);
  assert(column(1) == (Values{0, -1, 1}));  // Copying and creating leave the song alone.
  assert(sequencer->SetSongPattern(0, 1, 3) && sequencer->SetSongPattern(1, 1, 2));
  sequencer->AdjustPatternCount(1);
  assert(column(1) == (Values{3, 2, 1}));
  sequencer->AdjustPatternCount(-3);  // Releases patterns 2, 3 and 4.
  assert(sequencer->PatternCount(1) == 2 && column(1) == (Values{-1, -1, 1}));
  sequencer->SetActiveTrack(2);
  assert(sequencer->DeletePattern(0));
  assert(column(2) == (Values{-1, 0, -1}));
  sequencer->SetActiveTrack(3);
  assert(sequencer->DeletePattern(0));  // The only pattern is cleared, not removed: references stay.
  assert(column(3) == (Values{0, 0, 0}) && sequencer->PatternCount(3) == 1);
  assert(sequencer->SongRowAt(2).repeats == 3 && SynthSequencer::ValidateState(*sequencer->CaptureState()));
  sequencer->SetActiveTrack(0);

  // Key-light data: the loudest note of each step and the filled steps of each pattern.
  sequencer->Reset();
  state = sequencer->CaptureState();
  state->tracks[0].patternCount = 3;
  SetNote(*state, 0, 0, 0, 60, 90, 24, 0);
  SetNote(*state, 0, 0, 0, 64, 30, 24, 0);  // The loudest note is not the last one.
  SetNote(*state, 0, 0, 3, 62, 127, 24, 0);
  SetNote(*state, 0, 0, 9, 50, 1, 24, 0);
  SetNote(*state, 0, 1, 5, 70, 1, 24, 0);
  for (std::uint8_t pitch : {90, 20, 75, 21, 100, 5, 64, 33}) SetNote(*state, 0, 2, 7, pitch, 100, 24, 0);
  assert(sequencer->RestoreState(*state));
  ui = sequencer->BuildUiState(0);
  assert(ui.steps[0].velocity == 90 && ui.steps[3].velocity == 127 && ui.steps[9].velocity == 1 && ui.steps[1].velocity == 0);
  assert(ui.patternStepCounts[0] == 3 && ui.patternStepCounts[1] == 1 && ui.patternStepCounts[2] == 1 && ui.patternStepCounts[3] == 0);
  assert(ui.patternHasEvents[0] && ui.patternHasEvents[1] && ui.patternHasEvents[2] && !ui.patternHasEvents[3]);
  ui = sequencer->BuildUiState(1);
  assert(ui.patternStepCounts == (std::array<std::uint16_t, kSequencerMaxPatternCount>{}) && ui.steps[0].velocity == 0);

  // Step pitches in ascending order, for the active track's active pattern.
  std::array<std::uint8_t, 8> notes{};
  assert(sequencer->StepNotes(0, notes) == 2 && notes[0] == 60 && notes[1] == 64);
  assert(sequencer->StepNotes(1, notes) == 0 && sequencer->StepNotes(kSequencerStepCount, notes) == 0);
  assert(sequencer->QueuePattern(0, 2));  // Stopped: switches now.
  assert(sequencer->StepNotes(7, notes) == 8 && notes == (std::array<std::uint8_t, 8>{{5, 20, 21, 33, 64, 75, 90, 100}}));
  assert(sequencer->StepNotes(0, notes) == 0 && sequencer->BuildUiState(0).steps[7].velocity == 100);
  sequencer->SetActiveTrack(1);
  assert(sequencer->StepNotes(7, notes) == 0);
  sequencer.reset();
}

void TestSongPlayback() {
  using namespace synth;
  using midi::MidiStatus;
  PulseProbe probe;  // Outlives the sequencer's final stop callback.
  auto sequencer = std::make_unique<SynthSequencer>();
  probe.sequencer = sequencer.get();
  sequencer->SetMidiSink(ProbeSink, &probe);

  auto state = sequencer->CaptureState();
  state->bpm = 300;
  state->swing = 75;  // Odd sixteenths start 12 pulses late.
  state->tracks[0].patternCount = 3;
  SetNote(*state, 0, 0, 0, 60, 100, 6, 0);
  SetNote(*state, 0, 1, 0, 61, 100, 6, 0);
  SetNote(*state, 0, 2, 0, 62, 100, 6, 0);
  SetNote(*state, 1, 0, 0, 70, 100, 6, 0);
  SetNote(*state, 1, 0, 15, 71, 100, 24, 0);  // 372..396: still sounding at the row line.
  state->tracks[2].patternSteps.fill(3);             // A 72-pulse loop inside 384-pulse bars.
  SetNote(*state, 2, 0, 0, 80, 100, 6, 0);
  SetNote(*state, 3, 0, 15, 90, 100, 4, 5);   // 360 + 20 + 12 = 392: past the row line.
  state->songLength = 3;
  state->song[0] = SongRow{{{0, 0, 0, 0}}, 1};
  state->song[1] = SongRow{{{1, -1, 0, -1}}, 2};
  state->song[2] = SongRow{{{2, 0, 0, -1}}, 1};
  state->songLoop = false;
  assert(sequencer->RestoreState(*state));
  sequencer->SetSongMode(true);
  sequencer->SetRecordArmed(true);
  auto ui = sequencer->BuildUiState(0);
  assert(ui.songMode && !ui.songPlaying && ui.songRow == 0 && ui.songLength == 3);
  {
    std::lock_guard<std::mutex> lock(probe.mutex);
    probe.events.clear();  // RestoreState stopped first; count only this run's all-notes-off.
  }

  Arm(probe, 0, MidiStatus::NoteOn, 61);
  sequencer->Play();
  const auto rowOne = WaitFrozen(probe);
  assert(rowOne.songRow == 1 && rowOne.songBar == 0 && rowOne.barPosition == 0);
  ui = sequencer->BuildUiState(0);
  assert(ui.playing && ui.songPlaying && ui.songRow == 1 && ui.songBar == 0 && ui.songCueRow == kSongMaxRows);
  assert(std::lround(ui.songRowProgress * 768.0f) == 1);
  assert(ui.activePattern == 1 && ui.tracks[0].activePattern == 1 && ui.tracks[2].currentStep == 0 && ui.currentStep == 0);
  assert(!ui.tracks[0].songSilent && ui.tracks[1].songSilent && !ui.tracks[2].songSilent && ui.tracks[3].songSilent);
  // Silence leaves the mute and enable flags alone.
  assert(ui.tracks[1].enabled && !ui.tracks[1].muted && ui.tracks[3].enabled && !ui.tracks[3].muted);
  // Takes land in the row's pattern; a track the row leaves out drops them.
  sequencer->RecordNoteOn(1, 75, 100);
  sequencer->RecordNoteOff(1, 75);
  sequencer->RecordNoteOn(0, 66, 90);
  sequencer->RecordNoteOff(0, 66);
  Arm(probe, 0, MidiStatus::NoteOn, 66);
  Release(probe);

  // The second bar of the two-bar row: progress counts the whole row.
  const auto secondBar = WaitFrozen(probe);
  assert(secondBar.songRow == 1 && secondBar.songBar == 1 && secondBar.barPosition == 0);
  ui = sequencer->BuildUiState(0);
  assert(ui.songRow == 1 && ui.songBar == 1 && std::lround(ui.songRowProgress * 768.0f) == 385);
  Arm(probe, 0, MidiStatus::NoteOn, 62);
  Release(probe);

  const auto rowTwo = WaitFrozen(probe);
  assert(rowTwo.songRow == 2 && rowTwo.songBar == 0 && rowTwo.barPosition == 0);
  ui = sequencer->BuildUiState(0);
  assert(ui.songRow == 2 && ui.tracks[0].activePattern == 2 && !ui.tracks[1].songSilent && ui.tracks[3].songSilent);
  assert(std::lround(ui.songRowProgress * 384.0f) == 1);
  Release(probe);

  // Looping is off: playback stops on the bar line after the last row.
  assert(WaitStopped(*sequencer));
  ui = sequencer->BuildUiState(0);
  assert(!ui.playing && !ui.songPlaying && ui.songMode && ui.songRow == 0 && ui.songRowProgress == 0.0f);
  assert(!ui.tracks[1].songSilent && !ui.tracks[3].songSilent);
  sequencer->SetMidiSink(nullptr, nullptr);

  // Each row switches every track to its pattern; repeats play the row again.
  assert(SongEvents(probe, 0, MidiStatus::NoteOn, 60) == (SongSpots{SongSpot{0, 0, 0}}));
  assert(SongEvents(probe, 0, MidiStatus::NoteOn, 61) == (SongSpots{SongSpot{1, 0, 0}, SongSpot{1, 1, 0}}));
  assert(SongEvents(probe, 0, MidiStatus::NoteOn, 62) == (SongSpots{SongSpot{2, 0, 0}}));
  // A silent track plays nothing of its own...
  assert(SongEvents(probe, 1, MidiStatus::NoteOn, 70) == (SongSpots{SongSpot{0, 0, 0}, SongSpot{2, 0, 0}}));
  assert(SongEvents(probe, 1, MidiStatus::NoteOn, 71) == (SongSpots{SongSpot{0, 0, 372}, SongSpot{2, 0, 372}}));
  // ...yet a note sounding over the row line still ends on time, and the final stop releases the last one.
  assert(SongEvents(probe, 1, MidiStatus::NoteOff, 71) == (SongSpots{SongSpot{1, 0, 12}, SongSpot{-1, -1, -1}}));
  // The swung last step of row 0 sounds after the line, though track 3 is silent in row 1; nothing else does.
  assert(SongEvents(probe, 3, MidiStatus::NoteOn, 90) == (SongSpots{SongSpot{1, 0, 8}}));
  assert(SongEvents(probe, 3, MidiStatus::NoteOff, 90) == (SongSpots{SongSpot{1, 0, 12}}));
  // The three-step track restarts with every row and loops on inside the two-bar row.
  SongSpots expected;
  for (int pulse = 0; pulse <= 360; pulse += 72) expected.push_back(SongSpot{0, 0, pulse});
  for (int pulse = 0; pulse < 768; pulse += 72) expected.push_back(SongSpot{1, pulse / 384, pulse % 384});
  for (int pulse = 0; pulse <= 360; pulse += 72) expected.push_back(SongSpot{2, 0, pulse});
  assert(SongEvents(probe, 2, MidiStatus::NoteOn, 80) == expected);
  // The take plays from the row pattern's next pass; the silent track's take was dropped.
  assert(SongEvents(probe, 0, MidiStatus::NoteOn, 66) == (SongSpots{SongSpot{1, 1, 0}}));
  assert(SongEvents(probe, 1, MidiStatus::NoteOn, 75).empty());
  state = sequencer->CaptureState();
  assert(FindNote(*state, 0, 1, 0, 66) && FindNote(*state, 0, 1, 0, 61) && !FindNote(*state, 0, 0, 0, 66) && !FindNote(*state, 0, 2, 0, 66));
  for (std::size_t pattern = 0; pattern < kSequencerMaxPatternCount; ++pattern)
    for (std::size_t step = 0; step < kSequencerStepCount; ++step) assert(!FindNote(*state, 1, pattern, step, 75));
  // No hanging notes: every note-on was matched by a note-off, and the stop sent all-notes-off.
  {
    std::lock_guard<std::mutex> lock(probe.mutex);
    std::array<std::array<int, 128>, kSynthTrackCount> sounding{};
    std::size_t allNotesOff = 0;
    for (const auto& event : probe.events) {
      if (event.status == MidiStatus::NoteOn) ++sounding[event.track][event.note];
      if (event.status == MidiStatus::NoteOff) --sounding[event.track][event.note];
      if (event.status == MidiStatus::ControlChange) ++allNotesOff;
    }
    for (const auto& track : sounding)
      for (const int count : track) assert(count == 0);
    assert(allNotesOff == kSynthTrackCount);
  }
  sequencer.reset();
}

void TestSongCueAndToggle() {
  using namespace synth;
  using midi::MidiStatus;
  PulseProbe probe;
  auto sequencer = std::make_unique<SynthSequencer>();
  probe.sequencer = sequencer.get();
  sequencer->SetMidiSink(ProbeSink, &probe);

  auto state = sequencer->CaptureState();
  state->bpm = 300;
  state->tracks[0].patternCount = 3;
  SetNote(*state, 0, 0, 0, 60, 100, 6, 0);
  SetNote(*state, 0, 1, 0, 61, 100, 6, 0);
  SetNote(*state, 0, 2, 0, 62, 100, 6, 0);
  state->songLength = 3;
  state->song[0] = SongRow{{{0, 0, 0, 0}}, 1};
  state->song[1] = SongRow{{{1, 0, 0, 0}}, 2};
  state->song[2] = SongRow{{{2, 0, 0, -1}}, 1};
  assert(sequencer->RestoreState(*state) && sequencer->SongLoop());

  // Stopped: the cue is where the song starts.
  sequencer->CueSongRow(1);
  sequencer->SetSongMode(true);
  auto ui = sequencer->BuildUiState(0);
  assert(ui.songRow == 1 && ui.songCueRow == kSongMaxRows && !ui.songPlaying);
  Arm(probe, 0, MidiStatus::NoteOn, 61);
  sequencer->Play();
  const auto start = WaitFrozen(probe);
  assert(start.songRow == 1 && start.songBar == 0 && start.barPosition == 0);
  // A queued switch would land on track 0's next wrap; the row change on that bar line drops it.
  assert(sequencer->QueuePattern(0, 0) && sequencer->BuildUiState(0).tracks[0].queuedPattern == 0);
  // Playing: the cue jumps on the next bar line and cuts row 1's second bar.
  sequencer->CueSongRow(2);
  ui = sequencer->BuildUiState(0);
  assert(ui.songCueRow == 2 && ui.songRow == 1 && ui.songBar == 0);
  Arm(probe, 0, MidiStatus::NoteOn, 62);
  Release(probe);

  const auto jumped = WaitFrozen(probe);
  assert(jumped.songRow == 2 && jumped.songBar == 0 && jumped.barPosition == 0);
  ui = sequencer->BuildUiState(0);
  assert(ui.songCueRow == kSongMaxRows && ui.tracks[0].activePattern == 2 && ui.tracks[0].queuedPattern == kSequencerMaxPatternCount);
  // Row 2 is the last row: the song loops back to row 0.
  Arm(probe, 0, MidiStatus::NoteOn, 60);
  Release(probe);

  const auto looped = WaitFrozen(probe);
  assert(looped.songRow == 0 && looped.songBar == 0 && looped.barPosition == 0);
  // Switching song mode off waits for the bar line; then the row's patterns loop on.
  sequencer->SetSongMode(false);
  ui = sequencer->BuildUiState(0);
  assert(!ui.songMode && ui.songPlaying && ui.songRow == 0);
  Arm(probe, 0, MidiStatus::NoteOn, 60);
  Release(probe);

  const auto patternMode = WaitFrozen(probe);
  assert(patternMode.songRow == -1 && patternMode.barPosition == 0);
  ui = sequencer->BuildUiState(0);
  assert(!ui.songPlaying && ui.songRow == 1 && ui.tracks[0].activePattern == 0);
  // Without song mode a cue only moves the start row; with it, the cue is a jump, and the song
  // takes over on the next bar line at that row.
  sequencer->CueSongRow(2);
  assert(sequencer->BuildUiState(0).songRow == 2 && sequencer->BuildUiState(0).songCueRow == kSongMaxRows);
  sequencer->SetSongMode(true);
  sequencer->CueSongRow(1);
  ui = sequencer->BuildUiState(0);
  assert(ui.songMode && !ui.songPlaying && ui.songCueRow == 1 && ui.songRow == 2);
  Arm(probe, 0, MidiStatus::NoteOn, 61);
  Release(probe);

  const auto engaged = WaitFrozen(probe);
  assert(engaged.songRow == 1 && engaged.songBar == 0 && engaged.barPosition == 0);
  // Deleting an earlier row keeps the playing row playing under its new index...
  assert(sequencer->DeleteSongRow(0) && sequencer->SongLength() == 2);
  ui = sequencer->BuildUiState(0);
  assert(ui.songPlaying && ui.songRow == 0 && ui.songBar == 0 && ui.songCueRow == kSongMaxRows);
  // ...while deleting the playing two-bar row ends it on the next bar line; the row that took its place follows.
  assert(sequencer->DeleteSongRow(0) && sequencer->SongLength() == 1);
  Arm(probe, 0, MidiStatus::NoteOn, 62);
  Release(probe);

  const auto replaced = WaitFrozen(probe);
  assert(replaced.songRow == 0 && replaced.songBar == 0 && replaced.barPosition == 0);
  assert(sequencer->BuildUiState(0).tracks[3].songSilent);
  Release(probe);
  sequencer->Stop();
  ui = sequencer->BuildUiState(0);
  // Stopped: the start row (2) is clamped to the shorter song, and no track reads as silent.
  assert(!ui.songPlaying && ui.songRow == 0 && ui.songCueRow == kSongMaxRows && !ui.tracks[3].songSilent);

  // Play() always starts the song over from its start row, wherever the last run stopped.
  assert(sequencer->InsertSongRow(1, SongRow{{{0, 0, 0, 0}}, 1}) && sequencer->InsertSongRow(2, SongRow{{{1, 0, 0, 0}}, 2}));
  assert(sequencer->BuildUiState(0).songRow == 2);
  Arm(probe, 0, MidiStatus::NoteOn, 61);
  sequencer->Play();
  const auto restarted = WaitFrozen(probe);
  assert(restarted.songRow == 2 && restarted.songBar == 0 && restarted.barPosition == 0);
  assert(!sequencer->BuildUiState(0).tracks[3].songSilent);
  Release(probe);
  sequencer->Stop();
  sequencer->SetMidiSink(nullptr, nullptr);

  std::vector<std::pair<int, int>> markers;
  {
    std::lock_guard<std::mutex> lock(probe.mutex);
    for (const auto& event : probe.events)
      if (event.track == 0 && event.status == MidiStatus::NoteOn) markers.emplace_back(event.note, event.songRow);
  }
  assert(markers == (std::vector<std::pair<int, int>>{{61, 1}, {62, 2}, {60, 0}, {60, -1}, {61, 1}, {62, 0}, {61, 2}}));
  sequencer.reset();
}
}  // namespace
namespace synth::storage {
std::string ProjectDirectory() { return projectDirectory.string(); }
bool EnsureLayout() { return std::filesystem::is_directory(projectDirectory); }
}

namespace {
std::string ReadText(const std::filesystem::path& path) {
  std::ifstream input(path);
  return {std::istreambuf_iterator<char>(input), {}};
}

void WriteText(const std::filesystem::path& path, const std::string& text) {
  std::ofstream file(path, std::ios::trunc);
  file << text;
}

// Turns the T lines of a current project file into the old form: one length for the whole track.
std::string WithOneLengthPerTrack(const std::string& text) {
  std::string out;
  std::size_t at = 0;
  while (at < text.size()) {
    const std::size_t end = std::min(text.find('\n', at), text.size());
    std::string line = text.substr(at, end - at);
    if (line.rfind("T ", 0) == 0) {
      std::size_t space = 0;
      for (int fields = 0; fields < 8 && space != std::string::npos; ++fields) space = line.find(' ', space + 1);
      if (space != std::string::npos) line.resize(space);
    }
    out += line + '\n';
    at = end + 1;
  }
  return out;
}

void TestProjectVersion2() {
  using namespace synth;
  auto sequencer = std::make_unique<SynthSequencer>();
  sequencer->SetSwing(66);
  sequencer->SetTrackStepCount(1, 7);
  const std::uint8_t note = 64;
  sequencer->PressStep(3, &note, 1);
  sequencer->ReleaseStep(3);

  ProjectData project;
  project.sequence = sequencer->CaptureState();
  for (std::size_t i = 0; i < kSynthTrackCount; ++i) {
    project.kinds[i] = SynthEngineKind::Juno;
    project.engines[i] = {"factory.2", "", "Pad", {{"vcf.cutoff", 40}}};
  }
  project.mixer.level = {{10, 20, 0, 100}};
  project.mixer.muted = {{true, false, false, true}};
  project.mixer.soloed = {{false, true, false, false}};
  project.mixer.master = 55;
  project.backlight = 7;
  project.masterFx[static_cast<std::size_t>(fx::MasterParam::DelayTime)] = 11;
  project.masterFx[static_cast<std::size_t>(fx::MasterParam::ReverbLevel)] = 0;
  project.masterFx[static_cast<std::size_t>(fx::MasterParam::Tape)] = 127;
  // Version 6: the keyboard and each track's chord and arpeggiator.
  project.lock = 2;
  project.velocity = 90;
  project.play[3].chord = 5;
  project.play[3].voicing = 2;
  project.play[3].arp = ArpSettings{ArpMode::Played, 5, 4, 100, true};
  assert(SaveProjectFile("v2 trip", project, false) == ProjectResult::Ok);
  const auto path = projectDirectory / "v2 trip.ccproj";
  const std::string valid = ReadText(path);
  assert(valid.find("CARDENZA_PROJECT 7\nBUILD CardenzaSynth 1\nUI ") == 0);
  assert(valid.find("\nSYSTEM 7\nFX 12 11 56 72 100 76 52 36 0 0 64 64 127\nKEYS 2 90\nPLAYFX 0 0 0 0 3 1 50 0\n") != std::string::npos);
  assert(valid.find("\nPLAYFX 3 5 2 5 5 4 100 1\nTRACK 0 ") != std::string::npos);
  assert(valid.find("\nMIX 0 10 1 0\n") != std::string::npos && valid.find("\nSEQ 120 66\n") != std::string::npos);

  ProjectData loaded;
  assert(LoadProjectFile("v2 trip", loaded) == ProjectResult::Ok);
  assert(loaded.mixer.level == project.mixer.level && loaded.mixer.muted == project.mixer.muted &&
         loaded.mixer.soloed == project.mixer.soloed && loaded.mixer.master == 55 && loaded.backlight == 7);
  assert(loaded.masterFx == project.masterFx);
  assert(loaded.hasPlay && loaded.lock == 2 && loaded.velocity == 90 && loaded.play[3].chord == 5 && loaded.play[3].voicing == 2 &&
         loaded.play[3].arp == project.play[3].arp && loaded.play[0].arp == ArpSettings{});
  assert(loaded.sequence->swing == 66 && loaded.sequence->tracks[1].patternSteps[0] == 7 && loaded.sequence->tracks[0].patternSteps[0] == 16);
  assert(loaded.sequence->tracks[0].patterns[0].steps[3].notes[0].active && loaded.sequence->tracks[0].patterns[0].steps[3].notes[0].note == 64);
  assert(loaded.engines[2].values.at("vcf.cutoff") == 40 && loaded.kinds[3] == SynthEngineKind::Juno);
  assert(sequencer->RestoreState(*loaded.sequence) && sequencer->Swing() == 66 && sequencer->TrackStepCount(1) == 7);

  auto reject = [&](const std::string& from, const std::string& to) {
    std::string text = valid;
    const auto at = text.find(from);
    assert(at != std::string::npos);
    text.replace(at, from.size(), to);
    WriteText(path, text);
    auto* old = loaded.sequence.get();
    assert(LoadProjectFile("v2 trip", loaded) == ProjectResult::Invalid);
    assert(loaded.sequence.get() == old && loaded.backlight == 7);
  };
  reject("CARDENZA_PROJECT 7", "CARDENZA_PROJECT 8");
  reject("\nKEYS 2 90\n", "\nKEYS 3 90\n");
  reject("\nKEYS 2 90\n", "\nKEYS 2 0\n");
  reject("\nPLAYFX 3 5 2 5 5 4 100 1\n", "\nPLAYFX 3 11 2 5 5 4 100 1\n");
  reject("\nPLAYFX 3 5 2 5 5 4 100 1\n", "\nPLAYFX 3 5 2 6 5 4 100 1\n");
  reject("\nPLAYFX 3 5 2 5 5 4 100 1\n", "\nPLAYFX 3 5 2 5 5 5 100 1\n");
  reject("\nPLAYFX 3 5 2 5 5 4 100 1\n", "\nPLAYFX 3 5 2 5 5 4 9 1\n");
  reject("\nPLAYFX 3 5 2 5 5 4 100 1\n", "\n");
  // Version 5 (unreleased development builds) had a PLAY line after FX and no play settings of
  // its own: it loads without them, and the next save writes the current version.
  {
    const auto keys = valid.find("\nKEYS ");
    const auto track = valid.find("\nTRACK 0 ");
    std::string v5 = valid.substr(0, keys + 1) + "PLAY 4 2 1 96 3 5 2 75 1\n" + valid.substr(track + 1);
    v5.replace(0, std::string("CARDENZA_PROJECT 7").size(), "CARDENZA_PROJECT 5");
    reject("CARDENZA_PROJECT 7", "CARDENZA_PROJECT 5");  // a version 5 file needs its PLAY line
    WriteText(path, v5);
    ProjectData fromV5;
    assert(LoadProjectFile("v2 trip", fromV5) == ProjectResult::Ok);
    assert(!fromV5.hasPlay && fromV5.play[3].chord == 0);
    assert(fromV5.masterFx == project.masterFx && fromV5.mixer.level == project.mixer.level && fromV5.engines[2].values.at("vcf.cutoff") == 40);
    assert(SaveProjectFile("v2 trip", fromV5, true) == ProjectResult::Ok);
    const std::string resaved = ReadText(path);
    assert(resaved.find("CARDENZA_PROJECT 7\n") == 0 && resaved.find("\nPLAY ") == std::string::npos);
    WriteText(path, valid);
  }
  reject("\nFX 12 11 ", "\nFX 12 12 ");  // Delay time past the last division.
  reject("\nFX 12 11 ", "\nFX 13 11 ");  // One value missing before TRACK.
  reject(" 64 64 127\n", " 64 64 128\n");
  reject("\nFX 12 ", "\nFX 65 ");
  reject("\nMIX 0 10 1 0\n", "\nMIX 0 101 1 0\n");
  reject("\nMIX 1 20 0 1\n", "\nMIX 1 20 0 2\n");
  reject("\nMIX 2 0 0 0\n", "\nMIX 3 0 0 0\n");
  reject("\nMASTER 55\n", "\nMASTER 101\n");
  reject("\nSYSTEM 7\n", "\nSYSTEM 256\n");
  reject("\nSYSTEM 7\n", "\n");
  reject("\nSEQ 120 66\n", "\nSEQ 120 76\n");
  reject("\nSEQ 120 66\n", "\nSEQ 120 49\n");
  reject("\nT 1 0 110 24 1 0 7\n", "\nT 1 0 110 24 1 0 257\n");
  reject("\nT 1 0 110 24 1 0 7\n", "\nT 1 0 110 24 1 0 0\n");
  reject("\nT 1 0 110 24 1 0 7\n", "\nT 1 0 110 24 1 0\n");
  WriteText(path, valid);
  assert(LoadProjectFile("v2 trip", loaded) == ProjectResult::Ok);

  // Out-of-range values are refused before anything is written.
  project.mixer.level[2] = 101;
  assert(SaveProjectFile("v2 trip", project, true) == ProjectResult::Invalid);
  project.mixer.level[2] = 0;
  project.mixer.master = 101;
  assert(SaveProjectFile("v2 trip", project, true) == ProjectResult::Invalid);
  project.mixer.master = 55;
  project.sequence->swing = 80;
  assert(SaveProjectFile("v2 trip", project, true) == ProjectResult::Invalid);
  project.sequence->swing = 66;
  project.sequence->tracks[2].patternSteps.fill(0);
  assert(SaveProjectFile("v2 trip", project, true) == ProjectResult::Invalid);
  assert(ReadText(path) == valid);

  // Version 7: every pattern has its own length, up to 256 steps, and notes anywhere on them.
  {
    auto longSequence = std::make_unique<SynthSequencer>();
    longSequence->SetActiveTrack(1);
    longSequence->SetTrackStepCount(1, 256);
    longSequence->SelectOrCreatePattern(1);
    longSequence->SetTrackStepCount(1, 33);
    longSequence->SelectOrCreatePattern(0);
    const std::uint8_t pitch = 61;
    longSequence->PressStep(255, &pitch, 1);
    longSequence->ReleaseStep(255);
    ProjectData longProject;
    longProject.sequence = longSequence->CaptureState();
    longProject.kinds = project.kinds;
    longProject.engines = project.engines;
    assert(SaveProjectFile("long", longProject, true) == ProjectResult::Ok);
    assert(ReadText(projectDirectory / "long.ccproj").find("\nT 2 0 110 24 1 0 256 33\nNOTES 1\n0 255 61 ") != std::string::npos);
    ProjectData loadedLong;
    assert(LoadProjectFile("long", loadedLong) == ProjectResult::Ok);
    assert(loadedLong.sequence->tracks[1].patternSteps[0] == 256 && loadedLong.sequence->tracks[1].patternSteps[1] == 33);
    assert(loadedLong.sequence->tracks[1].patterns[0].steps[255].notes[0].note == 61);
    assert(longSequence->RestoreState(*loadedLong.sequence) && longSequence->TrackStepCount(1) == 256);
    std::filesystem::remove(projectDirectory / "long.ccproj");
  }

  // A version 1 file loads with the version 2 defaults.
  WriteText(projectDirectory / "legacy.ccproj",
            "CARDENZA_PROJECT 1\nBUILD CardenzaSynth 1\nUI 1 -1 2 3 1 9\n"
            "TRACK 0 1 \"factory.1\" \"user.1\" \"Brass\" \"\"\nPARAM 1\n\"global.feedback\" 5\n"
            "TRACK 1 2 \"factory.2\" \"\" \"\" \"\"\nPARAM 0\n"
            "TRACK 2 3 \"factory.3\" \"\" \"\" \"kick.wav\"\nPARAM 0\n"
            "TRACK 3 4 \"kit.1\" \"\" \"\" \"\"\nPARAM 0\n"
            "SEQ 128\n"
            "T 2 1 110 24 1 0\nNOTES 2\n0 0 60 100 24 0\n1 4 64 90 48 2\n"
            "T 1 0 100 48 1 1\nNOTES 0\n"
            "T 1 0 110 24 0 0\nNOTES 0\n"
            "T 1 0 110 24 1 0\nNOTES 0\n"
            "END\n");
  ProjectData legacy;
  assert(LoadProjectFile("legacy", legacy) == ProjectResult::Ok);
  assert(!legacy.hasPlay);
  const ProjectData::MixerData defaults;
  assert(legacy.mixer.level == defaults.level && legacy.mixer.muted == defaults.muted && legacy.mixer.soloed == defaults.soloed);
  assert(legacy.mixer.master == 80 && legacy.backlight == 180 && legacy.mixer.level[3] == 100);
  assert(legacy.activeTrack == 1 && legacy.octave == -1 && legacy.root == 2 && legacy.scale == 3 && legacy.ledBrightness == 9);
  assert(legacy.sequence->bpm == 128 && legacy.sequence->swing == 50);
  for (const auto& track : legacy.sequence->tracks) assert(track.patternSteps[0] == kSequencerStepCount);
  assert(legacy.sequence->tracks[0].activePattern == 1 && legacy.sequence->tracks[1].muted && !legacy.sequence->tracks[2].enabled);
  const auto& legacyNote = legacy.sequence->tracks[0].patterns[1].steps[4].notes[0];
  assert(legacyNote.active && legacyNote.note == 64 && legacyNote.velocity == 90 && legacyNote.lengthPulses == 48 && legacyNote.microstep == 2);
  assert(legacy.samples[2] == "kick.wav" && legacy.engines[0].values.at("global.feedback") == 5);
  assert(legacy.sequence->songLength == 0 && legacy.sequence->songLoop);
  // Re-saving upgrades it to the current version with the same content.
  assert(SaveProjectFile("legacy", legacy, true) == ProjectResult::Ok);
  assert(ReadText(projectDirectory / "legacy.ccproj").find("CARDENZA_PROJECT 7\n") == 0);
  ProjectData upgraded;
  assert(LoadProjectFile("legacy", upgraded) == ProjectResult::Ok);
  assert(upgraded.masterFx == ProjectData::DefaultMasterFx());
  assert(upgraded.sequence->bpm == 128 && upgraded.mixer.master == 80 && upgraded.backlight == 180 &&
         upgraded.sequence->tracks[0].patterns[1].steps[4].notes[0].note == 64);

  std::filesystem::remove(path);
  std::filesystem::remove(projectDirectory / "legacy.ccproj");
}

// The largest project the device can hold (patterns live in PSRAM): every step of every pattern of every
// track with eight notes. Its file is over 2 MB; at the old 1 MB limit it saved and never opened again.
void TestLargestProject() {
  using namespace synth;
  ProjectData project;
  project.sequence = std::make_unique<SynthSequencer::ProjectState>();
  for (std::size_t track = 0; track < kSynthTrackCount; ++track) {
    project.kinds[track] = SynthEngineKind::Juno;
    project.engines[track] = {"juno.a68", "juno.a68", "Pad", {}};
    auto& data = project.sequence->tracks[track];
    data.patternCount = kSequencerMaxPatternCount;
    for (std::size_t pattern = 0; pattern < kSequencerMaxPatternCount; ++pattern) {
      data.patternSteps[pattern] = static_cast<std::uint16_t>(kSequencerMaxSteps);
      for (std::size_t step = 0; step < kSequencerMaxSteps; ++step) {
        auto& notes = data.patterns[pattern].steps.Edit(step).notes;
        for (std::size_t index = 0; index < notes.size(); ++index)
          notes[index] = {static_cast<std::uint8_t>(36 + index * 5 + step % 7), 100, 12, static_cast<std::uint8_t>(index % 3), true};
      }
    }
  }
  assert(SaveProjectFile("largest", project, true) == ProjectResult::Ok);
  const auto path = projectDirectory / "largest.ccproj";
  assert(std::filesystem::file_size(path) > 2 * 1024 * 1024);
  ProjectData loaded;
  assert(LoadProjectFile("largest", loaded) == ProjectResult::Ok);
  const auto& last = loaded.sequence->tracks[3].patterns[15].steps[255].notes[7];
  assert(last.active && last.note == 36 + 7 * 5 + 255 % 7 && last.microstep == 1);
  std::filesystem::remove(path);
}

void TestProjectSong() {
  using namespace synth;
  auto sequencer = std::make_unique<SynthSequencer>();
  auto state = sequencer->CaptureState();
  state->tracks[1].patternCount = 3;
  state->songLength = 2;
  state->song[0] = SongRow{{{0, 2, -1, 0}}, 4};
  state->song[1] = SongRow{{{-1, 0, 0, 0}}, 16};
  state->songLoop = false;
  assert(sequencer->RestoreState(*state));
  sequencer->SetSongMode(true);

  ProjectData project;
  project.sequence = sequencer->CaptureState();
  for (std::size_t i = 0; i < kSynthTrackCount; ++i) {
    project.kinds[i] = SynthEngineKind::Juno;
    project.engines[i] = {"factory.2", "", "Pad", {}};
  }
  assert(SaveProjectFile("song trip", project, false) == ProjectResult::Ok);
  const auto path = projectDirectory / "song trip.ccproj";
  const std::string valid = ReadText(path);
  const std::string section = "SONG 2 0\nR 0 2 -1 0 4\nR -1 0 0 0 16\n";
  assert(valid.find("CARDENZA_PROJECT 7\n") == 0 && valid.find("\n" + section + "END\n") != std::string::npos);

  ProjectData loaded;
  assert(LoadProjectFile("song trip", loaded) == ProjectResult::Ok);
  assert(loaded.sequence->songLength == 2 && !loaded.sequence->songLoop && loaded.sequence->song[0] == state->song[0] &&
         loaded.sequence->song[1] == state->song[1] && loaded.sequence->song[2] == SongRow{});
  // The song mode switch is a performance control, not project data.
  auto other = std::make_unique<SynthSequencer>();
  assert(other->RestoreState(*loaded.sequence) && !other->SongMode() && other->SongLength() == 2 && !other->SongLoop());
  assert(other->SongRowAt(1) == state->song[1]);

  auto reject = [&](const std::string& from, const std::string& to) {
    std::string text = valid;
    const auto at = text.find(from);
    assert(at != std::string::npos);
    text.replace(at, from.size(), to);
    WriteText(path, text);
    auto* old = loaded.sequence.get();
    assert(LoadProjectFile("song trip", loaded) == ProjectResult::Invalid);
    assert(loaded.sequence.get() == old);
  };
  reject("\nSONG 2 0\n", "\nSONG 65 0\n");
  reject("\nSONG 2 0\n", "\nSONG -1 0\n");
  reject("\nSONG 2 0\n", "\nSONG 2 2\n");
  reject("\nSONG 2 0\n", "\nSONG 2\n");
  reject("\nSONG 2 0\n", "\nSONG 3 0\n");  // A row missing.
  reject("\nSONG 2 0\n", "\nSONG 1 0\n");  // A row too many.
  reject("\nR 0 2 -1 0 4\n", "\nR 0 3 -1 0 4\n");  // Track 1 holds three patterns.
  reject("\nR 0 2 -1 0 4\n", "\nR 1 2 -1 0 4\n");  // Track 0 holds one.
  reject("\nR 0 2 -1 0 4\n", "\nR 0 2 -2 0 4\n");
  reject("\nR 0 2 -1 0 4\n", "\nR 0 2 -1 0 0\n");
  reject("\nR 0 2 -1 0 4\n", "\nR 0 2 -1 0 17\n");
  reject("\nR 0 2 -1 0 4\n", "\nR 0 2 -1 4\n");
  reject("\nR 0 2 -1 0 4\n", "\nR 0 2 -1 0 4 4\n");
  reject("\nR 0 2 -1 0 4\n", "\nR 0 2 x 0 4\n");
  reject("\nR 0 2 -1 0 4\n", "\nX 0 2 -1 0 4\n");
  reject("\n" + section, "\n");  // Version 3 needs its song section.

  // Version 3 files have no FX line (nor the play settings of version 6) and load with the default
  // master effects.
  const auto fxLine = valid.find("\nFX ");
  std::string v3 = valid;
  v3.erase(fxLine + 1, valid.find("\nTRACK 0 ") - fxLine);
  v3.replace(0, std::string("CARDENZA_PROJECT 7").size(), "CARDENZA_PROJECT 3");
  v3 = WithOneLengthPerTrack(v3);
  WriteText(path, v3);
  assert(LoadProjectFile("song trip", loaded) == ProjectResult::Ok && loaded.masterFx == ProjectData::DefaultMasterFx());

  // Version 2 files have no song section and load with an empty, looping song.
  std::string v2 = v3;
  v2.replace(0, std::string("CARDENZA_PROJECT 3").size(), "CARDENZA_PROJECT 2");
  WriteText(path, v2);
  assert(LoadProjectFile("song trip", loaded) == ProjectResult::Invalid);
  v2.erase(v2.find(section), section.size());
  WriteText(path, v2);
  assert(LoadProjectFile("song trip", loaded) == ProjectResult::Ok);
  assert(loaded.sequence->songLength == 0 && loaded.sequence->songLoop && loaded.sequence->tracks[1].patternCount == 3);
  assert(other->RestoreState(*loaded.sequence) && other->SongLength() == 0 && other->SongLoop());

  // An invalid song is refused before anything is written.
  project.sequence->song[1].repeats = 0;
  assert(SaveProjectFile("song trip", project, true) == ProjectResult::Invalid);
  project.sequence->song[1].repeats = 16;
  project.sequence->song[0].patterns[1] = 3;
  assert(SaveProjectFile("song trip", project, true) == ProjectResult::Invalid);
  project.sequence->song[0].patterns[1] = 2;
  project.sequence->songLength = kSongMaxRows + 1;
  assert(SaveProjectFile("song trip", project, true) == ProjectResult::Invalid);
  assert(ReadText(path) == v2);
  std::filesystem::remove(path);
}
}

int main() {
  using namespace synth;
  using namespace std::chrono_literals;
  projectDirectory = std::filesystem::temp_directory_path() /
      ("cardenza-project-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  std::filesystem::create_directory(projectDirectory);
  TestSequencerEditing();
  TestSelectionEditing();
  TestChordTogglesAsOne();
  TestHeldNotesEdit();
  TestSequencerSettings();
  TestLiveRecording();
  TestSwingPlayback();
  TestQueuedPatterns();
  TestTrackStepCounts();
  TestSongEditing();
  TestSongPlayback();
  TestSongCueAndToggle();
  TestProjectVersion2();
  TestLargestProject();
  TestProjectSong();
  Output output; // Outlives the sequencer's final stop callback.
  auto sequencer = std::make_unique<SynthSequencer>();
  sequencer->SetMidiSink(Sink, &output);
  const std::uint8_t notes[] = {60, 64, 67};
  sequencer->PressStep(0, notes, 3);
  sequencer->ReleaseStep(0);
  sequencer->AdjustTempo(200);
  assert(sequencer->BuildUiState(0).bpm == 300);
  sequencer->AdjustTempo(-1000);
  assert(sequencer->BuildUiState(0).bpm == 30);
  sequencer->AdjustTempo(90);
  assert(sequencer->CopyPattern(0, 1));
  auto snapshot = sequencer->CaptureState();
  assert(snapshot && SynthSequencer::ValidateState(*snapshot));
  assert(snapshot->tracks[0].patterns[1].steps[0].notes[0].active);

  ProjectData project;
  project.sequence = std::move(snapshot);
  project.activeTrack = 2;
  project.octave = 3;
  for (std::size_t i = 0; i < kSynthTrackCount; ++i) {
    project.kinds[i] = SynthEngineKind::Dx7;
    project.engines[i] = {"factory.1", "user.1", "Test Voice", {{"global.feedback", 5}}};
  }
  project.kinds[2] = SynthEngineKind::Sampler;
  project.samples[2] = "Exact.WAV";
  assert(SaveProjectFile("round trip", project, false) == ProjectResult::Ok);
  assert(SaveProjectFile("round trip", project, false) == ProjectResult::Exists);
  ProjectData loaded;
  assert(LoadProjectFile("round trip", loaded) == ProjectResult::Ok);
  assert(loaded.activeTrack == 2 && loaded.octave == 3 && loaded.samples[2] == "Exact.WAV");
  assert(loaded.engines[0].values.at("global.feedback") == 5);
  assert(loaded.sequence->tracks[0].patternCount == project.sequence->tracks[0].patternCount);
  assert(loaded.sequence->tracks[0].patterns[1].steps[0].notes[2].note == 67);
  const auto path = projectDirectory / "round trip.ccproj";
  std::ifstream input(path);
  const std::string valid{std::istreambuf_iterator<char>(input), {}};
  input.close();
  auto reject = [&](const std::string& text) {
    std::ofstream file(path, std::ios::trunc); file << text; file.close();
    auto* old = loaded.sequence.get();
    assert(LoadProjectFile("round trip", loaded) == ProjectResult::Invalid);
    assert(loaded.sequence.get() == old && loaded.activeTrack == 2);
  };
  reject(valid.substr(0, valid.size() / 2));
  reject(valid + "TRAILING");
  auto bad = valid; bad.replace(bad.find("SEQ 120"), 7, "SEQ 999"); reject(bad);
  bad = valid; bad.replace(bad.find("factory.1"), 9, std::string(4000, 'x')); reject(bad);
  assert(SaveProjectFile("../escape", project, true) == ProjectResult::Invalid);
  assert(!IsProjectNameValid("CON") && !IsProjectNameValid("com1"));
  assert(SaveProjectFile("round trip", project, true) == ProjectResult::Ok);
  std::filesystem::rename(path, path.string() + ".bak");
  assert(ListProjects() == std::vector<std::string>{"round trip"});
  assert(LoadProjectFile("round trip", loaded) == ProjectResult::Ok);
  assert(SaveProjectFile("round trip", project, false) == ProjectResult::Exists);

  // Force a MIDI batch to overlap Stop. Stop must wait for that batch, then release it.
  assert(sequencer->RestoreState(*project.sequence));
  {
    std::lock_guard<std::mutex> lock(output.mutex);
    output.packets.clear(); output.noteEntered = false; output.blockNoteOn = true;
  }
  sequencer->Play();
  {
    std::unique_lock<std::mutex> lock(output.mutex);
    assert(output.changed.wait_for(lock, 2s, [&] { return output.noteEntered; }));
  }
  std::atomic<bool> stopped{false};
  std::thread stop([&] { sequencer->Stop(); stopped = true; });
  std::this_thread::sleep_for(20ms);
  assert(!stopped.load());
  {
    std::lock_guard<std::mutex> lock(output.mutex);
    output.blockNoteOn = false;
  }
  output.changed.notify_all();
  stop.join();
  assert(stopped.load() && !sequencer->Playing());
  std::size_t count;
  {
    std::lock_guard<std::mutex> lock(output.mutex);
    assert(!output.packets.empty());
    assert(output.packets.back().status == midi::MidiStatus::ControlChange);
    count = output.packets.size();
  }
  std::this_thread::sleep_for(40ms);
  {
    std::lock_guard<std::mutex> lock(output.mutex);
    assert(output.packets.size() == count);
  }
  sequencer->Resume();
  assert(sequencer->Playing());
  sequencer->ToggleTrackMuted();
  assert(sequencer->BuildUiState(0).trackMuted);
  sequencer->ToggleTrackEnabled();
  assert(!sequencer->BuildUiState(0).trackEnabled);
  sequencer->Stop();
  project.sequence->tracks[0].patterns[0].steps.Edit(0).notes[0].microstep = 255;
  assert(!sequencer->RestoreState(*project.sequence));
  sequencer->Reset();
  assert(!sequencer->StepHasEvents(0));
  sequencer.reset();
  // Only files created by this test are removed.
  std::filesystem::remove(path.string() + ".bak");
  std::filesystem::remove(projectDirectory);
  std::cout << "Project round-trip/recovery/malformed input and sequencer transport ordering passed\n";
}
