#include "Applications/Synth/UI/Sequencer/SequencerUiRenderer.h"
#include "Applications/Synth/UI/Render/SynthUiRenderPrimitives.h"
#include <algorithm>
#include <array>
#include <cmath>

namespace synth::ui_render {
namespace {
// Display labels for the eight performance columns while LEN / VEL / STEP is selected.
// They mirror kLengthPulses and kVelocityBuckets in SynthSequencer.cpp.
constexpr std::array<const char*, 8> kLengthLabels = {"1", "2", "3", "4", "6", "8", "12", "16"};
constexpr std::array<int, 8> kVelocityValues = {24, 40, 56, 72, 88, 104, 120, 127};

std::string Index(const char* prefix, std::size_t index)
{
  return std::string(prefix) + (index < 9 ? "0" : "") + std::to_string(index + 1);
}

ui::Rect HomeBody(const ui::Rect& bounds)
{
  return InsetRect(bounds, kHomeContentPadding, 0);
}

int KeyGridX(const ui::Rect& body)
{
  return body.x + (body.width - kKeyGridWidth) / 2;
}

std::string HeldStepLead(const SequencerUiState& sequence)
{
  return sequence.heldStepCount > 1 ? std::to_string(sequence.heldStepCount) + " STEPS" : Index("STEP ", sequence.primaryHeldStep);
}

std::string HeldStepDetail(const SequencerUiState& sequence)
{
  if (sequence.editLane != SequencerEditLane::Note) return sequence.valueLabel;
  return sequence.noteNames.empty() ? "EMPTY" : sequence.noteNames;
}

const char* OperationTitle(const SequencerUiState& sequence)
{
  if (sequence.clearMode) return "CLEAR";
  if (sequence.copyMode) return sequence.hasCopySource ? (sequence.copySourceIsPattern ? "COPY PATTERN" : "COPY STEP") : "COPY";
  if (sequence.quantizeMode) return "QUANTIZE";
  return "PATTERN";
}

std::string OperationHint(const SequencerUiState& sequence)
{
  if (sequence.copyMode)
    return sequence.hasCopySource ? Index(sequence.copySourceIsPattern ? "P" : "S", sequence.copySourceIndex) + " TO ?" : "SOURCE ?";
  if (sequence.clearMode || sequence.quantizeMode) return "TARGET ?";
  return {};
}

void DrawHomeTitle(ui::Canvas& canvas, const UiScreenSnapshot& snapshot, bool gridView, float alpha)
{
  const auto& sequence = snapshot.sequencer;
  if (sequence.shiftActive || sequence.patternSelectorVisible)
  {
    const bool operation = sequence.clearMode || sequence.copyMode || sequence.quantizeMode;
    DrawTitleBar(canvas, sequence.patternSelectorVisible ? "PATTERN" : "SEQUENCE",
                 operation ? std::string(OperationTitle(sequence)) : Index("P", sequence.activePattern),
                 sequence.clearMode ? kDanger : kAccent,
                 operation ? OperationHint(sequence) : std::to_string(sequence.patternCount) + " IN USE", {}, kDim, alpha);
    return;
  }
  if (sequence.hasHeldSteps)
  {
    if (gridView)
    {
      DrawTitleBar(canvas, sequence.heldStepCount > 1 ? std::to_string(sequence.heldStepCount) + " STEPS" : Index("STEP", sequence.primaryHeldStep),
                   HeldStepDetail(sequence), kAccent,
                   "ALL NOTES",
                   sequence.selectionLatched ? "HOME:EXIT" : "HOLD", kDim, alpha);
      return;
    }
    // The Piano view shows the value large below, so only the grid repeats it in the grey line.
    DrawTitleBar(canvas, "SEQUENCE", HeldStepLead(sequence), kAccent, gridView ? HeldStepDetail(sequence) : std::string{}, {}, kDim, alpha);
    return;
  }
  if (gridView)
  {
    DrawTitleBar(canvas, "SEQ", "T" + std::to_string(sequence.activeTrack + 1) + " " + snapshot.headerPrimary, kAccent,
                 snapshot.primaryText, "HOME:KEYS", kDim, alpha);
    return;
  }
  DrawTitleBar(canvas, snapshot.title, snapshot.headerPrimary, kAccent, snapshot.primaryText, OctaveStatusText(snapshot),
               snapshot.keyboardOctaveChanged ? kAccent : kDim, alpha);
}

void DrawStepGrid(ui::Canvas& canvas, const ui::Rect& body, const UiScreenSnapshot& snapshot, float alpha)
{
  const auto& sequence = snapshot.sequencer;
  constexpr int kTop = 11;
  constexpr int kCellHeight = 20;
  constexpr int kRowGap = 1;
  constexpr int kColumnGap = 3;
  const int cellWidth = (body.width - kColumnGap * 3) / 4;
  const ui::Color filled = MixColor(kSurface, kRule, 0.25f);
  const ui::Color quietNumber = MixColor(kDim, kRule, 0.15f);
  const bool copyStepSource = sequence.copyMode && sequence.hasCopySource && !sequence.copySourceIsPattern;

  DrawInk(canvas, body.x, body.y + 1, Index("P", sequence.activePattern) + " " + std::to_string(sequence.bpm) + "BPM", FadeColor(kDim, alpha));
  const std::string transport = !sequence.trackEnabled ? "OFF" : sequence.trackMuted ? "MUTE" : sequence.playing ? "PLAY" : "STOP";
  DrawInk(canvas, body.x + body.width - InkWidth(canvas, transport, 1), body.y + 1, transport,
          FadeColor(sequence.playing && sequence.trackEnabled && !sequence.trackMuted ? kPlayback : kDim, alpha));

  for (std::size_t i = 0; i < sequence.steps.size(); ++i)
  {
    const auto& step = sequence.steps[i];
    const int column = static_cast<int>(i % kSequencerStepColumnCount);
    const int row = static_cast<int>(i / kSequencerStepColumnCount);
    const float entrance = Clamp01(snapshot.sequencerReveal * 1.4f - static_cast<float>(row) * 0.13f);
    const float cellAlpha = alpha * entrance;
    const int lift = static_cast<int>((1.0f - entrance) * 3.0f);
    const auto cell = MakeRect(body.x + column * (cellWidth + kColumnGap), body.y + kTop + row * (kCellHeight + kRowGap) + lift,
                               cellWidth, kCellHeight);
    const bool pressed = step.held && !sequence.selectionLatched;
    ui::Color fill = pressed ? kAccent : step.held ? kSelection : step.hasEvents ? filled : kSurface;
    fill = MixColor(fill, kAccent, step.editFlash * 0.45f);
    if (step.playing && !step.held) fill = MixColor(fill, kPlayback, 0.12f);
    canvas.FillRect(cell, FadeColor(fill, cellAlpha));
    if (step.held || (copyStepSource && sequence.copySourceIndex == i))
    {
      const auto focus = FadeColor(kAccent, cellAlpha * (0.4f + 0.6f * snapshot.sequencerFocusReveal));
      canvas.StrokeRect(cell, focus);
      canvas.FillRect(MakeRect(cell.x, cell.y, 2, cell.height), focus);
    }
    else if (step.editFlash > 0.05f)
      canvas.StrokeRect(cell, FadeColor(kAccent, cellAlpha * step.editFlash));

    const auto ink = [&](ui::Color color) { return FadeColor(pressed ? kInverseText : color, cellAlpha); };
    DrawInk(canvas, cell.x + 3, cell.y + 1, Index("", i), ink(step.held ? kAccent : step.hasEvents ? kDim : quietNumber));
    if (step.lowestNote >= 0)
    {
      const std::string note = NoteName(step.lowestNote);
      DrawInk(canvas, cell.x + 3, cell.y + 10, note, ink(kText));
      if (step.noteCount > 1)
        canvas.FillRect(MakeRect(cell.x + cell.width - 5, cell.y + 4, 2, 4), ink(kAccent));
    }
    else canvas.FillRect(MakeRect(cell.x + 3, cell.y + 12, 5, 1), ink(kRule));
    if (step.playing)
    {
      const auto playhead = sequence.trackEnabled && !sequence.trackMuted ? kPlayback : kDim;
      const float phase = sequence.patternProgress * static_cast<float>(kSequencerStepCount);
      const int progress = std::clamp(1 + static_cast<int>((phase - std::floor(phase)) * (cell.height - 1)), 1, cell.height);
      // The right edge leaves both rows of the taller note labels untouched.
      canvas.FillRect(MakeRect(cell.x + cell.width - 2, cell.y, 2, cell.height), FadeColor(playhead, cellAlpha * 0.4f));
      canvas.FillRect(MakeRect(cell.x + cell.width - 2, cell.y + cell.height - progress, 2, progress), FadeColor(playhead, cellAlpha));
    }
  }

  const int footerY = body.y + 97;
  if (sequence.hasHeldSteps && sequence.editLane != SequencerEditLane::Note)
  {
    const std::size_t count = std::min(sequence.editValueCount, kHomePerformanceColumnCount);
    for (std::size_t i = 0; i < kHomePerformanceColumnCount; ++i)
    {
      const int x = body.x + static_cast<int>(i) * body.width / 8;
      const int width = body.width / 8 - 2;
      const bool selected = i < count && !sequence.editValueMixed && sequence.editValueIndex == i;
      const std::string value = i >= count ? "-" : sequence.editLane == SequencerEditLane::Length ? kLengthLabels[i] :
          std::to_string(i + 1);
      DrawInk(canvas, x + (width - InkWidth(canvas, value, 1)) / 2, footerY, value, FadeColor(selected ? kAccent : i < count ? kDim : kRule, alpha));
      const int height = sequence.editLane == SequencerEditLane::Velocity ? 1 + static_cast<int>(i) / 3 : 1;
      canvas.FillRect(MakeRect(x, footerY + 10 - height, width, height), FadeColor(selected ? kAccent : kRule, alpha));
    }
  }
  else
  {
    const std::string hint = sequence.copyMode ? OperationHint(sequence) : sequence.clearMode ? "TAP STEP TO CLEAR" :
        sequence.quantizeMode ? "TAP TO QUANTIZE" : sequence.hasHeldSteps ? "KEYS: EDIT NOTES" : "STEP > NOTE";
    DrawInk(canvas, body.x, footerY, TruncateText(canvas, hint, body.width, 1), FadeColor(sequence.hasHeldSteps ? kAccent : kDim, alpha));
  }
}

void DrawPatternSlots(ui::Canvas& canvas, int x, int y, const SequencerUiState& sequence, bool interactive, float alpha)
{
  constexpr int kSlotHeight = 28;
  constexpr int kRowGap = 5;
  const bool copyPatternSource = sequence.copyMode && sequence.hasCopySource && sequence.copySourceIsPattern;
  for (std::size_t i = 0; i < kSequencerMaxPatternCount; ++i)
  {
    const int column = static_cast<int>(i % kHomePerformanceColumnCount);
    const int row = static_cast<int>(i / kHomePerformanceColumnCount);
    const auto slot = MakeRect(x + column * (kKeyColumnWidth + kKeyColumnGap), y + row * (kSlotHeight + kRowGap),
                               kKeyColumnWidth, kSlotHeight);
    const bool available = i < sequence.patternCount;
    const bool current = available && i == sequence.activePattern;
    if (!available)
    {
      const bool addSlot = interactive && i == sequence.patternCount && !sequence.clearMode && !sequence.quantizeMode;
      if (addSlot)
      {
        canvas.StrokeRect(slot, FadeColor(kRule, alpha));
        DrawInk(canvas, slot.x + 5, slot.y + 10, "+", FadeColor(kDim, alpha));
      }
      else
      {
        canvas.FillRect(MakeRect(slot.x + 5, slot.y + slot.height / 2, 5, 1), FadeColor(kRule, alpha));
      }
      continue;
    }

    const bool pressed = sequence.patternPressed[i];
    canvas.FillRect(slot, FadeColor(current || pressed ? kAccent : kSurface, alpha));
    if (copyPatternSource && sequence.copySourceIndex == i) canvas.StrokeRect(slot, FadeColor(kAccent, alpha));
    const std::string number = std::to_string(i + 1);
    const auto ink = FadeColor(current || pressed ? kInverseText : kText, alpha);
    DrawInk(canvas, slot.x + (slot.width - InkWidth(canvas, number, 1) + 1) / 2, slot.y + 9, number, ink);
    if (sequence.patternHasEvents[i])
      canvas.FillRect(MakeRect(slot.x + 6, slot.y + slot.height - 5, 3, 2), FadeColor(current || pressed ? kInverseText : kDim, alpha));
  }
}

void DrawLaneSelector(ui::Canvas& canvas, int x, int y, const SequencerUiState& sequence, float alpha)
{
  const auto count = std::min(sequence.editValueCount, kHomePerformanceColumnCount);
  for (std::size_t i = 0; i < kHomePerformanceColumnCount; ++i)
  {
    const int left = x + static_cast<int>(i) * (kKeyColumnWidth + kKeyColumnGap);
    const bool enabled = i < count;
    const bool selected = enabled && !sequence.editValueMixed && sequence.editValueIndex == i;
    const auto color = FadeColor(selected ? kAccent : enabled ? kDim : kRule, alpha);
    if (sequence.editLane == SequencerEditLane::Velocity)
    {
      // Velocity reads as a ramp: bar height follows the value the column writes.
      const int height = 3 + kVelocityValues[i] * 21 / 127;
      canvas.FillRect(MakeRect(left, y + 24 - height, kKeyColumnWidth, height), selected ? color : FadeColor(kRule, alpha));
      continue;
    }
    canvas.FillRect(MakeRect(left, y, kKeyColumnWidth, 3), selected ? color : FadeColor(kRule, alpha));
    const std::string label = !enabled ? "-" : sequence.editLane == SequencerEditLane::Length ? kLengthLabels[i] : std::to_string(i + 1);
    DrawInk(canvas, left + (kKeyColumnWidth - InkWidth(canvas, label, 1) + 1) / 2, y + 10, label, color);
  }
}

void DrawStepEditPage(ui::Canvas& canvas, const ui::Rect& body, const UiScreenSnapshot& snapshot, float alpha)
{
  const auto& sequence = snapshot.sequencer;
  if (sequence.editLane == SequencerEditLane::Note)
  {
    const std::string notes = HeldStepDetail(sequence);
    const int scale = FitScale(canvas, notes, body.width, 2);
    DrawMarqueeTextInRect(canvas, MakeRect(body.x, body.y + 16, body.width, InkHeight(scale)), notes,
                          FadeColor(sequence.noteNames.empty() ? kDim : kText, alpha), scale);
    return;
  }
  DrawInk(canvas, body.x, body.y + 12, TruncateText(canvas, sequence.valueLabel, body.width, 2), FadeColor(kText, alpha), 2);
  DrawLaneSelector(canvas, KeyGridX(body), body.y + 52, sequence, alpha);
}
}  // namespace

bool DrawSequencerHomeContent(ui::Canvas& canvas, const ui::Rect& bounds, const UiScreenSnapshot& snapshot, float alpha)
{
  const auto& sequence = snapshot.sequencer;
  const auto body = HomeBody(bounds);
  if (sequence.patternSelectorVisible)
  {
    DrawHomeTitle(canvas, snapshot, true, alpha);
    DrawPatternSlots(canvas, KeyGridX(body), body.y + 8, sequence, true, alpha);
    DrawInk(canvas, body.x, body.y + 80, "HOLD +/- : PATTERNS", FadeColor(kDim, alpha));
    DrawInk(canvas, body.x, body.y + 94, sequence.copyMode ? OperationHint(sequence) : sequence.clearMode ? "SELECT TO DELETE" :
        sequence.quantizeMode ? "SELECT TO QUANTIZE" : "TAP +: ADD PATTERN", FadeColor(kDim, alpha));
    return true;
  }
  if (snapshot.showSequencerGrid || sequence.shiftActive)
  {
    DrawHomeTitle(canvas, snapshot, true, alpha);
    DrawStepGrid(canvas, body, snapshot, alpha);
    return true;
  }
  if (!sequence.hasHeldSteps) return false;
  DrawHomeTitle(canvas, snapshot, false, alpha);
  DrawStepEditPage(canvas, body, snapshot, alpha);
  return true;
}

bool DrawSequencerContextContent(ui::Canvas& canvas, const ui::Rect& rect, const UiScreenSnapshot& snapshot, float alpha)
{
  const auto& sequence = snapshot.sequencer;
  if (snapshot.focusItemId == "sys.seq.track")
  {
    const int top = DrawPageHeader(canvas, rect, "TRACKS", snapshot.headerSecondary, kAccent, kDim, alpha);
    for (std::size_t i = 0; i < sequence.tracks.size(); ++i)
    {
      const auto& track = sequence.tracks[i];
      const std::string label = "T" + std::to_string(i + 1);
      const bool selected = label == snapshot.headerSecondary;
      const int y = top + 6 + static_cast<int>(i) * 15;
      if (selected) canvas.FillRect(MakeRect(rect.x - 3, y - 3, rect.width + 6, 13), FadeColor(kSelection, alpha));
      DrawInk(canvas, rect.x, y, label, FadeColor(selected ? kAccent : kDim, alpha));
      DrawInk(canvas, rect.x + 28, y, track.muted ? "MUTED" : "OUT", FadeColor(track.muted ? kDim : kText, alpha));
      DrawInk(canvas, rect.x + 70, y, track.enabled ? "SEQ ON" : "SEQ OFF", FadeColor(track.enabled ? kText : kDim, alpha));
    }
    return true;
  }
  if (snapshot.focusItemId == "sys.seq.pattern")
  {
    const int top = DrawPageHeader(canvas, rect, "PATTERN",
                                   std::to_string(sequence.activePattern + 1) + "/" + std::to_string(sequence.patternCount), kAccent, kDim, alpha);
    DrawPatternSlots(canvas, rect.x, top + 6, sequence, false, alpha);
    DrawInk(canvas, rect.x, top + 74, std::to_string(sequence.populatedStepCount) + "/16 STEPS", FadeColor(kDim, alpha));
    return true;
  }
  return false;
}
}  // namespace synth::ui_render
