#include "Applications/Synth/UI/Groove/GrooveTypes.h"

#include <algorithm>
#include <string>

namespace groove {

namespace {

std::size_t ScaleNoteCount(std::uint16_t mask)
{
  std::size_t count = 0;
  for (std::size_t bit = 0; bit < 12; ++bit) count += (mask >> bit) & 1u;
  return std::max<std::size_t>(1, count);
}

int ScaleDegreeOffset(std::uint16_t mask, std::size_t degree)
{
  const std::size_t count = ScaleNoteCount(mask);
  std::size_t wanted = degree % count;
  for (int semitone = 0; semitone < 12; ++semitone)
  {
    if (((mask >> semitone) & 1u) == 0) continue;
    if (wanted == 0) return semitone;
    --wanted;
  }
  return 0;
}

}  // namespace

bool PadNote(const KeyboardSettings& settings, synth::SynthEngineKind kind, std::size_t row, std::size_t column, int& note)
{
  if (row >= kRows || column < kPadFirstColumn || column >= kPadFirstColumn + kPadColumns) return false;
  const std::size_t x = column - kPadFirstColumn;
  if (kind == synth::SynthEngineKind::Drums)
  {
    note = kDrumNotes[x];
    return true;
  }
  const std::size_t y = kRows - 1 - row;  // bottom row is lowest
  const int octaveShift = std::clamp(settings.octave, -3, 3) * 12;
  const int base = static_cast<int>(synth::kHomePerformanceBaseMidiNote) + octaveShift;

  switch (settings.layout)
  {
    case KeyboardLayout::Piano:
    {
      static constexpr std::array<int, 7> kWhite{{0, 2, 4, 5, 7, 9, 11}};
      static constexpr std::array<int, 7> kBlack{{-1, 1, 3, -1, 6, 8, 10}};
      // Two whole octaves, a pair of rows each, as on a keyboard: black keys above white keys that
      // start on C at the left. The lower pair is this octave (C at the bottom left), the upper pair
      // the next.
      static constexpr std::array<int, kRows> kRowOctave{{1, 1, 0, 0}};
      const std::size_t key = x % kWhite.size();
      const int octave = (kRowOctave[row] + static_cast<int>(x / kWhite.size())) * 12;
      const int offset = row % 2 == 1 ? kWhite[key] : kBlack[key];
      if (offset < 0) return false;
      note = base + octave + offset;
      break;
    }
    case KeyboardLayout::Scale:
    {
      const std::uint16_t mask = kScales[std::min(settings.scale, kScales.size() - 1)].mask;
      const std::size_t count = ScaleNoteCount(mask);
      note = base + settings.root + static_cast<int>(y) * 12 + static_cast<int>(x / count) * 12 + ScaleDegreeOffset(mask, x);
      break;
    }
    case KeyboardLayout::Chromatic:
    default:
      note = base + settings.root + static_cast<int>(y * kPadColumns + x);
      break;
  }
  return note >= 0 && note <= 127;
}

bool InScale(const KeyboardSettings& settings, int note)
{
  const std::uint16_t mask = kScales[std::min(settings.scale, kScales.size() - 1)].mask;
  const int degree = ((note - settings.root) % 12 + 12) % 12;
  return ((mask >> degree) & 1u) != 0;
}

int SnapToScale(const KeyboardSettings& settings, int note)
{
  if (settings.layout != KeyboardLayout::Scale && settings.lock == ScaleLock::Snap)
    while (note > 0 && !InScale(settings, note)) --note;
  return note;
}

int NextKeyboardNote(const KeyboardSettings& settings, int note, int direction)
{
  const bool scaleOnly = settings.layout == KeyboardLayout::Scale || settings.lock != ScaleLock::Off;
  const int step = direction < 0 ? -1 : 1;
  do note += step;
  while (scaleOnly && note >= 0 && note <= 127 && !InScale(settings, note));
  return note;
}

std::size_t PadNotes(const KeyboardSettings& settings, const ChordSettings& shape, synth::SynthEngineKind kind, int note,
                     std::array<std::uint8_t, kMaxPadNotes>& notes)
{
  if (note < 0 || note > 127) return 0;
  if (kind == synth::SynthEngineKind::Drums)
  {
    notes[0] = static_cast<std::uint8_t>(note);
    return 1;
  }
  // The scale lock only acts where keys outside the scale exist.
  if (settings.layout != KeyboardLayout::Scale && settings.lock == ScaleLock::Mute && !InScale(settings, note)) return 0;
  note = SnapToScale(settings, note);
  std::array<int, kMaxPadNotes> chord{};
  std::size_t count = 0;
  auto add = [&](int interval) { if (count < chord.size()) chord[count++] = note + interval; };
  const std::uint16_t mask = kScales[std::min(settings.scale, kScales.size() - 1)].mask;
  // Stacks thirds inside the scale: every other scale step above the note.
  auto stack = [&](std::size_t voices) {
    int pitch = note;
    add(0);
    for (std::size_t voice = 1; voice < voices; ++voice)
      for (int steps = 0; steps < 2; ++steps)
      {
        do ++pitch; while (((mask >> (((pitch - settings.root) % 12 + 12) % 12)) & 1u) == 0);
        if (steps == 1) add(pitch - note);
      }
  };
  const bool inScale = InScale(settings, note);
  switch (shape.chord)
  {
    case ChordType::Triad: if (inScale) stack(3); else { add(0); add(4); add(7); } break;
    case ChordType::Seventh: if (inScale) stack(4); else { add(0); add(4); add(7); add(10); } break;
    case ChordType::Major: add(0); add(4); add(7); break;
    case ChordType::Minor: add(0); add(3); add(7); break;
    case ChordType::Sus4: add(0); add(5); add(7); break;
    case ChordType::Major7: add(0); add(4); add(7); add(11); break;
    case ChordType::Minor7: add(0); add(3); add(7); add(10); break;
    case ChordType::Dominant7: add(0); add(4); add(7); add(10); break;
    case ChordType::Power: add(0); add(7); add(12); break;
    case ChordType::Octave: add(0); add(12); break;
    case ChordType::Off:
    default: add(0); break;
  }
  // Voicing: inversions lift the lowest notes an octave; SPREAD lifts the second note.
  if (count > 1)
  {
    switch (shape.voicing)
    {
      case 1: chord[0] += 12; break;
      case 2: chord[0] += 12; if (count > 2) chord[1] += 12; break;
      case 3: chord[1] += 12; break;
      default: break;
    }
  }
  std::size_t written = 0;
  for (std::size_t index = 0; index < count; ++index)
  {
    if (chord[index] < 0 || chord[index] > 127) continue;
    const auto pitch = static_cast<std::uint8_t>(chord[index]);
    if (std::find(notes.begin(), notes.begin() + static_cast<std::ptrdiff_t>(written), pitch) != notes.begin() + static_cast<std::ptrdiff_t>(written)) continue;
    notes[written++] = pitch;
  }
  std::sort(notes.begin(), notes.begin() + static_cast<std::ptrdiff_t>(written));
  return written;
}

std::string_view EngineShortName(synth::SynthEngineKind kind)
{
  switch (kind)
  {
    case synth::SynthEngineKind::Dx7: return "DX7";
    case synth::SynthEngineKind::Juno: return "JUNO";
    case synth::SynthEngineKind::Sampler: return "SAMPLER";
    case synth::SynthEngineKind::Drums: return "DRUMS";
    case synth::SynthEngineKind::None:
    default: return "---";
  }
}

std::string_view EngineCode(synth::SynthEngineKind kind)
{
  switch (kind)
  {
    case synth::SynthEngineKind::Dx7: return "DX7";
    case synth::SynthEngineKind::Juno: return "JUN";
    case synth::SynthEngineKind::Sampler: return "SMP";
    case synth::SynthEngineKind::Drums: return "DRM";
    case synth::SynthEngineKind::None:
    default: return "---";
  }
}

std::string_view DrumName(int note)
{
  for (std::size_t index = 0; index < kDrumNotes.size(); ++index)
    if (kDrumNotes[index] == note) return kDrumNames[index];
  return {};
}

std::string NoteName(int note)
{
  if (note < 0 || note > 127) return "--";
  return std::string(kNoteNames[static_cast<std::size_t>(note % 12)]) + std::to_string(note / 12 - 1);
}

}  // namespace groove
