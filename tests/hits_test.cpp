// HITS tests: a step's note fired 1..4 times inside its step. The sequencer is built with its test
// hooks (the clock is stepped by hand, one pulse at a time) and the project storage with its own (to
// write format 8 before the format bump).
#include "Applications/Synth/Runtime/ProjectStorage.h"
#include "Applications/Synth/Runtime/Sequencer/SynthSequencer.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

namespace {

std::filesystem::path projectDirectory;

}  // namespace

namespace synth::storage {
std::string ProjectDirectory() { return projectDirectory.string(); }
bool EnsureLayout() { return std::filesystem::is_directory(projectDirectory); }
}  // namespace synth::storage

namespace {

using midi::MidiStatus;
using synth::SequencerEditLane;
using synth::SynthSequencer;
using NoteEvent = synth::SynthSequencer::NoteEvent;

#define CHECK(condition)                                                                      \
  do                                                                                          \
  {                                                                                           \
    if (!(condition))                                                                         \
    {                                                                                         \
      std::fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #condition);     \
      std::abort();                                                                           \
    }                                                                                         \
  } while (0)

constexpr int kStep = synth::kSequencerPulsesPerStep;

struct Event {
  std::size_t track = 0;
  bool on = false;
  std::uint8_t note = 0;
  std::uint64_t tick = 0;
};

struct Capture {
  std::mutex mutex;
  std::vector<Event> events;
  std::uint64_t tick = 0;
};

void Sink(void* context, std::size_t track, const midi::MidiPacket& packet)
{
  auto& capture = *static_cast<Capture*>(context);
  std::lock_guard<std::mutex> lock(capture.mutex);
  const bool on = packet.status == MidiStatus::NoteOn && packet.data[2] > 0;
  const bool off = packet.status == MidiStatus::NoteOff || (packet.status == MidiStatus::NoteOn && packet.data[2] == 0);
  if (on || off) capture.events.push_back({track, on, packet.data[1], capture.tick});
}

// A sequencer on the manual clock; ticks count from Play().
struct Rig {
  Capture capture;
  std::unique_ptr<SynthSequencer> sequencer;

  Rig() : sequencer(std::make_unique<SynthSequencer>())
  {
    sequencer->UseManualClockForTest();
    sequencer->SetMidiSink(Sink, &capture);
  }

  SynthSequencer& seq() { return *sequencer; }

  void Run(std::uint64_t pulses)
  {
    for (std::uint64_t index = 0; index < pulses; ++index)
    {
      sequencer->AdvancePulsesForTest(1);
      ++capture.tick;
    }
  }

  // Plays from the top with these notes on track 0's pattern (16 steps unless `steps`).
  void PlayNotes(const std::vector<std::pair<std::size_t, NoteEvent>>& notes, std::uint8_t swing = 50, std::size_t steps = 16)
  {
    auto state = sequencer->CaptureState();
    CHECK(state);
    state->swing = swing;
    state->tracks[0].patternSteps[0] = static_cast<std::uint16_t>(steps);
    for (const auto& [step, note] : notes)
    {
      auto& slots = state->tracks[0].patterns[0].steps.Edit(step).notes;
      *std::find_if(slots.begin(), slots.end(), [](const NoteEvent& slot) { return !slot.active; }) = note;
    }
    CHECK(sequencer->RestoreState(*state));
    {
      std::lock_guard<std::mutex> lock(capture.mutex);
      capture.events.clear();
      capture.tick = 0;
    }
    sequencer->Play();
  }

  std::vector<Event> Events(std::uint8_t note)
  {
    std::lock_guard<std::mutex> lock(capture.mutex);
    std::vector<Event> events;
    for (const Event& event : capture.events)
      if (event.note == note) events.push_back(event);
    return events;
  }

  std::vector<std::uint64_t> Ticks(std::uint8_t note, bool on)
  {
    std::vector<std::uint64_t> ticks;
    for (const Event& event : Events(note))
      if (event.on == on) ticks.push_back(event.tick);
    return ticks;
  }
};

using Ticks = std::vector<std::uint64_t>;

// Where a note's hits start, from the top of the pattern: its offset, then a spacing apart.
Ticks Expected(std::uint64_t first, std::uint8_t hits, std::uint64_t pass = 0)
{
  Ticks ticks;
  for (std::uint8_t hit = 0; hit < hits; ++hit) ticks.push_back(pass + first + hit * synth::SequencerHitSpacingPulses(hits));
  return ticks;
}

void TestTimingOnWholePulses()
{
  // 2, 3 and 4 hits are 12, 8 and 6 pulses apart: whole pulses at 96 PPQ.
  CHECK(synth::SequencerHitSpacingPulses(1) == 24 && synth::SequencerHitSpacingPulses(2) == 12 && synth::SequencerHitSpacingPulses(3) == 8 &&
        synth::SequencerHitSpacingPulses(4) == 6);
  for (std::uint8_t hits = 1; hits <= synth::kSequencerMaxHits; ++hits)
    for (std::uint8_t microstep : {0, 3, 5})
      for (std::uint8_t swing : {50, 75})
        for (std::size_t step : {std::size_t{4}, std::size_t{5}})
        {
          Rig rig;
          rig.PlayNotes({{step, NoteEvent{60, 100, kStep, microstep, true, hits}}}, swing);
          rig.Run(16 * kStep);
          // Swing delays odd steps: at 75% an odd sixteenth starts 12 pulses late.
          const std::uint64_t first = step * kStep + microstep * synth::kSequencerPulsesPerMicrostep + (step % 2 == 1 && swing == 75 ? 12 : 0);
          CHECK(rig.Ticks(60, true) == Expected(first, hits));
          // A one-step note: every hit ends where the next starts, the last a spacing after it.
          Ticks offs = Expected(first, hits);
          for (auto& tick : offs) tick += synth::SequencerHitSpacingPulses(hits);
          CHECK(rig.Ticks(60, false) == offs);
          // Where a hit ends and the next starts on one pulse, the note-off goes first.
          const auto events = rig.Events(60);
          for (std::size_t index = 0; index + 1 < events.size(); ++index) CHECK(events[index].on != events[index + 1].on);
          CHECK(!events.empty() && events.front().on && !events.back().on);
          rig.seq().Stop();
        }
}

void TestGates()
{
  // A long note stutters in its first step and rings on to its end: 3 hits, 3 steps long.
  {
    Rig rig;
    rig.PlayNotes({{0, NoteEvent{62, 100, 3 * kStep, 0, true, 3}}});
    rig.Run(5 * kStep);
    CHECK(rig.Ticks(62, true) == (Ticks{0, 8, 16}));
    CHECK(rig.Ticks(62, false) == (Ticks{8, 16, 72}));
  }
  // A short note keeps its length on every hit: 4 hits of 4 pulses.
  {
    Rig rig;
    rig.PlayNotes({{2, NoteEvent{64, 100, 4, 0, true, 4}}});
    rig.Run(5 * kStep);
    CHECK(rig.Ticks(64, true) == (Ticks{48, 54, 60, 66}));
    CHECK(rig.Ticks(64, false) == (Ticks{52, 58, 64, 70}));
  }
  // A plain note is as it was: one hit, its length.
  {
    Rig rig;
    rig.PlayNotes({{1, NoteEvent{65, 100, 2 * kStep, 2, true, 1}}});
    rig.Run(5 * kStep);
    CHECK(rig.Ticks(65, true) == (Ticks{32}));
    CHECK(rig.Ticks(65, false) == (Ticks{80}));
  }
  CHECK(synth::SequencerNoteSoundingPulses(4, 4) == 22 && synth::SequencerNoteSoundingPulses(3, 72) == 72 &&
        synth::SequencerNoteSoundingPulses(1, 30) == 30);
}

void TestHitsReachLaterSteps()
{
  // The latest note (odd step, TIME 6, 75% swing, 4 hits) has its last hit two steps on: 32 + 18
  // pulses after its step starts. Inside the pattern...
  {
    Rig rig;
    rig.PlayNotes({{1, NoteEvent{67, 100, kStep, 5, true, 4}}}, 75);
    rig.Run(6 * kStep);
    CHECK(rig.Ticks(67, true) == Expected(kStep + 32, 4));
  }
  // ...and past the loop's end, into the next pass: the last step of a 16-step pattern.
  {
    Rig rig;
    rig.PlayNotes({{15, NoteEvent{69, 100, kStep, 5, true, 4}}}, 75);
    rig.Run(16 * kStep + 3 * kStep);
    CHECK(rig.Ticks(69, true) == Expected(15 * kStep + 32, 4));
    CHECK(rig.Ticks(69, false).size() == 4);
  }
  // A one-step pattern: the hits that fall past its end play at the top of the next pass, once.
  {
    Rig rig;
    rig.PlayNotes({{0, NoteEvent{71, 100, kStep, 5, true, 4}}}, 50, 1);
    rig.Run(2 * kStep);
    CHECK(rig.Ticks(71, true) == (Ticks{20, 26, 32, 38, 44}));  // the pass at 24 starts its own at 44
  }
}

void TestNoStuckNotes()
{
  // Stopping in the middle of a roll ends what sounds; nothing plays after.
  Rig rig;
  rig.PlayNotes({{0, NoteEvent{60, 100, kStep, 0, true, 4}}, {0, NoteEvent{64, 100, 4 * kStep, 0, true, 3}}});
  rig.Run(13);
  rig.seq().Stop();
  rig.Run(4 * kStep);
  for (std::uint8_t note : {60, 64})
  {
    const auto events = rig.Events(note);
    int sounding = 0;
    for (const Event& event : events) sounding = event.on ? 1 : 0;
    CHECK(sounding == 0);
    for (const Event& event : events) CHECK(event.tick <= 13);
  }
  std::array<std::uint32_t, 4> sounding{};
  rig.seq().AddSoundingNotes(0, sounding);
  CHECK(sounding == (std::array<std::uint32_t, 4>{}));
}

std::vector<std::uint8_t> HitsOf(const SynthSequencer& sequencer, std::size_t track, std::size_t step)
{
  std::array<SynthSequencer::StepNoteView, 8> views{};
  const std::size_t count = sequencer.StepNoteEvents(track, step, views);
  std::vector<std::uint8_t> hits;
  for (std::size_t index = 0; index < count; ++index) hits.push_back(views[index].hits);
  return hits;
}

std::uint8_t HitsOfNote(const SynthSequencer& sequencer, std::size_t step, std::uint8_t note)
{
  std::array<SynthSequencer::StepNoteView, 8> views{};
  const std::size_t count = sequencer.StepNoteEvents(0, step, views);
  for (std::size_t index = 0; index < count; ++index)
    if (views[index].note == note) return views[index].hits;
  return 0;
}

void TestStorageIsSixBytes()
{
  NoteEvent note;
  CHECK(!note.active && note.Hits() == 1);
  note.SetHits(4);
  CHECK(note.Hits() == 4);
  note.SetHits(9);
  CHECK(note.Hits() == 4);
  note.SetHits(0);
  CHECK(note.Hits() == 1);
  note = NoteEvent{};
  CHECK(note.Hits() == 1 && !note.active);
  static_assert(sizeof(NoteEvent) == 6 && sizeof(SynthSequencer::StepData) == 48, "a page of steps stays 768 bytes");
}

void TestLaneEdits()
{
  SynthSequencer sequencer;
  sequencer.SetActiveTrack(0);
  const std::uint8_t chord[] = {60, 64, 67};
  // NEW HITS: an empty selected step edits what notes written by hand start with.
  sequencer.SetStepSelected(2, true);
  sequencer.SetEditLane(SequencerEditLane::Hits);
  CHECK(sequencer.BuildUiState(0).valueLabel == "NEW HITS 1");
  CHECK(sequencer.AdjustSelectedSteps(1, false));
  CHECK(sequencer.BuildUiState(0).valueLabel == "NEW HITS 2");
  CHECK(sequencer.AdjustSelectedSteps(5, false) && sequencer.BuildUiState(0).valueLabel == "NEW HITS 4");
  CHECK(!sequencer.AdjustSelectedSteps(1, false));  // no wrap
  CHECK(sequencer.AdjustSelectedSteps(-1, false) && sequencer.BuildUiState(0).valueLabel == "NEW HITS 3");
  // Notes written by hand take it: a pad on a selected step, pads held and a step tapped.
  CHECK(sequencer.ToggleNotesOnSelectedSteps(chord, 3));
  CHECK(HitsOf(sequencer, 0, 2) == (std::vector<std::uint8_t>{3, 3, 3}));
  CHECK(sequencer.ToggleStepNotes(0, 7, chord, 1));
  CHECK(HitsOf(sequencer, 0, 7) == (std::vector<std::uint8_t>{3}));
  // A step with notes: ADD / SUB edit every note of it, 1..4, clamped like the other lanes.
  CHECK(sequencer.BuildUiState(0).valueLabel == "HITS 3");
  CHECK(sequencer.AdjustSelectedSteps(-1, false));
  CHECK(HitsOf(sequencer, 0, 2) == (std::vector<std::uint8_t>{2, 2, 2}) && sequencer.BuildUiState(0).valueLabel == "HITS 2");
  CHECK(sequencer.AdjustSelectedSteps(-1, false) && !sequencer.AdjustSelectedSteps(-1, false));
  CHECK(HitsOf(sequencer, 0, 2) == (std::vector<std::uint8_t>{1, 1, 1}));
  CHECK(sequencer.AdjustSelectedSteps(1, true));  // FN: to the end
  CHECK(HitsOf(sequencer, 0, 2) == (std::vector<std::uint8_t>{4, 4, 4}));
  // A held pad's notes alone (the same lane edit): E, then the step holds mixed values.
  const std::uint8_t e = 64;
  CHECK(sequencer.AdjustNotesOnSelectedSteps(&e, 1, -2, false));
  CHECK(HitsOfNote(sequencer, 2, 60) == 4 && HitsOfNote(sequencer, 2, 64) == 2 && HitsOfNote(sequencer, 2, 67) == 4);
  CHECK(sequencer.NotesLaneValue(&e, 1) == "2");
  const auto ui = sequencer.BuildUiState(0);
  CHECK(ui.editValueMixed && ui.valueLabel == "HITS MIXED" && ui.steps[2].hits == 4);
  // Two selected steps edited together; a step without the held note is left alone.
  sequencer.SetStepSelected(7, true);
  CHECK(sequencer.AdjustNotesOnSelectedSteps(&e, 1, 1, false));
  CHECK(HitsOfNote(sequencer, 2, 64) == 3 && HitsOf(sequencer, 0, 7) == (std::vector<std::uint8_t>{3}));
  CHECK(sequencer.AdjustSelectedSteps(-3, false));
  CHECK(HitsOf(sequencer, 0, 2) == (std::vector<std::uint8_t>{1, 1, 1}) && HitsOf(sequencer, 0, 7) == (std::vector<std::uint8_t>{1}));
  // The lanes: OK walks NOTE, LEN, VEL, TIME, HITS and round.
  sequencer.SetEditLane(SequencerEditLane::Micro);
  sequencer.CycleEditLane();
  CHECK(sequencer.EditLane() == SequencerEditLane::Hits && std::string(synth::SequencerEditLaneLabel(SequencerEditLane::Hits)) == "HITS");
  sequencer.CycleEditLane();
  CHECK(sequencer.EditLane() == SequencerEditLane::Note);
}

void TestRecordingWritesOneHit()
{
  // NEW HITS is for notes written by hand: what is played in (REC), a take laid down, step
  // recording, are single hits.
  Rig rig;
  rig.seq().SetActiveTrack(0);
  rig.seq().SetStepSelected(0, true);
  rig.seq().SetEditLane(SequencerEditLane::Hits);
  CHECK(rig.seq().AdjustSelectedSteps(3, false));
  rig.seq().ClearSelectedSteps();
  rig.PlayNotes({});
  rig.seq().SetRecordArmed(true);
  rig.Run(2 * kStep);
  rig.seq().RecordNoteOn(0, 72, 100);
  rig.Run(6);
  rig.seq().RecordNoteOff(0, 72);
  rig.seq().SetRecordArmed(false);
  rig.seq().Stop();
  // Wherever it was quantised to, it is one hit.
  std::size_t recorded = 0;
  for (std::size_t step = 0; step < 16; ++step)
    if (HitsOfNote(rig.seq(), step, 72) != 0)
    {
      CHECK(HitsOfNote(rig.seq(), step, 72) == 1);
      ++recorded;
    }
  CHECK(recorded == 1);
  CHECK(rig.seq().InsertNote(0, 0, 5, 0, 74, 100, kStep) && HitsOfNote(rig.seq(), 5, 74) == 1);
  const std::uint8_t notes[] = {76};
  CHECK(rig.seq().SetStepNotes(0, 9, notes, 1, 100) && HitsOfNote(rig.seq(), 9, 76) == 1);
  // A hand-written note still takes NEW HITS 4.
  CHECK(rig.seq().ToggleStepNotes(0, 11, notes, 1) && HitsOfNote(rig.seq(), 11, 76) == 4);
}

void TestCopiesCarryHits()
{
  SynthSequencer sequencer;
  sequencer.SetActiveTrack(0);
  const std::uint8_t c = 60, d = 62;
  sequencer.SetStepSelected(0, true);
  sequencer.SetEditLane(SequencerEditLane::Hits);
  CHECK(sequencer.ToggleNotesOnSelectedSteps(&c, 1) && sequencer.ToggleNotesOnSelectedSteps(&d, 1));
  CHECK(sequencer.AdjustNotesOnSelectedSteps(&c, 1, 2, false) && sequencer.AdjustNotesOnSelectedSteps(&d, 1, 1, false));
  CHECK(HitsOfNote(sequencer, 0, 60) == 3 && HitsOfNote(sequencer, 0, 62) == 2);
  // Step copy (PASTE on steps), pattern copy (PASTE on a pattern, DUP), quantise, a project snapshot.
  CHECK(sequencer.CopyStep(0, 6));
  CHECK(HitsOfNote(sequencer, 6, 60) == 3 && HitsOfNote(sequencer, 6, 62) == 2);
  sequencer.QuantizeActivePattern();
  CHECK(HitsOfNote(sequencer, 0, 60) == 3 && HitsOfNote(sequencer, 6, 62) == 2);
  CHECK(sequencer.CopyPattern(0, 1) && sequencer.SelectOrCreatePattern(1));
  CHECK(HitsOfNote(sequencer, 0, 60) == 3 && HitsOfNote(sequencer, 6, 62) == 2);
  const auto snapshot = sequencer.CaptureState();
  CHECK(snapshot && SynthSequencer::ValidateState(*snapshot));
  SynthSequencer other;
  CHECK(other.RestoreState(*snapshot));
  other.SetActiveTrack(0);
  CHECK(HitsOfNote(other, 6, 60) == 3 && HitsOfNote(other, 6, 62) == 2);
  // NEW HITS outside 1..4 is no state of ours.
  snapshot->tracks[0].defaultHits = 0;
  CHECK(!SynthSequencer::ValidateState(*snapshot));
  snapshot->tracks[0].defaultHits = 5;
  CHECK(!SynthSequencer::ValidateState(*snapshot));
}

std::string ReadText(const std::filesystem::path& path)
{
  std::ifstream input(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(input), {}};
}

void WriteText(const std::filesystem::path& path, const std::string& text)
{
  std::ofstream file(path, std::ios::trunc | std::ios::binary);
  file << text;
}

std::vector<std::string> Lines(const std::string& text)
{
  std::vector<std::string> lines;
  std::istringstream input(text);
  for (std::string line; std::getline(input, line);) lines.push_back(line);
  return lines;
}

std::size_t Words(const std::string& line)
{
  std::istringstream input(line);
  std::size_t count = 0;
  for (std::string word; input >> word;) ++count;
  return count;
}

// A project whose track 0 holds C (3 hits), D (1) on step 0 and E (4 hits, TIME 3) on step 5, NEW HITS 2.
synth::ProjectData HitsProject()
{
  synth::ProjectData project;
  project.sequence = std::make_unique<SynthSequencer::ProjectState>();
  for (std::size_t track = 0; track < synth::kSynthTrackCount; ++track)
  {
    project.kinds[track] = synth::SynthEngineKind::Juno;
    project.engines[track] = {"factory.2", "", "Pad", {}};
  }
  auto& track = project.sequence->tracks[0];
  track.defaultHits = 2;
  track.patterns[0].steps.Edit(0).notes[0] = NoteEvent{60, 100, kStep, 0, true, 3};
  track.patterns[0].steps.Edit(0).notes[1] = NoteEvent{62, 90, kStep, 0, true, 1};
  track.patterns[0].steps.Edit(5).notes[0] = NoteEvent{64, 80, 2 * kStep, 3, true, 4};
  CHECK(SynthSequencer::ValidateState(*project.sequence));
  return project;
}

std::uint8_t LoadedHits(const synth::ProjectData& project, std::size_t step, std::uint8_t pitch)
{
  for (const NoteEvent& note : project.sequence->tracks[0].patterns[0].steps[step].notes)
    if (note.active && note.note == pitch) return note.Hits();
  return 0;
}

void TestFormat()
{
  const synth::ProjectData project = HitsProject();
  // Until the format bump this build writes format 7, without the hits: the file is what format 7
  // always was, and the project loads with plain notes.
  static_assert(synth::kProjectWriteVersion == 7, "HITS wait for the format bump of the next test build (format 8).");
  const auto seven = projectDirectory / "seven.ccproj";
  CHECK(synth::SaveProjectFileAt(seven.string(), project) == synth::ProjectResult::Ok);
  const std::string sevenText = ReadText(seven);
  CHECK(sevenText.rfind("CARDENZA_PROJECT 7\n", 0) == 0);
  CHECK(sevenText.find("\nT 1 0 110 24 1 0 16\nNOTES 3\n0 0 60 100 24 0\n0 0 62 90 24 0\n0 5 64 80 48 3\n") != std::string::npos);
  synth::ProjectData loaded;
  CHECK(synth::LoadProjectFileAt(seven.string(), loaded) == synth::ProjectResult::Ok);
  CHECK(LoadedHits(loaded, 0, 60) == 1 && LoadedHits(loaded, 5, 64) == 1 && loaded.sequence->tracks[0].defaultHits == 1);
  // Written again it is the same file: format 7 reads and writes exactly as before.
  const auto again = projectDirectory / "again.ccproj";
  CHECK(synth::SaveProjectFileAt(again.string(), loaded) == synth::ProjectResult::Ok && ReadText(again) == sevenText);
  // Hits are no part of the format-7 fingerprint (they are not in the file).
  CHECK(synth::ProjectFingerprint(project) == synth::ProjectFingerprint(loaded));

  // Format 8, crafted from the format-7 file: NEW HITS after the new-note length on each T line, the
  // hits a seventh number on each note line.
  std::string crafted;
  std::size_t trackLines = 0;
  for (const std::string& line : Lines(sevenText))
  {
    std::string out = line;
    if (line == "CARDENZA_PROJECT 7") out = "CARDENZA_PROJECT 8";
    else if (line.rfind("T ", 0) == 0)
    {
      std::istringstream input(line);
      std::string t, count, active, velocity, length, rest;
      input >> t >> count >> active >> velocity >> length;
      std::getline(input, rest);
      out = "T " + count + " " + active + " " + velocity + " " + length + (trackLines++ == 0 ? " 2" : " 1") + rest;
    }
    else if (Words(line) == 6 && line.find_first_not_of("0123456789 ") == std::string::npos)
      out = line + (line == "0 0 60 100 24 0" ? " 3" : line == "0 5 64 80 48 3" ? " 4" : " 1");
    crafted += out + "\n";
  }
  const auto eight = projectDirectory / "eight.ccproj";
  WriteText(eight, crafted);
  CHECK(synth::LoadProjectFileAt(eight.string(), loaded) == synth::ProjectResult::Ok);
  CHECK(LoadedHits(loaded, 0, 60) == 3 && LoadedHits(loaded, 0, 62) == 1 && LoadedHits(loaded, 5, 64) == 4);
  CHECK(loaded.sequence->tracks[0].defaultHits == 2 && loaded.sequence->tracks[1].defaultHits == 1);
  // The writer behind the bump writes exactly that file.
  const auto written = projectDirectory / "written8.ccproj";
  CHECK(synth::SaveProjectFileAtVersionForTest(written.string(), project, 8) == synth::ProjectResult::Ok);
  CHECK(ReadText(written) == crafted);
  // Hits out of range are refused, as any value out of range.
  std::string bad = crafted;
  bad.replace(bad.find("0 5 64 80 48 3 4"), 16, "0 5 64 80 48 3 5");
  WriteText(eight, bad);
  CHECK(synth::LoadProjectFileAt(eight.string(), loaded) == synth::ProjectResult::Invalid);
  bad = crafted;
  bad.replace(bad.find("0 0 60 100 24 0 3"), 17, "0 0 60 100 24 0 0");
  WriteText(eight, bad);
  CHECK(synth::LoadProjectFileAt(eight.string(), loaded) == synth::ProjectResult::Invalid);
  // A format-7 file read as 8 (no hits on its lines) does not load.
  bad = sevenText;
  bad.replace(0, 18, "CARDENZA_PROJECT 8");
  WriteText(eight, bad);
  CHECK(synth::LoadProjectFileAt(eight.string(), loaded) == synth::ProjectResult::Invalid);
  // Nothing newer than 8 is read.
  bad = crafted;
  bad.replace(0, 18, "CARDENZA_PROJECT 9");
  WriteText(eight, bad);
  CHECK(synth::LoadProjectFileAt(eight.string(), loaded) == synth::ProjectResult::Invalid);
}

}  // namespace

int main()
{
  projectDirectory = std::filesystem::temp_directory_path() / ("cardenza-hits-test-" + std::to_string(std::rand()));
  std::filesystem::remove_all(projectDirectory);
  std::filesystem::create_directories(projectDirectory);
  TestStorageIsSixBytes();
  TestTimingOnWholePulses();
  TestGates();
  TestHitsReachLaterSteps();
  TestNoStuckNotes();
  TestLaneEdits();
  TestRecordingWritesOneHit();
  TestCopiesCarryHits();
  TestFormat();
  std::filesystem::remove_all(projectDirectory);
  std::printf("hits_test: all checks passed\n");
  return 0;
}
