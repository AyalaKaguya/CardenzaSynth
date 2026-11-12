#include "Applications/Synth/UI/SynthUiModel.h"

#include "Applications/Synth/UI/Dx7/Dx7UiModel.h"
#include "Applications/Synth/UI/Juno/JunoUiModel.h"
#include "Applications/Synth/UI/Menu/SynthUiMenu.h"
#include "Applications/Synth/UI/Sampler/SamplerUiModel.h"
#include "Devices/Device.h"
#include "Library/Synth/Core/Engines/Drums/DrumsSynthEngine.h"
#include "Library/Synth/Core/SynthStorage.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <sstream>
#include <set>
#include <utility>

#if defined(ESP_PLATFORM)
#include <dirent.h>
#endif

namespace synth {

using namespace ui_model;

namespace {

constexpr float kTransitionStep = 0.18f;
constexpr auto kModifierHoldDelay = std::chrono::milliseconds(200);
constexpr auto kContextHintHoldDelay = std::chrono::milliseconds(450);
constexpr auto kPresetBrowserAutoScrollInterval = std::chrono::milliseconds(90);
constexpr auto kHomeOctaveStatusDuration = std::chrono::seconds(1);
constexpr int kKeyboardOctaveMin = -3;
constexpr int kKeyboardOctaveMax = 3;
constexpr std::array<const char*, 4> kModifierBarLabels = {"TRK", "SND", "KEY", "SYS"};
constexpr std::array<UiBarIcon, 4> kHomeActionBarIcons = {
  UiBarIcon::Plus,
  UiBarIcon::Minus,
  UiBarIcon::Stop,
  UiBarIcon::Play,
};
constexpr std::size_t kPresetSearchMaxLength = 18;
constexpr std::size_t kSamplerNameMaxLength = 18;
constexpr std::size_t kGenericParameterPageSize = 6;
constexpr std::size_t kSelectableListVisibleRows = 7;
constexpr std::uint16_t kScaleMinor = 0b010110101101;
constexpr std::uint16_t kScaleMajor = 0b101010110101;
constexpr std::uint16_t kScaleDorian = 0b011010101101;
constexpr std::uint16_t kScaleMixolydian = 0b011010110101;
constexpr std::uint16_t kScaleMinorPentatonic = 0b010010101001;
constexpr std::uint16_t kScaleMajorPentatonic = 0b001010010101;
constexpr std::uint16_t kScaleBlues = 0b010011101001;
constexpr std::uint16_t kScaleHarmonicMinor = 0b100110101101;

struct KeyboardScaleDefinition {
  const char* label;
  const char* fullName;
  std::uint16_t mask;
};

constexpr std::array<KeyboardScaleDefinition, 8> kKeyboardScales = {{
    {"MAJ", "Major", kScaleMajor},
    {"MIN", "Minor", kScaleMinor},
    {"DOR", "Dorian", kScaleDorian},
    {"MIX", "Mixolydian", kScaleMixolydian},
    {"MPEN", "Minor Pentatonic", kScaleMinorPentatonic},
    {"PENT", "Major Pentatonic", kScaleMajorPentatonic},
    {"BLUE", "Blues", kScaleBlues},
    {"HMIN", "Harmonic Minor", kScaleHarmonicMinor},
}};

constexpr std::array<const char*, 12> kKeyboardRootNames = {
  "C",
  "C#",
  "D",
  "D#",
  "E",
  "F",
  "F#",
  "G",
  "G#",
  "A",
  "A#",
  "B",
};

constexpr std::array<std::array<const char*, kKeyboardMatrixColumns>, kKeyboardMatrixRows> kPresetSearchKeyMap = {{
    {{"", "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "-", "=", ""}},
    {{"", "q", "w", "e", "r", "t", "y", "u", "i", "o", "p", "[", "]", ""}},
    {{"", "", "a", "s", "d", "f", "g", "h", "j", "k", "l", ";", "", ""}},
    {{"", "", "", "z", "x", "c", "v", "b", "n", "m", ",", ".", "", ""}},
}};

constexpr std::size_t CountScaleNotes(std::uint16_t mask)
{
  std::size_t count = 0;
  for (std::size_t bit = 0; bit < 12; ++bit)
  {
    count += (mask & (static_cast<std::uint16_t>(1u) << bit)) != 0 ? 1 : 0;
  }
  return count;
}

static_assert(CountScaleNotes(kScaleMinor) <= kHomePerformanceColumnCount);
static_assert(CountScaleNotes(kScaleMajor) <= kHomePerformanceColumnCount);
static_assert(CountScaleNotes(kScaleDorian) <= kHomePerformanceColumnCount);
static_assert(CountScaleNotes(kScaleMixolydian) <= kHomePerformanceColumnCount);
static_assert(CountScaleNotes(kScaleMinorPentatonic) <= kHomePerformanceColumnCount);
static_assert(CountScaleNotes(kScaleMajorPentatonic) <= kHomePerformanceColumnCount);
static_assert(CountScaleNotes(kScaleBlues) <= kHomePerformanceColumnCount);
static_assert(CountScaleNotes(kScaleHarmonicMinor) <= kHomePerformanceColumnCount);

int PositiveModulo(int value, int modulo)
{
  const int remainder = value % modulo;
  return remainder < 0 ? remainder + modulo : remainder;
}

const KeyboardScaleDefinition& KeyboardScaleAt(std::size_t index)
{
  return kKeyboardScales[index % kKeyboardScales.size()];
}

int KeyboardScaleDegreeOffset(std::uint16_t mask, std::size_t degree)
{
  const std::size_t noteCount = std::max<std::size_t>(1, CountScaleNotes(mask));
  const std::size_t wrappedDegree = degree % noteCount;
  std::size_t currentDegree = 0;
  for (int note = 0; note < 12; ++note)
  {
    if ((mask & (static_cast<std::uint16_t>(1u) << note)) == 0)
    {
      continue;
    }

    if (currentDegree == wrappedDegree)
    {
      return note;
    }
    ++currentDegree;
  }
  return 0;
}

const char* KeyboardLayoutModeLabel(KeyboardLayoutMode mode)
{
  switch (mode)
  {
    case KeyboardLayoutMode::Piano:
      return "Piano";
    case KeyboardLayoutMode::Octave:
      return "Octave";
    case KeyboardLayoutMode::Chromatic:
      return "Chromatic";
    default:
      return "Keyboard";
  }
}

bool IsSamplerNameCharacter(const char* mappedValue)
{
  return mappedValue != nullptr && mappedValue[0] != '\0' && mappedValue[1] == '\0' &&
         (std::isalnum(static_cast<unsigned char>(mappedValue[0])) || mappedValue[0] == '-');
}

bool HasWavExtension(const std::string& name)
{
  if (name.size() < 4)
  {
    return false;
  }

  std::string extension = name.substr(name.size() - 4);
  std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  return extension == ".wav";
}

std::vector<std::string> CollectSamplerBrowserEntries()
{
  std::set<std::string> sortedNames;
  const std::string sampleDirectory = synth::storage::SampleDirectory();
  if (!synth::storage::EnsureLayout() || sampleDirectory.empty())
  {
    return {};
  }

#if defined(ESP_PLATFORM)
  DIR* directory = opendir(sampleDirectory.c_str());
  if (directory == nullptr)
  {
    return {};
  }

  while (const dirent* entry = readdir(directory))
  {
    if (entry->d_name[0] == '\0')
    {
      continue;
    }

    const std::string fileName(entry->d_name);
    if (!HasWavExtension(fileName))
    {
      continue;
    }
    sortedNames.insert(fileName);
  }

  closedir(directory);
#else
  namespace fs = std::filesystem;
  std::error_code error;
  for (const fs::directory_entry& entry : fs::directory_iterator(sampleDirectory, error))
  {
    if (error)
    {
      break;
    }
    if (!entry.is_regular_file(error))
    {
      continue;
    }

    const std::string fileName = entry.path().filename().string();
    if (!HasWavExtension(fileName))
    {
      continue;
    }
    sortedNames.insert(fileName);
  }
#endif

  return std::vector<std::string>(sortedNames.begin(), sortedNames.end());
}

std::vector<std::string> FilterSamplerBrowserEntries(const std::vector<std::string>& entries, const std::string& searchText)
{
  const std::string normalizedSearch = ToLower(searchText);
  std::vector<std::string> matches;
  matches.reserve(entries.size());
  for (const std::string& entry : entries)
  {
    if (ContainsCaseInsensitive(entry, normalizedSearch))
    {
      matches.push_back(entry);
    }
  }
  return matches;
}

std::string BuildSelectableListText(const std::vector<std::string>& items, std::size_t selectedIndex)
{
  if (items.empty())
  {
    return "No samples\nAdd WAV to Sample";
  }

  const std::size_t clampedSelection = std::min(selectedIndex, items.size() - 1);
  const std::size_t firstVisible = clampedSelection == 0 ? 0 : clampedSelection - 1;
  std::ostringstream stream;
  const std::size_t lastVisible = std::min(items.size(), firstVisible + kSelectableListVisibleRows);
  for (std::size_t index = firstVisible; index < lastVisible; ++index)
  {
    if (index > firstVisible)
    {
      stream << '\n';
    }

    stream << (index == clampedSelection ? ">" : " ") << items[index];
  }

  return stream.str();
}

std::string BuildSamplerBrowserListText(const std::vector<std::string>& items,
                                        const std::string& searchText,
                                        std::size_t selectedIndex)
{
  if (items.empty())
  {
    return searchText.empty() ? "No samples\nAdd WAV to Sample" : "No matches";
  }

  return BuildSelectableListText(items, selectedIndex);
}

ContextMenuGrid BuildMenuAt(std::initializer_list<std::pair<std::size_t, ContextMenuCell>> cells)
{
  ContextMenuGrid menu = BlankGrid();
  for (const auto& entry : cells)
  {
    if (entry.first < menu.size())
    {
      menu[entry.first] = entry.second;
    }
  }
  return menu;
}

std::pair<std::string, std::string> MenuItemPresentation(UiModifier modifier,
                                                         const std::string& itemId,
                                                         const std::string& patchName,
                                                         const ISynthEngine* engine)
{
  const std::string engineName = engine != nullptr ? engine->Descriptor().displayName : "SYNTH";

  if (modifier == UiModifier::Sound)
  {
    if (itemId == "snd.dx7.op1") return {"Operator 1", Dx7GroupSummary(engine, "par.dx7.group.op1")};
    if (itemId == "snd.dx7.op2") return {"Operator 2", Dx7GroupSummary(engine, "par.dx7.group.op2")};
    if (itemId == "snd.dx7.op3") return {"Operator 3", Dx7GroupSummary(engine, "par.dx7.group.op3")};
    if (itemId == "snd.dx7.op4") return {"Operator 4", Dx7GroupSummary(engine, "par.dx7.group.op4")};
    if (itemId == "snd.dx7.op5") return {"Operator 5", Dx7GroupSummary(engine, "par.dx7.group.op5")};
    if (itemId == "snd.dx7.op6") return {"Operator 6", Dx7GroupSummary(engine, "par.dx7.group.op6")};
    if (itemId == "snd.dx7.alg") return {"Algorithm", Dx7GroupSummary(engine, "par.dx7.group.alg")};
    if (itemId == "snd.dx7.glb") return {"Global", Dx7GroupSummary(engine, "par.dx7.group.glb")};
    if (itemId == "snd.engine") return {"Engine", engineName};
    if (itemId == "snd.preset") return {"Preset", patchName};
    if (itemId == "snd.save") return {"Save Preset", patchName};
    if (itemId == "snd.saveas") return {"Save Preset As", patchName};
    if (itemId == "snd.volume") return {"Volume", "92"};
    if (itemId == "snd.filter") return {"Filter", "72"};
  }

  if (modifier == UiModifier::Parameter)
  {
    if (itemId == "par.sampler.group.samp") return {"File", "Load, save, rename and manage sample files"};
    if (itemId == "par.sampler.group.trim") return {"Trim", "Set start and end points for playback"};
    if (itemId == "par.sampler.group.root") return {"Root", "Set the sample's base note for keyboard playback"};
    if (itemId == "par.sampler.group.loop") return {"Loop", "Choose one-shot or loop playback"};
    if (itemId == "par.sampler.group.amp") return {"Gain", "Set sample gain"};
    if (itemId == "par.sampler.group.filter") return {"Filter", "Set cutoff and resonance"};
    if (itemId == "par.sampler.group.env") return {"Envelope", "Set attack and release"};
    if (itemId == "par.prev") return {"Previous Parameter", "STEP"};
    if (itemId == "par.next") return {"Next Parameter", "STEP"};
    if (itemId == "par.fine") return {"Fine Adjust", "+12"};
    if (itemId == "par.rst") return {"Reset", "DEFAULT"};
    if (itemId == "par.min") return {"Minimum", "0"};
    if (itemId == "par.max") return {"Maximum", "127"};
    if (itemId == "par.lock") return {"Lock", "OFF"};
    if (itemId == "par.m1") return {"Macro 1", "UNMAPPED"};
    if (itemId == "par.m2") return {"Macro 2", "UNMAPPED"};
    if (itemId == "par.m3") return {"Macro 3", "UNMAPPED"};
    if (itemId == "par.m4") return {"Macro 4", "UNMAPPED"};
    if (itemId == "par.map") return {"Map Macro", "READY"};
    if (itemId == "par.clr") return {"Clear Macro", "READY"};
    if (itemId == "par.save") return {"Save Mapping", "READY"};
  }

  if (modifier == UiModifier::Layout)
  {
    if (itemId == "lay.scl") return {"Scale", "Select keyboard scale"};
    if (itemId == "lay.root") return {"Root", "Select root note"};
    if (itemId == "lay.mode") return {"Mode", "Select performance layout"};
    if (itemId == "lay.fx") return {"Effect", "EMPTY"};
  }

  if (modifier == UiModifier::Other)
  {
    if (itemId == "sys.project") return {"Project", "Save or load the complete four-track project"};
    if (itemId == "sys.seq") return {"Sequencer", "Transport, tempo, track and pattern settings"};
    if (itemId == "sys.seq.tempo") return {"Tempo", "Change BPM by one or ten"};
    if (itemId == "sys.seq.track") return {"Track", "Mute or enable the selected sequencer track"};
    if (itemId == "sys.seq.pattern") return {"Pattern", "Select a pattern or change the pattern count"};
  }

  return {"READY", patchName.empty() ? engineName : patchName};
}

std::pair<std::string, std::string> ActionHelp(ContextActionId actionId, const std::string& label)
{
  switch (actionId)
  {
    case ContextActionId::Back:
      return {label == "DONE" ? "Done" : "Back", "Return to the previous view"};
    case ContextActionId::EnginePrev:
    case ContextActionId::PresetPrev:
      return {"Previous", "Move selection up"};
    case ContextActionId::EngineNext:
    case ContextActionId::PresetNext:
      return {"Next", "Move selection down"};
    case ContextActionId::EngineLoad:
    case ContextActionId::PresetLoad:
      return {"Load", "Use the selected item"};
    case ContextActionId::PresetDelete:
    case ContextActionId::SamplerNameBackspace:
    case ContextActionId::PresetNameBackspace:
      return {"Delete Character", "Remove the last typed character"};
    case ContextActionId::PresetClear:
      return {"Clear Search", "Clear the current search text"};
    case ContextActionId::SavePreset:
      return {"Save Preset", "Update the current preset"};
    case ContextActionId::SavePresetAs:
      return {"Save Preset As", "Name preset copy"};
    case ContextActionId::PresetNameSave:
    case ContextActionId::SamplerNameSave:
      return {"Save Name", "Confirm the typed name"};
    case ContextActionId::SamplerRecord:
      return {"Record", "Arm sampler recording"};
    case ContextActionId::SamplerRecordStart:
      return {"Start Recording", "Record from audio input"};
    case ContextActionId::SamplerStop:
      return {"Stop", "Stop recording"};
    case ContextActionId::SamplerCancel:
      return {"Cancel", "Cancel the current sampler action"};
    case ContextActionId::SamplerPlay:
    case ContextActionId::SamplerTrimPreview:
      return {"Play", "Preview the sample"};
    case ContextActionId::SamplerEdit:
      return {"Edit", "Open rename and delete tools for the current sample"};
    case ContextActionId::SamplerSelect:
      return {"Files", "Open the sample browser"};
    case ContextActionId::SamplerName:
      return {"Name Sample", "Enter a sample file name"};
    case ContextActionId::SamplerConfirmOverwrite:
      return {"Replace", "Overwrite the existing file"};
    case ContextActionId::SamplerRename:
      return {"Rename", "Rename the current sample"};
    case ContextActionId::SamplerDelete:
      return {"Delete Sample", "Delete the current sample file"};
    case ContextActionId::SamplerTrimLeft:
      return {"Move Trim Left", "Move the trim cursor earlier"};
    case ContextActionId::SamplerTrimRight:
      return {"Move Trim Right", "Move the trim cursor later"};
    case ContextActionId::SamplerTrimBegin:
      return {"Set Trim Start", "Set sample start to the cursor"};
    case ContextActionId::SamplerTrimEnd:
      return {"Set Trim End", "Set sample end to the cursor"};
    case ContextActionId::SamplerTrimSet:
      return {"Save Trim", "Save the trimmed draft sample"};
    case ContextActionId::ParameterDecLarge:
    case ContextActionId::ParameterDecSmall:
      return {"Decrease", "Lower the selected value"};
    case ContextActionId::ParameterIncSmall:
    case ContextActionId::ParameterIncLarge:
      return {"Increase", "Raise the selected value"};
    case ContextActionId::ParameterMin:
      return {"Minimum", "Set the value to minimum"};
    case ContextActionId::ParameterMax:
      return {"Maximum", "Set the value to maximum"};
    case ContextActionId::ParameterReset:
      return {"Reset", "Restore the entry value"};
    case ContextActionId::KeyboardRootPrev:
    case ContextActionId::KeyboardScalePrev:
      return {"Previous", "Move to the previous keyboard value"};
    case ContextActionId::KeyboardRootNext:
    case ContextActionId::KeyboardScaleNext:
      return {"Next", "Move to the next keyboard value"};
    case ContextActionId::KeyboardModePiano:
      return {"Piano", "Use the piano-style keyboard layout"};
    case ContextActionId::KeyboardModeOctave:
      return {"Octave", "Use one scale octave per row"};
    case ContextActionId::KeyboardModeChromatic:
      return {"Chromatic", "Use chromatic note steps"};
    case ContextActionId::ProjectSave:
      return {"Save Project", "Save synth and sequencer state"};
    case ContextActionId::ProjectSaveAs:
      return {"Save Project As", "Create a named project copy"};
    case ContextActionId::ProjectLoad:
      return {"Load Project", "Load the selected project"};
    case ContextActionId::ProjectNew:
      return {"New Project", "Start a clean project"};
    case ContextActionId::ProjectConfirm:
      return {"Confirm", "Apply the project operation shown on screen"};
    case ContextActionId::ProjectNameSave:
      return {"Save Project", "Write the named project to storage"};
    case ContextActionId::ProjectNameBackspace:
      return {"Delete Character", "Remove the last character of the project name"};
    case ContextActionId::SequencerTogglePlayback:
      return {"Play / Stop", "Start from the beginning or stop playback"};
    case ContextActionId::SequencerToggleView:
      return {"Home View", label == "GRID" ? "Show the sequencer grid and return Home" :
                                            "Show the performance keyboard and return Home"};
    case ContextActionId::SequencerStop:
      return {"Stop", "Stop playback and release all sequencer notes"};
    case ContextActionId::SequencerResume:
      return {"Resume", "Continue from the stopped position"};
    case ContextActionId::SequencerTempoDec:
    case ContextActionId::SequencerTempoInc:
    case ContextActionId::SequencerTempoDecLarge:
    case ContextActionId::SequencerTempoIncLarge:
      return {"Tempo", "Adjust the sequencer BPM"};
    case ContextActionId::SequencerMute:
      return {"Mute", "Toggle sound from this sequencer track"};
    case ContextActionId::SequencerEnable:
      return {"Enable", "Toggle playback of this sequencer track"};
    case ContextActionId::SequencerPatternPrev:
    case ContextActionId::SequencerPatternNext:
    case ContextActionId::SequencerPatternCountDec:
    case ContextActionId::SequencerPatternCountInc:
      return {"Pattern", "Select a pattern or adjust the number of patterns"};
    default:
      return {label.empty() ? "Command" : label, "Run this command"};
  }
}

std::pair<std::string, std::string> ItemHelp(UiModifier modifier,
                                             const std::string& itemId,
                                             const std::string& patchName,
                                             const ISynthEngine* engine)
{
  if (modifier == UiModifier::Sound)
  {
    if (itemId == "snd.engine") return {"Engine", "Choose the track synth engine"};
    if (itemId == "snd.preset") return {"Preset", "Browse and load presets"};
    if (itemId == "snd.save") return {"Save Preset", "Update the current preset"};
    if (itemId == "snd.saveas") return {"Save Preset As", "Name preset copy"};
  }

  if (modifier == UiModifier::Parameter)
  {
    if (IsDx7ParameterGroupId(itemId))
    {
      return {Dx7GroupLabel(itemId), Dx7GroupSummary(engine, itemId)};
    }

    if (IsJunoParameterGroupId(itemId))
    {
      return {JunoGroupTitle(itemId), JunoGroupSummary(engine, itemId)};
    }

    SynthParameterInfo parameter;
    if (ReadParameter(engine, itemId, parameter))
    {
      return {parameter.label, engine != nullptr ? engine->DescribeParameterValue(itemId) : std::string{}};
    }
  }

  if (modifier == UiModifier::Layout)
  {
    if (itemId == "lay.scl") return {"Scale", "Set the keyboard scale"};
    if (itemId == "lay.root") return {"Root", "Set the keyboard root note"};
    if (itemId == "lay.mode") return {"Mode", "Choose piano, octave, or chromatic layout"};
    if (itemId == "lay.fx") return {"Effect", "Live Performance Effects"};
  }

  const auto presentation = MenuItemPresentation(modifier, itemId, patchName, engine);
  const std::string fallbackValue = patchName.empty()
      ? (engine != nullptr ? engine->Descriptor().displayName : std::string{"SYNTH"})
      : patchName;
  if (presentation.first == "READY" && presentation.second == fallbackValue)
  {
    return {"", ""};
  }

  return presentation;
}

std::pair<std::string, std::string> ContextCellHelp(const ContextMenuCell& cell,
                                                    UiModifier modifier,
                                                    const std::string& patchName,
                                                    const ISynthEngine* engine)
{
  if (!cell.fullName.empty() || !cell.description.empty())
  {
    return {cell.fullName.empty() ? cell.label : cell.fullName, cell.description};
  }

  if (cell.type == MenuCellType::Item)
  {
    const auto help = ItemHelp(modifier, cell.itemId, patchName, engine);
    if (!help.first.empty() || !help.second.empty())
    {
      return help;
    }

    return {cell.label, {}};
  }

  if (cell.type == MenuCellType::NextPage)
  {
    return {"Next Page", "Show more controls"};
  }

  if (cell.type == MenuCellType::UtilityAction)
  {
    return ActionHelp(cell.actionId, cell.label);
  }

  return {"", ""};
}

}  // namespace

SynthUiModel::SynthUiModel() = default;

void SynthUiModel::InitializeTrack(std::size_t trackIndex, SynthEngineKind kind)
{
  if (trackIndex >= tracks_.size())
  {
    return;
  }

  tracks_[trackIndex].kind = kind;
  tracks_[trackIndex].patchName = ToString(kind);
  tracks_[trackIndex].looperProgress = 0.0f;
}

void SynthUiModel::SyncTrackState(std::size_t trackIndex, const ISynthEngine* engine)
{
  if (trackIndex >= tracks_.size() || engine == nullptr)
  {
    return;
  }

  tracks_[trackIndex].kind = engine->Descriptor().kind;

  if (!engine->CurrentPresetName().empty())
  {
    tracks_[trackIndex].patchName = engine->CurrentPresetName();
  }
}

void SynthUiModel::SyncSamplerTrackState(std::size_t trackIndex, const SamplerTrackUiState& state)
{
  if (trackIndex >= tracks_.size())
  {
    return;
  }

  const bool recordingStarted = state.recording && !tracks_[trackIndex].sampler.recording;
  tracks_[trackIndex].sampler = state;
  if (trackIndex == activeTrack_ && state.recording)
  {
    samplerRecordConfirmMode_ = false;
    if (recordingStarted)
    {
      heldContextMenuIndex_ = -1;
      hasMenuHighlight_ = false;
    }
  }
}

void SynthUiModel::SetSequencerUiState(const SequencerUiState& state)
{
  const auto now = std::chrono::steady_clock::now();
  if (state.activeTrack != sequencer_.activeTrack || state.activePattern != sequencer_.activePattern)
  {
    sequencerChangedAt_.fill({});
    sequencerViewChangedAt_ = now;
  }
  else
  {
    for (std::size_t i = 0; i < state.steps.size(); ++i)
      if (state.steps[i].contentFingerprint != sequencer_.steps[i].contentFingerprint)
        sequencerChangedAt_[i] = now;
  }
  if (state.hasHeldSteps != sequencer_.hasHeldSteps || state.primaryHeldStep != sequencer_.primaryHeldStep ||
      state.editLane != sequencer_.editLane)
    sequencerFocusChangedAt_ = now;
  sequencer_ = state;
}

void SynthUiModel::ConfirmSequencerPatternDelete(std::size_t patternIndex, bool returnHome)
{
  if (patternIndex >= sequencer_.patternCount || patternIndex >= kSequencerMaxPatternCount)
    return;

  ExitToHome();
  ClearHeldModifiers();
  heldHomeActionRows_.fill(false);
  ActivateModifier(UiModifier::Other);
  selectedParameterGroupId_ = "sys.seq.pattern";
  contextLatched_ = true;
  sequencerDeleteConfirm_ = true;
  sequencerDeleteReturnHome_ = returnHome;
  sequencerDeleteTrack_ = activeTrack_;
  sequencerDeletePattern_ = patternIndex;
  CancelTransition();
}

void SynthUiModel::CloseSequencerPatternDelete()
{
  sequencerDeleteConfirm_ = false;
  if (sequencerDeleteReturnHome_)
    ExitToHome();
  else
    ClearContextSelection();
}

void SynthUiModel::SetActiveTrack(std::size_t trackIndex)
{
  if (trackIndex >= tracks_.size())
  {
    return;
  }

  if (trackIndex != activeTrack_ && sequencerDeleteConfirm_)
    CloseSequencerPatternDelete();
  activeTrack_ = trackIndex;
  homeOctaveStatusUntil_ = std::chrono::steady_clock::time_point{};
}

void SynthUiModel::SetHomeActionHeld(std::size_t row, bool held)
{
  if (row < heldHomeActionRows_.size())
  {
    heldHomeActionRows_[row] = held;
  }
}

void SynthUiModel::AdjustHomeKeyboardOctave(int delta)
{
  AdjustKeyboardOctave(delta);
}

void SynthUiModel::Tick(const ISynthEngine* engine)
{
  const auto now = std::chrono::steady_clock::now();
  for (std::size_t row = 0; row < heldModifiers_.size(); ++row)
  {
    if (!heldModifiers_[row] || modifierContextActivated_[row])
    {
      continue;
    }

    if ((now - modifierPressStartedAt_[row]) < kModifierHoldDelay)
    {
      continue;
    }

    const UiScreenSnapshot from = BuildScreenSnapshot(engine);
    ActivateModifier(ModifierForRow(row));
    modifierContextActivated_[row] = true;
    StartTransition(from, BuildScreenSnapshot(engine));
    break;
  }

  if (transition_.active)
  {
    transition_.progress = std::min(1.0f, transition_.progress + kTransitionStep);
    if (transition_.progress >= 1.0f)
    {
      transition_.active = false;
    }
  }

  const bool heldPresetBrowserScroll = HandlePresetBrowserAutoScroll(engine, now);
  if (!heldPresetBrowserScroll && mode_ == UiMode::ContextMenu && heldContextMenuIndex_ >= 0 && !contextMenuHintVisible_ &&
      contextMenuPressStartedAt_ != std::chrono::steady_clock::time_point{} &&
      (now - contextMenuPressStartedAt_) >= kContextHintHoldDelay)
  {
    contextMenuHintVisible_ = true;
  }
}

MatrixKeyHandling SynthUiModel::PressMatrixKey(ISynthEngine* engine, std::size_t row, std::size_t col)
{
  if (row >= kKeyboardMatrixRows || col >= kKeyboardMatrixColumns)
  {
    return MatrixKeyHandling::Ignored;
  }

  if (col == kHomeLeftBarColumn)
  {
    if (mode_ == UiMode::ContextMenu && contextLatched_)
    {
      const UiScreenSnapshot from = BuildScreenSnapshot(engine);
      ClearHeldModifiers();
      if (IsContextRoot())
      {
        ExitToHome();
      }
      else
      {
        ApplyMenuCell(engine, MakeActionCell("BACK", ContextActionId::Back), 0);
        heldContextMenuIndex_ = -1;
        contextMenuPressStartedAt_ = std::chrono::steady_clock::time_point{};
        presetBrowserAutoScrollLastAt_ = std::chrono::steady_clock::time_point{};
        heldContextMenuActionConsumed_ = false;
        contextMenuHintVisible_ = false;
        heldContextMenuHintTitle_.clear();
        heldContextMenuHintText_.clear();
        hasMenuHighlight_ = false;
      }
      StartTransition(from, BuildScreenSnapshot(engine));
      return MatrixKeyHandling::Ignored;
    }

    if (!heldModifiers_[row])
    {
      heldModifiers_[row] = true;
      modifierContextActivated_[row] = false;
      modifierPressStartedAt_[row] = std::chrono::steady_clock::now();
    }
    return MatrixKeyHandling::ModifierHold;
  }

  if (mode_ == UiMode::Home && col == kHomeRightBarColumn)
  {
    heldHomeActionRows_[row] = true;
    if (row == 0)
    {
      AdjustKeyboardOctave(1);
    }
    else if (row == 1)
    {
      AdjustKeyboardOctave(-1);
    }
    else if (row == 2)
    {
      QueueCommand(UiCommandType::SequencerStop);
    }
    else if (row == 3)
    {
      QueueCommand(UiCommandType::SequencerTogglePlayback);
    }
    return MatrixKeyHandling::Ignored;
  }

  const std::size_t contextMenuColumns = ContextMenuColumnCount();
  const std::size_t contextMenuFirstColumn = contextMenuColumns == 1 ? kHomeRightBarColumn : kContextMenuFirstColumn;
  if (mode_ == UiMode::ContextMenu && projectNameEntryMode_ && activeModifier_ == UiModifier::Other &&
      col > kHomeLeftBarColumn && col < contextMenuFirstColumn)
  {
    const char* value = kPresetSearchKeyMap[row][col];
    if (IsSamplerNameCharacter(value) && projectNameText_.size() < 24)
    {
      projectNameText_ += value;
      projectStatus_.clear();
    }
    return MatrixKeyHandling::Ignored;
  }

  if (mode_ == UiMode::ContextMenu && samplerNameEntryMode_ && activeModifier_ == UiModifier::Parameter &&
      IsSamplerParameterEngine(engine) && col > kHomeLeftBarColumn && col < contextMenuFirstColumn)
  {
    heldContextMenuIndex_ = -1;
    hasMenuHighlight_ = false;

    const char* mappedValue = kPresetSearchKeyMap[row][col];
    if (IsSamplerNameCharacter(mappedValue) && samplerNameText_.size() < kSamplerNameMaxLength)
    {
      samplerNameText_ += mappedValue;
      selectedValueText_ = samplerNameText_;
    }
    return MatrixKeyHandling::Ignored;
  }

  if (mode_ == UiMode::ContextMenu && presetNameEntryMode_ && activeModifier_ == UiModifier::Sound &&
      col > kHomeLeftBarColumn && col < contextMenuFirstColumn)
  {
    heldContextMenuIndex_ = -1;
    hasMenuHighlight_ = false;

    const char* mappedValue = kPresetSearchKeyMap[row][col];
    if (IsSamplerNameCharacter(mappedValue) && presetNameText_.size() < kPresetSearchMaxLength)
    {
      presetNameText_ += mappedValue;
      selectedValueText_ = presetNameText_;
    }
    return MatrixKeyHandling::Ignored;
  }

  if (mode_ == UiMode::ContextMenu && samplerBrowserMode_ && activeModifier_ == UiModifier::Parameter &&
      IsSamplerParameterEngine(engine) && col > kHomeLeftBarColumn && col < contextMenuFirstColumn)
  {
    heldContextMenuIndex_ = -1;
    hasMenuHighlight_ = false;
    samplerFileStatus_.clear();

    if (row == 3 && col == 1)
    {
      if (!samplerBrowserEntries_.empty())
      {
        samplerFileTarget_ = samplerBrowserEntries_[samplerBrowserSelectionIndex_];
        samplerFileStatus_.clear();
        samplerEditFromBrowser_ = true;
        samplerBrowserMode_ = false;
        selectedParameterGroupId_ = "par.sampler.group.edit";
        ClearContextSelection();
      }
      return MatrixKeyHandling::Ignored;
    }

    const char* mappedValue = kPresetSearchKeyMap[row][col];
    if (mappedValue != nullptr && mappedValue[0] != '\0' && samplerBrowserSearchText_.size() < kPresetSearchMaxLength)
    {
      samplerBrowserSearchText_ += mappedValue;
      samplerBrowserSelectionIndex_ = 0;
      RefreshSamplerBrowserEntries();
    }
    return MatrixKeyHandling::Ignored;
  }

  if (mode_ == UiMode::ContextMenu && presetBrowserMode_ && activeModifier_ == UiModifier::Sound &&
      col > kHomeLeftBarColumn && col < contextMenuFirstColumn)
  {
    heldContextMenuIndex_ = -1;
    hasMenuHighlight_ = false;

    const char* mappedValue = kPresetSearchKeyMap[row][col];
    if (mappedValue != nullptr && mappedValue[0] != '\0' && presetSearchText_.size() < kPresetSearchMaxLength)
    {
      presetSearchText_ += mappedValue;
      presetBrowserSelectionIndex_ = 0;
    }
    return MatrixKeyHandling::Ignored;
  }

  if (mode_ == UiMode::ContextMenu &&
      col >= contextMenuFirstColumn &&
      col < contextMenuFirstColumn + contextMenuColumns)
  {
    const auto menu = CurrentMenuCells(engine);
    const std::size_t menuIndex = contextMenuColumns == 1 ? row : row * kContextMenuColumns + (col - contextMenuFirstColumn);
    heldContextMenuIndex_ = static_cast<int>(menuIndex);
    contextMenuHintVisible_ = false;
    contextMenuPressStartedAt_ = std::chrono::steady_clock::now();
    presetBrowserAutoScrollLastAt_ = std::chrono::steady_clock::time_point{};
    heldContextMenuActionConsumed_ = false;
    heldContextMenuHintTitle_.clear();
    heldContextMenuHintText_.clear();
    if (menuIndex < menu.size())
    {
      const auto help = ContextCellHelp(menu[menuIndex], activeModifier_, tracks_[activeTrack_].patchName, engine);
      heldContextMenuHintTitle_ = help.first;
      heldContextMenuHintText_ = help.second;
    }
    return MatrixKeyHandling::MenuAction;
  }

  if (!IsHomePerformanceKey(row, col))
  {
    return MatrixKeyHandling::Ignored;
  }

  return MatrixKeyHandling::PerformanceKey;
}

void SynthUiModel::ReleaseMatrixKey(ISynthEngine* engine, std::size_t row, std::size_t col)
{
  if (row >= kKeyboardMatrixRows)
  {
    return;
  }

  const std::size_t contextMenuColumns = ContextMenuColumnCount();
  const std::size_t contextMenuFirstColumn = contextMenuColumns == 1 ? kHomeRightBarColumn : kContextMenuFirstColumn;
  if (mode_ == UiMode::ContextMenu &&
      col >= contextMenuFirstColumn &&
      col < contextMenuFirstColumn + contextMenuColumns)
  {
    const std::size_t menuIndex = contextMenuColumns == 1 ? row : row * kContextMenuColumns + (col - contextMenuFirstColumn);
    const bool shouldApply = heldContextMenuIndex_ == static_cast<int>(menuIndex) && !contextMenuHintVisible_ &&
                             !heldContextMenuActionConsumed_;
    const auto menu = CurrentMenuCells(engine);
    if (shouldApply && menuIndex < menu.size())
    {
      ApplyMenuCell(engine, menu[menuIndex], menuIndex);
    }

    heldContextMenuIndex_ = -1;
    contextMenuPressStartedAt_ = std::chrono::steady_clock::time_point{};
    presetBrowserAutoScrollLastAt_ = std::chrono::steady_clock::time_point{};
    heldContextMenuActionConsumed_ = false;
    contextMenuHintVisible_ = false;
    heldContextMenuHintTitle_.clear();
    heldContextMenuHintText_.clear();
    hasMenuHighlight_ = false;
    return;
  }

  if (col == kHomeRightBarColumn)
  {
    heldHomeActionRows_[row] = false;
    return;
  }

  if (col != kHomeLeftBarColumn || !heldModifiers_[row])
  {
    return;
  }

  const bool activatedContext = modifierContextActivated_[row];
  const auto heldDuration = std::chrono::steady_clock::now() - modifierPressStartedAt_[row];
  heldModifiers_[row] = false;
  modifierContextActivated_[row] = false;

  if (!activatedContext)
  {
    if (heldDuration < kModifierHoldDelay && row < tracks_.size())
    {
      SetActiveTrack(row);
      CancelTransition();
    }
    return;
  }

  if (contextLatched_)
  {
    return;
  }

  const UiScreenSnapshot from = BuildScreenSnapshot(engine);
  ExitToHome();
  StartTransition(from, BuildScreenSnapshot(engine));
}

std::array<int, 4> SynthUiModel::KeyboardSettings() const
{
  return {keyboardOctaveOffset_, keyboardRootNote_, static_cast<int>(keyboardScaleIndex_), static_cast<int>(keyboardLayoutMode_)};
}

bool SynthUiModel::RestoreKeyboardSettings(const std::array<int, 4>& settings)
{
  if (settings[0] < kKeyboardOctaveMin || settings[0] > kKeyboardOctaveMax || settings[1] < 0 || settings[1] > 11 ||
      settings[2] < 0 || settings[2] >= static_cast<int>(kKeyboardScales.size()) || settings[3] < 0 || settings[3] > 2)
  {
    return false;
  }
  keyboardOctaveOffset_ = settings[0];
  keyboardRootNote_ = settings[1];
  keyboardScaleIndex_ = static_cast<std::size_t>(settings[2]);
  keyboardLayoutMode_ = static_cast<KeyboardLayoutMode>(settings[3]);
  return true;
}

void SynthUiModel::SetProjectList(const std::vector<std::string>& names)
{
  projectNames_ = names;
  projectSelectionIndex_ = projectNames_.empty() ? 0 : std::min(projectSelectionIndex_, projectNames_.size() - 1);
}

void SynthUiModel::ProjectResult(const std::string& name, const std::string& status, bool success)
{
  projectStatus_ = status;
  projectConfirmCommand_ = UiCommandType::None;
  if (success)
  {
    projectName_ = name;
    projectNameEntryMode_ = false;
    selectedParameterGroupId_ = "sys.project";
  }
  SelectStatus("Project", status, "sys.project");
}

void SynthUiModel::ConfirmProjectOverwrite(const std::string& name)
{
  projectNameEntryMode_ = false;
  projectConfirmCommand_ = UiCommandType::ProjectOverwrite;
  projectConfirmName_ = name;
  projectStatus_.clear();
}

void SynthUiModel::EngineChangeFailed(const std::string& status)
{
  SelectStatus("Engine", status, "snd.engine.error");
}

void SynthUiModel::SamplerFileResult(bool success, const std::string& status, const std::string& renamedFile)
{
  samplerFileStatus_ = status;
  if (!success) return;
  if (samplerBrowserMode_)
  {
    ExitSamplerBrowser();
    selectedParameterGroupId_.clear();
    ClearContextSelection();
    return;
  }
  ExitSamplerNameEntry();
  if (!renamedFile.empty()) samplerFileTarget_ = renamedFile;
  samplerBrowserAllEntries_ = CollectSamplerBrowserEntries();
  if (std::find(samplerBrowserAllEntries_.begin(), samplerBrowserAllEntries_.end(), samplerFileTarget_) == samplerBrowserAllEntries_.end())
    samplerFileTarget_.clear();
  RefreshSamplerBrowserEntries();
  selectedParameterGroupId_ = "par.sampler.group.edit";
  ClearContextSelection();
}

void SynthUiModel::ExitToHome()
{
  sequencerDeleteConfirm_ = false;
  if (samplerNameEntryMode_ || tracks_[activeTrack_].sampler.deleteConfirm || tracks_[activeTrack_].sampler.overwritePending)
    QueueCommand(UiCommandType::SamplerCancelOverlay);
  ExitSamplerBrowser();
  samplerFileTarget_.clear();
  samplerFileStatus_.clear();
  mode_ = UiMode::Home;
  activeModifier_ = UiModifier::None;
  currentPage_ = 0;
  activeMenuRow_ = 0;
  activeMenuColumn_ = 0;
  heldContextMenuIndex_ = -1;
  contextMenuPressStartedAt_ = std::chrono::steady_clock::time_point{};
  presetBrowserAutoScrollLastAt_ = std::chrono::steady_clock::time_point{};
  heldContextMenuActionConsumed_ = false;
  contextMenuHintVisible_ = false;
  heldContextMenuHintTitle_.clear();
  heldContextMenuHintText_.clear();
  enginePickerMode_ = false;
  hasPendingEngineKind_ = false;
  pendingEngineKind_ = SynthEngineKind::None;
  presetBrowserMode_ = false;
  presetSearchText_.clear();
  presetBrowserSelectionIndex_ = 0;
  ExitPresetNameEntry();
  ExitSamplerNameEntry();
  ExitSamplerRecordConfirm();
  projectNameEntryMode_ = false;
  projectConfirmCommand_ = UiCommandType::None;
  contextLatched_ = false;
  selectedParameterGroupId_.clear();
  ClearContextSelection();
}

void SynthUiModel::HandleHomeButton(const ISynthEngine* engine)
{
  const UiScreenSnapshot from = BuildScreenSnapshot(engine);
  ClearHeldModifiers();
  heldHomeActionRows_.fill(false);
  ExitToHome();
  StartTransition(from, BuildScreenSnapshot(engine));
}

void SynthUiModel::ToggleHomeView()
{
  showSequencerGrid_ = !showSequencerGrid_;
  sequencerViewChangedAt_ = std::chrono::steady_clock::now();
  CancelTransition();
}

std::string SynthUiModel::HeaderEngine(const ISynthEngine* engine) const
{
  if (engine != nullptr)
  {
    return engine->Descriptor().displayName;
  }
  return ToString(tracks_[activeTrack_].kind);
}

std::string SynthUiModel::HeaderTrack() const
{
  if (mode_ == UiMode::Home && std::chrono::steady_clock::now() < homeOctaveStatusUntil_)
  {
    return std::string(showSequencerGrid_ ? "OCT " : "Octave ") + (keyboardOctaveOffset_ >= 0 ? "+" : "") + std::to_string(keyboardOctaveOffset_);
  }
  return std::string(showSequencerGrid_ ? "T" : "Track ") + std::to_string(activeTrack_ + 1);
}

bool SynthUiModel::KeyboardMidiNoteForKey(std::size_t row, std::size_t col, int& note) const
{
  if (row >= kKeyboardMatrixRows || col < kHomePerformanceFirstColumn || col > kHomePerformanceLastColumn)
  {
    return false;
  }

  const std::size_t x = col - kHomePerformanceFirstColumn;
  if (tracks_[activeTrack_].kind == SynthEngineKind::Drums)
  {
    note = kDrumNotes[x];
    return true;
  }
  const std::size_t y = kKeyboardMatrixRows - 1 - row;
  const int octaveShift = keyboardOctaveOffset_ * 12;

  if (keyboardLayoutMode_ == KeyboardLayoutMode::Piano)
  {
    static constexpr std::array<int, 7> kWhiteKeys = {0, 2, 4, 5, 7, 9, 11};
    static constexpr std::array<int, 7> kBlackKeys = {-1, 1, 3, -1, 6, 8, 10};
    const std::size_t keyIndex = x % kWhiteKeys.size();
    const int octave = static_cast<int>(y / 2 + x / kWhiteKeys.size()) * 12;
    if ((y % 2) == 0)
    {
      note = static_cast<int>(kHomePerformanceBaseMidiNote) + octaveShift + octave + kWhiteKeys[keyIndex];
      return note >= 0 && note <= 127;
    }

    const int blackKey = kBlackKeys[keyIndex];
    if (blackKey < 0)
    {
      return false;
    }
    note = static_cast<int>(kHomePerformanceBaseMidiNote) + octaveShift + octave + blackKey;
    return note >= 0 && note <= 127;
  }

  const int base = static_cast<int>(kHomePerformanceBaseMidiNote) + keyboardRootNote_ + octaveShift;
  if (keyboardLayoutMode_ == KeyboardLayoutMode::Octave)
  {
    const std::uint16_t scaleMask = KeyboardScaleAt(keyboardScaleIndex_).mask;
    const std::size_t scaleNoteCount = std::max<std::size_t>(1, CountScaleNotes(scaleMask));
    note = base + static_cast<int>(y) * 12 +
           static_cast<int>(x / scaleNoteCount) * 12 +
           KeyboardScaleDegreeOffset(scaleMask, x);
    return note >= 0 && note <= 127;
  }

  note = base + static_cast<int>(y * kHomePerformanceColumnCount + x);
  return note >= 0 && note <= 127;
}

bool SynthUiModel::IsHomePerformanceKey(std::size_t row, std::size_t col) const
{
  int note = 0;
  return KeyboardMidiNoteForKey(row, col, note);
}

bool SynthUiModel::IsKeyboardHintKeyLit(std::size_t row, std::size_t col) const
{
  if (row >= kKeyboardMatrixRows || col <= kHomeLeftBarColumn || col >= kHomeRightBarColumn)
  {
    return false;
  }

  if (mode_ == UiMode::ContextMenu && presetBrowserMode_)
  {
    const std::size_t contextMenuColumns = ContextMenuColumnCount();
    const std::size_t contextMenuFirstColumn = contextMenuColumns == 1 ? kHomeRightBarColumn : kContextMenuFirstColumn;
    return col < contextMenuFirstColumn;
  }

  return IsHomePerformanceKeyLit(row, col);
}

bool SynthUiModel::IsKeyboardScaleNote(int note) const
{
  const int noteFromRoot = PositiveModulo(note - keyboardRootNote_, 12);
  return (KeyboardScaleAt(keyboardScaleIndex_).mask & (static_cast<std::uint16_t>(1u) << noteFromRoot)) != 0;
}

bool SynthUiModel::IsHomePerformanceKeyLit(std::size_t row, std::size_t col) const
{
  int note = 0;
  if (!KeyboardMidiNoteForKey(row, col, note))
  {
    return false;
  }

  if (tracks_[activeTrack_].kind == SynthEngineKind::Drums || keyboardLayoutMode_ == KeyboardLayoutMode::Piano)
  {
    return true;
  }

  return IsKeyboardScaleNote(note);
}

std::uint8_t SynthUiModel::HomePerformanceMidiNote(std::size_t row, std::size_t col) const
{
  int note = 0;
  if (!KeyboardMidiNoteForKey(row, col, note))
  {
    return 0;
  }
  return static_cast<std::uint8_t>(std::clamp(note, 0, 127));
}

const std::string& SynthUiModel::PatchName() const
{
  return tracks_[activeTrack_].patchName;
}

float SynthUiModel::LooperProgress() const
{
  return tracks_[activeTrack_].looperProgress;
}

std::string SynthUiModel::ContextSectionTitle() const
{
  if (mode_ != UiMode::ContextMenu)
  {
    return "HOME";
  }

  switch (activeModifier_)
  {
    case UiModifier::Sound:
      return "SOUND";
    case UiModifier::Parameter:
      return "PARAMETER";
    case UiModifier::Layout:
      return "KEYBOARD";
    case UiModifier::Other:
      return "SYSTEM";
    case UiModifier::None:
    default:
      return "HOME";
  }
}

std::string SynthUiModel::ContextParameterName(const ISynthEngine* engine) const
{
  (void)engine;
  if (mode_ != UiMode::ContextMenu || !hasContextSelection_)
  {
    return "";
  }

  switch (selectionKind_)
  {
    case ContextSelectionKind::Preset:
      return "Preset";
    case ContextSelectionKind::Parameter:
    case ContextSelectionKind::Status:
      return selectedItemLabel_;
    case ContextSelectionKind::None:
    default:
      return "";
  }
}

std::string SynthUiModel::ContextValueText(const ISynthEngine* engine) const
{
  if (mode_ != UiMode::ContextMenu || !hasContextSelection_)
  {
    return "";
  }

  switch (selectionKind_)
  {
    case ContextSelectionKind::Preset:
      return engine != nullptr ? engine->CurrentPresetName() : PatchName();
    case ContextSelectionKind::Parameter:
      return engine != nullptr ? engine->DescribeParameterValue(selectedItemId_) : "--";
    case ContextSelectionKind::Status:
      return selectedValueText_;
    case ContextSelectionKind::None:
    default:
      return "";
  }
}

ContextMenuGrid SynthUiModel::CurrentMenuCells(const ISynthEngine* engine) const
{
  if (mode_ != UiMode::ContextMenu)
  {
    return BlankGrid();
  }

  switch (activeModifier_)
  {
    case UiModifier::Sound:
      return BuildSoundMenu(engine);
    case UiModifier::Parameter:
      if (parameterAdjustMode_)
      {
        return BuildParameterAdjustMenu();
      }
      return BuildParameterBrowseMenu(engine);
    case UiModifier::Layout:
      return BuildLayoutMenu();
    case UiModifier::Other:
      return BuildOtherMenu();
    case UiModifier::None:
    default:
      return BlankGrid();
  }
}

std::size_t SynthUiModel::ContextMenuColumnCount() const
{
  const SamplerTrackUiState& sampler = tracks_[activeTrack_].sampler;

  if (activeModifier_ == UiModifier::Other)
  {
    if (sequencerDeleteConfirm_ || projectNameEntryMode_ || projectConfirmCommand_ != UiCommandType::None ||
        selectedParameterGroupId_.empty() || selectedParameterGroupId_ == "sys.load")
    {
      return 1;
    }
    return 2;
  }

  if (activeModifier_ == UiModifier::Sound)
  {
    return 1;
  }

  if (activeModifier_ == UiModifier::Layout)
  {
    return 1;
  }

  if (activeModifier_ == UiModifier::Parameter && parameterAdjustMode_ &&
      tracks_[activeTrack_].kind == SynthEngineKind::Juno && IsJunoDiscreteAdjustParameterId(selectedItemId_))
  {
    return 1;
  }

  if (activeModifier_ == UiModifier::Parameter && parameterAdjustMode_ &&
      tracks_[activeTrack_].kind == SynthEngineKind::Sampler && selectedItemId_ == "sample.loop")
  {
    return 1;
  }

  if (activeModifier_ == UiModifier::Parameter && tracks_[activeTrack_].kind == SynthEngineKind::Juno &&
      IsJunoDirectActionGroupId(selectedParameterGroupId_))
  {
    return 1;
  }

  if (activeModifier_ == UiModifier::Parameter && tracks_[activeTrack_].kind == SynthEngineKind::Sampler &&
      (samplerNameEntryMode_ || sampler.overwritePending || sampler.deleteConfirm))
  {
    return 1;
  }

  if (activeModifier_ == UiModifier::Parameter && tracks_[activeTrack_].kind == SynthEngineKind::Sampler && samplerBrowserMode_)
  {
    return 1;
  }

  if (activeModifier_ == UiModifier::Parameter && tracks_[activeTrack_].kind == SynthEngineKind::Sampler && samplerRecordConfirmMode_)
  {
    return 1;
  }

  if (activeModifier_ == UiModifier::Parameter && tracks_[activeTrack_].kind == SynthEngineKind::Sampler && !parameterAdjustMode_)
  {
    if (selectedParameterGroupId_.empty())
    {
      return (!sampler.hasSample && !sampler.hasDraft) ? 1 : 2;
    }

    if (selectedParameterGroupId_ == "par.sampler.group.trim")
    {
      return 2;
    }

    if (selectedParameterGroupId_ == "par.sampler.group.samp")
    {
      return 1;
    }

    return 1;
  }

  return 2;
}

UiScreenSnapshot SynthUiModel::CurrentScreenSnapshot(const ISynthEngine* engine) const
{
  return BuildScreenSnapshot(engine);
}

bool SynthUiModel::ConsumePendingEngineKind(SynthEngineKind& kind)
{
  if (!hasPendingEngineKind_)
  {
    return false;
  }

  kind = pendingEngineKind_;
  hasPendingEngineKind_ = false;
  pendingEngineKind_ = SynthEngineKind::None;
  return true;
}

bool SynthUiModel::ConsumePendingCommand(UiCommand& command)
{
  if (pendingCommands_.empty())
  {
    return false;
  }

  command = pendingCommands_.front();
  pendingCommands_.erase(pendingCommands_.begin());
  return true;
}

bool SynthUiModel::IsModifierHighlighted(std::size_t row) const
{
  return mode_ == UiMode::ContextMenu && row == RowForModifier(activeModifier_);
}

bool SynthUiModel::IsMenuCellActive(std::size_t row, std::size_t col) const
{
  if (mode_ != UiMode::ContextMenu)
  {
    return false;
  }

  const std::size_t columns = ContextMenuColumnCount();
  if (col >= columns)
  {
    return false;
  }

  const std::size_t index = columns == 1 ? row : row * kContextMenuColumns + col;
  return heldContextMenuIndex_ == static_cast<int>(index) ||
         (hasMenuHighlight_ && row == activeMenuRow_ && col == activeMenuColumn_);
}

std::string SynthUiModel::DebugSummary(const ISynthEngine* engine) const
{
  std::ostringstream stream;
  stream << (mode_ == UiMode::Home ? "HOME" : "CONTEXT")
         << "  view=" << (showSequencerGrid_ ? "GRID" : "KEYS")
         << "  engine=" << HeaderEngine(engine)
         << "  track=" << (activeTrack_ + 1)
         << "  patch=" << PatchName();
  if (mode_ == UiMode::ContextMenu)
  {
    stream << "  section=" << ContextSectionTitle()
           << "  page=" << (currentPage_ + 1);
    if (hasContextSelection_)
    {
      stream << "  param=" << ContextParameterName(engine)
             << "  value=" << ContextValueText(engine);
    }
  }
  return stream.str();
}

UiBarSnapshot SynthUiModel::BuildModifierBarSnapshot(UiModifier highlightedModifier) const
{
  UiBarSnapshot items{};
  for (std::size_t row = 0; row < kModifierBarLabels.size(); ++row)
  {
    items[row].label = kModifierBarLabels[row];
    items[row].pressed = heldModifiers_[row];
    items[row].active = highlightedModifier != UiModifier::None && row == RowForModifier(highlightedModifier);
  }
  return items;
}

UiBarSnapshot SynthUiModel::BuildSequencerEditBarSnapshot() const
{
  UiBarSnapshot items{};
  items[0].label = "NOTE";
  items[1].label = "LEN";
  items[2].label = "VEL";
  items[3].label = "TIME";
  items[0].fullName = "Notes";
  items[1].fullName = "Length";
  items[2].fullName = "Velocity";
  items[3].fullName = "Timing Offset";
  items[0].description = "Edit selected step notes";
  items[1].description = "Edit selected step note length";
  items[2].description = "Edit selected step velocity";
  items[3].description = "Move notes within the step";
  for (std::size_t i = 0; i < 4; ++i)
  {
    items[i].pressed = sequencer_.editLanePressed[i];
    items[i].active = static_cast<std::size_t>(sequencer_.editLane) == i;
  }
  return items;
}

UiBarSnapshot SynthUiModel::BuildSequencerShiftBarSnapshot() const
{
  UiBarSnapshot items{};
  items[0].label = "CLR";
  items[1].label = "COPY";
  items[2].label = "QNT";
  items[0].fullName = "Clear";
  items[1].fullName = "Copy";
  items[2].fullName = "Quantize";
  items[0].description = "Clear held steps or selected patterns";
  items[1].description = "Copy steps or patterns";
  items[2].description = "Quantize held steps, or hold QNT and tap a step or pattern";
  items[0].active = sequencer_.clearMode;
  items[1].active = sequencer_.copyMode;
  items[2].active = sequencer_.quantizeMode;
  items[0].danger = true;
  for (std::size_t i = 0; i < 3; ++i) items[i].pressed = sequencer_.shiftActionPressed[i];
  return items;
}

UiBarSnapshot SynthUiModel::BuildHomeActionBarSnapshot() const
{
  UiBarSnapshot items{};
  for (std::size_t row = 0; row < kHomeActionBarIcons.size(); ++row)
  {
    items[row].icon = kHomeActionBarIcons[row];
    items[row].pressed = heldHomeActionRows_[row];
  }
  items[2].fullName = "Stop Sequencer";
  items[2].description = "Stop sequencer playback";
  items[3].playing = sequencer_.playing;
  items[3].fullName = "Play Sequencer";
  items[3].description = "Toggle sequencer playback";
  return items;
}

UiBarSnapshot BuildContextBackBarSnapshot()
{
  UiBarSnapshot items{};
  for (UiBarItemSnapshot& item : items)
  {
    item.label = "BACK";
    item.active = true;
  }
  return items;
}

namespace {

bool IsDangerAction(ContextActionId action)
{
  return action == ContextActionId::SamplerRecordStart || action == ContextActionId::SamplerDelete ||
         action == ContextActionId::SamplerConfirmOverwrite || action == ContextActionId::SequencerPatternDeleteConfirm ||
         action == ContextActionId::ProjectConfirm;
}

bool IsErrorStatus(const std::string& text)
{
  for (const char* marker : {"ERR", "FAIL", "INVALID", "UNAVAILABLE", "FIRST", "EXISTS", "TAKEN"})
  {
    if (text.find(marker) != std::string::npos) return true;
  }
  return false;
}

float ParameterLevel(const ISynthEngine* engine, const std::string& parameterId)
{
  SynthParameterInfo parameter;
  if (engine == nullptr || !engine->GetParameterInfo(parameterId, parameter) ||
      parameter.kind != SynthParameterKind::Continuous || parameter.maxValue <= parameter.minValue)
  {
    return -1.0f;
  }
  return static_cast<float>(parameter.value - parameter.minValue) / static_cast<float>(parameter.maxValue - parameter.minValue);
}

std::string PatternLabel(std::size_t index)
{
  return std::string("P") + (index < 9 ? "0" : "") + std::to_string(index + 1);
}

}  // namespace

std::pair<std::string, float> SynthUiModel::CellPreview(const ContextMenuCell& cell, const ISynthEngine* engine) const
{
  const std::string& id = cell.itemId;
  if (cell.type == MenuCellType::Empty || cell.disabled)
  {
    return {};
  }

  SynthParameterInfo parameter;
  if (!id.empty() && engine != nullptr && engine->GetParameterInfo(id, parameter))
  {
    return {engine->DescribeParameterValue(id), ParameterLevel(engine, id)};
  }

  const auto value = [engine](const char* parameterId) { return DescribeParameterValueOrFallback(engine, parameterId); };
  if (id == "snd.engine") return {HeaderEngine(engine), -1.0f};
  if (id == "snd.preset") return {PatchName(), -1.0f};
  if (id == "lay.mode") return {KeyboardLayoutName(), -1.0f};
  if (id == "lay.scl") return {KeyboardScaleName(), -1.0f};
  if (id == "lay.root") return {KeyboardRootName(), -1.0f};
  if (id == "sys.project") return {projectName_.empty() ? "UNTITLED" : projectName_, -1.0f};
  if (id == "sys.seq") return {std::to_string(sequencer_.bpm) + " BPM", -1.0f};
  if (id == "sys.seq.tempo") return {std::to_string(sequencer_.bpm), -1.0f};
  if (id == "sys.seq.track") return {"T" + std::to_string(activeTrack_ + 1), -1.0f};
  if (id == "sys.seq.pattern")
    return {PatternLabel(sequencer_.activePattern) + "/" + std::to_string(sequencer_.patternCount), -1.0f};
  if (cell.actionId == ContextActionId::ShowInfo) return {Device::BackendName(), -1.0f};
  if (id == "par.juno.group.lfo") return {"R" + value("lfo.rate") + " D" + value("lfo.delay"), -1.0f};
  if (id == "par.juno.group.dco") return {value("dco.range") + (ParameterValueOrDefault(engine, "dco.saw", 0) > 0 ? " SAW" : " PWM"), -1.0f};
  if (id == "par.juno.group.vcf") return {"C" + value("vcf.cutoff") + " R" + value("vcf.resonance"), -1.0f};
  if (id == "par.juno.group.amp") return {value("vca.level"), ParameterLevel(engine, "vca.level")};
  if (id == "par.sampler.group.root") return {value("sample.root"), -1.0f};
  if (id == "par.sampler.group.loop") return {ParameterValueOrDefault(engine, "sample.loop", 0) > 0 ? "LOOP" : "1SHOT", -1.0f};
  if (id == "par.sampler.group.amp") return {value("sample.gain"), ParameterLevel(engine, "sample.gain")};
  if (id == "par.sampler.group.filter") return {"C" + value("filter.cutoff") + " R" + value("filter.resonance"), -1.0f};
  if (id == "par.sampler.group.env") return {"A" + value("env.attack") + " R" + value("env.release"), -1.0f};
  return {};
}

void SynthUiModel::PopulatePerformanceKeys(UiScreenSnapshot& snapshot) const
{
  for (std::size_t row = 0; row < kKeyboardMatrixRows; ++row)
  {
    for (std::size_t x = 0; x < kHomePerformanceColumnCount; ++x)
    {
      const std::size_t column = kHomePerformanceFirstColumn + x;
      const std::size_t index = row * kHomePerformanceColumnCount + x;
      int note = 0;
      const bool mapped = KeyboardMidiNoteForKey(row, column, note);
      snapshot.performanceKeyNotes[index] = static_cast<std::int16_t>(mapped ? note : -1);
      snapshot.performanceKeyInScale[index] = mapped && IsHomePerformanceKeyLit(row, column);
    }
  }
  snapshot.keyboardLayoutMode = keyboardLayoutMode_;
  snapshot.keyboardRootNote = keyboardLayoutMode_ == KeyboardLayoutMode::Piano ? 0 : PositiveModulo(keyboardRootNote_, 12);
  snapshot.keyboardOctave = keyboardOctaveOffset_;
  snapshot.keyboardOctaveChanged = std::chrono::steady_clock::now() < homeOctaveStatusUntil_;
}

UiBarSnapshot SynthUiModel::BuildContextBarSnapshot(const ISynthEngine* engine) const
{
  UiBarSnapshot items{};
  const auto menu = CurrentMenuCells(engine);
  const std::size_t columns = ContextMenuColumnCount();
  for (std::size_t row = 0; row < kContextMenuRows; ++row)
  {
    for (std::size_t column = 0; column < columns; ++column)
    {
      const std::size_t index = columns == 1 ? row : row * kContextMenuColumns + column;
      items[index].label = menu[index].label;
      const auto help = ContextCellHelp(menu[index], activeModifier_, tracks_[activeTrack_].patchName, engine);
      items[index].fullName = help.first;
      items[index].description = help.second;
      items[index].disabled = menu[index].disabled;
      items[index].pressed = heldContextMenuIndex_ == static_cast<int>(index);
      items[index].active = hasMenuHighlight_ && row == activeMenuRow_ && column == activeMenuColumn_;
      items[index].danger = IsDangerAction(menu[index].actionId) ||
                            (tracks_[activeTrack_].sampler.recording && menu[index].actionId == ContextActionId::SamplerStop);
      items[index].playing = menu[index].actionId == ContextActionId::SequencerTogglePlayback && sequencer_.playing;
      const auto preview = CellPreview(menu[index], engine);
      items[index].value = preview.first;
      items[index].level = preview.second;
      const int currentValue = ParameterValueOrDefault(engine, selectedItemId_, -1);
      switch (menu[index].actionId)
      {
        case ContextActionId::JunoRange16: items[index].active = selectedItemId_ == "dco.range" && currentValue == 0; break;
        case ContextActionId::JunoRange8: items[index].active = selectedItemId_ == "dco.range" && currentValue == 1; break;
        case ContextActionId::JunoRange4: items[index].active = selectedItemId_ == "dco.range" && currentValue == 2; break;
        case ContextActionId::JunoSawOn: items[index].active = selectedItemId_ == "dco.saw" && currentValue > 0; break;
        case ContextActionId::JunoSawOff: items[index].active = selectedItemId_ == "dco.saw" && currentValue == 0; break;
        case ContextActionId::ParameterMax: if (selectedItemId_ == "sample.loop") items[index].active = currentValue > 0; break;
        case ContextActionId::ParameterMin: if (selectedItemId_ == "sample.loop") items[index].active = currentValue == 0; break;
        default: break;
      }
      if (!items[index].active && activeModifier_ == UiModifier::Layout && selectedParameterGroupId_ == "lay.mode")
      {
        items[index].active =
            (menu[index].actionId == ContextActionId::KeyboardModePiano && keyboardLayoutMode_ == KeyboardLayoutMode::Piano) ||
            (menu[index].actionId == ContextActionId::KeyboardModeOctave && keyboardLayoutMode_ == KeyboardLayoutMode::Octave) ||
            (menu[index].actionId == ContextActionId::KeyboardModeChromatic && keyboardLayoutMode_ == KeyboardLayoutMode::Chromatic);
      }
    }
  }
  return items;
}

UiScreenSnapshot SynthUiModel::BuildScreenSnapshot(const ISynthEngine* engine) const
{
  UiScreenSnapshot snapshot;
  snapshot.mode = mode_;
  snapshot.modifier = activeModifier_;
  snapshot.showSequencerGrid = showSequencerGrid_;
  snapshot.sequencer = sequencer_;
  const auto now = std::chrono::steady_clock::now();
  const auto reveal = [&](std::chrono::steady_clock::time_point started, float milliseconds) {
    if (started == std::chrono::steady_clock::time_point{}) return 1.0f;
    const float t = std::clamp(std::chrono::duration<float, std::milli>(now - started).count() / milliseconds, 0.0f, 1.0f);
    return 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t);
  };
  snapshot.sequencerReveal = reveal(sequencerViewChangedAt_, 180.0f);
  snapshot.sequencerFocusReveal = reveal(sequencerFocusChangedAt_, 120.0f);
  for (std::size_t i = 0; i < snapshot.sequencer.steps.size(); ++i)
    snapshot.sequencer.steps[i].editFlash = 1.0f - reveal(sequencerChangedAt_[i], 220.0f);
  snapshot.engineKind = engine != nullptr ? engine->Descriptor().kind : tracks_[activeTrack_].kind;
  snapshot.focusItemId = samplerBrowserMode_ ? "par.sampler.browser"
                                             : (selectedItemId_.empty() ? selectedParameterGroupId_ : selectedItemId_);
  if (snapshot.engineKind == SynthEngineKind::Sampler && activeModifier_ == UiModifier::Parameter)
  {
    const SamplerTrackUiState& sampler = tracks_[activeTrack_].sampler;
    if (samplerNameEntryMode_ || sampler.overwritePending)
    {
      snapshot.focusItemId = "par.sampler.page.save";
    }
    else if (selectedParameterGroupId_ == "par.sampler.group.trim" && !parameterAdjustMode_)
    {
      snapshot.focusItemId = selectedParameterGroupId_;
    }
  }
  snapshot.dx7Algorithm = ParameterValueOrDefault(engine, "global.algorithm", 0);
  snapshot.dx7Feedback = ParameterValueOrDefault(engine, "global.feedback", 0);

  if (mode_ == UiMode::Home)
  {
    snapshot.headerPrimary = HeaderEngine(engine);
    snapshot.headerSecondary = HeaderTrack();
    snapshot.primaryText = PatchName();
    if (snapshot.engineKind == SynthEngineKind::Sampler)
    {
      const SamplerTrackUiState& sampler = tracks_[activeTrack_].sampler;
      PopulateSamplerSnapshotValues(snapshot, engine, sampler);
      snapshot.primaryText = sampler.recording ? sampler.statusText : sampler.sampleName;
      snapshot.secondaryText = SamplerOverviewSummary(engine, sampler);
      snapshot.showSecondaryText = true;
    }
    if (sequencer_.hasHeldSteps && !sequencer_.valueLabel.empty())
    {
      snapshot.primaryText = sequencer_.valueLabel;
    }
    snapshot.looperProgress = LooperProgress();
    snapshot.title = "TRACK " + std::to_string(activeTrack_ + 1);
    PopulatePerformanceKeys(snapshot);
    snapshot.leftBarItems = (sequencer_.shiftActive || sequencer_.patternSelectorVisible) ? BuildSequencerShiftBarSnapshot()
                                                   : (sequencer_.hasHeldSteps ? BuildSequencerEditBarSnapshot()
                                                                              : BuildModifierBarSnapshot(UiModifier::None));
    snapshot.leftBarVisible = true;
    snapshot.leftBarColumns = 1;
    snapshot.leftBarWidthScale = 1.0f;
    snapshot.rightBarItems = BuildHomeActionBarSnapshot();
    snapshot.rightBarVisible = true;
    snapshot.rightBarColumns = 1;
    snapshot.rightBarWidthScale = 1.0f;
    return snapshot;
  }

  snapshot.headerPrimary = std::string(ModifierDisplayName(activeModifier_));
  if (contextMenuHintVisible_ && !heldContextMenuHintTitle_.empty())
  {
    snapshot.showHelpPopup = true;
    snapshot.helpPopupTitle = heldContextMenuHintTitle_;
    snapshot.helpPopupText = heldContextMenuHintText_;
  }
  if (activeModifier_ == UiModifier::Sound && presetBrowserMode_)
  {
    const auto presets = CollectPresetMatches(engine, presetSearchText_);
    snapshot.headerPrimary.clear();
    snapshot.primaryText = presetSearchText_.empty() ? "Preset" : presetSearchText_ + "_";
    snapshot.secondaryText = BuildPresetBrowserListText(presets, presetBrowserSelectionIndex_);
    snapshot.showPresetBrowser = true;
    snapshot.showSecondaryText = true;
    snapshot.contentStyle = UiContentStyle::List;
    snapshot.title = "PRESETS";
    snapshot.searchText = presetSearchText_;
    snapshot.meta = std::to_string(presets.size());
  }
  else if (activeModifier_ == UiModifier::Sound && presetNameEntryMode_)
  {
    snapshot.headerPrimary = "SAVE";
    snapshot.primaryText = selectedValueText_ == "SAVE ERR" ? "SAVE ERR" : "Save As";
    snapshot.secondaryText = presetNameText_.empty() ? "_" : presetNameText_ + "_";
    snapshot.showSecondaryText = true;
    snapshot.contentStyle = UiContentStyle::TextEntry;
    snapshot.title = "SAVE PRESET";
    snapshot.subject = presetNameText_;
    snapshot.dangerous = selectedValueText_ == "SAVE ERR";
    snapshot.meta = snapshot.dangerous ? "SAVE ERR" : std::string{};
  }
  else if (activeModifier_ == UiModifier::Sound && !enginePickerMode_)
  {
    snapshot.headerPrimary = "Track " + std::to_string(activeTrack_ + 1);
    const bool showStatus = selectedItemLabel_ == "Save Preset" || selectedItemId_ == "snd.engine.error";
    snapshot.primaryText = showStatus ? selectedItemLabel_ : HeaderEngine(engine);
    snapshot.secondaryText = showStatus ? selectedValueText_ : PatchName();
    snapshot.showSecondaryText = true;
    snapshot.contentStyle = UiContentStyle::KeyValues;
    snapshot.title = "TRACK " + std::to_string(activeTrack_ + 1);
    snapshot.meta = showStatus ? selectedValueText_ : std::string{};
    snapshot.dangerous = showStatus && IsErrorStatus(selectedValueText_);
  }
  else if (snapshot.engineKind == SynthEngineKind::Dx7 && activeModifier_ == UiModifier::Parameter)
  {
    if (parameterAdjustMode_ && hasContextSelection_)
    {
      snapshot.primaryText = ContextParameterName(engine);
      snapshot.secondaryText = ContextValueText(engine);
      snapshot.contentStyle = UiContentStyle::Adjust;
      snapshot.title = Dx7GroupLabel(selectedParameterGroupId_);
      snapshot.valueLevel = ParameterLevel(engine, selectedItemId_);
    }
    else if (!selectedParameterGroupId_.empty())
    {
      snapshot.primaryText = Dx7GroupLabel(selectedParameterGroupId_);
      snapshot.secondaryText = Dx7GroupSummary(engine, selectedParameterGroupId_);
      snapshot.contentStyle = UiContentStyle::KeyValues;
      snapshot.title = Dx7GroupLabel(selectedParameterGroupId_);
    }
    else
    {
      snapshot.primaryText = "DX7";
      snapshot.secondaryText = FormatDx7AlgorithmSummary(engine);
      snapshot.title = "DX7";
      snapshot.meta = "ALGORITHM " + std::to_string(snapshot.dx7Algorithm);
      snapshot.subtitle = "FEEDBACK " + std::to_string(snapshot.dx7Feedback);
      snapshot.status = PatchName();
    }
    snapshot.showSecondaryText = true;
  }
  else if (snapshot.engineKind == SynthEngineKind::Juno && activeModifier_ == UiModifier::Parameter)
  {
    PopulateJunoSnapshotValues(snapshot, engine);
    if (parameterAdjustMode_ && hasContextSelection_)
    {
      const std::string groupId = JunoGroupIdForParameterId(selectedItemId_);
      snapshot.headerPrimary = JunoGroupLabel(groupId);
      snapshot.primaryText = ContextParameterName(engine);
      snapshot.secondaryText = ContextValueText(engine);
      snapshot.contentStyle = UiContentStyle::Adjust;
      snapshot.title = JunoGroupTitle(groupId);
      snapshot.valueLevel = ParameterLevel(engine, selectedItemId_);
    }
    else if (!selectedParameterGroupId_.empty())
    {
      snapshot.headerPrimary = JunoGroupLabel(selectedParameterGroupId_);
      snapshot.contentStyle = UiContentStyle::KeyValues;
      snapshot.title = JunoGroupTitle(selectedParameterGroupId_);
      if (selectedParameterGroupId_ == "par.juno.group.fx")
      {
        snapshot.primaryText = "Chorus";
        snapshot.secondaryText = DescribeParameterValueOrFallback(engine, "chorus.mode");
      }
      else if (selectedParameterGroupId_ == "par.juno.group.hpf")
      {
        snapshot.primaryText = "MODE";
        snapshot.secondaryText = DescribeParameterValueOrFallback(engine, "hpf.mode");
      }
      else
      {
        snapshot.primaryText = JunoGroupTitle(selectedParameterGroupId_);
        snapshot.secondaryText = JunoGroupSummary(engine, selectedParameterGroupId_);
      }
    }
    else
    {
      snapshot.headerPrimary = "Preset";
      snapshot.primaryText = PatchName();
      snapshot.secondaryText = "RNG " + DescribeParameterValueOrFallback(engine, "dco.range") +
          "  CUT " + DescribeParameterValueOrFallback(engine, "vcf.cutoff");
      snapshot.contentStyle = UiContentStyle::KeyValues;
      snapshot.title = HeaderEngine(engine);
      snapshot.meta = PatchName();
    }
    snapshot.showSecondaryText = true;
  }
  else if (snapshot.engineKind == SynthEngineKind::Sampler && activeModifier_ == UiModifier::Parameter)
  {
    const SamplerTrackUiState& sampler = tracks_[activeTrack_].sampler;
    PopulateSamplerSnapshotValues(snapshot, engine, sampler);
    snapshot.samplerTrimCursor = samplerTrimCursor_;
    snapshot.samplerRecordConfirmMode = samplerRecordConfirmMode_;
    if (samplerNameEntryMode_)
    {
      snapshot.headerPrimary = samplerRenameEntryMode_ ? "REN" : "SAVE";
      snapshot.primaryText = samplerRenameEntryMode_ && !samplerFileStatus_.empty() ? samplerFileStatus_ :
          (samplerRenameEntryMode_ ? "Rename" : "Save");
      snapshot.secondaryText = samplerNameText_.empty() ? "_" : samplerNameText_ + "_";
      snapshot.contentStyle = UiContentStyle::TextEntry;
      snapshot.title = samplerRenameEntryMode_ ? "RENAME" : "SAVE SAMPLE";
      snapshot.subject = samplerNameText_;
      snapshot.dangerous = samplerRenameEntryMode_ && IsErrorStatus(samplerFileStatus_);
      snapshot.meta = snapshot.dangerous ? samplerFileStatus_ : std::string{};
    }
    else if (sampler.overwritePending)
    {
      snapshot.headerPrimary = "SAVE";
      snapshot.primaryText = sampler.statusText == "NAME EXISTS" ? "Replace?" : sampler.statusText;
      snapshot.secondaryText = sampler.draftName.empty() ? sampler.sampleName : sampler.draftName;
      snapshot.contentStyle = UiContentStyle::Confirm;
      snapshot.title = "REPLACE FILE?";
      snapshot.subject = snapshot.secondaryText;
      snapshot.note = sampler.statusText == "NAME EXISTS" ? "THE SAVED FILE WILL BE OVERWRITTEN" : sampler.statusText;
      snapshot.dangerous = true;
    }
    else if (sampler.deleteConfirm)
    {
      snapshot.headerPrimary = "FILE";
      snapshot.primaryText = "Delete?";
      snapshot.secondaryText = (samplerFileTarget_.empty() ? sampler.sampleName : samplerFileTarget_) +
          "\nMay break saved\nproject links";
      snapshot.contentStyle = UiContentStyle::Confirm;
      snapshot.title = "DELETE FILE?";
      snapshot.subject = samplerFileTarget_.empty() ? sampler.sampleName : samplerFileTarget_;
      snapshot.note = "PROJECTS USING IT WILL BREAK";
      snapshot.dangerous = true;
    }
    else if (samplerRecordConfirmMode_)
    {
      snapshot.headerPrimary = "REC";
      snapshot.primaryText = sampler.hasDraft ? "Replace take?" : "Start record?";
      snapshot.secondaryText = sampler.hasDraft ? "Unsaved take\nwill be lost" : "Capture to draft";
      snapshot.title = sampler.hasDraft ? "REPLACE TAKE?" : "RECORD";
      snapshot.dangerous = sampler.hasDraft;
    }
    else if (parameterAdjustMode_ && hasContextSelection_)
    {
      const std::string groupId = SamplerGroupIdForParameterId(selectedItemId_);
      snapshot.headerPrimary = SamplerGroupLabel(groupId);
      snapshot.primaryText = ContextParameterName(engine);
      snapshot.secondaryText = ContextValueText(engine);
      snapshot.contentStyle = UiContentStyle::Adjust;
      snapshot.title = SamplerGroupTitle(groupId);
      snapshot.valueLevel = ParameterLevel(engine, selectedItemId_);
    }
    else if (samplerBrowserMode_)
    {
      snapshot.headerPrimary = "SAMP";
      snapshot.primaryText = !samplerFileStatus_.empty() ? samplerFileStatus_ :
          (samplerBrowserSearchText_.empty() ? "Samples" : samplerBrowserSearchText_ + "_");
      snapshot.secondaryText = BuildSamplerBrowserListText(samplerBrowserEntries_, samplerBrowserSearchText_, samplerBrowserSelectionIndex_);
      snapshot.showPresetBrowser = true;
      snapshot.contentStyle = UiContentStyle::List;
      snapshot.title = "SAMPLES";
      snapshot.searchText = samplerBrowserSearchText_;
      snapshot.meta = samplerFileStatus_;
      snapshot.dangerous = IsErrorStatus(samplerFileStatus_);
    }
    else if (selectedParameterGroupId_ == "par.sampler.group.edit")
    {
      snapshot.headerPrimary = "FILE";
      snapshot.primaryText = samplerFileStatus_.empty() ? "File actions" : samplerFileStatus_;
      snapshot.secondaryText = samplerFileTarget_.empty() ? "BACK to files" :
          samplerFileTarget_ + "\nMay break saved\nproject links";
      snapshot.contentStyle = UiContentStyle::Confirm;
      snapshot.title = "FILE";
      snapshot.meta = samplerFileStatus_;
      snapshot.dangerous = IsErrorStatus(samplerFileStatus_);
      snapshot.subject = samplerFileTarget_.empty() ? "NO FILE SELECTED" : samplerFileTarget_;
      snapshot.note = samplerFileTarget_.empty() ? std::string{} : "PROJECTS USING IT MAY BREAK";
    }
    else if (!selectedParameterGroupId_.empty())
    {
      snapshot.headerPrimary = SamplerGroupLabel(selectedParameterGroupId_);
      snapshot.primaryText = SamplerGroupTitle(selectedParameterGroupId_);
      snapshot.secondaryText = SamplerGroupSummary(engine, sampler, selectedParameterGroupId_);
      snapshot.title = SamplerGroupTitle(selectedParameterGroupId_);
      if (selectedParameterGroupId_ == "par.sampler.group.filter" || selectedParameterGroupId_ == "par.sampler.group.env")
      {
        snapshot.contentStyle = UiContentStyle::KeyValues;
      }
    }
    else
    {
      snapshot.headerPrimary = "Sampler";
      snapshot.primaryText = sampler.sampleName;
      snapshot.secondaryText = SamplerOverviewSummary(engine, sampler);
      snapshot.title = "SAMPLER";
    }
    snapshot.showSecondaryText = true;
  }
  else if (snapshot.engineKind == SynthEngineKind::Drums && activeModifier_ == UiModifier::Parameter)
  {
    snapshot.headerPrimary = "DRUMS";
    snapshot.primaryText = hasContextSelection_ ? ContextParameterName(engine) : "Kit Levels";
    snapshot.secondaryText = hasContextSelection_ ? ContextValueText(engine) : "8 DRUMS\nMASTER LEVEL\nSelect a pad";
    snapshot.showSecondaryText = true;
    snapshot.contentStyle = hasContextSelection_ ? UiContentStyle::Adjust : UiContentStyle::KeyValues;
    snapshot.title = "KIT LEVELS";
    snapshot.valueLevel = hasContextSelection_ ? ParameterLevel(engine, selectedItemId_) : -1.0f;
  }
  else if (snapshot.engineKind == SynthEngineKind::Drums && activeModifier_ == UiModifier::Layout)
  {
    snapshot.headerPrimary = "DRUM PADS";
    snapshot.primaryText = "Fixed Pads";
    snapshot.title = "DRUM PADS";
    snapshot.meta = "GM MAP";
    PopulatePerformanceKeys(snapshot);
    snapshot.secondaryText = "COLUMNS 1 - 8\nSAME ON EVERY ROW\nGM DRUM NOTES";
    snapshot.showSecondaryText = true;
  }
  else if (activeModifier_ == UiModifier::Layout)
  {
    snapshot.headerPrimary = "Keyboard";
    PopulatePerformanceKeys(snapshot);
    snapshot.keyboardLayoutMode = keyboardLayoutMode_;
    snapshot.keyboardRootName = KeyboardRootName();
    snapshot.keyboardScaleName = KeyboardScaleName();
    snapshot.keyboardRootDisabled = keyboardLayoutMode_ == KeyboardLayoutMode::Piano;
    snapshot.keyboardScaleDisabled = keyboardLayoutMode_ == KeyboardLayoutMode::Piano;
    bool showLayoutValue = true;
    if (selectedParameterGroupId_.empty() && hasContextSelection_ && selectedItemId_ == "lay.scl")
    {
      snapshot.primaryText = "Scale";
      snapshot.secondaryText = selectedValueText_;
    }
    else if (selectedParameterGroupId_ == "lay.root")
    {
      snapshot.primaryText = "Root";
      snapshot.secondaryText = KeyboardRootName();
    }
    else if (selectedParameterGroupId_ == "lay.scl")
    {
      snapshot.primaryText = "Scale";
      snapshot.secondaryText = keyboardLayoutMode_ == KeyboardLayoutMode::Piano ? "Disabled in Piano" : KeyboardScaleName();
    }
    else if (selectedParameterGroupId_ == "lay.mode")
    {
      snapshot.primaryText = "Mode";
      snapshot.secondaryText = KeyboardLayoutName();
    }
    else if (selectedParameterGroupId_ == "lay.fx")
    {
      snapshot.primaryText = "Effect";
      snapshot.secondaryText = "EMPTY";
    }
    else
    {
      snapshot.primaryText = KeyboardSummaryText();
      snapshot.secondaryText.clear();
      showLayoutValue = false;
      snapshot.contentStyle = UiContentStyle::KeyValues;
    }
    snapshot.showSecondaryText = showLayoutValue;
    snapshot.title = showLayoutValue ? snapshot.primaryText : std::string{"KEYBOARD"};
  }
  else if (activeModifier_ == UiModifier::Other)
  {
    snapshot.headerPrimary = "SYS";
    snapshot.showSecondaryText = true;
    if (sequencerDeleteConfirm_)
    {
      snapshot.focusItemId = "sys.seq.delete";
      snapshot.headerPrimary = "PATTERN";
      snapshot.headerSecondary = std::string("P") + (sequencerDeletePattern_ < 9 ? "0" : "") +
                                 std::to_string(sequencerDeletePattern_ + 1);
      snapshot.primaryText = "Remove pattern?";
      snapshot.secondaryText = "Track " + std::to_string(sequencerDeleteTrack_ + 1) + "\nPattern " +
                               std::to_string(sequencerDeletePattern_ + 1) + "\nNotes will be lost";
      snapshot.contentStyle = UiContentStyle::Confirm;
      snapshot.title = "DELETE PATTERN?";
      snapshot.subject = snapshot.headerSecondary;
      snapshot.note = "ITS NOTES ON T" + std::to_string(sequencerDeleteTrack_ + 1) + " WILL BE LOST";
      snapshot.dangerous = true;
    }
    else if (projectNameEntryMode_)
    {
      snapshot.headerPrimary = "PROJECT";
      snapshot.primaryText = projectStatus_.empty() ? "Save As" : projectStatus_;
      snapshot.secondaryText = projectNameText_ + "_";
      snapshot.contentStyle = UiContentStyle::TextEntry;
      snapshot.title = "SAVE PROJECT";
      snapshot.subject = projectNameText_;
      snapshot.meta = projectStatus_;
      snapshot.dangerous = IsErrorStatus(projectStatus_);
    }
    else if (projectConfirmCommand_ != UiCommandType::None)
    {
      snapshot.headerPrimary = "PROJECT";
      snapshot.primaryText = projectConfirmCommand_ == UiCommandType::ProjectOverwrite ? "Replace file?" :
          (projectConfirmCommand_ == UiCommandType::ProjectLoad ? "Load project?" : "New project?");
      snapshot.secondaryText = projectConfirmCommand_ == UiCommandType::ProjectNew ? "Unsaved work\nwill be lost" :
          projectConfirmName_ + (projectConfirmCommand_ == UiCommandType::ProjectLoad ? "\nUnsaved work\nwill be lost" : "");
      snapshot.contentStyle = UiContentStyle::Confirm;
      snapshot.title = projectConfirmCommand_ == UiCommandType::ProjectOverwrite ? "REPLACE FILE?" :
          (projectConfirmCommand_ == UiCommandType::ProjectLoad ? "LOAD PROJECT?" : "NEW PROJECT?");
      snapshot.subject = projectConfirmCommand_ == UiCommandType::ProjectNew ? std::string{"UNTITLED"} : projectConfirmName_;
      snapshot.note = projectConfirmCommand_ == UiCommandType::ProjectOverwrite ? "THE SAVED FILE WILL BE OVERWRITTEN" :
                                                                                 "UNSAVED CHANGES WILL BE LOST";
      snapshot.dangerous = true;
    }
    else if (selectedParameterGroupId_ == "sys.load")
    {
      snapshot.headerPrimary = "PROJECT";
      snapshot.primaryText = projectStatus_.empty() ? "Load" : projectStatus_;
      snapshot.secondaryText = projectNames_.empty() ? "No saved projects" : BuildSelectableListText(projectNames_, projectSelectionIndex_);
      snapshot.showPresetBrowser = true;
      snapshot.contentStyle = UiContentStyle::List;
      snapshot.title = "PROJECTS";
      snapshot.meta = projectStatus_.empty() ? std::to_string(projectNames_.size()) : projectStatus_;
      snapshot.dangerous = IsErrorStatus(projectStatus_);
    }
    else if (selectedParameterGroupId_.rfind("sys.seq", 0) == 0)
    {
      snapshot.headerPrimary = "SEQ";
      snapshot.focusItemId = selectedParameterGroupId_;
      snapshot.headerSecondary = "T" + std::to_string(activeTrack_ + 1);
      snapshot.meta = snapshot.headerSecondary;
      if (selectedParameterGroupId_ == "sys.seq.tempo")
      {
        snapshot.primaryText = "Tempo";
        snapshot.secondaryText = std::to_string(sequencer_.bpm) + " BPM";
        snapshot.contentStyle = UiContentStyle::Adjust;
        snapshot.title = "TEMPO";
        snapshot.meta = sequencer_.playing ? "PLAYING" : "STOPPED";
        snapshot.valueLevel = static_cast<float>(sequencer_.bpm - 30) / 270.0f;
      }
      else if (selectedParameterGroupId_ == "sys.seq.track")
      {
        snapshot.primaryText = "Track " + std::to_string(activeTrack_ + 1);
        snapshot.secondaryText = std::string(sequencer_.trackMuted ? "MUTED" : "OUTPUT ON") +
            (sequencer_.trackEnabled ? "\nSEQ ON" : "\nSEQ OFF");
      }
      else if (selectedParameterGroupId_ == "sys.seq.pattern")
      {
        snapshot.primaryText = "Pattern";
        snapshot.secondaryText = std::to_string(sequencer_.activePattern + 1) + " / " +
            std::to_string(sequencer_.patternCount) + "\n" + std::to_string(sequencer_.populatedStepCount) + " filled steps";
      }
      else
      {
        snapshot.primaryText = sequencer_.playing ? "Playing" : "Stopped";
        snapshot.secondaryText = std::to_string(sequencer_.bpm) + " BPM";
        snapshot.contentStyle = UiContentStyle::KeyValues;
        snapshot.title = "SEQUENCER";
        snapshot.meta = sequencer_.playing ? "PLAYING" : "STOPPED";
      }
    }
    else if (selectedParameterGroupId_ == "sys.project")
    {
      snapshot.primaryText = projectName_.empty() ? "Untitled" : projectName_;
      snapshot.secondaryText = projectStatus_.empty() ? "Project" : projectStatus_;
      snapshot.contentStyle = UiContentStyle::Confirm;
      snapshot.title = "PROJECT";
      snapshot.subject = snapshot.primaryText;
      snapshot.meta = projectStatus_;
      snapshot.dangerous = IsErrorStatus(projectStatus_);
    }
    else
    {
      snapshot.primaryText = hasContextSelection_ ? ContextParameterName(engine) : "System";
      snapshot.secondaryText = hasContextSelection_ ? ContextValueText(engine) : "Project / Sequencer";
      snapshot.contentStyle = UiContentStyle::KeyValues;
      snapshot.title = "SYSTEM";
    }
  }
  else
  {
    snapshot.primaryText = hasContextSelection_ ? ContextParameterName(engine)
                                                : (activeModifier_ == UiModifier::Layout ? std::string{"Keyboard"}
                                                                                         : std::string{"Settings"});
    snapshot.secondaryText = hasContextSelection_ ? ContextValueText(engine) : std::string{};
    snapshot.showSecondaryText = hasContextSelection_;
    if (activeModifier_ == UiModifier::Sound && enginePickerMode_)
    {
      snapshot.contentStyle = UiContentStyle::Adjust;
      snapshot.title = "ENGINE";
      snapshot.meta = enginePickerPreviewKind_ == enginePickerInitialKind_ ? "CURRENT" : "";
    }
  }
  snapshot.leftBarItems = BuildContextBackBarSnapshot();
  snapshot.leftBarVisible = false;
  snapshot.leftBarColumns = 1;
  snapshot.leftBarWidthScale = 1.0f;
  snapshot.rightBarItems = BuildContextBarSnapshot(engine);
  snapshot.rightBarVisible = true;
  snapshot.rightBarColumns = ContextMenuColumnCount();
  snapshot.rightBarWidthScale = 1.0f;
  return snapshot;
}

void SynthUiModel::StartTransition(const UiScreenSnapshot& from, const UiScreenSnapshot& to)
{
  transition_.from = from;
  transition_.to = to;

  if (from.mode == to.mode && from.modifier == to.modifier && from.primaryText == to.primaryText &&
      from.secondaryText == to.secondaryText && from.leftBarVisible == to.leftBarVisible &&
      from.leftBarWidthScale == to.leftBarWidthScale && from.rightBarColumns == to.rightBarColumns &&
      from.rightBarWidthScale == to.rightBarWidthScale)
  {
    transition_.active = false;
    transition_.progress = 1.0f;
    return;
  }

  transition_.active = true;
  transition_.progress = 0.0f;
}

void SynthUiModel::CancelTransition()
{
  transition_.active = false;
  transition_.progress = 1.0f;
}

void SynthUiModel::ActivateModifier(UiModifier modifier)
{
  sequencerDeleteConfirm_ = false;
  if (modifier == UiModifier::None)
  {
    ExitToHome();
    return;
  }

  if (mode_ == UiMode::ContextMenu && activeModifier_ == modifier)
  {
    return;
  }

  if (samplerNameEntryMode_ || tracks_[activeTrack_].sampler.deleteConfirm || tracks_[activeTrack_].sampler.overwritePending)
    QueueCommand(UiCommandType::SamplerCancelOverlay);
  ExitSamplerBrowser();
  ExitSamplerNameEntry();
  ExitSamplerRecordConfirm();
  samplerFileTarget_.clear();
  samplerFileStatus_.clear();

  mode_ = UiMode::ContextMenu;
  activeModifier_ = modifier;
  currentPage_ = 0;
  activeMenuRow_ = 0;
  activeMenuColumn_ = 0;
  enginePickerMode_ = false;
  hasPendingEngineKind_ = false;
  pendingEngineKind_ = SynthEngineKind::None;
  presetBrowserMode_ = false;
  presetSearchText_.clear();
  presetBrowserSelectionIndex_ = 0;
  contextLatched_ = false;
  selectedParameterGroupId_.clear();
  projectNameEntryMode_ = false;
  projectConfirmCommand_ = UiCommandType::None;
  ClearContextSelection();
}

void SynthUiModel::ClearContextSelection()
{
  hasContextSelection_ = false;
  hasMenuHighlight_ = false;
  parameterAdjustMode_ = false;
  selectionKind_ = ContextSelectionKind::None;
  selectedItemId_.clear();
  selectedItemLabel_.clear();
  selectedValueText_.clear();
  selectedParameterEntryValue_ = 0;
}

void SynthUiModel::HighlightMenuIndex(std::size_t index)
{
  hasMenuHighlight_ = true;
  if (ContextMenuColumnCount() == 1)
  {
    activeMenuRow_ = index;
    activeMenuColumn_ = 0;
    return;
  }

  activeMenuRow_ = index / kContextMenuColumns;
  activeMenuColumn_ = index % kContextMenuColumns;
}

void SynthUiModel::SelectStatus(const std::string& label, const std::string& value, const std::string& itemId)
{
  hasContextSelection_ = true;
  selectionKind_ = ContextSelectionKind::Status;
  selectedItemId_ = itemId;
  selectedItemLabel_ = label;
  selectedValueText_ = value;
}

void SynthUiModel::AdjustKeyboardOctave(int delta)
{
  if (tracks_[activeTrack_].kind == SynthEngineKind::Drums) return;
  keyboardOctaveOffset_ = std::clamp(keyboardOctaveOffset_ + delta, kKeyboardOctaveMin, kKeyboardOctaveMax);
  homeOctaveStatusUntil_ = std::chrono::steady_clock::now() + kHomeOctaveStatusDuration;
}

void SynthUiModel::AdjustKeyboardRoot(int delta)
{
  keyboardRootNote_ = PositiveModulo(keyboardRootNote_ + delta, 12);
  SelectStatus("Root", KeyboardRootName(), "lay.root");
}

void SynthUiModel::AdjustKeyboardScale(int delta)
{
  if (keyboardLayoutMode_ == KeyboardLayoutMode::Piano)
  {
    SelectStatus("Scale", "PIANO", "lay.scl");
    return;
  }

  const int scaleCount = static_cast<int>(kKeyboardScales.size());
  keyboardScaleIndex_ = static_cast<std::size_t>(PositiveModulo(static_cast<int>(keyboardScaleIndex_) + delta, scaleCount));
  SelectStatus("Scale", KeyboardScaleName(), "lay.scl");
}

void SynthUiModel::SetKeyboardLayoutMode(KeyboardLayoutMode mode)
{
  keyboardLayoutMode_ = mode;
  SelectStatus("Mode", KeyboardLayoutName(), "lay.mode");
}

std::string SynthUiModel::KeyboardRootName() const
{
  return kKeyboardRootNames[static_cast<std::size_t>(PositiveModulo(keyboardRootNote_, 12))];
}

std::string SynthUiModel::KeyboardScaleName() const
{
  return KeyboardScaleAt(keyboardScaleIndex_).fullName;
}

std::string SynthUiModel::KeyboardLayoutName() const
{
  return KeyboardLayoutModeLabel(keyboardLayoutMode_);
}

std::string SynthUiModel::KeyboardSummaryText() const
{
  if (keyboardLayoutMode_ == KeyboardLayoutMode::Piano)
  {
    return "Piano";
  }

  const char* layoutLabel = keyboardLayoutMode_ == KeyboardLayoutMode::Chromatic ? "CHRM" : "OCT";
  return KeyboardRootName() + " " + KeyboardScaleAt(keyboardScaleIndex_).label + " " + layoutLabel;
}

void SynthUiModel::EnterEnginePicker()
{
  enginePickerMode_ = true;
  enginePickerInitialKind_ = tracks_[activeTrack_].kind;
  enginePickerPreviewKind_ = enginePickerInitialKind_;
  hasPendingEngineKind_ = false;
  pendingEngineKind_ = SynthEngineKind::None;
  heldContextMenuIndex_ = -1;
  SelectStatus("Engine", ToString(enginePickerPreviewKind_), "snd.engine");
}

void SynthUiModel::EnterPresetBrowser(const ISynthEngine* engine)
{
  presetBrowserMode_ = true;
  presetSearchText_.clear();
  presetBrowserSelectionIndex_ = 0;
  heldContextMenuIndex_ = -1;
  hasMenuHighlight_ = false;
  hasContextSelection_ = true;
  selectionKind_ = ContextSelectionKind::Preset;
  selectedItemId_ = "snd.preset";
  selectedItemLabel_ = "Preset";
  selectedValueText_ = engine != nullptr ? engine->CurrentPresetName() : PatchName();

  const auto presets = CollectPresetMatches(engine, presetSearchText_);
  if (!presets.empty())
  {
    presetBrowserSelectionIndex_ = ActivePresetMatchIndex(presets);
  }
}

void SynthUiModel::EnterSamplerBrowser()
{
  samplerBrowserMode_ = true;
  samplerEditFromBrowser_ = false;
  samplerFileTarget_.clear();
  samplerFileStatus_.clear();
  samplerBrowserAllEntries_ = CollectSamplerBrowserEntries();
  samplerBrowserSearchText_.clear();
  samplerBrowserSelectionIndex_ = 0;
  heldContextMenuIndex_ = -1;
  hasMenuHighlight_ = false;
  hasContextSelection_ = true;
  selectionKind_ = ContextSelectionKind::Status;
  selectedItemId_ = "par.sampler.browser";
  selectedItemLabel_ = "Samples";
  RefreshSamplerBrowserEntries();
}

void SynthUiModel::ExitSamplerBrowser()
{
  samplerBrowserMode_ = false;
  samplerBrowserAllEntries_.clear();
  samplerBrowserEntries_.clear();
  samplerBrowserSearchText_.clear();
  samplerBrowserSelectionIndex_ = 0;
  heldContextMenuIndex_ = -1;
}

void SynthUiModel::RefreshSamplerBrowserEntries()
{
  samplerBrowserEntries_ = FilterSamplerBrowserEntries(samplerBrowserAllEntries_, samplerBrowserSearchText_);
  if (samplerBrowserEntries_.empty())
  {
    samplerBrowserSelectionIndex_ = 0;
    selectedValueText_ = samplerBrowserSearchText_.empty() ? "EMPTY" : samplerBrowserSearchText_;
    return;
  }

  samplerBrowserSelectionIndex_ = std::min(samplerBrowserSelectionIndex_, samplerBrowserEntries_.size() - 1);
  selectedValueText_ = samplerBrowserEntries_[samplerBrowserSelectionIndex_];
}

void SynthUiModel::EnterParameterAdjustMode(const SynthParameterInfo& parameter, std::size_t menuIndex)
{
  hasContextSelection_ = true;
  parameterAdjustMode_ = true;
  selectionKind_ = ContextSelectionKind::Parameter;
  selectedItemId_ = parameter.id;
  selectedItemLabel_ = parameter.label;
  selectedParameterEntryValue_ = parameter.value;
  hasMenuHighlight_ = false;
  HighlightMenuIndex(menuIndex);
  hasMenuHighlight_ = false;
}

void SynthUiModel::QueueCommand(UiCommandType type, const std::string& text, const std::string& fileName)
{
  UiCommand command;
  command.type = type;
  command.trackIndex = activeTrack_;
  command.text = text;
  command.fileName = fileName;
  pendingCommands_.push_back(command);
}

void SynthUiModel::ExitPresetNameEntry()
{
  presetNameEntryMode_ = false;
  presetNameText_.clear();
}

void SynthUiModel::ExitSamplerNameEntry()
{
  samplerNameEntryMode_ = false;
  samplerRenameEntryMode_ = false;
  samplerNameText_.clear();
}

void SynthUiModel::ExitSamplerRecordConfirm()
{
  samplerRecordConfirmMode_ = false;
}

void SynthUiModel::MoveSamplerTrimCursor(int delta)
{
  samplerTrimCursor_ = std::clamp(samplerTrimCursor_ + delta, 0, 127);
  SelectStatus("Cursor", std::to_string(samplerTrimCursor_), "par.sampler.group.trim");
}

void SynthUiModel::SetSamplerTrimBoundary(ISynthEngine* engine, bool begin)
{
  if (engine == nullptr)
  {
    return;
  }

  SynthParameterInfo start;
  SynthParameterInfo end;
  const int currentStart = engine->GetParameterInfo("sample.start", start) ? start.value : 0;
  const int currentEnd = engine->GetParameterInfo("sample.end", end) ? end.value : 127;
  if (begin)
  {
    samplerTrimCursor_ = std::min(samplerTrimCursor_, std::max(0, currentEnd - 1));
    (void)engine->SetParameterValue("sample.start", samplerTrimCursor_);
    SelectStatus("Begin", engine->DescribeParameterValue("sample.start"), "sample.start");
    return;
  }

  samplerTrimCursor_ = std::max(samplerTrimCursor_, std::min(127, currentStart + 1));
  (void)engine->SetParameterValue("sample.end", samplerTrimCursor_);
  SelectStatus("End", engine->DescribeParameterValue("sample.end"), "sample.end");
}

bool SynthUiModel::HandlePresetBrowserAutoScroll(const ISynthEngine* engine, std::chrono::steady_clock::time_point now)
{
  if (mode_ != UiMode::ContextMenu || !presetBrowserMode_ || activeModifier_ != UiModifier::Sound || heldContextMenuIndex_ < 0)
  {
    return false;
  }

  const auto menu = CurrentMenuCells(engine);
  const std::size_t menuIndex = static_cast<std::size_t>(heldContextMenuIndex_);
  if (menuIndex >= menu.size())
  {
    return false;
  }

  const ContextActionId actionId = menu[menuIndex].actionId;
  if (actionId != ContextActionId::PresetPrev && actionId != ContextActionId::PresetNext)
  {
    return false;
  }

  if (contextMenuPressStartedAt_ == std::chrono::steady_clock::time_point{} ||
      (now - contextMenuPressStartedAt_) < kContextHintHoldDelay)
  {
    return true;
  }

  if (presetBrowserAutoScrollLastAt_ != std::chrono::steady_clock::time_point{} &&
      (now - presetBrowserAutoScrollLastAt_) < kPresetBrowserAutoScrollInterval)
  {
    return true;
  }

  StepPresetBrowserSelection(engine, actionId == ContextActionId::PresetNext ? 1 : -1);
  presetBrowserAutoScrollLastAt_ = now;
  heldContextMenuActionConsumed_ = true;
  return true;
}

void SynthUiModel::StepPresetBrowserSelection(const ISynthEngine* engine, int delta)
{
  const auto presets = CollectPresetMatches(engine, presetSearchText_);
  if (presets.empty())
  {
    return;
  }

  if (delta < 0)
  {
    if (presetBrowserSelectionIndex_ > 0)
    {
      --presetBrowserSelectionIndex_;
    }
    return;
  }

  if (delta > 0)
  {
    presetBrowserSelectionIndex_ = std::min(presetBrowserSelectionIndex_ + 1, presets.size() - 1);
  }
}

bool SynthUiModel::IsContextRoot() const
{
  const SamplerTrackUiState& sampler = tracks_[activeTrack_].sampler;
  return mode_ == UiMode::ContextMenu &&
         !samplerRecordConfirmMode_ &&
         !samplerNameEntryMode_ &&
         !sampler.overwritePending &&
         !sampler.deleteConfirm &&
         !samplerBrowserMode_ &&
         !presetBrowserMode_ &&
         !presetNameEntryMode_ &&
         !projectNameEntryMode_ &&
         projectConfirmCommand_ == UiCommandType::None &&
         !enginePickerMode_ &&
         !parameterAdjustMode_ &&
         selectedParameterGroupId_.empty() &&
         currentPage_ == 0 &&
         !hasContextSelection_;
}

void SynthUiModel::ApplyMenuCell(ISynthEngine* engine, const ContextMenuCell& cell, std::size_t index)
{
  if (cell.type == MenuCellType::Empty)
  {
    return;
  }

  if (FirstHeldModifier() != UiModifier::None)
  {
    contextLatched_ = true;
  }

  HighlightMenuIndex(index);
  if (cell.disabled)
  {
    SelectStatus(cell.fullName.empty() ? cell.label : cell.fullName,
                 cell.description.empty() ? "DISABLED" : cell.description,
                 cell.itemId);
    return;
  }

  if (cell.type == MenuCellType::NextPage)
  {
    currentPage_ = (currentPage_ + 1) % std::max<std::size_t>(1, PageCountForModifier(engine));
    ClearContextSelection();
    return;
  }

  if (cell.type == MenuCellType::Item)
  {
    if (activeModifier_ == UiModifier::Layout)
    {
      if (cell.itemId == "lay.root" || cell.itemId == "lay.scl" || cell.itemId == "lay.mode" || cell.itemId == "lay.fx")
      {
        if (cell.itemId == "lay.scl" && keyboardLayoutMode_ == KeyboardLayoutMode::Piano)
        {
          SelectStatus("Scale", "PIANO", "lay.scl");
          return;
        }

        selectedParameterGroupId_ = cell.itemId;
        currentPage_ = 0;
        ClearContextSelection();
        if (cell.itemId == "lay.root")
        {
          SelectStatus("Root", KeyboardRootName(), "lay.root");
        }
        else if (cell.itemId == "lay.scl")
        {
          SelectStatus("Scale", KeyboardScaleName(), "lay.scl");
        }
        else if (cell.itemId == "lay.mode")
        {
          SelectStatus("Mode", KeyboardLayoutName(), "lay.mode");
        }
        else
        {
          SelectStatus("Effect", "EMPTY", "lay.fx");
        }
        return;
      }
    }

    if (activeModifier_ == UiModifier::Sound && cell.itemId == "snd.engine")
    {
      EnterEnginePicker();
      return;
    }

    if (activeModifier_ == UiModifier::Sound && cell.itemId == "snd.preset")
    {
      EnterPresetBrowser(engine);
      return;
    }

    if (activeModifier_ == UiModifier::Sound && cell.itemId == "snd.save")
    {
      ApplyMenuCell(engine, MakeActionCell("SAVE", ContextActionId::SavePreset), index);
      return;
    }

    if (activeModifier_ == UiModifier::Sound && cell.itemId == "snd.saveas")
    {
      ApplyMenuCell(engine, MakeActionCell("SAVEAS", ContextActionId::SavePresetAs), index);
      return;
    }

    if (activeModifier_ == UiModifier::Other && cell.itemId.rfind("sys.", 0) == 0)
    {
      selectedParameterGroupId_ = cell.itemId;
      projectStatus_.clear();
      currentPage_ = 0;
      ClearContextSelection();
      return;
    }

    if (activeModifier_ == UiModifier::Parameter && IsDx7ParameterEngine(engine))
    {
      if (IsDx7ParameterGroupId(cell.itemId))
      {
        selectedParameterGroupId_ = cell.itemId;
        currentPage_ = 0;
        ClearContextSelection();
        return;
      }

      if (cell.itemId == "par.dx7.back")
      {
        selectedParameterGroupId_.clear();
        currentPage_ = 0;
        ClearContextSelection();
        return;
      }

      if (!selectedParameterGroupId_.empty() && engine != nullptr)
      {
        SynthParameterInfo parameter;
        if (engine->GetParameterInfo(cell.itemId, parameter))
        {
          EnterParameterAdjustMode(parameter, index);
          return;
        }
      }
    }

    if (activeModifier_ == UiModifier::Parameter && IsJunoParameterEngine(engine))
    {
      if (IsJunoParameterGroupId(cell.itemId))
      {
        selectedParameterGroupId_ = cell.itemId;
        currentPage_ = 0;
        if (cell.itemId == "par.juno.group.amp" && engine != nullptr)
        {
          SynthParameterInfo parameter;
          if (engine->GetParameterInfo("vca.level", parameter))
          {
            EnterParameterAdjustMode(parameter, index);
            return;
          }
        }

        if (cell.itemId == "par.juno.group.fx")
        {
          SelectStatus("Chorus", DescribeParameterValueOrFallback(engine, "chorus.mode"), "chorus.mode");
          return;
        }

        if (cell.itemId == "par.juno.group.hpf")
        {
          SelectStatus("HPF", DescribeParameterValueOrFallback(engine, "hpf.mode"), "hpf.mode");
          return;
        }

        ClearContextSelection();
        return;
      }

      if (cell.itemId == "par.juno.back")
      {
        selectedParameterGroupId_.clear();
        currentPage_ = 0;
        ClearContextSelection();
        return;
      }

      if (!selectedParameterGroupId_.empty() && engine != nullptr)
      {
        SynthParameterInfo parameter;
        if (engine->GetParameterInfo(cell.itemId, parameter))
        {
          EnterParameterAdjustMode(parameter, index);
          return;
        }
      }
    }

    if (activeModifier_ == UiModifier::Parameter && IsSamplerParameterEngine(engine))
    {
      if (IsSamplerParameterGroupId(cell.itemId))
      {
        ExitSamplerRecordConfirm();
        selectedParameterGroupId_ = cell.itemId;
        currentPage_ = 0;
        if (cell.itemId == "par.sampler.group.trim" && engine != nullptr)
        {
          SynthParameterInfo start;
          samplerTrimCursor_ = engine->GetParameterInfo("sample.start", start) ? start.value : 0;
        }

        if (IsSamplerDirectAdjustGroupId(cell.itemId) && engine != nullptr)
        {
          SynthParameterInfo parameter;
          const std::string parameterId = SamplerDirectAdjustParameterId(cell.itemId);
          if (!parameterId.empty() && engine->GetParameterInfo(parameterId, parameter))
          {
            EnterParameterAdjustMode(parameter, index);
            return;
          }
        }

        ClearContextSelection();
        return;
      }

      if (cell.itemId == "par.sampler.back")
      {
        selectedParameterGroupId_.clear();
        currentPage_ = 0;
        ClearContextSelection();
        return;
      }

      if (engine != nullptr)
      {
        SynthParameterInfo parameter;
        if (engine->GetParameterInfo(cell.itemId, parameter))
        {
          EnterParameterAdjustMode(parameter, index);
          return;
        }
      }
    }

    if (activeModifier_ == UiModifier::Parameter && engine != nullptr)
    {
      SynthParameterInfo parameter;
      if (engine->GetParameterInfo(cell.itemId, parameter))
      {
        EnterParameterAdjustMode(parameter, index);
        return;
      }
    }
    const auto presentation = MenuItemPresentation(activeModifier_, cell.itemId, tracks_[activeTrack_].patchName, engine);
    SelectStatus(presentation.first, presentation.second, cell.itemId);
    return;
  }

  if (cell.type != MenuCellType::UtilityAction)
  {
    return;
  }

  switch (cell.actionId)
  {
    case ContextActionId::EngineNext:
      if (enginePickerMode_)
      {
        enginePickerPreviewKind_ = StepSelectableEngineKind(enginePickerPreviewKind_, 1);
        SelectStatus("Engine", ToString(enginePickerPreviewKind_), "snd.engine");
      }
      break;
    case ContextActionId::EnginePrev:
      if (enginePickerMode_)
      {
        enginePickerPreviewKind_ = StepSelectableEngineKind(enginePickerPreviewKind_, -1);
        SelectStatus("Engine", ToString(enginePickerPreviewKind_), "snd.engine");
      }
      break;
    case ContextActionId::EngineReset:
      if (enginePickerMode_)
      {
        enginePickerPreviewKind_ = enginePickerInitialKind_;
        SelectStatus("Engine", ToString(enginePickerPreviewKind_), "snd.engine");
      }
      break;
    case ContextActionId::EngineLoad:
      if (enginePickerMode_)
      {
        pendingEngineKind_ = enginePickerPreviewKind_;
        hasPendingEngineKind_ = true;
        enginePickerMode_ = false;
        heldContextMenuIndex_ = -1;
        currentPage_ = 0;
        ClearContextSelection();
      }
      break;
    case ContextActionId::PresetLoad:
      if (samplerBrowserMode_)
      {
        if (!samplerBrowserEntries_.empty())
        {
          const std::size_t selectedIndex = std::min(samplerBrowserSelectionIndex_, samplerBrowserEntries_.size() - 1);
          QueueCommand(UiCommandType::SamplerOpenBrowser, samplerBrowserEntries_[selectedIndex]);
        }
        break;
      }
      if (presetBrowserMode_ && engine != nullptr)
      {
        const auto presets = CollectPresetMatches(engine, presetSearchText_);
        if (!presets.empty())
        {
          const std::size_t selectedIndex = std::min(presetBrowserSelectionIndex_, presets.size() - 1);
          if (engine->LoadPreset(presets[selectedIndex].id))
          {
            tracks_[activeTrack_].patchName = engine->CurrentPresetName();
            presetBrowserMode_ = false;
            presetSearchText_.clear();
            presetBrowserSelectionIndex_ = 0;
            heldContextMenuIndex_ = -1;
            ClearContextSelection();
          }
        }
      }
      break;
    case ContextActionId::PresetPrev:
      if (samplerBrowserMode_) samplerFileStatus_.clear();
      if (activeModifier_ == UiModifier::Other && selectedParameterGroupId_ == "sys.load")
      {
        if (projectSelectionIndex_ > 0) --projectSelectionIndex_;
        break;
      }
      if (samplerBrowserMode_ && samplerBrowserSelectionIndex_ > 0)
      {
        --samplerBrowserSelectionIndex_;
        SelectStatus("Samples", samplerBrowserEntries_[samplerBrowserSelectionIndex_], "par.sampler.browser");
        break;
      }
      if (presetBrowserMode_ && presetBrowserSelectionIndex_ > 0)
      {
        --presetBrowserSelectionIndex_;
      }
      break;
    case ContextActionId::PresetNext:
      if (samplerBrowserMode_) samplerFileStatus_.clear();
      if (activeModifier_ == UiModifier::Other && selectedParameterGroupId_ == "sys.load")
      {
        if (!projectNames_.empty()) projectSelectionIndex_ = std::min(projectSelectionIndex_ + 1, projectNames_.size() - 1);
        break;
      }
      if (samplerBrowserMode_ && !samplerBrowserEntries_.empty())
      {
        samplerBrowserSelectionIndex_ = std::min(samplerBrowserSelectionIndex_ + 1, samplerBrowserEntries_.size() - 1);
        SelectStatus("Samples", samplerBrowserEntries_[samplerBrowserSelectionIndex_], "par.sampler.browser");
        break;
      }
      if (presetBrowserMode_ && engine != nullptr)
      {
        const auto presets = CollectPresetMatches(engine, presetSearchText_);
        if (!presets.empty())
        {
          presetBrowserSelectionIndex_ = std::min(presetBrowserSelectionIndex_ + 1, presets.size() - 1);
        }
      }
      break;
    case ContextActionId::PresetDelete:
      if (samplerBrowserMode_) samplerFileStatus_.clear();
      if (samplerBrowserMode_ && !samplerBrowserSearchText_.empty())
      {
        samplerBrowserSearchText_.pop_back();
        samplerBrowserSelectionIndex_ = 0;
        RefreshSamplerBrowserEntries();
        break;
      }
      if (presetBrowserMode_ && !presetSearchText_.empty())
      {
        presetSearchText_.pop_back();
        const auto presets = CollectPresetMatches(engine, presetSearchText_);
        presetBrowserSelectionIndex_ = presetSearchText_.empty() ? ActivePresetMatchIndex(presets) : 0;
      }
      break;
    case ContextActionId::PresetClear:
      if (presetBrowserMode_)
      {
        presetSearchText_.clear();
        presetBrowserSelectionIndex_ = ActivePresetMatchIndex(CollectPresetMatches(engine, presetSearchText_));
      }
      break;
    case ContextActionId::PresetNameSave:
      if (engine != nullptr && !presetNameText_.empty())
      {
        if (!engine->SavePreset("", presetNameText_))
        {
          SelectStatus("Save As", "SAVE ERR", "snd.preset");
          break;
        }
        tracks_[activeTrack_].patchName = engine->CurrentPresetName();
        ExitPresetNameEntry();
        SelectStatus("Save As", tracks_[activeTrack_].patchName, "snd.preset");
      }
      break;
    case ContextActionId::PresetNameBackspace:
      if (!presetNameText_.empty())
      {
        presetNameText_.pop_back();
      }
      SelectStatus("Name", presetNameText_.empty() ? "_" : presetNameText_, "snd.preset");
      break;
    case ContextActionId::ParameterDecLarge:
      if (engine != nullptr)
      {
        const int delta = selectedItemId_ == "sample.root" ? -12 : -10;
        (void)engine->AdjustParameterValue(selectedItemId_, delta);
        hasContextSelection_ = true;
        selectionKind_ = ContextSelectionKind::Parameter;
      }
      break;
    case ContextActionId::ParameterDecSmall:
      if (engine != nullptr)
      {
        (void)engine->AdjustParameterValue(selectedItemId_, -1);
        hasContextSelection_ = true;
        selectionKind_ = ContextSelectionKind::Parameter;
      }
      break;
    case ContextActionId::ParameterIncSmall:
      if (engine != nullptr)
      {
        (void)engine->AdjustParameterValue(selectedItemId_, 1);
        hasContextSelection_ = true;
        selectionKind_ = ContextSelectionKind::Parameter;
      }
      break;
    case ContextActionId::ParameterIncLarge:
      if (engine != nullptr)
      {
        const int delta = selectedItemId_ == "sample.root" ? 12 : 10;
        (void)engine->AdjustParameterValue(selectedItemId_, delta);
        hasContextSelection_ = true;
        selectionKind_ = ContextSelectionKind::Parameter;
      }
      break;
    case ContextActionId::ParameterMin:
      if (engine != nullptr)
      {
        SynthParameterInfo parameter;
        if (engine->GetParameterInfo(selectedItemId_, parameter))
        {
          (void)engine->SetParameterValue(selectedItemId_, parameter.minValue);
        }
      }
      break;
    case ContextActionId::ParameterMax:
      if (engine != nullptr)
      {
        SynthParameterInfo parameter;
        if (engine->GetParameterInfo(selectedItemId_, parameter))
        {
          (void)engine->SetParameterValue(selectedItemId_, parameter.maxValue);
        }
      }
      break;
    case ContextActionId::ParameterReset:
      if (engine != nullptr)
      {
        (void)engine->SetParameterValue(selectedItemId_, selectedParameterEntryValue_);
      }
      break;
    case ContextActionId::JunoChorusOff:
      if (engine != nullptr)
      {
        (void)engine->SetParameterValue("chorus.mode", 0);
        SelectStatus("Chorus", DescribeParameterValueOrFallback(engine, "chorus.mode"), "chorus.mode");
      }
      break;
    case ContextActionId::JunoChorusI:
      if (engine != nullptr)
      {
        (void)engine->SetParameterValue("chorus.mode", 1);
        SelectStatus("Chorus", DescribeParameterValueOrFallback(engine, "chorus.mode"), "chorus.mode");
      }
      break;
    case ContextActionId::JunoChorusII:
      if (engine != nullptr)
      {
        (void)engine->SetParameterValue("chorus.mode", 2);
        SelectStatus("Chorus", DescribeParameterValueOrFallback(engine, "chorus.mode"), "chorus.mode");
      }
      break;
    case ContextActionId::JunoRange16:
      if (engine != nullptr)
      {
        (void)engine->SetParameterValue("dco.range", 0);
        hasContextSelection_ = true;
        selectionKind_ = ContextSelectionKind::Parameter;
      }
      break;
    case ContextActionId::JunoRange8:
      if (engine != nullptr)
      {
        (void)engine->SetParameterValue("dco.range", 1);
        hasContextSelection_ = true;
        selectionKind_ = ContextSelectionKind::Parameter;
      }
      break;
    case ContextActionId::JunoRange4:
      if (engine != nullptr)
      {
        (void)engine->SetParameterValue("dco.range", 2);
        hasContextSelection_ = true;
        selectionKind_ = ContextSelectionKind::Parameter;
      }
      break;
    case ContextActionId::JunoSawOn:
      if (engine != nullptr)
      {
        (void)engine->SetParameterValue("dco.saw", 1);
        hasContextSelection_ = true;
        selectionKind_ = ContextSelectionKind::Parameter;
      }
      break;
    case ContextActionId::JunoSawOff:
      if (engine != nullptr)
      {
        (void)engine->SetParameterValue("dco.saw", 0);
        hasContextSelection_ = true;
        selectionKind_ = ContextSelectionKind::Parameter;
      }
      break;
    case ContextActionId::JunoHpf0:
      if (engine != nullptr)
      {
        (void)engine->SetParameterValue("hpf.mode", 0);
        SelectStatus("HPF", DescribeParameterValueOrFallback(engine, "hpf.mode"), "hpf.mode");
      }
      break;
    case ContextActionId::JunoHpf1:
      if (engine != nullptr)
      {
        (void)engine->SetParameterValue("hpf.mode", 1);
        SelectStatus("HPF", DescribeParameterValueOrFallback(engine, "hpf.mode"), "hpf.mode");
      }
      break;
    case ContextActionId::JunoHpf2:
      if (engine != nullptr)
      {
        (void)engine->SetParameterValue("hpf.mode", 2);
        SelectStatus("HPF", DescribeParameterValueOrFallback(engine, "hpf.mode"), "hpf.mode");
      }
      break;
    case ContextActionId::JunoHpf3:
      if (engine != nullptr)
      {
        (void)engine->SetParameterValue("hpf.mode", 3);
        SelectStatus("HPF", DescribeParameterValueOrFallback(engine, "hpf.mode"), "hpf.mode");
      }
      break;
    case ContextActionId::JunoHpfReset:
      if (engine != nullptr)
      {
        SynthParameterInfo parameter;
        const int defaultValue = engine->GetParameterInfo("hpf.mode", parameter) ? parameter.defaultValue : 0;
        (void)engine->SetParameterValue("hpf.mode", defaultValue);
        SelectStatus("HPF", DescribeParameterValueOrFallback(engine, "hpf.mode"), "hpf.mode");
      }
      break;
    case ContextActionId::SamplerRecord:
      selectedParameterGroupId_ = "par.sampler.group.samp";
      currentPage_ = 0;
      samplerRecordConfirmMode_ = true;
      SelectStatus("Record", "READY", "par.sampler.group.samp");
      break;
    case ContextActionId::SamplerRecordStart:
      selectedParameterGroupId_ = "par.sampler.group.samp";
      currentPage_ = 0;
      QueueCommand(UiCommandType::SamplerStartRecord, tracks_[activeTrack_].sampler.hasDraft ? "replace" : "");
      SelectStatus("Record", "START", "par.sampler.group.samp");
      break;
    case ContextActionId::SamplerStop:
      QueueCommand(UiCommandType::SamplerStopRecord);
      selectedParameterGroupId_ = "par.sampler.group.trim";
      currentPage_ = 0;
      SelectStatus("Trim", "DRAFT", "par.sampler.group.trim");
      break;
    case ContextActionId::SamplerCancel:
      if (samplerRecordConfirmMode_)
      {
        ExitSamplerRecordConfirm();
      }
      else
      {
        QueueCommand(UiCommandType::SamplerCancelRecord);
      }
      SelectStatus("Record", "CANCEL", "par.sampler.group.samp");
      break;
    case ContextActionId::SamplerPlay:
      QueueCommand(UiCommandType::SamplerPreview);
      SelectStatus("Sample", "PLAY", "par.sampler.group.samp");
      break;
    case ContextActionId::SamplerEdit:
      if (!tracks_[activeTrack_].sampler.hasSample)
      {
        SelectStatus("Sample", "NO SAMPLE", samplerBrowserMode_ ? "par.sampler.browser" : "par.sampler.group.edit");
        break;
      }
      samplerEditFromBrowser_ = samplerBrowserMode_;
      if (samplerBrowserMode_)
      {
        ExitSamplerBrowser();
      }
      selectedParameterGroupId_ = "par.sampler.group.edit";
      currentPage_ = 0;
      ClearContextSelection();
      break;
    case ContextActionId::SamplerSelect:
      selectedParameterGroupId_ = "par.sampler.group.samp";
      currentPage_ = 0;
      EnterSamplerBrowser();
      break;
    case ContextActionId::SamplerName:
    case ContextActionId::SamplerTrimSet:
      if (!tracks_[activeTrack_].sampler.hasDraft)
      {
        SelectStatus("Sample",
                     "NO DRAFT",
                     selectedParameterGroupId_.empty() ? std::string{"par.sampler.group.samp"} : selectedParameterGroupId_);
        break;
      }
      samplerNameEntryMode_ = true;
      samplerNameText_ = tracks_[activeTrack_].sampler.draftName.empty() ? "sample" : tracks_[activeTrack_].sampler.draftName;
      QueueCommand(UiCommandType::SamplerEnterNaming);
      SelectStatus("Name",
                   samplerNameText_,
                   selectedParameterGroupId_.empty() ? std::string{"par.sampler.group.samp"} : selectedParameterGroupId_);
      break;
    case ContextActionId::SamplerNameSave:
      if (!samplerNameText_.empty())
      {
        const bool renaming = samplerRenameEntryMode_;
        if (renaming)
        {
          QueueCommand(UiCommandType::SamplerRenameFile, samplerNameText_, samplerFileTarget_);
          break;
        }
        else
        {
          QueueCommand(UiCommandType::SamplerCommitName, samplerNameText_);
        }
        ExitSamplerNameEntry();
        selectedParameterGroupId_ = renaming ? std::string{"par.sampler.group.edit"} : std::string{};
        currentPage_ = 0;
        SelectStatus("Sample", renaming ? "RENAME" : "SAVE", renaming ? std::string{"par.sampler.group.edit"} : std::string{"par.sampler.group.samp"});
      }
      break;
    case ContextActionId::SamplerNameBackspace:
      if (!samplerNameText_.empty())
      {
        samplerNameText_.pop_back();
      }
      SelectStatus("Name",
                   samplerNameText_.empty() ? "_" : samplerNameText_,
                   selectedParameterGroupId_.empty() ? std::string{"par.sampler.group.samp"} : selectedParameterGroupId_);
      break;
    case ContextActionId::SamplerNameCancel:
    {
      const bool cancellingDelete = tracks_[activeTrack_].sampler.deleteConfirm;
      const bool cancellingRename = samplerRenameEntryMode_;
      QueueCommand(UiCommandType::SamplerCancelOverlay);
      ExitSamplerNameEntry();
      if (cancellingDelete)
      {
        selectedParameterGroupId_ = "par.sampler.group.edit";
        currentPage_ = 0;
      }
      else if (cancellingRename)
      {
        selectedParameterGroupId_ = "par.sampler.group.edit";
        currentPage_ = 0;
      }
      SelectStatus("Sample",
                   "CANCEL",
                   selectedParameterGroupId_.empty() ? std::string{"par.sampler.group.samp"} : selectedParameterGroupId_);
      break;
    }
    case ContextActionId::SamplerConfirmOverwrite:
      QueueCommand(UiCommandType::SamplerConfirmOverwrite);
      selectedParameterGroupId_.clear();
      currentPage_ = 0;
      SelectStatus("Sample", "REPLACE", "par.sampler.group.samp");
      break;
    case ContextActionId::SamplerRename:
      if (samplerFileTarget_.empty())
      {
        SelectStatus("Sample", "NO SAMPLE", "par.sampler.group.edit");
        break;
      }
      samplerNameEntryMode_ = true;
      samplerRenameEntryMode_ = true;
      samplerFileStatus_.clear();
      samplerNameText_ = samplerFileTarget_.substr(0, samplerFileTarget_.find_last_of('.'));
      SelectStatus("Rename", samplerNameText_, "par.sampler.group.edit");
      break;
    case ContextActionId::SamplerDelete:
    {
      QueueCommand(UiCommandType::SamplerDeleteFile, samplerFileTarget_);
      SelectStatus("Sample", "DELETE", "par.sampler.group.edit");
      break;
    }
    case ContextActionId::SamplerTrimLeft:
      MoveSamplerTrimCursor(-4);
      break;
    case ContextActionId::SamplerTrimRight:
      MoveSamplerTrimCursor(4);
      break;
    case ContextActionId::SamplerTrimBegin:
      SetSamplerTrimBoundary(engine, true);
      break;
    case ContextActionId::SamplerTrimEnd:
      SetSamplerTrimBoundary(engine, false);
      break;
    case ContextActionId::SamplerTrimPreview:
      QueueCommand(UiCommandType::SamplerPreview);
      SelectStatus("Trim", "PLAY", "par.sampler.group.trim");
      break;
    case ContextActionId::Back:
      if (IsContextRoot())
      {
        ExitToHome();
        break;
      }
      if (activeModifier_ == UiModifier::Other)
      {
        projectStatus_.clear();
        if (sequencerDeleteConfirm_)
        {
          CloseSequencerPatternDelete();
        }
        else if (projectConfirmCommand_ != UiCommandType::None)
        {
          if (projectConfirmCommand_ == UiCommandType::ProjectOverwrite)
          {
            projectNameEntryMode_ = true;
            projectNameText_ = projectConfirmName_;
          }
          projectConfirmCommand_ = UiCommandType::None;
        }
        else if (projectNameEntryMode_)
        {
          projectNameEntryMode_ = false;
          selectedParameterGroupId_ = "sys.project";
        }
        else if (selectedParameterGroupId_ == "sys.load")
        {
          selectedParameterGroupId_ = "sys.project";
        }
        else if (selectedParameterGroupId_.rfind("sys.seq.", 0) == 0)
        {
          selectedParameterGroupId_ = "sys.seq";
        }
        else if (!selectedParameterGroupId_.empty())
        {
          selectedParameterGroupId_.clear();
        }
        else
        {
          ExitToHome();
        }
        ClearContextSelection();
        break;
      }
      if (samplerRecordConfirmMode_)
      {
        ExitSamplerRecordConfirm();
        selectedParameterGroupId_.clear();
        currentPage_ = 0;
        ClearContextSelection();
        break;
      }

      if (activeModifier_ == UiModifier::Parameter && tracks_[activeTrack_].sampler.recording &&
          selectedParameterGroupId_ == "par.sampler.group.samp" && !samplerBrowserMode_)
      {
        break;
      }

      if (samplerNameEntryMode_ || tracks_[activeTrack_].sampler.overwritePending || tracks_[activeTrack_].sampler.deleteConfirm)
      {
        samplerFileStatus_.clear();
        const bool cancellingDelete = tracks_[activeTrack_].sampler.deleteConfirm;
        const bool cancellingRename = samplerRenameEntryMode_;
        QueueCommand(UiCommandType::SamplerCancelOverlay);
        ExitSamplerNameEntry();
        if (cancellingDelete)
        {
          selectedParameterGroupId_ = "par.sampler.group.edit";
          currentPage_ = 0;
        }
        else if (cancellingRename)
        {
          selectedParameterGroupId_ = "par.sampler.group.edit";
          currentPage_ = 0;
        }
        ClearContextSelection();
        break;
      }

      if (samplerBrowserMode_)
      {
        if (!samplerBrowserSearchText_.empty())
        {
          samplerBrowserSearchText_.clear();
          samplerBrowserSelectionIndex_ = 0;
          RefreshSamplerBrowserEntries();
          break;
        }

        ExitSamplerBrowser();
        selectedParameterGroupId_.clear();
        currentPage_ = 0;
        ClearContextSelection();
        break;
      }

      if (presetBrowserMode_)
      {
        presetBrowserMode_ = false;
        presetSearchText_.clear();
        presetBrowserSelectionIndex_ = 0;
        heldContextMenuIndex_ = -1;
        ClearContextSelection();
        break;
      }

      if (presetNameEntryMode_)
      {
        ExitPresetNameEntry();
        ClearContextSelection();
        break;
      }

      if (enginePickerMode_)
      {
        enginePickerMode_ = false;
        enginePickerPreviewKind_ = SynthEngineKind::None;
        hasPendingEngineKind_ = false;
        pendingEngineKind_ = SynthEngineKind::None;
        ClearContextSelection();
        break;
      }

      if (activeModifier_ == UiModifier::Parameter && parameterAdjustMode_)
      {
        if (IsJunoDirectAdjustGroupId(selectedParameterGroupId_) || IsSamplerDirectAdjustGroupId(selectedParameterGroupId_))
        {
          selectedParameterGroupId_.clear();
        }
        parameterAdjustMode_ = false;
        selectionKind_ = ContextSelectionKind::None;
        hasContextSelection_ = false;
        selectedItemId_.clear();
        selectedItemLabel_.clear();
        selectedValueText_.clear();
        selectedParameterEntryValue_ = 0;
        hasMenuHighlight_ = false;
      }
      else
      {
        if (activeModifier_ == UiModifier::Parameter && selectedParameterGroupId_ == "par.sampler.group.edit")
        {
          if (samplerEditFromBrowser_)
          {
            selectedParameterGroupId_ = "par.sampler.group.samp";
            currentPage_ = 0;
            samplerBrowserMode_ = true;
            samplerEditFromBrowser_ = false;
            samplerFileStatus_.clear();
            samplerBrowserAllEntries_ = CollectSamplerBrowserEntries();
            RefreshSamplerBrowserEntries();
            ClearContextSelection();
            break;
          }

          selectedParameterGroupId_.clear();
          currentPage_ = 0;
          ClearContextSelection();
          break;
        }

        if (activeModifier_ == UiModifier::Parameter && !selectedParameterGroupId_.empty())
        {
          selectedParameterGroupId_.clear();
        }
        if (activeModifier_ == UiModifier::Layout && !selectedParameterGroupId_.empty())
        {
          selectedParameterGroupId_.clear();
        }
        if (activeModifier_ == UiModifier::Other && !selectedParameterGroupId_.empty())
        {
          selectedParameterGroupId_.clear();
        }
        currentPage_ = 0;
        ClearContextSelection();
      }
      break;
    case ContextActionId::SavePreset:
      if (engine != nullptr)
      {
        if (!engine->SavePreset(engine->CurrentPresetId(), engine->CurrentPresetName()))
        {
          SelectStatus("Save Preset", "SAVE ERR");
          break;
        }
        tracks_[activeTrack_].patchName = engine->CurrentPresetName();
        SelectStatus("Save Preset", tracks_[activeTrack_].patchName);
      }
      break;
    case ContextActionId::SavePresetAs:
      presetNameEntryMode_ = true;
      presetNameText_ = tracks_[activeTrack_].patchName.empty() ? "preset" : tracks_[activeTrack_].patchName;
      SelectStatus("Name", presetNameText_, "snd.preset");
      break;
    case ContextActionId::LayoutChromatic:
      SelectStatus("Layout", "CHROMATIC");
      break;
    case ContextActionId::LayoutScale:
      SelectStatus("Layout", "SCALE");
      break;
    case ContextActionId::KeyboardRootPrev:
      AdjustKeyboardRoot(-1);
      break;
    case ContextActionId::KeyboardRootNext:
      AdjustKeyboardRoot(1);
      break;
    case ContextActionId::KeyboardScalePrev:
      AdjustKeyboardScale(-1);
      break;
    case ContextActionId::KeyboardScaleNext:
      AdjustKeyboardScale(1);
      break;
    case ContextActionId::KeyboardModePiano:
      SetKeyboardLayoutMode(KeyboardLayoutMode::Piano);
      break;
    case ContextActionId::KeyboardModeOctave:
      SetKeyboardLayoutMode(KeyboardLayoutMode::Octave);
      break;
    case ContextActionId::KeyboardModeChromatic:
      SetKeyboardLayoutMode(KeyboardLayoutMode::Chromatic);
      break;
    case ContextActionId::LayoutArp:
      SelectStatus("Arp", "ON");
      break;
    case ContextActionId::LayoutLatch:
      SelectStatus("Latch", "ON");
      break;
    case ContextActionId::LayoutOctaveDown:
      SelectStatus("Octave", "-");
      break;
    case ContextActionId::LayoutOctaveUp:
      SelectStatus("Octave", "+");
      break;
    case ContextActionId::LayoutGrid:
      SelectStatus("Grid", "4x14");
      break;
    case ContextActionId::LayoutSave:
      SelectStatus("Layout", "SAVED");
      break;
    case ContextActionId::ShowInfo:
      SelectStatus("CardenzaSynth", Device::BackendName());
      break;
    case ContextActionId::AudioSetup:
      SelectStatus("Audio", "ROUTING");
      break;
    case ContextActionId::MidiSetup:
      SelectStatus("MIDI", "LOCAL");
      break;
    case ContextActionId::ProjectSave:
      projectStatus_.clear();
      if (!projectName_.empty())
      {
        QueueCommand(UiCommandType::ProjectSave, projectName_);
        break;
      }
      [[fallthrough]];
    case ContextActionId::ProjectSaveAs:
      projectNameEntryMode_ = true;
      projectNameText_ = projectName_.empty() ? "Project" : projectName_;
      projectStatus_.clear();
      break;
    case ContextActionId::ProjectNameSave:
      if (!projectNameText_.empty())
      {
        QueueCommand(UiCommandType::ProjectSave, projectNameText_);
        projectStatus_ = "Saving...";
      }
      break;
    case ContextActionId::ProjectNameBackspace:
      if (!projectNameText_.empty()) projectNameText_.pop_back();
      projectStatus_.clear();
      break;
    case ContextActionId::ProjectLoad:
      if (selectedParameterGroupId_ != "sys.load")
      {
        selectedParameterGroupId_ = "sys.load";
        projectSelectionIndex_ = 0;
        projectStatus_.clear();
        QueueCommand(UiCommandType::ProjectList);
      }
      else if (!projectNames_.empty())
      {
        projectConfirmCommand_ = UiCommandType::ProjectLoad;
        projectConfirmName_ = projectNames_[projectSelectionIndex_];
      }
      break;
    case ContextActionId::ProjectNew:
      projectConfirmCommand_ = UiCommandType::ProjectNew;
      projectConfirmName_.clear();
      break;
    case ContextActionId::ProjectConfirm:
      if (projectConfirmCommand_ != UiCommandType::None)
      {
        QueueCommand(projectConfirmCommand_, projectConfirmName_);
        projectConfirmCommand_ = UiCommandType::None;
        projectStatus_ = "Working...";
      }
      break;
    case ContextActionId::SequencerTogglePlayback:
      QueueCommand(UiCommandType::SequencerTogglePlayback);
      break;
    case ContextActionId::SequencerToggleView:
      QueueCommand(UiCommandType::SequencerToggleView);
      break;
    case ContextActionId::SequencerStop:
      QueueCommand(UiCommandType::SequencerStop);
      break;
    case ContextActionId::SequencerResume:
      QueueCommand(UiCommandType::SequencerResume);
      break;
    case ContextActionId::SequencerTempoDec:
      QueueCommand(UiCommandType::SequencerTempoDec);
      break;
    case ContextActionId::SequencerTempoInc:
      QueueCommand(UiCommandType::SequencerTempoInc);
      break;
    case ContextActionId::SequencerTempoDecLarge:
      QueueCommand(UiCommandType::SequencerTempoDecLarge);
      break;
    case ContextActionId::SequencerTempoIncLarge:
      QueueCommand(UiCommandType::SequencerTempoIncLarge);
      break;
    case ContextActionId::SequencerMute:
      QueueCommand(UiCommandType::SequencerMute);
      break;
    case ContextActionId::SequencerEnable:
      QueueCommand(UiCommandType::SequencerEnable);
      break;
    case ContextActionId::SequencerTrackPrev:
      QueueCommand(UiCommandType::SequencerTrackPrev);
      break;
    case ContextActionId::SequencerTrackNext:
      QueueCommand(UiCommandType::SequencerTrackNext);
      break;
    case ContextActionId::SequencerPatternPrev:
      QueueCommand(UiCommandType::SequencerPatternPrev);
      break;
    case ContextActionId::SequencerPatternNext:
      QueueCommand(UiCommandType::SequencerPatternNext);
      break;
    case ContextActionId::SequencerPatternCountDec:
      if (sequencer_.patternCount > 1)
      {
        if (sequencer_.patternHasEvents[sequencer_.patternCount - 1])
          ConfirmSequencerPatternDelete(sequencer_.patternCount - 1, false);
        else QueueCommand(UiCommandType::SequencerPatternCountDec);
      }
      break;
    case ContextActionId::SequencerPatternDeleteConfirm:
      if (sequencerDeleteConfirm_)
      {
        UiCommand command;
        command.type = UiCommandType::SequencerPatternDelete;
        command.trackIndex = sequencerDeleteTrack_;
        command.patternIndex = sequencerDeletePattern_;
        pendingCommands_.push_back(command);
        CloseSequencerPatternDelete();
      }
      break;
    case ContextActionId::SequencerPatternCountInc:
      QueueCommand(UiCommandType::SequencerPatternCountInc);
      break;
    case ContextActionId::ResetPatch:
      SelectStatus("Patch", "RESET");
      break;
    case ContextActionId::None:
    default:
      break;
  }

  hasMenuHighlight_ = false;
}

void SynthUiModel::ClearHeldModifiers()
{
  heldModifiers_.fill(false);
  modifierContextActivated_.fill(false);
  modifierPressStartedAt_.fill(std::chrono::steady_clock::time_point{});
}

std::size_t SynthUiModel::PageCountForModifier(const ISynthEngine* engine) const
{
  if (engine == nullptr)
  {
    return 1;
  }

  switch (activeModifier_)
  {
    case UiModifier::Sound:
      return 1;
    case UiModifier::Parameter:
      if (parameterAdjustMode_)
      {
        return 1;
      }
      if (IsDx7ParameterEngine(engine) && !selectedParameterGroupId_.empty())
      {
        return Dx7ParameterPageCount(engine, selectedParameterGroupId_);
      }
      if (IsJunoParameterEngine(engine) && !selectedParameterGroupId_.empty())
      {
        return JunoParameterPageCount(engine, selectedParameterGroupId_);
      }
      if (IsSamplerParameterEngine(engine))
      {
        const SamplerTrackUiState& sampler = tracks_[activeTrack_].sampler;
        if (samplerNameEntryMode_ || samplerRecordConfirmMode_ || sampler.recording || sampler.overwritePending || sampler.deleteConfirm)
        {
          return 1;
        }
        if (samplerBrowserMode_)
        {
          return 1;
        }
        if (selectedParameterGroupId_.empty())
        {
          return 1;
        }
        if (selectedParameterGroupId_ == "par.sampler.group.samp")
        {
          return 1;
        }
        if (selectedParameterGroupId_ == "par.sampler.group.trim")
        {
          return 1;
        }
        if (selectedParameterGroupId_ == "par.sampler.group.edit")
        {
          return 1;
        }
        const std::size_t singleColumnPagedItemCount = kContextMenuRows - 1;
        const std::size_t itemCount = CollectSamplerGroupParameters(engine, selectedParameterGroupId_).size();
        return std::max<std::size_t>(1, (itemCount + singleColumnPagedItemCount - 1) / singleColumnPagedItemCount);
      }
      if (IsDx7ParameterEngine(engine) || IsJunoParameterEngine(engine)) return 1;
      return std::max<std::size_t>(1, (engine->ListParameters({0, 0}).totalCount + kGenericParameterPageSize - 1) / kGenericParameterPageSize);
    case UiModifier::Layout:
      return 1;
    case UiModifier::Other:
      return 1;
    case UiModifier::None:
    default:
      return 1;
  }
}

ContextMenuGrid SynthUiModel::BuildSoundMenu(const ISynthEngine* engine) const
{
  (void)engine;

  if (presetBrowserMode_)
  {
    return ContextMenuGrid{
        MakeActionCell(presetSearchText_.empty() ? "BACK" : "DEL",
                       presetSearchText_.empty() ? ContextActionId::Back : ContextActionId::PresetDelete),
        MakeActionCell("PREV", ContextActionId::PresetPrev),
        MakeActionCell("NEXT", ContextActionId::PresetNext),
        MakeActionCell("LOAD", ContextActionId::PresetLoad),
        ContextMenuCell{},
        ContextMenuCell{},
        ContextMenuCell{},
        ContextMenuCell{},
    };
  }

  if (presetNameEntryMode_)
  {
    return ContextMenuGrid{
        MakeActionCell("DEL", ContextActionId::PresetNameBackspace),
        ContextMenuCell{},
        MakeActionCell("BACK", ContextActionId::Back),
        MakeActionCell("SAVE", ContextActionId::PresetNameSave),
        ContextMenuCell{},
        ContextMenuCell{},
        ContextMenuCell{},
        ContextMenuCell{},
    };
  }

  if (enginePickerMode_)
  {
    return ContextMenuGrid{
        MakeActionCell("RESET", ContextActionId::EngineReset),
        MakeActionCell("PREV", ContextActionId::EnginePrev),
        MakeActionCell("NEXT", ContextActionId::EngineNext),
        MakeActionCell("LOAD", ContextActionId::EngineLoad),
        ContextMenuCell{},
        ContextMenuCell{},
        ContextMenuCell{},
        ContextMenuCell{},
    };
  }

  return ContextMenuGrid{
      MakeItemCell("ENGINE", "snd.engine"),
      MakeItemCell("PRESET", "snd.preset"),
      MakeItemCell("SAVE", "snd.save"),
      MakeItemCell("SAVEAS", "snd.saveas"),
      ContextMenuCell{},
      ContextMenuCell{},
      ContextMenuCell{},
      ContextMenuCell{},
  };
}

ContextMenuGrid SynthUiModel::BuildParameterBrowseMenu(const ISynthEngine* engine) const
{
  if (IsDx7ParameterEngine(engine))
  {
    if (selectedParameterGroupId_.empty())
    {
      return ContextMenuGrid{
          MakeItemCell("OP1", "par.dx7.group.op1"),
          MakeItemCell("OP2", "par.dx7.group.op2"),
          MakeItemCell("OP3", "par.dx7.group.op3"),
          MakeItemCell("OP4", "par.dx7.group.op4"),
          MakeItemCell("OP5", "par.dx7.group.op5"),
          MakeItemCell("OP6", "par.dx7.group.op6"),
          MakeItemCell("ALG", "par.dx7.group.alg"),
          MakeItemCell("GLB", "par.dx7.group.glb"),
      };
    }

    ContextMenuGrid menu = BlankGrid();
    const auto parameters = CollectDx7GroupParameters(engine, selectedParameterGroupId_);
    const std::size_t pageCount = Dx7ParameterPageCount(engine, selectedParameterGroupId_);
    const std::size_t pageStart = currentPage_ * kDx7ParameterMenuItemCount;
    const std::size_t pageEnd = std::min(parameters.size(), pageStart + kDx7ParameterMenuItemCount);
    std::size_t menuIndex = 0;
    for (std::size_t parameterIndex = pageStart; parameterIndex < pageEnd && menuIndex < kDx7ParameterMenuItemCount;
         ++parameterIndex, ++menuIndex)
    {
      ContextMenuCell cell = MakeItemCell(Dx7ParameterMenuLabel(parameters[parameterIndex]), parameters[parameterIndex].id);
      cell.fullName = Dx7ParameterHelpTitle(parameters[parameterIndex]);
      cell.description = Dx7ParameterHelpDescription(parameters[parameterIndex]);
      menu[menuIndex] = std::move(cell);
    }
    menu[6] = MakeActionCell("BACK", ContextActionId::Back);
    if (pageCount > 1)
    {
      menu[7] = MakeNextPageCell(std::to_string(currentPage_ + 1) + "/" + std::to_string(pageCount));
    }
    else
    {
      menu[7] = ContextMenuCell{};
    }
    return menu;
  }

  if (IsJunoParameterEngine(engine))
  {
    if (selectedParameterGroupId_.empty())
    {
      return ContextMenuGrid{
          MakeItemCell("LFO", "par.juno.group.lfo"),
          MakeItemCell("DCO", "par.juno.group.dco"),
          ContextMenuCell{},
          MakeItemCell("VCF", "par.juno.group.vcf"),
          MakeItemCell("VCA", "par.juno.group.amp"),
          MakeItemCell("ENV", "par.juno.group.env"),
          ContextMenuCell{},
          ContextMenuCell{},
      };
    }

    if (selectedParameterGroupId_ == "par.juno.group.hpf")
    {
      return ContextMenuGrid{
          MakeActionCell("0", ContextActionId::JunoHpf0),
          MakeActionCell("1", ContextActionId::JunoHpf1),
          MakeActionCell("2", ContextActionId::JunoHpf2),
          MakeActionCell("3", ContextActionId::JunoHpf3),
          ContextMenuCell{},
          ContextMenuCell{},
          MakeActionCell("RST", ContextActionId::JunoHpfReset),
          MakeActionCell("DONE", ContextActionId::Back),
      };
    }

    if (selectedParameterGroupId_ == "par.juno.group.fx")
    {
      return ContextMenuGrid{
          MakeActionCell("OFF", ContextActionId::JunoChorusOff),
          MakeActionCell("I", ContextActionId::JunoChorusI),
          MakeActionCell("II", ContextActionId::JunoChorusII),
          MakeActionCell("DONE", ContextActionId::Back),
          ContextMenuCell{},
          ContextMenuCell{},
          ContextMenuCell{},
          ContextMenuCell{},
      };
    }

    ContextMenuGrid menu = BlankGrid();
    const auto parameters = CollectJunoGroupParameters(engine, selectedParameterGroupId_);
    const std::size_t pageCount = JunoParameterPageCount(engine, selectedParameterGroupId_);
        const std::size_t pageStart = currentPage_ * kPagedMenuItemCount;
        const std::size_t pageEnd = std::min(parameters.size(), pageStart + kPagedMenuItemCount);
    std::size_t menuIndex = 0;
        for (std::size_t parameterIndex = pageStart; parameterIndex < pageEnd && menuIndex < kPagedMenuItemCount;
         ++parameterIndex, ++menuIndex)
    {
      menu[menuIndex] = MakeItemCell(JunoParameterMenuLabel(parameters[parameterIndex]), parameters[parameterIndex].id);
    }

    if (pageCount > 1)
    {
      menu[7] = MakeNextPageCell(std::to_string(currentPage_ + 1) + "/" + std::to_string(pageCount));
    }
    else
    {
      menu[7] = MakeActionCell("DONE", ContextActionId::Back);
    }
    return menu;
  }

  if (IsSamplerParameterEngine(engine))
  {
    const SamplerTrackUiState& sampler = tracks_[activeTrack_].sampler;
    if (samplerNameEntryMode_)
    {
      return ContextMenuGrid{
          MakeActionCell("DEL", ContextActionId::SamplerNameBackspace),
          ContextMenuCell{},
          MakeActionCell("BACK", ContextActionId::Back),
          MakeActionCell("SAVE", ContextActionId::SamplerNameSave),
          ContextMenuCell{},
          ContextMenuCell{},
          ContextMenuCell{},
          ContextMenuCell{},
      };
    }

    if (sampler.overwritePending)
    {
      return ContextMenuGrid{
          ContextMenuCell{},
          ContextMenuCell{},
          MakeActionCell("BACK", ContextActionId::Back),
          MakeActionCell("RPLC", ContextActionId::SamplerConfirmOverwrite),
          ContextMenuCell{},
          ContextMenuCell{},
          ContextMenuCell{},
          ContextMenuCell{},
      };
    }

    if (sampler.deleteConfirm)
    {
      return ContextMenuGrid{
          ContextMenuCell{},
          ContextMenuCell{},
          MakeActionCell("BACK", ContextActionId::Back),
          MakeActionCell("YES", ContextActionId::SamplerDelete),
          ContextMenuCell{},
          ContextMenuCell{},
          ContextMenuCell{},
          ContextMenuCell{},
      };
    }

    if (selectedParameterGroupId_.empty())
    {
      if (!sampler.hasSample && !sampler.hasDraft)
      {
        return BuildMenuAt({
            {0, MakeActionCell("FILE", ContextActionId::SamplerSelect)},
            {3, MakeActionCell("REC", ContextActionId::SamplerRecord)},
        });
      }

      return ContextMenuGrid{
          MakeActionCell("FILE", ContextActionId::SamplerSelect),
          MakeActionCell("REC", ContextActionId::SamplerRecord),
          MakeItemCell("TRIM", "par.sampler.group.trim"),
          MakeItemCell("ROOT", "par.sampler.group.root"),
          MakeItemCell("LOOP", "par.sampler.group.loop"),
          MakeItemCell("GAIN", "par.sampler.group.amp"),
          MakeItemCell("FLT", "par.sampler.group.filter"),
          MakeItemCell("ENV", "par.sampler.group.env"),
        };
    }

    if (selectedParameterGroupId_ == "par.sampler.group.samp")
    {
      if (samplerBrowserMode_)
      {
        const bool samplerBrowserHasSearch = !samplerBrowserSearchText_.empty();
        return BuildMenuAt({
          {0, MakeActionCell(samplerBrowserHasSearch ? "DEL" : "BACK",
                             samplerBrowserHasSearch ? ContextActionId::PresetDelete : ContextActionId::Back)},
          {1, MakeActionCell("PREV", ContextActionId::PresetPrev)},
          {2, MakeActionCell("NEXT", ContextActionId::PresetNext)},
          {3, MakeActionCell("LOAD", ContextActionId::PresetLoad)},
        });
      }

      if (samplerRecordConfirmMode_)
      {
        return BuildMenuAt({
            {2, MakeActionCell("BACK", ContextActionId::Back)},
            {3, MakeActionCell("START", ContextActionId::SamplerRecordStart)},
        });
      }

      if (sampler.recording)
      {
        return BuildMenuAt({
            {3, MakeActionCell("REC", ContextActionId::SamplerStop)},
        });
      }

      return sampler.hasDraft
          ? BuildMenuAt({
              {0, MakeActionCell("REC", ContextActionId::SamplerRecord)},
              {2, MakeActionCell("BACK", ContextActionId::Back)},
              {3, MakeActionCell("SAVE", ContextActionId::SamplerName)},
            })
          : BuildMenuAt({
              {0, MakeActionCell("BACK", ContextActionId::Back)},
            });
    }

    if (selectedParameterGroupId_ == "par.sampler.group.edit")
    {
      auto menu = BuildMenuAt({
          {0, MakeActionCell("REN", ContextActionId::SamplerRename)},
          {1, MakeActionCell("DEL", ContextActionId::SamplerDelete)},
          {3, MakeActionCell("BACK", ContextActionId::Back)},
      });
      menu[0].disabled = menu[1].disabled = samplerFileTarget_.empty();
      return menu;
    }

    if (selectedParameterGroupId_ == "par.sampler.group.trim")
    {
      return BuildMenuAt({
          {0, MakeActionCell("LEFT", ContextActionId::SamplerTrimLeft)},
          {1, MakeActionCell("RGHT", ContextActionId::SamplerTrimRight)},
          {2, MakeActionCell("BEG", ContextActionId::SamplerTrimBegin)},
          {3, MakeActionCell("END", ContextActionId::SamplerTrimEnd)},
          {4, MakeActionCell("PLAY", ContextActionId::SamplerTrimPreview)},
          {7, MakeActionCell("DONE", ContextActionId::Back)},
      });
    }

    ContextMenuGrid menu = BlankGrid();
    const auto parameters = CollectSamplerGroupParameters(engine, selectedParameterGroupId_);
    const std::size_t singleColumnPagedItemCount = kContextMenuRows - 1;
    const std::size_t pageCount = std::max<std::size_t>(1, (parameters.size() + singleColumnPagedItemCount - 1) / singleColumnPagedItemCount);
    const std::size_t pageStart = currentPage_ * singleColumnPagedItemCount;
    const std::size_t pageEnd = std::min(parameters.size(), pageStart + singleColumnPagedItemCount);
    std::size_t menuIndex = 0;
    for (std::size_t parameterIndex = pageStart; parameterIndex < pageEnd && menuIndex < singleColumnPagedItemCount;
         ++parameterIndex, ++menuIndex)
    {
      menu[menuIndex] = MakeItemCell(SamplerParameterMenuLabel(parameters[parameterIndex]), parameters[parameterIndex].id);
    }

    menu[3] = pageCount > 1 ? MakeNextPageCell(std::to_string(currentPage_ + 1) + "/" + std::to_string(pageCount))
                            : MakeActionCell("BACK", ContextActionId::Back);
    return menu;
  }

  auto menu = BlankGrid();
  if (engine == nullptr) return menu;
  const auto page = engine->ListParameters({currentPage_ * kGenericParameterPageSize, kGenericParameterPageSize});
  for (std::size_t i = 0; i < page.items.size() && i < kGenericParameterPageSize; ++i)
  {
    const auto& parameter = page.items[i];
    std::string label = parameter.label;
    if (parameter.id == "closed_hat.level") label = "C HAT";
    if (parameter.id == "open_hat.level") label = "O HAT";
    if (parameter.id == "cowbell.level") label = "BELL";
    if (parameter.id == "maraca.level") label = "MARA";
    if (parameter.id == "master.level") label = "MSTR";
    menu[i] = MakeItemCell(label, parameter.id);
    menu[i].fullName = parameter.label;
    menu[i].description = parameter.id == "master.level" ? "Overall track volume" : "Level for subsequent drum hits";
  }
  const auto pages = std::max<std::size_t>(1, (page.totalCount + kGenericParameterPageSize - 1) / kGenericParameterPageSize);
  menu[pages > 1 ? 6 : 7] = MakeActionCell("BACK", ContextActionId::Back);
  if (pages > 1) menu[7] = MakeNextPageCell(std::to_string(currentPage_ + 1) + "/" + std::to_string(pages));
  return menu;
}

ContextMenuGrid SynthUiModel::BuildParameterAdjustMenu() const
{
  if (selectedItemId_ == "dco.range")
  {
    return ContextMenuGrid{
        MakeActionCell("16'", ContextActionId::JunoRange16),
        MakeActionCell("8'", ContextActionId::JunoRange8),
        MakeActionCell("4'", ContextActionId::JunoRange4),
        MakeActionCell("DONE", ContextActionId::Back),
        ContextMenuCell{},
        ContextMenuCell{},
        ContextMenuCell{},
        ContextMenuCell{},
    };
  }

  if (selectedItemId_ == "dco.saw")
  {
    return ContextMenuGrid{
        MakeActionCell("ON", ContextActionId::JunoSawOn),
        MakeActionCell("OFF", ContextActionId::JunoSawOff),
        ContextMenuCell{},
        MakeActionCell("DONE", ContextActionId::Back),
        ContextMenuCell{},
        ContextMenuCell{},
        ContextMenuCell{},
        ContextMenuCell{},
    };
  }

  if (selectedItemId_ == "sample.loop")
  {
    return ContextMenuGrid{
        MakeActionCell("ON", ContextActionId::ParameterMax),
        MakeActionCell("OFF", ContextActionId::ParameterMin),
        MakeActionCell("RST", ContextActionId::ParameterReset),
        MakeActionCell("DONE", ContextActionId::Back),
        ContextMenuCell{},
        ContextMenuCell{},
        ContextMenuCell{},
        ContextMenuCell{},
    };
  }

  if (selectedItemId_ == "sample.root")
  {
    return ContextMenuGrid{
        MakeActionCell("-1", ContextActionId::ParameterDecSmall),
        MakeActionCell("+1", ContextActionId::ParameterIncSmall),
        MakeActionCell("OCT-", ContextActionId::ParameterDecLarge),
        MakeActionCell("OCT+", ContextActionId::ParameterIncLarge),
        MakeActionCell("MIN", ContextActionId::ParameterMin),
        MakeActionCell("MAX", ContextActionId::ParameterMax),
        MakeActionCell("RST", ContextActionId::ParameterReset),
        MakeActionCell("DONE", ContextActionId::Back),
    };
  }

  return ContextMenuGrid{
      MakeActionCell("-1", ContextActionId::ParameterDecSmall),
      MakeActionCell("+1", ContextActionId::ParameterIncSmall),
      MakeActionCell("-10", ContextActionId::ParameterDecLarge),
      MakeActionCell("+10", ContextActionId::ParameterIncLarge),
      MakeActionCell("MIN", ContextActionId::ParameterMin),
      MakeActionCell("MAX", ContextActionId::ParameterMax),
      MakeActionCell("RST", ContextActionId::ParameterReset),
      MakeActionCell("DONE", ContextActionId::Back),
  };
}

ContextMenuGrid SynthUiModel::BuildLayoutMenu() const
{
  if (tracks_[activeTrack_].kind == SynthEngineKind::Drums)
    return BuildMenuAt({{3, MakeActionCell("BACK", ContextActionId::Back)}});
  if (selectedParameterGroupId_ == "lay.root")
  {
    return BuildMenuAt({
        {0, MakeActionCell("PREV", ContextActionId::KeyboardRootPrev)},
        {1, MakeActionCell("NEXT", ContextActionId::KeyboardRootNext)},
        {3, MakeActionCell("DONE", ContextActionId::Back)},
    });
  }

  if (selectedParameterGroupId_ == "lay.scl")
  {
    return BuildMenuAt({
        {0, MakeActionCell("PREV", ContextActionId::KeyboardScalePrev)},
        {1, MakeActionCell("NEXT", ContextActionId::KeyboardScaleNext)},
        {3, MakeActionCell("DONE", ContextActionId::Back)},
    });
  }

  if (selectedParameterGroupId_ == "lay.mode")
  {
    return BuildMenuAt({
        {0, MakeActionCell("PIANO", ContextActionId::KeyboardModePiano)},
        {1, MakeActionCell("OCT", ContextActionId::KeyboardModeOctave)},
        {2, MakeActionCell("CHRM", ContextActionId::KeyboardModeChromatic)},
        {3, MakeActionCell("DONE", ContextActionId::Back)},
    });
  }

  if (selectedParameterGroupId_ == "lay.fx")
  {
    return BuildMenuAt({
        {3, MakeActionCell("DONE", ContextActionId::Back)},
    });
  }

  ContextMenuCell rootCell = MakeItemCell("ROOT", "lay.root");
  rootCell.disabled = keyboardLayoutMode_ == KeyboardLayoutMode::Piano;
  rootCell.description = rootCell.disabled ? "Disabled in Piano mode" : "Set the keyboard root note";
  ContextMenuCell scaleCell = MakeItemCell("SCALE", "lay.scl");
  scaleCell.disabled = keyboardLayoutMode_ == KeyboardLayoutMode::Piano;
  scaleCell.description = scaleCell.disabled ? "Disabled in Piano mode" : "Set the keyboard scale";
  return BuildMenuAt({
      {0, MakeItemCell("MODE", "lay.mode")},
      {1, scaleCell},
      {2, rootCell},
      {3, MakeItemCell("FX", "lay.fx")},
  });
}

ContextMenuGrid SynthUiModel::BuildOtherMenu() const
{
  if (sequencerDeleteConfirm_)
  {
    return BuildMenuAt({{2, MakeActionCell("BACK", ContextActionId::Back)},
                       {3, MakeActionCell("YES", ContextActionId::SequencerPatternDeleteConfirm)}});
  }
  if (projectConfirmCommand_ != UiCommandType::None)
  {
    return BuildMenuAt({{2, MakeActionCell("BACK", ContextActionId::Back)},
                        {3, MakeActionCell("YES", ContextActionId::ProjectConfirm)}});
  }
  if (projectNameEntryMode_)
  {
    auto save = MakeActionCell("SAVE", ContextActionId::ProjectNameSave);
    save.disabled = projectNameText_.empty();
    return BuildMenuAt({{0, MakeActionCell("DEL", ContextActionId::ProjectNameBackspace)},
                        {2, MakeActionCell("BACK", ContextActionId::Back)}, {3, save}});
  }
  if (selectedParameterGroupId_ == "sys.project")
  {
    return BuildMenuAt({
        {0, MakeActionCell("SAVE", ContextActionId::ProjectSave)},
        {1, MakeActionCell("SAVEAS", ContextActionId::ProjectSaveAs)},
        {2, MakeActionCell("LOAD", ContextActionId::ProjectLoad)},
        {3, MakeActionCell("NEW", ContextActionId::ProjectNew)},
        {7, MakeActionCell("BACK", ContextActionId::Back)}});
  }
  if (selectedParameterGroupId_ == "sys.load")
  {
    auto load = MakeActionCell("LOAD", ContextActionId::ProjectLoad);
    load.disabled = projectNames_.empty();
    return BuildMenuAt({
        {0, MakeActionCell("PREV", ContextActionId::PresetPrev)},
        {1, MakeActionCell("NEXT", ContextActionId::PresetNext)},
        {2, MakeActionCell("BACK", ContextActionId::Back)}, {3, load}});
  }
  if (selectedParameterGroupId_ == "sys.seq")
  {
    return BuildMenuAt({
        {0, MakeActionCell(sequencer_.playing ? "STOP" : "PLAY", ContextActionId::SequencerTogglePlayback)},
        {1, MakeActionCell("STOP", ContextActionId::SequencerStop)},
        {2, MakeActionCell("RES", ContextActionId::SequencerResume)},
        {3, MakeItemCell("BPM", "sys.seq.tempo")},
        {4, MakeItemCell("TRK", "sys.seq.track")},
        {5, MakeItemCell("PATT", "sys.seq.pattern")},
        {6, MakeActionCell(showSequencerGrid_ ? "KEYS" : "GRID", ContextActionId::SequencerToggleView)},
        {7, MakeActionCell("BACK", ContextActionId::Back)}});
  }
  if (selectedParameterGroupId_ == "sys.seq.tempo")
  {
    return BuildMenuAt({
        {0, MakeActionCell("-10", ContextActionId::SequencerTempoDecLarge)},
        {1, MakeActionCell("+10", ContextActionId::SequencerTempoIncLarge)},
        {2, MakeActionCell("-1", ContextActionId::SequencerTempoDec)},
        {3, MakeActionCell("+1", ContextActionId::SequencerTempoInc)},
        {7, MakeActionCell("BACK", ContextActionId::Back)}});
  }
  if (selectedParameterGroupId_ == "sys.seq.track")
  {
    return BuildMenuAt({
        {0, MakeActionCell("T-", ContextActionId::SequencerTrackPrev)},
        {1, MakeActionCell("T+", ContextActionId::SequencerTrackNext)},
        {2, MakeActionCell(sequencer_.trackMuted ? "UNM" : "MUTE", ContextActionId::SequencerMute)},
        {3, MakeActionCell(sequencer_.trackEnabled ? "OFF" : "ON", ContextActionId::SequencerEnable)},
        {7, MakeActionCell("BACK", ContextActionId::Back)}});
  }
  if (selectedParameterGroupId_ == "sys.seq.pattern")
  {
    auto menu = BuildMenuAt({
        {0, MakeActionCell("PREV", ContextActionId::SequencerPatternPrev)},
        {1, MakeActionCell("NEXT", ContextActionId::SequencerPatternNext)},
        {2, MakeActionCell("CNT-", ContextActionId::SequencerPatternCountDec)},
        {3, MakeActionCell("CNT+", ContextActionId::SequencerPatternCountInc)},
        {7, MakeActionCell("BACK", ContextActionId::Back)}});
    menu[2].disabled = sequencer_.patternCount <= 1;
    menu[3].disabled = sequencer_.patternCount >= kSequencerMaxPatternCount;
    return menu;
  }
  return BuildMenuAt({
      {0, MakeItemCell("PROJ", "sys.project")},
      {1, MakeItemCell("SEQ", "sys.seq")},
      {2, MakeActionCell("INFO", ContextActionId::ShowInfo)},
      {3, MakeActionCell("BACK", ContextActionId::Back)}});
}

UiModifier SynthUiModel::FirstHeldModifier() const
{
  for (std::size_t row = 0; row < heldModifiers_.size(); ++row)
  {
    if (heldModifiers_[row])
    {
      return ModifierForRow(row);
    }
  }
  return UiModifier::None;
}

}  // namespace synth
