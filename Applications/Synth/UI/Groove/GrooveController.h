#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "Applications/Synth/UI/Groove/GrooveHost.h"
#include "Applications/Synth/UI/Groove/GrooveTypes.h"
#include "Applications/Synth/UI/Groove/InstrumentPages.h"
#if defined(CARDENZA_NATIVE_UI)
#include <atomic>
#include "Applications/Synth/UI/Groove/Native/NativeAction.h"
#endif

namespace groove {

// UiState::padNote of a pad that is not down.
inline constexpr std::uint8_t kNoNote = 0xFF;
inline constexpr std::uint32_t kHoldMs = 250;
// A parameter's GRID cell held this long turns the pads into its values.
inline constexpr std::uint32_t kCellPickMs = 200;
// OK in SEQ: held this long it opens the RECORD panel instead of switching the view.
inline constexpr std::uint32_t kOkHoldMs = 450;
// Key LED states, in perceived brightness 0-15 (the device gamma-corrects them, Devices/LedLight.h):
// off, half, full. Half is the dimmest the keys can glow (1/15 of the time): clearly apart from full.
inline constexpr std::uint8_t kLedOff = 0;
inline constexpr std::uint8_t kLedHalf = 6;
inline constexpr std::uint8_t kLedFull = 15;
// The edge keys (left and right columns) light at most this much on the keys themselves.
inline constexpr std::uint8_t kLedEdgeMax = kLedHalf;
inline constexpr std::uint32_t kRepeatDelayMs = 400;
inline constexpr std::uint32_t kRepeatIntervalMs = 70;
inline constexpr std::uint32_t kRepeatAccelerateAfter = 12;
inline constexpr std::uint32_t kToastMs = 2000;
inline constexpr std::uint32_t kTransitionMs = 110;
inline constexpr std::uint32_t kHudMs = 1100;
// How long an FN menu tile stays lit after its key lets go.
inline constexpr std::uint32_t kMenuPressMs = 350;

enum class ToastKind : std::uint8_t { Info, Ok, Error };

struct Toast {
  std::string text;
  ToastKind kind = ToastKind::Info;
  std::uint32_t until = 0;
};

enum class ModalKind : std::uint8_t {
  None,
  TextEntry,
  Confirm,
  PresetBrowser,
  EnginePicker,
  SampleBrowser,
  SampleOptions,
  SampleRecord,
  Menu,    // a tap on FN: the menu of the screen underneath, kept open
  Record,  // SEQ: hold OK. Live recording, the note view and the captured takes
};

// SEQ note page views, for melodic and drum tracks alike: ROLL draws a piano roll, STEP a machine
// (the drum machine's look: a row per drum or note over the page's steps, beside the GRID). Melodic
// tracks start on ROLL, drum tracks on STEP. Both take the same keys (Controller::StepKeyPress): a
// step key selects its step while it is down, a tap keeps it selected, pads held and a step tapped
// put their notes in.
enum class SeqView : std::uint8_t { Roll, Step };

// Capture: everything the pads play is kept, without recording, as takes split by pauses. A take
// can be put into a pattern afterwards (RECORD panel).
inline constexpr std::size_t kCaptureTakes = 8;
// The notes of all takes together: one long take can have them all, and the oldest takes make room.
inline constexpr std::size_t kCaptureNotes = 400;
inline constexpr std::uint32_t kCaptureGapMs = 2500;
inline constexpr std::uint32_t kCaptureMaxMs = 60000;

struct CaptureNote {
  std::uint8_t note = 0;
  std::uint8_t velocity = 0;
  std::uint16_t startMs = 0;      // from the take's start
  std::uint16_t lengthMs = 0;     // 0 while still held
  std::uint16_t startPulse = 0;   // synced takes: sequencer pulses from the take's first bar line
  std::uint16_t lengthPulses = 0;
};

struct CaptureTake {
  std::uint32_t first = 0;        // where its notes begin in RecordState::notes (keeps counting up)
  std::uint16_t count = 0;
  std::uint8_t track = 0;
  bool synced = false;            // played with the transport running: timed by the sequencer
  std::uint16_t bpm = 120;        // tempo when played (free takes are timed with it)
  std::uint32_t startedAt = 0;    // ms
  std::uint32_t endedAt = 0;      // ms of the last note-off
  std::uint64_t basePulse = 0;    // synced takes: the bar line before the first note
  std::uint32_t id = 0;           // take number, counting up; 0 = empty
};

// The RECORD panel's GRID: settings on row 0 (the fourth cell is empty), takes on rows 1 and 2
// (newest first).
inline constexpr std::size_t kRecordRec = 0;
inline constexpr std::size_t kRecordSnap = 1;
inline constexpr std::size_t kRecordView = 2;
inline constexpr std::size_t kRecordFirstTake = 4;

enum class TextPurpose : std::uint8_t {
  ProjectSaveAs,
  PresetSaveAs,
  SampleRename,
  Search,  // the filter of the preset / sample browser underneath (FN + F)
};

enum class ConfirmPurpose : std::uint8_t {
  NewProject,
  LoadProject,
  OverwriteProject,
  ClearPattern,
  DeleteSample,
  DiscardTake,
  ReplaceTake,
  ChangePreset,  // the track's sound has edits: loading a preset (payload: its id) loses them
  ChangeEngine,  // the same for another engine (payload: its SynthEngineKind as a number)
  Notice,  // only says something (title, and the detail's lines): ENTER or ESC closes it
};

struct ListItem {
  std::string id;
  std::string label;
  bool marked = false;  // current / active entry
};

// How many rows a list shows, and the first of them with the cursor on `cursor`.
inline constexpr std::size_t kListVisibleRows = 9;
constexpr std::size_t ListFirstRow(std::size_t cursor)
{
  return cursor >= kListVisibleRows ? cursor - kListVisibleRows + 1 : 0;
}

struct ModalFields {
  ModalKind kind = ModalKind::None;
  // Text entry
  TextPurpose textPurpose = TextPurpose::ProjectSaveAs;
  std::string title;
  std::string text;
  std::size_t maxLength = 24;
  // Confirm
  ConfirmPurpose confirmPurpose = ConfirmPurpose::NewProject;
  std::string detail;
  std::string payload;
  bool danger = false;
  std::size_t cursor = 0;
  std::string search;
  // Sample take editing: 0 = start marker, 1 = end marker.
  std::uint8_t trimMarker = 0;
  // Text entry: insertion point (FN + , / move it, FN + ; . jump to the ends).
  std::size_t textCursor = 0;
};

struct Modal : ModalFields {
  // Lists (presets, engines, samples, file options). `items` holds the rows from `firstItem` on,
  // the whole list or, when `itemCount` says it is longer, a window of it. There is no memory for a
  // long list: the preset browser keeps the rows on screen, the rest stay with the engine.
  std::vector<ListItem> items;
  std::size_t firstItem = 0;
  std::size_t itemCount = 0;
  std::vector<std::uint16_t> presets;  // Preset browser: the engine's presets that match the search.

  std::size_t Count() const { return itemCount != 0 ? itemCount : items.size(); }
  const ListItem* Row(std::size_t index) const
  {
    return index >= firstItem && index - firstItem < items.size() ? &items[index - firstItem] : nullptr;
  }
};

// Parameter feedback for FN functions and SETUP edits: a pop-up with the name and the new value.
struct Hud {
  std::string label;
  std::string value;
  float fraction = -1.0f;  // 0..1 bar, -1 none
  bool bipolar = false;    // fraction is -1..1 around a centre
  std::uint32_t at = 0;
};

enum class Arrow : std::uint8_t { Up, Down, Left, Right };

// Key-light vocabulary. Brightness says how much (value / content), the effect says what kind of key
// it is: steady = holds a value, breathe = runs an action when pressed, shimmer = focus / selection,
// blink = waiting (queued / cued). The Animator turns effects into motion.
// Steady holds the level; Blink alternates it with off (something waiting).
// Blink: waiting (4 changes a second). Hint: the keys a Learn guide step waits for, often a whole block,
// blink at half that speed (easier on the eye) until the step is learned.
enum class LedEffect : std::uint8_t { Steady, Blink, Hint };

struct LedCell {
  std::uint8_t level = 0;
  LedEffect effect = LedEffect::Steady;
};
using LedFrame = std::array<LedCell, kKeyCount>;


// A parameter on the pads (a held GRID cell): one pad per value, or an even spread over all of them,
// in reading order. An odd spread when the range has a middle, so a bipolar value can land on it.
inline std::size_t CellPickCount(const synth::SynthParameterInfo& info)
{
  const int range = info.maxValue - info.minValue;
  const int pads = static_cast<int>(kRows * kPadColumns);
  if (range < pads) return static_cast<std::size_t>(range + 1);
  return static_cast<std::size_t>(range % 2 == 0 ? pads - 1 : pads);
}
inline int CellPickValue(const synth::SynthParameterInfo& info, std::size_t pad)
{
  const std::size_t count = CellPickCount(info);
  if (count <= 1) return info.minValue;
  const int range = info.maxValue - info.minValue;
  const int last = static_cast<int>(count) - 1;
  return info.minValue + (static_cast<int>(pad) * range + last / 2) / last;
}
inline std::size_t CellPickPad(const synth::SynthParameterInfo& info)
{
  const std::size_t count = CellPickCount(info);
  std::size_t nearest = 0;
  int distance = 0x7FFFFFFF;
  for (std::size_t pad = 0; pad < count; ++pad)
  {
    const int value = CellPickValue(info, pad);
    const int away = value < info.value ? info.value - value : value - info.value;
    if (away < distance)
    {
      distance = away;
      nearest = pad;
    }
  }
  return nearest;
}

// What ADD and SUB do right now, which the icons on the screen's right edge say: change a value, move
// up and down a list, move sideways (steps, pages, tracks), erase text, or nothing.
enum class AdjustMeaning : std::uint8_t { Value, List, Sideways, Erase, None };

// A drum pad or sequencer hit, for the wave it sends along its column.
struct DrumHit {
  std::uint32_t at = 0;
  std::uint8_t row = 0;
  std::uint8_t velocity = 0;
};

// The GRID cell being held, for the pads to pick its value. A press on a focused switch or choice
// steps it, but only when it was a tap: holding that same cell picks instead.
struct CellPress {
  bool down = false;
  std::uint8_t index = 0;
  bool stepOnRelease = false;
  bool picked = false;
  std::uint32_t at = 0;
};

struct LeftKeyState {
  bool down = false;
  bool used = false;        // combined with another key while held
  bool suppressed = false;  // part of a chord that has its own meaning (Fn + CTRL), or pressed under a modal
  std::uint32_t pressedAt = 0;
};

struct RepeatState {
  bool down = false;
  int direction = 0;
  bool coarse = false;  // FN + ADD / SUB: bigger steps
  std::uint32_t nextAt = 0;
  std::uint32_t count = 0;
  // FN + a quick key (FnQuickControls): the setting it changes and the key held (kKeyCount = none).
  const char* quickId = nullptr;
  std::size_t quickKey = kKeyCount;
};

// FN is the menu of the current screen. Hold FN (or tap it to keep the menu open) and the GRID shows
// what this screen can do: rows 0-2 belong to the page, row 3 is the same everywhere. FN + GRID
// (or GRID in the open menu) runs an item; items with a letter also run on FN + that pad.
enum class MenuAction : std::uint8_t {
  None,
  // Row 3, every screen
  Save,
  TapTempo,
  Record,
  Setup,  // in SETUP: back to the scene it was opened from
  // INSTR
  Presets,
  SavePreset,
  SavePresetAs,
  Engine,
  Default,  // the focused value back to its default (also FN + ENTER)
  SampleFiles,
  SampleRecord,
  SamplePreview,
  // SEQ
  Copy,
  Paste,
  Clear,
  Quantize,
  Duplicate,
  View,         // ROLL / STEP
  PagePrev,     // the GRID's page of a long pattern (also FN + ADD / SUB)
  PageNext,
  LengthDown,   // the length of the current pattern
  LengthUp,
  RecordPanel,  // the RECORD panel (also a held OK)
  Scope,        // the note a chord step edits (also FN + ENTER)
  // TAPE
  LaunchRow,
  Capture,   // the playing patterns become a new SONG row (also FN + ENTER on SESSION)
  SongMode,  // SONG / LOOP (also FN + ENTER on SONG)
  UnmuteAll,
  UnsoloAll,
  // SETUP
  NewProject,
  LoadProject,
  SaveProjectAs,
};

struct MenuItem {
  MenuAction action = MenuAction::None;
  const char* label = "";
  std::string detail;        // what it acts on, or its state
  const char* key = nullptr;  // the FN shortcut that also runs it ("OK" = FN + ENTER, "PLAY" = FN + PLAY)
  bool available = true;
  bool on = false;            // a switch that is on (full LED)
};
using MenuItems = std::array<MenuItem, kGridCount>;

// FN quick controls: while FN is held, the 2x3 pad block beside the GRID changes the most used
// settings at once on any screen. Upper key +, lower key - (like ADD over SUB).
struct FnQuickControl {
  char up;
  char down;
  const char* id;
  const char* label;
};
inline constexpr std::array<FnQuickControl, 3> kFnQuickControls{{
    {'6', 'Y', "proj.bpm", "BPM"},
    {'7', 'U', "sys.volume", "VOLUME"},
    {'8', 'I', "keys.octave", "OCTAVE"},
}};

struct RecordState {
  SeqView noteView = SeqView::Roll;  // the view of melodic tracks
  SeqView drumView = SeqView::Step;  // the view of drum tracks (STEP: the drum machine)
  bool snap = true;               // placing a take snaps its notes to whole steps
  std::uint8_t focus = kRecordFirstTake;  // RECORD panel GRID focus
  std::uint8_t takeBar = 0;       // which bar of the focused take goes into a pattern
  // OK held in SEQ: a tap switches the view, a hold opens the RECORD panel.
  bool okDown = false;
  bool okHeld = false;
  std::uint32_t okDownAt = 0;
  std::array<CaptureTake, kCaptureTakes> takes{};
  std::array<CaptureNote, kCaptureNotes> notes{};
  std::uint32_t nextNote = 0;
  const CaptureNote& TakeNote(const CaptureTake& take, std::size_t index) const { return notes[(take.first + index) % kCaptureNotes]; }
  std::size_t newest = kCaptureTakes;  // index of the newest take; kCaptureTakes = none yet
  std::uint32_t nextTakeId = 1;
  std::uint32_t lastPadActivity = 0;
  std::array<std::uint16_t, 128> openNote{};  // capture note index + 1 of each held pitch
};

// A GRID key down on the note page (ROLL, STEP and the drum machine), as MatrixOS's sequencer has it: its step is
// selected while the key is down. A tap (shorter than kHoldMs, nothing else pressed meanwhile) keeps
// it selected; a hold lets it go at the release, unless it was selected before.
struct StepKey {
  bool down = false;
  bool wrote = false;        // pads were held: the press put their notes in and selects nothing
  bool used = false;         // another key was pressed while this one was down: it was a hold
  bool wasSelected = false;  // selected before this press
  bool wasOnly = false;      // the only step selected before this press: a tap lets it go
  std::size_t step = 0;      // from the start of the pattern
  std::uint32_t at = 0;
};

// Stopped, a step sounds while its key is down: its notes, until that key lets go.
struct StepAudition {
  std::array<std::uint8_t, 8> notes{};
  std::uint8_t count = 0;
  std::uint8_t track = 0;
  std::uint8_t key = 0xFF;  // the GRID index of the step key; 0xFF = none
};

struct UiState {
  // It boots on the instrument, first page (VOICE).
  Scene scene = Scene::Instrument;
  Scene previousScene = Scene::Sequence;
  std::array<std::uint8_t, kSceneCount> page{};
  std::array<std::uint8_t, kTrackCount> drumVelocity{{100, 100, 100, 100}};  // how hard the drum the pads last chose was hit
  std::array<std::uint8_t, kRows * kPadColumns> padVelocity{};
  std::array<DrumHit, kDrumNotes.size()> drumPadHits{};
  std::array<std::uint8_t, kTrackCount> drumLane{};  // the drum each track's pads last chose on the drum page
  // The note each melodic track's pads last chose on the note page (kNoNote: none yet): the row the
  // STEP view's GRID shows.
  std::array<std::uint8_t, kTrackCount> noteLane{{kNoNote, kNoNote, kNoNote, kNoNote}};
  std::array<StepKey, kGridCount> stepKeys{};
  StepAudition stepAudition;
  std::size_t track = synth::kDefaultActiveTrack;
  KeyboardSettings keyboard{};
  // Each track's chord effect (INSTR CHORD).
  std::array<ChordSettings, kTrackCount> chords{};

  std::array<LeftKeyState, 4> left{};  // ESC, TAB, FN, CTRL
  RepeatState repeat{};

  std::vector<Modal> modals;
  Toast toast{};
  Hud hud{};
  std::uint32_t sceneChangedAt = 0;
  std::uint32_t now = 0;

  // Focused GRID slot per INSTR page and per fixed page. Each track keeps its own INSTR page and focus.
  std::array<std::uint8_t, 16> instrumentFocus{};
  std::array<std::uint8_t, kTrackCount> trackInstrumentPage{};
  std::array<std::array<std::uint8_t, 16>, kTrackCount> trackInstrumentFocus{};
  std::uint8_t projectFocus = 4;  // SETUP PROJECT: BPM
  std::uint8_t seqTrackFocus = 0;
  std::uint8_t mixFocus = 0;      // 0..3 track, 4 master
  std::uint8_t sendsFocus = 2;    // TAPE SENDS cell: row = effect, column = track
  std::uint8_t fxFocus = 0;       // TAPE FX: synth::fx::MasterParam
  // Value + 1 each SENDS cell had before ENTER switched it off (0 = none), so ENTER can bring it back.
  std::array<std::uint8_t, kGridCount> sendsMemory{};
  // TAPE song cursor: arrangement row and track, plus the first row shown on the GRID.
  std::size_t songRow = 0;
  std::size_t songTrack = 0;
  std::size_t songScroll = 0;
  synth::SongRow songClipboard{};
  bool songClipboardValid = false;
  std::size_t fileCursor = 0;
  std::vector<std::string> projects;

  std::string projectName;
  bool dirty = false;

  // Copy / paste: steps of the active pattern or a whole pattern.
  bool clipboardIsPattern = false;
  std::size_t clipboardIndex = 0;
  bool clipboardValid = false;

  // Pads: the key each held pad pressed, the notes it sounds (scale lock, chord) and their track.
  CellPress cellPress;
  std::array<std::uint8_t, kRows * kPadColumns> padNote{};
  std::array<std::array<std::uint8_t, kMaxPadNotes>, kRows * kPadColumns> padChord{};
  std::array<std::uint8_t, kRows * kPadColumns> padChordCount{};
  std::array<std::uint8_t, kRows * kPadColumns> padTrack{};
  std::array<bool, kRows * kPadColumns> padArp{};  // the pad feeds the arpeggiator
  // SETUP settings pages (KEYS, SYSTEM): the focused setting of each.
  std::array<std::uint8_t, kSettingsPageCount> settingsFocus{};
  RecordState record;
  // TAPE song cursor edits the pattern (0) or the row length in bars (1); a second tap toggles.
  std::uint8_t songLane = 0;
  // TAPE SESSION: the first pattern row shown and the focused cell (row = pattern, column = track).
  std::size_t sessionScroll = 0;
  std::size_t sessionPattern = 0;
  std::size_t sessionTrack = 0;
  std::array<bool, kGridCount> gridDown{};
  // The FN menu tile under a GRID key pressed with FN (not a plain GRID press: its release must not
  // run the page's own release action). Lit while down and kMenuPressMs after.
  struct MenuPress {
    std::uint8_t index = 0xFF;
    bool down = false;
    std::uint32_t releasedAt = 0;
  } menuPress;
  std::array<std::uint32_t, kTrackCount> lastTrackNoteAt{};

  // Tap tempo
  std::array<std::uint32_t, 4> taps{};
  std::size_t tapCount = 0;

  // Event log for motion and key lighting: when each matrix key was last pressed, the last panic
  // stop, and the last time the whole session was replaced (load / new).
  std::array<std::uint32_t, kKeyCount> keyPressedAt{};
  std::uint32_t panicAt = 0;
  std::uint32_t sessionAt = 0;

};

#if defined(CARDENZA_NATIVE_UI)
enum class NativePresentationKind : std::uint8_t { CellPress, PerformanceNote, Overlay };

struct NativePresentationEvent {
  std::uint64_t serial = 0;
  std::uint32_t at = 0;
  NativePresentationKind kind = NativePresentationKind::CellPress;
  std::uint8_t index = 0, track = 0, page = 0, modal = 0, overlayMask = 0;
  Scene scene = Scene::Instrument;
  std::uint8_t count = 0, velocity = 0;
  bool active = false;
  std::array<std::uint8_t, kMaxPadNotes> notes{};
};

// Presentation-only history. The system/UI task owns both writers and the
// animator consumer; short down/up gestures survive until its next update.
struct NativePresentationEvents {
  std::array<NativePresentationEvent, 16> history{};
  std::uint64_t serial = 0;
  std::uint32_t lastInputAt = 0;
  std::array<bool, 4> overlays{};  // scenes, tracks, menu, pages
};
#endif

class Controller {
public:
  explicit Controller(Host& host);

  // `dirty` restores a session that had unsaved changes (an app switch hands it back).
  void Restore(const ProjectUiSettings& settings, const std::string& projectName, bool dirty = false);
  ProjectUiSettings Settings() const;

  void KeyEvent(std::size_t row, std::size_t column, bool pressed, std::uint32_t nowMs);
#if defined(CARDENZA_NATIVE_UI)
  // Native pointer input uses domain commands; physical keyboard input remains
  // routed through KeyEvent. True means accepted input/activity, not project dirty.
  bool Dispatch(const native::Command& command, std::uint32_t nowMs);
  std::uint8_t NativeFocusedCell() const { return nativeFocusedCell_; }
  bool NativePerformanceHeld(std::size_t track, std::uint8_t note) const;
  void NotifyNativeInput(std::uint32_t nowMs);
  void NotifyNativeCellPress(std::size_t index, std::uint32_t nowMs);
  void NotifyNativeOverlay(std::size_t index, bool active, std::uint32_t nowMs);
  const NativePresentationEvents& NativePresentation() const { return nativePresentation_; }
#endif
  void Tick(std::uint32_t nowMs);
  // What ENTER on a project in FILES does: with unsaved changes it asks first.
  void OpenProject(std::string name);
  // A track on the Drums engine: its pads are the drums (and how hard). In the STEP view its note page
  // is the drum machine (DrumLayout); the GRID's step keys work as on any note page.
  bool DrumTrack() const;
  bool DrumLayout() const;
  AdjustMeaning AdjustKeysMean() const;
  // The pads as the values of the GRID cell being held. What a cell offers: `info` is its range and
  // current value, as for ADD / SUB. Returns false for a cell that holds no parameter.
  bool CellPickerActive() const;
  bool CellParameter(std::size_t index, std::string& label, synth::SynthParameterInfo& info, std::string& text) const;
  // Marks the notes the pads held down are sounding on a track, one bit per MIDI note.
  void AddHeldPadNotes(std::size_t track, std::array<std::uint32_t, 4>& notes) const;
  void UsePianoRollOnDrums(bool roll) { state_.record.drumView = roll ? SeqView::Roll : SeqView::Step; }  // Tests: the drum tracks' view.
  std::size_t DrumLane() const { return state_.drumLane[state_.track]; }
  // A take came back from the card: its screen opens, as it was left.
  void ResumeTake(std::size_t track);
  // The autosave could not be written.
  void ReportAutosaveProblem(const std::string& text);
  // The device started without a card: nothing will be saved. A notice until ENTER or ESC closes it,
  // so it is still there to read after the boot animation.
  void ShowNoCardNotice();
  void ComputeLeds(LedFrame& frame) const;
  // Levels only (no effects), for callers that do not animate.
  void ComputeLeds(std::array<std::uint8_t, kKeyCount>& levels) const;

  // The guide: a lesson page drives the device (Applications/Synth/GrooveAppGuide.cpp).
  // The header says GUIDE, and the keys of the step are lit (bit
  // KeyIndex(row, column) of `keys`): blinking while the step waits for the user, steady once it is
  // learned and the user tries more keys.
  void SetGuide(bool on);
  bool Guide() const { return guide_; }
  void SetGuideHint(std::uint64_t keys, bool waiting = true)
  {
    guideHint_ = guide_ ? keys : 0;
    guideHintWaits_ = waiting;
  }
  std::uint64_t GuideHint() const { return guideHint_; }
  // Where a lesson starts: the track, then the scene and its page (from 0; -1 keeps the scene's page).
  void GuideGoTo(Scene scene, int page, std::size_t track);
  // What the lesson page checks, letter and number pairs with the page tab last:
  // "s1p0t0e0y0m0o0h0q1w0k0z0c0a0g0l0x0v123nVOICE" (GrooveGuide.cpp says what each one is).
  std::string GuideState() const;


  const UiState& State() const { return state_; }
  const Host& HostView() const { return host_; }
  const Modal* TopModal() const { return state_.modals.empty() ? nullptr : &state_.modals.back(); }
  // Hold maps: while a left key is held, the GRID chooses what it switches to. ESC = scenes (column
  // 4 is SETUP), TAB = tracks (select / mute / solo / seq), CTRL = pages of the current scene, FN =
  // the menu of the current screen.
  bool EscOverlay() const;
  bool TabOverlay() const;
  bool FnOverlay() const;
  bool CtrlOverlay() const;
  // The menu kept open by a tap on FN (a modal).
  bool MenuOpen() const { return TopModal() != nullptr && TopModal()->kind == ModalKind::Menu; }
  bool RecordOpen() const { return TopModal() != nullptr && TopModal()->kind == ModalKind::Record; }
  // The current track's note page view (drum and melodic tracks keep one each).
  SeqView View() const { return DrumTrack() ? state_.record.drumView : state_.record.noteView; }

  // Captured takes, newest first (order 0). Null past the last take.
  struct PlacedNote {
    std::uint8_t step = 0;
    std::uint8_t microstep = 0;
    std::uint8_t note = 0;
    std::uint8_t velocity = 0;
    std::uint16_t lengthPulses = 0;
  };
  std::size_t TakeCount() const;
  const CaptureTake* TakeAt(std::size_t order) const;
  // A take laid out on the steps of its track's pattern: how many pattern lengths it spans, and
  // the notes of one of them (snapped to steps when the SNAP setting is on).
  std::size_t TakeBars(const CaptureTake& take) const;
  void ForEachTakeBarNote(const CaptureTake& take, std::size_t bar, void (*visit)(const PlacedNote&, void*), void* context) const;
  // While a screen takes input (text, lists, the sample editor) FN is the yellow keycap layer:
  // ; , . / are arrows. Everywhere else FN is the menu.
  bool FnYellowMode() const { return TopModal() != nullptr && TopModal()->kind != ModalKind::Menu; }
  // The menu is on screen: FN held (past the hold threshold) or kept open.
  bool MenuShown() const { return FnOverlay() || MenuOpen(); }
  // The menu of the current screen (the screen under the open menu).
  MenuItems Menu() const;

  std::size_t PageCount(Scene scene) const;
  std::uint8_t CurrentPage() const { return state_.page[static_cast<std::size_t>(state_.scene)]; }
  const InstrumentPage* CurrentInstrumentPage() const;
  InstrumentPageList InstrumentPages() const;
  std::uint8_t InstrumentFocus() const;
  std::string PageTab(Scene scene, std::size_t page) const;
  // Short page description for the CTRL page map.
  std::string PageName(Scene scene, std::size_t page) const;

  bool PadNoteFor(std::size_t row, std::size_t column, int& note) const;
  bool IsPadDown(std::size_t row, std::size_t column) const;
  bool IsNoteHeld(int note) const;

  // FN combinations besides the menu (ESC stop, TAB previous track, ...), for the menu's footer.
  struct FnEntry {
    const char* key;
    const char* label;
    bool available;
  };
  std::vector<FnEntry> FnEntries() const;

  // Presentation helpers shared by renderer and LEDs. Slots are engine parameters or settings:
  // "keys.*" (layout, scale, root, octave, lock, velocity, select) for every track, "chord.*" (type,
  // voicing) and "arp.*" (mode, rate, octaves, gate, latch) of the current track, "proj.*" (bpm,
  // swing, mode, steps, rec) and "sys.*" (volume, led, screen, log).
  bool SlotHasValue(const Slot& slot) const;
  bool SlotValue(const Slot& slot, synth::SynthParameterInfo& info, std::string& text) const;
  bool SettingValue(const char* id, synth::SynthParameterInfo& info, std::string& text) const;
  // The SETUP settings page on screen (KEYS, SYSTEM), or null, and its focused cell.
  const InstrumentPage* SettingsPageShown() const;
  std::uint8_t SettingsFocus() const;
  // Pads that sound right now for the active track, base keys and chord notes.
  bool IsChordNoteHeld(int note) const;

private:
#if defined(CARDENZA_NATIVE_UI)
  bool NativeFocusCell(std::size_t index);
  void NativeEnter();
  void NativeCancelGesture();
  void SaveProjectAutomatic();
  std::uint8_t nativeFocusedCell_ = 0xFF;
  bool nativeSampleSaving_ = false;
  NativePresentationEvents nativePresentation_{};
  void RecordNativePresentation(NativePresentationEvent event);
  void NotifyNativePerformance(std::size_t track, const std::uint8_t* notes, std::size_t count,
                               std::uint8_t velocity, std::uint32_t nowMs);
  struct NativeContact {
    bool active = false;
    std::uint8_t id = 0, track = 0, baseNote = 0, count = 0, velocity = 0;
    bool arp = false;
    std::array<std::uint8_t, kMaxPadNotes> notes{};
  };
  std::array<NativeContact, 16> nativeContacts_{};
  // Read by the sequencer clock task; the UI alone owns the contact/pad arrays.
  std::array<std::array<std::atomic<std::uint32_t>, 4>, kTrackCount> nativeDirectHeld_{};
  void NativePublishPerformance();
  bool NativeNoteDown(std::uint8_t contact, std::uint8_t note, std::uint8_t velocity);
  bool NativeNoteUp(std::uint8_t contact);
  void NativeReleasePerformance();
  std::size_t NativeNoteOwners(std::size_t track, std::uint8_t note, bool arp) const;
  void NativeSoundPerformance(std::size_t track, std::uint8_t note, std::uint8_t velocity, bool on, bool arp);
#endif
  // Input routing
  void PressLeft(std::size_t row);
  void ReleaseLeft(std::size_t row);
  void PressRight(KeyRole role);
  void ReleaseRight(KeyRole role);
  void PressPad(std::size_t row, std::size_t column);
  void ReleasePad(std::size_t row, std::size_t column);
  void PressGrid(std::size_t index);
  void ReleaseGrid(std::size_t index);
  bool HandleFnKey(std::size_t row, std::size_t column);
  bool HandleTextKey(std::size_t row, std::size_t column);
  bool HandleSearchKey(std::size_t row, std::size_t column);

  // Navigation
  void GoToScene(Scene scene);
  void SetPage(std::uint8_t page);
  void StepPage(int delta);
  void Back();
  void SelectTrack(std::size_t track);
  void Adjust(int delta, bool coarse);
  void Enter();
  void FnEnter();
  void TogglePlay();
  void ToggleRecord();
  void ArrowKey(Arrow arrow);

  // Scene handlers (GrooveScenes.cpp)
  void ProjectGrid(std::size_t index);
  void ProjectAdjust(int delta, bool coarse);
  void ProjectEnter();
  void InstrumentGrid(std::size_t index);
  void InstrumentAdjust(int delta, bool coarse);
  void InstrumentEnter();
  void InstrumentReset();
  void SequenceGrid(std::size_t index);
  void SequenceAdjust(int delta, bool coarse);
  void SequenceEnter();
  void SongGrid(std::size_t index);
  void SongAdjust(int delta, bool coarse);
  void SongEnter();
  void SongFnEnter();
  void SongArrow(int horizontal, int vertical);
  void MixGrid(std::size_t index);
  void MixAdjust(int delta, bool coarse);
  void MixEnter();
  void SessionGrid(std::size_t index);
  void SessionAdjust(int delta);
  void SessionLaunchRow();
  void SessionCapture();
  void SendsGrid(std::size_t index);
  void SendsAdjust(int delta, bool coarse);
  void SendsEnter();
  void SendsReset();
  void FxGrid(std::size_t index);
  void FxAdjust(int delta, bool coarse);
  void FxEnter();
  void FxReset();
  void SetSendValue(std::size_t track, const char* id, int value);
  void RunSlotAction(SlotAction action);
  // Global settings by id (see SettingValue). Step wraps choices and flips switches (ENTER).
  void AdjustSetting(const char* id, int delta);
  void StepSetting(const char* id);
  void ResetSetting(const char* id);
  void ShowSettingHud(const char* id, const char* label);
  void ShowHud(const std::string& label, const std::string& value, float fraction = -1.0f, bool bipolar = false);
  void FlipStepPage(int delta);
  // The note page's step keys (StepKey). Any other key pressed while one is down makes it a hold.
  void StepKeyPress(std::size_t index);
  void StepKeyRelease(std::size_t index);
  void UseStepKeys();
  void AuditionStep(std::size_t index, std::size_t step);
  void StopStepAudition();
  void AdjustPatternLength(int delta, bool coarse);
  void SettingsGrid(std::size_t index);
  void SettingsAdjust(int delta, bool coarse);
  void SettingsEnter();
  void SettingsReset();
  void SettingsArrow(int step);
  void OpenMenu();
  void RunMenu(std::size_t index);
  void RunMenuAction(const MenuItem& item);
  void SoundPad(std::size_t index, bool on);
  void StepPreset(int delta);
  // Edits to the track's sound are lost by loading a preset or switching the engine: with edits this
  // asks first (ENTER loads, ESC keeps them) and returns true; the confirm sits over the browser or
  // picker it came from, so ESC goes back there.
  bool ConfirmLosingEdits(ConfirmPurpose purpose, const std::string& title, const std::string& payload);
  void ApplyPreset(const std::string& presetId);
  void ApplyEngine(synth::SynthEngineKind kind);
  bool PadForSequence(const std::uint8_t* notes, std::size_t count, bool pressed, std::uint8_t velocity = 0);
  // Recording (GrooveRecord.cpp).
  void CaptureNoteOn(std::size_t track, std::uint8_t note, std::uint8_t velocity);
  void CaptureNoteOff(std::size_t track, std::uint8_t note);
  bool OkDeferred() const;
  void OkPress();
  void OkRelease();
  void OkTick();
  void ToggleSeqView();
  void OpenRecord();
  void RecordGrid(std::size_t index);
  void RecordAdjust(int delta);
  void RecordEnter();
  void RecordFnEnter();
  bool PlaceTake(bool merge);

  // Fn functions
  void AllStop();
  void Copy();
  void Paste();
  void Clear();
  void Quantize();
  void Duplicate();
  void TapTempo();
  void QuickSave();
  // `announce` pops up the result (FN presses); grids that show the state themselves stay quiet.
  void ToggleMute(std::size_t track, bool announce = false);

  // Modals (GrooveModals.cpp)
  void OpenModal(Modal modal);
  void CloseModal();
  Modal* Top();
  void OpenTextEntry(TextPurpose purpose, const std::string& title, const std::string& initial, std::size_t maxLength);
  void OpenConfirm(ConfirmPurpose purpose, const std::string& title, const std::string& detail, bool danger, const std::string& payload = {});
  void OpenPresetBrowser();
  void RefreshPresetBrowser();
  bool guide_ = false;
  std::uint64_t guideHint_ = 0;
  bool guideHintWaits_ = true;
  void ComputeSceneLeds(LedFrame& frame) const;
  void FillPresetRows(Modal& modal) const;
  void SetCellParameter(std::size_t index, int value);
  void PickCellValue(std::size_t row, std::size_t column);
  void ShowCellHud();
  void StepCell(std::size_t index);
  void OpenEnginePicker();
  void OpenSampleBrowser();
  void RefreshSampleBrowser();
  void OpenSampleRecord();
  void ModalAdjust(int delta, bool coarse);
  void ModalEnter();
  void ModalFnEnter();
  void ModalGrid(std::size_t index);
  void ModalCancel();
  void CommitText();
  void Confirmed();
  void SampleRecordEnter();
  void SampleRecordGrid(std::size_t index);

  // Project actions
  void SaveProject(bool saveAs);
  void SaveProjectNamed(const std::string& name, bool overwrite);
  void LoadProjectNamed(const std::string& name);
  void NewProject();
  void RefreshProjects();

  bool SongPage() const;
  void ShowToast(const std::string& text, ToastKind kind = ToastKind::Info);
  void MarkDirty() { state_.dirty = true; }
  void ReleaseAllPads();
  std::size_t CollectHeldNotes(std::array<std::uint8_t, kRows * kPadColumns>& notes) const;
  synth::ISynthEngine* ActiveEngine();
  const synth::ISynthEngine* ActiveEngine() const;
  synth::SynthEngineKind ActiveKind() const;
  void ClampPages();

  Host& host_;
  UiState state_{};
};

}  // namespace groove
