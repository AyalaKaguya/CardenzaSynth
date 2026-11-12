#include "Applications/Synth/SynthApp.h"
#include "Applications/Synth/Runtime/ProjectStorage.h"
#include "Applications/Synth/Runtime/Sampler/SamplerWav.h"
#include "Applications/Synth/UI/SynthUiRenderer.h"
#include "Devices/DeviceAudio.h"
#include "Library/Synth/Core/Engines/Drums/DrumsSynthEngine.h"
#include "Library/Synth/Core/SynthStorage.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <thread>
#include <vector>
#if defined(_MSC_VER)
#include <crtdbg.h>
#endif

namespace {
std::filesystem::path storageRoot;
std::uint8_t brightness = Device::LED::kBrightnessMax;
std::vector<std::int16_t> microphone;
std::size_t microphoneCursor = 0;
bool microphoneRunning = false;
std::array<synth::audio::SharedSampleBuffer, synth::kSynthTrackCount> audioSamples;
std::array<std::atomic<unsigned>, synth::kSynthTrackCount> audioNoteOnCounts{};
std::array<std::atomic<unsigned>, synth::kSynthTrackCount> audioAllNotesOffCounts{};
std::array<std::atomic<int>, synth::kSynthTrackCount> audioLastNotes{};
std::array<std::atomic<int>, synth::kSynthTrackCount> audioLastVelocities{};
std::array<float, synth::kSynthTrackCount> audioTrackGains{{1.0f, 1.0f, 1.0f, 1.0f}};
std::array<bool, synth::kSynthTrackCount> audioTrackMuted{};
std::uint8_t displayBacklight = Device::Display::kDefaultBacklight;
std::uint8_t outputVolume = Device::Audio::kDefaultOutputVolume;
}

// Use the production app, engines, MIDI routing, serializers and UI model. Only
// device IO, audio rendering, pixels and the synth's storage folders are replaced: this is a
// state/flow test.
namespace synth::storage {
std::string SampleDirectory() { return (storageRoot / "Sample").string(); }
std::string ProjectDirectory() { return (storageRoot / "Project").string(); }
std::string PresetDirectory(std::string_view instrument) { return (storageRoot / "Preset" / std::string(instrument)).string(); }
std::string SessionDirectory() { return (storageRoot / "Session").string(); }
bool EnsureLayout() {
  std::error_code error;
  for (const char* directory : {"Sample", "Project", "Session", "Preset/DX7", "Preset/Juno", "Preset/Sampler", "Preset/Drums"}) {
    std::filesystem::create_directories(storageRoot / directory, error);
    if (error) return false;
  }
  return true;
}
}
namespace Device {
std::string BackendName() { return "project-test"; }
namespace LED {
void Clear() {}
void SetKey(std::size_t, std::size_t, std::uint8_t) {}
void SetEnabled(bool) {}
std::uint8_t GlobalBrightness() { return brightness; }
void SetGlobalBrightness(std::uint8_t value) { brightness = value; }
}
namespace Display {
MutableFrameBufferView MutableFrameBuffer() { return {}; }
void SetBacklight(std::uint8_t level) { displayBacklight = level; }
std::uint8_t Backlight() { return displayBacklight; }
}
namespace Audio {
void SetOutputVolume(std::uint8_t percent) { outputVolume = std::min<std::uint8_t>(percent, 100); }
std::uint8_t OutputVolume() { return outputVolume; }
InputStreamInfo InputInfo() { return {}; }
InputStatus StartInput() { microphoneRunning = true; microphoneCursor = 0; return InputStatus::Ready; }
void StopInput() { microphoneRunning = false; }
InputStatus ReadInputFrames(std::int16_t* destination, std::size_t count, std::size_t& read, std::uint32_t) {
  read = std::min(count, microphone.size() - microphoneCursor);
  if (read != 0) std::copy_n(microphone.data() + microphoneCursor, read, destination);
  microphoneCursor += read;
  return InputStatus::Ready;
}
}
}
namespace device {
void SetSynthAudioRenderProvider(SynthAudioRenderProvider) {}
SynthAudioPipelineBinding ConnectSynthAudioPipeline(std::size_t index) { return {index, true, false, "project-test"}; }
}
namespace synth::audio {
void EnsureEngine(std::size_t, SynthEngineKind) {}
void SetEnginePreset(std::size_t, std::string_view) {}
void SetEngineParameter(std::size_t, std::string_view, int) {}
void SetSamplerFrames(std::size_t index, SharedSampleBuffer sample, std::uint32_t) { audioSamples.at(index) = std::move(sample); }
void NoteOn(std::size_t index, std::uint8_t note, std::uint8_t velocity) {
  audioLastNotes.at(index) = note;
  audioLastVelocities.at(index) = velocity;
  ++audioNoteOnCounts.at(index);
}
void NoteOff(std::size_t, std::uint8_t) {}
void AllNotesOff(std::size_t index) { ++audioAllNotesOffCounts.at(index); }
void WarmUp() {}
std::size_t RenderFrames(std::int16_t* frames, std::size_t count) { std::fill_n(frames, count, std::int16_t{0}); return count; }
void SetTrackGain(std::size_t index, float gain) { if (index < audioTrackGains.size()) audioTrackGains[index] = gain > 0.0f ? std::min(gain, 1.0f) : 0.0f; }
float TrackGain(std::size_t index) { return index < audioTrackGains.size() ? audioTrackGains[index] : 1.0f; }
void SetTrackMuted(std::size_t index, bool muted) { if (index < audioTrackMuted.size()) audioTrackMuted[index] = muted; }
bool TrackMuted(std::size_t index) { return index < audioTrackMuted.size() && audioTrackMuted[index]; }
float TrackActivity(std::size_t) { return 0.0f; }
float OutputPeak() { return 0.0f; }
}
namespace ui {
Canvas::Canvas(Device::Display::MutableFrameBufferView buffer) : frameBuffer_(buffer) {}
}
namespace synth {
void RenderSynthUiFrame(ui::Canvas&, const SynthUiModel&, const ISynthEngine*,
    const std::array<bool, kKeyboardMatrixRows * kKeyboardMatrixColumns>&) {}
}

namespace {
using namespace synth;

ISynthEngine* ActiveEngine(apps::SynthApp& app) { return app.Rack().Slot(app.Ui().ActiveTrack()).engine.get(); }
UiScreenSnapshot Screen(apps::SynthApp& app) { return app.Ui().CurrentScreenSnapshot(ActiveEngine(app)); }
void Tap(apps::SynthApp& app, std::size_t row, std::size_t column) {
  app.HandleMatrixKeyEvent(row, column, true);
  app.HandleMatrixKeyEvent(row, column, false);
}
void Select(apps::SynthApp& app, ContextActionId action, const std::string& id = "") {
  const auto menu = app.Ui().CurrentMenuCells(ActiveEngine(app));
  const auto columns = app.Ui().ContextMenuColumnCount();
  for (std::size_t i = 0; i < columns * 4; ++i) {
    if ((action != ContextActionId::None && menu[i].actionId == action) || (!id.empty() && menu[i].itemId == id)) {
      assert(!menu[i].disabled);
      Tap(app, columns == 1 ? i : i / 2, columns == 1 ? kHomeRightBarColumn : kContextMenuFirstColumn + i % 2);
      return;
    }
  }
  std::cerr << "Unreachable action " << static_cast<int>(action) << " / " << id << " in";
  for (const auto& cell : menu) std::cerr << ' ' << cell.label;
  const auto screen = Screen(app);
  std::cerr << " (screen: " << screen.primaryText << " / " << screen.secondaryText
            << ", track: " << app.Ui().ActiveTrack() << ", input running: " << microphoneRunning << ")\n";
  assert(false);
}
void Enter(apps::SynthApp& app, std::size_t modifierRow, ContextActionId action, const std::string& id = "") {
  app.HandleHomeButtonEvent(true);
  app.HandleMatrixKeyEvent(modifierRow, kHomeLeftBarColumn, true);
  std::this_thread::sleep_for(std::chrono::milliseconds(210));
  app.Tick();
  Select(app, action, id); // Choosing while held latches the menu through the normal input path.
  app.HandleMatrixKeyEvent(modifierRow, kHomeLeftBarColumn, false);
}
void ProjectMenu(apps::SynthApp& app) { Enter(app, 3, ContextActionId::None, "sys.project"); }
void TypeText(apps::SynthApp& app, const std::string& name) {
  const std::array<std::string, 4> rows{{"1234567890-=", "qwertyuiop[]", " asdfghjkl; ", "  zxcvbnm,. "}};
  for (char character : name) {
    bool found = false;
    for (std::size_t row = 0; row < rows.size() && !found; ++row) {
      const auto position = rows[row].find(character);
      if (position != std::string::npos) { Tap(app, row, position + 1); found = true; }
    }
    assert(found);
  }
}
void TypeProjectName(apps::SynthApp& app, const std::string& name) {
  assert(name.size() <= 24); // Match the public project-name entry limit.
  for (int i = 0; Screen(app).secondaryText != "_" && i < 25; ++i) Select(app, ContextActionId::ProjectNameBackspace);
  assert(Screen(app).secondaryText == "_");
  TypeText(app, name);
  assert(Screen(app).secondaryText == name + "_");
}
void TypeSampleName(apps::SynthApp& app, const std::string& name) {
  for (int i = 0; Screen(app).secondaryText != "_" && i < 25; ++i) Select(app, ContextActionId::SamplerNameBackspace);
  assert(Screen(app).secondaryText == "_");
  TypeText(app, name);
  assert(Screen(app).secondaryText == name + "_");
}
void SaveAs(apps::SynthApp& app, const std::string& name) {
  ProjectMenu(app);
  Select(app, ContextActionId::ProjectSaveAs);
  TypeProjectName(app, name);
  Select(app, ContextActionId::ProjectNameSave);
  assert(Screen(app).secondaryText == "SAVED");
}
void RequestLoad(apps::SynthApp& app, const std::string& name, bool confirm) {
  ProjectMenu(app);
  Select(app, ContextActionId::ProjectLoad);
  const auto names = ListProjects();
  const auto entry = std::find(names.begin(), names.end(), name);
  assert(entry != names.end());
  for (auto it = names.begin(); it != entry; ++it) Select(app, ContextActionId::PresetNext);
  Select(app, ContextActionId::ProjectLoad);
  assert(Screen(app).primaryText == "Load project?");
  Select(app, confirm ? ContextActionId::ProjectConfirm : ContextActionId::Back);
}
void RequestNew(apps::SynthApp& app, bool confirm) {
  ProjectMenu(app);
  Select(app, ContextActionId::ProjectNew);
  assert(Screen(app).primaryText == "New project?");
  Select(app, confirm ? ContextActionId::ProjectConfirm : ContextActionId::Back);
}
void AssertEngineEqual(const SynthEngineState& left, const SynthEngineState& right) {
  assert(left.basePresetId == right.basePresetId && left.presetId == right.presetId);
  assert(left.presetName == right.presetName && left.values == right.values);
}
void AssertProjectEqual(const ProjectData& left, const ProjectData& right) {
  assert(left.kinds == right.kinds && left.samples == right.samples && left.activeTrack == right.activeTrack);
  assert(left.octave == right.octave && left.root == right.root && left.scale == right.scale && left.layout == right.layout);
  assert(left.ledBrightness == right.ledBrightness && left.sequence && right.sequence);
  assert(left.sequence->bpm == right.sequence->bpm);
  for (std::size_t t = 0; t < kSynthTrackCount; ++t) {
    AssertEngineEqual(left.engines[t], right.engines[t]);
    const auto& a = left.sequence->tracks[t];
    const auto& b = right.sequence->tracks[t];
    assert(a.patternCount == b.patternCount && a.activePattern == b.activePattern);
    assert(a.enabled == b.enabled && a.muted == b.muted);
    assert(a.defaultVelocity == b.defaultVelocity && a.defaultLengthPulses == b.defaultLengthPulses);
    for (std::size_t p = 0; p < a.patternCount; ++p) for (std::size_t s = 0; s < kSequencerStepCount; ++s)
      for (std::size_t n = 0; n < a.patterns[p].steps[s].notes.size(); ++n) {
        const auto& x = a.patterns[p].steps[s].notes[n];
        const auto& y = b.patterns[p].steps[s].notes[n];
        assert(x.active == y.active);
        if (x.active) assert(x.note == y.note && x.velocity == y.velocity && x.lengthPulses == y.lengthPulses && x.microstep == y.microstep);
      }
  }
}
struct Session {
  std::array<ISynthEngine*, kSynthTrackCount> instances{};
  std::array<SynthEngineState, kSynthTrackCount> engines;
  std::array<int, 4> keyboard;
  std::array<audio::SharedSampleBuffer, kSynthTrackCount> samples;
  std::uint8_t ledBrightness;
  std::uint16_t tempo;
};
Session CaptureSession(apps::SynthApp& app) {
  Session session;
  for (std::size_t track = 0; track < kSynthTrackCount; ++track) {
    session.instances[track] = app.Rack().Slot(track).engine.get();
    session.engines[track] = session.instances[track]->CaptureState();
  }
  session.keyboard = app.Ui().KeyboardSettings();
  session.samples = audioSamples;
  session.ledBrightness = brightness;
  session.tempo = Screen(app).sequencer.bpm;
  return session;
}
void AssertSessionUnchanged(apps::SynthApp& app, const Session& before) {
  const auto after = CaptureSession(app);
  assert(after.instances == before.instances && after.keyboard == before.keyboard && after.samples == before.samples);
  assert(after.ledBrightness == before.ledBrightness && after.tempo == before.tempo);
  for (std::size_t track = 0; track < kSynthTrackCount; ++track) AssertEngineEqual(after.engines[track], before.engines[track]);
}
void AssertEngineChangeBlocked(apps::SynthApp& app) {
  const auto session = CaptureSession(app);
  const auto sampler = Screen(app);
  const auto cursor = microphoneCursor;
  Enter(app, 0, ContextActionId::None, "snd.engine");
  Select(app, ContextActionId::EngineNext);
  Select(app, ContextActionId::EngineLoad);
  assert(Screen(app).secondaryText == "SAVE SAMPLE FIRST");
  assert(app.Rack().Slot(2).kind == SynthEngineKind::Sampler);
  AssertSessionUnchanged(app, session);
  Enter(app, 1, ContextActionId::SamplerSelect);
  const auto after = Screen(app);
  assert(after.samplerRecording == sampler.samplerRecording && microphoneRunning == sampler.samplerRecording);
  assert(after.samplerHasDraft == sampler.samplerHasDraft && after.samplerDraftName == sampler.samplerDraftName);
  assert(after.samplerFrameCount == sampler.samplerFrameCount && after.samplerWaveform == sampler.samplerWaveform);
  assert(microphoneCursor == cursor);
}
void TestBrowserFileActions(apps::SynthApp& app) {
  const auto target = storageRoot / "Sample" / "target.wav";
  const auto renamed = storageRoot / "Sample" / "target-new.wav";
  const auto tone = storageRoot / "Sample" / "tone.WAV";
  const auto collision = storageRoot / "Sample" / "taken.wav";
  const std::array<std::int16_t, 8> frames{{300, -300, 200, -200, 100, -100, 50, -50}};
  assert(sampler::WriteMono16Wav(target.string(), frames.data(), frames.size(), 22050));
  assert(sampler::WriteMono16Wav(collision.string(), frames.data(), frames.size(), 22050));
  app.Ui().SetActiveTrack(2);

  // OPT manages the highlighted file even while the sampler slot is empty.
  Enter(app, 1, ContextActionId::SamplerSelect);
  TypeText(app, "target");
  Tap(app, 3, 1);
  assert(Screen(app).headerPrimary == "FILE" && Screen(app).secondaryText.find("target.wav") == 0);
  assert(!Screen(app).samplerHasSample && !audioSamples[2]);
  Select(app, ContextActionId::Back);
  assert(Screen(app).showPresetBrowser && Screen(app).primaryText == "target_");

  Enter(app, 1, ContextActionId::SamplerSelect);
  TypeText(app, "tone");
  Select(app, ContextActionId::PresetLoad);
  assert(Screen(app).samplerSampleName == "tone" && audioSamples[2]);
  const auto loaded = CaptureSession(app);
  Enter(app, 1, ContextActionId::SamplerSelect);
  TypeText(app, "target");
  Tap(app, 3, 1);
  Select(app, ContextActionId::SamplerRename);
  TypeSampleName(app, "taken");
  Select(app, ContextActionId::SamplerNameSave);
  assert(Screen(app).primaryText == "REN ERR" && Screen(app).secondaryText == "taken_");
  assert(std::filesystem::exists(target) && std::filesystem::exists(collision) && std::filesystem::exists(tone));
  AssertSessionUnchanged(app, loaded);
  TypeSampleName(app, "target-new");
  Select(app, ContextActionId::SamplerNameSave);
  assert(Screen(app).primaryText == "RENAMED" && Screen(app).secondaryText.find("target-new.wav") == 0);
  assert(!std::filesystem::exists(target) && std::filesystem::exists(renamed));
  assert(Screen(app).samplerSampleName == "tone");
  AssertSessionUnchanged(app, loaded);
  Select(app, ContextActionId::Back);
  assert(Screen(app).showPresetBrowser && Screen(app).primaryText == "target_");
  assert(Screen(app).secondaryText.find("target-new.wav") != std::string::npos);
  Tap(app, 3, 1);
  Select(app, ContextActionId::SamplerDelete);
  assert(Screen(app).samplerDeleteConfirm && std::filesystem::exists(renamed));
  Select(app, ContextActionId::Back);
  assert(!Screen(app).samplerDeleteConfirm && Screen(app).primaryText == "File actions");
  assert(std::filesystem::exists(renamed));
  Select(app, ContextActionId::SamplerDelete);
  assert(Screen(app).samplerDeleteConfirm && std::filesystem::exists(renamed));
  Select(app, ContextActionId::SamplerDelete);
  assert(!Screen(app).samplerDeleteConfirm && Screen(app).primaryText == "DELETED");
  assert(!std::filesystem::exists(renamed) && std::filesystem::exists(tone));
  assert(Screen(app).samplerSampleName == "tone");
  AssertSessionUnchanged(app, loaded);
  Select(app, ContextActionId::Back);
  assert(Screen(app).showPresetBrowser && Screen(app).primaryText == "target_");
  assert(Screen(app).secondaryText == "No matches");
  RequestNew(app, true);
  assert(!audioSamples[2]);
}
void TestRecordingFileBack(apps::SynthApp& app) {
  const auto session = CaptureSession(app);
  Enter(app, 1, ContextActionId::SamplerSelect);
  Select(app, ContextActionId::PresetLoad);
  assert(Screen(app).showPresetBrowser && Screen(app).primaryText == "SAVE SAMPLE FIRST");
  Select(app, ContextActionId::Back);
  assert(!Screen(app).showPresetBrowser && Screen(app).headerPrimary == "Sampler");
  assert(microphoneRunning);
  Select(app, ContextActionId::SamplerSelect);
  Tap(app, 3, 1);
  Select(app, ContextActionId::SamplerRename);
  TypeSampleName(app, "blocked");
  Select(app, ContextActionId::SamplerNameSave);
  assert(Screen(app).primaryText == "SAVE SAMPLE FIRST");
  Select(app, ContextActionId::Back);
  assert(Screen(app).headerPrimary == "FILE" && Screen(app).primaryText == "File actions");
  Select(app, ContextActionId::Back);
  assert(Screen(app).showPresetBrowser);
  Select(app, ContextActionId::Back);
  assert(!Screen(app).showPresetBrowser && microphoneRunning);
  assert(std::filesystem::exists(storageRoot / "Sample" / "tone.WAV"));
  assert(!std::filesystem::exists(storageRoot / "Sample" / "blocked.wav"));
  AssertSessionUnchanged(app, session);
}
void AddStep(apps::SynthApp& app, std::size_t step) {
  app.HandleHomeButtonEvent(true);
  app.HandleMatrixKeyEvent(3, 1, true);
  Tap(app, SequencerStepRow(step), SequencerStepColumn(step));
  app.HandleMatrixKeyEvent(3, 1, false);
  app.Tick();
  assert(Screen(app).sequencer.steps[step].hasEvents);
}
void TestSequencerWorkflow() {
  apps::SynthApp app;
  app.Setup({});
  assert(app.Ui().ActiveTrack() == 0);
  assert(Screen(app).sequencer.populatedStepCount == 0);
  const auto noteA = app.Ui().HomePerformanceMidiNote(3, 1);
  const auto noteB = app.Ui().HomePerformanceMidiNote(3, 3);
  assert(noteA < noteB);
  const auto noteName = [](std::uint8_t note) {
    const std::array<const char*, 12> names{{"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"}};
    return std::string(names[note % 12]) + std::to_string(note / 12 - 1);
  };

  // Both public recording gestures must produce the same chord, without a
  // private sequencer API or a project fixture preloading the notes.
  app.HandleMatrixKeyEvent(3, 1, true);
  app.HandleMatrixKeyEvent(3, 3, true);
  Tap(app, SequencerStepRow(0), SequencerStepColumn(0));
  app.HandleMatrixKeyEvent(3, 1, false);
  app.HandleMatrixKeyEvent(3, 3, false);
  app.HandleMatrixKeyEvent(SequencerStepRow(5), SequencerStepColumn(5), true);
  Tap(app, 3, 1);
  Tap(app, 3, 3);
  auto state = Screen(app).sequencer;
  assert(state.primaryHeldStep == 5 && state.heldStepCount == 1 && state.noteCount == 2);
  assert(state.noteNames == noteName(noteA) + " " + noteName(noteB));
  assert(state.populatedStepCount == 2 && state.patternHasEvents[0]);

  // Lane edits affect every note of the held steps. Releasing a lane key keeps that
  // editing lane visibly selected until the steps release.
  app.HandleMatrixKeyEvent(SequencerStepRow(0), SequencerStepColumn(0), true);
  state = Screen(app).sequencer;
  assert(state.primaryHeldStep == 0 && state.heldStepCount == 2);
  Tap(app, 0, kHomeLeftBarColumn);
  assert(Screen(app).sequencer.editLane == SequencerEditLane::Note);
  Tap(app, 1, kHomeLeftBarColumn);
  assert(Screen(app).sequencer.editLane == SequencerEditLane::Length && Screen(app).leftBarItems[1].active);
  Tap(app, 3, 4); // Four 16th-note steps.
  assert(Screen(app).sequencer.editValueIndex == 3);
  Tap(app, 2, kHomeLeftBarColumn);
  assert(Screen(app).sequencer.editLane == SequencerEditLane::Velocity && Screen(app).leftBarItems[2].active);
  Tap(app, 3, 5); // Velocity bucket 88.
  assert(Screen(app).sequencer.editValueIndex == 4);
  Tap(app, 3, kHomeLeftBarColumn);
  assert(Screen(app).sequencer.editLane == SequencerEditLane::Micro && Screen(app).leftBarItems[3].active);
  Tap(app, 3, 4); // Displayed slot 4/6, stored zero-based offset 3.
  assert(Screen(app).sequencer.editValueIndex == 3);
  app.HandleMatrixKeyEvent(SequencerStepRow(0), SequencerStepColumn(0), false);
  app.HandleMatrixKeyEvent(SequencerStepRow(5), SequencerStepColumn(5), false);
  assert(!Screen(app).sequencer.hasHeldSteps && Screen(app).sequencer.editLane == SequencerEditLane::Note);

  SaveAs(app, "sequencer-edited");
  ProjectData edited;
  assert(LoadProjectFile("sequencer-edited", edited) == ProjectResult::Ok && edited.sequence);
  for (const std::size_t step : {0u, 5u}) {
    const auto& notes = edited.sequence->tracks[0].patterns[0].steps[step].notes;
    const auto a = std::find_if(notes.begin(), notes.end(), [noteA](const auto& note) { return note.active && note.note == noteA; });
    const auto b = std::find_if(notes.begin(), notes.end(), [noteB](const auto& note) { return note.active && note.note == noteB; });
    assert(a != notes.end() && b != notes.end());
    assert(a->lengthPulses == 4 * kSequencerPulsesPerStep && a->velocity == 88 && a->microstep == 3);
    assert(b->lengthPulses == 4 * kSequencerPulsesPerStep && b->velocity == 88 && b->microstep == 3);
  }

  // A single held OCT exposes Pattern actions. COPY can keep the layer open
  // after OCT releases, without falling through to the normal track keys.
  app.HandleHomeButtonEvent(true);
  app.HandleMatrixKeyEvent(0, kHomeRightBarColumn, true);
  auto shiftedScreen = Screen(app);
  assert(shiftedScreen.sequencer.patternSelectorVisible && shiftedScreen.sequencer.shiftActive && app.Ui().ActiveTrack() == 0);
  assert(shiftedScreen.leftBarItems[0].label == "CLR" && shiftedScreen.leftBarItems[1].label == "COPY" &&
         shiftedScreen.leftBarItems[2].label == "QNT");
  assert(!shiftedScreen.sequencer.steps[9].hasEvents);

  app.HandleMatrixKeyEvent(1, kHomeLeftBarColumn, true);
  app.HandleMatrixKeyEvent(0, kHomeRightBarColumn, false);
  assert(Screen(app).sequencer.copyMode && Screen(app).leftBarItems[1].active);
  assert(Screen(app).sequencer.patternSelectorVisible);
  assert(app.Ui().ActiveTrack() == 0);
  Tap(app, SequencerStepRow(0), SequencerStepColumn(0));
  state = Screen(app).sequencer;
  assert(state.hasCopySource && !state.copySourceIsPattern && state.copySourceIndex == 0);
  Tap(app, SequencerStepRow(9), SequencerStepColumn(9));
  assert(Screen(app).sequencer.steps[9].hasEvents && app.Ui().ActiveTrack() == 0);
  app.HandleMatrixKeyEvent(1, kHomeLeftBarColumn, false);
  assert(!Screen(app).sequencer.patternSelectorVisible && !Screen(app).sequencer.copyMode);

  // Holding a target before QNT quantizes it immediately; the original source
  // must retain its deliberately offset note in the save.
  app.HandleMatrixKeyEvent(0, kHomeRightBarColumn, true);
  app.HandleMatrixKeyEvent(SequencerStepRow(9), SequencerStepColumn(9), true);
  assert(Screen(app).sequencer.primaryHeldStep == 9 && Screen(app).sequencer.heldStepCount == 1);
  app.HandleMatrixKeyEvent(2, kHomeLeftBarColumn, true);
  assert(Screen(app).sequencer.quantizeMode && Screen(app).leftBarItems[2].active);
  assert(app.Ui().ActiveTrack() == 0);
  app.HandleMatrixKeyEvent(2, kHomeLeftBarColumn, false);
  app.HandleMatrixKeyEvent(SequencerStepRow(9), SequencerStepColumn(9), false);

  app.HandleMatrixKeyEvent(0, kHomeLeftBarColumn, true);
  assert(Screen(app).sequencer.clearMode && Screen(app).leftBarItems[0].active);
  Tap(app, SequencerStepRow(5), SequencerStepColumn(5));
  app.HandleMatrixKeyEvent(0, kHomeLeftBarColumn, false);
  state = Screen(app).sequencer;
  assert(!state.steps[5].hasEvents && state.steps[0].hasEvents && state.steps[9].hasEvents);
  assert(state.populatedStepCount == 2 && state.patternSelectorVisible && app.Ui().ActiveTrack() == 0);

  app.HandleMatrixKeyEvent(0, kHomeRightBarColumn, false);
  const auto homeAfterShift = Screen(app);
  assert(app.Ui().Mode() == UiMode::Home && app.Ui().ActiveTrack() == 0);
  assert(!homeAfterShift.sequencer.patternSelectorVisible && !homeAfterShift.sequencer.shiftActive && !homeAfterShift.sequencer.hasHeldSteps);
  assert(homeAfterShift.leftBarItems[0].label == "TRK" && homeAfterShift.leftBarItems[1].label == "SND" &&
         homeAfterShift.leftBarItems[2].label == "KEY" && homeAfterShift.leftBarItems[3].label == "SYS");
  SaveAs(app, "sequencer-shift");
  ProjectData shifted;
  assert(LoadProjectFile("sequencer-shift", shifted) == ProjectResult::Ok && shifted.sequence);
  assert(shifted.activeTrack == 0);
  const auto& shiftedSteps = shifted.sequence->tracks[0].patterns[0].steps;
  const auto& sourceNotes = shiftedSteps[0].notes;
  const auto& copiedNotes = shiftedSteps[9].notes;
  assert(std::count_if(sourceNotes.begin(), sourceNotes.end(), [](const auto& note) { return note.active; }) == 2);
  assert(std::count_if(copiedNotes.begin(), copiedNotes.end(), [](const auto& note) { return note.active; }) == 2);
  assert(std::none_of(shiftedSteps[5].notes.begin(), shiftedSteps[5].notes.end(), [](const auto& note) { return note.active; }));
  for (const auto pitch : {noteA, noteB}) {
    const auto& originalNotes = edited.sequence->tracks[0].patterns[0].steps[0].notes;
    const auto original = std::find_if(originalNotes.begin(), originalNotes.end(), [pitch](const auto& note) { return note.active && note.note == pitch; });
    const auto source = std::find_if(sourceNotes.begin(), sourceNotes.end(), [pitch](const auto& note) { return note.active && note.note == pitch; });
    const auto copied = std::find_if(copiedNotes.begin(), copiedNotes.end(), [pitch](const auto& note) { return note.active && note.note == pitch; });
    assert(original != originalNotes.end() && source != sourceNotes.end() && copied != copiedNotes.end());
    assert(source->lengthPulses == original->lengthPulses && source->velocity == original->velocity && source->microstep == original->microstep);
    assert(copied->lengthPulses == source->lengthPulses && copied->velocity == source->velocity && copied->microstep == 0);
  }

  // Playback reaches the actual app audio sink; Stop sends a release and permits
  // no later note-on from an already prepared sequencer batch.
  app.HandleHomeButtonEvent(true);
  const auto beforePlay = audioNoteOnCounts[0].load();
  Tap(app, 3, kHomeRightBarColumn);
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
  while (audioNoteOnCounts[0].load() == beforePlay && std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  app.Tick();
  assert(Screen(app).sequencer.playing && audioNoteOnCounts[0].load() > beforePlay);
  const auto beforeStop = audioAllNotesOffCounts[0].load();
  Tap(app, 2, kHomeRightBarColumn);
  assert(!Screen(app).sequencer.playing && audioAllNotesOffCounts[0].load() > beforeStop);
  const auto afterStop = audioNoteOnCounts[0].load();
  std::this_thread::sleep_for(std::chrono::milliseconds(30));
  assert(audioNoteOnCounts[0].load() == afterStop);

  // Context pages must show their own data and immediately follow T-/T+.
  Enter(app, 3, ContextActionId::None, "sys.seq");
  const auto overview = Screen(app);
  Select(app, ContextActionId::None, "sys.seq.tempo");
  const auto tempo = Screen(app);
  assert(tempo.focusItemId == "sys.seq.tempo" && tempo.primaryText != overview.primaryText);
  Select(app, ContextActionId::SequencerTempoInc);
  assert(Screen(app).sequencer.bpm == 121);
  Select(app, ContextActionId::Back);
  Select(app, ContextActionId::None, "sys.seq.track");
  Select(app, ContextActionId::SequencerTrackNext);
  assert(app.Ui().ActiveTrack() == 1 && Screen(app).sequencer.trackEnabled && !Screen(app).sequencer.trackMuted);
  Select(app, ContextActionId::SequencerMute);
  Select(app, ContextActionId::SequencerEnable);
  assert(Screen(app).sequencer.trackMuted && !Screen(app).sequencer.trackEnabled);
  Select(app, ContextActionId::SequencerTrackPrev);
  assert(app.Ui().ActiveTrack() == 0 && Screen(app).sequencer.trackEnabled && !Screen(app).sequencer.trackMuted);
  Select(app, ContextActionId::SequencerTrackNext);
  assert(app.Ui().ActiveTrack() == 1 && Screen(app).sequencer.trackMuted && !Screen(app).sequencer.trackEnabled);
  Select(app, ContextActionId::SequencerTrackPrev);
  Select(app, ContextActionId::Back);
  Select(app, ContextActionId::None, "sys.seq.pattern");
  const auto pattern = Screen(app);
  assert(pattern.focusItemId == "sys.seq.pattern" && pattern.primaryText != tempo.primaryText);
  Select(app, ContextActionId::SequencerPatternCountInc);
  Select(app, ContextActionId::SequencerPatternNext);
  assert(Screen(app).sequencer.activePattern == 1 && Screen(app).sequencer.patternCount == 2);
  AddStep(app, 3);

  // CNT- removes the last pattern, including when another pattern is selected.
  // Existing musical content requires an explicit confirmation.
  Enter(app, 3, ContextActionId::None, "sys.seq");
  Select(app, ContextActionId::None, "sys.seq.pattern");
  Select(app, ContextActionId::SequencerPatternPrev);
  assert(Screen(app).sequencer.activePattern == 0 && Screen(app).sequencer.patternHasEvents[1]);
  Select(app, ContextActionId::SequencerPatternCountDec);
  assert(Screen(app).primaryText == "Remove pattern?" && Screen(app).sequencer.patternCount == 2);
  Select(app, ContextActionId::Back);
  assert(Screen(app).sequencer.patternCount == 2 && Screen(app).sequencer.patternHasEvents[1]);
  Select(app, ContextActionId::SequencerPatternCountDec);
  assert(Screen(app).primaryText == "Remove pattern?");
  app.HandleHomeButtonEvent(true); // HOME cancels the pending destructive overlay too.
  assert(app.Ui().Mode() == UiMode::Home);
  Enter(app, 3, ContextActionId::None, "sys.seq");
  assert(Screen(app).focusItemId == "sys.seq" && Screen(app).primaryText != "Remove pattern?");
  assert(Screen(app).sequencer.patternCount == 2 && Screen(app).sequencer.patternHasEvents[1]);
  Select(app, ContextActionId::None, "sys.seq.pattern");
  Select(app, ContextActionId::SequencerPatternCountDec);
  const auto confirmation = app.Ui().CurrentMenuCells(ActiveEngine(app));
  assert(app.Ui().ContextMenuColumnCount() == 1 && confirmation[2].actionId == ContextActionId::Back);
  assert(confirmation[3].actionId == ContextActionId::SequencerPatternDeleteConfirm);
  Select(app, ContextActionId::SequencerPatternDeleteConfirm);
  assert(Screen(app).sequencer.patternCount == 1 && Screen(app).sequencer.activePattern == 0);
  assert(Screen(app).sequencer.patternHasEvents[0] && Screen(app).sequencer.populatedStepCount == 2);
  Select(app, ContextActionId::SequencerPatternCountInc);
  Select(app, ContextActionId::SequencerPatternCountDec);
  assert(Screen(app).sequencer.patternCount == 1 && Screen(app).primaryText != "Remove pattern?");

  // A blank Piano black-key position still selects a length bucket, but must
  // never insert MIDI note 0 when the same physical key is used in NOTE mode.
  app.HandleHomeButtonEvent(true);
  assert(app.Ui().RestoreKeyboardSettings({0, 0, 0, 0}));
  assert(!app.Ui().IsHomePerformanceKey(0, 1));
  app.HandleMatrixKeyEvent(SequencerStepRow(0), SequencerStepColumn(0), true);
  const auto pianoNotes = Screen(app).sequencer.noteNames;
  assert(Screen(app).sequencer.noteCount == 2);
  Tap(app, 0, 1);
  assert(Screen(app).sequencer.noteCount == 2 && Screen(app).sequencer.noteNames == pianoNotes);
  Tap(app, 1, kHomeLeftBarColumn);
  Tap(app, 0, 4);
  assert(Screen(app).sequencer.editValueIndex == 3);
  Tap(app, 0, 1);
  assert(Screen(app).sequencer.editValueIndex == 0);
  app.HandleMatrixKeyEvent(SequencerStepRow(0), SequencerStepColumn(0), false);
  SaveAs(app, "piano-edited");
  ProjectData piano;
  assert(LoadProjectFile("piano-edited", piano) == ProjectResult::Ok);
  const auto& pianoEvents = piano.sequence->tracks[0].patterns[0].steps[0].notes;
  assert(std::count_if(pianoEvents.begin(), pianoEvents.end(), [](const auto& note) { return note.active; }) == 2);
  for (const auto& note : pianoEvents) {
    if (!note.active) continue;
    assert((note.note == noteA || note.note == noteB) && note.note != 0);
    assert(note.lengthPulses == kSequencerPulsesPerStep);
  }
}
void TestSequencerDestructiveActions() {
  apps::SynthApp app;
  app.Setup({});
  SaveAs(app, "seq-safety-fixture");
  ProjectData expected;
  assert(LoadProjectFile("seq-safety-fixture", expected) == ProjectResult::Ok);
  // Different notes and offsets expose accidental changes to a sibling step,
  // another pattern, or another track through the real app input/save path.
  for (std::size_t track = 0; track < 2; ++track) {
    auto& sequence = expected.sequence->tracks[track];
    sequence.patternCount = track == 0 ? 4 : 3;
    sequence.activePattern = 2;
    for (std::size_t pattern = 0; pattern < 3; ++pattern) {
      for (const std::size_t step : {0u, 5u}) {
        auto& note = sequence.patterns[pattern].steps[step].notes[0];
        note.active = true;
        note.note = static_cast<std::uint8_t>(60 + track * 12 + pattern);
        note.microstep = step == 0 ? 3 : 5;
      }
    }
  }
  assert(SaveProjectFile("seq-safety-fixture", expected, true) == ProjectResult::Ok);
  RequestLoad(app, "seq-safety-fixture", true);
  const auto checkSaved = [&](const std::string& name) {
    SaveAs(app, name);
    ProjectData actual;
    assert(LoadProjectFile(name, actual) == ProjectResult::Ok);
    AssertProjectEqual(expected, actual);
    app.HandleHomeButtonEvent(true);
  };
  const auto holdPatterns = [&] {
    app.HandleHomeButtonEvent(true);
    app.HandleMatrixKeyEvent(0, kHomeRightBarColumn, true);
    assert(Screen(app).sequencer.patternSelectorVisible);
  };

  holdPatterns();
  Tap(app, 0, 2);
  app.HandleMatrixKeyEvent(0, kHomeRightBarColumn, false);
  assert(Screen(app).sequencer.activePattern == 1 && !Screen(app).sequencer.patternSelectorVisible);
  holdPatterns();
  Tap(app, 0, 3);
  app.HandleMatrixKeyEvent(0, kHomeRightBarColumn, false);
  assert(Screen(app).sequencer.activePattern == 2 && Screen(app).sequencer.patternCount == 4);
  holdPatterns();
  Tap(app, 2, kHomeLeftBarColumn); // Merely arming/releasing QNT must not mutate.
  app.HandleMatrixKeyEvent(0, kHomeRightBarColumn, false);
  checkSaved("seq-qnt-no-target");
  holdPatterns();
  app.HandleMatrixKeyEvent(2, kHomeLeftBarColumn, true);
  app.HandleMatrixKeyEvent(0, kHomeRightBarColumn, false);
  Tap(app, SequencerStepRow(5), SequencerStepColumn(5));
  app.HandleMatrixKeyEvent(2, kHomeLeftBarColumn, false);
  expected.sequence->tracks[0].patterns[2].steps[5].notes[0].microstep = 0;
  checkSaved("seq-qnt-step-target");

  holdPatterns();
  app.HandleMatrixKeyEvent(2, kHomeLeftBarColumn, true);
  Tap(app, 0, 1); // Explicitly quantize P01, while P03 remains selected.
  Tap(app, 0, 5); // The + slot is not a quantization target and must not create.
  Tap(app, 1, 8); // Neither is a later unavailable slot.
  app.HandleMatrixKeyEvent(2, kHomeLeftBarColumn, false);
  app.HandleMatrixKeyEvent(0, kHomeRightBarColumn, false);
  assert(Screen(app).sequencer.activePattern == 2 && Screen(app).sequencer.patternCount == 4);
  for (const std::size_t step : {0u, 5u})
    expected.sequence->tracks[0].patterns[0].steps[step].notes[0].microstep = 0;
  checkSaved("seq-qnt-pattern-target");

  holdPatterns();
  app.HandleMatrixKeyEvent(0, kHomeLeftBarColumn, true);
  Tap(app, 0, 4); // Empty trailing P04 can be removed immediately.
  app.HandleMatrixKeyEvent(0, kHomeLeftBarColumn, false);
  app.HandleMatrixKeyEvent(0, kHomeRightBarColumn, false);
  assert(app.Ui().Mode() == UiMode::Home && Screen(app).sequencer.patternCount == 3);
  expected.sequence->tracks[0].patternCount = 3;

  // Releasing OCT (the dialog's right column), CLR and the target after the
  // transition must not operate its menu.
  app.HandleHomeButtonEvent(true);
  app.HandleMatrixKeyEvent(0, kHomeRightBarColumn, true);
  app.HandleMatrixKeyEvent(0, kHomeLeftBarColumn, true);
  app.HandleMatrixKeyEvent(0, 1, true);
  assert(Screen(app).focusItemId == "sys.seq.delete" && Screen(app).headerSecondary == "P01");
  app.HandleMatrixKeyEvent(0, 1, false);
  app.HandleMatrixKeyEvent(0, kHomeLeftBarColumn, false);
  app.HandleMatrixKeyEvent(0, kHomeRightBarColumn, false);
  assert(Screen(app).focusItemId == "sys.seq.delete" && Screen(app).sequencer.patternCount == 3);
  app.HandleHomeButtonEvent(true);
  assert(app.Ui().Mode() == UiMode::Home && Screen(app).sequencer.patternHasEvents[0]);
  checkSaved("seq-home-cancelled");

  const auto requestDeleteFirst = [&] {
    holdPatterns();
    app.HandleMatrixKeyEvent(0, kHomeLeftBarColumn, true);
    // Keep both trigger keys down across the modal transition. Their releases
    // must neither approve deletion nor leak a track switch into the dialog.
    app.HandleMatrixKeyEvent(0, 1, true);
    assert(Screen(app).focusItemId == "sys.seq.delete" && Screen(app).headerSecondary == "P01");
    app.HandleMatrixKeyEvent(0, 1, false);
    app.HandleMatrixKeyEvent(0, kHomeLeftBarColumn, false);
    app.HandleMatrixKeyEvent(0, kHomeRightBarColumn, false);
    assert(Screen(app).focusItemId == "sys.seq.delete" && app.Ui().ActiveTrack() == 0);
    assert(Screen(app).sequencer.patternHasEvents[0]);
  };
  requestDeleteFirst();
  assert(Screen(app).sequencer.activePattern == 2 && Screen(app).sequencer.patternCount == 3);
  Select(app, ContextActionId::Back);
  assert(app.Ui().Mode() == UiMode::Home);
  checkSaved("seq-delete-cancelled");
  requestDeleteFirst();
  Select(app, ContextActionId::SequencerPatternDeleteConfirm);
  assert(app.Ui().Mode() == UiMode::Home && Screen(app).sequencer.patternCount == 2);
  auto& track = expected.sequence->tracks[0];
  track.patterns[0] = track.patterns[1];
  track.patterns[1] = track.patterns[2];
  track.patterns[2] = {};
  track.patternCount = 2;
  track.activePattern = 1; // Preserve P03's musical content at its new index.
  checkSaved("seq-delete-nonlast");

  // The only populated pattern also needs confirmation; approval clears its
  // content but retains one valid slot, without affecting the other tracks.
  track.patterns[1] = {};
  track.patternCount = 1;
  track.activePattern = 0;
  assert(SaveProjectFile("seq-safety-sole", expected, false) == ProjectResult::Ok);
  RequestLoad(app, "seq-safety-sole", true);
  requestDeleteFirst();
  assert(Screen(app).sequencer.patternCount == 1);
  Select(app, ContextActionId::Back);
  assert(app.Ui().Mode() == UiMode::Home && Screen(app).sequencer.patternHasEvents[0]);
  requestDeleteFirst();
  Select(app, ContextActionId::SequencerPatternDeleteConfirm);
  assert(app.Ui().Mode() == UiMode::Home && Screen(app).sequencer.patternCount == 1);
  assert(!Screen(app).sequencer.patternHasEvents[0]);
  track.patterns[0] = {};
  checkSaved("seq-delete-sole");
}
void TestSequencerHomeViews() {
  apps::SynthApp app;
  app.Setup({});
  assert(!app.Ui().HomeShowsSequencerGrid() && !Screen(app).showSequencerGrid);
  const auto keyboard = app.Ui().KeyboardSettings();
  const auto pitch = app.Ui().HomePerformanceMidiNote(3, 1);
  app.HandleMatrixKeyEvent(3, 1, true);
  for (const std::size_t step : {0u, 5u, 10u, 15u})
    Tap(app, SequencerStepRow(step), SequencerStepColumn(step));
  app.HandleMatrixKeyEvent(3, 1, false);
  const auto enterSeq = [&] {
    assert(app.Ui().Mode() == UiMode::Home);
    app.HandleMatrixKeyEvent(3, kHomeLeftBarColumn, true);
    std::this_thread::sleep_for(std::chrono::milliseconds(210));
    app.Tick();
    Select(app, ContextActionId::None, "sys.seq");
    app.HandleMatrixKeyEvent(3, kHomeLeftBarColumn, false);
  };
  const auto toggleMenuView = [&] {
    const bool wasGrid = app.Ui().HomeShowsSequencerGrid();
    enterSeq();
    const auto menu = app.Ui().CurrentMenuCells(ActiveEngine(app));
    assert(menu[6].actionId == ContextActionId::SequencerToggleView && menu[6].label == (wasGrid ? "KEYS" : "GRID"));
    Select(app, ContextActionId::SequencerToggleView);
    assert(app.Ui().Mode() == UiMode::Home && Screen(app).showSequencerGrid == !wasGrid);
  };
  // Enter's existing HOME shortcut must cancel a context, rather than toggle
  // neutral Home, while capturing the baseline for a full saved-data comparison.
  enterSeq();
  SaveAs(app, "seq-view-before");
  ProjectData before;
  assert(LoadProjectFile("seq-view-before", before) == ProjectResult::Ok);
  app.HandleHomeButtonEvent(true);
  assert(!Screen(app).showSequencerGrid);
  Tap(app, 3, kHomeRightBarColumn);
  assert(Screen(app).sequencer.playing);
  toggleMenuView();
  assert(Screen(app).showSequencerGrid && Screen(app).sequencer.playing);
  const auto playing = Screen(app).sequencer;
  assert(playing.steps[playing.currentStep].playing);
  assert(std::count_if(playing.steps.begin(), playing.steps.end(), [](const auto& step) { return step.playing; }) == 1);
  for (const std::size_t step : {0u, 5u, 10u, 15u}) assert(Screen(app).sequencer.steps[step].hasEvents);
  assert(Screen(app).sequencer.populatedStepCount == 4);
  app.HandleHomeButtonEvent(true); // Neutral Home toggles, without stopping music.
  assert(!Screen(app).showSequencerGrid && Screen(app).sequencer.playing);
  app.HandleHomeButtonEvent(true);
  assert(Screen(app).showSequencerGrid && Screen(app).sequencer.playing);
  // Both +/- press orders and both release orders perform exactly one view
  // toggle. Repeats or a repress while the other key remains down are consumed.
  for (const std::size_t firstDown : {0u, 1u}) for (const std::size_t firstUp : {0u, 1u}) {
    const bool wasGrid = Screen(app).showSequencerGrid;
    app.HandleMatrixKeyEvent(firstDown, kHomeRightBarColumn, true);
    assert(Screen(app).sequencer.patternSelectorVisible);
    app.HandleMatrixKeyEvent(1 - firstDown, kHomeRightBarColumn, true);
    const auto checkCombo = [&] {
      const auto current = Screen(app);
      assert(current.showSequencerGrid == !wasGrid && !current.sequencer.patternSelectorVisible && !current.sequencer.shiftActive);
      assert(current.sequencer.playing && app.Ui().ActiveTrack() == 0 && app.Ui().KeyboardSettings() == keyboard);
    };
    checkCombo();
    app.HandleMatrixKeyEvent(firstDown, kHomeRightBarColumn, true);
    app.HandleMatrixKeyEvent(1 - firstDown, kHomeRightBarColumn, true);
    checkCombo();
    app.HandleMatrixKeyEvent(firstUp, kHomeRightBarColumn, false);
    checkCombo();
    app.HandleMatrixKeyEvent(firstUp, kHomeRightBarColumn, true);
    checkCombo();
    app.HandleMatrixKeyEvent(firstUp, kHomeRightBarColumn, false);
    app.HandleMatrixKeyEvent(1 - firstUp, kHomeRightBarColumn, false);
    checkCombo();
  }
  // Chord key-up events remain consumed even after a new context owns input.
  app.HandleMatrixKeyEvent(0, kHomeRightBarColumn, true);
  app.HandleMatrixKeyEvent(1, kHomeRightBarColumn, true);
  assert(!Screen(app).showSequencerGrid);
  app.HandleMatrixKeyEvent(3, kHomeLeftBarColumn, true);
  std::this_thread::sleep_for(std::chrono::milliseconds(210));
  app.Tick();
  assert(app.Ui().Mode() == UiMode::ContextMenu);
  app.HandleMatrixKeyEvent(0, kHomeRightBarColumn, false);
  app.HandleMatrixKeyEvent(1, kHomeRightBarColumn, false);
  assert(app.Ui().Mode() == UiMode::ContextMenu && !Screen(app).showSequencerGrid);
  app.HandleMatrixKeyEvent(3, kHomeLeftBarColumn, false);
  assert(app.Ui().Mode() == UiMode::Home && app.Ui().KeyboardSettings() == keyboard);
  app.HandleMatrixKeyEvent(0, kHomeRightBarColumn, true);
  app.HandleMatrixKeyEvent(1, kHomeRightBarColumn, true);
  assert(Screen(app).showSequencerGrid);
  app.HandleHomeButtonEvent(true); // Cancel an active chord, without a second toggle.
  app.HandleMatrixKeyEvent(1, kHomeRightBarColumn, false);
  app.HandleMatrixKeyEvent(0, kHomeRightBarColumn, false);
  assert(Screen(app).showSequencerGrid && !Screen(app).sequencer.patternSelectorVisible && Screen(app).sequencer.playing);
  Tap(app, 0, kHomeRightBarColumn); // A new isolated tap still changes octave.
  auto raisedKeyboard = keyboard;
  ++raisedKeyboard[0];
  assert(app.Ui().KeyboardSettings() == raisedKeyboard && !Screen(app).sequencer.patternSelectorVisible);
  Tap(app, 1, kHomeRightBarColumn);
  assert(app.Ui().KeyboardSettings() == keyboard && Screen(app).showSequencerGrid);
  enterSeq();
  app.HandleHomeButtonEvent(true); // Context Home cancels, keeping the chosen view.
  assert(app.Ui().Mode() == UiMode::Home && Screen(app).showSequencerGrid);
  Tap(app, 1, kHomeLeftBarColumn);
  assert(app.Ui().ActiveTrack() == 1 && Screen(app).showSequencerGrid && Screen(app).sequencer.populatedStepCount == 0);
  Tap(app, 0, kHomeLeftBarColumn);
  assert(app.Ui().ActiveTrack() == 0 && Screen(app).showSequencerGrid && Screen(app).sequencer.populatedStepCount == 4);
  assert(app.Ui().KeyboardSettings() == keyboard && app.Ui().HomePerformanceMidiNote(3, 1) == pitch);

  for (const std::size_t step : {0u, 5u})
    app.HandleMatrixKeyEvent(SequencerStepRow(step), SequencerStepColumn(step), true);
  Tap(app, 2, kHomeLeftBarColumn);
  Tap(app, 3, 5); // The same physical value columns edit the selected grid steps.
  auto screen = Screen(app);
  assert(screen.showSequencerGrid && screen.sequencer.heldStepCount == 2);
  assert(screen.sequencer.steps[0].held && screen.sequencer.steps[5].held);
  assert(screen.sequencer.editLane == SequencerEditLane::Velocity && screen.sequencer.editValueIndex == 4);
  app.HandleHomeButtonEvent(true); // Held-step editing is cancelled, not switched.
  assert(Screen(app).showSequencerGrid && !Screen(app).sequencer.hasHeldSteps);
  for (const std::size_t step : {0u, 5u})
    app.HandleMatrixKeyEvent(SequencerStepRow(step), SequencerStepColumn(step), false);
  assert(Screen(app).showSequencerGrid && Screen(app).sequencer.playing);

  app.HandleMatrixKeyEvent(0, kHomeRightBarColumn, true);
  assert(Screen(app).sequencer.patternSelectorVisible);
  app.HandleHomeButtonEvent(true);
  app.HandleMatrixKeyEvent(0, kHomeRightBarColumn, false);
  assert(Screen(app).showSequencerGrid && !Screen(app).sequencer.patternSelectorVisible);
  app.HandleMatrixKeyEvent(0, kHomeRightBarColumn, true);
  app.HandleMatrixKeyEvent(0, kHomeLeftBarColumn, true);
  Tap(app, 0, 1);
  app.HandleMatrixKeyEvent(0, kHomeLeftBarColumn, false);
  app.HandleMatrixKeyEvent(0, kHomeRightBarColumn, false);
  assert(Screen(app).focusItemId == "sys.seq.delete");
  app.HandleHomeButtonEvent(true);
  assert(Screen(app).showSequencerGrid && Screen(app).sequencer.populatedStepCount == 4 && Screen(app).sequencer.playing);
  Tap(app, 2, kHomeRightBarColumn);

  enterSeq();
  SaveAs(app, "seq-view-after");
  ProjectData after;
  assert(LoadProjectFile("seq-view-after", after) == ProjectResult::Ok);
  before.sequence->tracks[0].defaultVelocity = 88;
  for (const std::size_t step : {0u, 5u}) before.sequence->tracks[0].patterns[0].steps[step].notes[0].velocity = 88;
  AssertProjectEqual(before, after); // Only the deliberately edited velocities changed.
  app.HandleHomeButtonEvent(true);
  assert(Screen(app).showSequencerGrid);
  toggleMenuView();
  assert(!Screen(app).showSequencerGrid && app.Ui().KeyboardSettings() == keyboard);
}
void TestSequencerTapEditing() {
  apps::SynthApp app;
  app.Setup({});
  app.HandleHomeButtonEvent(true);
  assert(Screen(app).showSequencerGrid);
  const auto step = [&](std::size_t index) { Tap(app, SequencerStepRow(index), SequencerStepColumn(index)); };
  step(0);
  auto state = Screen(app).sequencer;
  assert(state.selectionLatched && state.hasHeldSteps && state.primaryHeldStep == 0 && !state.steps[0].hasEvents);
  Tap(app, 2, kHomeLeftBarColumn);
  Tap(app, 3, 5);
  assert(Screen(app).sequencer.valueLabel == "NEW VEL 88" && Screen(app).sequencer.editValueIndex == 4);
  Tap(app, 3, kHomeLeftBarColumn);
  assert(Screen(app).sequencer.valueLabel == "ADD NOTES FIRST" && Screen(app).sequencer.editValueCount == 0);
  Tap(app, 0, kHomeLeftBarColumn);
  const auto auditionCount = audioNoteOnCounts[0].load();
  Tap(app, 3, 1);
  app.Tick();
  assert(audioNoteOnCounts[0] > auditionCount);
  assert(audioLastVelocities[0] == 88);
  assert(Screen(app).sequencer.steps[0].noteCount == 1);
  Tap(app, 3, 1);
  app.Tick();
  assert(audioNoteOnCounts[0] == auditionCount + 1); // Removing a pitch is silent.
  assert(!Screen(app).sequencer.steps[0].hasEvents); // Tapping a pitch again removes it.
  for (const auto column : {1u, 3u, 5u}) Tap(app, 3, column);
  assert(Screen(app).sequencer.steps[0].noteCount == 3);
  Tap(app, 2, kHomeLeftBarColumn);
  Tap(app, 3, 5);
  state = Screen(app).sequencer;
  assert(state.selectionLatched && state.editLane == SequencerEditLane::Velocity && state.valueLabel == "VEL 88");
  Tap(app, 1, kHomeLeftBarColumn);
  Tap(app, 3, 4);
  assert(Screen(app).sequencer.valueLabel == "LEN 4");
  Tap(app, 0, kHomeLeftBarColumn); // Back to the notes.
  assert(Screen(app).sequencer.editLane == SequencerEditLane::Note);
  Tap(app, 0, kHomeLeftBarColumn);
  assert(Screen(app).sequencer.editLane == SequencerEditLane::Note);
  step(1);
  state = Screen(app).sequencer;
  assert(state.selectionLatched && state.primaryHeldStep == 1 && !state.steps[0].held);
  Tap(app, 3, 2);
  assert(Screen(app).sequencer.steps[0].noteCount == 3 && Screen(app).sequencer.steps[1].noteCount == 1);
  Tap(app, 3, kHomeRightBarColumn);
  assert(Screen(app).sequencer.playing && Screen(app).sequencer.selectionLatched);
  Tap(app, 2, kHomeRightBarColumn);
  assert(!Screen(app).sequencer.playing && Screen(app).sequencer.selectionLatched);
  step(1); // Same step exits; the next pitch is live performance, with no data mutation.
  assert(!Screen(app).sequencer.hasHeldSteps && !Screen(app).sequencer.selectionLatched);
  app.HandleMatrixKeyEvent(SequencerStepRow(2), SequencerStepColumn(2), true);
  std::this_thread::sleep_for(std::chrono::milliseconds(260));
  app.HandleMatrixKeyEvent(SequencerStepRow(2), SequencerStepColumn(2), false);
  assert(!Screen(app).sequencer.selectionLatched && !Screen(app).sequencer.hasHeldSteps);
  Tap(app, 3, 3);
  assert(Screen(app).sequencer.steps[1].noteCount == 1);
  step(0);
  app.HandleHomeButtonEvent(true); // HOME first exits the edit, then switches views.
  assert(Screen(app).showSequencerGrid && !Screen(app).sequencer.hasHeldSteps);
  app.HandleHomeButtonEvent(true);
  assert(!Screen(app).showSequencerGrid);
  step(0);
  assert(!Screen(app).sequencer.hasHeldSteps); // Performance view retains momentary editing.
  app.HandleHomeButtonEvent(true);
  app.HandleMatrixKeyEvent(SequencerStepRow(0), SequencerStepColumn(0), true);
  app.HandleMatrixKeyEvent(SequencerStepRow(1), SequencerStepColumn(1), true);
  Tap(app, 2, kHomeLeftBarColumn);
  Tap(app, 3, 7);
  app.HandleMatrixKeyEvent(SequencerStepRow(0), SequencerStepColumn(0), false);
  app.HandleMatrixKeyEvent(SequencerStepRow(1), SequencerStepColumn(1), false);
  assert(!Screen(app).sequencer.hasHeldSteps && !Screen(app).sequencer.selectionLatched);
  step(0);
  app.HandleMatrixKeyEvent(0, kHomeRightBarColumn, true);
  Tap(app, 0, 2); // New pattern cannot inherit a hidden latched edit.
  app.HandleMatrixKeyEvent(0, kHomeRightBarColumn, false);
  assert(Screen(app).sequencer.activePattern == 1 && !Screen(app).sequencer.hasHeldSteps);
  Tap(app, 1, kHomeLeftBarColumn);
  assert(app.Ui().ActiveTrack() == 1 && !Screen(app).sequencer.selectionLatched);
}

void TestDrumsWorkflow() {
  static_assert(static_cast<int>(SynthEngineKind::Dx7) == 1);
  static_assert(static_cast<int>(SynthEngineKind::Juno) == 2);
  static_assert(static_cast<int>(SynthEngineKind::Sampler) == 3);
  static_assert(static_cast<int>(SynthEngineKind::Drums) == 4);
  const std::array<std::uint8_t, 8> expectedNotes{{36, 38, 42, 46, 39, 43, 56, 70}};
  assert(kDrumNotes == expectedNotes);

  apps::SynthApp app;
  app.Setup({});
  Enter(app, 0, ContextActionId::None, "snd.engine");
  for (int i = 0; Screen(app).secondaryText != "Drums" && i < 4; ++i)
    Select(app, ContextActionId::EngineNext);
  assert(Screen(app).secondaryText == "Drums");
  Select(app, ContextActionId::EngineLoad);
  assert(app.Rack().Slot(0).kind == SynthEngineKind::Drums);
  assert(ActiveEngine(app)->CurrentPresetId() == "drums.808");
  const auto parameters = ActiveEngine(app)->ListParameters({0, 16});
  // Eight kit levels and the master, then the seven track effects every engine carries.
  assert(parameters.totalCount == 16 && parameters.items.size() == 16);
  for (std::size_t index = 0; index < 9; ++index)
  {
    const auto& parameter = parameters.items[index];
    assert(parameter.minValue == 0 && parameter.maxValue == 127 && parameter.defaultValue == 100 && parameter.value == 100);
  }
  assert(parameters.items[9].id == "fx.drive" && parameters.items[15].id == "fx.reverb");

  // All nine levels must be reachable through the actual SND menu. Both pages
  // keep BACK lower-left and the page selector lower-right.
  Enter(app, 1, ContextActionId::None, "kick.level");
  Select(app, ContextActionId::Back);
  for (std::size_t index = 0; index < 9; ++index) {
    const auto& parameter = parameters.items[index];
    auto menu = app.Ui().CurrentMenuCells(ActiveEngine(app));
    if (std::none_of(menu.begin(), menu.end(), [&](const auto& cell) { return cell.itemId == parameter.id; })) {
      assert(menu[7].type == MenuCellType::NextPage);
      Tap(app, 3, kHomeRightBarColumn);
      menu = app.Ui().CurrentMenuCells(ActiveEngine(app));
    }
    assert(app.Ui().ContextMenuColumnCount() == 2);
    assert(menu[6].actionId == ContextActionId::Back && menu[7].type == MenuCellType::NextPage);
    assert(std::any_of(menu.begin(), menu.begin() + 6, [&](const auto& cell) { return cell.itemId == parameter.id; }));
    Select(app, ContextActionId::None, parameter.id);
    assert(Screen(app).focusItemId == parameter.id);
    Select(app, ContextActionId::ParameterIncSmall);
    SynthParameterInfo adjusted;
    assert(ActiveEngine(app)->GetParameterInfo(parameter.id, adjusted) && adjusted.value == 101);
    Select(app, ContextActionId::Back);
  }
  // The seven track effects follow on a third page.
  assert(app.Ui().CurrentMenuCells(ActiveEngine(app))[7].label == "2/3");
  Tap(app, 3, kHomeRightBarColumn);
  assert(app.Ui().CurrentMenuCells(ActiveEngine(app))[7].label == "3/3");
  Tap(app, 3, kHomeRightBarColumn);
  assert(app.Ui().CurrentMenuCells(ActiveEngine(app))[7].label == "1/3");
  assert(app.Ui().CurrentMenuCells(ActiveEngine(app))[6].actionId == ContextActionId::Back);
  Select(app, ContextActionId::Back);
  assert(app.Ui().Mode() == UiMode::Home);

  // All four rows repeat the same eight GM drums, including in Piano mode and
  // with non-default octave/root/scale. Check actual MIDI delivery as well as UI.
  app.HandleHomeButtonEvent(true);
  for (int layout = 0; layout < 3; ++layout) {
    assert(app.Ui().RestoreKeyboardSettings({layout - 2, 11, 7, layout}));
    for (std::size_t row = 0; row < kKeyboardMatrixRows; ++row) {
      for (std::size_t pad = 0; pad < expectedNotes.size(); ++pad) {
        assert(app.Ui().HomePerformanceMidiNote(row, pad + 1) == expectedNotes[pad]);
        const auto before = audioNoteOnCounts[0].load();
        Tap(app, row, pad + 1);
        app.Tick();
        assert(audioNoteOnCounts[0].load() == before + 1 && audioLastNotes[0].load() == expectedNotes[pad]);
      }
    }
  }
  const auto sharedKeyboard = app.Ui().KeyboardSettings();
  Tap(app, 0, kHomeRightBarColumn); // Drums must not change the shared melodic keyboard settings.
  assert(app.Ui().KeyboardSettings() == sharedKeyboard);
  Tap(app, 1, kHomeRightBarColumn);
  assert(app.Ui().KeyboardSettings() == sharedKeyboard);
  for (std::size_t pad = 0; pad < expectedNotes.size(); ++pad)
    assert(app.Ui().HomePerformanceMidiNote(3, pad + 1) == expectedNotes[pad]);
  app.HandleMatrixKeyEvent(0, kHomeRightBarColumn, true);
  std::this_thread::sleep_for(std::chrono::milliseconds(210));
  app.Tick();
  assert(Screen(app).sequencer.patternSelectorVisible); // Long hold retains the sequencer modifier.
  app.HandleMatrixKeyEvent(0, kHomeRightBarColumn, false);
  assert(app.Ui().KeyboardSettings() == sharedKeyboard);
  app.HandleMatrixKeyEvent(2, kHomeLeftBarColumn, true);
  std::this_thread::sleep_for(std::chrono::milliseconds(210));
  app.Tick();
  assert(Screen(app).primaryText == "Fixed Pads");
  const auto fixedPads = app.Ui().CurrentMenuCells(ActiveEngine(app));
  assert(app.Ui().ContextMenuColumnCount() == 1 && fixedPads[3].actionId == ContextActionId::Back);
  for (std::size_t i = 0; i < 3; ++i) assert(fixedPads[i].type == MenuCellType::Empty);
  app.HandleMatrixKeyEvent(2, kHomeLeftBarColumn, false);
  assert(app.Ui().KeyboardSettings() == sharedKeyboard);

  for (std::size_t pad = 0; pad < kDrumParameterIds.size(); ++pad)
    assert(ActiveEngine(app)->SetParameterValue(kDrumParameterIds[pad], 20 + static_cast<int>(pad) * 8));
  assert(ActiveEngine(app)->SetParameterValue("master.level", 73));
  Enter(app, 0, ContextActionId::None, "snd.saveas");
  for (int i = 0; Screen(app).secondaryText != "_" && i < 49; ++i)
    Select(app, ContextActionId::PresetNameBackspace);
  assert(Screen(app).secondaryText == "_");
  TypeText(app, "kit-roundtrip");
  Select(app, ContextActionId::PresetNameSave);
  assert(ActiveEngine(app)->CurrentPresetName() == "kit-roundtrip");
  const auto savedPreset = ActiveEngine(app)->CaptureState();
  assert(savedPreset.basePresetId == "drums.808" && savedPreset.presetId.rfind("user.", 0) == 0);
  assert(std::filesystem::exists(storageRoot / "Preset" / "Drums" / "user.ccpreset"));
  assert(ActiveEngine(app)->LoadPreset("drums.808"));
  Enter(app, 0, ContextActionId::None, "snd.preset");
  TypeText(app, "kit-roundtrip");
  Select(app, ContextActionId::PresetLoad);
  AssertEngineEqual(savedPreset, ActiveEngine(app)->CaptureState());

  app.HandleHomeButtonEvent(true);
  for (std::size_t pad = 0; pad < expectedNotes.size(); ++pad) {
    const std::size_t row = pad % kKeyboardMatrixRows;
    app.HandleMatrixKeyEvent(row, pad + 1, true);
    Tap(app, SequencerStepRow(pad), SequencerStepColumn(pad));
    app.HandleMatrixKeyEvent(row, pad + 1, false);
  }
  app.Tick();
  assert(Screen(app).sequencer.populatedStepCount == 8);
  SaveAs(app, "drums-saved");
  ProjectData saved;
  assert(LoadProjectFile("drums-saved", saved) == ProjectResult::Ok && saved.kinds[0] == SynthEngineKind::Drums);
  AssertEngineEqual(savedPreset, saved.engines[0]);
  for (std::size_t pad = 0; pad < expectedNotes.size(); ++pad) {
    const auto& notes = saved.sequence->tracks[0].patterns[0].steps[pad].notes;
    assert(std::count_if(notes.begin(), notes.end(), [](const auto& note) { return note.active; }) == 1);
    assert(std::any_of(notes.begin(), notes.end(), [&](const auto& note) { return note.active && note.note == expectedNotes[pad]; }));
  }

  // Version 1 gains only kind 4; unknown kinds and sampler references on Drums
  // remain invalid. The legacy 1/2/3 meanings must not shift.
  std::ifstream stored(storageRoot / "Project" / "drums-saved.ccproj");
  std::string serialized((std::istreambuf_iterator<char>(stored)), std::istreambuf_iterator<char>());
  assert(serialized.find("CARDENZA_PROJECT 4\n") == 0 && serialized.find("TRACK 0 4 ") != std::string::npos);
  saved.kinds[0] = static_cast<SynthEngineKind>(5);
  assert(SaveProjectFile("drums-invalid", saved, false) == ProjectResult::Invalid);
  saved.kinds[0] = SynthEngineKind::Drums;
  saved.samples[0] = "wrong.wav";
  assert(SaveProjectFile("drums-invalid", saved, false) == ProjectResult::Invalid);
  saved.samples[0].clear();
  serialized.replace(serialized.find("TRACK 0 4 "), std::string("TRACK 0 4 ").size(), "TRACK 0 99 ");
  { std::ofstream invalid(storageRoot / "Project" / "drums-invalid.ccproj"); invalid << serialized; }
  ProjectData invalid;
  assert(LoadProjectFile("drums-invalid", invalid) == ProjectResult::Invalid);

  RequestNew(app, true);
  assert(app.Rack().Slot(0).kind == SynthEngineKind::Dx7);
  RequestLoad(app, "drums-saved", true);
  assert(app.Rack().Slot(0).kind == SynthEngineKind::Drums && Screen(app).secondaryText == "LOADED");
  AssertEngineEqual(savedPreset, ActiveEngine(app)->CaptureState());
  SaveAs(app, "drums-roundtrip");
  ProjectData roundtrip;
  assert(LoadProjectFile("drums-roundtrip", roundtrip) == ProjectResult::Ok);
  AssertProjectEqual(saved, roundtrip);
}
}

int main()
{
#if defined(_MSC_VER)
  _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
  _set_error_mode(_OUT_TO_STDERR);
  _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
  _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
#endif
  storageRoot = std::filesystem::temp_directory_path() /
      ("cardenza-project-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  assert(synth::storage::EnsureLayout());
  TestSequencerWorkflow();
  TestSequencerDestructiveActions();
  TestSequencerHomeViews();
  TestSequencerTapEditing();
  TestDrumsWorkflow();
  {
    apps::SynthApp app;
    app.Setup({});
    SaveAs(app, "defaults");
    ProjectData defaults;
    assert(LoadProjectFile("defaults", defaults) == ProjectResult::Ok);

    std::array<std::int16_t, 127> sample{};
    for (std::size_t i = 0; i < sample.size(); ++i) sample[i] = static_cast<std::int16_t>(i * 19);
    assert(sampler::WriteMono16Wav((storageRoot / "Sample" / "tone.WAV").string(), sample.data(), sample.size(), 22050));
    app.Ui().SetActiveTrack(2);
    Enter(app, 1, ContextActionId::SamplerSelect);
    Select(app, ContextActionId::PresetLoad);
    assert(audioSamples[2] && audioSamples[2]->frameCount == sample.size());
    assert(app.Rack().Slot(2).engine->SetParameterValue("sample.root", 45));
    assert(app.Rack().Slot(2).engine->SetParameterValue("sample.start", 16));
    assert(app.Rack().Slot(2).engine->SetParameterValue("sample.end", 91));
    assert(app.Ui().RestoreKeyboardSettings({1, 4, 3, 2}));
    assert(app.Rack().Slot(0).engine->SetParameterValue("global.transpose", -5));
    assert(app.Rack().Slot(0).engine->SetParameterValue("op1.output", 37));
    assert(app.Rack().Slot(1).engine->SetParameterValue("vcf.cutoff", 18));
    Device::LED::SetGlobalBrightness(7);
    app.Ui().SetActiveTrack(3);
    Enter(app, 0, ContextActionId::None, "snd.engine");
    Select(app, ContextActionId::EngineNext);
    Select(app, ContextActionId::EngineLoad);
    assert(app.Rack().Slot(3).kind == SynthEngineKind::Juno);
    app.Ui().SetActiveTrack(0);
    AddStep(app, 0);
    Enter(app, 3, ContextActionId::None, "sys.seq");
    Select(app, ContextActionId::None, "sys.seq.tempo");
    Select(app, ContextActionId::SequencerTempoIncLarge);
    Select(app, ContextActionId::SequencerTempoInc);
    Select(app, ContextActionId::Back);
    Select(app, ContextActionId::None, "sys.seq.track");
    Select(app, ContextActionId::SequencerMute);
    Select(app, ContextActionId::Back);
    Select(app, ContextActionId::None, "sys.seq.pattern");
    Select(app, ContextActionId::SequencerPatternCountInc);
    SaveAs(app, "saved");
    ProjectData saved;
    assert(LoadProjectFile("saved", saved) == ProjectResult::Ok);
    assert(saved.sequence->bpm == 131 && saved.sequence->tracks[0].muted && saved.sequence->tracks[0].patternCount == 2);
    assert(saved.sequence->tracks[0].patterns[0].steps[0].notes[0].active);
    assert(saved.samples[2] == "tone.WAV");

    // Change every category through public model/engine APIs or normal UI input.
    assert(app.Ui().RestoreKeyboardSettings({-2, 1, 0, 1}));
    assert(app.Rack().Slot(0).engine->SetParameterValue("op1.output", 99));
    app.Ui().SetActiveTrack(3);
    Enter(app, 0, ContextActionId::None, "snd.engine");
    Select(app, ContextActionId::EnginePrev);
    Select(app, ContextActionId::EngineLoad);
    assert(app.Rack().Slot(3).kind == SynthEngineKind::Dx7);
    app.Ui().SetActiveTrack(0);
    AddStep(app, 5);
    Enter(app, 3, ContextActionId::None, "sys.seq");
    Select(app, ContextActionId::None, "sys.seq.tempo");
    Select(app, ContextActionId::SequencerTempoDecLarge);
    Device::LED::SetGlobalBrightness(2);
    const auto edited = CaptureSession(app);
    RequestLoad(app, "saved", false);
    AssertSessionUnchanged(app, edited);
    RequestLoad(app, "saved", true);
    assert(Screen(app).secondaryText == "LOADED");
    SaveAs(app, "roundtrip");
    ProjectData roundtrip;
    assert(LoadProjectFile("roundtrip", roundtrip) == ProjectResult::Ok);
    AssertProjectEqual(saved, roundtrip);
    assert(audioSamples[2] && std::equal(sample.begin(), sample.end(), audioSamples[2]->frames.get()));

    // Both parse failure and sample preflight failure must preserve the live objects.
    { std::ofstream bad(storageRoot / "Project" / "broken.ccproj"); bad << "not a project\n"; }
    ProjectData missing;
    assert(LoadProjectFile("saved", missing) == ProjectResult::Ok);
    missing.samples[2] = "missing.wav";
    assert(SaveProjectFile("missing", missing, false) == ProjectResult::Ok);
    const auto beforeFailures = CaptureSession(app);
    RequestLoad(app, "broken", true);
    assert(Screen(app).primaryText == "INVALID PROJECT");
    AssertSessionUnchanged(app, beforeFailures);
    RequestLoad(app, "missing", true);
    assert(Screen(app).primaryText == "SAMPLE UNAVAILABLE");
    AssertSessionUnchanged(app, beforeFailures);
    RequestNew(app, false);
    AssertSessionUnchanged(app, beforeFailures);
    RequestNew(app, true);
    assert(Screen(app).secondaryText == "NEW");
    SaveAs(app, "reset");
    ProjectData reset;
    assert(LoadProjectFile("reset", reset) == ProjectResult::Ok);
    AssertProjectEqual(defaults, reset);
    assert(!audioSamples[2]);
    TestBrowserFileActions(app);

    // Recording and an unsaved stopped take each block both replacement commands.
    app.Ui().SetActiveTrack(2);
    microphone.assign(4096, 1234);
    Enter(app, 1, ContextActionId::SamplerRecord);
    Select(app, ContextActionId::SamplerRecordStart);
    app.Tick();
    assert(microphoneRunning && microphoneCursor == microphone.size());
    const auto duringRecording = CaptureSession(app);
    AssertEngineChangeBlocked(app);
    TestRecordingFileBack(app);
    RequestLoad(app, "saved", true);
    assert(Screen(app).primaryText == "SAVE SAMPLE FIRST");
    AssertSessionUnchanged(app, duringRecording);
    assert(microphoneRunning);
    RequestNew(app, true);
    assert(Screen(app).secondaryText == "SAVE SAMPLE FIRST");
    AssertSessionUnchanged(app, duringRecording);
    assert(microphoneRunning);
    Enter(app, 1, ContextActionId::SamplerRecord);
    Select(app, ContextActionId::SamplerStop);
    assert(!microphoneRunning && Screen(app).samplerHasDraft);
    const auto withDraft = CaptureSession(app);
    AssertEngineChangeBlocked(app);
    RequestLoad(app, "saved", true);
    assert(Screen(app).primaryText == "SAVE SAMPLE FIRST");
    AssertSessionUnchanged(app, withDraft);
    RequestNew(app, true);
    assert(Screen(app).secondaryText == "SAVE SAMPLE FIRST");
    AssertSessionUnchanged(app, withDraft);
    // CaptureState does not reveal draft state; reenter the public sampler view to verify it remains.
    Enter(app, 1, ContextActionId::SamplerSelect);
    assert(Screen(app).samplerHasDraft);
    Enter(app, 1, ContextActionId::SamplerRecord);
    assert(Screen(app).primaryText == "Replace take?");
    Select(app, ContextActionId::Back);
    assert(Screen(app).samplerHasDraft && !microphoneRunning);
    AssertSessionUnchanged(app, withDraft);
    Enter(app, 1, ContextActionId::SamplerRecord);
    Select(app, ContextActionId::SamplerRecordStart);
    assert(microphoneRunning && !Screen(app).samplerHasDraft);
    app.Tick();
    Select(app, ContextActionId::SamplerStop);
    assert(!microphoneRunning && Screen(app).samplerHasDraft);
  }
  audioSamples.fill({});
  std::filesystem::remove_all(storageRoot);
  std::cout << "Sequencer input/edit/playback/settings, Drums mapping/presets/projects, project save/load/new, FILE actions, session preservation and recording guards passed\n";
}
