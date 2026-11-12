#include "Applications/Synth/UI/Groove/GrooveController.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <string_view>

namespace groove {

namespace {


constexpr std::size_t LeftIndex(KeyRole role)
{
  switch (role)
  {
    case KeyRole::Esc: return 0;
    case KeyRole::Tab: return 1;
    case KeyRole::Fn: return 2;
    case KeyRole::Ctrl:
    default: return 3;
  }
}

constexpr std::size_t PadIndex(std::size_t row, std::size_t column)
{
  return row * kPadColumns + (column - kPadFirstColumn);
}

}  // namespace

Controller::Controller(Host& host) : host_(host)
{
  state_.padNote.fill(kNoNote);
  state_.padChordCount.fill(0);
  state_.padTrack.fill(0);
  state_.instrumentFocus.fill(0);
  state_.page.fill(0);
#if defined(CARDENZA_NATIVE_UI)
  NativePublishPerformance();
#endif
}

void Controller::Restore(const ProjectUiSettings& settings, const std::string& projectName, bool dirty)
{
#if defined(CARDENZA_NATIVE_UI)
  NativeCancelGesture();
  nativeFocusedCell_ = 0xFF;
#endif
  ReleaseAllPads();
  state_.modals.clear();
  state_.track = std::min(settings.activeTrack, kTrackCount - 1);
  state_.keyboard = settings.keyboard;
  state_.chords = settings.chords;
  state_.projectName = projectName;
  state_.dirty = dirty;
  state_.sessionAt = state_.now == 0 ? 1 : state_.now;
  state_.clipboardValid = false;
  state_.songRow = 0;
  state_.songTrack = 0;
  state_.songScroll = 0;
  state_.songClipboardValid = false;
  state_.instrumentFocus.fill(0);
  state_.trackInstrumentPage.fill(0);
  for (auto& focus : state_.trackInstrumentFocus) focus.fill(0);
  state_.page[static_cast<std::size_t>(Scene::Instrument)] = 0;
  host_.Sequencer().SetActiveTrack(state_.track);
  host_.Sequencer().ClearSelectedSteps();
  ClampPages();
}

ProjectUiSettings Controller::Settings() const
{
  ProjectUiSettings settings;
  settings.activeTrack = state_.track;
  settings.keyboard = state_.keyboard;
  settings.chords = state_.chords;
  return settings;
}

bool Controller::EscOverlay() const
{
  const auto& key = state_.left[0];
  return key.down && (key.used || state_.now - key.pressedAt >= kHoldMs);
}

bool Controller::TabOverlay() const
{
  const auto& key = state_.left[1];
  return key.down && (key.used || state_.now - key.pressedAt >= kHoldMs);
}

bool Controller::FnOverlay() const
{
  // The held menu; in input screens FN is the yellow keycap layer instead and shows no menu.
  const auto& key = state_.left[2];
  return key.down && state_.now - key.pressedAt >= kHoldMs && !FnYellowMode();
}

bool Controller::CtrlOverlay() const
{
  const auto& key = state_.left[3];
  return key.down && !key.suppressed && TopModal() == nullptr && !state_.left[2].down &&
         (key.used || state_.now - key.pressedAt >= kHoldMs);
}

synth::ISynthEngine* Controller::ActiveEngine()
{
  return host_.Engine(state_.track);
}

const synth::ISynthEngine* Controller::ActiveEngine() const
{
  return host_.Engine(state_.track);
}

synth::SynthEngineKind Controller::ActiveKind() const
{
  return host_.EngineKind(state_.track);
}

InstrumentPageList Controller::InstrumentPages() const
{
  return InstrumentPagesFor(ActiveKind());
}

const InstrumentPage* Controller::CurrentInstrumentPage() const
{
  const InstrumentPageList list = InstrumentPages();
  const std::size_t page = std::min<std::size_t>(state_.page[static_cast<std::size_t>(Scene::Instrument)], list.count - 1);
  return list.pages + page;
}

std::uint8_t Controller::InstrumentFocus() const
{
  const std::size_t page = std::min<std::size_t>(state_.page[static_cast<std::size_t>(Scene::Instrument)], state_.instrumentFocus.size() - 1);
  return state_.instrumentFocus[page];
}

std::size_t Controller::PageCount(Scene scene) const
{
  switch (scene)
  {
    case Scene::Instrument: return InstrumentPages().count;
    case Scene::Sequence: return 3;
    case Scene::Tape: return kTapePageCount;
    case Scene::Setup:
    default: return kSetupPageCount;
  }
}

std::string Controller::PageName(Scene scene, std::size_t page) const
{
  static constexpr std::array<const char*, kTapePageCount> kTape{{"CLIPS", "ARRANGE", "LEVELS", "PER TRK", "MASTER"}};
  static constexpr std::array<const char*, 3> kSequence{{"EDIT", "SLOTS", "LENGTH"}};
  static constexpr std::array<const char*, kSetupPageCount> kSetup{{"SAVE BPM", "LOAD", "SCALE", "DEVICE"}};
  switch (scene)
  {
    case Scene::Instrument:
    {
      const InstrumentPageList list = InstrumentPages();
      return page < list.count ? list.pages[page].name : "";
    }
    case Scene::Sequence: return page < kSequence.size() ? kSequence[page] : "";
    case Scene::Tape: return page < kTape.size() ? kTape[page] : "";
    case Scene::Setup:
    default: return page < kSetup.size() ? kSetup[page] : "";
  }
}

std::string Controller::PageTab(Scene scene, std::size_t page) const
{
  static constexpr std::array<const char*, kTapePageCount> kTape{{"SESSION", "SONG", "MIX", "SENDS", "FX"}};
  static constexpr std::array<const char*, 3> kSequence{{"STEPS", "PATTERN", "TRACK"}};
  static constexpr std::array<const char*, kSetupPageCount> kSetup{{"PROJECT", "FILES", "KEYS", "SYSTEM"}};
  switch (scene)
  {
    case Scene::Instrument:
    {
      const InstrumentPageList list = InstrumentPages();
      return page < list.count ? list.pages[page].tab : "";
    }
    case Scene::Sequence:
      if (page == 0 && DrumLayout()) return "DRUMS";
      if (page == 0) return View() == SeqView::Roll ? "ROLL" : "STEP";
      return page < kSequence.size() ? kSequence[page] : "";
    case Scene::Tape: return page < kTape.size() ? kTape[page] : "";
    case Scene::Setup:
    default: return page < kSetup.size() ? kSetup[page] : "";
  }
}

void Controller::ClampPages()
{
  for (std::size_t scene = 0; scene < kSceneCount; ++scene)
  {
    const std::size_t count = PageCount(static_cast<Scene>(scene));
    if (state_.page[scene] >= count) state_.page[scene] = static_cast<std::uint8_t>(count - 1);
  }
}

bool Controller::PadNoteFor(std::size_t row, std::size_t column, int& note) const
{
  return PadNote(state_.keyboard, ActiveKind(), row, column, note);
}

bool Controller::IsPadDown(std::size_t row, std::size_t column) const
{
  if (row >= kRows || column < kPadFirstColumn || column >= kPadFirstColumn + kPadColumns) return false;
  return state_.padNote[PadIndex(row, column)] != kNoNote;
}

bool Controller::IsNoteHeld(int note) const
{
#if defined(CARDENZA_NATIVE_UI)
  for (const auto& contact : nativeContacts_)
    if (contact.active && contact.baseNote == note && contact.track == state_.track) return true;
#endif
  for (std::size_t index = 0; index < state_.padNote.size(); ++index)
    if (state_.padNote[index] == note && state_.padTrack[index] == state_.track) return true;
  return false;
}

bool Controller::IsChordNoteHeld(int note) const
{
#if defined(CARDENZA_NATIVE_UI)
  if (note >= 0 && note < 128 && (NativeNoteOwners(state_.track, static_cast<std::uint8_t>(note), false) != 0 ||
      NativeNoteOwners(state_.track, static_cast<std::uint8_t>(note), true) != 0)) return true;
#endif
  for (std::size_t index = 0; index < state_.padNote.size(); ++index)
  {
    if (state_.padNote[index] == kNoNote || state_.padTrack[index] != state_.track) continue;
    for (std::size_t voice = 0; voice < state_.padChordCount[index]; ++voice)
      if (state_.padChord[index][voice] == note) return true;
  }
  return false;
}

// Every note the held pads sound on the active track (chords included), for step input.
std::size_t Controller::CollectHeldNotes(std::array<std::uint8_t, kRows * kPadColumns>& notes) const
{
  std::size_t count = 0;
#if defined(CARDENZA_NATIVE_UI)
  for (const auto& contact : nativeContacts_)
  {
    if (!contact.active || contact.track != state_.track) continue;
    for (std::size_t voice = 0; voice < contact.count && count < notes.size(); ++voice)
    {
      const auto note = contact.notes[voice];
      if (std::find(notes.begin(), notes.begin() + static_cast<std::ptrdiff_t>(count), note) == notes.begin() + static_cast<std::ptrdiff_t>(count))
        notes[count++] = note;
    }
  }
#endif
  for (std::size_t index = 0; index < state_.padNote.size(); ++index)
  {
    if (state_.padNote[index] == kNoNote || state_.padTrack[index] != state_.track) continue;
    for (std::size_t voice = 0; voice < state_.padChordCount[index] && count < notes.size(); ++voice)
    {
      const std::uint8_t note = state_.padChord[index][voice];
      if (std::find(notes.begin(), notes.begin() + static_cast<std::ptrdiff_t>(count), note) == notes.begin() + static_cast<std::ptrdiff_t>(count))
        notes[count++] = note;
    }
  }
  return count;
}

// Starts or ends the notes of one held pad: straight to the engine, or into the arpeggiator.
void Controller::SoundPad(std::size_t index, bool on)
{
  auto& sequencer = host_.Sequencer();
  const std::uint8_t track = state_.padTrack[index];
  for (std::size_t voice = 0; voice < state_.padChordCount[index]; ++voice)
  {
    const std::uint8_t note = state_.padChord[index][voice];
#if defined(CARDENZA_NATIVE_UI)
    NativeSoundPerformance(track, note, state_.padVelocity[index], on, state_.padArp[index]);
    continue;
#endif
    if (state_.padArp[index])
    {
      if (on) sequencer.ArpNoteOn(track, note, state_.padVelocity[index]);
      else sequencer.ArpNoteOff(track, note);
      continue;
    }
    if (on)
    {
      host_.NoteOn(track, note, state_.padVelocity[index]);
      CaptureNoteOn(track, note, state_.padVelocity[index]);
    }
    else
    {
      host_.NoteOff(track, note);
      sequencer.RecordNoteOff(track, note);
      CaptureNoteOff(track, note);
    }
  }
}

void Controller::AddHeldPadNotes(std::size_t track, std::array<std::uint32_t, 4>& notes) const
{
#if defined(CARDENZA_NATIVE_UI)
  for (const auto& contact : nativeContacts_)
  {
    if (!contact.active || contact.track != track) continue;
    for (std::size_t voice = 0; voice < contact.count; ++voice)
    {
      const auto note = contact.notes[voice];
      notes[note / 32] |= 1u << (note % 32);
    }
  }
#endif
  for (std::size_t index = 0; index < state_.padNote.size(); ++index)
  {
    if (state_.padNote[index] == kNoNote || state_.padTrack[index] != track) continue;
    for (std::size_t voice = 0; voice < state_.padChordCount[index]; ++voice)
    {
      const std::uint8_t note = state_.padChord[index][voice] & 0x7F;
      notes[note / 32] |= 1u << (note % 32);
    }
  }
  // A step key held while stopped sounds its step (AuditionStep).
  const StepAudition& audition = state_.stepAudition;
  if (static_cast<std::size_t>(audition.track) != track) return;
  for (std::size_t slot = 0; slot < audition.count; ++slot)
  {
    const std::uint8_t note = audition.notes[slot] & 0x7F;
    notes[note / 32] |= 1u << (note % 32);
  }
}

void Controller::ReleaseAllPads()
{
  StopStepAudition();
  for (std::size_t index = 0; index < state_.padNote.size(); ++index)
  {
    if (state_.padNote[index] == kNoNote) continue;
#if defined(CARDENZA_NATIVE_UI)
    state_.padNote[index] = kNoNote;
    NativePublishPerformance();
#endif
    SoundPad(index, false);
    state_.padNote[index] = kNoNote;
    state_.padChordCount[index] = 0;
  }
#if defined(CARDENZA_NATIVE_UI)
  NativePublishPerformance();
  NativeReleasePerformance();
#endif
}

void Controller::ShowToast(const std::string& text, ToastKind kind)
{
  state_.toast.text = text;
  state_.toast.kind = kind;
  state_.toast.until = state_.now + kToastMs;
  // What the screen said goes into the console log too (OS/System/Log.h).
  std::printf("ui: %s%s\n", kind == ToastKind::Error ? "error: " : "", text.c_str());
}

// ---------------------------------------------------------------------------------------------
// Input routing

void Controller::KeyEvent(std::size_t row, std::size_t column, bool pressed, std::uint32_t nowMs)
{
  state_.now = nowMs;
  const KeyRole role = RoleFor(row, column);
  if (role == KeyRole::None) return;
#if defined(CARDENZA_NATIVE_UI)
  if (host_.Sampler().SavingTrack() >= 0 && pressed) return;
  if (pressed && role == KeyRole::Grid) nativeFocusedCell_ = 0xFF;
#endif
  if (pressed) state_.keyPressedAt[KeyIndex(row, column)] = nowMs == 0 ? 1 : nowMs;

  // Text entry owns the whole keyboard except the left modifiers. Releases still settle key state
  // that was pressed before the field opened (a held FN, the SAVE AS key, a sounding pad).
  const Modal* modal = TopModal();
  if (modal != nullptr && modal->kind == ModalKind::TextEntry)
  {
    if (pressed)
    {
      // FN is tracked so FN + key can use the yellow keycap layer (cursor keys, forward delete).
      if (role == KeyRole::Fn) state_.left[row] = LeftKeyState{true, false, false, nowMs};
      if (role != KeyRole::Esc) HandleTextKey(row, column);
      return;
    }
    switch (role)
    {
      case KeyRole::Esc:
        if (state_.left[0].down) state_.left[0] = LeftKeyState{};
        else CloseModal();
        return;
      case KeyRole::Tab:
      case KeyRole::Fn:
      case KeyRole::Ctrl:
        state_.left[row] = LeftKeyState{};
        return;
      case KeyRole::Sub:
      case KeyRole::Add:
        state_.repeat = RepeatState{};
        return;
      case KeyRole::Pad:
        ReleasePad(row, column);
        return;
      case KeyRole::Grid:
        state_.gridDown[GridIndex(row, column)] = false;
        StepKeyRelease(GridIndex(row, column));
        return;
      default:
        return;
    }
  }

  // A step key on the SEQ note page down while any other key is pressed: a hold, not a tap.
  if (pressed) UseStepKeys();
  switch (role)
  {
    case KeyRole::Esc:
    case KeyRole::Tab:
    case KeyRole::Fn:
    case KeyRole::Ctrl:
      pressed ? PressLeft(row) : ReleaseLeft(row);
      return;
    case KeyRole::Sub:
    case KeyRole::Add:
    case KeyRole::Enter:
    case KeyRole::Play:
      pressed ? PressRight(role) : ReleaseRight(role);
      return;
    case KeyRole::Pad:
      pressed ? PressPad(row, column) : ReleasePad(row, column);
      return;
    case KeyRole::Grid:
      if (!pressed && state_.menuPress.down && state_.menuPress.index == GridIndex(row, column))
      {
        state_.menuPress.down = false;
        state_.menuPress.releasedAt = nowMs;
      }
      pressed ? PressGrid(GridIndex(row, column)) : ReleaseGrid(GridIndex(row, column));
      return;
    case KeyRole::None:
    default:
      return;
  }
}

// FN + ESC: stop the sequencer, every arpeggiator and every sounding note. (The HOME side key is
// unassigned for now.)
void Controller::AllStop()
{
  ReleaseAllPads();
  host_.Sequencer().ArpReleaseAll();
  host_.Panic();
  state_.panicAt = state_.now == 0 ? 1 : state_.now;
  ShowToast("ALL STOP");
}

void Controller::PressLeft(std::size_t row)
{
  auto& key = state_.left[row];
  key.down = true;
  key.used = false;
  key.suppressed = false;
  key.pressedAt = state_.now;
  const KeyRole role = RoleFor(row, 0);
  // FN + ESC stops everything; FN + TAB goes to the previous track. Neither does its plain job.
  auto& fn = state_.left[LeftIndex(KeyRole::Fn)];
  if (fn.down && (role == KeyRole::Esc || role == KeyRole::Tab))
  {
    fn.used = true;
    key.used = true;
    if (role == KeyRole::Esc) AllStop();
    else if (TopModal() == nullptr)
    {
      SelectTrack((state_.track + kTrackCount - 1) % kTrackCount);
      ShowHud("TRACK", "T" + std::to_string(state_.track + 1));
    }
    return;
  }
  if (role == KeyRole::Ctrl)
  {
    // CTRL steps pages on release (a tap); held, it opens the page map. FN + CTRL steps back.
    if (fn.down && !FnYellowMode())
    {
      fn.used = true;
      key.used = true;
      key.suppressed = true;
      if (MenuOpen()) CloseModal();
      if (TopModal() == nullptr) StepPage(-1);
      return;
    }
    if (TopModal() != nullptr) key.suppressed = true;
  }
}

void Controller::ReleaseLeft(std::size_t row)
{
  auto& key = state_.left[row];
  if (!key.down) return;
  const bool tap = !key.used && state_.now - key.pressedAt < kHoldMs;
  key.down = false;
  switch (RoleFor(row, 0))
  {
    case KeyRole::Esc:
      if (tap) Back();
      break;
    case KeyRole::Tab:
      if (tap && TopModal() == nullptr) SelectTrack((state_.track + 1) % kTrackCount);
      break;
    case KeyRole::Ctrl:
      if (tap && !key.suppressed && TopModal() == nullptr) StepPage(1);
      break;
    case KeyRole::Fn:
      // A tap on FN opens or closes the menu.
      if (tap && MenuOpen()) CloseModal();
      else if (tap && TopModal() == nullptr) OpenMenu();
      break;
    default:
      break;
  }
  key.used = false;
  key.suppressed = false;
}

void Controller::PressRight(KeyRole role)
{
  const bool fn = state_.left[LeftIndex(KeyRole::Fn)].down;
  if (fn) state_.left[LeftIndex(KeyRole::Fn)].used = true;
  const bool tab = state_.left[LeftIndex(KeyRole::Tab)].down;
  auto& ctrl = state_.left[LeftIndex(KeyRole::Ctrl)];
  const bool pageMap = ctrl.down && !ctrl.suppressed && TopModal() == nullptr;
  if (pageMap) ctrl.used = true;
  switch (role)
  {
    case KeyRole::Sub:
    case KeyRole::Add:
    {
      const int direction = role == KeyRole::Sub ? -1 : 1;
      if (pageMap)
      {
        StepPage(direction);
        return;
      }
      if (tab && TopModal() == nullptr)
      {
        state_.left[LeftIndex(KeyRole::Tab)].used = true;
        SelectTrack((state_.track + kTrackCount + static_cast<std::size_t>(direction + static_cast<int>(kTrackCount))) % kTrackCount);
        return;
      }
      // FN + ADD / SUB take bigger steps; in input screens they jump through lists a page at a time.
      state_.repeat.down = true;
      state_.repeat.direction = direction;
      state_.repeat.coarse = fn;
      state_.repeat.count = 0;
      state_.repeat.nextAt = state_.now + kRepeatDelayMs;
      Adjust(direction, state_.repeat.coarse);
      return;
    }
    case KeyRole::Enter:
      // SEQ notes: OK is the record key. A tap switches the view, a hold opens RECORD.
      if (!fn && OkDeferred()) OkPress();
      else fn ? FnEnter() : Enter();
      return;
    case KeyRole::Play:
      fn ? ToggleRecord() : TogglePlay();
      return;
    default:
      return;
  }
}

void Controller::ReleaseRight(KeyRole role)
{
  if (role == KeyRole::Enter) OkRelease();
  if (role == KeyRole::Sub || role == KeyRole::Add)
  {
    const int direction = role == KeyRole::Sub ? -1 : 1;
    if (state_.repeat.direction == direction) state_.repeat = RepeatState{};
  }
}

void Controller::PressPad(std::size_t row, std::size_t column)
{
  // FN + a pad runs its letter function. While ESC, TAB or CTRL is held the GRID chooses and the
  // pads stay silent.
  if (state_.left[LeftIndex(KeyRole::Fn)].down)
  {
    state_.left[LeftIndex(KeyRole::Fn)].used = true;
    HandleFnKey(row, column);
    return;
  }
  for (const KeyRole modifier : {KeyRole::Esc, KeyRole::Tab, KeyRole::Ctrl})
  {
    auto& key = state_.left[LeftIndex(modifier)];
    if (!key.down || (modifier == KeyRole::Ctrl && key.suppressed)) continue;
    key.used = true;
    return;
  }
  if (HandleSearchKey(row, column)) return;
  if (CellPickerActive())
  {
    PickCellValue(row, column);
    return;
  }

  int note = 0;
  if (!PadNoteFor(row, column, note)) return;
  const std::size_t index = PadIndex(row, column);
  if (state_.padNote[index] != kNoNote) return;

  // The scale lock and the chord decide what this key sounds.
  std::array<std::uint8_t, kMaxPadNotes> chord{};
  const std::size_t voices = PadNotes(state_.keyboard, state_.chords[state_.track], ActiveKind(), note, chord);
  if (voices == 0) return;
  auto& sequencer = host_.Sequencer();
  // On a drum track the row says how hard. On the drum page a pad also chooses what the GRID puts on
  // the steps: this drum, this hard.
  const bool drums = ActiveKind() == synth::SynthEngineKind::Drums;
  const std::uint8_t velocity = drums ? kDrumRowVelocity[row] : state_.keyboard.velocity;
  state_.padVelocity[index] = velocity;
  if (drums) state_.drumPadHits[column - kPadFirstColumn] = DrumHit{state_.now == 0 ? 1 : state_.now, static_cast<std::uint8_t>(row), velocity};
  // Selected steps (held or tapped) turn the pads into a note editor for them; on the drum machine a
  // drum goes in as hard as the pad's row, and the pad becomes the drum its GRID view shows.
  bool editing = false;
  if (state_.scene == Scene::Sequence && CurrentPage() == 0 && TopModal() == nullptr)
  {
    if (drums)
    {
      state_.drumLane[state_.track] = static_cast<std::uint8_t>(column - kPadFirstColumn);
      state_.drumVelocity[state_.track] = velocity;
    }
    else state_.noteLane[state_.track] = static_cast<std::uint8_t>(note);
    // The pad's notes as one: a chord goes in whole, or (any of it in a step) comes out whole.
    editing = PadForSequence(chord.data(), voices, true, drums ? velocity : std::uint8_t{0});
  }
  const bool audition = !editing || !sequencer.Playing();
  state_.padNote[index] = static_cast<std::uint8_t>(note);
  state_.padChord[index] = chord;
  state_.padChordCount[index] = static_cast<std::uint8_t>(voices);
  state_.padTrack[index] = static_cast<std::uint8_t>(state_.track);
  // Drums skip the arpeggiator: one hit per press.
  state_.padArp[index] = audition && sequencer.GetArpSettings(state_.track).mode != synth::ArpMode::Off && ActiveKind() != synth::SynthEngineKind::Drums;
  state_.lastTrackNoteAt[state_.track] = state_.now;
  if (!audition)
  {
    state_.padChordCount[index] = 0;  // edit only: nothing sounds, nothing to release
    return;
  }
#if defined(CARDENZA_NATIVE_UI)
  NativePublishPerformance();
#endif
  SoundPad(index, true);
  // The arpeggiator records its own notes; straight notes are recorded here.
  if (!editing && !state_.padArp[index] && sequencer.RecordArmed() && sequencer.Playing())
  {
    for (std::size_t voice = 0; voice < voices; ++voice)
    {
#if defined(CARDENZA_NATIVE_UI)
      if (NativeNoteOwners(state_.track, chord[voice], false) > 1) continue;
#endif
      sequencer.RecordNoteOn(state_.track, chord[voice], velocity);
    }
    MarkDirty();
  }
}

void Controller::ReleasePad(std::size_t row, std::size_t column)
{
  if (row >= kRows || column < kPadFirstColumn || column >= kPadFirstColumn + kPadColumns) return;
  if (state_.repeat.quickKey == KeyIndex(row, column)) state_.repeat = RepeatState{};
  const std::size_t index = PadIndex(row, column);
  if (state_.padNote[index] == kNoNote) return;
#if defined(CARDENZA_NATIVE_UI)
  state_.padNote[index] = kNoNote;
  NativePublishPerformance();
#endif
  SoundPad(index, false);
  state_.padNote[index] = kNoNote;
  state_.padChordCount[index] = 0;
#if defined(CARDENZA_NATIVE_UI)
  NativePublishPerformance();
#endif
}

void Controller::PressGrid(std::size_t index)
{
  if (index >= kGridCount) return;
  const std::size_t row = index / kGridColumns;
  const std::size_t column = kGridFirstColumn + index % kGridColumns;
  // Hold maps. ESC: the GRID columns are the four scenes; column 1 (0 SETUP) is the only way into SETUP.
  auto& esc = state_.left[LeftIndex(KeyRole::Esc)];
  if (esc.down)
  {
    esc.used = true;
    if (index % kGridColumns < kSceneCount) GoToScene(static_cast<Scene>(index % kGridColumns));
    return;
  }
  // TAB: the columns are the tracks, laid out like MIX (select / mute / solo / sequencer).
  auto& tab = state_.left[LeftIndex(KeyRole::Tab)];
  if (tab.down)
  {
    tab.used = true;
    if (TopModal() == nullptr) MixGrid(index);
    return;
  }
  // CTRL: every page of the current scene, in order.
  auto& ctrl = state_.left[LeftIndex(KeyRole::Ctrl)];
  if (ctrl.down && !ctrl.suppressed && TopModal() == nullptr)
  {
    ctrl.used = true;
    if (index < PageCount(state_.scene)) SetPage(static_cast<std::uint8_t>(index));
    return;
  }
  if (state_.left[LeftIndex(KeyRole::Fn)].down)
  {
    state_.left[LeftIndex(KeyRole::Fn)].used = true;
    if (FnOverlay()) state_.menuPress = UiState::MenuPress{static_cast<std::uint8_t>(index), true, 0};
    HandleFnKey(row, column);
    return;
  }
  if (HandleSearchKey(row, column)) return;
  state_.gridDown[index] = true;
  if (TopModal() != nullptr)
  {
    ModalGrid(index);
    return;
  }
  state_.cellPress = CellPress{true, static_cast<std::uint8_t>(index), false, false, state_.now};
  switch (state_.scene)
  {
    case Scene::Instrument: InstrumentGrid(index); break;
    case Scene::Sequence: SequenceGrid(index); break;
    case Scene::Tape:
      if (CurrentPage() == kTapeSession) SessionGrid(index);
      else if (CurrentPage() == kTapeSong) SongGrid(index);
      else if (CurrentPage() == kTapeMix) MixGrid(index);
      else if (CurrentPage() == kTapeSends) SendsGrid(index);
      else FxGrid(index);
      break;
    case Scene::Setup:
      if (CurrentPage() < kSetupKeys) ProjectGrid(index);
      else SettingsGrid(index);
      break;
  }
}

void Controller::ReleaseGrid(std::size_t index)
{
  if (index >= kGridCount || !state_.gridDown[index]) return;
  state_.gridDown[index] = false;
  if (state_.cellPress.down && state_.cellPress.index == index)
  {
    const CellPress press = state_.cellPress;
    state_.cellPress = CellPress{};
    if (press.stepOnRelease && !press.picked && TopModal() == nullptr && state_.now - press.at < kCellPickMs) StepCell(index);
  }
  // A step key settles wherever its release lands (a modal or another scene may be up by now).
  StepKeyRelease(index);
}

bool Controller::HandleTextKey(std::size_t row, std::size_t column)
{
  Modal* modal = Top();
  if (modal == nullptr) return false;
  std::string& text = modal->text;
  std::size_t& cursor = modal->textCursor;
  cursor = std::min(cursor, text.size());
  const char value = LegendChar(row, column);
  // FN + key: the yellow keycap layer. , / move the cursor, ; . jump to the start / end, and the
  // top-right "del" key deletes the character after the cursor.
  if (state_.left[LeftIndex(KeyRole::Fn)].down)
  {
    state_.left[LeftIndex(KeyRole::Fn)].used = true;
    if (value == ',' && cursor > 0) --cursor;
    else if (value == '/' && cursor < text.size()) ++cursor;
    else if (value == ';') cursor = 0;
    else if (value == '.') cursor = text.size();
    else if (row == 0 && column == kColumns - 1 && cursor < text.size()) text.erase(cursor, 1);
    return true;
  }
  // The top-right key carries the "del" legend, so it erases whatever role it has elsewhere.
  if (row == 0 && column == kColumns - 1)
  {
    if (cursor > 0)
    {
      text.erase(cursor - 1, 1);
      --cursor;
    }
    return true;
  }
  switch (RoleFor(row, column))
  {
    case KeyRole::Enter:
      CommitText();
      return true;
    case KeyRole::Add:
    case KeyRole::Sub:
    case KeyRole::Tab:
    case KeyRole::Fn:
    case KeyRole::Ctrl:
      return true;
    default:
      break;
  }
  const bool allowed = (value >= 'A' && value <= 'Z') || (value >= '0' && value <= '9') || value == ' ' || value == '-';
  if (!allowed) return true;
  if (value == ' ' && cursor == 0) return true;
  if (text.size() < modal->maxLength) text.insert(cursor++, 1, value);
  return true;
}

bool Controller::HandleSearchKey(std::size_t row, std::size_t column)
{
  // In the preset / sample browsers the pads and GRID do not type: searching is FN + F, a field one
  // step away, so a stray key never starts a filter.
  (void)row;
  (void)column;
  const Modal* modal = TopModal();
  return modal != nullptr && (modal->kind == ModalKind::PresetBrowser || modal->kind == ModalKind::SampleBrowser);
}

bool Controller::HandleFnKey(std::size_t row, std::size_t column)
{
  const char key = LegendChar(row, column);
  // Input screens: the yellow keycap layer. ; , . / are the arrows.
  if (FnYellowMode())
  {
    switch (key)
    {
      case ';': ArrowKey(Arrow::Up); break;
      case '.': ArrowKey(Arrow::Down); break;
      case ',': ArrowKey(Arrow::Left); break;
      case '/': ArrowKey(Arrow::Right); break;
      case 'F':
      {
        // FN + F: the search field of a preset / sample browser.
        const Modal* browser = TopModal();
        if (browser != nullptr && (browser->kind == ModalKind::PresetBrowser || browser->kind == ModalKind::SampleBrowser))
          OpenTextEntry(TextPurpose::Search, "SEARCH", browser->search, 16);
        break;
      }
      default: break;
    }
    return true;
  }
  // Quick controls: one chord changes BPM, volume or octave from any FN page, repeating while held.
  for (const FnQuickControl& quick : kFnQuickControls)
  {
    if (key != quick.up && key != quick.down) continue;
    const int direction = key == quick.up ? 1 : -1;
    AdjustSetting(quick.id, direction);
    ShowSettingHud(quick.id, quick.label);
    state_.repeat = RepeatState{};
    state_.repeat.down = true;
    state_.repeat.direction = direction;
    state_.repeat.nextAt = state_.now + kRepeatDelayMs;
    state_.repeat.quickId = quick.id;
    state_.repeat.quickKey = KeyIndex(row, column);
    return true;
  }
  // The menu: FN + GRID runs the item on that key, FN + a letter the item that carries it.
  if (RoleFor(row, column) == KeyRole::Grid)
  {
    RunMenu(GridIndex(row, column));
    return true;
  }
  if (key == 0) return true;
  const MenuItems menu = Menu();
  for (std::size_t index = 0; index < menu.size(); ++index)
    if (menu[index].key != nullptr && menu[index].key[0] == key && menu[index].key[1] == 0)
    {
      RunMenu(index);
      break;
    }
  return true;
}

void Controller::ResumeTake(std::size_t track)
{
  state_.track = std::min(track, kTrackCount - 1);
  OpenSampleRecord();
  ShowToast("UNSAVED TAKE IS BACK");
}

void Controller::ReportAutosaveProblem(const std::string& text)
{
  ShowToast(text, ToastKind::Error);
}

void Controller::Tick(std::uint32_t nowMs)
{
  state_.now = nowMs;
  OkTick();
  if (CellPickerActive()) ShowCellHud();
  if (synth::SynthSequencer::ConsumePatternMemoryFull()) ShowToast("MEMORY FULL - NOTE NOT WRITTEN", ToastKind::Error);
  auto& repeat = state_.repeat;
  if (repeat.down && nowMs >= repeat.nextAt)
  {
    ++repeat.count;
    repeat.nextAt = nowMs + kRepeatIntervalMs;
    const int step = repeat.count > kRepeatAccelerateAfter ? 5 : 1;
    if (repeat.quickId != nullptr)
    {
      AdjustSetting(repeat.quickId, repeat.direction * step);
      for (const FnQuickControl& quick : kFnQuickControls)
        if (quick.id == repeat.quickId) ShowSettingHud(quick.id, quick.label);
    }
    else Adjust(repeat.direction * step, repeat.coarse);
  }
  if (!state_.toast.text.empty() && nowMs >= state_.toast.until) state_.toast.text.clear();
  // The record modal follows the sampler's own state machine.
  const Modal* modal = TopModal();
  // The microphone takes the speaker's clock pin, so it never runs without its screen.
  if (const int recording = host_.Sampler().RecordingTrack();
      recording >= 0 && (modal == nullptr || modal->kind != ModalKind::SampleRecord || state_.track != static_cast<std::size_t>(recording)))
  {
    host_.Sampler().CancelRecord(static_cast<std::size_t>(recording));
    ShowToast("RECORDING CANCELLED");
  }
  if (modal != nullptr && modal->kind == ModalKind::SampleRecord)
  {
    const auto sampler = host_.Sampler().BuildUiState(state_.track);
#if defined(CARDENZA_NATIVE_UI)
    if (sampler.saving) nativeSampleSaving_ = true;
    else if (nativeSampleSaving_)
    {
      nativeSampleSaving_ = false;
      if (!sampler.hasDraft && sampler.hasSample)
      {
        CloseModal();
        MarkDirty();
        ShowToast("SAVED " + sampler.sampleName, ToastKind::Ok);
        return;
      }
      ShowToast(sampler.statusText, ToastKind::Error);
    }
#endif
    if (host_.EngineKind(state_.track) != synth::SynthEngineKind::Sampler) CloseModal();
    else if (sampler.needsName && !sampler.overwritePending && sampler.phase == synth::SamplerTrackPhase::Naming)
    {
      // A new recording takes the next free name and no field opens: renaming is in the sample list.
      const std::string name = host_.Sampler().FreeTakeName();
      if (host_.Sampler().CommitDraftName(state_.track, name))
      {
#if defined(CARDENZA_NATIVE_UI)
        if (host_.Sampler().BuildUiState(state_.track).saving)
        {
          nativeSampleSaving_ = true;
          ShowToast("SAVING " + name);
          return;
        }
#endif
        CloseModal();
        MarkDirty();
        ShowToast("SAVED " + name, ToastKind::Ok);
      }
      else
      {
        const std::string problem = host_.Sampler().BuildUiState(state_.track).statusText;
        host_.Sampler().CancelNameEntry(state_.track);
        ShowToast(problem, ToastKind::Error);
      }
    }
  }
  ClampPages();
}

// ---------------------------------------------------------------------------------------------
// Navigation

void Controller::GoToScene(Scene scene)
{
  if (scene == state_.scene) return;
#if defined(CARDENZA_NATIVE_UI)
  NativeReleasePerformance();
  nativeFocusedCell_ = 0xFF;
#endif
  state_.modals.clear();
  host_.Sequencer().ClearSelectedSteps();
  state_.previousScene = state_.scene;
  state_.scene = scene;
  state_.sceneChangedAt = state_.now;
  if (scene == Scene::Setup && CurrentPage() == kSetupFiles) RefreshProjects();
  if (scene == Scene::Tape && CurrentPage() == kTapeMix) state_.mixFocus = static_cast<std::uint8_t>(state_.track);
}

void Controller::SetPage(std::uint8_t page)
{
  const std::size_t count = PageCount(state_.scene);
  if (count == 0) return;
  page = static_cast<std::uint8_t>(std::min<std::size_t>(page, count - 1));
  if (page == CurrentPage()) return;
#if defined(CARDENZA_NATIVE_UI)
  NativeReleasePerformance();
  nativeFocusedCell_ = 0xFF;
#endif
  if (state_.scene == Scene::Sequence) host_.Sequencer().ClearSelectedSteps();
  state_.page[static_cast<std::size_t>(state_.scene)] = page;
  if (state_.scene == Scene::Setup && page == kSetupFiles) RefreshProjects();
  if (state_.scene == Scene::Tape && page == kTapeMix) state_.mixFocus = static_cast<std::uint8_t>(state_.track);
}

void Controller::StepPage(int delta)
{
  const int count = static_cast<int>(PageCount(state_.scene));
  if (count <= 1) return;
  const int next = (static_cast<int>(CurrentPage()) + delta % count + count) % count;
  SetPage(static_cast<std::uint8_t>(next));
}

void Controller::Back()
{
  if (TopModal() != nullptr)
  {
    ModalCancel();
    return;
  }
  auto& sequencer = host_.Sequencer();
  if (state_.scene == Scene::Sequence && sequencer.BuildUiState(state_.track).hasHeldSteps)
  {
    sequencer.ClearSelectedSteps();
    return;
  }
  // SETUP is left the way it was entered: back to the scene it was opened from.
  if (state_.scene == Scene::Setup)
  {
    GoToScene(IsMusicScene(state_.previousScene) ? state_.previousScene : Scene::Instrument);
    return;
  }
  // A tap on ESC walks the music scenes in a ring: INSTR, SEQ, TAPE, INSTR ... (never SETUP).
  GoToScene(NextMusicScene(state_.scene));
}

void Controller::SelectTrack(std::size_t track)
{
  if (track >= kTrackCount || track == state_.track) return;
#if defined(CARDENZA_NATIVE_UI)
  NativeReleasePerformance();
  nativeFocusedCell_ = 0xFF;
#endif
  state_.trackInstrumentPage[state_.track] = state_.page[static_cast<std::size_t>(Scene::Instrument)];
  state_.trackInstrumentFocus[state_.track] = state_.instrumentFocus;
  state_.track = track;
  state_.page[static_cast<std::size_t>(Scene::Instrument)] = state_.trackInstrumentPage[track];
  state_.instrumentFocus = state_.trackInstrumentFocus[track];
  host_.Sequencer().SetActiveTrack(track);
  host_.Sequencer().ClearSelectedSteps();
  if (state_.mixFocus < kTrackCount) state_.mixFocus = static_cast<std::uint8_t>(track);
  state_.clipboardValid = state_.clipboardValid && !state_.clipboardIsPattern;
  ClampPages();
}

void Controller::Adjust(int delta, bool coarse)
{
  if (TopModal() != nullptr)
  {
    ModalAdjust(delta, coarse);
    return;
  }
#if defined(CARDENZA_NATIVE_UI)
  if (state_.scene == Scene::Tape && CurrentPage() == kTapeMix && nativeFocusedCell_ < kGridCount && nativeFocusedCell_ / kGridColumns != 0)
  {
    const std::size_t track = nativeFocusedCell_ % kGridColumns;
    const std::size_t row = nativeFocusedCell_ / kGridColumns;
    const bool value = delta > 0;
    bool changed = false;
    if (row == 1 && host_.TrackMuted(track) != value) { host_.SetTrackMuted(track, value); changed = true; }
    else if (row == 2 && host_.TrackSoloed(track) != value) { host_.SetTrackSoloed(track, value); changed = true; }
    else if (row == 3 && host_.Sequencer().TrackEnabled(track) != value) { host_.Sequencer().SetTrackEnabled(track, value); changed = true; }
    if (changed) MarkDirty();
    return;
  }
#endif
  switch (state_.scene)
  {
    case Scene::Instrument: InstrumentAdjust(delta, coarse); break;
    case Scene::Sequence: SequenceAdjust(delta, coarse); break;
    case Scene::Tape:
      // ADD is the upper key: up the list, as everywhere (AdjustKeysMean).
      if (CurrentPage() == kTapeSession) SessionAdjust(-delta);
      else if (CurrentPage() == kTapeSong) SongAdjust(delta, coarse);
      else if (CurrentPage() == kTapeMix) MixAdjust(delta, coarse);
      else if (CurrentPage() == kTapeSends) SendsAdjust(delta, coarse);
      else FxAdjust(delta, coarse);
      break;
    case Scene::Setup:
      if (CurrentPage() < kSetupKeys) ProjectAdjust(delta, coarse);
      else SettingsAdjust(delta, coarse);
      break;
  }
}

AdjustMeaning Controller::AdjustKeysMean() const
{
  if (const Modal* modal = TopModal())
  {
    switch (modal->kind)
    {
      case ModalKind::PresetBrowser:
      case ModalKind::SampleBrowser:
      case ModalKind::SampleOptions:
        return AdjustMeaning::List;
      case ModalKind::EnginePicker:
        return AdjustMeaning::Sideways;
      case ModalKind::TextEntry:
        return AdjustMeaning::Erase;
      case ModalKind::Record:
      case ModalKind::SampleRecord:
        return AdjustMeaning::Value;
      default:
        return AdjustMeaning::None;
    }
  }
  // The page map and the track map step through what they show.
  const auto& ctrl = state_.left[LeftIndex(KeyRole::Ctrl)];
  if ((ctrl.down && !ctrl.suppressed) || state_.left[LeftIndex(KeyRole::Tab)].down) return AdjustMeaning::Sideways;
  switch (state_.scene)
  {
    case Scene::Instrument:
      return CurrentInstrumentPage()->graphic == PageGraphic::Voice ? AdjustMeaning::List : AdjustMeaning::Value;
    case Scene::Sequence:
      // The note page with nothing selected: ADD / SUB flip the GRID's page.
      if (CurrentPage() == 0 && !host_.Sequencer().HasHeldSteps()) return AdjustMeaning::Sideways;
      return AdjustMeaning::Value;
    case Scene::Tape:
      return CurrentPage() == kTapeSession ? AdjustMeaning::List : AdjustMeaning::Value;
    case Scene::Setup:
    default:
      return CurrentPage() == kSetupFiles ? AdjustMeaning::List : AdjustMeaning::Value;
  }
}

void Controller::Enter()
{
  if (TopModal() != nullptr)
  {
    ModalEnter();
    return;
  }
  switch (state_.scene)
  {
    case Scene::Instrument: InstrumentEnter(); break;
    case Scene::Sequence: SequenceEnter(); break;
    case Scene::Tape:
      if (CurrentPage() == kTapeSession) SessionLaunchRow();
      else if (CurrentPage() == kTapeSong) SongEnter();
      else if (CurrentPage() == kTapeMix) MixEnter();
      else if (CurrentPage() == kTapeSends) SendsEnter();
      else FxEnter();
      break;
    case Scene::Setup:
      if (CurrentPage() < kSetupKeys) ProjectEnter();
      else SettingsEnter();
      break;
  }
}

void Controller::FnEnter()
{
  if (TopModal() != nullptr)
  {
    ModalFnEnter();
    return;
  }
  if (state_.scene == Scene::Instrument) InstrumentReset();
  else if (state_.scene == Scene::Sequence && CurrentPage() == 0) host_.Sequencer().CycleNoteScope();
  else if (state_.scene == Scene::Tape && CurrentPage() == kTapeSong) SongFnEnter();
  else if (state_.scene == Scene::Tape && CurrentPage() == kTapeSession) SessionCapture();
  else if (state_.scene == Scene::Tape && CurrentPage() == kTapeSends) SendsReset();
  else if (state_.scene == Scene::Tape && CurrentPage() == kTapeFx) FxReset();
  else if (state_.scene == Scene::Setup && CurrentPage() >= kSetupKeys) SettingsReset();
}

void Controller::TogglePlay()
{
  const Modal* modal = TopModal();
  if (modal != nullptr && modal->kind == ModalKind::SampleRecord)
  {
    host_.PreviewSample(state_.track);
    return;
  }
  auto& sequencer = host_.Sequencer();
  if (sequencer.Playing())
  {
    sequencer.Stop();
    return;
  }
  sequencer.Play();
}

void Controller::ToggleRecord()
{
  auto& sequencer = host_.Sequencer();
  const bool armed = !sequencer.RecordArmed();
  sequencer.SetRecordArmed(armed);
  ShowToast(armed ? "REC ARMED" : "REC OFF", armed ? ToastKind::Error : ToastKind::Info);
}

void Controller::ArrowKey(Arrow arrow)
{
  const int vertical = arrow == Arrow::Up ? -1 : arrow == Arrow::Down ? 1 : 0;
  const int horizontal = arrow == Arrow::Left ? -1 : arrow == Arrow::Right ? 1 : 0;
  Modal* modal = Top();
  if (modal != nullptr)
  {
    if (vertical != 0) ModalAdjust(vertical, false);
    else if (modal->kind == ModalKind::SampleRecord) modal->trimMarker = horizontal < 0 ? 0 : 1;
    return;
  }
  switch (state_.scene)
  {
    case Scene::Setup:
      if (CurrentPage() == kSetupFiles) ProjectAdjust(vertical == 0 ? horizontal : vertical, false);
      else if (CurrentPage() >= kSetupKeys) SettingsArrow(vertical != 0 ? vertical * static_cast<int>(kGridColumns) : horizontal);
      break;
    case Scene::Tape:
      if (CurrentPage() == kTapeSong) SongArrow(horizontal, vertical);
      else if (CurrentPage() == kTapeSession)
      {
        if (vertical != 0) SessionAdjust(vertical);
        else state_.sessionTrack = (state_.sessionTrack + kTrackCount + static_cast<std::size_t>(horizontal + static_cast<int>(kTrackCount))) % kTrackCount;
      }
      else if (CurrentPage() == kTapeSends)
      {
        const int step = vertical != 0 ? vertical * static_cast<int>(kGridColumns) : horizontal;
        SendsGrid(static_cast<std::size_t>((state_.sendsFocus + step + static_cast<int>(kGridCount)) % static_cast<int>(kGridCount)));
      }
      else if (CurrentPage() == kTapeFx)
      {
        const int count = static_cast<int>(synth::fx::kMasterParamCount);
        const int step = vertical != 0 ? vertical * static_cast<int>(kGridColumns) : horizontal;
        state_.fxFocus = static_cast<std::uint8_t>((state_.fxFocus + step + count) % count);
      }
      else if (CurrentPage() == kTapeMix)
      {
        if (horizontal != 0)
        {
          state_.mixFocus = static_cast<std::uint8_t>((state_.mixFocus + 5 + horizontal) % 5);
          if (state_.mixFocus < kTrackCount) SelectTrack(state_.mixFocus);
        }
        else MixAdjust(-vertical, false);
      }
      break;
    case Scene::Instrument:
    {
      const InstrumentPage* page = CurrentInstrumentPage();
      const int step = vertical != 0 ? vertical * static_cast<int>(kGridColumns) : horizontal;
      int focus = InstrumentFocus();
      for (std::size_t attempt = 0; attempt < kGridCount; ++attempt)
      {
        focus = (focus + step + static_cast<int>(kGridCount)) % static_cast<int>(kGridCount);
        if (page->slots[static_cast<std::size_t>(focus)].IsParameter()) break;
      }
      if (page->slots[static_cast<std::size_t>(focus)].IsParameter())
        state_.instrumentFocus[std::min<std::size_t>(CurrentPage(), state_.instrumentFocus.size() - 1)] = static_cast<std::uint8_t>(focus);
      break;
    }
    case Scene::Sequence:
    {
      if (CurrentPage() != 0) break;
      auto& sequencer = host_.Sequencer();
      const auto ui = sequencer.BuildUiState(state_.track);
      const int step = vertical != 0 ? vertical * static_cast<int>(kGridColumns) : horizontal;
      const int length = static_cast<int>(std::max<std::size_t>(kGridCount, ui.tracks[state_.track].stepCount));
      const int from = ui.hasHeldSteps ? static_cast<int>(ui.primaryHeldStep) : static_cast<int>(ui.firstViewStep) - step;
      const int next = (from + step + length) % length;
      sequencer.ClearSelectedSteps();
      sequencer.SetStepSelected(static_cast<std::size_t>(next), true);
      // Walking off the page takes the GRID along.
      if (next / static_cast<int>(kGridCount) != static_cast<int>(ui.viewPage)) sequencer.SetViewPage(state_.track, static_cast<std::size_t>(next) / kGridCount);
      break;
    }
  }
}

// ---------------------------------------------------------------------------------------------
// LEDs
//
// Key levels are perceived brightness, gamma-corrected by the device (Devices/LedLight.h); the UI uses
// three of them, far apart (kLedOff / kLedHalf / kLedFull in GrooveController.h):
//   off   nothing there: an empty step, a key that cannot sound, a function not available here
//   half  something there, not chosen: a step with notes, a playable key, an option, an action
//   full  chosen or happening: the selection or focus, a held or sounding key, what is playing
// Blink (full / off) is the only effect: something waiting (a queued pattern, a cued row, a
// confirmation).

namespace {

// A press on a key without a held state (ENTER, PLAY) shows full this long.
constexpr std::uint32_t kLedPressFlashMs = 120;

}  // namespace

void Controller::ComputeLeds(std::array<std::uint8_t, kKeyCount>& levels) const
{
  LedFrame frame{};
  ComputeLeds(frame);
  for (std::size_t index = 0; index < kKeyCount; ++index) levels[index] = frame[index].level;
}

void Controller::ComputeLeds(LedFrame& frame) const
{
  ComputeSceneLeds(frame);
  // The guide's hint, as the lesson page shows it (the keys to press framed, the rest dimmed): those
  // keys full, every other key no brighter than half. They blink (slowly) until the step is learned,
  // then stay lit while the user tries more of them.
  if (guideHint_ == 0) return;
  for (std::size_t index = 0; index < kKeyCount; ++index)
  {
    if ((guideHint_ >> index) & 1u) frame[index] = LedCell{kLedFull, guideHintWaits_ ? LedEffect::Hint : LedEffect::Steady};
    else frame[index].level = std::min(frame[index].level, kLedHalf);
  }
}

void Controller::ComputeSceneLeds(LedFrame& frame) const
{
  frame.fill(LedCell{});
  const auto& sequencer = host_.Sequencer();
  const auto seq = sequencer.BuildUiState(state_.track);
  const Modal* modal = TopModal();
  const bool textEntry = modal != nullptr && modal->kind == ModalKind::TextEntry;
  const bool menuOpen = modal != nullptr && modal->kind == ModalKind::Menu;
  const bool searching = modal != nullptr && (modal->kind == ModalKind::PresetBrowser || modal->kind == ModalKind::SampleBrowser);
  const std::size_t page = CurrentPage();
  auto set = [&](std::size_t index, std::uint8_t level, LedEffect effect = LedEffect::Steady) {
    frame[index] = LedCell{level, effect};
  };
  // half or full: an option that is (or is not) the chosen one.
  auto choice = [](bool chosen) { return chosen ? kLedFull : kLedHalf; };
  auto gridKey = [](std::size_t index) { return KeyIndex(index / kGridColumns, kGridFirstColumn + index % kGridColumns); };
  auto setGrid = [&](std::size_t index, std::uint8_t level, LedEffect effect = LedEffect::Steady) {
    if (state_.gridDown[index]) set(gridKey(index), kLedFull);
    else set(gridKey(index), level, effect);
  };
  const std::size_t addKey = KeyIndex(0, kColumns - 1);
  const std::size_t subKey = KeyIndex(1, kColumns - 1);
  const std::size_t enterKey = KeyIndex(2, kColumns - 1);
  const std::size_t playKey = KeyIndex(3, kColumns - 1);
  // Keys without a held state flash full when pressed; held keys stay full.
  auto finish = [&]() {
    for (const std::size_t index : {enterKey, playKey})
    {
      const std::uint32_t pressed = state_.keyPressedAt[index];
      if (pressed != 0 && state_.now - pressed < kLedPressFlashMs) set(index, kLedFull);
    }
    if (state_.repeat.down && state_.repeat.quickId == nullptr) set(state_.repeat.direction > 0 ? addKey : subKey, kLedFull);
    for (std::size_t row = 0; row < kRows; ++row)
      if (state_.left[row].down) set(KeyIndex(row, 0), kLedFull);
  };

  // Left keys. ESC always goes back; TAB and CTRL work on screens, not in pop-ups; FN is full while
  // the menu is open.
  set(KeyIndex(0, 0), kLedHalf);
  set(KeyIndex(1, 0), modal == nullptr ? kLedHalf : kLedOff);
  set(KeyIndex(2, 0), menuOpen ? kLedFull : kLedHalf);
  set(KeyIndex(3, 0), modal == nullptr && PageCount(state_.scene) > 1 ? kLedHalf : kLedOff);

  // Right keys: ADD / SUB when there is something to change, ENTER when it does something, PLAY
  // the transport (half stopped, full playing, blinking while recording is armed).
  bool adjustable = modal != nullptr;
  bool enterAction = modal != nullptr;
  switch (state_.scene)
  {
    case Scene::Instrument:
    {
      const InstrumentPage* instrumentPage = CurrentInstrumentPage();
      adjustable = adjustable || instrumentPage->graphic == PageGraphic::Voice || instrumentPage->slots[InstrumentFocus()].IsParameter();
      enterAction = true;
      break;
    }
    case Scene::Sequence:
      adjustable = true;
      enterAction = true;  // the note page's OK records (tap: view, hold: RECORD)
      break;
    case Scene::Tape:
      adjustable = true;
      enterAction = true;
      break;
    case Scene::Setup:
      adjustable = adjustable || page != kSetupFiles || !state_.projects.empty();
      enterAction = enterAction || page != kSetupFiles || !state_.projects.empty();
      break;
  }
  set(addKey, adjustable ? kLedHalf : kLedOff);
  set(subKey, adjustable ? kLedHalf : kLedOff);
  if (modal != nullptr && modal->kind == ModalKind::Confirm) set(enterKey, kLedFull, LedEffect::Blink);
  else if (modal == nullptr && state_.scene == Scene::Sequence && page == 0) set(enterKey, choice(View() == SeqView::Step));
  else set(enterKey, enterAction ? kLedHalf : kLedOff);
  if (sequencer.RecordArmed()) set(playKey, kLedFull, LedEffect::Blink);
  else set(playKey, choice(seq.playing));

  if (textEntry)
  {
    // Typing: the keyboard is the keyboard. Every key that types (A-Z, 0-9, -, space) is half, as
    // are erase (top right "del"), ENTER and ESC; TAB, FN, CTRL and keys that type nothing are off.
    // A key flashes full as it types.
    for (std::size_t row = 0; row < kRows; ++row)
      for (std::size_t column = 0; column < kColumns; ++column)
      {
        const char value = LegendChar(row, column);
        const bool types = (value >= 'A' && value <= 'Z') || (value >= '0' && value <= '9') || value == '-';
        set(KeyIndex(row, column), types ? kLedHalf : kLedOff);
      }
    for (const std::size_t index : {KeyIndex(0, 0), addKey, enterKey, playKey}) set(index, kLedHalf);
    for (std::size_t index = 0; index < kKeyCount; ++index)
    {
      const std::uint32_t pressed = state_.keyPressedAt[index];
      if (frame[index].level > 0 && pressed != 0 && state_.now - pressed < kLedPressFlashMs) set(index, kLedFull);
    }
    return;
  }

  // Hold maps: only the held key and the keys the map accepts are lit; pads are silent and dark.
  const bool fnHeld = state_.left[2].down;
  const bool anyMap = EscOverlay() || TabOverlay() || CtrlOverlay() || fnHeld;
  if (anyMap)
  {
    const std::size_t held = EscOverlay() ? 0 : TabOverlay() ? 1 : CtrlOverlay() ? 3 : 2;
    for (std::size_t row = 0; row < kRows; ++row)
    {
      if (row != held) set(KeyIndex(row, 0), kLedOff);
      set(KeyIndex(row, kColumns - 1), kLedOff);
      for (std::size_t column = kPadFirstColumn; column < kColumns - 1; ++column) set(KeyIndex(row, column), kLedOff);
    }
    // TAB and CTRL also step with ADD / SUB.
    if (held == 1 || held == 3)
    {
      set(addKey, kLedHalf);
      set(subKey, kLedHalf);
    }
  }
  if (EscOverlay())
  {
    // One column per scene: the current one full, the others half (column 1 is SETUP).
    for (std::size_t index = 0; index < kGridCount; ++index)
      if (index % kGridColumns < kSceneCount) setGrid(index, choice(index % kGridColumns == static_cast<std::size_t>(state_.scene)));
    finish();
    return;
  }
  // The track map and MIX share a layout: select, mute, solo, sequencer on.
  auto trackRows = [&](std::size_t focus) {
    for (std::size_t track = 0; track < kTrackCount; ++track)
    {
      setGrid(track, choice(track == focus));
      setGrid(4 + track, choice(host_.TrackMuted(track)));
      setGrid(8 + track, choice(host_.TrackSoloed(track)));
      setGrid(12 + track, choice(seq.tracks[track].enabled));
    }
  };
  if (TabOverlay())
  {
    trackRows(state_.track);
    finish();
    return;
  }
  if (CtrlOverlay())
  {
    const std::size_t count = std::min(PageCount(state_.scene), kGridCount);
    for (std::size_t index = 0; index < count; ++index) setGrid(index, choice(index == page));
    finish();
    return;
  }
  if (fnHeld && FnYellowMode())
  {
    // Yellow keycap layer in input screens: only the arrows, and ADD / SUB a page at a time.
    for (std::size_t row = 0; row < kRows; ++row)
      for (std::size_t column = kPadFirstColumn; column < kColumns - 1; ++column)
      {
        const char key = LegendChar(row, column);
        set(KeyIndex(row, column), key == ';' || key == ',' || key == '.' || key == '/' ? kLedHalf : kLedOff);
      }
    set(addKey, kLedHalf);
    set(subKey, kLedHalf);
    finish();
    return;
  }
  // The screen has an FN + ENTER (a second ENTER: default, scope, capture, song / loop).
  const bool screenFnEnter = state_.scene == Scene::Instrument || (state_.scene == Scene::Sequence && page == 0) ||
                             (state_.scene == Scene::Tape && page != kTapeMix) || (state_.scene == Scene::Setup && page >= kSetupKeys);
  // The menu's items on the GRID: half, a switch that is on full, nothing there off.
  auto menuGrid = [&]() {
    const MenuItems menu = Menu();
    for (std::size_t index = 0; index < kGridCount; ++index)
      if (menu[index].action != MenuAction::None && menu[index].available) setGrid(index, choice(menu[index].on));
    return menu;
  };
  if (fnHeld)
  {
    // The menu. FN + a letter runs the item that carries it, so those pads are half, as are the quick
    // controls; other pads are off. ESC = stop, TAB = previous track, CTRL = previous page, ADD / SUB
    // bigger steps, PLAY = record, ENTER where the screen has an FN + ENTER.
    set(addKey, kLedHalf);
    set(subKey, kLedHalf);
    set(enterKey, screenFnEnter ? kLedHalf : kLedOff);
    set(playKey, sequencer.RecordArmed() ? kLedFull : kLedHalf);
    set(KeyIndex(0, 0), kLedHalf);
    set(KeyIndex(1, 0), kLedHalf);
    set(KeyIndex(3, 0), PageCount(state_.scene) > 1 ? kLedHalf : kLedOff);
    const MenuItems menu = menuGrid();
    for (std::size_t row = 0; row < kRows; ++row)
      for (std::size_t column = kPadFirstColumn; column < kPadFirstColumn + kPadColumns; ++column)
      {
        const char key = LegendChar(row, column);
        std::uint8_t level = kLedOff;
        for (const MenuItem& item : menu)
          if (item.available && item.key != nullptr && key != 0 && item.key[0] == key && item.key[1] == 0) level = choice(item.on);
        for (const FnQuickControl& quick : kFnQuickControls)
          if (key == quick.up || key == quick.down) level = choice(state_.repeat.quickKey == KeyIndex(row, column));
        set(KeyIndex(row, column), level);
      }
    finish();
    return;
  }

  // Pads: the keyboard of the current track. A key that sounds is half, a held or sounding key
  // full (with the other tones of its chord and the arpeggiator's note); a key that cannot sound
  // (PIANO gaps, keys the scale lock mutes) is off. With a step selected in SEQ the pads edit it: its
  // notes (or drums) are full; playing with no step selected, the notes of the step under the
  // playhead are full.
  const bool chordEditor = state_.scene == Scene::Sequence && page == 0 && modal == nullptr && seq.hasHeldSteps;
  std::array<bool, 128> lit{};
  if (chordEditor)
  {
    std::array<std::uint8_t, 8> notes{};
    const std::size_t count = sequencer.StepNotes(seq.primaryHeldStep, notes);
    for (std::size_t index = 0; index < count; ++index) lit[notes[index] & 0x7F] = true;
  }
  else if (state_.scene == Scene::Sequence && page == 0 && modal == nullptr && seq.playing)
  {
    std::array<std::uint8_t, 8> notes{};
    const std::size_t count = sequencer.StepNotes(seq.currentStep, notes);
    for (std::size_t index = 0; index < count; ++index) lit[notes[index] & 0x7F] = true;
  }
  if (seq.arpActive && seq.arpTrack == state_.track && seq.arpSounding >= 0 && seq.arpSounding < 128)
    lit[static_cast<std::size_t>(seq.arpSounding)] = true;
  const auto kind = ActiveKind();
  const KeyboardSettings& keys = state_.keyboard;
  // A drum sounding (or chosen) shows how hard as a bar up its column: from the bottom, softest, to
  // the row of its velocity. A top-row hit lights the whole column, a bottom-row hit one key.
  std::array<std::uint8_t, kPadColumns> drumBar{};
  if (kind == synth::SynthEngineKind::Drums)
  {
    const auto raiseBar = [&](std::size_t lane, std::uint8_t velocity) { drumBar[lane] = std::max(drumBar[lane], velocity); };
    if (chordEditor || (state_.scene == Scene::Sequence && page == 0 && modal == nullptr && seq.playing))
    {
      std::array<synth::SynthSequencer::StepNoteView, 8> notes{};
      const std::size_t step = chordEditor ? seq.primaryHeldStep : seq.currentStep;
      const std::size_t count = sequencer.StepNoteEvents(state_.track, step, notes);
      for (std::size_t slot = 0; slot < count; ++slot)
        for (std::size_t lane = 0; lane < kDrumNotes.size(); ++lane)
          if (kDrumNotes[lane] == notes[slot].note) raiseBar(lane, notes[slot].velocity);
    }
    for (std::size_t pad = 0; pad < state_.padNote.size(); ++pad)
      if (state_.padNote[pad] != kNoNote && state_.padTrack[pad] == state_.track) raiseBar(pad % kPadColumns, state_.padVelocity[pad]);
    // The drum the pads chose, while no step is selected (a selected step shows its own drums).
    if (state_.scene == Scene::Sequence && page == 0 && modal == nullptr && !chordEditor) raiseBar(DrumLane(), state_.drumVelocity[state_.track]);
  }
  for (std::size_t row = 0; row < kRows; ++row)
    for (std::size_t column = kPadFirstColumn; column < kPadFirstColumn + kPadColumns; ++column)
    {
      const std::size_t index = KeyIndex(row, column);
      if (searching)
      {
        // The browser search field: letters and digits type.
        const char key = LegendChar(row, column);
        set(index, (key >= 'A' && key <= 'Z') || (key >= '0' && key <= '9') || key == '-' ? kLedHalf : kLedOff);
        continue;
      }
      if (IsPadDown(row, column))
      {
        set(index, kLedFull);
        continue;
      }
      int note = 0;
      if (!PadNoteFor(row, column, note)) continue;
      const bool melodic = kind != synth::SynthEngineKind::Drums;
      if (melodic && keys.lock == ScaleLock::Mute && !InScale(keys, note)) continue;
      if (!melodic)
      {
        const std::uint8_t bar = drumBar[column - kPadFirstColumn];
        set(index, choice(bar != 0 && row >= DrumRowForVelocity(bar)));
        continue;
      }
      const bool sounding = lit[static_cast<std::size_t>(note)] || IsChordNoteHeld(note);
      set(index, choice(sounding));
    }

  // GRID: the 4x4 cells on screen.
  if (modal != nullptr)
  {
    if (modal->kind == ModalKind::SampleRecord)
      for (std::size_t index = 0; index < 4; ++index) setGrid(index, choice(index < 2 && modal->trimMarker == index));
    if (modal->kind == ModalKind::EnginePicker)
      for (std::size_t index = 0; index < modal->items.size() && index < 4; ++index) setGrid(index, choice(index == modal->cursor));
    if (modal->kind == ModalKind::Record)
    {
      const RecordState& rec = state_.record;
      setGrid(kRecordRec, choice(sequencer.RecordArmed() || rec.focus == kRecordRec));
      setGrid(kRecordSnap, choice(rec.snap || rec.focus == kRecordSnap));
      setGrid(kRecordView, choice(rec.focus == kRecordView));
      for (std::size_t order = 0; order < kCaptureTakes; ++order)
        if (TakeAt(order) != nullptr) setGrid(kRecordFirstTake + order, choice(rec.focus == kRecordFirstTake + order));
    }
    if (menuOpen)
    {
      // The open menu takes only the GRID.
      (void)menuGrid();
      for (const std::size_t index : {addKey, subKey, enterKey}) set(index, kLedOff);
    }
    finish();
    return;
  }
  switch (state_.scene)
  {
    case Scene::Instrument:
    {
      // Parameters and actions half, the focused parameter full, empty slots off.
      const InstrumentPage* instrumentPage = CurrentInstrumentPage();
      for (std::size_t index = 0; index < kGridCount; ++index)
      {
        const Slot& slot = instrumentPage->slots[index];
        if (slot.IsAction()) setGrid(index, kLedHalf);
        // HOLD is a performance switch: on shows on, wherever the focus is.
        else if (slot.IsParameter()) setGrid(index, choice(index == InstrumentFocus() || (std::string_view(slot.id) == "arp.latch" && sequencer.GetArpSettings(state_.track).latch)));
      }
      break;
    }
    case Scene::Sequence:
      if (page == 0)
      {
        // Steps, every view and the drum machine alike: empty off, with notes half, selected full; the
        // playhead runs full. With pads held (and nothing selected) the steps that hold their notes
        // (drums) are full too: a step tapped now takes those out.
        const std::size_t steps = seq.tracks[state_.track].stepCount;
        {
          std::array<std::uint8_t, kRows * kPadColumns> heldNotes{};
          const std::size_t heldCount = seq.hasHeldSteps ? 0 : CollectHeldNotes(heldNotes);
          for (std::size_t index = 0; index < kGridCount && seq.firstViewStep + index < steps; ++index)
          {
            const auto& step = seq.steps[index];
            bool holdsHeld = false;
            if (heldCount != 0 && step.hasEvents)
            {
              std::array<synth::SynthSequencer::StepNoteView, 8> notes{};
              const std::size_t count = sequencer.StepNoteEvents(state_.track, seq.firstViewStep + index, notes);
              for (std::size_t slot = 0; slot < count && !holdsHeld; ++slot)
                holdsHeld = std::find(heldNotes.begin(), heldNotes.begin() + static_cast<std::ptrdiff_t>(heldCount), notes[slot].note) !=
                            heldNotes.begin() + static_cast<std::ptrdiff_t>(heldCount);
            }
            if (step.held || step.playing || holdsHeld) setGrid(index, kLedFull);
            else if (step.hasEvents) setGrid(index, kLedHalf);
          }
        }
      }
      else if (page == 1)
      {
        // Patterns: the playing one full, the others half, a queued one blinks.
        const auto& track = seq.tracks[state_.track];
        for (std::size_t index = 0; index < kGridCount && index < track.patternCount; ++index)
        {
          if (index == track.queuedPattern) setGrid(index, kLedFull, LedEffect::Blink);
          else setGrid(index, choice(index == track.activePattern));
        }
      }
      else
      {
        for (std::size_t index = 0; index < 4; ++index) setGrid(index, choice(index == state_.seqTrackFocus));
      }
      break;
    case Scene::Tape:
      if (page == kTapeSession)
      {
        // Clips: each track's current pattern full, the other patterns half, queued blink.
        for (std::size_t index = 0; index < kGridCount; ++index)
        {
          const std::size_t track = index % kGridColumns;
          const std::size_t pattern = state_.sessionScroll + index / kGridColumns;
          const auto& summary = seq.tracks[track];
          if (pattern >= summary.patternCount) continue;
          if (pattern == summary.queuedPattern) setGrid(index, kLedFull, LedEffect::Blink);
          else setGrid(index, choice(pattern == summary.activePattern));
        }
      }
      else if (page == kTapeSong)
      {
        // Arrangement: a cell with a pattern half, a silent one (--) off; the selected cell and the
        // playing row full, a cued row blinks.
        for (std::size_t column = 0; column < kGridColumns; ++column)
        {
          const std::size_t row = state_.songScroll + column;
          if (row >= seq.songLength) continue;
          const auto songRow = sequencer.SongRowAt(row);
          const bool playingRow = seq.songPlaying && row == seq.songRow;
          for (std::size_t track = 0; track < kTrackCount; ++track)
          {
            const std::size_t index = track * kGridColumns + column;
            const bool sounds = songRow.patterns[track] >= 0;
            if (row == state_.songRow && track == state_.songTrack) setGrid(index, kLedFull);
            else if (seq.songCueRow == row && sounds) setGrid(index, kLedFull, LedEffect::Blink);
            else if (sounds) setGrid(index, choice(playingRow));
          }
        }
      }
      else if (page == kTapeMix)
      {
        trackRows(state_.mixFocus);
      }
      else if (page == kTapeSends)
      {
        // Sends: on half, off off, the focused one full.
        for (std::size_t index = 0; index < kGridCount; ++index)
        {
          if (index == state_.sendsFocus)
          {
            setGrid(index, kLedFull);
            continue;
          }
          const synth::ISynthEngine* trackEngine = host_.Engine(index % kGridColumns);
          synth::SynthParameterInfo info;
          if (trackEngine != nullptr && trackEngine->GetParameterInfo(kSendIds[index / kGridColumns], info) && info.value != info.defaultValue)
            setGrid(index, kLedHalf);
        }
      }
      else if (page == kTapeFx)
      {
        for (std::size_t param = 0; param < synth::fx::kMasterParamCount && param < kGridCount; ++param)
          setGrid(param, choice(param == state_.fxFocus));
      }
      break;
    case Scene::Setup:
      if (page == kSetupProject)
      {
        // SAVE / LOAD / NEW and TAP are actions, BPM and SWING values; PLAY is a switch (full in SONG
        // mode).
        for (const std::size_t action : {std::size_t{0}, std::size_t{1}, std::size_t{2}, std::size_t{6}}) setGrid(action, kLedHalf);
        setGrid(4, choice(state_.projectFocus == 4));
        setGrid(5, choice(state_.projectFocus == 5));
        setGrid(7, choice(sequencer.SongMode()));
      }
      else if (const InstrumentPage* settings = SettingsPageShown())
      {
        for (std::size_t index = 0; index < kGridCount; ++index)
          if (settings->slots[index].IsParameter()) setGrid(index, choice(index == SettingsFocus()));
        // KEY LED focused: the pads show every brightness the keys can give, dark to full left to
        // right, to judge the setting (and the LEDs) by eye. Perceived levels: after the gamma they are
        // the HAL duties 0, 1, 2, 3, 4, 7, 10, 15 (Devices/LedLight.h).
        const char* focusId = settings->slots[SettingsFocus()].id;
        if (focusId != nullptr && std::string_view(focusId) == "sys.led")
        {
          static constexpr std::array<std::uint8_t, kPadColumns> kRamp{{0, 6, 7, 8, 9, 11, 13, 15}};
          for (std::size_t row = 0; row < kRows; ++row)
            for (std::size_t column = 0; column < kPadColumns; ++column) set(KeyIndex(row, kPadFirstColumn + column), kRamp[column]);
        }
      }
      break;
  }
  // A parameter's cell held: the pads are its values, the current one full.
  if (CellPickerActive())
  {
    std::string label, text;
    synth::SynthParameterInfo info;
    (void)CellParameter(state_.cellPress.index, label, info, text);
    const std::size_t count = CellPickCount(info);
    const std::size_t current = CellPickPad(info);
    for (std::size_t row = 0; row < kRows; ++row)
      for (std::size_t column = 0; column < kPadColumns; ++column)
      {
        const std::size_t pad = row * kPadColumns + column;
        set(KeyIndex(row, kPadFirstColumn + column), pad >= count ? kLedOff : pad == current ? kLedFull : kLedHalf);
      }
  }
  finish();
}

}  // namespace groove
