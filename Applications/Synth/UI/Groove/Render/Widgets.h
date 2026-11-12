#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "Applications/Synth/UI/Groove/Render/Gfx.h"

namespace groove::widgets {

// Rect of GRID cell (row, column), or a span of cells.
Rect Cell(std::size_t row, std::size_t column);
Rect CellSpan(std::size_t row, std::size_t column, std::size_t rows, std::size_t columns);

enum class CellStyle : std::uint8_t { Normal, Focused, Disabled };

struct ParamCellLayout {
  int paddingX = 3;
  int rightPadding = 2;
  int labelY = 2;
  int labelHeight = 10;
  int valueY = 12;
  int barBottom = 2;
  int barHeight = 2;
  Font labelFont = Font::Micro;
  Font valueFont = Font::Small;
  bool dottedOutline = false;
};
// Different display sizes use the original cell painter with explicit metrics.
// Focus and availability are independent, including a focused unavailable slot.
void ParamCell(Gfx& g, const Rect& rect, std::string_view label, std::string_view value, float fraction,
               bool focused, bool disabled, const ParamCellLayout& layout);
void ParamCell(Gfx& g, const Rect& rect, std::string_view label, std::string_view value, float fraction,
               bool focused, bool disabled, const ParamCellLayout& layout, float flash, std::uint32_t now, bool ants = true);
void DottedFrame(Gfx& g, const Rect& rect, Color color);

// Parameter cell: label, value text and a dithered level bar for `fraction` (negative = no bar).
void ParamCell(Gfx& g, const Rect& rect, std::string_view label, std::string_view value, float fraction, CellStyle style);
// Animated variant: `flash` (1 -> 0) pops the value after an edit; a focused cell gets marching ants.
void ParamCell(Gfx& g, const Rect& rect, std::string_view label, std::string_view value, float fraction, CellStyle style,
               float flash, std::uint32_t now, bool ants = true);
// Action cell: label with an arrow, dotted outline.
void ActionCell(Gfx& g, const Rect& rect, std::string_view label, bool pressed, Color accent);
struct ActionCellLayout {
  Font labelFont = Font::Micro;
  Font detailFont = Font::Micro;
  int padding = 4;
  int labelY = -1;
  int detailBottom = 0;
  int focusBandHeight = 0;
  bool strongOutline = false;
};
void ActionCell(Gfx& g, const Rect& rect, std::string_view label, std::string_view detail,
                bool focused, bool pressed, bool available, Color accent, const ActionCellLayout& layout,
                float flash = 0.0f, std::uint32_t now = 0);
// Horizontal dithered level bar.
void LevelBar(Gfx& g, const Rect& rect, float fraction, Color on, Color off);
// Vertical dithered fader.
void Fader(Gfx& g, const Rect& rect, float fraction, Color on, bool focused);
// Inverse "badge" text, e.g. MUTE / SOLO.
void Badge(Gfx& g, const Rect& rect, std::string_view text, bool active, Color activeColor);

// Graphs. All values are normalized 0..1.
void Envelope(Gfx& g, const Rect& rect, const std::array<float, 5>& times, const std::array<float, 5>& levels, Color color);
void FilterCurve(Gfx& g, const Rect& rect, float cutoff, float resonance, Color color);
// A negative `pulseWidth` leaves the pulse wave out.
void OscillatorScope(Gfx& g, const Rect& rect, bool saw, float pulseWidth, float sub, float noise, float lfoRate, Color color,
                     float phase = 0.0f);
void Waveform(Gfx& g, const Rect& rect, const float* bins, std::size_t count, float start, float end, float cursor, bool cursorVisible);
// With `activity` > 0 signal dots flow along the routing toward the carriers.
// Scale 1 preserves the compact screen; larger nodes use a centred tree layout.
void Dx7Algorithm(Gfx& g, const Rect& rect, int algorithm, int highlightOperator, int feedback, float activity = 0.0f,
                  std::uint32_t now = 0, int scale = 1);

// True when `op` (1..6) is a carrier in DX7 `algorithm` (1..32).
bool Dx7IsCarrier(int algorithm, int op);
// LFO shape: DX7 wave order (0 TRI, 1 SAW DOWN, 2 SAW UP, 3 SQUARE, 4 SINE, 5 S/H). `cycles` across the
// rect, `delay` (0..1 of the width) fades the depth in, `phase` scrolls it.
void LfoShape(Gfx& g, const Rect& rect, int wave, float cycles, float delay, float depth, float phase, Color color);
// Bipolar bar: centred tick, filled from the centre toward `value` (-1..1).
void BipolarBar(Gfx& g, const Rect& rect, float value, Color on, Color off);

// Horizontal scrollbar / list position indicator.
void ScrollIndicator(Gfx& g, const Rect& rect, std::size_t first, std::size_t visible, std::size_t total);

}  // namespace groove::widgets
