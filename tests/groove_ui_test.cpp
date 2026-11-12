// Groovebox UI workflow tests: the real GrooveApp and runtime on the simulator device, driven key by
// key with a fake clock so hold, repeat and transition timing is deterministic.
#include <algorithm>
#include <bitset>
#include <array>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iterator>
#include <fstream>
#include <iostream>
#include <map>
#include <random>
#include <string>
#include <vector>

#include "Applications/Synth/GrooveApp.h"
#include "Applications/Synth/Runtime/ProjectStorage.h"
#include "Library/Synth/Core/SynthStorage.h"
#include "Library/Synth/Audio/SynthAudioRuntime.h"
#include "Applications/Synth/UI/Groove/Render/Theme.h"
#include "Devices/CardenzaSim/SimulatorInput.h"
#include "Devices/Device.h"
#include "OS/System/System.h"

namespace {

using groove::KeyRole;
using groove::ModalKind;
using groove::Scene;

std::uint32_t gNow = 10000;
std::uint32_t FakeClock() { return gNow; }

struct Key {
  std::size_t row;
  std::size_t column;
};

constexpr Key kEsc{0, 0}, kTab{1, 0}, kFn{2, 0}, kCtrl{3, 0};
constexpr Key kAdd{0, 13}, kSub{1, 13}, kEnter{2, 13}, kPlay{3, 13};
// In text entry the top-right key ("del" on the cap) erases.
constexpr Key kDel{0, 13};

constexpr Key Digit(int digit) { return Key{0, static_cast<std::size_t>(digit == 0 ? 10 : digit)}; }
constexpr Key Grid(std::size_t index) { return Key{index / 4, 9 + index % 4}; }
constexpr Key kDrumPadA{3, 2}, kDrumPadB{3, 3};

class Harness {
public:
  explicit Harness(apps::GrooveApp& app) : app_(app) {}

  void Frames(int count = 1)
  {
    for (int index = 0; index < count; ++index)
    {
      gNow += 16;
      app_.Tick();
    }
  }
  void Wait(std::uint32_t ms) { Frames(static_cast<int>(ms / 16) + 1); }
  void Down(Key key) { app_.HandleMatrixKeyEvent(key.row, key.column, true); Frames(); }
  void Up(Key key) { app_.HandleMatrixKeyEvent(key.row, key.column, false); Frames(); }
  void Tap(Key key) { Down(key); Up(key); }
  void Hold(Key key, std::uint32_t ms) { Down(key); Wait(ms); }
  void Chord(Key modifier, Key key) { Down(modifier); Tap(key); Up(modifier); }
  void Render() { app_.RenderFrame(); }

  const groove::UiState& State() const { return app_.Ui().State(); }
  const groove::Modal* Modal() const { return app_.Ui().TopModal(); }
  synth::SequencerUiState Seq() const { return app_.Sequencer().BuildUiState(State().track); }
  apps::GrooveApp& App() { return app_; }

private:
  apps::GrooveApp& app_;
};

int ParameterValue(apps::GrooveApp& app, std::size_t track, const char* id)
{
  synth::SynthParameterInfo info;
  assert(app.Engine(track) != nullptr && app.Engine(track)->GetParameterInfo(id, info));
  return info.value;
}

// Scenes by their number on screen (0 SETUP, 1 INSTR, 2 SEQ, 3 TAPE) = their ESC map column.
void GoToScene(Harness& h, int number)
{
  h.Chord(kEsc, Grid(static_cast<std::size_t>(number)));
  h.Wait(200);
  assert(static_cast<int>(h.State().scene) == number);
}

// SETUP (scene 0) is reached only with ESC held + GRID column 1; CTRL then walks its pages.
void GoToSetupPage(Harness& h, std::uint8_t page)
{
  GoToScene(h, 0);
  while (h.App().Ui().CurrentPage() != page) h.Tap(kCtrl);
}

// An INSTR page of the current track by its tab.
void GoToInstrumentPage(Harness& h, const char* tab)
{
  GoToScene(h, 1);
  for (std::size_t tries = 0; tries < 16 && std::string(h.App().Ui().CurrentInstrumentPage()->tab) != tab; ++tries) h.Tap(kCtrl);
  assert(std::string(h.App().Ui().CurrentInstrumentPage()->tab) == tab);
}

// Every engine: the preset list opens, every row draws, and a sound loads.
void TestPresetBrowsers(Harness& h)
{
  GoToScene(h, 1);
  for (std::size_t track = 0; track < groove::kTrackCount; ++track)
  {
    h.Chord(kTab, Grid(track));
    while (h.App().Ui().CurrentPage() != 0) h.Tap(kCtrl);
    h.Tap(Grid(1));
    assert(h.Modal() != nullptr && h.Modal()->kind == ModalKind::PresetBrowser);
    const std::size_t count = h.Modal()->Count();
    assert(count > 0);
    h.Render();
    for (std::size_t row = 0; row < count + 2; ++row)
    {
      h.Tap(kAdd);
      h.Render();
    }
    h.Tap(kEnter);
    h.Wait(50);
    h.Render();
    if (h.Modal() != nullptr) h.Tap(kEsc);
    h.Wait(400);
    assert(h.Modal() == nullptr);
  }
  h.Chord(kTab, Grid(synth::kDefaultActiveTrack));
}

void TestBoot(Harness& h)
{
  // It boots on the instrument, first page (VOICE).
  assert(h.State().scene == Scene::Instrument);
  assert(h.App().Ui().CurrentPage() == 0 && h.App().Ui().CurrentInstrumentPage()->graphic == groove::PageGraphic::Voice);
  // It starts on T1, the drum machine.
  assert(h.State().track == synth::kDefaultActiveTrack && h.State().track == 0 && h.App().EngineKind(0) == synth::SynthEngineKind::Drums);
  // A melodic track: the pads show the piano layout, two whole octaves of a pair of rows each: white
  // keys on the second and the bottom row (C at the left), black keys above each, with the gaps
  // (columns 1, 4, 8) dark; unpressed keys are half lit.
  h.Chord(kTab, Grid(1));
  assert(h.State().track == 1);
  std::array<std::uint8_t, groove::kKeyCount> leds{};
  h.App().Ui().ComputeLeds(leds);
  for (std::size_t column = 1; column <= 8; ++column)
  {
    const bool gap = column == 1 || column == 4 || column == 8;
    const std::uint8_t blackRow = gap ? groove::kLedOff : groove::kLedHalf;
    assert(leds[groove::KeyIndex(1, column)] == groove::kLedHalf && leds[groove::KeyIndex(3, column)] == groove::kLedHalf);
    assert(leds[groove::KeyIndex(0, column)] == blackRow && leds[groove::KeyIndex(2, column)] == blackRow);
  }
  // Every pad plays where it should: C3 at the bottom left, its black keys on the row above, the next
  // octave on the upper pair of rows, C4 at the second row's left (and at the bottom row's right).
  groove::KeyboardSettings keyboard;
  int note = 0;
  const auto kind = synth::SynthEngineKind::Juno;
  assert(groove::PadNote(keyboard, kind, 3, 1, note) && note == 48);
  assert(groove::PadNote(keyboard, kind, 3, 8, note) && note == 60);
  assert(groove::PadNote(keyboard, kind, 2, 2, note) && note == 49);
  assert(groove::PadNote(keyboard, kind, 2, 7, note) && note == 58);
  assert(groove::PadNote(keyboard, kind, 1, 1, note) && note == 60);
  assert(groove::PadNote(keyboard, kind, 1, 8, note) && note == 72);
  assert(groove::PadNote(keyboard, kind, 0, 2, note) && note == 61);
  assert(groove::PadNote(keyboard, kind, 0, 7, note) && note == 70);
  assert(!groove::PadNote(keyboard, kind, 2, 1, note) && !groove::PadNote(keyboard, kind, 0, 4, note));
  for (std::size_t track = 0; track < synth::kSynthTrackCount; ++track)
    assert(h.App().EngineKind(track) == apps::GrooveApp::kDefaultKinds[track]);
  assert(h.App().EngineKind(0) == synth::SynthEngineKind::Drums);
  // A melodic track's note page starts as a piano roll, a drum track's as the drum machine.
  assert(h.App().Ui().View() == groove::SeqView::Roll && h.App().Ui().PageTab(Scene::Sequence, 0) == "ROLL");
  h.Chord(kTab, Grid(0));
  assert(h.App().Ui().PageTab(Scene::Sequence, 0) == "DRUMS");
  // The walkthrough below edits T1's drum notes with the piano roll (TestDrumMachine: the other way).
  h.App().Ui().UsePianoRollOnDrums(true);
  assert(h.App().Ui().PageTab(Scene::Sequence, 0) == "ROLL");
  // The walkthrough continues in SEQ.
  GoToScene(h, 2);
}

void TestNavigation(Harness& h)
{
  // Holding ESC shows the scene map: GRID columns are the scenes (any row), the pads stay silent,
  // and the release does not also go back.
  h.Hold(kEsc, 300);
  assert(h.App().Ui().EscOverlay());
  h.Tap(Key{1, 3});
  assert(h.State().padNote[1 * 8 + 2] == 0xFF);
  h.Tap(Grid(0));  // the first column is SETUP (the only way in)
  assert(h.State().scene == Scene::Setup);
  h.Tap(Grid(9));  // row 2, column 1: INSTR
  h.Up(kEsc);
  assert(h.State().scene == Scene::Instrument);
  assert(!h.App().Ui().EscOverlay());
  // Tapping ESC walks the music scenes in a ring: INSTR, SEQ, TAPE, INSTR (never SETUP).
  h.Tap(kEsc);
  assert(h.State().scene == Scene::Sequence);
  h.Tap(kEsc);
  assert(h.State().scene == Scene::Tape);
  h.Tap(kEsc);
  assert(h.State().scene == Scene::Instrument);
  GoToScene(h, 2);

  // TAB: tap = next track; held, the GRID is laid out like MIX (columns are tracks: select, mute,
  // solo, sequencer); TAB + SUB / ADD = previous / next.
  h.Tap(kTab);
  assert(h.State().track == 1);
  h.Hold(kTab, 300);
  assert(h.App().Ui().TabOverlay());
  h.Tap(Grid(3));
  assert(h.State().track == 3);
  h.Tap(Grid(4 + 2));
  assert(h.App().TrackMuted(2));
  h.Tap(Grid(4 + 2));
  assert(!h.App().TrackMuted(2));
  h.Up(kTab);
  assert(h.State().track == 3);
  h.Chord(kTab, kSub);
  assert(h.State().track == 2);
  h.Chord(kTab, kAdd);
  h.Chord(kTab, kAdd);
  assert(h.State().track == 0);

  // CTRL: a tap steps to the next page; a hold opens the page map without moving, the GRID jumps
  // to any page and the page stays after the release; CTRL + ADD / SUB step.
  h.Tap(kCtrl);
  assert(h.App().Ui().CurrentPage() == 1);
  h.Hold(kCtrl, 400);
  assert(h.App().Ui().CtrlOverlay());
  assert(h.App().Ui().CurrentPage() == 1);
  h.Tap(Grid(2));
  assert(h.App().Ui().CurrentPage() == 2);
  h.Tap(Grid(9));  // SEQ has three pages: nothing there
  assert(h.App().Ui().CurrentPage() == 2);
  h.Tap(kAdd);
  assert(h.App().Ui().CurrentPage() == 0);
  h.Tap(kSub);
  assert(h.App().Ui().CurrentPage() == 2);
  h.Up(kCtrl);
  assert(h.App().Ui().CurrentPage() == 2 && !h.App().Ui().CtrlOverlay());
  h.Tap(kCtrl);
  assert(h.App().Ui().CurrentPage() == 0);

  // Held FN shows the menu of this screen after the hold threshold: the SEQ items on rows 0-2, the
  // shared row (SAVE, TAP, REC, SETUP) at the bottom. Nothing touches the screen underneath until an
  // item runs.
  const auto steps = h.Seq().populatedStepCount;
  h.Down(kFn);
  assert(!h.App().Ui().FnOverlay());
  h.Wait(300);
  assert(h.App().Ui().FnOverlay() && h.App().Ui().Menu()[0].action == groove::MenuAction::Copy);
  assert(h.App().Ui().Menu()[15].action == groove::MenuAction::Setup);
  h.Render();
  // FN + GRID 16 (column 4) runs SETUP; a tap on ESC goes back to where it came from, and taps
  // never cycle into SETUP.
  h.Tap(Grid(15));
  h.Up(kFn);
  assert(h.State().scene == Scene::Setup && !h.App().Ui().FnOverlay());
  h.Render();
  h.Tap(kEsc);
  assert(h.State().scene == Scene::Sequence);
  for (int tap = 0; tap < 3; ++tap)
  {
    h.Tap(kEsc);
    assert(h.State().scene != Scene::Setup);
  }
  assert(h.State().scene == Scene::Sequence);
  // A tap on FN keeps the menu open; the GRID runs an item and closes it. FN + GRID runs it too.
  h.Tap(kFn);
  assert(h.App().Ui().MenuOpen());
  h.Render();
  h.Tap(Grid(5));  // VIEW: ROLL -> STEP
  assert(!h.App().Ui().MenuOpen() && h.App().Ui().View() == groove::SeqView::Step);
  h.Chord(kFn, Grid(5));
  assert(h.App().Ui().View() == groove::SeqView::Roll);
  // FN + CTRL steps back a page.
  h.Chord(kFn, kCtrl);
  assert(h.App().Ui().CurrentPage() == 2);
  h.Tap(kCtrl);
  assert(h.Seq().populatedStepCount == steps && h.App().Ui().CurrentPage() == 0);
  assert(h.State().track == 0);
}

void TestStepEditing(Harness& h)
{
  auto& sequencer = h.App().Sequencer();
  assert(h.State().scene == Scene::Sequence && h.App().Ui().CurrentPage() == 0 && h.State().track == 0);
  // Hold a pad, tap steps: the snare goes onto steps 1 and 3 without selecting them.
  h.Down(Key{2, 2});
  h.Tap(Grid(0));
  h.Tap(Grid(2));
  h.Up(Key{2, 2});
  auto seq = h.Seq();
  assert(seq.steps[0].hasEvents && seq.steps[0].lowestNote == 38);
  assert(seq.steps[2].hasEvents && !seq.hasHeldSteps);
  assert(h.State().dirty);

  // Tap selects; pads then toggle notes on the selection.
  h.Tap(Grid(0));
  seq = h.Seq();
  assert(seq.hasHeldSteps && seq.steps[0].held);
  h.Tap(Key{2, 3});
  assert(h.Seq().steps[0].noteCount == 2);
  h.Tap(Key{2, 3});
  assert(h.Seq().steps[0].noteCount == 1);

  // ENTER cycles the lane, ADD edits it.
  assert(h.Seq().editLane == synth::SequencerEditLane::Note);
  h.Tap(kEnter);
  assert(h.Seq().editLane == synth::SequencerEditLane::Length);
  h.Tap(kEnter);
  assert(h.Seq().editLane == synth::SequencerEditLane::Velocity);
  const std::string before = h.Seq().valueLabel;
  h.Tap(kSub);
  assert(h.Seq().valueLabel != before);

  // ESC clears the selection before it leaves the scene.
  h.Tap(kEsc);
  assert(!h.Seq().hasHeldSteps && h.State().scene == Scene::Sequence);

  // Fn + C / V copies step 1 onto a selected step.
  h.Tap(Grid(0));
  h.Chord(kFn, Key{3, 5});  // C
  h.Tap(kEsc);
  h.Tap(Grid(5));
  h.Chord(kFn, Key{3, 6});  // V
  assert(h.Seq().steps[5].hasEvents && h.Seq().steps[5].lowestNote == 38);
  // Fn + X clears the selected steps.
  h.Chord(kFn, Key{3, 4});
  assert(!h.Seq().steps[5].hasEvents);
  h.Tap(kEsc);
  assert(sequencer.ActivePattern(0) == 0);
}

void TestPatterns(Harness& h)
{
  auto& sequencer = h.App().Sequencer();
  h.Tap(kCtrl);
  assert(h.App().Ui().CurrentPage() == 1);
  // The slot after the last pattern creates one; while stopped the switch is immediate.
  h.Tap(Grid(1));
  assert(sequencer.PatternCount(0) == 2 && sequencer.ActivePattern(0) == 1);
  h.Tap(Grid(0));
  assert(sequencer.ActivePattern(0) == 0);
  // Fn + D duplicates the active pattern into the next free slot.
  h.Chord(kFn, Key{2, 4});
  assert(sequencer.PatternCount(0) == 3 && sequencer.ActivePattern(0) == 2);
  assert(h.Seq().patternHasEvents[2]);
  h.Tap(Grid(0));
  // TRACK page: step count and the sequence switch.
  h.Tap(kCtrl);
  assert(h.App().Ui().CurrentPage() == 2);
  h.Tap(Grid(1));
  h.Tap(kSub);
  assert(sequencer.TrackStepCount(0) == 15);
  h.Tap(kAdd);
  assert(sequencer.TrackStepCount(0) == 16);
  h.Tap(Grid(0));
  assert(!sequencer.TrackEnabled(0));
  h.Tap(Grid(0));
  assert(sequencer.TrackEnabled(0));
  h.Tap(kCtrl);
  assert(h.App().Ui().CurrentPage() == 0);
}

void TestTransport(Harness& h)
{
  auto& sequencer = h.App().Sequencer();
  h.Tap(kPlay);
  assert(sequencer.Playing());
  h.Chord(kFn, kPlay);
  assert(sequencer.RecordArmed());
  h.Tap(kPlay);
  assert(!sequencer.Playing());
  h.Chord(kFn, kPlay);
  assert(!sequencer.RecordArmed());
  // Tap tempo: the interval between two Fn + T taps sets the BPM.
  h.Down(kFn);
  const std::uint32_t first = gNow;
  h.Tap(Key{1, 5});
  h.Wait(480);
  const std::uint32_t second = gNow;
  h.Tap(Key{1, 5});
  h.Up(kFn);
  const std::uint32_t interval = second - first;
  assert(h.Seq().bpm == (60000 + interval / 2) / interval);
  // The HOME side key is unassigned; FN + ESC is the panic stop.
  h.Tap(kPlay);
  assert(sequencer.Playing());
  const std::uint32_t panic = h.State().panicAt;
  h.App().HandleHomeButtonEvent(true);
  h.App().HandleHomeButtonEvent(false);
  assert(sequencer.Playing() && h.State().panicAt == panic);
  h.Chord(kFn, kEsc);
  assert(!sequencer.Playing() && h.State().panicAt != panic);
}

void TestInstrument(Harness& h)
{
  auto& app = h.App();
  GoToScene(h, 1);
  h.Chord(kTab, Grid(1));  // Juno
  assert(app.EngineKind(1) == synth::SynthEngineKind::Juno);
  assert(h.App().Ui().CurrentPage() == 0);

  // VOICE: SUB / ADD step through presets immediately.
  const std::string first = app.Engine(1)->CurrentPresetId();
  h.Tap(kAdd);
  assert(app.Engine(1)->CurrentPresetId() != first);
  h.Tap(kSub);
  assert(app.Engine(1)->CurrentPresetId() == first);

  // VOICE, then the track's MIDI effects CHORD and ARP, then the engine: DCO.
  h.Tap(kCtrl);
  assert(std::string(h.App().Ui().CurrentInstrumentPage()->tab) == "CHORD");
  h.Tap(kCtrl);
  assert(std::string(h.App().Ui().CurrentInstrumentPage()->tab) == "ARP");
  h.Tap(kCtrl);
  // DCO page: GRID focuses PWM, ADD raises it, Fn + ENTER restores the default.
  assert(std::string(h.App().Ui().CurrentInstrumentPage()->tab) == "DCO");
  h.Tap(Grid(3));
  const int pwm = ParameterValue(app, 1, "dco.pwm");
  h.Tap(kAdd);
  assert(ParameterValue(app, 1, "dco.pwm") == std::min(127, pwm + 1));
  // FN + ADD / SUB take big steps (PWM by ten); FN + ENTER, or RESET in the menu, restores the
  // default.
  h.Chord(kFn, kAdd);
  assert(ParameterValue(app, 1, "dco.pwm") == std::min(127, pwm + 11));
  h.Chord(kFn, kSub);
  assert(ParameterValue(app, 1, "dco.pwm") == std::min(127, pwm + 1));
  h.Chord(kFn, kEnter);
  assert(ParameterValue(app, 1, "dco.pwm") == 48);
  h.Tap(kAdd);
  assert(h.App().Ui().Menu()[4].action == groove::MenuAction::Default && h.App().Ui().Menu()[4].detail == "PWM");
  h.Chord(kFn, Grid(4));
  assert(ParameterValue(app, 1, "dco.pwm") == 48);
  // A second press on a focused toggle flips it.
  const int saw = ParameterValue(app, 1, "dco.saw");
  h.Tap(Grid(1));
  h.Tap(Grid(1));
  assert(ParameterValue(app, 1, "dco.saw") != saw);

  // Auto-repeat on a held ADD.
  h.Tap(Grid(5));
  const int sub = ParameterValue(app, 1, "dco.sub");
  h.Hold(kAdd, 800);
  h.Up(kAdd);
  assert(ParameterValue(app, 1, "dco.sub") >= std::min(127, sub + 4));

  // Each track keeps its own INSTR page.
  h.Chord(kTab, Grid(2));
  assert(h.App().Ui().CurrentPage() == 0);
  h.Chord(kTab, Grid(1));
  assert(std::string(h.App().Ui().CurrentInstrumentPage()->tab) == "DCO");

  // FX page through the CTRL page map: drive, crush, filter, chorus and the two sends are sound
  // parameters of the track's engine.
  h.Down(kCtrl);
  h.Tap(Grid(7));
  h.Up(kCtrl);
  assert(std::string(h.App().Ui().CurrentInstrumentPage()->tab) == "FX");
  h.Tap(Grid(5));
  for (int press = 0; press < 11; ++press) h.Tap(kAdd);
  assert(ParameterValue(app, 1, "fx.delay") == 11);
  h.Tap(Grid(4));
  const int chorus = ParameterValue(app, 1, "fx.chorus");
  h.Tap(kEnter);
  assert(ParameterValue(app, 1, "fx.chorus") == (chorus + 1) % 4);
  h.Tap(Grid(2));
  h.Tap(kSub);
  assert(ParameterValue(app, 1, "fx.filter") == 63);
  assert(app.Engine(1)->DescribeParameterValue("fx.filter").rfind("LP", 0) == 0);
  h.Chord(kFn, kEnter);
  assert(ParameterValue(app, 1, "fx.filter") == 64 && app.Engine(1)->DescribeParameterValue("fx.filter") == "OFF");
  h.Render();

  // SETUP KEYS: the keyboard settings for every track. ESC goes back to INSTR.
  GoToSetupPage(h, groove::kSetupKeys);
  h.Render();
  h.Tap(Grid(3));  // OCTAVE
  h.Tap(kAdd);
  assert(h.State().keyboard.octave == 1);
  h.Chord(kFn, kEnter);  // FN + ENTER: the default
  assert(h.State().keyboard.octave == 0);
  h.Tap(kEsc);
  assert(h.State().scene == Scene::Instrument);

  // ENGINE picker: switch track 2 from Juno to DX7. Its sound has the edits made above, so the switch
  // asks first: ESC goes back to the picker with the Juno kept, ENTER switches.
  while (h.App().Ui().CurrentPage() != 0) h.Tap(kCtrl);
  h.Tap(Grid(0));
  assert(h.Modal() != nullptr && h.Modal()->kind == ModalKind::EnginePicker);
  h.Render();
  assert(app.Engine(1)->HasEdits());
  h.Tap(Grid(0));
  assert(h.Modal() != nullptr && h.Modal()->kind == ModalKind::Confirm && h.Modal()->confirmPurpose == groove::ConfirmPurpose::ChangeEngine);
  assert(h.Modal()->title == "SWITCH TO DX7?" && app.EngineKind(1) == synth::SynthEngineKind::Juno);
  h.Render();
  h.Tap(kEsc);
  assert(h.Modal() != nullptr && h.Modal()->kind == ModalKind::EnginePicker && app.EngineKind(1) == synth::SynthEngineKind::Juno);
  h.Tap(Grid(0));
  h.Tap(kEnter);
  assert(app.EngineKind(1) == synth::SynthEngineKind::Dx7);
  assert(h.Modal() == nullptr);
  // The preset browser: the pads do not type; FN + F opens the search field, ENTER filters. ADD
  // (the upper key) moves up, SUB down; ENTER loads.
  h.Tap(Grid(1));
  assert(h.Modal() != nullptr && h.Modal()->kind == ModalKind::PresetBrowser);
  const std::size_t all = h.Modal()->Count();
  h.Tap(Key{2, 2});  // A: nothing
  assert(h.Modal()->kind == ModalKind::PresetBrowser && h.Modal()->search.empty() && h.Modal()->Count() == all);
  const std::size_t start = h.Modal()->cursor;
  h.Tap(kSub);
  assert(h.Modal()->cursor == start + 1);
  h.Tap(kAdd);
  assert(h.Modal()->cursor == start);
  h.Chord(kFn, Key{2, 5});  // F
  assert(h.Modal()->kind == ModalKind::TextEntry);
  h.Tap(Key{2, 2});  // A
  h.Tap(kEnter);
  assert(h.Modal()->kind == ModalKind::PresetBrowser && h.Modal()->search == "A" && h.Modal()->Count() < all);
  h.Render();
  h.Tap(kEsc);
  assert(h.Modal() != nullptr && h.Modal()->search.empty());
  h.Tap(kSub);
  h.Tap(kEnter);
  assert(h.Modal() == nullptr);
}

// Loading a preset (ADD / SUB on VOICE, the browser) asks first when the track's sound has edits;
// without edits it loads at once.
void TestPresetEdits(Harness& h)
{
  auto& app = h.App();
  GoToScene(h, 1);
  h.Chord(kTab, Grid(2));
  while (h.App().Ui().CurrentPage() != 0) h.Tap(kCtrl);
  synth::ISynthEngine* engine = app.Engine(2);
  assert(engine != nullptr && !engine->HasEdits());
  const auto isConfirm = [&] {
    return h.Modal() != nullptr && h.Modal()->kind == ModalKind::Confirm && h.Modal()->confirmPurpose == groove::ConfirmPurpose::ChangePreset;
  };
  const auto edit = [&] {
    const int delay = ParameterValue(app, 2, "fx.delay");
    assert(engine->SetParameterValue("fx.delay", delay < 100 ? delay + 9 : delay - 9) && engine->HasEdits());
  };
  const std::string first = engine->CurrentPresetId();
  h.Tap(kAdd);
  const std::string second = engine->CurrentPresetId();
  assert(second != first && h.Modal() == nullptr);

  // ADD with an edit asks; ESC keeps the sound and the edit.
  edit();
  h.Tap(kAdd);
  assert(isConfirm() && h.Modal()->danger && h.Modal()->detail.find("WILL BE LOST") != std::string::npos);
  assert(h.Modal()->title.rfind("LOAD ", 0) == 0);
  h.Render();
  h.Tap(kEsc);
  assert(h.Modal() == nullptr && engine->CurrentPresetId() == second && engine->HasEdits());
  // ENTER loads: the edit is gone, so the next SUB does not ask.
  h.Tap(kAdd);
  h.Tap(kEnter);
  assert(h.Modal() == nullptr && engine->CurrentPresetId() != second && !engine->HasEdits());
  h.Tap(kSub);
  assert(h.Modal() == nullptr && engine->CurrentPresetId() == second);

  // The browser: the confirm sits over it, ESC goes back to the list, ENTER loads and closes both.
  edit();
  h.Tap(Grid(1));
  assert(h.Modal() != nullptr && h.Modal()->kind == ModalKind::PresetBrowser);
  h.Tap(kAdd);  // up the list: the current preset is its last (ADD on VOICE went back from the first)
  h.Tap(kEnter);
  assert(isConfirm());
  h.Tap(kEsc);
  assert(h.Modal() != nullptr && h.Modal()->kind == ModalKind::PresetBrowser && engine->HasEdits());
  h.Tap(kEnter);
  assert(isConfirm());
  h.Tap(kEnter);
  assert(h.Modal() == nullptr && !engine->HasEdits() && engine->CurrentPresetId() != second);
}

void TestTape(Harness& h)
{
  auto& sequencer = h.App().Sequencer();
  GoToScene(h, 3);
  while (h.App().Ui().CurrentPage() != groove::kTapeSong) h.Tap(kCtrl);
  assert(sequencer.SongLength() == 0);
  // The first empty column appends rows; each new row copies the one before.
  h.Tap(Grid(0));
  h.Tap(Grid(1));
  h.Tap(Grid(2));
  assert(sequencer.SongLength() == 3);
  assert(h.State().songRow == 2 && h.State().songTrack == 0);
  // GRID row = track, column = song row. ADD steps the cell through silent and the track's patterns.
  h.Tap(Grid(5));
  assert(h.State().songRow == 1 && h.State().songTrack == 1);
  const int before = sequencer.SongRowAt(1).patterns[1];
  h.Tap(kAdd);
  const int after = sequencer.SongRowAt(1).patterns[1];
  assert(after != before && after >= -1);
  h.Tap(kSub);
  assert(sequencer.SongRowAt(1).patterns[1] == before);
  // A second tap on the cell switches to the row length (BARS); ADD lengthens the row.
  h.Tap(Grid(5));
  assert(h.State().songLane == 1);
  h.Tap(kAdd);
  assert(sequencer.SongRowAt(1).repeats == 2);
  h.Tap(Grid(5));
  assert(h.State().songLane == 0);
  // ENTER cues the row and switches to song mode; Fn + ENTER toggles back to loop mode.
  h.Tap(kEnter);
  assert(sequencer.SongMode());
  h.Chord(kFn, kEnter);
  assert(!sequencer.SongMode());
  // Row clipboard: copy, paste after the cursor, delete, duplicate.
  h.Chord(kFn, Key{3, 5});  // C
  h.Chord(kFn, Key{3, 6});  // V
  assert(sequencer.SongLength() == 4 && sequencer.SongRowAt(2).repeats == 2);
  h.Chord(kFn, Key{3, 4});  // X
  assert(sequencer.SongLength() == 3);
  h.Chord(kFn, Key{2, 4});  // D
  assert(sequencer.SongLength() == 4);
  // Lights: the cursor cell shimmers at focus level, empty columns stay dark.
  std::array<std::uint8_t, groove::kKeyCount> levels{};
  h.App().Ui().ComputeLeds(levels);
  const std::size_t cursor = h.State().songTrack * 4 + (h.State().songRow - h.State().songScroll);
  assert(levels[groove::KeyIndex(cursor / 4, 9 + cursor % 4)] >= 12);
  h.Render();
}

void TestMix(Harness& h)
{
  auto& app = h.App();
  GoToScene(h, 3);
  while (h.App().Ui().CurrentPage() != groove::kTapeMix) h.Tap(kCtrl);
  h.Tap(Grid(4));   // mute T1
  h.Tap(Grid(9));   // solo T2
  assert(app.TrackMuted(0) && app.TrackSoloed(1));
  assert(!app.TrackAudible(0) && app.TrackAudible(1) && !app.TrackAudible(2));
  h.Tap(Grid(1));   // focus T2
  assert(h.State().track == 1);
  h.Tap(kSub);
  assert(app.TrackLevel(1) == 99);
  for (int press = 0; press < 10; ++press) h.Tap(kSub);
  assert(app.TrackLevel(1) == 89);
  h.Tap(kEnter);    // master focus
  const std::uint8_t master = app.MasterVolume();
  h.Tap(kSub);
  assert(app.MasterVolume() == master - 1);
  h.Tap(kEnter);
  h.Tap(Grid(9));
  assert(!app.TrackSoloed(1) && app.TrackAudible(2));
  // FN + a number does nothing (the menu is on the GRID); it never mutes.
  h.Chord(kFn, Digit(1));
  assert(app.TrackMuted(0) && h.Modal() == nullptr);
  // The MIX menu: UNMUTE ALL, then T1 muted again on the GRID.
  h.Chord(kFn, Grid(0));
  assert(!app.TrackMuted(0));
  h.Tap(Grid(4));
  assert(app.TrackMuted(0));
}

void TestSendsAndFx(Harness& h)
{
  auto& app = h.App();
  GoToScene(h, 3);
  // SENDS: columns are tracks, rows filter / chorus / delay / reverb of each track's engine.
  h.Down(kCtrl);
  h.Tap(Grid(groove::kTapeSends));
  h.Up(kCtrl);
  assert(h.App().Ui().CurrentPage() == groove::kTapeSends);
  h.Tap(Grid(8 + 2));  // T3 delay send, also selects T3
  assert(h.State().track == 2);
  for (int press = 0; press < 11; ++press) h.Tap(kAdd);
  assert(ParameterValue(app, 2, "fx.delay") == 11);
  h.Tap(kEnter);       // switches the send off ...
  assert(ParameterValue(app, 2, "fx.delay") == 0);
  h.Tap(kEnter);       // ... and back to where it was
  assert(ParameterValue(app, 2, "fx.delay") == 11);
  h.Tap(Grid(0));      // T1 filter: bipolar, low-pass below the centre
  h.Tap(kSub);
  assert(ParameterValue(app, 0, "fx.filter") == 63);
  h.Chord(kFn, kEnter);
  assert(ParameterValue(app, 0, "fx.filter") == 64);
  h.Tap(Grid(4 + 1));  // T2 chorus: a second press steps the mode
  const int chorus = ParameterValue(app, 1, "fx.chorus");
  h.Tap(Grid(4 + 1));
  assert(ParameterValue(app, 1, "fx.chorus") == (chorus + 1) % 4);
  h.Render();

  // FX: the master bus belongs to the project.
  h.Tap(kCtrl);
  assert(h.App().Ui().CurrentPage() == groove::kTapeFx);
  using synth::fx::MasterParam;
  const auto param = [](MasterParam value) { return static_cast<std::size_t>(value); };
  const int time = app.MasterFx(param(MasterParam::DelayTime));
  h.Tap(Grid(param(MasterParam::DelayTime)));
  h.Tap(kEnter);
  assert(app.MasterFx(param(MasterParam::DelayTime)) == (time + 1) % 12);
  h.Tap(Grid(param(MasterParam::EqLow)));
  for (int press = 0; press < 10; ++press) h.Tap(kAdd);
  assert(app.MasterFx(param(MasterParam::EqLow)) == 74);
  h.Chord(kFn, kEnter);
  assert(app.MasterFx(param(MasterParam::EqLow)) == 64);
  h.Tap(Grid(param(MasterParam::ReverbSize)));
  h.Tap(kSub);
  assert(app.MasterFx(param(MasterParam::ReverbSize)) == synth::fx::kMasterParams[param(MasterParam::ReverbSize)].defaultValue - 1);
  h.Tap(Grid(13));     // the last row is the bus display, not a parameter
  assert(h.State().fxFocus == param(MasterParam::ReverbSize));
  h.Render();
}

void TypeText(Harness& h, const std::string& text)
{
  for (const char c : text)
  {
    for (std::size_t row = 0; row < 4; ++row)
      for (std::size_t column = 1; column < 14; ++column)
        if (groove::LegendChar(row, column) == c) { h.Tap(Key{row, column}); goto typed; }
    assert(false && "character has no key");
  typed:;
  }
}

void TestProject(Harness& h)
{
  auto& app = h.App();
  GoToSetupPage(h, groove::kSetupProject);
  // SAVE AS (in the FN menu, like everything that types): the name field starts with a suggestion;
  // DEL erases, letters type.
  h.Chord(kFn, Grid(2));
  assert(h.Modal() != nullptr && h.Modal()->kind == ModalKind::TextEntry);
  h.Tap(kSub);  // the second key does not erase while typing
  assert(h.Modal()->text == "SONG");
  for (int index = 0; index < 8; ++index) h.Tap(kDel);
  assert(h.Modal()->text.empty());
  TypeText(h, "GROOVE 1");
  assert(h.Modal()->text == "GROOVE 1");
  h.Render();
  h.Tap(kEnter);
  assert(h.Modal() == nullptr);
  assert(h.State().projectName == "GROOVE 1" && !h.State().dirty);
  const auto projects = app.ListProjects();
  assert(std::find(projects.begin(), projects.end(), "GROOVE 1") != projects.end());

  // Keys pressed before the name field opened are released cleanly.
  assert(!h.State().gridDown[2]);

  // RENAME (FN menu, next to SAVE AS): the file on the card changes its name, no copy is left, and the
  // open project goes on under the new one.
  h.Chord(kFn, Grid(3));
  assert(h.Modal() != nullptr && h.Modal()->kind == ModalKind::TextEntry && h.Modal()->text == "GROOVE 1");
  for (int index = 0; index < 8; ++index) h.Tap(kDel);
  TypeText(h, "GROOVE 2");
  h.Tap(kEnter);
  assert(h.Modal() == nullptr && h.State().projectName == "GROOVE 2" && !h.State().dirty);
  {
    const auto renamed = app.ListProjects();
    assert(std::find(renamed.begin(), renamed.end(), "GROOVE 2") != renamed.end());
    assert(std::find(renamed.begin(), renamed.end(), "GROOVE 1") == renamed.end());
  }
  // A name another project has is refused; both stay as they were.
  h.Chord(kFn, Grid(2));  // SAVE AS GROOVE 3: a second project
  for (int index = 0; index < 8; ++index) h.Tap(kDel);
  TypeText(h, "GROOVE 3");
  h.Tap(kEnter);
  assert(h.State().projectName == "GROOVE 3");
  h.Chord(kFn, Grid(3));
  for (int index = 0; index < 8; ++index) h.Tap(kDel);
  TypeText(h, "GROOVE 2");
  h.Tap(kEnter);
  assert(h.Modal() == nullptr && h.State().projectName == "GROOVE 3");
  assert(h.State().toast.text.find("TAKEN") != std::string::npos);
  {
    const auto both = app.ListProjects();
    assert(std::count(both.begin(), both.end(), "GROOVE 2") == 1 && std::count(both.begin(), both.end(), "GROOVE 3") == 1);
  }
  // Back to GROOVE 1, the name the rest of this test expects (and ESC in the field changes nothing).
  h.Chord(kFn, Grid(3));
  h.Tap(kEsc);
  assert(h.Modal() == nullptr && h.State().projectName == "GROOVE 3");
  h.Chord(kFn, Grid(3));
  for (int index = 0; index < 8; ++index) h.Tap(kDel);
  TypeText(h, "GROOVE 1");
  h.Tap(kEnter);
  assert(h.State().projectName == "GROOVE 1");

  // Saving again under the same name overwrites silently.
  h.Tap(Grid(0));
  assert(h.Modal() == nullptr && !h.State().dirty);

  // NEW with unsaved changes asks first; ESC keeps the session.
  h.Tap(Grid(4));
  h.Tap(kAdd);  // BPM + 1 marks the project dirty
  assert(h.State().dirty);
  h.Tap(Grid(2));
  assert(h.Modal() != nullptr && h.Modal()->kind == ModalKind::Confirm);
  h.Tap(kEsc);
  assert(h.Modal() == nullptr && h.State().projectName == "GROOVE 1");
  h.Tap(Grid(2));
  h.Tap(kEnter);
  assert(h.State().projectName.empty() && !h.State().dirty);
  assert(!app.TrackMuted(0));
  assert(app.EngineKind(1) == synth::SynthEngineKind::Juno);
  assert(!app.Sequencer().BuildUiState(0).steps[0].hasEvents);
  const auto reverbSize = static_cast<std::size_t>(synth::fx::MasterParam::ReverbSize);
  assert(app.MasterFx(reverbSize) == synth::fx::kMasterParams[reverbSize].defaultValue);
  assert(ParameterValue(app, 2, "fx.delay") == 0);

  // FILES: load the saved project back, mixer and engines included.
  h.Tap(Grid(1));
  assert(h.App().Ui().CurrentPage() == groove::kSetupFiles);
  assert(!h.State().projects.empty());
  h.Tap(kEnter);
  assert(h.State().projectName == "GROOVE 1");
  assert(app.TrackMuted(0) && app.TrackLevel(1) == 89);
  assert(app.EngineKind(1) == synth::SynthEngineKind::Dx7);
  assert(app.Sequencer().BuildUiState(0).steps[0].hasEvents);
  assert(app.Sequencer().PatternCount(0) == 3);
  // Master effects and the track sends come back with the project.
  assert(app.MasterFx(reverbSize) == synth::fx::kMasterParams[reverbSize].defaultValue - 1);
  assert(ParameterValue(app, 2, "fx.delay") == 11);

  // RENAME on FILES acts on the project under the cursor, not the open one.
  h.Tap(kSub);
  assert(h.State().projects[h.State().fileCursor] == "GROOVE 2");
  h.Chord(kFn, Grid(3));
  assert(h.Modal() != nullptr && h.Modal()->text == "GROOVE 2");
  for (int index = 0; index < 8; ++index) h.Tap(kDel);
  TypeText(h, "OTHER");
  h.Tap(kEnter);
  assert(h.State().projectName == "GROOVE 1");
  assert(std::count(h.State().projects.begin(), h.State().projects.end(), "OTHER") == 1);
  assert(std::count(h.State().projects.begin(), h.State().projects.end(), "GROOVE 2") == 0);
  assert(h.State().projects[h.State().fileCursor] == "OTHER");

  // SETUP SYSTEM page (CTRL steps the pages): key lights and screen.
  while (h.App().Ui().CurrentPage() != groove::kSetupSystem) h.Tap(kCtrl);
  assert(std::string(h.App().Ui().SettingsPageShown()->tab) == "SYSTEM");
  h.Render();
  h.Tap(Grid(1));
  // KEY LED focused: the pads show the brightness ramp, dark to full.
  std::array<std::uint8_t, groove::kKeyCount> ramp{};
  app.Ui().ComputeLeds(ramp);
  assert(ramp[groove::KeyIndex(2, 1)] == 0 && ramp[groove::KeyIndex(2, 3)] == 7 && ramp[groove::KeyIndex(2, 8)] == 15);
  h.Tap(kSub);
  assert(app.LedBrightness() == 14);
  h.Tap(kAdd);
  h.Tap(Grid(2));
  h.Tap(kSub);
  assert(app.Backlight() == 175);
  // LOG: the console log on the card, on unless someone turns it off.
  assert(app.Logging());
  h.Tap(Grid(3));
  h.Tap(kSub);
  assert(!app.Logging());
  h.Tap(kAdd);
  assert(app.Logging());
  // THEME: the screen's palette (sys.theme), DARK unless picked; the next frame draws in it.
  const auto fieldShown = [&] {
    // The field is what most of a frame is.
    h.Render();
    const auto frame = Device::Display::CurrentFrameBuffer();
    std::map<Device::Display::Pixel, std::size_t> counts;
    for (std::size_t index = 0; index < static_cast<std::size_t>(frame.width) * frame.height; ++index) ++counts[frame.pixels[index]];
    return std::max_element(counts.begin(), counts.end(), [](const auto& a, const auto& b) { return a.second < b.second; })->first;
  };
  assert(app.ScreenTheme() == os::storage::Theme::Dark && fieldShown() == groove::theme::kDarkPalette.field);
  h.Tap(Grid(4));
  h.Tap(kAdd);
  assert(app.ScreenTheme() == os::storage::Theme::Light && fieldShown() == groove::theme::kLightPalette.field);
  h.Tap(kAdd);
  h.Tap(kAdd);  // RETRO is the last
  assert(app.ScreenTheme() == os::storage::Theme::Retro && fieldShown() == groove::theme::kRetroPalette.field);
  h.Chord(kFn, kEnter);  // the default
  assert(app.ScreenTheme() == os::storage::Theme::Dark && fieldShown() == groove::theme::kDarkPalette.field);
  h.Tap(kEsc);
  assert(h.State().scene != Scene::Setup);
}

void TestQuickSaveReleasesFn(Harness& h)
{
  // Fn + S on an unnamed project saves it at once as SONG nn: no name field. Letting go of FN
  // must not leave it stuck down. The name field is one step further: SAVE AS.
  GoToSetupPage(h, groove::kSetupProject);
  // FN + ADD / SUB take big steps: BPM by ten.
  h.Tap(Grid(4));
  const auto tempo = h.App().Sequencer().Tempo();
  h.Chord(kFn, kAdd);
  assert(h.App().Sequencer().Tempo() == tempo + 10);
  h.Chord(kFn, kSub);
  h.Tap(Grid(2));  // NEW
  if (h.Modal() != nullptr) h.Tap(kEnter);
  assert(h.State().projectName.empty());
  h.Down(kFn);
  h.Tap(Key{2, 3});  // S
  assert(h.Modal() == nullptr && h.State().projectName.rfind("SONG ", 0) == 0);
  h.Up(kFn);
  assert(!h.State().left[2].down && !h.App().Ui().FnOverlay());
  h.Chord(kFn, Grid(2));  // SAVE AS
  assert(h.Modal() != nullptr && h.Modal()->kind == ModalKind::TextEntry);
  // FN in text entry is the yellow keycap layer: , / move the cursor, ; . jump to the start / end,
  // FN + del deletes forward. No FN panel shows meanwhile.
  while (!h.Modal()->text.empty()) h.Tap(kDel);  // the field opens with a suggested name
  h.Tap(Key{2, 2});  // A
  h.Tap(Key{3, 7});  // B
  h.Down(kFn);
  h.Wait(300);
  assert(!h.App().Ui().FnOverlay());
  h.Tap(Key{3, 10});  // ,
  h.Up(kFn);
  h.Tap(Key{3, 5});  // C
  assert(h.Modal()->text == "ACB" && h.Modal()->textCursor == 2);
  h.Render();
  h.Chord(kFn, Key{2, 11});  // ;
  h.Chord(kFn, kDel);
  assert(h.Modal()->text == "CB" && h.Modal()->textCursor == 0);
  h.Chord(kFn, Key{3, 11});  // .
  h.Tap(kDel);
  assert(h.Modal()->text == "C");
  h.Tap(kEsc);
  assert(h.Modal() == nullptr);
}

// Every key is off (nothing there), half (there, not chosen) or full (chosen, held, sounding).
constexpr int kOff = groove::kLedOff, kHalf = groove::kLedHalf, kFull = groove::kLedFull;

void TestLeds(Harness& h)
{
  // A fresh project on T2 (Juno) in SEQ: no notes anywhere.
  GoToSetupPage(h, groove::kSetupProject);
  h.Tap(Grid(2));  // NEW
  if (h.Modal() != nullptr) h.Tap(kEnter);
  GoToScene(h, 2);
  while (h.App().Ui().CurrentPage() != 0) h.Tap(kCtrl);
  h.Chord(kTab, Grid(1));
  std::array<std::uint8_t, groove::kKeyCount> levels{};
  auto level = [&](Key key) {
    h.App().Ui().ComputeLeds(levels);
    return static_cast<int>(levels[groove::KeyIndex(key.row, key.column)]);
  };

  // Steps: empty off; with notes half; selected full.
  for (std::size_t step = 0; step < 16; ++step) assert(level(Grid(step)) == kOff);
  const std::uint8_t e3 = 52;
  h.App().Sequencer().PressStep(5, &e3, 1);
  h.App().Sequencer().ReleaseStep(5);
  assert(level(Grid(5)) == kHalf && level(Grid(4)) == kOff);
  h.Tap(Grid(5));
  assert(h.Seq().steps[5].held && level(Grid(5)) == kFull);
  // The selected step's note is full on the pads (E3 on the bottom white row), the other keys half.
  assert(level(Key{3, 3}) == kFull && level(Key{3, 1}) == kHalf);
  h.Tap(Grid(5));
  assert(level(Grid(5)) == kHalf);

  // PIANO keyboard: the bottom row is white keys, all half; the row above is the black keys, half
  // where there is one and off in the gaps (before C#, between E and F, after A#).
  for (std::size_t column = 1; column <= 8; ++column) assert(level(Key{3, column}) == kHalf);
  for (std::size_t column = 1; column <= 8; ++column)
  {
    const bool gap = column == 1 || column == 4 || column == 8;
    assert(level(Key{2, column}) == (gap ? kOff : kHalf));
  }
  // A held key is full; letting go brings it back to half.
  h.Down(Key{3, 1});
  assert(level(Key{3, 1}) == kFull && level(Key{3, 2}) == kHalf);
  h.Up(Key{3, 1});
  assert(level(Key{3, 1}) == kHalf);

  // Edge keys: navigation half (CTRL too, SEQ has pages), ADD / SUB half, OK half (the record key:
  // full in the STEP view), PLAY half when stopped and full while playing.
  for (std::size_t row = 0; row < 4; ++row) assert(level(Key{row, 0}) == kHalf);
  assert(level(kAdd) == kHalf && level(kSub) == kHalf && level(kEnter) == kHalf && level(kPlay) == kHalf);
  h.Down(kTab);
  assert(level(kTab) == kFull);
  h.Up(kTab);
  h.Tap(kPlay);
  h.Wait(200);
  assert(level(kPlay) == kFull);
  // The playhead runs full over the steps.
  const auto seq = h.Seq();
  assert(seq.playing && level(Grid(seq.currentStep)) == kFull);
  h.Tap(kPlay);
  h.Wait(200);
  assert(level(kPlay) == kHalf);
  h.Chord(kTab, Grid(0));
}

// Publishes one LED frame the way os::system::RunFrame does and reads a key back.
int Led(Harness& h, Key key)
{
  Device::LED::BeginFrame();
  Device::LED::Clear();
  h.App().UpdateLeds();
  Device::LED::Present();
  return Device::LED::Brightness(key.row, key.column);
}

// The lights fade toward new levels frame by frame (falls take about half a second): publish
// frames until they arrive.
void Settle(Harness& h)
{
  for (int frame = 0; frame < 40; ++frame)
  {
    h.Frames();
    Led(h, kEsc);
  }
}

void TestLedMotion(Harness& h)
{
  GoToScene(h, 2);
  h.Wait(1200);
  // A press lights only its own key: no ring spills onto the neighbours.
  const int before = Led(h, Key{0, 11});
  h.Down(Grid(0));
  h.Wait(70);
  assert(Led(h, Grid(0)) == kFull && Led(h, Key{0, 11}) == before);
  h.Up(Grid(0));
  h.Tap(Grid(0));  // the release selected it: deselect again
  h.Wait(1200);
  // ESC map: the GRID keys come on corner to corner; each column is a scene.
  h.Down(kEsc);
  h.Wait(250);
  assert(h.App().Ui().EscOverlay());
  const int early = Led(h, Grid(15));
  h.Wait(250);
  // Column 4 (TAPE, another scene) is half once it has lit; the current scene (SEQ, column 3) is
  // full; the first column (SETUP) is half like any other scene; the pads go dark.
  assert(Led(h, Grid(15)) > early && Led(h, Grid(15)) == kHalf);
  assert(Led(h, Grid(2)) == kFull && Led(h, Grid(14)) == kFull);
  assert(Led(h, Grid(0)) == kHalf && Led(h, Key{1, 4}) == kOff);
  h.Up(kEsc);
  h.Wait(600);
  // CTRL map: one key per page of the current scene.
  h.Down(kCtrl);
  h.Wait(600);
  assert(h.App().Ui().CtrlOverlay());
  Settle(h);
  assert(Led(h, Grid(0)) == kFull && Led(h, Grid(1)) == kHalf && Led(h, Grid(2)) == kHalf && Led(h, Grid(3)) == kOff);
  h.Up(kCtrl);
  assert(h.App().Ui().CurrentPage() == 0);
  h.Wait(600);
  // Panic (FN + ESC): every key flares, then the lights settle back.
  h.Chord(kFn, kEsc);
  h.Frames();
  // The edge keys never go above the dimmest glow, even here.
  // (A playable key rests at half; the chord takes a few frames, so the flare has begun to fade.)
  assert(Led(h, Key{1, 5}) >= 12 && Led(h, Key{3, 0}) <= groove::kLedEdgeMax && Led(h, Key{3, 0}) > 0);
  h.Wait(900);
  assert(Led(h, Key{1, 5}) < 12);
}

std::size_t PadSlot(Key key) { return key.row * groove::kPadColumns + (key.column - groove::kPadFirstColumn); }

void TestPlayFeatures(Harness& h)
{
  auto& app = h.App();
  const auto& keys = h.State().keyboard;
  GoToScene(h, 1);
  h.Chord(kTab, Grid(1));  // T2, a melodic track
  assert(h.State().track == 1 && app.EngineKind(1) != synth::SynthEngineKind::Drums);
  h.Wait(900);
  // The default keyboard is the piano with C on the second row from the bottom, C major.
  assert(keys.layout == groove::KeyboardLayout::Piano && keys.scale == 0 && keys.root == 0);
  const Key c3{3, 1}, e3{3, 3}, g3{3, 5}, cSharp{2, 2};
  int note = 0;
  assert(app.Ui().PadNoteFor(c3.row, c3.column, note) && note == 48);
  assert(app.Ui().PadNoteFor(cSharp.row, cSharp.column, note) && note == 49);
  // Key lights: every key that sounds is half, white or black.
  std::array<std::uint8_t, groove::kKeyCount> levels{};
  app.Ui().ComputeLeds(levels);
  assert(levels[groove::KeyIndex(c3.row, c3.column)] == kHalf && levels[groove::KeyIndex(e3.row, e3.column)] == kHalf);
  assert(levels[groove::KeyIndex(cSharp.row, cSharp.column)] == kHalf);

  // INSTR CHORD (the track's MIDI effect): TRIAD turns one key into C E G, and the chord tones light
  // on the pads. Each page starts focused on its first setting.
  GoToInstrumentPage(h, "CHORD");
  assert(app.Ui().InstrumentFocus() == 0);
  h.Tap(kAdd);
  const auto& shape = h.State().chords[1];
  assert(shape.chord == groove::ChordType::Triad);
  h.Down(c3);
  const auto& chord = h.State().padChord[PadSlot(c3)];
  assert(h.State().padChordCount[PadSlot(c3)] == 3 && chord[0] == 48 && chord[1] == 52 && chord[2] == 55);
  app.Ui().ComputeLeds(levels);
  assert(levels[groove::KeyIndex(c3.row, c3.column)] == kFull && levels[groove::KeyIndex(e3.row, e3.column)] == kFull &&
         levels[groove::KeyIndex(g3.row, g3.column)] == kFull);
  h.Render();
  h.Up(c3);
  // VOICING INV1 lifts the root an octave; Fn + ENTER restores the default.
  h.Tap(Grid(1));
  h.Tap(kAdd);
  h.Down(c3);
  assert(chord[0] == 52 && chord[1] == 55 && chord[2] == 60);
  h.Up(c3);
  h.Chord(kFn, kEnter);
  assert(shape.voicing == 0);
  // The chord belongs to T2: T3 still plays single notes, and comes back to its own page.
  h.Chord(kTab, Grid(2));
  h.Down(c3);
  assert(h.State().padChordCount[PadSlot(c3)] == 1 && h.State().chords[2].chord == groove::ChordType::Off);
  h.Up(c3);
  h.Chord(kTab, Grid(1));
  assert(std::string(app.Ui().CurrentInstrumentPage()->tab) == "CHORD");
  // A chord's pad on a selected step: the chord goes in whole and comes out whole. With one of its notes
  // in the step (E, alone) a press takes it out and puts nothing in; then all three go in, then out.
  {
    auto& sequencer = app.Sequencer();
    GoToScene(h, 2);
    while (app.Ui().CurrentPage() != 0) h.Tap(kCtrl);
    assert(h.State().scene == Scene::Sequence && h.State().track == 1);
    const std::uint8_t e = 52;
    std::array<synth::SynthSequencer::StepNoteView, 8> views{};
    // An empty step of the page on the GRID (the project's pattern has notes on some); it is empty again after.
    const std::size_t first = h.Seq().firstViewStep;
    std::size_t cell = 0;
    while (cell < 16 && sequencer.StepNoteEvents(1, first + cell, views) != 0) ++cell;
    assert(cell < 16);
    const std::size_t step = first + cell;
    assert(sequencer.ToggleStepNotes(1, step, &e, 1));
    h.Tap(Grid(cell));
    assert(h.Seq().steps[cell].held);
    h.Tap(c3);  // C E G: E is there, so it comes out and C and G stay out
    assert(sequencer.StepNoteEvents(1, step, views) == 0);
    h.Tap(c3);  // none there: all three go in
    assert(sequencer.StepNoteEvents(1, step, views) == 3);
    std::array<int, 3> pitches{views[0].note, views[1].note, views[2].note};
    std::sort(pitches.begin(), pitches.end());
    assert((pitches == std::array<int, 3>{48, 52, 55}));
    h.Tap(c3);
    assert(sequencer.StepNoteEvents(1, step, views) == 0);
    h.Tap(kEsc);  // lets the step go
    assert(!h.Seq().hasHeldSteps);
    GoToScene(h, 1);
    assert(std::string(app.Ui().CurrentInstrumentPage()->tab) == "CHORD");
  }
  // SETUP KEYS LOCK (the keyboard of every track): MUTE silences keys outside the scale (and darkens
  // them); SNAP moves them down to it.
  GoToSetupPage(h, groove::kSetupKeys);
  h.Tap(Grid(5));
  h.Tap(kAdd);
  assert(keys.lock == groove::ScaleLock::Mute);
  h.Down(cSharp);
  assert(h.State().padNote[PadSlot(cSharp)] == 0xFF);
  h.Up(cSharp);
  app.Ui().ComputeLeds(levels);
  assert(levels[groove::KeyIndex(cSharp.row, cSharp.column)] == 0);
  h.Tap(kAdd);
  h.Down(cSharp);
  assert(h.State().padChordCount[PadSlot(cSharp)] == 3 && h.State().padChord[PadSlot(cSharp)][0] == 48);
  h.Up(cSharp);
  h.Chord(kFn, kEnter);
  assert(keys.lock == groove::ScaleLock::Off);
  // VELO: what every pad plays.
  h.Tap(Grid(4));
  h.Tap(kSub);
  assert(keys.velocity == 109);
  h.Chord(kFn, kEnter);
  assert(keys.velocity == 110);
  // Every SETUP page draws; ESC goes back to INSTR, still on CHORD.
  for (std::uint8_t page = 0; page < groove::kSetupPageCount; ++page)
  {
    while (app.Ui().CurrentPage() != page) h.Tap(kCtrl);
    h.Render();
  }
  h.Tap(kEsc);
  assert(h.State().scene == Scene::Instrument && std::string(app.Ui().CurrentInstrumentPage()->tab) == "CHORD");
  // CHORD back to OFF (a second press on the focused CHORD steps it); then ARP, T2's own: UP.
  assert(app.Ui().InstrumentFocus() == 1);  // the page kept its focus: VOICING
  h.Tap(Grid(0));
  assert(app.Ui().InstrumentFocus() == 0 && shape.chord == groove::ChordType::Triad);
  h.Tap(Grid(0));
  assert(shape.chord != groove::ChordType::Triad);
  while (shape.chord != groove::ChordType::Off) h.Tap(kSub);
  h.Tap(kCtrl);
  assert(std::string(app.Ui().CurrentInstrumentPage()->tab) == "ARP");
  h.Render();
  h.Tap(kAdd);  // ARP UP
  assert(app.Sequencer().GetArpSettings(1).mode == synth::ArpMode::Up && app.Sequencer().GetArpSettings(2).mode == synth::ArpMode::Off);
  // ARP: a held key feeds T2's arpeggiator; letting go stops it (latch is off). The menu draws over it.
  h.Down(c3);
  assert(app.Sequencer().ArpActive(1));
  h.Wait(300);
  h.Down(kFn);
  h.Wait(300);
  h.Render();
  h.Up(kFn);
  h.Up(c3);
  h.Wait(50);
  assert(!app.Sequencer().ArpActive());
  h.Tap(kSub);
  assert(app.Sequencer().GetArpSettings(1).mode == synth::ArpMode::Off);

  // FN + TAB = previous track, FN + ESC = all stop; neither does its plain job.
  h.Chord(kFn, kTab);
  assert(h.State().track == 0 && h.State().scene == Scene::Instrument);
  const std::uint32_t panic = h.State().panicAt;
  h.Chord(kFn, kEsc);
  assert(h.State().panicAt != panic && h.State().scene == Scene::Instrument);
  h.Wait(900);
  // After a track change the left column says which track: T2 = the second key.
  h.Tap(kTab);
  h.Wait(60);
  assert(Led(h, kTab) == groove::kLedEdgeMax && Led(h, kEsc) == 0 && Led(h, kCtrl) == 0);
  h.Wait(900);
  // Holding FN lights only the keys that have an FN function: the menu items on the GRID, their
  // letters and the quick keys on the pads, ADD / SUB (big steps).
  h.Down(kFn);
  h.Wait(300);
  app.Ui().ComputeLeds(levels);
  assert(levels[groove::KeyIndex(0, 13)] == kHalf && levels[groove::KeyIndex(1, 13)] == kHalf);  // big steps
  assert(levels[groove::KeyIndex(0, 1)] == kOff && levels[groove::KeyIndex(0, 3)] == kOff);      // 1, 3: nothing
  assert(levels[groove::KeyIndex(0, 6)] == kHalf && levels[groove::KeyIndex(1, 6)] == kHalf);  // quick BPM + / -
  assert(levels[groove::KeyIndex(Grid(0).row, Grid(0).column)] == kHalf);                         // PRESETS
  assert(levels[groove::KeyIndex(Grid(15).row, Grid(15).column)] == kHalf);                       // SETUP
  assert(levels[groove::KeyIndex(Grid(8).row, Grid(8).column)] == kOff);                          // nothing there
  assert(levels[groove::KeyIndex(3, 5)] == kOff);                                                 // C: no COPY in INSTR
  assert(levels[groove::KeyIndex(3, 13)] == kHalf && levels[groove::KeyIndex(0, 0)] == kHalf);  // REC, STOP
  assert(levels[groove::KeyIndex(1, 2)] == kOff);                                                 // W: nothing
  assert(levels[groove::KeyIndex(2, 3)] == kHalf);                                                // S: SAVE
  h.Up(kFn);
}

// SEQ note page, a lit pad (its note is in a selected step): a tap takes the note out when it lets go,
// never while it is held; held, ADD / SUB act on its notes alone, on the current edit lane. NOTE moves
// them a key of the keyboard at a time, past the step's other notes (on a drum track to the next drum
// of the kit); LEN / VEL / TIME change only their values.
void TestHeldPadEdits(Harness& h)
{
  auto& app = h.App();
  auto& sequencer = app.Sequencer();
  const Scene startScene = h.State().scene;
  const std::size_t startTrack = h.State().track;
  GoToScene(h, 2);
  while (app.Ui().CurrentPage() != 0) h.Tap(kCtrl);
  h.Chord(kTab, Grid(1));  // T2, melodic, chord off, the piano keyboard in C major
  assert(h.State().track == 1 && !app.Ui().DrumTrack() && h.State().chords[1].chord == groove::ChordType::Off);
  assert(h.State().keyboard.layout == groove::KeyboardLayout::Piano && h.State().keyboard.lock == groove::ScaleLock::Off &&
         h.State().keyboard.octave == 0 && h.State().keyboard.scale == 0 && !h.State().keyboard.multiSelect);
  assert(!sequencer.Playing() && h.Seq().editLane == synth::SequencerEditLane::Note);
  const Key c3{3, 1}, d3{3, 2}, e3{3, 3}, g3{3, 5};
  // Empty steps of the page on the GRID (the project's pattern has notes on some).
  std::array<synth::SynthSequencer::StepNoteView, 8> views{};
  std::vector<std::size_t> cells;
  const std::size_t first = h.Seq().firstViewStep;
  for (std::size_t cell = 0; cell < 16 && cells.size() < 3; ++cell)
    if (sequencer.StepNoteEvents(1, first + cell, views) == 0) cells.push_back(cell);
  assert(cells.size() == 3);
  const auto pitches = [&](std::size_t cell) {
    std::vector<int> notes;
    const std::size_t count = sequencer.StepNoteEvents(1, first + cell, views);
    for (std::size_t slot = 0; slot < count; ++slot) notes.push_back(views[slot].note);
    std::sort(notes.begin(), notes.end());
    return notes;
  };
  const auto event = [&](std::size_t cell, int note) {
    const std::size_t count = sequencer.StepNoteEvents(1, first + cell, views);
    for (std::size_t slot = 0; slot < count; ++slot)
      if (views[slot].note == note) return views[slot];
    assert(false);
    return views[0];
  };
  const std::array<std::uint8_t, 3> triad{48, 52, 55};
  assert(sequencer.ToggleStepNotes(1, first + cells[0], triad.data(), triad.size()));
  h.Tap(Grid(cells[0]));
  assert(h.Seq().steps[cells[0]].held);

  // A tap on a lit pad: E comes out at the release, not before.
  h.Down(e3);
  assert((pitches(cells[0]) == std::vector<int>{48, 52, 55}));
  h.Wait(600);  // however long it is held
  assert((pitches(cells[0]) == std::vector<int>{48, 52, 55}));
  h.Up(e3);
  assert((pitches(cells[0]) == std::vector<int>{48, 55}));
  // A pad whose note is not there puts it in as it goes down.
  h.Down(e3);
  assert((pitches(cells[0]) == std::vector<int>{48, 52, 55}));
  h.Up(e3);
  assert((pitches(cells[0]) == std::vector<int>{48, 52, 55}));

  // Held + ADD: C moves up a semitone, the chord's other notes stay, and the release keeps it. It sounds
  // where it went (stopped), and the next ADD moves the same note, now on another pad's key; E is taken,
  // so C# D D# ... F.
  const auto velocity = event(cells[0], 48).velocity;
  const auto length = event(cells[0], 48).lengthPulses;
  h.Down(c3);
  h.Tap(kAdd);
  assert((pitches(cells[0]) == std::vector<int>{49, 52, 55}) && h.State().hud.label == "NOTE" && h.State().hud.value == "C#3");
  assert(h.State().padChord[PadSlot(c3)][0] == 49 && h.State().padChordCount[PadSlot(c3)] == 1);
  h.Tap(kAdd);
  h.Tap(kAdd);
  assert((pitches(cells[0]) == std::vector<int>{51, 52, 55}));
  h.Tap(kAdd);  // past E
  assert((pitches(cells[0]) == std::vector<int>{52, 53, 55}) && h.State().hud.value == "F3");
  assert(event(cells[0], 53).velocity == velocity && event(cells[0], 53).lengthPulses == length);
  h.Tap(kSub);
  assert((pitches(cells[0]) == std::vector<int>{51, 52, 55}));
  h.Up(c3);
  assert((pitches(cells[0]) == std::vector<int>{51, 52, 55}) && h.Seq().steps[cells[0]].held);
  // ADD / SUB without a pad are the lane's again: the whole step.
  h.Tap(kAdd);
  assert((pitches(cells[0]) == std::vector<int>{52, 53, 56}));
  h.Tap(kSub);
  // The pad lets go while ADD repeats: the repeat does nothing more (no lane edit, no page flip).
  h.Down(e3);
  h.Down(kAdd);
  h.Up(e3);
  const auto moved = pitches(cells[0]);
  assert((moved == std::vector<int>{51, 53, 55}));
  h.Wait(900);
  assert(pitches(cells[0]) == moved && h.Seq().steps[cells[0]].held);
  h.Up(kAdd);

  // Other lanes: the held pad's note alone. VEL (OK twice), then LEN and TIME.
  h.Tap(kEnter);
  h.Tap(kEnter);
  assert(h.Seq().editLane == synth::SequencerEditLane::Velocity);
  const auto gVelocity = event(cells[0], 55).velocity;
  const auto otherVelocity = event(cells[0], 51).velocity;
  h.Down(g3);
  h.Tap(kSub);
  h.Tap(kSub);
  h.Up(g3);
  assert(event(cells[0], 55).velocity == gVelocity - 2 && event(cells[0], 51).velocity == otherVelocity);
  assert(h.State().hud.label == "VEL G3" && h.State().hud.value == std::to_string(gVelocity - 2));
  assert((pitches(cells[0]) == std::vector<int>{51, 53, 55}));  // the release kept it
  h.Tap(kEnter);  // TIME
  assert(h.Seq().editLane == synth::SequencerEditLane::Micro);
  h.Down(g3);
  h.Tap(kAdd);
  h.Up(g3);
  assert(event(cells[0], 55).microstep == 1 && event(cells[0], 51).microstep == 0 && event(cells[0], 53).microstep == 0);
  // HITS: the held pad's note fires 3 times in its step, the others stay single; the edit strip names
  // the lane and the HUD the note.
  h.Tap(kEnter);
  assert(h.Seq().editLane == synth::SequencerEditLane::Hits && h.Seq().valueLabel == "HITS 1");
  h.Down(g3);
  h.Tap(kAdd);
  h.Tap(kAdd);
  h.Up(g3);
  assert(event(cells[0], 55).hits == 3 && event(cells[0], 51).hits == 1 && event(cells[0], 53).hits == 1);
  assert(h.State().hud.label == "HITS G3" && h.State().hud.value == "3" && h.Seq().valueLabel == "HITS MIXED");
  assert((pitches(cells[0]) == std::vector<int>{51, 53, 55}));  // the release kept it
  // Without a pad, the whole step: up to 4 and no further, back down to 1.
  for (int press = 0; press < 4; ++press) h.Tap(kAdd);
  assert(event(cells[0], 55).hits == 4 && event(cells[0], 51).hits == 4 && h.Seq().valueLabel == "HITS 4");
  for (int press = 0; press < 4; ++press) h.Tap(kSub);
  assert(event(cells[0], 55).hits == 1 && event(cells[0], 53).hits == 1 && h.Seq().valueLabel == "HITS 1");
  h.Tap(kEnter);  // NOTE
  h.Tap(kEnter);  // LEN
  assert(h.Seq().editLane == synth::SequencerEditLane::Length);
  const auto gLength = event(cells[0], 55).lengthPulses;
  h.Down(g3);
  h.Tap(kAdd);
  h.Up(g3);
  assert(event(cells[0], 55).lengthPulses > gLength && event(cells[0], 51).lengthPulses == length);
  h.Tap(kEnter);
  h.Tap(kEnter);
  h.Tap(kEnter);
  h.Tap(kEnter);  // back to NOTE
  assert(h.Seq().editLane == synth::SequencerEditLane::Note);
  h.Tap(kEsc);
  assert(!h.Seq().hasHeldSteps);

  // Several steps selected (held together): the note moves on each that has it; a step without it
  // neither moves nor blocks the move.
  const std::uint8_t c = 48, d = 50;
  assert(sequencer.SetStepNotes(1, first + cells[1], &c, 1, 100) && sequencer.SetStepNotes(1, first + cells[2], &d, 1, 100));
  h.Down(Grid(cells[1]));
  h.Down(Grid(cells[2]));
  h.Down(c3);
  h.Tap(kAdd);
  h.Tap(kAdd);  // C# D: D is only in the step that has no C
  h.Up(c3);
  h.Up(Grid(cells[2]));
  h.Up(Grid(cells[1]));
  assert((pitches(cells[1]) == std::vector<int>{50}) && (pitches(cells[2]) == std::vector<int>{50}));
  assert((pitches(cells[0]) == std::vector<int>{51, 53, 55}) && !h.Seq().hasHeldSteps);

  // The CHORD effect: a pad is its chord, and its notes move as the next key's chord (C# borrows a
  // major chord, D is the scale's D minor).
  GoToInstrumentPage(h, "CHORD");
  h.Tap(kAdd);
  assert(h.State().chords[1].chord == groove::ChordType::Triad);
  GoToScene(h, 2);
  assert(sequencer.SetStepNotes(1, first + cells[1], triad.data(), triad.size(), 100));
  h.Tap(Grid(cells[1]));
  h.Down(c3);
  h.Tap(kAdd);
  assert((pitches(cells[1]) == std::vector<int>{49, 53, 56}));
  h.Tap(kAdd);
  assert((pitches(cells[1]) == std::vector<int>{50, 53, 57}) && h.State().hud.value == "D3 F3 A3");
  h.Up(c3);
  assert((pitches(cells[1]) == std::vector<int>{50, 53, 57}));
  h.Tap(d3);  // D's chord is there now: a tap takes it out
  assert(pitches(cells[1]).empty());
  h.Tap(kEsc);
  GoToInstrumentPage(h, "CHORD");
  h.Tap(kSub);
  assert(h.State().chords[1].chord == groove::ChordType::Off);
  GoToScene(h, 2);
  for (const std::size_t cell : cells) assert(sequencer.SetStepNotes(1, first + cell, nullptr, 0, 0));

  // A drum track: NOTE moves a held hit to the next drum of the kit the step does not hold, keeping its
  // values (and the release keeps it); the other lanes edit it.
  h.Chord(kTab, Grid(0));
  assert(app.Ui().DrumTrack());
  const Key bd{0, 1}, sd{0, 2};
  const std::size_t drumFirst = h.Seq().firstViewStep;
  std::size_t drumCell = 0;
  while (drumCell < 16 && sequencer.StepNoteEvents(0, drumFirst + drumCell, views) != 0) ++drumCell;
  assert(drumCell < 16);
  const std::array<std::uint8_t, 2> hits{36, 38};
  assert(sequencer.SetStepNotes(0, drumFirst + drumCell, hits.data(), hits.size(), 100));
  const auto drumPitches = [&]() {
    std::vector<int> notes;
    const std::size_t count = sequencer.StepNoteEvents(0, drumFirst + drumCell, views);
    for (std::size_t slot = 0; slot < count; ++slot) notes.push_back(views[slot].note);
    std::sort(notes.begin(), notes.end());
    return notes;
  };
  h.Tap(Grid(drumCell));
  h.Down(bd);
  h.Tap(kAdd);  // BD past SD (there already) to CH
  assert((drumPitches() == std::vector<int>{38, 42}) && h.State().hud.label == "DRUM" && h.State().hud.value == "CH");
  assert(app.Ui().DrumLane() == 2);
  for (std::size_t slot = 0; slot < sequencer.StepNoteEvents(0, drumFirst + drumCell, views); ++slot) assert(views[slot].velocity == 100);
  h.Tap(kSub);  // back past SD to BD
  assert((drumPitches() == std::vector<int>{36, 38}) && h.State().hud.value == "BD");
  h.Tap(kSub);  // no drum below BD
  assert((drumPitches() == std::vector<int>{36, 38}));
  h.Up(bd);
  assert((drumPitches() == std::vector<int>{36, 38}));
  h.Down(sd);
  assert(sequencer.StepNoteEvents(0, drumFirst + drumCell, views) == 2);
  h.Up(sd);
  assert(sequencer.StepNoteEvents(0, drumFirst + drumCell, views) == 1 && views[0].note == 36);
  h.Tap(kEnter);
  h.Tap(kEnter);  // VEL
  h.Down(bd);
  h.Tap(kSub);
  h.Up(bd);
  assert(sequencer.StepNoteEvents(0, drumFirst + drumCell, views) == 1 && views[0].velocity == 99 && h.State().hud.label == "VEL BD");
  h.Tap(kEnter);
  h.Tap(kEnter);  // HITS: a roll on the held hit
  h.Down(bd);
  h.Tap(kAdd);
  h.Tap(kAdd);
  h.Tap(kAdd);
  h.Up(bd);
  assert(sequencer.StepNoteEvents(0, drumFirst + drumCell, views) == 1 && views[0].hits == 4 && h.State().hud.label == "HITS BD" &&
         h.State().hud.value == "4");
  h.Tap(kEnter);  // back to NOTE
  assert(h.Seq().editLane == synth::SequencerEditLane::Note);
  h.Tap(kEsc);
  assert(sequencer.SetStepNotes(0, drumFirst + drumCell, nullptr, 0, 0));
  h.Chord(kTab, Grid(startTrack));
  if (h.State().scene != startScene) GoToScene(h, static_cast<int>(startScene));
}

void TestFnQuick(Harness& h)
{
  auto& app = h.App();
  // FN + 6 / Y: BPM up / down, FN + 7 / U: volume, FN + 8 / I: octave, from any FN page, with a
  // pop-up showing the value. Holding repeats.
  const auto bpm = app.Sequencer().Tempo();
  const auto volume = app.MasterVolume();
  const int octave = h.State().keyboard.octave;
  h.Down(kFn);
  h.Tap(Key{0, 6});
  assert(app.Sequencer().Tempo() == bpm + 1 && h.State().hud.label == "BPM");
  h.Tap(Key{1, 6});
  assert(app.Sequencer().Tempo() == bpm);
  h.Tap(Key{1, 7});
  assert(app.MasterVolume() == std::max(0, volume - 2) && h.State().hud.label == "VOLUME");
  h.Tap(Key{0, 7});
  assert(app.MasterVolume() == volume);
  h.Tap(Key{0, 8});
  assert(h.State().keyboard.octave == std::min(3, octave + 1) && h.State().hud.label == "OCTAVE");
  h.Tap(Key{1, 8});
  assert(h.State().keyboard.octave == octave);
  // Held: repeats until the key is let go, and the quick key is lit full meanwhile.
  h.Down(Key{0, 6});
  h.Wait(800);
  std::array<std::uint8_t, groove::kKeyCount> levels{};
  app.Ui().ComputeLeds(levels);
  assert(levels[groove::KeyIndex(0, 6)] == kFull && levels[groove::KeyIndex(0, 13)] == kHalf);
  h.Up(Key{0, 6});
  const auto raised = app.Sequencer().Tempo();
  assert(raised >= bpm + 4);
  h.Wait(300);
  assert(app.Sequencer().Tempo() == raised);
  while (app.Sequencer().Tempo() > bpm) h.Tap(Key{1, 6});
  h.Up(kFn);
  // The pads play again after FN.
  assert(!h.App().Ui().FnOverlay());
}

void TestRecording(Harness& h)
{
  auto& app = h.App();
  auto& sequencer = app.Sequencer();
  GoToScene(h, 2);
  while (app.Ui().CurrentPage() != 0) h.Tap(kCtrl);
  h.Chord(kTab, Grid(1));  // T2 (Juno)
  if (sequencer.Playing()) h.Tap(kPlay);
  // A fresh pattern to write into.
  const std::size_t patterns = sequencer.PatternCount(1);
  h.Down(kCtrl);
  h.Wait(300);
  h.Up(kCtrl);
  h.Chord(kCtrl, Grid(1));  // PATTERN page
  h.Tap(Grid(patterns));    // new pattern
  h.Chord(kCtrl, Grid(0));
  assert(sequencer.ActivePattern(1) == patterns && !h.Seq().populatedStepCount);

  // A tap on OK switches the note page between PIANO ROLL and STEPS. Both take the same keys.
  if (app.Ui().View() != groove::SeqView::Roll) h.Tap(kEnter);
  assert(app.Ui().View() == groove::SeqView::Roll && app.Ui().PageTab(Scene::Sequence, 0) == "ROLL");
  h.Render();
  h.Tap(kEnter);
  assert(app.Ui().View() == groove::SeqView::Step && h.State().hud.value == "STEPS" && app.Ui().PageTab(Scene::Sequence, 0) == "STEP");
  groove::LedFrame frame{};
  app.Ui().ComputeLeds(frame);
  assert(frame[groove::KeyIndex(kEnter.row, kEnter.column)].level == kFull);
  for (std::size_t step = 0; step < 16; ++step)
    assert(frame[groove::KeyIndex(Grid(step).row, Grid(step).column)].effect == groove::LedEffect::Steady);  // no write cursor
  const auto gridLevel = [&](std::size_t step) {
    app.Ui().ComputeLeds(frame);
    return static_cast<int>(frame[groove::KeyIndex(Grid(step).row, Grid(step).column)].level);
  };

  // A tap selects a step and keeps it: the pads put their notes in or take them out, and the
  // selection stays where it is (nothing moves on by itself).
  const Key c3{3, 1}, e3{3, 3}, g3{3, 5};
  std::array<std::uint8_t, 8> notes{};
  h.Tap(Grid(0));
  assert(h.Seq().hasHeldSteps && h.Seq().steps[0].held && gridLevel(0) == kFull);
  h.Tap(c3);
  h.Tap(e3);
  assert(sequencer.StepNotes(0, notes) == 2 && notes[0] == 48 && notes[1] == 52 && h.Seq().steps[0].held && h.Seq().heldStepCount == 1);
  h.Tap(e3);
  assert(sequencer.StepNotes(0, notes) == 1 && notes[0] == 48);
  // ADD / SUB and OK edit it, as in the piano roll; the selection stays.
  h.Tap(kEnter);
  assert(h.Seq().editLane == synth::SequencerEditLane::Length && app.Ui().View() == groove::SeqView::Step);
  h.Tap(kEnter);
  h.Tap(kEnter);
  h.Tap(kEnter);
  assert(h.Seq().editLane == synth::SequencerEditLane::Hits);
  h.Tap(kEnter);
  assert(h.Seq().editLane == synth::SequencerEditLane::Note && h.Seq().steps[0].held);
  // A tap on the step selected alone lets it go, and the pads play again.
  h.Tap(Grid(0));
  assert(!h.Seq().hasHeldSteps);
  h.Tap(c3);
  assert(sequencer.StepNotes(0, notes) == 1);
  // A tap on another step moves the selection there (SELECT ONE).
  h.Tap(Grid(0));
  h.Tap(Grid(3));
  assert(!h.Seq().steps[0].held && h.Seq().steps[3].held && h.Seq().heldStepCount == 1);
  h.Tap(Grid(3));
  assert(!h.Seq().hasHeldSteps);

  // Held (MatrixOS): a step is selected while its key is down, steps held together are edited
  // together, and they let go with their keys.
  h.Down(Grid(1));
  h.Down(Grid(2));
  assert(h.Seq().heldStepCount == 2);
  h.Tap(g3);
  h.Up(Grid(2));
  h.Up(Grid(1));
  assert(!h.Seq().hasHeldSteps);
  assert(sequencer.StepNotes(1, notes) == 1 && notes[0] == 55 && sequencer.StepNotes(2, notes) == 1 && notes[0] == 55);
  // Held long with nothing else pressed: still a hold.
  h.Down(Grid(3));
  h.Wait(400);
  assert(h.Seq().steps[3].held);
  h.Up(Grid(3));
  assert(!h.Seq().hasHeldSteps);
  // A selected step held to edit it stays selected.
  h.Tap(Grid(1));
  h.Down(Grid(1));
  h.Tap(kAdd);
  h.Up(Grid(1));
  assert(h.Seq().steps[1].held);
  h.Tap(kEsc);
  assert(!h.Seq().hasHeldSteps && h.State().scene == Scene::Sequence);

  // Pads held, steps tapped: their notes go in, or come out of a step that has them. The steps
  // that hold them are full; steps left alone are rests.
  h.Down(e3);
  h.Tap(Grid(8));
  h.Tap(Grid(10));
  assert(sequencer.StepNotes(8, notes) == 1 && notes[0] == 52 && sequencer.StepNotes(10, notes) == 1 && notes[0] == 52);
  assert(gridLevel(8) == kFull && gridLevel(1) == kHalf && gridLevel(9) == kOff);
  h.Tap(Grid(10));
  assert(sequencer.StepNotes(10, notes) == 0 && !h.Seq().hasHeldSteps);
  h.Up(e3);
  assert(gridLevel(8) == kHalf);
  // FN + X clears the selected step, not the pattern.
  h.Tap(Grid(8));
  h.Down(kFn);
  assert(h.App().Ui().Menu()[2].detail == "STEPS");
  h.Tap(Key{3, 4});  // X
  h.Up(kFn);
  assert(!h.App().Ui().MenuOpen() && h.Modal() == nullptr);
  assert(sequencer.StepNotes(8, notes) == 0 && sequencer.StepNotes(1, notes) == 1 && h.State().toast.text == "STEPS CLEARED");
  h.Tap(kEsc);
  for (const std::size_t step : {std::size_t{0}, std::size_t{1}, std::size_t{2}}) assert(sequencer.SetStepNotes(1, step, nullptr, 0, 0));
  assert(h.Seq().populatedStepCount == 0);  // nothing left in the pattern
  h.Render();
  h.Tap(kEnter);
  assert(app.Ui().View() == groove::SeqView::Roll);
  h.Render();

  // Capture: notes played without recording are kept as takes, split by pauses (the pads played
  // above are the take before).
  h.Wait(3000);
  const std::size_t takes = app.Ui().TakeCount();
  for (const Key pad : {c3, e3, g3, e3})
  {
    h.Down(pad);
    h.Wait(100);
    h.Up(pad);
    h.Wait(150);
  }
  assert(app.Ui().TakeCount() == takes + 1);
  const groove::CaptureTake* take = app.Ui().TakeAt(0);
  assert(take != nullptr && take->count == 4 && take->track == 1 && !take->synced);
  h.Wait(3000);
  h.Tap(c3);
  assert(app.Ui().TakeCount() == takes + 2 && app.Ui().TakeAt(0)->count == 1 && app.Ui().TakeAt(1)->count == 4);

  // Hold OK: the RECORD panel, on the newest take. The previous take goes into a new pattern.
  h.Down(kEnter);
  h.Wait(600);
  assert(app.Ui().RecordOpen() && h.State().record.focus == groove::kRecordFirstTake);
  h.Up(kEnter);
  assert(app.Ui().RecordOpen());
  h.Render();
  h.Tap(Grid(groove::kRecordFirstTake + 1));
  std::vector<groove::Controller::PlacedNote> placed;
  app.Ui().ForEachTakeBarNote(*app.Ui().TakeAt(1), 0, [](const groove::Controller::PlacedNote& note, void* context) {
    static_cast<std::vector<groove::Controller::PlacedNote>*>(context)->push_back(note);
  }, &placed);
  assert(placed.size() == 4 && placed[0].step == 0 && placed[1].step == 2);
  h.Render();
  const std::size_t before = sequencer.PatternCount(1);
  h.Tap(kEnter);
  assert(!app.Ui().RecordOpen() && sequencer.PatternCount(1) == before + 1 && sequencer.ActivePattern(1) == before);
  assert(h.State().hud.label.rfind("TAKE", 0) == 0 && h.Seq().populatedStepCount == 4);
  assert(sequencer.StepNotes(0, notes) == 1 && notes[0] == 48);
  // FN + OK merges the newest take into the pattern that is playing.
  h.Down(kEnter);
  h.Wait(600);
  h.Up(kEnter);
  h.Chord(kFn, kEnter);
  assert(sequencer.PatternCount(1) == before + 1 && h.State().hud.value.rfind("MERGED", 0) == 0);
  // REC from the panel arms recording like FN + PLAY.
  h.Down(kEnter);
  h.Wait(600);
  h.Up(kEnter);
  h.Tap(Grid(groove::kRecordRec));
  assert(sequencer.RecordArmed());
  h.Tap(Grid(groove::kRecordRec));
  assert(!sequencer.RecordArmed());
  h.Tap(kEsc);
  assert(!app.Ui().RecordOpen());
}

void TestSession(Harness& h)
{
  auto& app = h.App();
  auto& sequencer = app.Sequencer();
  GoToScene(h, 3);
  while (h.App().Ui().CurrentPage() != groove::kTapeSession) h.Tap(kCtrl);
  if (sequencer.Playing()) h.Tap(kPlay);
  const std::size_t patterns = sequencer.PatternCount(1);
  assert(patterns < 4);
  // Columns are tracks, rows pattern slots. The slot after the last pattern creates one, and while
  // stopped the track switches to it at once.
  h.Tap(Grid(patterns * 4 + 1));
  assert(sequencer.PatternCount(1) == patterns + 1 && sequencer.ActivePattern(1) == patterns);
  assert(h.State().track == 1 && h.State().sessionTrack == 1 && h.State().sessionPattern == patterns);
  h.Wait(400);
  std::array<std::uint8_t, groove::kKeyCount> levels{};
  app.Ui().ComputeLeds(levels);
  const auto grid = [](std::size_t index) { return groove::KeyIndex(index / 4, 9 + index % 4); };
  assert(levels[grid(patterns * 4 + 1)] == kFull);                 // the track's current pattern
  assert(levels[grid(0 * 4 + 1)] == kHalf);                         // another pattern of T2
  assert(levels[grid((patterns + 1) * 4 + 1)] == kOff);             // "+": no pattern there yet
  assert(levels[grid(3 * 4 + 0)] == kOff || sequencer.PatternCount(0) > 3);  // nothing there
  // While playing a launch waits for the loop end: the cell is queued and blinks.
  h.Render();
  h.Tap(kPlay);
  h.Tap(Grid(1));
  auto ui = sequencer.BuildUiState(1);
  assert(ui.tracks[1].queuedPattern == 0 && h.State().hud.label == "T2 NEXT");
  groove::LedFrame frame{};
  app.Ui().ComputeLeds(frame);
  assert(frame[grid(1)].level == kFull && frame[grid(1)].effect == groove::LedEffect::Blink);
  h.Render();
  // Fn + ENTER captures what plays (or is queued) into the SONG as a new row.
  const std::size_t rows = sequencer.SongLength();
  h.Chord(kFn, kEnter);
  assert(sequencer.SongLength() == rows + 1 && sequencer.SongRowAt(rows).patterns[1] == 0);
  assert(h.State().hud.label == "CAPTURED");
  h.Tap(kPlay);
  // Launching from the session leaves SONG mode.
  sequencer.SetSongMode(true);
  h.Tap(Grid(patterns * 4 + 1));
  assert(!sequencer.SongMode());
  // ENTER launches the cursor's row on every track that has that pattern.
  h.Tap(kEnter);
  assert(sequencer.ActivePattern(1) == patterns && h.State().hud.label == "ROW");
  // SUB and ADD walk down and up the patterns and scroll the grid.
  assert(app.Ui().AdjustKeysMean() == groove::AdjustMeaning::List);
  for (int press = 0; press < 8; ++press) h.Tap(kSub);
  assert(h.State().sessionPattern == std::min<std::size_t>(patterns + 8, 15) && h.State().sessionScroll + 3 == h.State().sessionPattern);
  for (int press = 0; press < 16; ++press) h.Tap(kAdd);
  assert(h.State().sessionPattern == 0 && h.State().sessionScroll == 0);
  h.Render();
}

// Every FN menu item says what it did (toast, HUD or a pop-up), and the tile under the key lights.
void TestMenuFeedback(Harness& h)
{
  auto& app = h.App();
  GoToScene(h, 2);
  h.Chord(kTab, Grid(1));
  while (app.Ui().CurrentPage() != 0) h.Tap(kCtrl);
  if (app.Sequencer().Playing()) h.Tap(kPlay);
  const auto feedback = [&](std::size_t index) {
    const std::uint32_t toast = h.State().toast.until, hud = h.State().hud.at;
    h.Down(kFn);
    h.Wait(400);  // the menu shows once FN has been held a moment
    assert(app.Ui().FnOverlay());
    h.Down(Grid(index));
    assert(h.State().menuPress.down && h.State().menuPress.index == index);
    h.Up(Grid(index));
    const bool popup = h.Modal() != nullptr;
    h.Up(kFn);
    assert(!h.State().menuPress.down);
    assert(popup || h.State().toast.until != toast || h.State().hud.at != hud);
    while (h.Modal() != nullptr) h.Tap(kEsc);
    h.Wait(1200);
  };
  const groove::MenuItems menu = app.Ui().Menu();
  for (std::size_t index = 0; index < groove::kGridCount; ++index)
  {
    const groove::MenuAction action = menu[index].action;
    // SETUP leaves the scene; REC arms recording; the rest stays on the page.
    if (action == groove::MenuAction::None || action == groove::MenuAction::Setup || action == groove::MenuAction::Record ||
        action == groove::MenuAction::Save)
      continue;
    feedback(index);
  }
  // An item that does not apply here says so.
  for (std::size_t index = 0; index < groove::kGridCount; ++index)
    if (menu[index].action != groove::MenuAction::None && !app.Ui().Menu()[index].available)
    {
      h.Chord(kFn, Grid(index));
      assert(h.State().toast.text.find("NOT HERE") != std::string::npos);
      break;
    }
  h.Render();
}

void TestSelectMode(Harness& h)
{
  // ROLL with SELECT ONE (the default): a GRID tap walks to that one step, its notes are full on the
  // pads, and the same step again lets go so the pads play.
  GoToScene(h, 2);
  while (h.App().Ui().CurrentPage() != 0) h.Tap(kCtrl);
  h.Chord(kTab, Grid(1));
  assert(!h.State().keyboard.multiSelect);
  // Steps 1 and 5 hold just the C at the bottom left.
  const Key c{3, 1};
  int noteC = 0;
  assert(groove::PadNote(h.State().keyboard, h.App().EngineKind(h.State().track), c.row, c.column, noteC));
  const std::uint8_t cNote = static_cast<std::uint8_t>(noteC);
  for (const std::size_t step : {std::size_t{0}, std::size_t{4}})
    assert(h.App().Sequencer().SetStepNotes(h.State().track, step, &cNote, 1, 110));
  h.Tap(Grid(0));
  assert(h.Seq().steps[0].held && h.Seq().heldStepCount == 1);
  h.Tap(Grid(4));
  assert(!h.Seq().steps[0].held && h.Seq().steps[4].held && h.Seq().heldStepCount == 1);
  std::array<std::uint8_t, groove::kKeyCount> leds{};
  h.App().Ui().ComputeLeds(leds);
  assert(leds[groove::KeyIndex(c.row, c.column)] == groove::kLedFull && leds[groove::KeyIndex(2, 2)] == groove::kLedHalf);
  assert(leds[groove::KeyIndex(Grid(4).row, Grid(4).column)] == groove::kLedFull);
  h.Tap(Grid(4));
  assert(!h.Seq().hasHeldSteps);

  // Playing with nothing selected: the notes of the step under the playhead light up (playback
  // starts on step 1, which holds C).
  h.Tap(kPlay);
  h.Wait(20);
  assert(h.Seq().playing && h.Seq().currentStep == 0);
  h.App().Ui().ComputeLeds(leds);
  assert(leds[groove::KeyIndex(c.row, c.column)] == groove::kLedFull && leds[groove::KeyIndex(2, 2)] == groove::kLedHalf);
  h.Tap(kPlay);
  h.Wait(100);
  h.App().Ui().ComputeLeds(leds);
  assert(leds[groove::KeyIndex(c.row, c.column)] == groove::kLedHalf);

  // SELECT MULTI (FN panel, KEYS page): taps add steps to the selection.
  auto settings = h.App().Ui().Settings();
  settings.keyboard.multiSelect = true;
  h.App().Ui().Restore(settings, h.State().projectName, h.State().dirty);
  h.Tap(Grid(0));
  h.Tap(Grid(4));
  assert(h.Seq().steps[0].held && h.Seq().steps[4].held && h.Seq().heldStepCount == 2);
  h.App().Sequencer().ClearSelectedSteps();
  settings.keyboard.multiSelect = false;
  h.App().Ui().Restore(settings, h.State().projectName, h.State().dirty);
}

// Everything the pads play is kept: a long phrase is one take, and old takes make room for new notes.
void TestLongCapture(Harness& h)
{
  auto& ui = h.App().Ui();
  h.Wait(groove::kCaptureGapMs + 100);
  const Key pad{2, 2};
  h.Tap(pad);
  const std::uint32_t firstTake = ui.TakeAt(0)->id;
  for (int note = 1; note < 100; ++note) h.Tap(pad);
  assert(ui.TakeAt(0)->id == firstTake && ui.TakeAt(0)->count == 100);
  h.Render();
  // A new take after a pause; the old one stays until its notes are needed.
  h.Wait(groove::kCaptureGapMs + 100);
  for (std::size_t note = 0; note < 250; ++note) h.Tap(pad);
  assert(ui.TakeAt(0)->id == firstTake + 1 && ui.TakeAt(0)->count == 250 && ui.TakeAt(1)->id == firstTake && ui.TakeAt(1)->count == 100);
  h.Wait(groove::kCaptureGapMs + 100);
  for (std::size_t note = 0; note < 100; ++note) h.Tap(pad);
  assert(ui.TakeAt(0)->id == firstTake + 2 && ui.TakeAt(1)->id == firstTake + 1 && ui.TakeAt(1)->count == 250);
  assert(ui.TakeAt(2) == nullptr || ui.TakeAt(2)->id != firstTake);  // the 100 notes of the first gave way
  // The panel shows and places every bar of the long take.
  h.Hold(kEnter, 500);
  h.Render();
  h.Up(kEnter);
  h.Tap(kEsc);
}

// A long pattern on the GRID: FN flips its pages and sets its length.
void TestLongPatternUi(Harness& h)
{
  auto& sequencer = h.App().Sequencer();
  GoToScene(h, 2);
  while (h.App().Ui().CurrentPage() != 0) h.Tap(kCtrl);
  const std::size_t track = h.State().track;
  const std::size_t length = sequencer.TrackStepCount(track);
  // The menu's row 2: PAGE - / PAGE + / LEN - / LEN +. A big step lands on a page boundary.
  sequencer.SetTrackStepCount(track, 16);
  h.Chord(kFn, Grid(11));
  assert(sequencer.TrackStepCount(track) == 32 && h.Seq().viewPageCount == 2);
  h.Chord(kFn, Grid(11));
  assert(sequencer.TrackStepCount(track) == 48 && h.Seq().viewPageCount == 3);
  h.Chord(kFn, Grid(10));
  assert(sequencer.TrackStepCount(track) == 32);
  h.Chord(kFn, Grid(9));
  assert(h.Seq().viewPage == 1 && h.Seq().firstViewStep == 16);
  h.Render();
  // ADD / SUB flip the pages (the keys show sideways arrows); FN + ADD / SUB go through the patterns.
  assert(h.App().Ui().AdjustKeysMean() == groove::AdjustMeaning::Sideways);
  h.Tap(kSub);
  assert(h.Seq().viewPage == 0 && h.State().hud.label == "PAGE");
  h.Tap(kAdd);
  assert(h.Seq().viewPage == 1);
  const std::size_t pattern = sequencer.ActivePattern(track);
  const std::size_t patterns = sequencer.PatternCount(track);
  h.Chord(kFn, kAdd);
  assert(sequencer.ActivePattern(track) == (pattern + 1) % patterns && h.State().hud.label.rfind("PATTERN", 0) == 0);
  h.Chord(kFn, kSub);
  assert(sequencer.ActivePattern(track) == pattern && h.State().hud.value == "P" + std::string(pattern + 1 < 10 ? "0" : "") + std::to_string(pattern + 1));
  // Another pattern shows its first page; back on the second.
  if (h.Seq().viewPage != 1) h.Tap(kAdd);
  assert(h.Seq().viewPage == 1);
  h.Render();
  // The GRID edits the page it shows: its third cell is step 19 here.
  h.Tap(Grid(2));
  auto ui = h.Seq();
  assert(ui.hasHeldSteps && ui.primaryHeldStep == 18 && ui.steps[2].held);
  h.Render();
  // Arrow keys walk off the page and take the GRID along.
  h.Tap(Grid(2));
  assert(!h.Seq().hasHeldSteps);
  // Hold a pad, tap a step: it lands on this page.
  const std::size_t before = h.Seq().tracks[track].populatedStepCount;
  h.Down(Key{2, 2});
  h.Tap(Grid(4));
  h.Up(Key{2, 2});
  assert(sequencer.StepHasEvents(20) && h.Seq().tracks[track].populatedStepCount == before + 1);
  h.Render();
  sequencer.ClearStep(20);
  // STEP view: the same keys on the same page.
  h.Chord(kFn, Grid(5));
  assert(h.App().Ui().View() == groove::SeqView::Step);
  h.Tap(Grid(1));
  assert(h.Seq().primaryHeldStep == 17 && h.Seq().viewPage == 1);
  h.Render();
  // ADD / SUB flip the page once nothing is selected.
  h.Tap(kEsc);
  h.Tap(kSub);
  assert(h.Seq().viewPage == 0 && !h.Seq().hasHeldSteps);
  h.Chord(kFn, Grid(5));
  // The longest pattern is 256 steps; TRACK steps by one, FN by a page.
  sequencer.SetTrackStepCount(track, 255);
  h.Chord(kFn, Grid(11));
  assert(sequencer.TrackStepCount(track) == 256 && h.Seq().viewPageCount == 16);
  h.Chord(kFn, Grid(11));
  assert(sequencer.TrackStepCount(track) == 256);
  h.Render();
  h.Tap(kCtrl);
  h.Tap(kCtrl);
  assert(h.App().Ui().CurrentPage() == 2);
  h.Tap(Grid(1));
  h.Render();
  h.Tap(kSub);
  assert(sequencer.TrackStepCount(track) == 255);
  h.Down(kFn);
  h.Tap(kSub);
  h.Up(kFn);
  assert(sequencer.TrackStepCount(track) == 240);
  sequencer.SetTrackStepCount(track, length);
  while (h.App().Ui().CurrentPage() != 0) h.Tap(kCtrl);
  GoToScene(h, 1);
}

// The whole sample recording flow through the keys, with a microphone that delivers in real time.
void TestSampleRecording(Harness& h)
{
  namespace sim = CardenzaSim::Input;
  auto& sampler = h.App().Sampler();
  const std::size_t track = 3;
  sim::SetMicrophonePresent(true);
  h.Chord(kTab, Grid(track));
  assert(h.App().EngineKind(track) == synth::SynthEngineKind::Sampler);
  GoToInstrumentPage(h, "SAMPLE");
  auto openRecord = [&] {
    h.Tap(Grid(1));
    assert(h.Modal() != nullptr && h.Modal()->kind == ModalKind::SampleRecord);
    h.Render();
  };
  auto feed = [&](int frames) {
    for (int index = 0; index < frames; ++index)
    {
      sim::FeedMicrophone(735);
      h.Frames();
      h.Render();
    }
  };

  // Opening the screen starts nothing; ENTER records, ENTER stops.
  openRecord();
  assert(!sampler.BuildUiState(track).recording);
  h.Tap(kEnter);
  assert(sampler.BuildUiState(track).recording);
  feed(20);
  assert(sampler.BuildUiState(track).frameCount == 20 * 735);
  h.Tap(kEnter);
  assert(!sampler.BuildUiState(track).recording && sampler.BuildUiState(track).hasDraft);
  h.Render();
  // The take plays, and ESC + ENTER discards it.
  h.Tap(kPlay);
  assert(sampler.BuildUiState(track).previewing);
  h.Wait(100);
  h.Render();
  h.Tap(kEsc);
  assert(h.Modal() != nullptr && h.Modal()->kind == ModalKind::Confirm);
  h.Render();
  h.Tap(kEnter);
  assert(h.Modal() == nullptr && !sampler.BuildUiState(track).hasDraft);

  // ESC cancels a running recording; the screen stays open and can start again.
  openRecord();
  h.Tap(kEnter);
  feed(5);
  h.Tap(kEsc);
  assert(!sampler.BuildUiState(track).recording && !sampler.BuildUiState(track).hasDraft);
  h.Render();
  h.Tap(kEnter);
  feed(5);
  // A recording that reaches its limit stops by itself and SAVE keeps it as a file.
  const std::uint32_t limit = sampler.BuildUiState(track).recordMaxFrameCount;
  assert(limit != 0);
  sim::FeedMicrophone(limit);
  h.Frames(2);
  h.Render();
  assert(!sampler.BuildUiState(track).recording && sampler.BuildUiState(track).hasDraft);
  h.Tap(Grid(3));
  h.Frames(3);
  h.Render();
  assert(h.Modal() == nullptr && sampler.BuildUiState(track).hasSample && !sampler.BuildUiState(track).hasDraft);

  // Leaving the screen while recording never leaves the microphone running.
  openRecord();
  h.Tap(kEnter);
  assert(sampler.BuildUiState(track).recording);
  h.Chord(kEsc, Grid(2));
  h.Wait(100);
  h.Render();
  assert(!sampler.BuildUiState(track).recording);
  GoToInstrumentPage(h, "SAMPLE");

  sim::SetMicrophonePresent(false);
  // No microphone: an error on the screen, not a dead end.
  openRecord();
  h.Tap(kEnter);
  h.Render();
  assert(!sampler.BuildUiState(track).recording);
  h.Tap(kEsc);
  assert(h.Modal() == nullptr);
  GoToScene(h, 1);
  h.Chord(kTab, Grid(synth::kDefaultActiveTrack));
}

// A track with the drum engine plays the drum machine on the note page: the pads choose a lane, the
// GRID is that lane's 16 steps, a tap adds at once, a tap on a hit removes it when let go, and a hit
// held with ADD / SUB changes its velocity.
void TestDrumMachine(Harness& h)
{
  h.App().Ui().UsePianoRollOnDrums(false);
  GoToSetupPage(h, groove::kSetupProject);
  h.Tap(Grid(2));  // NEW
  if (h.Modal() != nullptr) h.Tap(kEnter);
  h.Chord(kTab, Grid(0));
  GoToScene(h, 2);
  while (h.App().Ui().CurrentPage() != 0) h.Tap(kCtrl);
  assert(h.App().Ui().DrumLayout() && h.State().track == 0);
  auto& sequencer = h.App().Sequencer();
  std::array<synth::SynthSequencer::StepNoteView, 8> notes{};
  const auto hitsOn = [&](std::size_t step) { return sequencer.StepNoteEvents(0, step, notes); };
  const std::uint8_t drumA = groove::kDrumNotes[kDrumPadA.column - groove::kPadFirstColumn];
  const std::uint8_t drumB = groove::kDrumNotes[kDrumPadB.column - groove::kPadFirstColumn];
  // Pads choose the drum the GRID view shows.
  const std::size_t firstLane = h.App().Ui().DrumLane();
  h.Tap(kDrumPadA);
  assert(h.App().Ui().DrumLane() != firstLane);
  // The step keys are the note page's. A tap selects a step and adds nothing.
  assert(hitsOn(5) == 0);
  h.Tap(Grid(5));
  assert(hitsOn(5) == 0 && h.Seq().steps[5].held);
  // Pads on the selected step: each drum goes in (as hard as its pad's row) or comes out.
  h.Tap(kDrumPadA);
  h.Tap(kDrumPadB);
  assert(hitsOn(5) == 2 && notes[0].velocity == groove::kDrumRowVelocity[kDrumPadA.row]);
  h.Tap(kDrumPadA);
  assert(hitsOn(5) == 1 && notes[0].note == drumB);
  h.Tap(Grid(5));  // the step selected alone: let go
  assert(!h.Seq().hasHeldSteps && hitsOn(5) == 1);
  // Hold a drum pad, tap steps: it goes in there, and comes out of a step that has it.
  h.Down(kDrumPadA);
  h.Tap(Grid(6));
  h.Tap(Grid(7));
  h.Up(kDrumPadA);
  assert(hitsOn(6) == 1 && hitsOn(7) == 1 && notes[0].note == drumA && !h.Seq().hasHeldSteps);
  h.Down(kDrumPadA);
  h.Tap(Grid(7));
  h.Up(kDrumPadA);
  assert(hitsOn(7) == 0);
  // A held step is selected only while held; nothing is edited by holding it long.
  h.Down(Grid(6));
  h.Wait(500);
  assert(h.Seq().steps[6].held && hitsOn(6) == 1);
  // ADD / SUB: the NOTE lane does not move a drum off its pad; OK goes on to LEN, VEL, TIME.
  h.Tap(kAdd);
  assert(hitsOn(6) == 1 && notes[0].note == drumA && h.State().hud.value == "PADS PICK DRUMS");
  h.Tap(kEnter);
  h.Tap(kEnter);
  assert(h.Seq().editLane == synth::SequencerEditLane::Velocity);
  const std::uint8_t before = notes[0].velocity;
  h.Tap(kSub);
  assert(hitsOn(6) == 1 && notes[0].velocity == before - 1);
  h.Up(Grid(6));
  assert(!h.Seq().hasHeldSteps && hitsOn(6) == 1);
  // The drum machine is the STEP view; OK switches drum tracks to the piano roll and back.
  h.Render();
  h.Tap(kEnter);
  assert(h.App().Ui().View() == groove::SeqView::Roll && !h.App().Ui().DrumLayout() && h.App().Ui().PageTab(Scene::Sequence, 0) == "ROLL");
  h.Down(kDrumPadA);
  h.Render();
  h.Up(kDrumPadA);
  h.Tap(kEnter);
  assert(h.App().Ui().DrumLayout() && h.App().Ui().PageTab(Scene::Sequence, 0) == "DRUMS");
  GoToScene(h, 1);
}

// Every key, in any order, on every screen: once all keys are up, the transport stopped and the
// arpeggiators off, nothing may still hold a note, and the safety net must not have had to step in.
void TestNoStuckNotes(Harness& h)
{
  auto& app = h.App();
  const char* seed = std::getenv("CARDENZA_FUZZ_SEED");
  std::mt19937 random(seed != nullptr ? static_cast<std::uint32_t>(std::atoi(seed)) : 20261101u);
  const std::uint32_t orphansBefore = app.EndedOrphanNotes();
  const std::uint32_t voicesBefore = synth::audio::ReleasedOrphanVoiceCount();
  std::vector<Key> down;
  std::vector<std::string> trace;
  std::array<std::int16_t, 256> block{};
  const auto render = [&](int blocks) { for (int index = 0; index < blocks; ++index) synth::audio::RenderFrames(block.data(), block.size()); };
  const auto press = [&](Key key, bool pressed) {
    trace.push_back(std::string(pressed ? "down " : "up ") + std::to_string(key.row) + "," + std::to_string(key.column));
    if (trace.size() > 60) trace.erase(trace.begin());
    pressed ? h.Down(key) : h.Up(key);
  };
  const auto pick = [&]() -> Key {
    const std::uint32_t kind = random() % 100;
    if (kind < 55) return Key{random() % 4, 1 + random() % 8};   // Pads.
    if (kind < 70) return Key{random() % 4, 0};                  // ESC, TAB, FN, CTRL.
    if (kind < 90) return Key{random() % 4, 9 + random() % 4};   // GRID.
    return Key{random() % 4, 13};                                // ADD, SUB, ENTER, PLAY.
  };
  for (int round = 0; round < 60; ++round)
  {
    for (int event = 0; event < 300; ++event)
    {
      // The HAL reports at most four keys at a time.
      if (down.size() >= 4 || (!down.empty() && random() % 2 == 0))
      {
        const std::size_t index = random() % down.size();
        press(down[index], false);
        down.erase(down.begin() + static_cast<std::ptrdiff_t>(index));
      }
      else
      {
        const Key key = pick();
        const bool held = std::any_of(down.begin(), down.end(), [&](const Key& other) { return other.row == key.row && other.column == key.column; });
        if (held) continue;
        press(key, true);
        down.push_back(key);
      }
      for (std::uint32_t frame = 1 + random() % 3; frame > 0; --frame)
      {
        h.Frames(1);
        render(1);
      }
      if (app.EndedOrphanNotes() != orphansBefore || synth::audio::ReleasedOrphanVoiceCount() != voicesBefore)
      {
        std::cerr << "safety net fired: round " << round << " event " << event << " scene " << static_cast<int>(h.State().scene) << " track " << h.State().track << " orphans " << app.EndedOrphanNotes() << " voices " << synth::audio::ReleasedOrphanVoiceCount() << "\n";
        for (std::size_t track = 0; track < synth::kSynthTrackCount; ++track) std::cerr << "kind " << static_cast<int>(app.EngineKind(track)) << "\n";
        for (const std::string& line : trace) std::cerr << line << " ";
        std::cerr << "\n";
        std::abort();
      }
    }
    for (const Key& key : down) press(key, false);
    down.clear();
    for (int frame = 0; frame < 8; ++frame) { h.Frames(1); render(1); }
    // Close whatever is open, let every screen settle.
    for (int index = 0; index < 4 && h.Modal() != nullptr; ++index) { press(kEsc, true); press(kEsc, false); for (int frame = 0; frame < 4; ++frame) { h.Frames(1); render(1); } }
    for (int frame = 0; frame < 8; ++frame) { h.Frames(1); render(1); }
    // Stopped, with no arpeggiator running: no key is down, so nothing has a reason to sound.
    if (!app.Sequencer().Playing())
    {
      for (std::size_t track = 0; track < synth::kSynthTrackCount; ++track)
      {
        const synth::audio::NoteSet held = synth::audio::HeldNotes(track);
        if (app.Sequencer().ArpActive(track) || std::all_of(held.begin(), held.end(), [](std::uint32_t word) { return word == 0; })) continue;
        std::cerr << "a note sounds with no key down: round " << round << " track " << track << "\n";
        for (const std::string& line : trace) std::cerr << line << " ";
        std::cerr << "\n";
        std::abort();
      }
    }
    app.Sequencer().Stop();
    for (std::size_t track = 0; track < synth::kSynthTrackCount; ++track)
    {
      synth::ArpSettings arp = app.Sequencer().GetArpSettings(track);
      arp.mode = synth::ArpMode::Off;
      app.Sequencer().SetArpSettings(track, arp);
    }
    for (int frame = 0; frame < 16; ++frame) { h.Frames(1); render(1); }
    // With every key up, nothing may still behave as if one were down.
    const auto& state = h.State();
    bool keysClear = !app.Ui().EscOverlay() && !app.Ui().TabOverlay() && !app.Ui().FnOverlay() && !app.Ui().CtrlOverlay() && !state.repeat.down;
    for (const auto& left : state.left) keysClear = keysClear && !left.down;
    for (const bool grid : state.gridDown) keysClear = keysClear && !grid;
    for (const auto note : state.padNote) keysClear = keysClear && note == groove::kNoNote;
    if (!keysClear)
    {
      std::cerr << "a key still counts as down: round " << round << " esc " << state.left[0].down << " tab " << state.left[1].down << " fn " << state.left[2].down << " ctrl " << state.left[3].down << " repeat " << state.repeat.down << "\n";
      for (const std::string& line : trace) std::cerr << line << "\n";
      std::abort();
    }
    for (std::size_t track = 0; track < synth::kSynthTrackCount; ++track)
    {
      const synth::audio::NoteSet held = synth::audio::HeldNotes(track);
      const bool silent = std::all_of(held.begin(), held.end(), [](std::uint32_t word) { return word == 0; });
      if (silent && app.EndedOrphanNotes() == orphansBefore && synth::audio::ReleasedOrphanVoiceCount() == voicesBefore) continue;
      std::cerr << "stuck note: round " << round << " track " << track << " silent " << silent << " orphans " << app.EndedOrphanNotes() << " voices " << synth::audio::ReleasedOrphanVoiceCount() << "\n";
      for (const std::string& line : trace) std::cerr << line << "\n";
      std::abort();
    }
  }
  GoToScene(h, 1);
}

// ADD and SUB's icons say what they do, and a list goes the way its arrows point.
void TestAdjustIcons(Harness& h)
{
  auto& app = h.App();
  using Meaning = groove::AdjustMeaning;
  app.Ui().UsePianoRollOnDrums(false);
  GoToScene(h, 1);
  h.Chord(kTab, Grid(1));
  while (app.Ui().CurrentInstrumentPage()->graphic != groove::PageGraphic::Voice) h.Tap(kCtrl);
  // The instrument's first page walks the presets: up goes up the list, as in the browser. (The
  // earlier tests left edits on T2, which would make the first step ask: they go first.)
  assert(app.Ui().AdjustKeysMean() == Meaning::List);
  assert(app.Engine(1)->LoadPreset(app.Engine(1)->CurrentPresetId()) && !app.Engine(1)->HasEdits());
  h.Tap(kSub);
  h.Tap(kSub);
  const std::string lower = app.Engine(1)->CurrentPresetName();
  h.Tap(kAdd);
  const std::string upper = app.Engine(1)->CurrentPresetName();
  assert(lower != upper);
  h.Tap(Grid(1));
  assert(app.Ui().AdjustKeysMean() == Meaning::List);
  const std::size_t cursor = h.Modal()->cursor;
  h.Tap(kAdd);
  assert(h.Modal()->cursor + 1 == cursor);
  h.Tap(kEsc);
  // Parameters are values; an engine choice is a row.
  h.Tap(kCtrl);
  assert(app.Ui().AdjustKeysMean() == Meaning::Value);
  h.Tap(kCtrl);
  h.Tap(kCtrl);
  assert(app.Ui().AdjustKeysMean() == Meaning::Value);
  while (app.Ui().CurrentInstrumentPage()->graphic != groove::PageGraphic::Voice) h.Tap(kCtrl);
  h.Tap(Grid(0));
  assert(h.Modal() != nullptr && h.Modal()->kind == ModalKind::EnginePicker && app.Ui().AdjustKeysMean() == Meaning::Sideways);
  h.Tap(kEsc);
  // Holding CTRL or TAB, they step the pages or the tracks.
  h.Down(kCtrl);
  assert(app.Ui().AdjustKeysMean() == Meaning::Sideways);
  h.Tap(kAdd);
  h.Up(kCtrl);
  h.Down(kTab);
  assert(app.Ui().AdjustKeysMean() == Meaning::Sideways);
  h.Tap(kAdd);
  h.Up(kTab);
  h.Chord(kTab, Grid(1));
  // SEQ note page: sideways (pages) with nothing selected, in both views; a value with a step
  // selected (its edit lane).
  GoToScene(h, 2);
  while (app.Ui().CurrentPage() != 0) h.Tap(kCtrl);
  assert(app.Ui().AdjustKeysMean() == Meaning::Sideways);
  h.Tap(kEnter);
  h.Wait(50);
  assert(app.Ui().AdjustKeysMean() == Meaning::Sideways);
  if (app.Ui().View() == groove::SeqView::Step) { h.Tap(kEnter); h.Wait(50); }
  h.Tap(Grid(0));
  assert(app.Ui().AdjustKeysMean() == Meaning::Value);
  h.Tap(kEsc);
  // TAPE: the session is a list, the rest values.
  GoToScene(h, 3);
  while (app.Ui().CurrentPage() != groove::kTapeSession) h.Tap(kCtrl);
  assert(app.Ui().AdjustKeysMean() == Meaning::List);
  const std::size_t before = h.State().sessionPattern;
  h.Tap(kSub);
  assert(h.State().sessionPattern == before + 1);
  h.Tap(kAdd);
  assert(h.State().sessionPattern == before);
  while (app.Ui().CurrentPage() != groove::kTapeMix) h.Tap(kCtrl);
  assert(app.Ui().AdjustKeysMean() == Meaning::Value);
  // SETUP: the file list is a list; a name field erases; a confirmation has no use for them.
  GoToSetupPage(h, groove::kSetupFiles);
  assert(app.Ui().AdjustKeysMean() == Meaning::List);
  GoToSetupPage(h, groove::kSetupProject);
  assert(app.Ui().AdjustKeysMean() == Meaning::Value);
  h.Chord(kFn, Grid(2));
  assert(h.Modal() != nullptr && h.Modal()->kind == ModalKind::TextEntry && app.Ui().AdjustKeysMean() == Meaning::Erase);
  h.Tap(kEsc);
  if (h.Modal() != nullptr) h.Tap(kEsc);
  GoToScene(h, 1);
}

// On a drum track a column of pads is a drum and its rows how hard: the top row hardest. The column
// shows it as a bar, and a hit runs along it, further the harder it was.
void TestDrumPads(Harness& h)
{
  auto& app = h.App();
  app.Ui().UsePianoRollOnDrums(false);
  GoToScene(h, 1);
  h.Chord(kTab, Grid(0));
  assert(app.EngineKind(0) == synth::SynthEngineKind::Drums);
  std::array<std::uint8_t, groove::kKeyCount> leds{};
  const auto logical = [&](Key key) {
    app.Ui().ComputeLeds(leds);
    return leds[groove::KeyIndex(key.row, key.column)];
  };
  // Every row of a column plays the same drum, each with its own velocity.
  for (std::size_t row = 0; row < groove::kRows; ++row)
  {
    const Key pad{row, 2};
    h.Down(pad);
    const std::size_t index = row * groove::kPadColumns + 1;
    assert(h.State().padNote[index] == groove::kDrumNotes[1] && h.State().padVelocity[index] == groove::kDrumRowVelocity[row]);
    // A bar from the bottom up to this row; the other columns are not touched.
    for (std::size_t other = 0; other < groove::kRows; ++other)
    {
      assert(logical(Key{other, 2}) == (other >= row ? groove::kLedFull : groove::kLedHalf));
      assert(logical(Key{other, 3}) == groove::kLedHalf);
    }
    h.Up(pad);
    assert(logical(pad) == groove::kLedHalf);
  }
  // A melodic track goes on playing at the keyboard's velocity, on every row.
  h.Chord(kTab, Grid(1));
  h.Down(Key{0, 1});
  assert(h.State().padVelocity[0] == h.State().keyboard.velocity);
  h.Up(Key{0, 1});
  h.Chord(kTab, Grid(0));
  h.Wait(1000);

  // At rest every drum pad is half, no brighter than a melodic pad; a hard hit strikes at once and
  // runs down the whole column, a soft one stays near its key.
  for (std::size_t row = 0; row < groove::kRows; ++row) assert(Led(h, Key{row, 5}) == groove::kLedHalf);
  const int restTop = groove::kLedHalf;
  const int restBottom = groove::kLedHalf;
  h.Down(Key{0, 5});
  assert(Led(h, Key{0, 5}) >= 12 && Led(h, Key{3, 5}) == restBottom);  // Not there yet.
  h.Up(Key{0, 5});
  int bottom = 0;
  for (int frame = 0; frame < 12; ++frame) { h.Frames(1); bottom = std::max(bottom, Led(h, Key{3, 5})); }
  assert(bottom > restBottom + 2);
  h.Wait(1000);
  assert(Led(h, Key{0, 5}) == restTop && Led(h, Key{3, 5}) == restBottom);
  // Held, the bar stays: from the bottom up to the row played.
  h.Down(Key{1, 5});
  h.Wait(300);
  assert(Led(h, Key{1, 5}) == groove::kLedFull && Led(h, Key{3, 5}) == groove::kLedFull && Led(h, Key{0, 5}) == restTop);
  h.Up(Key{1, 5});
  h.Wait(1000);
  h.Down(Key{3, 5});
  assert(Led(h, Key{3, 5}) >= 12);
  h.Up(Key{3, 5});
  int top = 0;
  for (int frame = 0; frame < 12; ++frame) { h.Frames(1); top = std::max(top, Led(h, Key{0, 5})); }
  assert(top == restTop);
  h.Wait(1000);

  // On the drum page a pad chooses the drum and how hard: held, the steps tapped take it at its row's
  // velocity.
  GoToScene(h, 2);
  while (app.Ui().CurrentPage() != 0) h.Tap(kCtrl);
  auto& sequencer = app.Sequencer();
  std::array<synth::SynthSequencer::StepNoteView, 8> notes{};
  std::size_t step = 0;
  while (sequencer.StepNoteEvents(0, step, notes) != 0 && step < 15) ++step;
  assert(sequencer.StepNoteEvents(0, step, notes) == 0);
  h.Tap(Key{1, 4});
  assert(app.Ui().DrumLane() == 3 && h.State().drumVelocity[0] == groove::kDrumRowVelocity[1]);
  assert(logical(Key{1, 4}) == groove::kLedFull && logical(Key{3, 4}) == groove::kLedFull && logical(Key{0, 4}) == groove::kLedHalf);
  h.Down(Key{1, 4});
  h.Tap(Grid(step));
  h.Up(Key{1, 4});
  assert(sequencer.StepNoteEvents(0, step, notes) == 1 && notes[0].note == groove::kDrumNotes[3] && notes[0].velocity == groove::kDrumRowVelocity[1]);
  // A tapped step stays selected: its pad takes the drum out, a pad of another row puts it back as
  // hard as that row.
  h.Tap(Grid(step));
  assert(h.Seq().steps[step].held && sequencer.StepNoteEvents(0, step, notes) == 1);
  h.Tap(Key{0, 4});
  assert(sequencer.StepNoteEvents(0, step, notes) == 0);
  h.Tap(Key{0, 4});
  assert(sequencer.StepNoteEvents(0, step, notes) == 1 && notes[0].velocity == groove::kDrumRowVelocity[0]);
  // Its values as on the note page: OK picks the lane (NOTE, LEN, VEL, TIME), ADD / SUB change it.
  h.Tap(kEnter);
  h.Tap(kEnter);
  h.Tap(kSub);
  assert(sequencer.StepNoteEvents(0, step, notes) == 1 && notes[0].velocity == groove::kDrumRowVelocity[0] - 1 && notes[0].microstep == 0);
  h.Tap(kEnter);
  h.Tap(kAdd);
  assert(sequencer.StepNoteEvents(0, step, notes) == 1 && notes[0].microstep == 1);
  h.Render();
  h.Tap(Grid(step));
  assert(!h.Seq().hasHeldSteps && sequencer.StepNoteEvents(0, step, notes) == 1);
  assert(sequencer.RemoveNote(0, sequencer.ActivePattern(0), step, groove::kDrumNotes[3]));
  // Moving a note that is not there, or out of the step, does nothing.
  assert(sequencer.InsertNote(0, sequencer.ActivePattern(0), step, 2, groove::kDrumNotes[3], 100, 24));
  assert(!sequencer.MoveNote(0, sequencer.ActivePattern(0), step, groove::kDrumNotes[3], 0, 1));
  assert(!sequencer.MoveNote(0, sequencer.ActivePattern(0), step, groove::kDrumNotes[3], 2, synth::kSequencerMicrostepCount));
  assert(sequencer.MoveNote(0, sequencer.ActivePattern(0), step, groove::kDrumNotes[3], 2, 4));
  assert(sequencer.StepNoteEvents(0, step, notes) == 1 && notes[0].microstep == 4);
  assert(sequencer.RemoveNote(0, sequencer.ActivePattern(0), step, groove::kDrumNotes[3]));
  GoToScene(h, 1);
}

// Holding a parameter's GRID cell turns the pads into its values: lit where there is a value, the
// current one full, a press sets it. A tap on the cell still does what it did, and the pads play again.
void TestCellPicker(Harness& h)
{
  auto& app = h.App();
  h.App().Ui().UsePianoRollOnDrums(false);
  std::array<std::uint8_t, groove::kKeyCount> leds{};
  const auto padLed = [&](std::size_t pad) {
    app.Ui().ComputeLeds(leds);
    return leds[groove::KeyIndex(pad / groove::kPadColumns, groove::kPadFirstColumn + pad % groove::kPadColumns)];
  };
  const auto padKey = [](std::size_t pad) { return Key{pad / groove::kPadColumns, groove::kPadFirstColumn + pad % groove::kPadColumns}; };
  GoToScene(h, 1);
  h.Chord(kTab, Grid(1));
  // ARP page of T2: mode (a choice), rate, range (a number of four), gate (a long range), HOLD.
  while (std::string(app.Ui().CurrentInstrumentPage()->tab) != "ARP") h.Tap(kCtrl);
  const std::size_t modes = synth::kArpModeCount, rates = synth::kArpRateCount;
  // Not yet: a short press leaves the pads alone, and a pad pressed meanwhile plays.
  h.Down(Grid(1));
  assert(!app.Ui().CellPickerActive());
  h.Up(Grid(1));
  assert(!app.Ui().CellPickerActive());
  // Held: as many pads as values.
  h.Hold(Grid(1), 300);
  assert(app.Ui().CellPickerActive());
  std::string label, text;
  synth::SynthParameterInfo info;
  assert(app.Ui().CellParameter(1, label, info, text) && label == "RATE");
  for (std::size_t pad = 0; pad < rates; ++pad) assert(padLed(pad) == (pad == static_cast<std::size_t>(info.value) ? groove::kLedFull : groove::kLedHalf));
  assert(padLed(rates) == groove::kLedOff);
  // A press sets it, reports it and plays nothing; a press where nothing is lit does nothing.
  h.Tap(padKey(1));
  assert(app.Sequencer().GetArpSettings(1).rate == 1 && h.State().hud.label == "RATE" && h.State().padNote[0] == groove::kNoNote);
  assert(padLed(1) == groove::kLedFull && padLed(info.value) == groove::kLedHalf);
  h.Tap(padKey(rates));
  assert(app.Sequencer().GetArpSettings(1).rate == 1);
  h.Up(Grid(1));
  // Letting go: the pads are the keyboard again, and picking did not also step the value.
  assert(!app.Ui().CellPickerActive() && app.Sequencer().GetArpSettings(1).rate == 1);
  h.Down(padKey(16));
  assert(h.State().padNote[16] != groove::kNoNote);
  h.Up(padKey(16));
  // A tap on the focused cell still steps it; a long hold without a pick does not.
  h.Tap(Grid(1));
  assert(app.Sequencer().GetArpSettings(1).rate == 2);
  h.Hold(Grid(1), 300);
  h.Up(Grid(1));
  assert(app.Sequencer().GetArpSettings(1).rate == 2);
  // A long range spreads over all the pads, ends included.
  h.Hold(Grid(3), 300);
  h.Tap(padKey(30));  // An even range: 31 pads, so that the middle is on one.
  assert(app.Sequencer().GetArpSettings(1).gate == 100);
  h.Tap(padKey(0));
  assert(app.Sequencer().GetArpSettings(1).gate == 10);
  h.Up(Grid(3));
  (void)modes;
  // A left key takes over: the page map, not the values.
  h.Hold(Grid(1), 300);
  h.Hold(kCtrl, 300);
  assert(!app.Ui().CellPickerActive());
  h.Up(kCtrl);
  h.Up(Grid(1));
  while (std::string(app.Ui().CurrentInstrumentPage()->tab) != "ARP") h.Tap(kCtrl);
  // Cells with no parameter: nothing.
  h.Hold(Grid(15), 300);
  assert(!app.Ui().CellPickerActive());
  h.Up(Grid(15));

  // HOLD: one tap, the arp plays on after the keys let go; a new press starts a new set; off stops it.
  h.Tap(Grid(0));
  while (app.Sequencer().GetArpSettings(1).mode == synth::ArpMode::Off) h.Tap(Grid(0));
  assert(!app.Sequencer().GetArpSettings(1).latch);
  h.Tap(Grid(4));
  assert(app.Sequencer().GetArpSettings(1).latch);
  app.Ui().ComputeLeds(leds);
  assert(leds[groove::KeyIndex(Grid(4).row, Grid(4).column)] == groove::kLedFull);
  h.Tap(Key{2, 1});
  assert(app.Sequencer().ArpActive(1));
  h.Tap(Grid(4));
  assert(!app.Sequencer().GetArpSettings(1).latch && !app.Sequencer().ArpActive(1));
  synth::ArpSettings off = app.Sequencer().GetArpSettings(1);
  off.mode = synth::ArpMode::Off;
  app.Sequencer().SetArpSettings(1, off);

  // SETUP, the mixer and the effects offer their values too.
  GoToSetupPage(h, groove::kSetupKeys);
  h.Hold(Grid(1), 300);
  assert(app.Ui().CellPickerActive());
  h.Up(Grid(1));
  GoToSetupPage(h, groove::kSetupProject);
  h.Hold(Grid(5), 300);  // SWING
  assert(app.Ui().CellPickerActive());
  h.Up(Grid(5));
  h.Hold(Grid(7), 300);  // PLAY mode, an action rather than a value.
  assert(!app.Ui().CellPickerActive());
  h.Up(Grid(7));
  if (app.Sequencer().SongMode()) h.Tap(Grid(7));
  GoToScene(h, 3);
  while (app.Ui().CurrentPage() != groove::kTapeMix) h.Tap(kCtrl);
  h.Hold(Grid(2), 300);
  assert(app.Ui().CellPickerActive());
  h.Tap(padKey(30));
  assert(app.TrackLevel(2) == 100);
  h.Tap(padKey(15));
  assert(app.TrackLevel(2) == 50);
  h.Up(Grid(2));
  h.Hold(Grid(4 + 2), 300);  // Mute is a switch, no value to pick.
  assert(!app.Ui().CellPickerActive());
  h.Up(Grid(4 + 2));
  if (app.TrackMuted(2)) h.Tap(Grid(4 + 2));
  while (app.Ui().CurrentPage() != groove::kTapeSends) h.Tap(kCtrl);
  h.Hold(Grid(4 + 1), 300);  // T2's chorus modes.
  assert(app.Ui().CellPickerActive());
  h.Up(Grid(4 + 1));
  GoToScene(h, 1);
}

// A note held while anything else happens ends when its key does, whatever happened in between.
void TestHeldNotesEnd(Harness& h)
{
  std::array<std::int16_t, 256> block{};
  const auto settle = [&]() { for (int frame = 0; frame < 6; ++frame) { h.Frames(1); synth::audio::RenderFrames(block.data(), block.size()); } };
  const auto silent = [&]() {
    for (std::size_t track = 0; track < synth::kSynthTrackCount; ++track)
    {
      const synth::audio::NoteSet held = synth::audio::HeldNotes(track);
      if (!std::all_of(held.begin(), held.end(), [](std::uint32_t word) { return word == 0; })) return false;
    }
    return true;
  };
  const Key pad{3, 1};
  const auto holdThrough = [&](const char* what, const auto& action) {
    GoToScene(h, 1);
    h.Chord(kTab, Grid(1));
    settle();
    assert(silent());
    h.Down(pad);
    settle();
    assert(!silent());
    action();
    settle();
    h.Up(pad);
    settle();
    if (!silent())
    {
      std::cerr << "note left sounding after: " << what << "\n";
      std::abort();
    }
    while (h.Modal() != nullptr) h.Tap(kEsc);
  };
  holdThrough("a scene change", [&]() { h.Tap(kEsc); h.Tap(kEsc); });
  holdThrough("the scene map", [&]() { h.Chord(kEsc, Grid(1)); });
  holdThrough("a track change", [&]() { h.Tap(kTab); });
  holdThrough("the track map", [&]() { h.Chord(kTab, Grid(3)); });
  holdThrough("a page change", [&]() { h.Tap(kCtrl); });
  holdThrough("the FN menu", [&]() { h.Tap(kFn); h.Tap(kFn); });
  holdThrough("a preset change", [&]() { h.Tap(kAdd); h.Tap(kSub); });
  holdThrough("the preset browser", [&]() { h.Tap(Grid(1)); h.Tap(kAdd); h.Tap(kEnter); });
  holdThrough("an engine change", [&]() { h.Tap(Grid(0)); h.Tap(kAdd); h.Tap(kEnter); h.Tap(Grid(0)); h.Tap(kSub); h.Tap(kEnter); });
  holdThrough("transport", [&]() { h.Tap(kPlay); h.Wait(100); h.Tap(kPlay); });
  holdThrough("an octave change", [&]() {
    GoToSetupPage(h, groove::kSetupKeys);
    h.Tap(Grid(3));
    h.Tap(kAdd);
    h.Tap(kSub);
    h.Tap(kEsc);
  });
  holdThrough("a project change", [&]() {
    GoToSetupPage(h, groove::kSetupProject);
    h.Tap(Grid(2));
    if (h.Modal() != nullptr) h.Tap(kEnter);
    h.Tap(kEsc);
  });
  GoToScene(h, 1);
}

// DEMO-2 as a user's board saved it under the 0.1.0 release (format 6), unchanged.
constexpr const char* kReleaseProject = R"FIXTURE(CARDENZA_PROJECT 6
BUILD CardenzaSynth 1
UI 0 -1 4 0 1 15
MASTER 100
MIX 0 100 0 0
MIX 1 100 0 0
MIX 2 100 0 0
MIX 3 100 0 0
SYSTEM 180
FX 12 7 56 72 100 76 52 36 100 0 64 64 0
KEYS 0 110
PLAYFX 0 0 0 0 3 1 50 0
PLAYFX 1 0 0 0 3 1 50 0
PLAYFX 2 0 0 0 3 1 50 0
PLAYFX 3 0 0 0 3 1 50 0
TRACK 0 4 "drums.808" "drums.808" "808 Kit" ""
PARAM 16
"clap.level" 100
"closed_hat.level" 100
"cowbell.level" 100
"fx.chorus" 2
"fx.crush" 0
"fx.delay" 0
"fx.drive" 12
"fx.filter" 64
"fx.resonance" 20
"fx.reverb" 40
"kick.level" 127
"maraca.level" 100
"master.level" 100
"open_hat.level" 100
"snare.level" 100
"tom.level" 100
TRACK 1 2 "juno.a68" "juno.a68" "Synth Pad" ""
PARAM 29
"dco.lfo" 5
"dco.noise" 0
"dco.pulse" 1
"dco.pwm" 43
"dco.pwm_mode" 0
"dco.range" 1
"dco.saw" 0
"dco.sub" 37
"env.attack" 0
"env.decay" 65
"env.release" 70
"env.sustain" 22
"fx.chorus" 2
"fx.crush" 0
"fx.delay" 0
"fx.drive" 0
"fx.filter" 64
"fx.resonance" 20
"fx.reverb" 0
"lfo.delay" 0
"lfo.rate" 56
"vca.level" 127
"vca.mode" 0
"vcf.cutoff" 37
"vcf.env" 84
"vcf.keyboard" 127
"vcf.lfo" 0
"vcf.polarity" 0
"vcf.resonance" 0
TRACK 2 2 "juno.b78" "juno.b78" "Brassy Organ" ""
PARAM 29
"dco.lfo" 0
"dco.noise" 0
"dco.pulse" 1
"dco.pwm" 68
"dco.pwm_mode" 0
"dco.range" 0
"dco.saw" 1
"dco.sub" 56
"env.attack" 0
"env.decay" 36
"env.release" 0
"env.sustain" 109
"fx.chorus" 1
"fx.crush" 0
"fx.delay" 0
"fx.drive" 0
"fx.filter" 64
"fx.resonance" 20
"fx.reverb" 0
"lfo.delay" 0
"lfo.rate" 16
"vca.level" 127
"vca.mode" 0
"vcf.cutoff" 8
"vcf.env" 87
"vcf.keyboard" 52
"vcf.lfo" 0
"vcf.polarity" 0
"vcf.resonance" 0
TRACK 3 3 "sampler.empty" "sampler.empty" "Empty Slot" ""
PARAM 18
"amp.level" 96
"env.attack" 4
"env.release" 18
"filter.cutoff" 96
"filter.resonance" 18
"fx.chorus" 0
"fx.crush" 0
"fx.delay" 0
"fx.drive" 0
"fx.filter" 64
"fx.resonance" 20
"fx.reverb" 0
"sample.end" 127
"sample.gain" 96
"sample.loop" 0
"sample.root" 60
"sample.start" 0
"sample.tune" 0
SEQ 100 50
T 3 2 110 24 1 0 16
NOTES 14
0 0 36 127 96 0
0 4 42 110 28 0
0 10 36 110 31 2
0 12 42 110 24 0
1 0 36 110 20 1
1 1 36 110 22 2
1 6 39 110 25 2
1 8 36 110 24 0
1 12 42 110 24 0
2 0 36 110 24 0
2 4 39 110 24 0
2 8 36 110 24 0
2 10 36 110 24 0
2 12 39 110 24 0
T 4 2 110 24 1 0 16
NOTES 28
0 0 73 110 72 0
0 2 80 110 24 0
0 4 76 110 24 0
0 6 73 110 24 0
0 10 75 110 24 0
0 12 76 110 24 0
0 14 68 110 36 0
1 0 69 110 72 0
1 2 80 110 24 0
1 4 76 110 24 0
1 6 73 110 24 0
1 10 76 110 24 0
1 12 78 110 24 0
1 14 73 110 36 0
2 0 66 110 72 0
2 2 80 110 24 0
2 4 76 110 24 0
2 6 73 110 24 0
2 10 78 110 24 0
2 12 80 110 24 0
2 14 73 110 36 0
3 0 74 110 72 0
3 2 78 110 24 0
3 4 81 110 24 0
3 6 83 110 24 0
3 10 78 110 24 0
3 12 81 110 24 0
3 14 83 110 36 0
T 1 0 110 24 1 0 16
NOTES 0
T 1 0 110 24 1 0 16
NOTES 0
SONG 4 1
R 2 0 0 0 2
R 2 1 0 0 2
R 2 2 0 0 2
R 2 3 0 0 2
END
)FIXTURE";

// The 0.1.0 release kept its projects and samples where they are now, <card>/CardenzaSynth/{Projects,
// Samples} (storage protocol 2): they are listed and load as they are. Its session handover file is not
// read or changed.
void TestReleaseProjects(Harness& h)
{
  namespace fs = std::filesystem;
  auto& app = h.App();
  const fs::path release = fs::path(Device::Storage::RootPath()) / "CardenzaSynth";
  const fs::path ours = fs::path(synth::storage::ProjectDirectory());
  assert(fs::equivalent(ours, release / "Projects"));
  const auto write = [](const fs::path& path, const std::string& text) {
    fs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << text;
  };
  const auto read = [](const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(file), {});
  };
  const auto listed = [&](const std::string& name) {
    const auto projects = app.ListProjects();
    return std::find(projects.begin(), projects.end(), name) != projects.end();
  };
  for (const char* name : {"DEMO-1", "DEMO-2", "DRUM SET", "SONG1995"}) write(ours / (std::string(name) + ".ccproj"), kReleaseProject);
  write(release / "Session" / "session.cfg", "pending=1");
  for (const char* name : {"DEMO-1", "DEMO-2", "DRUM SET", "SONG1995"}) assert(listed(name));
  // Loading it from FILES: the notes are there, on the track they were on.
  GoToSetupPage(h, groove::kSetupFiles);
  const auto& projects = h.State().projects;
  const std::size_t index = static_cast<std::size_t>(std::find(projects.begin(), projects.end(), "DEMO-1") - projects.begin());
  assert(index < projects.size());
  while (h.State().fileCursor < index) h.Tap(kSub);
  while (h.State().fileCursor > index) h.Tap(kAdd);
  assert(h.State().fileCursor == index);
  h.Tap(kEnter);
  if (h.Modal() != nullptr) h.Tap(kEnter);
  assert(h.State().projectName == "DEMO-1");
  // All four tracks as they were: the sounds with their changes, the patterns, the notes.
  assert(app.EngineKind(0) == synth::SynthEngineKind::Drums && app.EngineKind(1) == synth::SynthEngineKind::Juno);
  assert(app.EngineKind(2) == synth::SynthEngineKind::Juno && app.EngineKind(3) == synth::SynthEngineKind::Sampler);
  const auto parameter = [&](std::size_t track, const char* id) {
    synth::SynthParameterInfo info;
    assert(app.Engine(track)->GetParameterInfo(id, info));
    return info.value;
  };
  assert(parameter(1, "env.sustain") == 22 && parameter(1, "env.release") == 70 && parameter(1, "vca.level") == 127);
  assert(parameter(1, "fx.chorus") == 2 && parameter(1, "dco.saw") == 0 && parameter(1, "dco.sub") == 37);
  assert(h.Seq().bpm == 100);
  assert(app.Sequencer().ActivePattern(0) == 2 && app.Sequencer().ActivePattern(1) == 2);
  std::array<synth::SynthSequencer::StepNoteView, 8> notes{};
  assert(app.Sequencer().StepNoteEvents(1, 0, notes) == 1 && notes[0].note == 66 && notes[0].lengthPulses == 72);
  assert(app.Sequencer().StepNoteEvents(0, 4, notes) == 1 && notes[0].note == 39);
  // Saving it writes it in place, in the current format, and the list shows it once.
  h.Chord(kFn, Key{2, 3});  // S
  assert(read(ours / "DEMO-1.ccproj").rfind("CARDENZA_PROJECT 7\n", 0) == 0);
  { const auto all = app.ListProjects(); assert(std::count(all.begin(), all.end(), std::string("DEMO-1")) == 1); }
  assert(read(ours / "DEMO-2.ccproj") == kReleaseProject);
  // A new project goes there too.
  GoToSetupPage(h, groove::kSetupProject);
  h.Tap(Grid(2));  // NEW
  if (h.Modal() != nullptr) h.Tap(kEnter);
  h.Chord(kFn, Key{2, 3});  // S
  assert(fs::exists(ours / (h.State().projectName + ".ccproj")));
  // The release's samples are the samples.
  assert(fs::equivalent(fs::path(synth::storage::SampleDirectory()), release / "Samples"));
  write(release / "Samples" / "OLD-KICK.wav", "riff");
  { const auto all = app.ListSamples(); assert(std::count(all.begin(), all.end(), std::string("OLD-KICK.wav")) == 1); }
  // The release's session file is left alone.
  assert(read(release / "Session" / "session.cfg") == "pending=1");
  for (const char* name : {"DEMO-2", "DRUM SET", "SONG1995"}) fs::remove(ours / (std::string(name) + ".ccproj"));
  fs::remove(release / "Samples" / "OLD-KICK.wav");
  fs::remove(release / "Session" / "session.cfg");
  GoToScene(h, 1);
}

// The device heap is small (no PSRAM, about 40 KB free while it plays): loading a project or NEW once
// built all four new engines beside the four playing and ran out, which ended the program (a user's
// DEMO-2 from 0.1.0, 2026-11-03). With too little room both say so and change nothing.
std::size_t gHeapFree = 0, gHeapLargest = 0;
void FakeHeap(std::size_t& freeBytes, std::size_t& largestBlock)
{
  freeBytes = gHeapFree;
  largestBlock = gHeapLargest;
}

void TestLoadWithoutMemory(Harness& h)
{
  namespace fs = std::filesystem;
  auto& app = h.App();
  const fs::path release = fs::path(synth::storage::ProjectDirectory());
  fs::create_directories(release);
  std::ofstream(release / "DEMO-2.ccproj", std::ios::binary) << kReleaseProject;
  GoToSetupPage(h, groove::kSetupProject);
  h.Tap(Grid(2));  // NEW: the default kinds, one of them a DX7
  if (h.Modal() != nullptr) h.Tap(kEnter);
  const std::string name = h.State().projectName;
  const auto kinds = [&] {
    std::array<synth::SynthEngineKind, synth::kSynthTrackCount> result{};
    for (std::size_t track = 0; track < result.size(); ++track) result[track] = app.EngineKind(track);
    return result;
  };
  const auto before = kinds();
  assert(before[2] == synth::SynthEngineKind::Dx7);
  const auto load = [&] {
    GoToSetupPage(h, groove::kSetupFiles);
    const auto& projects = h.State().projects;
    const std::size_t index = static_cast<std::size_t>(std::find(projects.begin(), projects.end(), "DEMO-2") - projects.begin());
    assert(index < projects.size());
    while (h.State().fileCursor < index) h.Tap(kSub);
    while (h.State().fileCursor > index) h.Tap(kAdd);
    h.Tap(kEnter);
    if (h.Modal() != nullptr) h.Tap(kEnter);
  };
  // A note of the current project: a load that fails after making room for itself (its patterns
  // wait in the autosave) brings it back.
  const std::uint8_t note = 64;
  assert(app.Sequencer().SetStepNotes(1, 5, &note, 1, 100));
  const auto hasNote = [&] {
    std::array<synth::SynthSequencer::StepNoteView, 8> notes{};
    return app.Sequencer().StepNoteEvents(1, 5, notes) == 1 && notes[0].note == note;
  };
  // Too little: NO MEMORY, and the session is as it was.
  gHeapFree = gHeapLargest = 18 * 1024;
  app.UseHeapProbe(&FakeHeap);
  load();
  assert(h.State().toast.text == "NO MEMORY");
  assert(h.State().projectName == name && kinds() == before && hasNote());
  GoToSetupPage(h, groove::kSetupProject);
  h.Tap(Grid(2));
  if (h.Modal() != nullptr) h.Tap(kEnter);
  assert(h.State().toast.text == "NO MEMORY" && kinds() == before && hasNote());
  // A free heap broken into blocks too small for the largest piece of an engine.
  gHeapFree = 200 * 1024;
  gHeapLargest = 1024;
  h.Tap(Grid(2));
  if (h.Modal() != nullptr) h.Tap(kEnter);
  assert(h.State().toast.text == "NO MEMORY" && kinds() == before);
  // Enough: DEMO-2 loads as it was saved.
  gHeapFree = 64 * 1024;
  gHeapLargest = 32 * 1024;
  load();
  assert(h.State().projectName == "DEMO-2");
  assert(app.EngineKind(0) == synth::SynthEngineKind::Drums && app.EngineKind(1) == synth::SynthEngineKind::Juno);
  assert(app.EngineKind(2) == synth::SynthEngineKind::Juno && app.EngineKind(3) == synth::SynthEngineKind::Sampler);
  assert(ParameterValue(app, 1, "env.sustain") == 22 && ParameterValue(app, 2, "vcf.cutoff") == 8);
  app.UseHeapProbe(nullptr);
  fs::remove(release / "DEMO-2.ccproj");
  GoToScene(h, 1);
}

void TestRenderSmoke(Harness& h)
{
  // Every scene and page renders; the left strips and header are always drawn.
  for (int scene = 1; scene <= 3; ++scene)
  {
    GoToScene(h, scene);
    const std::size_t pages = h.App().Ui().PageCount(h.State().scene);
    for (std::size_t page = 0; page < pages; ++page)
    {
      h.Wait(200);
      h.Render();
      const auto frame = Device::Display::CurrentFrameBuffer();
      bool content = false;
      for (int y = groove::theme::kContentY; y < groove::theme::kContentBottom && !content; ++y)
        for (int x = groove::theme::kContentX; x < groove::theme::kContentRight; ++x)
          if (frame.pixels[static_cast<std::size_t>(y) * frame.width + x] != 0) { content = true; break; }
      assert(content);
      h.Tap(kCtrl);
    }
  }
  h.Down(kEsc);
  h.Wait(300);
  h.Render();
  h.Up(kEsc);

  // Informational: render cost of a steady frame versus a scene-slide frame (both scenes drawn).
  using Clock = std::chrono::steady_clock;
  auto timeFrames = [&](int frames) {
    const auto start = Clock::now();
    for (int index = 0; index < frames; ++index) h.Render();
    return std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - start).count() / frames;
  };
  GoToScene(h, 2);
  h.Wait(400);
  const auto steady = timeFrames(200);
  h.Chord(kEsc, Digit(3));
  gNow -= 32;  // stay inside the 260 ms slide while timing it
  const auto sliding = timeFrames(200);
  std::cout << "render us: steady " << steady << ", scene slide " << sliding << "\n";
}

}  // namespace

// A computer on USB: MIDI channels 1-4 play tracks 1-4 and those notes have an owner (the safety net
// leaves them alone); a disconnect ends them; what the device plays goes back out, track n on channel n.
void TestComputerMidi(Harness& h)
{
  auto& app = h.App();
  std::array<std::int16_t, 256> block{};
  const auto settle = [&](int frames) {
    for (int frame = 0; frame < frames; ++frame)
    {
      h.Frames(1);
      synth::audio::RenderFrames(block.data(), block.size());
    }
  };
  const auto held = [](std::size_t track, std::uint8_t note) {
    return (synth::audio::HeldNotes(track)[note / 32] & (1u << (note % 32))) != 0;
  };
  std::uint8_t packet[4];
  while (CardenzaSim::Input::TakeDeviceMidi(packet)) {}
  std::string error;
  assert(app.EngineKind(1) == synth::SynthEngineKind::Juno || app.SwitchEngine(1, synth::SynthEngineKind::Juno, error));
  // A solo left by an earlier test would keep track 2 quiet.
  for (std::size_t track = 0; track < synth::kSynthTrackCount; ++track)
  {
    app.SetTrackSoloed(track, false);
    app.SetTrackMuted(track, false);
  }
  settle(4);
  const std::uint32_t orphansBefore = app.EndedOrphanNotes();
  CardenzaSim::Input::SetComputerConnected(true);
  const std::uint8_t on[4] = {0x09, 0x91, 64, 100};  // channel 2: track 2
  CardenzaSim::Input::SendComputerMidi(on);
  settle(80);  // longer than the safety net takes to confirm
  assert(held(1, 64) && app.EndedOrphanNotes() == orphansBefore);
  const std::uint8_t off[4] = {0x08, 0x81, 64, 0};
  CardenzaSim::Input::SendComputerMidi(off);
  settle(4);
  assert(!held(1, 64));
  CardenzaSim::Input::SendComputerMidi(on);
  settle(4);
  assert(held(1, 64));
  CardenzaSim::Input::SetComputerConnected(false);
  settle(4);
  assert(!held(1, 64) && app.EndedOrphanNotes() == orphansBefore);
  // The pads go out on the current track's channel.
  CardenzaSim::Input::SetComputerConnected(true);
  GoToScene(h, 1);
  h.Chord(kTab, Key{0, 10});  // track 2
  h.Down(Key{3, 1});  // C3
  h.Up(Key{3, 1});
  bool noteOn = false, noteOff = false;
  while (CardenzaSim::Input::TakeDeviceMidi(packet))
  {
    noteOn = noteOn || packet[1] == 0x91;
    noteOff = noteOff || packet[1] == 0x81;
  }
  assert(noteOn && noteOff);
  CardenzaSim::Input::SetComputerConnected(false);
  settle(4);
}

// The guide: a lesson page sets the working state aside in a practice project, drives the device,
// follows its keys and state, and the working state comes back however the lesson ends.
void TestGuide(Harness& h)
{
  auto& app = h.App();
  auto& ui = app.Ui();
  std::vector<std::string> heard;
  const auto listen = [&]() {
    std::string message;
    while (CardenzaSim::Input::TakeDeviceApp(message)) heard.push_back(message);
  };
  const auto send = [&](const std::string& text) {
    CardenzaSim::Input::SendComputerApp(text);
    h.Frames(2);
    listen();
  };
  const auto sent = [&](const std::string& text) { return std::find(heard.begin(), heard.end(), text) != heard.end(); };
  // The last state, and one field of it ('n' is the page tab: not a number).
  const auto state = [&]() {
    for (auto it = heard.rbegin(); it != heard.rend(); ++it)
      if (it->rfind("GS", 0) == 0) return it->substr(2);
    return std::string();
  };
  const auto field = [&](char name) {
    const std::string text = state();
    const std::size_t at = text.find(name);
    assert(at != std::string::npos);
    return std::atoi(text.c_str() + at + 1);
  };
  const auto hits = [&](std::size_t track) {
    std::size_t count = 0;
    std::array<synth::SynthSequencer::StepNoteView, 8> notes{};
    for (std::size_t step = 0; step < 16; ++step) count += app.Sequencer().StepNoteEvents(track, step, notes) > 0 ? 1 : 0;
    return count;
  };
  listen();
  heard.clear();
  // The user's keyboard is CHROMA (every pad a semitone): the lessons play with it.
  const auto setLayout = [&](groove::KeyboardLayout layout, Key step) {
    GoToSetupPage(h, groove::kSetupKeys);
    h.Tap(Grid(0));  // LAYOUT
    for (int tries = 0; tries < 4 && h.State().keyboard.layout != layout; ++tries) h.Tap(step);
    assert(h.State().keyboard.layout == layout);
    h.Tap(kEsc);
  };
  setLayout(groove::KeyboardLayout::Chromatic, kAdd);
  // The working state: SEQ, T1 with one hit on step 3.
  GoToScene(h, 2);
  h.Chord(kTab, Grid(0));
  const std::uint8_t kick = 36;
  // One pattern, whatever the tests before left on T1 (the key storm makes some).
  app.Sequencer().SetActiveTrack(0);
  (void)app.Sequencer().SelectOrCreatePattern(0);
  while (app.Sequencer().PatternCount(0) > 1) assert(app.Sequencer().DeletePattern(app.Sequencer().PatternCount(0) - 1));
  for (std::size_t step = 0; step < 16; ++step) (void)app.Sequencer().SetStepNotes(0, step, &kick, 0, 100);
  assert(app.Sequencer().SetStepNotes(0, 2, &kick, 1, 100));
  assert(hits(0) == 1);

  send("G?");
  assert(sent("G!1 g0"));
  // Keys are not reported, nor do the guide's commands work, before it starts.
  send("GLs3");
  assert(h.State().scene == Scene::Sequence);
  send("GE");
  assert(sent("GE1") && ui.Guide());
  // A fresh practice project on INSTR VOICE, T1.
  assert(field('s') == 1 && field('p') == 0 && field('t') == 0 && field('h') == 0 && hits(0) == 0);
  assert(state().find("nVOICE") != std::string::npos);
  h.Render();

  // The practice project has the user's keyboard; f1 gives a lesson a new project's (PIANO). Its pages are
  // those of a new start: SETUP opens on PROJECT, though the user left it on KEYS.
  assert(field('z') == 2);
  send("GLs0");
  assert(field('s') == 0 && field('p') == 0);
  // Every pad of the hint lights, all 32 in two messages (one holds 29: kAppMessageSize), and none the
  // keyboard leaves silent: PIANO's gaps stay dark.
  const auto hex = [](std::size_t key) { return std::string(1, "0123456789abcdef"[key / 16]) + "0123456789abcdef"[key % 16]; };
  std::string first = "GH", rest = "G+";
  std::size_t listed = 0;
  for (std::size_t row = 0; row < 4; ++row)
    for (std::size_t column = 1; column <= 8; ++column) (listed++ < 29 ? first : rest) += hex(groove::KeyIndex(row, column));
  assert(first.size() <= Device::USB::kAppMessageSize);
  const auto padsFull = [&]() {
    groove::LedFrame frame{};
    ui.ComputeLeds(frame);
    std::uint64_t full = 0;
    for (std::size_t row = 0; row < 4; ++row)
      for (std::size_t column = 1; column <= 8; ++column)
        if (frame[groove::KeyIndex(row, column)].level == groove::kLedFull) full |= std::uint64_t{1} << groove::KeyIndex(row, column);
    return full;
  };
  send("GLs1p0t2");
  send(first);
  send(rest);
  assert(std::bitset<64>(padsFull()).count() == 32);
  send("GLr1f1s1p0t2");
  assert(field('z') == 0 && h.State().track == 1);
  send(first);
  send(rest);
  const std::uint64_t lit = padsFull();
  assert(std::bitset<64>(lit).count() == 26);
  for (const std::size_t row : {0u, 2u})
    for (const std::size_t column : {1u, 4u, 8u}) assert(((lit >> groove::KeyIndex(row, column)) & 1u) == 0);
  assert(((lit >> groove::KeyIndex(3, 8)) & 1u) != 0);  // from G+
  send("GLr1s1p0t1");
  assert(field('z') == 2 && field('t') == 0);
  send("GH");

  // The hint, as the lesson page shows it: ESC and ADD full, every other key no brighter than half; on
  // the device the two edge keys go full too (otherwise held to the dimmest glow). While the step waits
  // (GH) they blink slowly; once it is learned (GT) they stay lit. Only while the guide runs.
  const auto settle = [&]() {
    for (int frame = 0; frame < 20; ++frame)
    {
      h.Frames(1);
      app.UpdateLeds();
    }
  };
  const auto hintIs = [&](groove::LedEffect effect) {
    groove::LedFrame frame{};
    ui.ComputeLeds(frame);
    for (std::size_t index = 0; index < groove::kKeyCount; ++index)
    {
      const bool hinted = index == 0 || index == 13;
      assert(!hinted || frame[index].effect == effect);
      assert(hinted ? frame[index].level == groove::kLedFull : frame[index].level <= groove::kLedHalf);
    }
  };
  send("GH000d");
  hintIs(groove::LedEffect::Hint);
  settle();
  // Blinking: lit and dark in turn, half a second each, ADD with ESC; the other edge keys stay dim.
  bool seenLit = false, seenDark = false;
  for (int look = 0; look < 12; ++look)
  {
    h.Wait(130);
    app.UpdateLeds();
    const std::uint8_t esc = Device::LED::Brightness(0, 0);
    seenLit = seenLit || esc == groove::kLedFull;
    seenDark = seenDark || esc == 0;
    assert(Device::LED::Brightness(1, 0) <= groove::kLedEdgeMax);
  }
  assert(seenLit && seenDark);
  send("GT000d");
  hintIs(groove::LedEffect::Steady);
  settle();
  for (int look = 0; look < 6; ++look)
  {
    h.Wait(130);
    app.UpdateLeds();
    assert(Device::LED::Brightness(0, 0) == groove::kLedFull && Device::LED::Brightness(0, 13) == groove::kLedFull);
  }
  send("GH");
  settle();
  assert(ui.GuideHint() == 0 && Device::LED::Brightness(0, 0) <= groove::kLedEdgeMax);

  // Keys the user presses go to the page; keys the page presses do not come back.
  heard.clear();
  h.Tap(kTab);
  listen();
  assert(sent("GK0e1") && sent("GK0e0"));
  h.Frames(4);
  listen();
  assert(field('t') == 1);
  heard.clear();
  send("GK0e1");
  send("GK0e0");
  h.Frames(4);
  listen();
  assert(!sent("GK0e1") && field('t') == 2);

  // A lesson start: SEQ on T1, where hits written by the page's keys count.
  send("GLs2p0t1");
  assert(field('s') == 2 && field('t') == 0 && h.State().track == 0);
  const std::string pad = "GK" + std::string(1, "0123456789abcdef"[groove::KeyIndex(3, 2) / 16]) + "0123456789abcdef"[groove::KeyIndex(3, 2) % 16];
  send(pad + "1");
  send("GK091");
  send("GK090");
  send(pad + "0");
  h.Frames(4);
  listen();
  assert(field('h') == 1 && hits(0) == 1);
  // The song lessons' song so far: the beat (d1), its busier copy that T1 stays on (d2), the chords
  // on T3 (d3), the tune on T2 (d4).
  send("GLd1s2p0t1");
  assert(field('s') == 2 && field('q') == 1 && field('h') == 8 && field('g') == 0);
  send("GLd4s3p1t1");
  assert(field('s') == 3 && field('p') == 1 && field('q') == 2 && field('w') == 1 && field('h') == 8 && field('g') == 0);
  {
    std::array<synth::SynthSequencer::StepNoteView, 8> chord{};
    assert(app.Sequencer().StepNoteEvents(0, 0, chord) == 2 && app.Sequencer().StepNoteEvents(2, 0, chord) == 3 &&
           app.Sequencer().StepNoteEvents(1, 14, chord) == 1 && chord[0].note == 60);
  }

  // The end: the working state is back, where the user left it.
  send("GX");
  assert(sent("GX") && !ui.Guide());
  // The user's pages are theirs again: SETUP where they left it.
  assert(h.State().page[static_cast<std::size_t>(Scene::Setup)] == groove::kSetupKeys);
  assert(h.State().scene == Scene::Sequence && h.State().track == 0 && hits(0) == 1);
  std::array<synth::SynthSequencer::StepNoteView, 8> notes{};
  assert(app.Sequencer().StepNoteEvents(0, 2, notes) == 1 && app.Sequencer().PatternCount(0) == 1);
  assert(ui.GuideHint() == 0);

  // A page that goes quiet ends the guide by itself.
  send("GE");
  assert(ui.Guide() && hits(0) == 0);
  heard.clear();
  h.Wait(31000);
  listen();
  assert(!ui.Guide() && sent("GX") && hits(0) == 1);
  // While nothing runs, messages without G are ignored, and a guide message after the end is too.
  send("GH000d");
  assert(ui.GuideHint() == 0);
  setLayout(groove::KeyboardLayout::Piano, kSub);
}

// Every cell of every page of every scene, on every engine, stopped and playing: moved both ways, in
// big steps to both ends, ENTER, picked from the pads; then every parameter of every engine at both ends,
// and every preset. Whatever falls over here shows as an assertion or under ASan. Running out of memory
// does not show on a host: see the on-board sweep in Applications/Synth/StressTest.cpp.
void TestEveryParameter(Harness& h)
{
  using Kind = synth::SynthEngineKind;
  auto& app = h.App();
  std::array<std::int16_t, 256> block{};
  const auto heard = [&]() { synth::audio::RenderFrames(block.data(), block.size()); };
  const auto closeModals = [&]() { for (int i = 0; i < 6 && h.Modal() != nullptr; ++i) h.Tap(kEsc); };
  const auto setKinds = [&](const std::array<Kind, 4>& kinds) {
    for (std::size_t track = 0; track < kinds.size(); ++track)
    {
      std::string error;
      assert(app.EngineKind(track) == kinds[track] || app.SwitchEngine(track, kinds[track], error));
    }
  };
  const auto addNotes = [&]() {
    for (std::size_t step = 0; step < 16; step += 2)
    {
      const std::uint8_t notes[4] = {static_cast<std::uint8_t>(36 + step % 5), 64, 67, 71};
      for (std::size_t track = 0; track < 4; ++track) (void)app.Sequencer().SetStepNotes(track, step, notes, 1 + track, 110);
    }
  };
  const std::array<std::array<Kind, 4>, 2> rack{{{{Kind::Drums, Kind::Juno, Kind::Dx7, Kind::Sampler}},
                                                 {{Kind::Juno, Kind::Dx7, Kind::Sampler, Kind::Drums}}}};
  struct Walk { groove::Scene scene; std::size_t track; };
  const std::array<Walk, 10> walks{{{groove::Scene::Tape, 0},
                                    {groove::Scene::Instrument, 0}, {groove::Scene::Instrument, 1},
                                    {groove::Scene::Instrument, 2}, {groove::Scene::Instrument, 3},
                                    {groove::Scene::Sequence, 0}, {groove::Scene::Sequence, 1},
                                    {groove::Scene::Sequence, 2}, {groove::Scene::Sequence, 3},
                                    {groove::Scene::Setup, 0}}};
  for (std::size_t pass = 0; pass < rack.size(); ++pass)
  {
    closeModals();
    setKinds(rack[pass]);
    addNotes();
    for (const Walk& walk : walks)
    {
      closeModals();
      h.Chord(kEsc, Key{0, 9 + static_cast<std::size_t>(walk.scene)});
      h.Chord(kTab, Key{0, 9 + walk.track});
      closeModals();
      assert(h.State().scene == walk.scene && h.State().track == walk.track);
      for (std::size_t page = 0; page < app.Ui().PageCount(walk.scene); ++page)
      {
        for (std::size_t cell = 0; cell < 16; ++cell)
        {
          // The cell before may have gone anywhere.
          closeModals();
          if (h.State().scene != walk.scene) h.Chord(kEsc, Key{0, 9 + static_cast<std::size_t>(walk.scene)});
          closeModals();
          if (h.State().track != walk.track) h.Chord(kTab, Key{0, 9 + walk.track});
          closeModals();
          if (app.Ui().CurrentPage() != page) h.Chord(kCtrl, Grid(page));
          closeModals();
          assert(app.Ui().CurrentPage() == page);
          if (app.Sequencer().Playing() != (pass == 1)) app.Sequencer().TogglePlayback();
          h.Tap(Grid(cell));
          closeModals();
          for (const auto& [modifier, key] : std::array<std::pair<bool, Key>, 7>{{{false, kAdd}, {true, kAdd}, {true, kAdd}, {false, kSub}, {true, kSub}, {true, kSub}, {true, kSub}}})
          {
            if (modifier) h.Chord(kFn, key); else h.Tap(key);
            closeModals();
            heard();
          }
          h.Tap(kEnter);
          closeModals();
          h.Down(Grid(cell));
          h.Wait(250);
          h.Tap(Key{0, 1});
          h.Tap(Key{3, 8});
          h.Up(Grid(cell));
          closeModals();
          heard();
          h.Render();
        }
      }
    }
    // Everything the engines have, also what is on no page.
    app.Sequencer().Play();
    for (std::size_t track = 0; track < 4; ++track)
    {
      synth::ISynthEngine* engine = app.Engine(track);
      synth::SynthPageRequest request;
      request.limit = 1;
      for (request.offset = 0;; ++request.offset)
      {
        const synth::SynthParameterPage parameters = engine->ListParameters(request);
        if (parameters.items.empty()) break;
        const synth::SynthParameterInfo& parameter = parameters.items[0];
        for (const int value : {parameter.minValue, parameter.maxValue, parameter.defaultValue})
        {
          (void)engine->SetParameterValue(parameter.id, value);
          (void)engine->DescribeParameterValue(parameter.id);
          h.Frames();
          heard();
        }
      }
      for (request.offset = 0;; ++request.offset)
      {
        const synth::SynthPresetPage presets = engine->ListPresets(request);
        if (presets.items.empty()) break;
        (void)engine->LoadPreset(presets.items[0].id);
        h.Frames();
        heard();
      }
    }
    h.Render();
    app.Sequencer().Stop();
  }
  setKinds(rack[0]);
  closeModals();
}

int main()
{
  // Projects and samples go to a scratch card under the test's working directory.
  const auto root = std::filesystem::temp_directory_path() / ("cardenza-groove-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root);
  std::filesystem::current_path(root);

  assert(os::system::Begin());
  auto* app = dynamic_cast<apps::GrooveApp*>(os::system::ActiveApplication());
  assert(app != nullptr);
  // Before the first Tick starts the boot animation, no working-keyboard frame may flash onto the
  // LEDs. Repeated publication must also remain dark.
  for (int frame = 0; frame < 3; ++frame)
  {
    app->UpdateLeds();
    for (std::size_t row = 0; row < groove::kRows; ++row)
      for (std::size_t column = 0; column < groove::kColumns; ++column)
        assert(Device::LED::Brightness(row, column) == 0);
  }
  app->UseClock(&FakeClock);
  Harness h(*app);
  h.Frames(4);

  TestPresetBrowsers(h);
  TestBoot(h);
  TestNavigation(h);
  TestStepEditing(h);
  TestPatterns(h);
  TestTransport(h);
  TestInstrument(h);
  TestPresetEdits(h);
  TestTape(h);
  TestMix(h);
  TestSendsAndFx(h);
  TestProject(h);
  TestQuickSaveReleasesFn(h);
  TestLeds(h);
  TestLedMotion(h);
  TestPlayFeatures(h);
  TestFnQuick(h);
  TestSession(h);
  TestRecording(h);
  TestSelectMode(h);
  TestLongCapture(h);
  TestHeldPadEdits(h);
  TestLongPatternUi(h);
  TestSampleRecording(h);
  TestDrumMachine(h);
  TestRenderSmoke(h);
  TestReleaseProjects(h);
  TestLoadWithoutMemory(h);
  TestAdjustIcons(h);
  TestDrumPads(h);
  TestCellPicker(h);
  TestHeldNotesEnd(h);
  TestNoStuckNotes(h);
  TestComputerMidi(h);
  TestGuide(h);
  TestEveryParameter(h);
  // Last: it runs every item of the SEQ menu, which changes the pattern.
  TestMenuFeedback(h);

  std::filesystem::current_path(std::filesystem::temp_directory_path());
  std::filesystem::remove_all(root);
  std::cout << "Groovebox UI workflows passed\n";
  return 0;
}
