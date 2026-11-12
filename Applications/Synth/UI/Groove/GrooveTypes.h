#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "Library/Synth/Core/SynthTypes.h"

namespace groove {

inline constexpr std::size_t kTrackCount = synth::kSynthTrackCount;
inline constexpr std::size_t kRows = synth::kKeyboardMatrixRows;
inline constexpr std::size_t kColumns = synth::kKeyboardMatrixColumns;
inline constexpr std::size_t kKeyCount = kRows * kColumns;
inline constexpr std::size_t kPadColumns = synth::kHomePerformanceColumnCount;
inline constexpr std::size_t kPadFirstColumn = synth::kHomePerformanceFirstColumn;
inline constexpr std::size_t kGridFirstColumn = synth::kSequencerStepFirstColumn;
inline constexpr std::size_t kGridColumns = synth::kSequencerStepColumnCount;
inline constexpr std::size_t kGridCount = kRows * kGridColumns;

// Four scenes in two kinds, numbered as on screen. 0 SETUP comes first and holds what is not the
// music of a track: the project (save, load, tempo) and the global settings (keys, system). Then
// the music, which a tap on ESC walks in a ring: 1 INSTR (what the current track sounds like,
// its chord and arpeggiator included), 2 SEQ (what it plays) and 3 TAPE (all four tracks together:
// clips, song, mix, sends and master effects). ESC never cycles into SETUP; hold ESC and press
// GRID column 1.
enum class Scene : std::uint8_t {
  Setup = 0,
  Instrument = 1,
  Sequence = 2,
  Tape = 3,
};
inline constexpr std::size_t kSceneCount = 4;

inline constexpr std::array<const char*, kSceneCount> kSceneNames{{"SETUP", "INSTR", "SEQ", "TAPE"}};

// The ring a tap on ESC walks: INSTR, SEQ, TAPE, INSTR ...
constexpr bool IsMusicScene(Scene scene) { return scene != Scene::Setup; }
constexpr Scene NextMusicScene(Scene scene) { return scene == Scene::Instrument ? Scene::Sequence : scene == Scene::Sequence ? Scene::Tape : Scene::Instrument; }

// TAPE sub-pages: SESSION (launch patterns per track, like a clip grid), SONG (the arrangement),
// then the mix, the per-track sends and the master effects.
inline constexpr std::uint8_t kTapeSession = 0;
inline constexpr std::uint8_t kTapeSong = 1;
inline constexpr std::uint8_t kTapeMix = 2;
inline constexpr std::uint8_t kTapeSends = 3;
inline constexpr std::uint8_t kTapeFx = 4;
inline constexpr std::size_t kTapePageCount = 5;

// SETUP sub-pages: the project and its files, then the settings for every track (KEYS) and the
// device (SYSTEM). The settings pages are slot pages (SettingsPage(page - kSetupKeys)).
inline constexpr std::uint8_t kSetupProject = 0;
inline constexpr std::uint8_t kSetupFiles = 1;
inline constexpr std::uint8_t kSetupKeys = 2;
inline constexpr std::uint8_t kSetupSystem = 3;
inline constexpr std::size_t kSetupPageCount = 4;

// TAPE SENDS rows: the per-track effect each GRID row edits (columns are the tracks).
inline constexpr std::array<const char*, 4> kSendIds{{"fx.filter", "fx.chorus", "fx.delay", "fx.reverb"}};
inline constexpr std::array<const char*, 4> kSendTags{{"FLT", "CHO", "DLY", "REV"}};

enum class KeyRole : std::uint8_t {
  None,
  Esc,
  Tab,
  Fn,
  Ctrl,
  Sub,
  Add,
  Enter,
  Play,
  Pad,
  Grid,
};

constexpr KeyRole RoleFor(std::size_t row, std::size_t column)
{
  if (row >= kRows || column >= kColumns) return KeyRole::None;
  if (column == 0)
  {
    constexpr std::array<KeyRole, kRows> kLeft{{KeyRole::Esc, KeyRole::Tab, KeyRole::Fn, KeyRole::Ctrl}};
    return kLeft[row];
  }
  if (column == kColumns - 1)
  {
    // Top to bottom: ADD (+), SUB (-), ENTER, PLAY.
    constexpr std::array<KeyRole, kRows> kRight{{KeyRole::Add, KeyRole::Sub, KeyRole::Enter, KeyRole::Play}};
    return kRight[row];
  }
  return column < kGridFirstColumn ? KeyRole::Pad : KeyRole::Grid;
}

constexpr std::size_t GridIndex(std::size_t row, std::size_t column)
{
  return row * kGridColumns + (column - kGridFirstColumn);
}

constexpr std::size_t KeyIndex(std::size_t row, std::size_t column)
{
  return row * kColumns + column;
}

// Key-cap characters used for text entry and Fn shortcuts. 0 = no character.
inline constexpr std::array<std::array<char, kColumns>, kRows> kLegend{{
    {{'`', '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', 0}},
    {{0, 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '[', ']', '\\'}},
    {{0, 0, 'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ';', '\'', 0}},
    {{0, 0, 0, 'Z', 'X', 'C', 'V', 'B', 'N', 'M', ',', '.', '/', ' '}},
}};

constexpr char LegendChar(std::size_t row, std::size_t column)
{
  return row < kRows && column < kColumns ? kLegend[row][column] : 0;
}

enum class KeyboardLayout : std::uint8_t {
  Piano = 0,
  Scale = 1,
  Chromatic = 2,
};

struct ScaleDefinition {
  const char* label;
  std::uint16_t mask;  // bit n = semitone n
};

// Projects store the index: append new scales, never reorder.
inline constexpr std::array<ScaleDefinition, 14> kScales{{
    {"MAJOR", 0b101010110101},
    {"MINOR", 0b010110101101},
    {"DORIAN", 0b011010101101},
    {"MIXOLYD", 0b011010110101},
    {"MIN PENT", 0b010010101001},
    {"MAJ PENT", 0b001010010101},
    {"BLUES", 0b010011101001},
    {"HARM MIN", 0b100110101101},
    {"PHRYGIAN", 0b010110101011},
    {"LYDIAN", 0b101011010101},
    {"LOCRIAN", 0b010101101011},
    {"MEL MIN", 0b101010101101},
    {"WHOLE", 0b010101010101},
    {"IN SEN", 0b010010100011},
}};

inline constexpr std::array<const char*, 12> kNoteNames{{"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"}};
inline constexpr std::array<const char*, 3> kLayoutNames{{"PIANO", "SCALE", "CHROMA"}};
inline constexpr std::array<std::uint8_t, 8> kDrumNotes{{36, 38, 42, 46, 39, 43, 56, 70}};
// On a drum track a column of pads is one drum and its rows are how hard: top row hardest.
inline constexpr std::array<std::uint8_t, 4> kDrumRowVelocity{{127, 100, 70, 40}};
constexpr std::size_t DrumRowForVelocity(std::uint8_t velocity)
{
  return velocity >= 114 ? 0 : velocity >= 85 ? 1 : velocity >= 55 ? 2 : 3;
}
inline constexpr std::array<const char*, 8> kDrumNames{{"BD", "SD", "CH", "OH", "CP", "TM", "CB", "MA"}};

// What one pad press plays. TRIAD and 7TH stack thirds inside the scale (a key outside the scale
// borrows a major / dominant chord); the rest are fixed shapes.
enum class ChordType : std::uint8_t { Off, Triad, Seventh, Major, Minor, Sus4, Major7, Minor7, Dominant7, Power, Octave };
inline constexpr std::size_t kChordTypeCount = 11;
inline constexpr std::array<const char*, kChordTypeCount> kChordNames{{
    "OFF", "TRIAD", "7TH", "MAJ", "MIN", "SUS4", "MAJ7", "MIN7", "DOM7", "POWER", "OCTAVE"}};
inline constexpr std::array<const char*, 4> kVoicingNames{{"ROOT", "INV1", "INV2", "SPREAD"}};
// Keys outside the scale (PIANO / CHROMA layouts): play them, mute them, or snap down to the scale.
enum class ScaleLock : std::uint8_t { Off, Mute, Snap };
inline constexpr std::array<const char*, 3> kScaleLockNames{{"OFF", "MUTE", "SNAP"}};
inline constexpr std::size_t kMaxPadNotes = 5;

// The keyboard for every track (SETUP KEYS, saved with the project). The default layout is the
// piano keyboard: white keys on the lower row of each pair, black keys above.
struct KeyboardSettings {
  int octave = 0;           // -3..+3
  int root = 0;             // 0..11
  std::size_t scale = 0;    // index into kScales
  KeyboardLayout layout = KeyboardLayout::Piano;
  ScaleLock lock = ScaleLock::Off;
  std::uint8_t velocity = 110;  // 1..127
  // ROLL: a GRID tap moves to that one step (false) or adds it to a selection of several (true).
  bool multiSelect = false;
};

// Maps a pad (row, column) to a MIDI note for the given engine. Returns false for gaps (Piano layout).
bool PadNote(const KeyboardSettings& settings, synth::SynthEngineKind kind, std::size_t row, std::size_t column, int& note);

// One track's chord effect (INSTR CHORD, saved with the project): what a key press of that track
// plays, built on the scale of the keyboard settings.
struct ChordSettings {
  ChordType chord = ChordType::Off;
  std::uint8_t voicing = 0;  // index into kVoicingNames

  bool operator==(const ChordSettings& other) const { return chord == other.chord && voicing == other.voicing; }
};
bool InScale(const KeyboardSettings& settings, int note);
// The note a melodic key's chord is built on: under LOCK SNAP a key outside the scale moves down to it.
int SnapToScale(const KeyboardSettings& settings, int note);
// One key of the keyboard up (direction 1) or down (-1) from `note`: the next semitone, or the next
// note of the scale where the pads play only the scale (SCALE layout, or a scale lock). Below 0 or
// above 127 past the ends.
int NextKeyboardNote(const KeyboardSettings& settings, int note, int direction);
// The notes one pad press plays: the scale lock, then the track's chord and its voicing. Drums
// always play the single drum. Returns the count (0 = the key is muted by the scale lock).
std::size_t PadNotes(const KeyboardSettings& settings, const ChordSettings& chord, synth::SynthEngineKind kind, int note,
                     std::array<std::uint8_t, kMaxPadNotes>& notes);

std::string_view EngineShortName(synth::SynthEngineKind kind);
// Three-letter engine code for narrow columns.
std::string_view EngineCode(synth::SynthEngineKind kind);
// Drum name for GM drum notes on the Drums engine, empty otherwise.
std::string_view DrumName(int note);
std::string NoteName(int note);

}  // namespace groove
