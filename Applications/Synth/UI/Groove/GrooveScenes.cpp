#include "Applications/Synth/UI/Groove/GrooveController.h"
#include "OS/Storage/DeviceSettings.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace groove {

namespace {

constexpr std::size_t kProjectSave = 0;
constexpr std::size_t kProjectLoad = 1;
constexpr std::size_t kProjectNew = 2;
constexpr std::size_t kProjectBpm = 4;
constexpr std::size_t kProjectSwing = 5;
constexpr std::size_t kProjectTap = 6;
constexpr std::size_t kProjectMode = 7;

constexpr std::size_t kTrackSeq = 0;
constexpr std::size_t kTrackSteps = 1;
constexpr std::size_t kTrackSwing = 2;
constexpr std::size_t kTrackBpm = 3;

bool StartsWith(const char* text, const char* prefix)
{
  return text != nullptr && std::strncmp(text, prefix, std::strlen(prefix)) == 0;
}

std::string PatternLabel(std::size_t index)
{
  char text[8];
  std::snprintf(text, sizeof(text), "P%02u", static_cast<unsigned>(index + 1));
  return text;
}

// The sound's name with the first number no other sound has.
std::string FreePresetName(const synth::ISynthEngine& engine)
{
  std::vector<std::string> taken;
  constexpr std::size_t kPage = 32;
  for (std::size_t offset = 0;; offset += kPage)
  {
    const auto page = engine.ListPresets({offset, kPage});
    for (const auto& preset : page.items) taken.push_back(preset.name);
    if (page.items.empty() || offset + kPage >= page.totalCount) break;
  }
#if defined(CARDENZA_NATIVE_UI)
  // A native save never opens a text field or grows a chain of numeric suffixes.
  for (unsigned number = 1; number <= 9999; ++number)
  {
    char name[24];
    std::snprintf(name, sizeof(name), "PRESET %02u", number);
    if (std::find(taken.begin(), taken.end(), name) == taken.end()) return name;
  }
  return "PRESET";
#else
  const std::string base = engine.CurrentPresetName();
  for (int number = 2; number < 100; ++number)
  {
    const std::string name = base + " " + std::to_string(number);
    if (std::find(taken.begin(), taken.end(), name) == taken.end()) return name;
  }
  return base;
#endif
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// Shared slot presentation

bool Controller::SlotHasValue(const Slot& slot) const
{
  return slot.IsParameter();
}

bool Controller::SlotValue(const Slot& slot, synth::SynthParameterInfo& info, std::string& text) const
{
  if (!slot.IsParameter()) return false;
  if (SettingValue(slot.id, info, text)) return true;
  const synth::ISynthEngine* engine = ActiveEngine();
  if (engine == nullptr || !engine->GetParameterInfo(slot.id, info)) return false;
  text = engine->DescribeParameterValue(slot.id);
  return true;
}

// ---------------------------------------------------------------------------------------------
// Settings: the keyboard for every track, the current track's chord and arpeggiator, the project and
// the system.

namespace {

bool IsSetting(const char* id)
{
  return StartsWith(id, "keys.") || StartsWith(id, "chord.") || StartsWith(id, "arp.") || StartsWith(id, "proj.") || StartsWith(id, "sys.");
}

bool Is(const char* id, const char* name)
{
  return std::strcmp(id, name) == 0;
}

}  // namespace

bool Controller::SettingValue(const char* id, synth::SynthParameterInfo& info, std::string& text) const
{
  if (id == nullptr || !IsSetting(id)) return false;
  const KeyboardSettings& keys = state_.keyboard;
  const ChordSettings& shape = state_.chords[state_.track];
  const synth::ArpSettings arp = host_.Sequencer().GetArpSettings(state_.track);
  info = synth::SynthParameterInfo{};
  info.id = id;
  info.kind = synth::SynthParameterKind::Choice;
  info.minValue = 0;
  auto choice = [&](int value, int count, int fallback, const char* name) {
    info.maxValue = count - 1;
    info.value = std::clamp(value, 0, count - 1);
    info.defaultValue = fallback;
    text = name;
  };
  auto number = [&](int value, int minimum, int maximum, int fallback, const std::string& shown) {
    info.kind = synth::SynthParameterKind::Continuous;
    info.minValue = minimum;
    info.maxValue = maximum;
    info.value = value;
    info.defaultValue = fallback;
    text = shown;
  };
  if (Is(id, "keys.layout")) choice(static_cast<int>(keys.layout), 3, 0, kLayoutNames[static_cast<std::size_t>(keys.layout)]);
  else if (Is(id, "keys.scale")) choice(static_cast<int>(keys.scale), static_cast<int>(kScales.size()), 0, kScales[std::min(keys.scale, kScales.size() - 1)].label);
  else if (Is(id, "keys.root")) choice(keys.root, 12, 0, kNoteNames[static_cast<std::size_t>(std::clamp(keys.root, 0, 11))]);
  else if (Is(id, "keys.octave")) number(keys.octave, -3, 3, 0, (keys.octave > 0 ? "+" : "") + std::to_string(keys.octave));
  else if (Is(id, "chord.type")) choice(static_cast<int>(shape.chord), static_cast<int>(kChordTypeCount), 0, kChordNames[static_cast<std::size_t>(shape.chord)]);
  else if (Is(id, "chord.voicing")) choice(shape.voicing, static_cast<int>(kVoicingNames.size()), 0, kVoicingNames[std::min<std::size_t>(shape.voicing, 3)]);
  else if (Is(id, "keys.lock")) choice(static_cast<int>(keys.lock), 3, 0, kScaleLockNames[static_cast<std::size_t>(keys.lock)]);
  else if (Is(id, "keys.velocity")) number(keys.velocity, 1, 127, 110, std::to_string(keys.velocity));
  else if (Is(id, "keys.select")) choice(keys.multiSelect ? 1 : 0, 2, 0, keys.multiSelect ? "MULTI" : "ONE");
  else if (Is(id, "arp.mode")) choice(static_cast<int>(arp.mode), static_cast<int>(synth::kArpModeCount), 0, synth::kArpModeNames[static_cast<std::size_t>(arp.mode)]);
  else if (Is(id, "arp.rate")) choice(arp.rate, static_cast<int>(synth::kArpRateCount), 3, synth::kArpRateNames[std::min<std::size_t>(arp.rate, synth::kArpRateCount - 1)]);
  else if (Is(id, "arp.octaves")) number(arp.octaves, 1, 4, 1, std::to_string(arp.octaves) + " OCT");
  else if (Is(id, "arp.gate")) number(arp.gate, 10, 100, 50, std::to_string(arp.gate) + "%");
  else if (Is(id, "arp.latch"))
  {
    choice(arp.latch ? 1 : 0, 2, 0, arp.latch ? "ON" : "OFF");
    info.kind = synth::SynthParameterKind::Toggle;
  }
  else if (StartsWith(id, "proj."))
  {
    // Project settings live in the sequencer and are saved with the project.
    const auto& sequencer = host_.Sequencer();
    if (Is(id, "proj.bpm")) number(sequencer.Tempo(), 30, 300, 120, std::to_string(sequencer.Tempo()));
    else if (Is(id, "proj.swing")) number(sequencer.Swing(), 50, 75, 50, std::to_string(sequencer.Swing()) + "%");
    else if (Is(id, "proj.mode")) choice(sequencer.SongMode() ? 1 : 0, 2, 0, sequencer.SongMode() ? "SONG" : "LOOP");
    else if (Is(id, "proj.steps"))
      number(static_cast<int>(sequencer.TrackStepCount(state_.track)), 1, static_cast<int>(synth::kSequencerMaxSteps), static_cast<int>(synth::kSequencerStepCount),
             "T" + std::to_string(state_.track + 1) + " " + std::to_string(sequencer.TrackStepCount(state_.track)));
    else if (Is(id, "proj.rec"))
    {
      choice(sequencer.RecordArmed() ? 1 : 0, 2, 0, sequencer.RecordArmed() ? "ARMED" : "OFF");
      info.kind = synth::SynthParameterKind::Toggle;
    }
    else return false;
  }
  else if (Is(id, "sys.volume")) number(host_.MasterVolume(), 0, 100, os::storage::kDefaultOutputVolume, std::to_string(host_.MasterVolume()));
  else if (Is(id, "sys.led")) number(host_.LedBrightness(), 0, 15, os::storage::kDefaultKeyLedBrightness, std::to_string(host_.LedBrightness()));
  else if (Is(id, "sys.screen")) number(host_.Backlight(), 10, 255, os::storage::kDefaultScreenBacklight, std::to_string(host_.Backlight() * 100 / 255) + "%");
  else if (Is(id, "sys.log"))
  {
    choice(host_.Logging() ? 1 : 0, 2, os::storage::kDefaultLogEnabled ? 1 : 0, host_.Logging() ? "ON" : "OFF");
    info.kind = synth::SynthParameterKind::Toggle;
  }
  else return false;
  return true;
}

void Controller::AdjustSetting(const char* id, int delta)
{
  synth::SynthParameterInfo info;
  std::string text;
  if (delta == 0 || !SettingValue(id, info, text)) return;
  // Wide ranges move in useful steps: the screen in 5s (of 255), the volume in 2s.
  if (Is(id, "sys.screen")) delta *= 5;
  else if (Is(id, "sys.volume")) delta *= 2;
  const int value = Is(id, "keys.root") ? ((info.value + delta) % 12 + 12) % 12 : std::clamp(info.value + delta, info.minValue, info.maxValue);
  if (value == info.value) return;
  KeyboardSettings& keys = state_.keyboard;
  ChordSettings& shape = state_.chords[state_.track];
  auto& sequencer = host_.Sequencer();
  synth::ArpSettings arp = sequencer.GetArpSettings(state_.track);
  if (Is(id, "keys.layout")) keys.layout = static_cast<KeyboardLayout>(value);
  else if (Is(id, "keys.scale")) keys.scale = static_cast<std::size_t>(value);
  else if (Is(id, "keys.root")) keys.root = value;
  else if (Is(id, "keys.octave")) keys.octave = value;
  else if (Is(id, "chord.type")) shape.chord = static_cast<ChordType>(value);
  else if (Is(id, "chord.voicing")) shape.voicing = static_cast<std::uint8_t>(value);
  else if (Is(id, "keys.lock")) keys.lock = static_cast<ScaleLock>(value);
  else if (Is(id, "keys.select")) keys.multiSelect = value != 0;
  else if (Is(id, "keys.velocity")) keys.velocity = static_cast<std::uint8_t>(value);
  else if (StartsWith(id, "arp."))
  {
    if (Is(id, "arp.mode")) arp.mode = static_cast<synth::ArpMode>(value);
    else if (Is(id, "arp.rate")) arp.rate = static_cast<std::uint8_t>(value);
    else if (Is(id, "arp.octaves")) arp.octaves = static_cast<std::uint8_t>(value);
    else if (Is(id, "arp.gate")) arp.gate = static_cast<std::uint8_t>(value);
    else if (Is(id, "arp.latch")) arp.latch = value != 0;
    sequencer.SetArpSettings(state_.track, arp);
  }
  else if (Is(id, "proj.bpm")) sequencer.SetTempo(static_cast<std::uint16_t>(value));
  else if (Is(id, "proj.swing")) sequencer.SetSwing(static_cast<std::uint8_t>(value));
  else if (Is(id, "proj.mode")) sequencer.SetSongMode(value != 0);
  else if (Is(id, "proj.steps")) sequencer.SetTrackStepCount(state_.track, static_cast<std::size_t>(value));
  else if (Is(id, "proj.rec")) sequencer.SetRecordArmed(value != 0);
  else if (Is(id, "sys.volume")) host_.SetMasterVolume(static_cast<std::uint8_t>(value));
  else if (Is(id, "sys.led")) host_.SetLedBrightness(static_cast<std::uint8_t>(value));
  else if (Is(id, "sys.screen")) host_.SetBacklight(static_cast<std::uint8_t>(value));
  else if (Is(id, "sys.log")) host_.SetLogging(value != 0);
  // The key range moves under held pads: let them go before they map to other notes.
  if (Is(id, "keys.layout") || Is(id, "keys.octave")) ReleaseAllPads();
  // Only project data makes the project unsaved: the keyboard, the tracks' chords and arpeggiators,
  // tempo, swing and steps. SELECT is an app setting, the rest are performance or device settings.
  if ((StartsWith(id, "keys.") && !Is(id, "keys.select")) || StartsWith(id, "chord.") || StartsWith(id, "arp.") || Is(id, "proj.bpm") ||
      Is(id, "proj.swing") || Is(id, "proj.steps"))
    MarkDirty();
}

void Controller::StepSetting(const char* id)
{
  synth::SynthParameterInfo info;
  std::string text;
  if (!SettingValue(id, info, text) || info.kind == synth::SynthParameterKind::Continuous) return;
  AdjustSetting(id, info.value >= info.maxValue ? info.minValue - info.value : 1);
}

void Controller::ResetSetting(const char* id)
{
  synth::SynthParameterInfo info;
  std::string text;
  if (!SettingValue(id, info, text)) return;
  AdjustSetting(id, info.defaultValue - info.value);
}

void Controller::ShowHud(const std::string& label, const std::string& value, float fraction, bool bipolar)
{
  state_.hud.label = label;
  state_.hud.value = value;
  state_.hud.fraction = fraction;
  state_.hud.bipolar = bipolar;
  state_.hud.at = state_.now == 0 ? 1 : state_.now;
}

void Controller::ShowSettingHud(const char* id, const char* label)
{
  synth::SynthParameterInfo info;
  std::string text;
  if (!SettingValue(id, info, text)) return;
  const float span = static_cast<float>(std::max(1, info.maxValue - info.minValue));
  const bool bipolar = info.minValue < 0;
  const float fraction = bipolar ? static_cast<float>(info.value) / static_cast<float>(info.maxValue)
                                 : static_cast<float>(info.value - info.minValue) / span;
  ShowHud(label, text, fraction, bipolar);
}

// ---------------------------------------------------------------------------------------------
// SETUP settings pages (KEYS, CHORD, ARP, SYSTEM): the GRID picks a setting, ADD / SUB change it,
// ENTER steps a choice, FN + ENTER restores the default. Pads keep playing, so every change can be
// heard at once.

const InstrumentPage* Controller::SettingsPageShown() const
{
  if (state_.scene != Scene::Setup || CurrentPage() < kSetupKeys) return nullptr;
  return &SettingsPage(CurrentPage() - kSetupKeys);
}

std::uint8_t Controller::SettingsFocus() const
{
  if (state_.scene != Scene::Setup || CurrentPage() < kSetupKeys) return 0;
  return state_.settingsFocus[(CurrentPage() - kSetupKeys) % kSettingsPageCount];
}

void Controller::SettingsGrid(std::size_t index)
{
  const InstrumentPage* page = SettingsPageShown();
  if (page == nullptr || index >= kGridCount) return;
  const Slot& slot = page->slots[index];
  if (!slot.IsParameter()) return;
  auto& focus = state_.settingsFocus[(CurrentPage() - kSetupKeys) % kSettingsPageCount];
  // A second press steps a choice or flips a switch, like ENTER. When it was a tap: held, the pads pick.
  if (focus == index) state_.cellPress.stepOnRelease = true;
  focus = static_cast<std::uint8_t>(index);
}

void Controller::SettingsAdjust(int delta, bool coarse)
{
  const InstrumentPage* page = SettingsPageShown();
  if (page == nullptr) return;
  const Slot& slot = page->slots[SettingsFocus()];
  if (slot.IsParameter()) AdjustSetting(slot.id, delta * (coarse ? 5 : 1));
}

void Controller::SettingsEnter()
{
  const InstrumentPage* page = SettingsPageShown();
  if (page == nullptr) return;
  const Slot& slot = page->slots[SettingsFocus()];
  if (slot.IsParameter()) StepSetting(slot.id);
}

void Controller::SettingsReset()
{
  const InstrumentPage* page = SettingsPageShown();
  if (page == nullptr) return;
  const Slot& slot = page->slots[SettingsFocus()];
  if (!slot.IsParameter()) return;
  ResetSetting(slot.id);
  ShowSettingHud(slot.id, slot.label);
}

void Controller::SettingsArrow(int step)
{
  const InstrumentPage* page = SettingsPageShown();
  if (page == nullptr) return;
  int focus = SettingsFocus();
  for (std::size_t attempt = 0; attempt < kGridCount; ++attempt)
  {
    focus = (focus + step + static_cast<int>(kGridCount)) % static_cast<int>(kGridCount);
    if (page->slots[static_cast<std::size_t>(focus)].IsParameter()) break;
  }
  if (page->slots[static_cast<std::size_t>(focus)].IsParameter())
    state_.settingsFocus[(CurrentPage() - kSetupKeys) % kSettingsPageCount] = static_cast<std::uint8_t>(focus);
}

// ---------------------------------------------------------------------------------------------
// SETUP PROJECT and FILES

void Controller::ProjectGrid(std::size_t index)
{
  const std::size_t page = CurrentPage();
  if (page == kSetupProject)
  {
    switch (index)
    {
      case kProjectSave: SaveProject(false); break;
      case kProjectLoad: SetPage(kSetupFiles); break;
      case kProjectNew:
        if (state_.dirty) OpenConfirm(ConfirmPurpose::NewProject, "NEW PROJECT?", "UNSAVED CHANGES WILL BE LOST", true);
        else NewProject();
        break;
      case kProjectBpm:
      case kProjectSwing:
        state_.projectFocus = static_cast<std::uint8_t>(index);
        break;
      case kProjectTap:
        state_.projectFocus = static_cast<std::uint8_t>(index);
        TapTempo();
        break;
      case kProjectMode:
        StepSetting("proj.mode");
        ShowSettingHud("proj.mode", "PLAY");
        break;
      default: break;
    }
  }
}

void Controller::ProjectAdjust(int delta, bool coarse)
{
  auto& sequencer = host_.Sequencer();
  switch (CurrentPage())
  {
    case kSetupProject:
    {
      const auto ui = sequencer.BuildUiState(state_.track);
      if (state_.projectFocus == kProjectBpm)
      {
        sequencer.SetTempo(static_cast<std::uint16_t>(std::clamp(static_cast<int>(ui.bpm) + delta * (coarse ? 10 : 1), 30, 300)));
        MarkDirty();
      }
      else if (state_.projectFocus == kProjectSwing)
      {
        sequencer.SetSwing(static_cast<std::uint8_t>(std::clamp(static_cast<int>(ui.swing) + delta * (coarse ? 5 : 1), 50, 75)));
        MarkDirty();
      }
      break;
    }
    case kSetupFiles:
    {
      if (state_.projects.empty()) break;
      const int count = static_cast<int>(state_.projects.size());
      const int next = std::clamp(static_cast<int>(state_.fileCursor) - delta * (coarse ? 8 : 1), 0, count - 1);
      state_.fileCursor = static_cast<std::size_t>(next);
      break;
    }
    default: break;
  }
}

void Controller::ProjectEnter()
{
  const std::size_t page = CurrentPage();
  if (page == kSetupProject && state_.projectFocus == kProjectTap) TapTempo();
  else if (page == kSetupProject) SaveProject(false);
  else if (page == kSetupFiles && state_.fileCursor < state_.projects.size())
  {
    OpenProject(state_.projects[state_.fileCursor]);
  }
}

void Controller::OpenProject(std::string name)
{
  RefreshProjects();
  if (state_.dirty) OpenConfirm(ConfirmPurpose::LoadProject, "LOAD " + name + "?", "UNSAVED CHANGES WILL BE LOST", true, name);
  else LoadProjectNamed(name);
}

void Controller::SaveProject(bool saveAs)
{
#if defined(CARDENZA_NATIVE_UI)
  if (saveAs || state_.projectName.empty())
  {
    SaveProjectAutomatic();
    return;
  }
#endif
  if (saveAs)
  {
    OpenTextEntry(TextPurpose::ProjectSaveAs, "SAVE PROJECT AS", state_.projectName.empty() ? "SONG" : state_.projectName, 24);
    return;
  }
  if (state_.projectName.empty())
  {
    // A quick save never asks for a name: the first free SONG n. SAVE AS (SETUP PROJECT) renames.
    const std::vector<std::string> existing = host_.ListProjects();
    std::string name;
    for (int number = 1; number < 1000 && name.empty(); ++number)
    {
      const std::string candidate = "SONG " + std::string(number < 10 ? "0" : "") + std::to_string(number);
      if (std::find(existing.begin(), existing.end(), candidate) == existing.end()) name = candidate;
    }
    SaveProjectNamed(name.empty() ? "SONG" : name, false);
    return;
  }
  SaveProjectNamed(state_.projectName, true);
}

void Controller::SaveProjectNamed(const std::string& name, bool overwrite)
{
  std::string message;
  switch (host_.SaveProject(name, overwrite, Settings(), message))
  {
    case SaveResult::Ok:
      state_.projectName = name;
      state_.dirty = false;
      ShowToast("SAVED " + name, ToastKind::Ok);
      break;
    case SaveResult::Exists:
      OpenConfirm(ConfirmPurpose::OverwriteProject, "OVERWRITE " + name + "?", "THE SAVED PROJECT WILL BE REPLACED", true, name);
      break;
    case SaveResult::Failed:
    default:
      ShowToast(message.empty() ? "SAVE FAILED" : message, ToastKind::Error);
      break;
  }
}

void Controller::LoadProjectNamed(const std::string& name)
{
  ProjectUiSettings settings;
  std::string message;
  ReleaseAllPads();
  if (!host_.LoadProject(name, settings, message))
  {
    ShowToast(message.empty() ? "LOAD FAILED" : message, ToastKind::Error);
    return;
  }
  Restore(settings, name);
  ShowToast("LOADED " + name, ToastKind::Ok);
}

void Controller::NewProject()
{
  ProjectUiSettings settings;
  std::string message;
  ReleaseAllPads();
  if (!host_.NewProject(settings, message))
  {
    ShowToast(message.empty() ? "NEW FAILED" : message, ToastKind::Error);
    return;
  }
  Restore(settings, "");
  ShowToast("NEW PROJECT", ToastKind::Ok);
}

void Controller::RefreshProjects()
{
  state_.projects = host_.ListProjects();
  std::sort(state_.projects.begin(), state_.projects.end());
  const auto current = std::find(state_.projects.begin(), state_.projects.end(), state_.projectName);
  if (current != state_.projects.end()) state_.fileCursor = static_cast<std::size_t>(current - state_.projects.begin());
  if (state_.fileCursor >= state_.projects.size()) state_.fileCursor = state_.projects.empty() ? 0 : state_.projects.size() - 1;
}

// ---------------------------------------------------------------------------------------------
// INSTR

void Controller::InstrumentGrid(std::size_t index)
{
  const InstrumentPage* page = CurrentInstrumentPage();
  const Slot& slot = page->slots[index];
  if (slot.Empty()) return;
  if (slot.IsAction())
  {
    RunSlotAction(slot.action);
    return;
  }
  auto& focus = state_.instrumentFocus[std::min<std::size_t>(CurrentPage(), state_.instrumentFocus.size() - 1)];
  synth::SynthParameterInfo info;
  std::string text;
  // A second press on a focused switch flips it and steps a choice (wrapping), like ENTER; HOLD flips
  // on the first, it is played rather than set. When the press was a tap: held, the pads pick instead.
  const bool performanceSwitch = std::strcmp(slot.id, "arp.latch") == 0;
  if ((focus == index || performanceSwitch) && SlotValue(slot, info, text) && info.kind != synth::SynthParameterKind::Continuous)
    state_.cellPress.stepOnRelease = true;
  focus = static_cast<std::uint8_t>(index);
}

void Controller::StepCell(std::size_t index)
{
  if (state_.scene == Scene::Instrument) InstrumentEnter();
  else if (state_.scene == Scene::Setup)
  {
    const InstrumentPage* page = SettingsPageShown();
    if (page != nullptr && index < kGridCount && page->slots[index].IsParameter()) StepSetting(page->slots[index].id);
  }
  else if (state_.scene == Scene::Tape && CurrentPage() == kTapeSends) SendsEnter();
}

bool Controller::CellParameter(std::size_t index, std::string& label, synth::SynthParameterInfo& info, std::string& text) const
{
  if (index >= kGridCount) return false;
  const Slot* slot = nullptr;
  if (state_.scene == Scene::Instrument) slot = &CurrentInstrumentPage()->slots[index];
  else if (state_.scene == Scene::Setup)
  {
    if (const InstrumentPage* page = SettingsPageShown()) slot = &page->slots[index];
  }
  if (slot != nullptr)
  {
    if (!slot->IsParameter() || !SlotValue(*slot, info, text)) return false;
    label = slot->label;
    return true;
  }
  if (state_.scene == Scene::Setup && CurrentPage() == kSetupProject && (index == kProjectBpm || index == kProjectSwing))
  {
    label = index == kProjectBpm ? "BPM" : "SWING";
    return SettingValue(index == kProjectBpm ? "proj.bpm" : "proj.swing", info, text);
  }
  if (state_.scene != Scene::Tape) return false;
  const std::size_t row = index / kGridColumns;
  const std::size_t track = index % kGridColumns;
  if (CurrentPage() == kTapeSends)
  {
    const synth::ISynthEngine* engine = host_.Engine(track);
    if (engine == nullptr || !engine->GetParameterInfo(kSendIds[row], info)) return false;
    text = engine->DescribeParameterValue(kSendIds[row]);
    label = "T" + std::to_string(track + 1) + " " + info.label;
    return true;
  }
  if (CurrentPage() == kTapeFx)
  {
    if (index >= synth::fx::kMasterParamCount) return false;
    const auto& master = synth::fx::kMasterParams[index];
    info = synth::SynthParameterInfo{};
    info.kind = synth::SynthParameterKind::Continuous;
    info.minValue = master.minValue;
    info.maxValue = master.maxValue;
    info.value = host_.MasterFx(index);
    text = std::to_string(info.value);
    label = master.label;
    return true;
  }
  if (CurrentPage() == kTapeMix && row == 0)
  {
    info = synth::SynthParameterInfo{};
    info.kind = synth::SynthParameterKind::Continuous;
    info.minValue = 0;
    info.maxValue = 100;
    info.value = host_.TrackLevel(track);
    text = std::to_string(info.value);
    label = "T" + std::to_string(track + 1) + " LEVEL";
    return true;
  }
  return false;
}

void Controller::SetCellParameter(std::size_t index, int value)
{
  std::string label, text;
  synth::SynthParameterInfo info;
  if (!CellParameter(index, label, info, text) || value == info.value) return;
  const Slot* slot = nullptr;
  if (state_.scene == Scene::Instrument) slot = &CurrentInstrumentPage()->slots[index];
  else if (state_.scene == Scene::Setup)
  {
    const InstrumentPage* page = SettingsPageShown();
    if (page == nullptr)
    {
      AdjustSetting(index == kProjectBpm ? "proj.bpm" : "proj.swing", value - info.value);
      return;
    }
    slot = &page->slots[index];
  }
  if (slot != nullptr)
  {
    if (IsSetting(slot->id)) AdjustSetting(slot->id, value - info.value);
    else if (synth::ISynthEngine* engine = ActiveEngine(); engine != nullptr && engine->SetParameterValue(slot->id, value)) MarkDirty();
    return;
  }
  const std::size_t track = index % kGridColumns;
  if (CurrentPage() == kTapeSends)
  {
    if (synth::ISynthEngine* engine = host_.Engine(track); engine != nullptr && engine->SetParameterValue(kSendIds[index / kGridColumns], value)) MarkDirty();
  }
  else if (CurrentPage() == kTapeFx) host_.SetMasterFx(index, value), MarkDirty();
  else if (CurrentPage() == kTapeMix) host_.SetTrackLevel(track, static_cast<std::uint8_t>(value)), MarkDirty();
}

bool Controller::CellPickerActive() const
{
  const CellPress& press = state_.cellPress;
  if (!press.down || TopModal() != nullptr) return false;
  for (const auto& left : state_.left)
    if (left.down) return false;
  if (!press.picked && state_.now - press.at < kCellPickMs) return false;
  std::string label, text;
  synth::SynthParameterInfo info;
  return CellParameter(press.index, label, info, text);
}

void Controller::PickCellValue(std::size_t row, std::size_t column)
{
  std::string label, text;
  synth::SynthParameterInfo info;
  CellPress& press = state_.cellPress;
  if (!CellParameter(press.index, label, info, text)) return;
  const std::size_t pad = row * kPadColumns + (column - kPadFirstColumn);
  if (pad >= CellPickCount(info)) return;
  SetCellParameter(press.index, CellPickValue(info, pad));
  press.picked = true;
  ShowCellHud();
}

void Controller::ShowCellHud()
{
  std::string label, text;
  synth::SynthParameterInfo info;
  if (!CellParameter(state_.cellPress.index, label, info, text)) return;
  // Up for as long as the cell is held: it pops in once, then stays at this age.
  constexpr std::uint32_t kSettledMs = 200;
  const std::uint32_t age = state_.hud.at != 0 && state_.hud.label == label ? state_.now - state_.hud.at : 0;
  const int range = info.maxValue - info.minValue;
  ShowHud(label, text, range > 0 ? static_cast<float>(info.value - info.minValue) / static_cast<float>(range) : -1.0f);
  state_.hud.at = state_.now - std::min(age, kSettledMs);
  if (state_.hud.at == 0) state_.hud.at = 1;
}

void Controller::InstrumentAdjust(int delta, bool coarse)
{
  const InstrumentPage* page = CurrentInstrumentPage();
  if (page->graphic == PageGraphic::Voice)
  {
    // ADD is the upper key: up the list, as in the preset browser.
    StepPreset(-delta * (coarse ? 10 : 1));
    return;
  }
  const Slot& slot = page->slots[InstrumentFocus()];
  if (!slot.IsParameter()) return;
  if (IsSetting(slot.id))
  {
    AdjustSetting(slot.id, delta * (coarse ? 3 : 1));
    return;
  }
  synth::ISynthEngine* engine = ActiveEngine();
  synth::SynthParameterInfo info;
  if (engine == nullptr || !engine->GetParameterInfo(slot.id, info)) return;
  int scaled = delta;
  if (coarse) scaled *= info.maxValue - info.minValue >= 40 ? 10 : 3;
  if (engine->AdjustParameterValue(slot.id, scaled)) MarkDirty();
}

void Controller::InstrumentEnter()
{
  const InstrumentPage* page = CurrentInstrumentPage();
  if (page->graphic == PageGraphic::Voice)
  {
    OpenPresetBrowser();
    return;
  }
  const Slot& slot = page->slots[InstrumentFocus()];
  synth::SynthParameterInfo info;
  std::string text;
  if (!SlotValue(slot, info, text)) return;
  // ENTER steps switches and choices; continuous values stay on SUB / ADD.
  if (info.kind == synth::SynthParameterKind::Toggle) InstrumentAdjust(info.value == info.maxValue ? -1 : 1, false);
  else if (info.kind == synth::SynthParameterKind::Choice)
  {
    if (info.value >= info.maxValue)
    {
      if (IsSetting(slot.id)) StepSetting(slot.id);
      else if (synth::ISynthEngine* engine = ActiveEngine(); engine != nullptr && engine->SetParameterValue(slot.id, info.minValue)) MarkDirty();
    }
    else InstrumentAdjust(1, false);
  }
}

void Controller::InstrumentReset()
{
  const InstrumentPage* page = CurrentInstrumentPage();
  const Slot& slot = page->slots[InstrumentFocus()];
  if (!slot.IsParameter()) return;
  if (IsSetting(slot.id))
  {
    ResetSetting(slot.id);
    return;
  }
  synth::ISynthEngine* engine = ActiveEngine();
  synth::SynthParameterInfo info;
  if (engine != nullptr && engine->GetParameterInfo(slot.id, info) && engine->SetParameterValue(slot.id, info.defaultValue))
  {
    MarkDirty();
    ShowToast(std::string(slot.label) + " DEFAULT");
  }
}

void Controller::StepPreset(int delta)
{
  synth::ISynthEngine* engine = ActiveEngine();
  if (engine == nullptr || delta == 0) return;
  constexpr std::size_t kPage = 8;  // Each page is one block of memory.
  std::size_t total = 0;
  std::size_t current = 0;
  bool found = false;
  for (std::size_t offset = 0;; offset += kPage)
  {
    const auto page = engine->ListPresets({offset, kPage});
    total = page.totalCount;
    for (std::size_t index = 0; index < page.items.size() && !found; ++index)
      if (page.items[index].active) { current = offset + index; found = true; }
    if (found || page.items.empty() || offset + kPage >= total) break;
  }
  if (total == 0) return;
  const int next = (static_cast<int>(current) + delta % static_cast<int>(total) + static_cast<int>(total)) % static_cast<int>(total);
  const auto target = engine->ListPresets({static_cast<std::size_t>(next), 1});
  if (target.items.empty()) return;
  const synth::SynthPresetInfo& preset = target.items.front();
  if (ConfirmLosingEdits(ConfirmPurpose::ChangePreset, "LOAD " + preset.name + "?", preset.id)) return;
  ApplyPreset(preset.id);
}

void Controller::RunSlotAction(SlotAction action)
{
  synth::ISynthEngine* engine = ActiveEngine();
  switch (action)
  {
    case SlotAction::Engine: OpenEnginePicker(); break;
    case SlotAction::Preset: OpenPresetBrowser(); break;
    case SlotAction::SavePreset:
    {
      if (engine == nullptr) break;
      const std::string id = engine->CurrentPresetId();
      if (id.rfind("user.", 0) != 0)
      {
        // A factory sound becomes a new user sound without asking for a name: SAVE AS, right next
        // to it in the menu, is the one that asks.
        const std::string name = FreePresetName(*engine);
        if (engine->SavePreset("", name)) ShowToast("SAVED " + name, ToastKind::Ok);
        else ShowToast("PRESET SAVE FAILED", ToastKind::Error);
        break;
      }
      if (engine->SavePreset(id, engine->CurrentPresetName())) ShowToast("PRESET SAVED", ToastKind::Ok);
      else ShowToast("PRESET SAVE FAILED", ToastKind::Error);
      break;
    }
    case SlotAction::SavePresetAs:
#if defined(CARDENZA_NATIVE_UI)
      if (engine != nullptr)
      {
        const std::string name = FreePresetName(*engine);
        if (engine->SavePreset("", name)) ShowToast("SAVED " + name, ToastKind::Ok);
        else ShowToast("PRESET SAVE FAILED", ToastKind::Error);
      }
#else
      if (engine != nullptr) OpenTextEntry(TextPurpose::PresetSaveAs, "SAVE PRESET AS", engine->CurrentPresetName(), 18);
#endif
      break;
    case SlotAction::SampleFile: OpenSampleBrowser(); break;
    case SlotAction::SampleRecord:
    {
      const auto sampler = host_.Sampler().BuildUiState(state_.track);
      if (sampler.hasDraft && !sampler.recording)
        OpenConfirm(ConfirmPurpose::ReplaceTake, "RECORD AGAIN?", "THE UNSAVED TAKE WILL BE REPLACED", true);
      else OpenSampleRecord();
      break;
    }
    case SlotAction::SamplePreview: host_.PreviewSample(state_.track); break;
    case SlotAction::None:
    default: break;
  }
}

// ---------------------------------------------------------------------------------------------
// SEQ

// A pad while steps are selected (held or tapped): its notes (its chord, as one) go into them, or come
// out: a step that holds any of them loses them all and gains none. A new note gets `velocity` (a drum
// pad's row), or with 0 the track's new-note velocity.
bool Controller::PadForSequence(const std::uint8_t* notes, std::size_t count, bool pressed, std::uint8_t velocity)
{
  if (!pressed || count == 0) return false;
  auto& sequencer = host_.Sequencer();
  if (!sequencer.BuildUiState(state_.track).hasHeldSteps) return false;
  if (sequencer.ToggleNotesOnSelectedSteps(notes, count, velocity)) MarkDirty();
  return true;
}

// The note page's step keys, as MatrixOS's sequencer has them: a step is selected while its key is
// down, so the pads (its notes), ADD / SUB and OK (its values) edit it, and steps held together are
// edited together. Pads held and a step pressed: their notes go in (or come out). Beyond MatrixOS,
// a tap keeps the step selected, to edit it with the hands free; a tap on the step selected alone
// lets it go (with SELECT MULTI a tap adds a step, and a tap on a selected one lets that one go).
// The drum machine takes the same keys: its pads are the notes, a drum hit as hard as its pad's row.
void Controller::StepKeyPress(std::size_t index)
{
  auto& sequencer = host_.Sequencer();
  const auto ui = sequencer.BuildUiState(state_.track);
  const std::size_t step = ui.firstViewStep + index;
  if (index >= kGridCount || step >= ui.tracks[state_.track].stepCount) return;
  StepKey& key = state_.stepKeys[index];
  key = StepKey{};
  key.down = true;
  key.step = step;
  key.at = state_.now;
  std::array<std::uint8_t, kRows * kPadColumns> notes{};
  const std::size_t count = CollectHeldNotes(notes);
  if (count > 0)
  {
    key.wrote = true;
    // Drums go in as hard as the row of the pad that holds them; melodic notes at the track's
    // new-note velocity.
    std::array<std::uint8_t, kRows * kPadColumns> velocities{};
    const bool drums = ActiveKind() == synth::SynthEngineKind::Drums;
    for (std::size_t held = 0; drums && held < count; ++held)
      for (std::size_t pad = 0; pad < state_.padNote.size(); ++pad)
        if (state_.padNote[pad] == notes[held] && state_.padTrack[pad] == state_.track)
          velocities[held] = std::max(velocities[held], state_.padVelocity[pad]);
    if (sequencer.ToggleStepNotes(state_.track, step, notes.data(), count, drums ? velocities.data() : nullptr)) MarkDirty();
    return;
  }
  key.wasSelected = ui.steps[index].held;
  key.wasOnly = key.wasSelected && ui.heldStepCount == 1;
  // Another step key down (KeyEvent made it a hold): this step joins it.
  bool joining = false;
  for (std::size_t other = 0; other < kGridCount; ++other)
    joining = joining || (other != index && state_.stepKeys[other].down && !state_.stepKeys[other].wrote);
  if (joining) key.used = true;
  else if (!state_.keyboard.multiSelect && !key.wasOnly) sequencer.ClearSelectedSteps();
  sequencer.SetStepSelected(step, true);
  AuditionStep(index, step);
}

void Controller::StepKeyRelease(std::size_t index)
{
  if (index >= kGridCount || !state_.stepKeys[index].down) return;
  const StepKey key = state_.stepKeys[index];
  state_.stepKeys[index] = StepKey{};
  if (static_cast<std::size_t>(state_.stepAudition.key) == index) StopStepAudition();
  if (key.wrote) return;
  auto& sequencer = host_.Sequencer();
  if (!key.used && state_.now - key.at < kHoldMs)
  {
    if (state_.keyboard.multiSelect ? key.wasSelected : key.wasOnly) sequencer.SetStepSelected(key.step, false);
    return;
  }
  // Held: selected for as long as the key was down.
  if (!key.wasSelected) sequencer.SetStepSelected(key.step, false);
}

void Controller::UseStepKeys()
{
  for (StepKey& key : state_.stepKeys)
    if (key.down) key.used = true;
}

// Stopped, a step sounds while its key is down, as MatrixOS does: you hear what you are on.
void Controller::AuditionStep(std::size_t index, std::size_t step)
{
  StopStepAudition();
  auto& sequencer = host_.Sequencer();
  if (sequencer.Playing()) return;
  std::array<synth::SynthSequencer::StepNoteView, 8> notes{};
  const std::size_t count = sequencer.StepNoteEvents(state_.track, step, notes);
  StepAudition& audition = state_.stepAudition;
  audition.track = static_cast<std::uint8_t>(state_.track);
  audition.key = static_cast<std::uint8_t>(index);
  for (std::size_t slot = 0; slot < count; ++slot)
  {
    host_.NoteOn(state_.track, notes[slot].note, notes[slot].velocity);
    audition.notes[audition.count++] = notes[slot].note;
  }
}

void Controller::StopStepAudition()
{
  StepAudition& audition = state_.stepAudition;
  for (std::size_t slot = 0; slot < audition.count; ++slot)
  {
    // A pad sounding the same note itself keeps it: its own release ends it.
    bool held = false;
#if defined(CARDENZA_NATIVE_UI)
    held = NativePerformanceHeld(audition.track, audition.notes[slot]);
#endif
    for (std::size_t pad = 0; pad < state_.padNote.size() && !held; ++pad)
      for (std::size_t voice = 0; voice < state_.padChordCount[pad] && !held; ++voice)
        held = state_.padTrack[pad] == audition.track && !state_.padArp[pad] && state_.padChord[pad][voice] == audition.notes[slot];
    if (!held) host_.NoteOff(audition.track, audition.notes[slot]);
  }
  audition = StepAudition{};
}

bool Controller::DrumTrack() const
{
  return ActiveKind() == synth::SynthEngineKind::Drums;
}

bool Controller::DrumLayout() const
{
  return DrumTrack() && state_.record.drumView == SeqView::Step;
}

void Controller::SequenceGrid(std::size_t index)
{
  auto& sequencer = host_.Sequencer();
  switch (CurrentPage())
  {
    case 0:
      // ROLL, STEP and the drum machine alike: the GRID is one page of the pattern's steps.
      StepKeyPress(index);
      break;
    case 1:
    {
      const auto ui = sequencer.BuildUiState(state_.track);
      const auto& track = ui.tracks[state_.track];
      if (index > track.patternCount) return;
      const bool creating = index == track.patternCount;
      if (sequencer.QueuePattern(state_.track, index))
      {
        if (creating) MarkDirty();
        if (sequencer.Playing() && index != track.activePattern) ShowToast("NEXT " + PatternLabel(index));
      }
      break;
    }
    case 2:
      if (index == kTrackSeq)
      {
        sequencer.SetTrackEnabled(state_.track, !sequencer.TrackEnabled(state_.track));
        MarkDirty();
      }
      if (index <= kTrackBpm) state_.seqTrackFocus = static_cast<std::uint8_t>(index);
      break;
    default: break;
  }
}

void Controller::FlipStepPage(int delta)
{
  auto& sequencer = host_.Sequencer();
  const auto ui = sequencer.BuildUiState(state_.track);
  const int count = static_cast<int>(ui.viewPageCount);
  const int page = (static_cast<int>(ui.viewPage) + delta % count + count) % count;
  if (count > 1) sequencer.SetViewPage(state_.track, static_cast<std::size_t>(page));
  // A selection out of sight would take the pads for nothing visible.
  if (!state_.keyboard.multiSelect) sequencer.ClearSelectedSteps();
  ShowHud("PAGE", std::to_string(page + 1) + "/" + std::to_string(count), static_cast<float>(page) / static_cast<float>(std::max(1, count - 1)));
}

// The length of the current pattern. A big step lands on the page boundaries (and 1, 2, 4, 8).
void Controller::AdjustPatternLength(int delta, bool coarse)
{
  auto& sequencer = host_.Sequencer();
  const int page = static_cast<int>(kGridCount);
  const int current = static_cast<int>(sequencer.TrackStepCount(state_.track));
  int next = current + delta;
  if (coarse)
  {
    if (delta > 0) next = current < page ? std::min(page, current < 4 ? current * 2 : (current < 8 ? 8 : page)) : (current / page + 1) * page;
    else next = current <= page ? std::max(1, current <= 8 ? (current + 1) / 2 : 8) : (current - 1) / page * page;
  }
  next = std::clamp(next, 1, static_cast<int>(synth::kSequencerMaxSteps));
  sequencer.SetTrackStepCount(state_.track, static_cast<std::size_t>(next));
  MarkDirty();
  ShowHud("LENGTH", std::to_string(next) + " STEPS", static_cast<float>(next - 1) / static_cast<float>(synth::kSequencerMaxSteps - 1));
}

void Controller::SequenceAdjust(int delta, bool coarse)
{
  auto& sequencer = host_.Sequencer();
  const auto ui = sequencer.BuildUiState(state_.track);
  switch (CurrentPage())
  {
    case 0:
      // Selected steps: the value of the edit lane (OK chooses it). Nothing selected: ADD / SUB flip
      // the GRID's page, FN + ADD / SUB go to the next / previous pattern.
      if (ui.hasHeldSteps)
      {
        // On a drum track the pads pick the drum: the NOTE lane does not move hits to pitches that
        // are no drum (OK goes on to the length, velocity and timing).
        if (DrumTrack() && ui.editLane == synth::SequencerEditLane::Note)
        {
          ShowHud("NOTE", "PADS PICK DRUMS");
          return;
        }
        if (sequencer.AdjustSelectedSteps(delta, coarse)) MarkDirty();
        return;
      }
      if (!coarse)
      {
        FlipStepPage(delta);
        return;
      }
      [[fallthrough]];
    case 1:
    {
      const auto& track = ui.tracks[state_.track];
      const std::size_t base = track.queuedPattern < synth::kSequencerMaxPatternCount ? track.queuedPattern : track.activePattern;
      const int count = static_cast<int>(track.patternCount);
      const int next = (static_cast<int>(base) + (delta > 0 ? 1 : -1) + count) % count;
      sequencer.QueuePattern(state_.track, static_cast<std::size_t>(next));
      // Say where to: while playing the switch waits for the loop's end.
      const bool waits = sequencer.Playing() && static_cast<std::size_t>(next) != track.activePattern;
      ShowHud(std::string(waits ? "NEXT PATTERN " : "PATTERN ") + std::to_string(next + 1) + "/" + std::to_string(count),
              PatternLabel(static_cast<std::size_t>(next)), count > 1 ? static_cast<float>(next) / static_cast<float>(count - 1) : -1.0f);
      break;
    }
    case 2:
      switch (state_.seqTrackFocus)
      {
        case kTrackSeq:
          sequencer.SetTrackEnabled(state_.track, delta > 0);
          break;
        case kTrackSteps:
          AdjustPatternLength(delta, coarse);
          break;
        case kTrackSwing:
          sequencer.SetSwing(static_cast<std::uint8_t>(std::clamp(static_cast<int>(ui.swing) + delta * (coarse ? 5 : 1), 50, 75)));
          break;
        case kTrackBpm:
        default:
          sequencer.SetTempo(static_cast<std::uint16_t>(std::clamp(static_cast<int>(ui.bpm) + delta * (coarse ? 10 : 1), 30, 300)));
          break;
      }
      MarkDirty();
      break;
    default: break;
  }
}

void Controller::SequenceEnter()
{
  auto& sequencer = host_.Sequencer();
  switch (CurrentPage())
  {
    case 0:
      if (sequencer.BuildUiState(state_.track).hasHeldSteps) sequencer.CycleEditLane();
      break;
    case 1:
      SetPage(0);
      break;
    case 2:
      if (state_.seqTrackFocus == kTrackSeq)
      {
        sequencer.SetTrackEnabled(state_.track, !sequencer.TrackEnabled(state_.track));
        MarkDirty();
      }
      break;
    default: break;
  }
}

// ---------------------------------------------------------------------------------------------
// TAPE: song arrangement. GRID columns are four consecutive song rows (left to right in time),
// GRID rows are the four tracks.

void Controller::SongGrid(std::size_t index)
{
  auto& sequencer = host_.Sequencer();
  const std::size_t track = index / kGridColumns;
  const std::size_t row = state_.songScroll + index % kGridColumns;
  const std::size_t length = sequencer.SongLength();
  if (row > length || row >= synth::kSongMaxRows) return;
  if (row == length)
  {
    // The first empty column appends a row: a copy of the last row, or every track on its active pattern.
    synth::SongRow fresh{};
    if (length > 0) fresh = sequencer.SongRowAt(length - 1);
    else
      for (std::size_t t = 0; t < kTrackCount; ++t) fresh.patterns[t] = static_cast<std::int8_t>(sequencer.ActivePattern(t));
    if (!sequencer.InsertSongRow(length, fresh)) return;
    MarkDirty();
    ShowToast("ROW " + std::to_string(row + 1) + " ADDED");
  }
  // A second tap on the focused cell switches between editing its pattern and the row length.
  if (row == state_.songRow && track == state_.songTrack && row < length)
  {
    state_.songLane = static_cast<std::uint8_t>(state_.songLane ^ 1u);
    ShowHud("EDIT", state_.songLane == 0 ? "PATTERN" : "BARS");
  }
  state_.songRow = row;
  state_.songTrack = track;
}

void Controller::SongAdjust(int delta, bool coarse)
{
  auto& sequencer = host_.Sequencer();
  if (state_.songRow >= sequencer.SongLength()) return;
  const auto row = sequencer.SongRowAt(state_.songRow);
  if (coarse || state_.songLane == 1)
  {
    // The BARS lane changes how many bars the row lasts.
    const int repeats = std::clamp(static_cast<int>(row.repeats) + (delta > 0 ? 1 : -1), 1, 16);
    if (sequencer.SetSongRepeats(state_.songRow, static_cast<std::uint8_t>(repeats))) MarkDirty();
    return;
  }
  // Step through "silent" (-1) and the track's patterns.
  const int count = static_cast<int>(sequencer.PatternCount(state_.songTrack));
  const int current = row.patterns[state_.songTrack];
  const int next = ((current + 1 + delta) % (count + 1) + count + 1) % (count + 1) - 1;
  if (sequencer.SetSongPattern(state_.songRow, state_.songTrack, next)) MarkDirty();
}

void Controller::SongEnter()
{
  auto& sequencer = host_.Sequencer();
  if (state_.songRow >= sequencer.SongLength()) return;
  sequencer.CueSongRow(state_.songRow);
  if (!sequencer.SongMode())
  {
    sequencer.SetSongMode(true);
    ShowToast("SONG MODE");
  }
  else if (sequencer.Playing()) ShowToast("JUMP TO ROW " + std::to_string(state_.songRow + 1));
  else ShowToast("START AT ROW " + std::to_string(state_.songRow + 1));
}

void Controller::SongFnEnter()
{
  auto& sequencer = host_.Sequencer();
  const bool song = !sequencer.SongMode();
  sequencer.SetSongMode(song);
  ShowToast(song ? "SONG MODE" : "LOOP MODE");
}

void Controller::SongArrow(int horizontal, int vertical)
{
  const std::size_t length = host_.Sequencer().SongLength();
  if (horizontal != 0)
  {
    const int next = std::clamp(static_cast<int>(state_.songRow) + horizontal, 0, static_cast<int>(length));
    state_.songRow = static_cast<std::size_t>(next);
  }
  if (vertical != 0)
    state_.songTrack = static_cast<std::size_t>((static_cast<int>(state_.songTrack) + vertical + static_cast<int>(kTrackCount)) % static_cast<int>(kTrackCount));
  // Keep the cursor inside the four visible columns.
  if (state_.songRow < state_.songScroll) state_.songScroll = state_.songRow;
  if (state_.songRow >= state_.songScroll + kGridColumns) state_.songScroll = state_.songRow - kGridColumns + 1;
}

// ---------------------------------------------------------------------------------------------
// SESSION: a clip grid. GRID columns are the tracks (as on MIX), rows are pattern slots from the
// scroll position down. A tap launches that pattern on that track: at once when stopped, at the
// track's loop end while playing. ENTER launches the whole row (a scene), Fn + ENTER captures what
// plays now as a new SONG row, ADD / SUB move through the patterns.

void Controller::SessionGrid(std::size_t index)
{
  if (index >= kGridCount) return;
  auto& sequencer = host_.Sequencer();
  const std::size_t track = index % kGridColumns;
  const std::size_t pattern = state_.sessionScroll + index / kGridColumns;
  state_.sessionTrack = track;
  state_.sessionPattern = std::min(pattern, synth::kSequencerMaxPatternCount - 1);
  const std::size_t count = sequencer.PatternCount(track);
  if (pattern > count || pattern >= synth::kSequencerMaxPatternCount) return;
  // Launching takes the track out of the arrangement, like playing a clip in a session.
  if (sequencer.SongMode())
  {
    sequencer.SetSongMode(false);
    ShowHud("SESSION", "LOOP MODE");
  }
  const bool creating = pattern == count;
  if (!sequencer.QueuePattern(track, pattern)) return;
  if (creating) MarkDirty();
  SelectTrack(track);
  ShowHud("T" + std::to_string(track + 1) + (creating ? " NEW" : sequencer.Playing() ? " NEXT" : ""), PatternLabel(pattern));
}

void Controller::SessionAdjust(int delta)
{
  const int last = static_cast<int>(synth::kSequencerMaxPatternCount) - 1;
  state_.sessionPattern = static_cast<std::size_t>(std::clamp(static_cast<int>(state_.sessionPattern) + delta, 0, last));
  // Keep the cursor row on screen.
  if (state_.sessionPattern < state_.sessionScroll) state_.sessionScroll = state_.sessionPattern;
  if (state_.sessionPattern >= state_.sessionScroll + kRows) state_.sessionScroll = state_.sessionPattern - (kRows - 1);
}

void Controller::SessionLaunchRow()
{
  auto& sequencer = host_.Sequencer();
  if (sequencer.SongMode()) sequencer.SetSongMode(false);
  std::size_t launched = 0;
  for (std::size_t track = 0; track < kTrackCount; ++track)
    if (state_.sessionPattern < sequencer.PatternCount(track) && sequencer.QueuePattern(track, state_.sessionPattern)) ++launched;
  ShowHud(sequencer.Playing() ? "NEXT ROW" : "ROW", PatternLabel(state_.sessionPattern) + "  " + std::to_string(launched) + "/4 TRACKS");
}

void Controller::SessionCapture()
{
  // What every track plays right now (or has queued) becomes the next SONG row.
  auto& sequencer = host_.Sequencer();
  const auto ui = sequencer.BuildUiState(state_.track);
  synth::SongRow row{};
  for (std::size_t track = 0; track < kTrackCount; ++track)
  {
    const auto& summary = ui.tracks[track];
    const std::size_t pattern = summary.queuedPattern < summary.patternCount ? summary.queuedPattern : summary.activePattern;
    row.patterns[track] = static_cast<std::int8_t>(pattern);
    // The row lasts as long as its longest pattern (a bar is 16 steps).
    const std::size_t steps = sequencer.PatternStepCount(track, pattern);
    row.repeats = std::max(row.repeats, static_cast<std::uint8_t>(std::min<std::size_t>((steps + synth::kSongBarSteps - 1) / synth::kSongBarSteps, synth::kSongMaxRepeats)));
  }
  const std::size_t at = sequencer.SongLength();
  if (!sequencer.InsertSongRow(at, row))
  {
    ShowHud("SONG", "FULL", 1.0f);
    return;
  }
  MarkDirty();
  ShowHud("CAPTURED", "SONG ROW " + std::to_string(at + 1));
}

// ---------------------------------------------------------------------------------------------
// MIX

void Controller::MixGrid(std::size_t index)
{
  const std::size_t row = index / kGridColumns;
  const std::size_t track = index % kGridColumns;
  auto& sequencer = host_.Sequencer();
  switch (row)
  {
    case 0:
      state_.mixFocus = static_cast<std::uint8_t>(track);
      SelectTrack(track);
      break;
    case 1:
      ToggleMute(track);
      break;
    case 2:
      host_.SetTrackSoloed(track, !host_.TrackSoloed(track));
      MarkDirty();
      break;
    case 3:
      sequencer.SetTrackEnabled(track, !sequencer.TrackEnabled(track));
      MarkDirty();
      break;
    default: break;
  }
}

void Controller::MixAdjust(int delta, bool coarse)
{
  const int step = delta * (coarse ? 10 : 1);
  if (state_.mixFocus >= kTrackCount)
  {
    host_.SetMasterVolume(static_cast<std::uint8_t>(std::clamp(static_cast<int>(host_.MasterVolume()) + step, 0, 100)));
    return;
  }
  const std::size_t track = state_.mixFocus;
  host_.SetTrackLevel(track, static_cast<std::uint8_t>(std::clamp(static_cast<int>(host_.TrackLevel(track)) + step, 0, 100)));
  MarkDirty();
}

void Controller::MixEnter()
{
  state_.mixFocus = state_.mixFocus >= kTrackCount ? static_cast<std::uint8_t>(state_.track) : static_cast<std::uint8_t>(kTrackCount);
}

// ---------------------------------------------------------------------------------------------
// SENDS: GRID rows are the track effects (filter, chorus, delay send, reverb send), columns the
// tracks. The values are the "fx.*" parameters of each track's engine.

void Controller::SetSendValue(std::size_t track, const char* id, int value)
{
  synth::ISynthEngine* engine = host_.Engine(track);
  if (engine != nullptr && engine->SetParameterValue(id, value)) MarkDirty();
}

void Controller::SendsGrid(std::size_t index)
{
  if (index >= kGridCount) return;
  const std::size_t track = index % kGridColumns;
  // A second press on a focused chorus cell steps its mode, like a switch on an INSTR page.
  if (state_.sendsFocus == index && index / kGridColumns == 1)
  {
    state_.cellPress.stepOnRelease = true;
    return;
  }
  state_.sendsFocus = static_cast<std::uint8_t>(index);
  SelectTrack(track);
}

void Controller::SendsAdjust(int delta, bool coarse)
{
  const std::size_t track = state_.sendsFocus % kGridColumns;
  const char* id = kSendIds[state_.sendsFocus / kGridColumns];
  synth::ISynthEngine* engine = host_.Engine(track);
  synth::SynthParameterInfo info;
  if (engine == nullptr || !engine->GetParameterInfo(id, info)) return;
  const int scale = coarse && info.kind == synth::SynthParameterKind::Continuous ? 10 : 1;
  if (engine->AdjustParameterValue(id, delta * scale)) MarkDirty();
}

void Controller::SendsEnter()
{
  // ENTER steps the chorus mode and switches a filter or send off and back on.
  const std::size_t track = state_.sendsFocus % kGridColumns;
  const std::size_t row = state_.sendsFocus / kGridColumns;
  const char* id = kSendIds[row];
  synth::ISynthEngine* engine = host_.Engine(track);
  synth::SynthParameterInfo info;
  if (engine == nullptr || !engine->GetParameterInfo(id, info)) return;
  if (info.kind == synth::SynthParameterKind::Choice)
  {
    SetSendValue(track, id, info.value >= info.maxValue ? info.minValue : info.value + 1);
    return;
  }
  // The memory holds value + 1; 0 means nothing was switched off yet.
  auto& memory = state_.sendsMemory[state_.sendsFocus];
  if (info.value != info.defaultValue)
  {
    memory = static_cast<std::uint8_t>(info.value + 1);
    SetSendValue(track, id, info.defaultValue);
  }
  else if (memory != 0) SetSendValue(track, id, memory - 1);
}

void Controller::SendsReset()
{
  const std::size_t track = state_.sendsFocus % kGridColumns;
  const char* id = kSendIds[state_.sendsFocus / kGridColumns];
  synth::ISynthEngine* engine = host_.Engine(track);
  synth::SynthParameterInfo info;
  if (engine == nullptr || !engine->GetParameterInfo(id, info)) return;
  SetSendValue(track, id, info.defaultValue);
  ShowToast("T" + std::to_string(track + 1) + " " + kSendTags[state_.sendsFocus / kGridColumns] + " DEFAULT");
}

// ---------------------------------------------------------------------------------------------
// FX: the master bus. GRID rows: delay, reverb, master; values are project settings.

void Controller::FxGrid(std::size_t index)
{
  if (index < synth::fx::kMasterParamCount) state_.fxFocus = static_cast<std::uint8_t>(index);
}

void Controller::FxAdjust(int delta, bool coarse)
{
  const std::size_t param = state_.fxFocus;
  if (param >= synth::fx::kMasterParamCount) return;
  const auto& info = synth::fx::kMasterParams[param];
  const int scale = coarse && info.maxValue - info.minValue >= 40 ? 10 : 1;
  const int value = std::clamp(host_.MasterFx(param) + delta * scale, info.minValue, info.maxValue);
  if (value == host_.MasterFx(param)) return;
  host_.SetMasterFx(param, value);
  MarkDirty();
}

void Controller::FxEnter()
{
  // The delay time is a list of note divisions; ENTER walks it.
  if (state_.fxFocus != static_cast<std::uint8_t>(synth::fx::MasterParam::DelayTime)) return;
  const auto& info = synth::fx::kMasterParams[state_.fxFocus];
  const int value = host_.MasterFx(state_.fxFocus);
  host_.SetMasterFx(state_.fxFocus, value >= info.maxValue ? info.minValue : value + 1);
  MarkDirty();
}

void Controller::FxReset()
{
  if (state_.fxFocus >= synth::fx::kMasterParamCount) return;
  host_.SetMasterFx(state_.fxFocus, synth::fx::kMasterParams[state_.fxFocus].defaultValue);
  MarkDirty();
  ShowToast(std::string(synth::fx::kMasterParams[state_.fxFocus].label) + " DEFAULT");
}

// ---------------------------------------------------------------------------------------------
// Fn functions

bool Controller::SongPage() const
{
  return state_.scene == Scene::Tape && CurrentPage() == kTapeSong;
}

void Controller::Copy()
{
  if (SongPage())
  {
    auto& sequencer = host_.Sequencer();
    if (state_.songRow >= sequencer.SongLength()) return;
    state_.songClipboard = sequencer.SongRowAt(state_.songRow);
    state_.songClipboardValid = true;
    ShowToast("COPIED ROW " + std::to_string(state_.songRow + 1));
    return;
  }
  if (state_.scene != Scene::Sequence)
  {
    ShowToast("COPY WORKS IN SEQ AND TAPE");
    return;
  }
  const auto ui = host_.Sequencer().BuildUiState(state_.track);
  if (CurrentPage() == 0 && ui.hasHeldSteps)
  {
    state_.clipboardIsPattern = false;
    state_.clipboardIndex = ui.primaryHeldStep;
    state_.clipboardValid = true;
    ShowToast("COPIED STEP " + std::to_string(ui.primaryHeldStep + 1));
    return;
  }
  state_.clipboardIsPattern = true;
  state_.clipboardIndex = ui.tracks[state_.track].activePattern;
  state_.clipboardValid = true;
  ShowToast("COPIED " + PatternLabel(state_.clipboardIndex));
}

void Controller::Paste()
{
  if (SongPage())
  {
    auto& sequencer = host_.Sequencer();
    if (!state_.songClipboardValid) return;
    // Paste inserts the copied row after the cursor.
    const std::size_t at = std::min(state_.songRow + 1, sequencer.SongLength());
    if (sequencer.InsertSongRow(at, state_.songClipboard))
    {
      state_.songRow = at;
      SongArrow(0, 0);
      MarkDirty();
      ShowToast("PASTED ROW " + std::to_string(at + 1), ToastKind::Ok);
    }
    return;
  }
  if (state_.scene != Scene::Sequence || !state_.clipboardValid)
  {
    ShowToast("NOTHING TO PASTE");
    return;
  }
  auto& sequencer = host_.Sequencer();
  const auto ui = sequencer.BuildUiState(state_.track);
  if (!state_.clipboardIsPattern)
  {
    if (!ui.hasHeldSteps)
    {
      ShowToast("SELECT STEPS TO PASTE");
      return;
    }
    for (std::size_t step = 0; step < synth::kSequencerMaxSteps; ++step)
      if (step != state_.clipboardIndex && sequencer.StepSelected(step)) (void)sequencer.CopyStep(state_.clipboardIndex, step);
    MarkDirty();
    ShowToast("PASTED", ToastKind::Ok);
    return;
  }
  const std::size_t destination = ui.tracks[state_.track].activePattern;
  if (destination == state_.clipboardIndex) return;
  if (sequencer.CopyPattern(state_.clipboardIndex, destination))
  {
    MarkDirty();
    ShowToast("PASTED TO " + PatternLabel(destination), ToastKind::Ok);
  }
}

void Controller::Clear()
{
  if (SongPage())
  {
    auto& sequencer = host_.Sequencer();
    if (state_.songRow >= sequencer.SongLength()) return;
    if (sequencer.DeleteSongRow(state_.songRow))
    {
      MarkDirty();
      ShowToast("ROW " + std::to_string(state_.songRow + 1) + " DELETED");
      state_.songRow = std::min(state_.songRow, sequencer.SongLength());
      SongArrow(0, 0);
    }
    return;
  }
  if (state_.scene != Scene::Sequence) return;
  auto& sequencer = host_.Sequencer();
  const auto ui = sequencer.BuildUiState(state_.track);
  if (CurrentPage() == 0 && ui.hasHeldSteps)
  {
    sequencer.ClearHeldStepEvents();
    MarkDirty();
    ShowToast("STEPS CLEARED");
    return;
  }
  const std::size_t pattern = ui.tracks[state_.track].activePattern;
  if (!ui.patternHasEvents[pattern]) return;
  OpenConfirm(ConfirmPurpose::ClearPattern, "CLEAR " + PatternLabel(pattern) + "?", "ALL NOTES IN THIS PATTERN", true);
}

void Controller::Quantize()
{
  if (state_.scene != Scene::Sequence) return;
  auto& sequencer = host_.Sequencer();
  const auto ui = sequencer.BuildUiState(state_.track);
  if (CurrentPage() == 0 && ui.hasHeldSteps) sequencer.QuantizeHeldSteps();
  else sequencer.QuantizeActivePattern();
  MarkDirty();
  ShowToast("QUANTIZED");
}

void Controller::Duplicate()
{
  auto& sequencer = host_.Sequencer();
  if (SongPage())
  {
    if (state_.songRow >= sequencer.SongLength()) return;
    if (sequencer.InsertSongRow(state_.songRow + 1, sequencer.SongRowAt(state_.songRow)))
    {
      ++state_.songRow;
      SongArrow(0, 0);
      MarkDirty();
      ShowToast("ROW DUPLICATED", ToastKind::Ok);
    }
    return;
  }
  const auto ui = sequencer.BuildUiState(state_.track);
  const auto& track = ui.tracks[state_.track];
  if (track.patternCount >= synth::kSequencerMaxPatternCount)
  {
    ShowToast("16 PATTERNS MAX", ToastKind::Error);
    return;
  }
  if (sequencer.CopyPattern(track.activePattern, track.patternCount))
  {
    MarkDirty();
    ShowToast("DUPLICATED TO " + PatternLabel(track.patternCount), ToastKind::Ok);
  }
}

void Controller::TapTempo()
{
  const std::uint32_t now = state_.now;
  if (state_.tapCount > 0 && now - state_.taps[(state_.tapCount - 1) % state_.taps.size()] > 2000) state_.tapCount = 0;
  state_.taps[state_.tapCount % state_.taps.size()] = now;
  ++state_.tapCount;
  if (state_.tapCount < 2)
  {
    ShowToast("TAP");
    return;
  }
  const std::size_t samples = std::min(state_.tapCount, state_.taps.size());
  const std::uint32_t first = state_.taps[(state_.tapCount - samples) % state_.taps.size()];
  const std::uint32_t interval = (now - first) / static_cast<std::uint32_t>(samples - 1);
  if (interval == 0) return;
  const int bpm = std::clamp(static_cast<int>((60000 + interval / 2) / interval), 30, 300);
  host_.Sequencer().SetTempo(static_cast<std::uint16_t>(bpm));
  MarkDirty();
  ShowHud("TAP TEMPO", std::to_string(bpm), static_cast<float>(bpm - 30) / 270.0f);
}

void Controller::QuickSave()
{
  SaveProject(false);
}

void Controller::ToggleMute(std::size_t track, bool announce)
{
  if (track >= kTrackCount) return;
  const bool muted = !host_.TrackMuted(track);
  host_.SetTrackMuted(track, muted);
  MarkDirty();
  if (announce) ShowHud("T" + std::to_string(track + 1), muted ? "MUTED" : "ON");
}

// What FN does besides the menu: the combinations with the keys around it.
std::vector<Controller::FnEntry> Controller::FnEntries() const
{
  return {
      {"GRID", "MENU", true},
      {"6/Y", "BPM+-", true},
      {"7/U", "VOL+-", true},
      {"8/I", "OCT+-", true},
      {"+-", state_.scene == Scene::Sequence && CurrentPage() == 0 && !host_.Sequencer().HasHeldSteps() ? "PATTERN" : "BIG STEP", true},
      {"CTRL", "PREV PAGE", PageCount(state_.scene) > 1},
      {"TAB", "PREV TRK", true},
      {"ESC", "STOP", true},
      {"PLAY", "REC", true},
  };
}

// ---------------------------------------------------------------------------------------------
// FN menu: what the current screen can do. Rows 0-2 belong to the page, row 3 is the same on every
// screen. An item with a key also runs on FN + that key.

void Controller::OpenMenu()
{
  Modal modal;
  modal.kind = ModalKind::Menu;
  modal.title = "MENU";
  OpenModal(std::move(modal));
}

MenuItems Controller::Menu() const
{
  MenuItems menu{};
  auto put = [&](std::size_t index, MenuAction action, const char* label, std::string detail, const char* key = nullptr, bool available = true, bool on = false) {
    menu[index] = MenuItem{action, label, std::move(detail), key, available, on};
  };
  const auto& sequencer = host_.Sequencer();
  const auto ui = sequencer.BuildUiState(state_.track);
  const std::size_t page = CurrentPage();

  switch (state_.scene)
  {
    case Scene::Instrument:
    {
      // The sound of this track: presets, the engine, and the focused value back to default.
      put(0, MenuAction::Presets, "PRESETS", "BROWSE");
      put(1, MenuAction::SavePreset, "SAVE", "PRESET");
      put(2, MenuAction::SavePresetAs, "SAVE AS", "PRESET");
      put(3, MenuAction::Engine, "ENGINE", std::string(EngineShortName(ActiveKind())));
      const InstrumentPage* instrumentPage = CurrentInstrumentPage();
      const Slot& slot = instrumentPage->slots[InstrumentFocus()];
      const bool param = instrumentPage->graphic != PageGraphic::Voice && slot.IsParameter();
      put(4, MenuAction::Default, "RESET", param ? slot.label : "", "OK", param);
      if (ActiveKind() == synth::SynthEngineKind::Sampler)
      {
        put(8, MenuAction::SampleFiles, "SAMPLES", "FILES");
        put(9, MenuAction::SampleRecord, "RECORD", "SAMPLE");
        put(10, MenuAction::SamplePreview, "PREVIEW", "SAMPLE");
      }
      break;
    }
    case Scene::Sequence:
    {
      // Copy / paste / clear / quantize work on the selected steps, or else the whole pattern.
      const bool steps = page == 0 && ui.hasHeldSteps;
      const std::string target = steps ? "STEPS" : "PATTERN";
      const bool pasteable = state_.clipboardValid && (state_.clipboardIsPattern || steps);
      put(0, MenuAction::Copy, "COPY", target, "C");
      put(1, MenuAction::Paste, "PASTE", target, "V", pasteable);
      put(2, MenuAction::Clear, "CLEAR", target, "X");
      put(3, MenuAction::Quantize, "QUANT", target, "Q");
      put(4, MenuAction::Duplicate, "DUP", "PATTERN", "D", ui.tracks[state_.track].patternCount < synth::kSequencerMaxPatternCount);
      put(5, MenuAction::View, "VIEW", PageTab(Scene::Sequence, 0), nullptr, true, View() == SeqView::Step);
      put(6, MenuAction::RecordPanel, "RECORD", "TAKES");
      put(7, MenuAction::Scope, "SCOPE", steps ? ui.scopeLabel : "", "OK", steps);
      // The pages of a long pattern, and how long it is.
      const std::string pages = std::to_string(ui.viewPage + 1) + "/" + std::to_string(ui.viewPageCount);
      const std::string length = std::to_string(ui.tracks[state_.track].stepCount);
      put(8, MenuAction::PagePrev, "PAGE -", pages, nullptr, page == 0 && ui.viewPageCount > 1);
      put(9, MenuAction::PageNext, "PAGE +", pages, nullptr, page == 0 && ui.viewPageCount > 1);
      put(10, MenuAction::LengthDown, "LEN -", length, nullptr, ui.tracks[state_.track].stepCount > 1);
      put(11, MenuAction::LengthUp, "LEN +", length, nullptr, ui.tracks[state_.track].stepCount < synth::kSequencerMaxSteps);
      break;
    }
    case Scene::Tape:
      if (page == kTapeSession)
      {
        put(0, MenuAction::LaunchRow, "LAUNCH", "ROW " + std::to_string(state_.sessionPattern + 1));
        put(1, MenuAction::Capture, "ADD", "TO SONG", "OK");
      }
      else if (page == kTapeSong)
      {
        const bool row = state_.songRow < ui.songLength;
        const std::string target = "ROW " + std::to_string(state_.songRow + 1);
        put(0, MenuAction::Copy, "COPY", target, "C", row);
        put(1, MenuAction::Paste, "PASTE", "AFTER", "V", state_.songClipboardValid);
        put(2, MenuAction::Clear, "DELETE", target, "X", row);
        put(3, MenuAction::Duplicate, "DUP", target, "D", row);
        put(4, MenuAction::SongMode, "PLAY", sequencer.SongMode() ? "SONG" : "LOOP", "OK", true, sequencer.SongMode());
      }
      else if (page == kTapeMix)
      {
        bool muted = false;
        bool soloed = false;
        for (std::size_t track = 0; track < kTrackCount; ++track)
        {
          muted = muted || host_.TrackMuted(track);
          soloed = soloed || host_.TrackSoloed(track);
        }
        put(0, MenuAction::UnmuteAll, "UNMUTE", "ALL", nullptr, muted);
        put(1, MenuAction::UnsoloAll, "UNSOLO", "ALL", nullptr, soloed);
      }
      else
      {
        put(0, MenuAction::Default, "RESET", page == kTapeSends ? kSendTags[state_.sendsFocus / kGridColumns] : "PARAM", "OK");
      }
      break;
    case Scene::Setup:
      if (page < kSetupKeys)
      {
        put(0, MenuAction::NewProject, "NEW", "PROJECT");
        put(1, MenuAction::LoadProject, "LOAD", "PROJECT");
        put(2, MenuAction::SaveProjectAs, "SAVE AS", "PROJECT");
      }
      else if (const InstrumentPage* settings = SettingsPageShown())
      {
        const Slot& slot = settings->slots[SettingsFocus()];
        put(0, MenuAction::Default, "RESET", slot.IsParameter() ? slot.label : "", "OK", slot.IsParameter());
      }
      break;
  }

  // Row 3, the same everywhere: save the project, tap the tempo, arm recording, and SETUP (from
  // SETUP: back to the scene it was opened from).
  put(12, MenuAction::Save, "SAVE", state_.dirty ? "UNSAVED" : "PROJECT", "S");
  put(13, MenuAction::TapTempo, "TAP", std::to_string(ui.bpm) + " BPM", "T");
  put(14, MenuAction::Record, "REC", sequencer.RecordArmed() ? "ARMED" : "OFF", "PLAY", true, sequencer.RecordArmed());
  if (state_.scene == Scene::Setup)
  {
    const Scene back = IsMusicScene(state_.previousScene) ? state_.previousScene : Scene::Instrument;
    put(15, MenuAction::Setup, "BACK", kSceneNames[static_cast<std::size_t>(back)]);
  }
  else put(15, MenuAction::Setup, "SETUP", "GLOBAL");
  return menu;
}

void Controller::RunMenu(std::size_t index)
{
  if (index >= kGridCount) return;
  const MenuItem item = Menu()[index];
  if (item.action == MenuAction::None) return;
  if (!item.available)
  {
    ShowToast(item.label + std::string(" NOT HERE"));
    return;
  }
  // The open menu closes first, so an item that opens a pop-up opens it over the screen.
  if (MenuOpen()) CloseModal();
  const std::uint32_t toastBefore = state_.toast.until;
  const std::uint32_t hudBefore = state_.hud.at;
  const Modal* modalBefore = TopModal();
  RunMenuAction(item);
  // Every item says what it did: one that showed nothing (and opened no pop-up) gets its own name and
  // new state in the footer, e.g. VIEW STEP.
  if (state_.toast.until == toastBefore && state_.hud.at == hudBefore && TopModal() == modalBefore)
  {
    const MenuItem now = Menu()[index];
    const MenuItem& shown = now.action == item.action ? now : item;
    ShowToast(shown.detail.empty() ? shown.label : shown.label + std::string(" ") + shown.detail);
  }
}

void Controller::RunMenuAction(const MenuItem& item)
{
  switch (item.action)
  {
    case MenuAction::Save: QuickSave(); break;
    case MenuAction::TapTempo: TapTempo(); break;
    case MenuAction::Record: ToggleRecord(); break;
    case MenuAction::Setup:
      if (state_.scene == Scene::Setup) Back();
      else GoToScene(Scene::Setup);
      break;
    case MenuAction::Presets: RunSlotAction(SlotAction::Preset); break;
    case MenuAction::SavePreset: RunSlotAction(SlotAction::SavePreset); break;
    case MenuAction::SavePresetAs: RunSlotAction(SlotAction::SavePresetAs); break;
    case MenuAction::Engine: RunSlotAction(SlotAction::Engine); break;
    case MenuAction::SampleFiles: RunSlotAction(SlotAction::SampleFile); break;
    case MenuAction::SampleRecord: RunSlotAction(SlotAction::SampleRecord); break;
    case MenuAction::SamplePreview: RunSlotAction(SlotAction::SamplePreview); break;
    case MenuAction::Default:
    case MenuAction::Scope:
    case MenuAction::Capture:
    case MenuAction::SongMode: FnEnter(); break;
    case MenuAction::Copy: Copy(); break;
    case MenuAction::Paste: Paste(); break;
    case MenuAction::Clear: Clear(); break;
    case MenuAction::Quantize: Quantize(); break;
    case MenuAction::Duplicate: Duplicate(); break;
    case MenuAction::View: ToggleSeqView(); break;
    case MenuAction::PagePrev: FlipStepPage(-1); break;
    case MenuAction::PageNext: FlipStepPage(1); break;
    case MenuAction::LengthDown: AdjustPatternLength(-1, true); break;
    case MenuAction::LengthUp: AdjustPatternLength(1, true); break;
    case MenuAction::RecordPanel:
      if (CurrentPage() != 0) SetPage(0);
      OpenRecord();
      break;
    case MenuAction::LaunchRow: SessionLaunchRow(); break;
    case MenuAction::UnmuteAll:
    case MenuAction::UnsoloAll:
      for (std::size_t track = 0; track < kTrackCount; ++track)
      {
        if (item.action == MenuAction::UnmuteAll) host_.SetTrackMuted(track, false);
        else host_.SetTrackSoloed(track, false);
      }
      MarkDirty();
      ShowHud("MIX", item.action == MenuAction::UnmuteAll ? "ALL UNMUTED" : "SOLO OFF");
      break;
    case MenuAction::NewProject:
      if (state_.dirty) OpenConfirm(ConfirmPurpose::NewProject, "NEW PROJECT?", "UNSAVED CHANGES WILL BE LOST", true);
      else NewProject();
      break;
    case MenuAction::LoadProject:
      GoToScene(Scene::Setup);
      SetPage(kSetupFiles);
      break;
    case MenuAction::SaveProjectAs: SaveProject(true); break;
    case MenuAction::None:
    default: break;
  }
}

}  // namespace groove
