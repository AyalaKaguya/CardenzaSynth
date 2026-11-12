#include "Applications/Synth/UI/Groove/Render/Widgets.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>

#include "Applications/Synth/UI/Groove/Render/Effects.h"
#include "Applications/Synth/UI/Groove/Render/Theme.h"

namespace groove::widgets {

namespace {

using namespace groove::theme;

constexpr std::array<std::array<std::uint8_t, 4>, 4> kBayer = {{
    {{0, 8, 2, 10}},
    {{12, 4, 14, 6}},
    {{3, 11, 1, 9}},
    {{15, 7, 13, 5}},
}};

constexpr int kGridOffsetX = kContentX + (kContentWidth - (4 * kCellWidth + 3 * kCellGap)) / 2;
constexpr int kGridOffsetY = kContentY + (kContentHeight - (4 * kCellHeight + 3 * kCellGap)) / 2;

int Clamp(int value, int low, int high) { return std::max(low, std::min(high, value)); }
float Clamp01(float value) { return std::max(0.0f, std::min(1.0f, value)); }

// Fills each column from its curve height down to the rect bottom with an ordered dither.
template <typename HeightAt>
void FillUnder(Gfx& g, const Rect& rect, HeightAt heightAt, Color color, int level, Color line)
{
  int previous = -1;
  for (int column = 0; column < rect.width; ++column)
  {
    const int height = Clamp(heightAt(column), 0, rect.height - 1);
    const int x = rect.x + column;
    const int top = rect.y + rect.height - 1 - height;
    for (int y = top + 1; y < rect.y + rect.height; ++y)
      if (kBayer[static_cast<std::size_t>(y & 3)][static_cast<std::size_t>(x & 3)] < level) g.Pixel(x, y, color);
    if (previous >= 0) g.Line(x - 1, previous, x, top, line);
    else g.Pixel(x, top, line);
    previous = top;
  }
}

}  // namespace

Rect Cell(std::size_t row, std::size_t column)
{
  return Rect{kGridOffsetX + static_cast<int>(column) * (kCellWidth + kCellGap),
              kGridOffsetY + static_cast<int>(row) * (kCellHeight + kCellGap), kCellWidth, kCellHeight};
}

Rect CellSpan(std::size_t row, std::size_t column, std::size_t rows, std::size_t columns)
{
  const Rect first = Cell(row, column);
  return Rect{first.x, first.y,
              static_cast<int>(columns) * kCellWidth + static_cast<int>(columns - 1) * kCellGap,
              static_cast<int>(rows) * kCellHeight + static_cast<int>(rows - 1) * kCellGap};
}

void ParamCell(Gfx& g, const Rect& rect, std::string_view label, std::string_view value, float fraction, CellStyle style)
{
  ParamCell(g, rect, label, value, fraction, style == CellStyle::Focused, style == CellStyle::Disabled, {});
}

void ParamCell(Gfx& g, const Rect& rect, std::string_view label, std::string_view value, float fraction,
               bool focused, bool disabled, const ParamCellLayout& layout)
{
  const int textX = rect.x + layout.paddingX;
  const int textWidth = rect.width - layout.paddingX - layout.rightPadding;
  if (layout.dottedOutline) DottedFrame(g, rect, disabled ? kFaint : kDim);
  if (focused)
  {
    g.Fill(Rect{rect.x, rect.y, rect.width, layout.labelHeight}, kInk);
    g.TextFit(textX, rect.y + layout.labelY, textWidth, label, kBlack, layout.labelFont);
    g.VLine(rect.x, rect.y + layout.labelHeight, rect.height - layout.labelHeight, kInk);
  }
  else
  {
    g.TextFit(textX, rect.y + layout.labelY, textWidth, label, disabled ? kFaint : kDim, layout.labelFont);
  }
  g.TextFit(textX, rect.y + layout.valueY, textWidth, value, disabled ? kFaint : kInk, layout.valueFont);
  if (fraction >= 0.0f)
  {
    LevelBar(g, Rect{textX, rect.y + rect.height - layout.barBottom, textWidth, layout.barHeight}, fraction,
             disabled ? kFaint : kInk, kFaint);
  }
}

void ParamCell(Gfx& g, const Rect& rect, std::string_view label, std::string_view value, float fraction, CellStyle style,
               float flash, std::uint32_t now, bool ants)
{
  ParamCell(g, rect, label, value, fraction, style == CellStyle::Focused, style == CellStyle::Disabled, {}, flash, now, ants);
}

void ParamCell(Gfx& g, const Rect& rect, std::string_view label, std::string_view value, float fraction,
               bool focused, bool disabled, const ParamCellLayout& layout, float flash, std::uint32_t now, bool ants)
{
  ParamCell(g, rect, label, value, fraction, focused, disabled, layout);
  if (ants && focused) fx::MarchingAnts(g, Rect{rect.x - 1, rect.y - 1, rect.width + 2, rect.height + 2}, kInk, now);
  if (flash <= 0.0f) return;
  // Fresh edit: the value pops inverse, then an underline glow fades out.
  const int valueHeight = Gfx::CapHeight(layout.valueFont) + 2;
  const Rect valueBox{rect.x + 1, rect.y + layout.valueY - 1,
      std::min(rect.width - 2, Gfx::Measure(value, layout.valueFont) + 4), valueHeight};
  if (flash > 0.62f)
  {
    g.Fill(valueBox, kInk);
    g.TextFit(rect.x + layout.paddingX, rect.y + layout.valueY,
        rect.width - layout.paddingX - layout.rightPadding, value, kBlack, layout.valueFont);
  }
  else g.Dither(Rect{valueBox.x, valueBox.y + valueBox.height - 1, valueBox.width, 2}, kInk, static_cast<int>(flash * 20.0f));
}

void DottedFrame(Gfx& g, const Rect& rect, Color color)
{
  g.DottedHLine(rect.x, rect.y, rect.width, color);
  g.DottedHLine(rect.x, rect.y + rect.height - 1, rect.width, color);
  g.DottedVLine(rect.x, rect.y, rect.height, color);
  g.DottedVLine(rect.x + rect.width - 1, rect.y, rect.height, color);
}

void ActionCell(Gfx& g, const Rect& rect, std::string_view label, bool pressed, Color accent)
{
  ActionCell(g, rect, label, {}, false, pressed, true, accent, {});
}

void ActionCell(Gfx& g, const Rect& rect, std::string_view label, std::string_view detail,
                bool focused, bool pressed, bool available, Color accent, const ActionCellLayout& layout,
                float flash, std::uint32_t now)
{
  const int y = layout.labelY >= 0 ? rect.y + layout.labelY : rect.y + (rect.height - 7) / 2;
  if (pressed)
  {
    g.Fill(rect, accent);
    g.TextFit(rect.x + layout.padding, y, rect.width - layout.padding * 2, label, kBlack, layout.labelFont);
    if (layout.detailBottom > 0 && !detail.empty())
      g.TextFit(rect.x + layout.padding, rect.y + rect.height - layout.detailBottom,
          rect.width - layout.padding * 2, detail, kBlack, layout.detailFont);
    return;
  }
  DottedFrame(g, rect, layout.strongOutline ? (available ? kDim : kFaint) : (available ? kFaint : kShade));
  const bool band = focused && layout.focusBandHeight > 0;
  if (band)
  {
    g.Fill(Rect{rect.x, rect.y, rect.width, layout.focusBandHeight}, kInk);
    g.VLine(rect.x, rect.y + layout.focusBandHeight, rect.height - layout.focusBandHeight, kInk);
  }
  if (layout.detailBottom > 0)
  {
    g.TextFit(rect.x + layout.padding, y, rect.width - layout.padding * 2, label,
        band ? kBlack : available ? kInk : kFaint, layout.labelFont);
    g.TextFit(rect.x + layout.padding, rect.y + rect.height - layout.detailBottom, rect.width - layout.padding * 2 - 7,
        detail, available ? kDim : kFaint, layout.detailFont);
    if (available) g.Text(rect.x + rect.width - 11, rect.y + rect.height - layout.detailBottom + 1, ">", accent);
    if (flash > 0.0f) fx::Ripple(g, rect, 1.0f - flash, accent);
    (void)now;
    return;
  }
  // Long labels keep their text and drop the arrow.
  if (Gfx::Measure(label, layout.labelFont) > rect.width - 14)
  {
    g.TextFit(rect.x + layout.padding, y, rect.width - 6, label, available ? kInk : kFaint, layout.labelFont);
    return;
  }
  g.Text(rect.x + layout.padding, y, label, available ? kInk : kFaint, layout.labelFont);
  g.Text(rect.x + rect.width - 9, y, ">", accent);
}

void LevelBar(Gfx& g, const Rect& rect, float fraction, Color on, Color off)
{
  const int filled = static_cast<int>(std::lround(Clamp01(fraction) * static_cast<float>(rect.width)));
  g.Dither(Rect{rect.x, rect.y, filled, rect.height}, on, kDitherLevelOn);
  g.Dither(Rect{rect.x + filled, rect.y, rect.width - filled, rect.height}, off, 4);
}

void Fader(Gfx& g, const Rect& rect, float fraction, Color on, bool focused)
{
  const int filled = static_cast<int>(std::lround(Clamp01(fraction) * static_cast<float>(rect.height - 2)));
  const Rect inner{rect.x + 1, rect.y + 1, rect.width - 2, rect.height - 2};
  g.Dither(Rect{inner.x, inner.y, inner.width, inner.height - filled}, kFaint, kDitherLevelOff);
  g.Dither(Rect{inner.x, inner.y + inner.height - filled, inner.width, filled}, on, focused ? kDitherSolid : kDitherLevelOn);
  if (focused) g.Stroke(rect, kInk);
  else
  {
    g.DottedVLine(rect.x, rect.y, rect.height, kFaint);
    g.DottedVLine(rect.x + rect.width - 1, rect.y, rect.height, kFaint);
  }
  // Unity marker at 100 %.
  g.HLine(rect.x - 2, rect.y, 2, kDim);
}

void Badge(Gfx& g, const Rect& rect, std::string_view text, bool active, Color activeColor)
{
  if (active)
  {
    g.Fill(rect, activeColor);
    g.TextAligned(rect, text, kBlack, Font::Micro, Align::Center);
    return;
  }
  g.DottedHLine(rect.x, rect.y, rect.width, kFaint);
  g.DottedHLine(rect.x, rect.y + rect.height - 1, rect.width, kFaint);
  g.TextAligned(rect, text, kDim, Font::Micro, Align::Center);
}

void Envelope(Gfx& g, const Rect& rect, const std::array<float, 5>& times, const std::array<float, 5>& levels, Color color)
{
  float total = 0.0f;
  for (const float time : times) total += std::max(0.02f, time);
  std::array<float, 6> x{};
  std::array<float, 6> y{};
  x[0] = 0.0f;
  y[0] = 0.0f;
  for (std::size_t index = 0; index < times.size(); ++index)
  {
    x[index + 1] = x[index] + std::max(0.02f, times[index]) / total;
    y[index + 1] = Clamp01(levels[index]);
  }
  const int width = rect.width;
  FillUnder(g, rect, [&](int column) {
    const float position = static_cast<float>(column) / static_cast<float>(std::max(1, width - 1));
    std::size_t segment = 0;
    while (segment + 1 < x.size() - 1 && position > x[segment + 1]) ++segment;
    const float span = std::max(0.0001f, x[segment + 1] - x[segment]);
    const float t = Clamp01((position - x[segment]) / span);
    const float level = y[segment] + (y[segment + 1] - y[segment]) * t;
    return static_cast<int>(level * static_cast<float>(rect.height - 2));
  }, color, kDitherFill, kInk);
  // Segment joints as ticks on the baseline.
  for (std::size_t index = 1; index < x.size() - 1; ++index)
    g.VLine(rect.x + static_cast<int>(x[index] * static_cast<float>(width - 1)), rect.y + rect.height, 2, kDim);
}

void FilterCurve(Gfx& g, const Rect& rect, float cutoff, float resonance, Color color)
{
  const float c = 0.08f + Clamp01(cutoff) * 0.84f;
  const float q = Clamp01(resonance);
  const int width = rect.width;
  FillUnder(g, rect, [&](int column) {
    const float f = static_cast<float>(column) / static_cast<float>(std::max(1, width - 1));
    const float ratio = f / c;
    float response = 1.0f / std::sqrt(1.0f + ratio * ratio * ratio * ratio * ratio * ratio);
    const float distance = (f - c) / 0.06f;
    response += q * 0.9f * std::exp(-distance * distance);
    return static_cast<int>(Clamp01(response * 0.55f) * static_cast<float>(rect.height - 2));
  }, color, kDitherFill, kInk);
  const int cx = rect.x + static_cast<int>(c * static_cast<float>(width - 1));
  g.DottedVLine(cx, rect.y, rect.height, kDim, 2);
}

void OscillatorScope(Gfx& g, const Rect& rect, bool saw, float pulseWidth, float sub, float noise, float lfoRate, Color color,
                     float phaseOffset)
{
  const int mid = rect.y + rect.height / 2;
  g.DottedHLine(rect.x, mid, rect.width, kFaint, 3);
  const float cycles = 2.0f;
  int previousY = mid;
  for (int column = 0; column < rect.width; ++column)
  {
    const float position = static_cast<float>(column) / static_cast<float>(rect.width) + phaseOffset;
    const float phase = std::fmod(position * cycles, 1.0f);
    const float subPhase = std::fmod(position * cycles * 0.5f, 1.0f);
    float value = 0.0f;
    if (saw) value += (1.0f - 2.0f * phase) * 0.55f;
    if (pulseWidth >= 0.0f) value += (phase < 0.15f + 0.7f * Clamp01(pulseWidth) ? 0.45f : -0.45f);
    value += Clamp01(sub) * (subPhase < 0.5f ? 0.35f : -0.35f);
    const std::uint32_t hash = static_cast<std::uint32_t>(column + static_cast<int>(phaseOffset * 977.0f)) * 2654435761u;
    value += Clamp01(noise) * (static_cast<float>(hash >> 24) / 255.0f - 0.5f) * 0.6f;
    value = std::max(-1.0f, std::min(1.0f, value / 1.4f));
    const int y = mid - static_cast<int>(value * static_cast<float>(rect.height / 2 - 2));
    if (column > 0) g.Line(rect.x + column - 1, previousY, rect.x + column, y, color);
    previousY = y;
  }
  // LFO as a slow dotted sine across the scope.
  const float lfoCycles = 0.5f + Clamp01(lfoRate) * 3.5f;
  for (int column = 0; column < rect.width; column += 3)
  {
    const float value = std::sin((static_cast<float>(column) / static_cast<float>(rect.width) + phaseOffset * 0.25f) * lfoCycles * 6.2831853f);
    g.Pixel(rect.x + column, mid - static_cast<int>(value * static_cast<float>(rect.height / 2 - 1)), kDim);
  }
}

void Waveform(Gfx& g, const Rect& rect, const float* bins, std::size_t count, float start, float end, float cursor, bool cursorVisible)
{
  const int mid = rect.y + rect.height / 2;
  g.DottedHLine(rect.x, mid, rect.width, kFaint, 3);
  if (bins == nullptr || count == 0) return;
  const int startX = rect.x + static_cast<int>(Clamp01(start) * static_cast<float>(rect.width - 1));
  const int endX = rect.x + static_cast<int>(Clamp01(end) * static_cast<float>(rect.width - 1));
  for (int column = 0; column < rect.width; ++column)
  {
    const std::size_t bin = std::min(count - 1, static_cast<std::size_t>(column) * count / static_cast<std::size_t>(rect.width));
    const int half = static_cast<int>(Clamp01(bins[bin]) * static_cast<float>(rect.height / 2 - 1));
    const int x = rect.x + column;
    const bool inside = x >= startX && x <= endX;
    g.Dither(Rect{x, mid - half, 1, half * 2 + 1}, inside ? kInk : kFaint, inside ? kDitherLevelOn : 8);
  }
  g.VLine(startX, rect.y, rect.height, kInk);
  g.Fill(Rect{startX, rect.y, 3, 3}, kInk);
  g.VLine(endX, rect.y, rect.height, kInk);
  g.Fill(Rect{endX - 2, rect.y + rect.height - 3, 3, 3}, kInk);
  if (cursorVisible)
  {
    const int cx = rect.x + static_cast<int>(Clamp01(cursor) * static_cast<float>(rect.width - 1));
    g.VLine(cx, rect.y, rect.height, kPlay);
  }
}

namespace {

struct AlgorithmRouting {
  const char* edges;    // pairs "TM": operator M modulates operator T (T == M is feedback)
  const char* carriers;
};

// The 32 DX7 routings, carried over from the previous renderer's verified table.
constexpr std::array<AlgorithmRouting, 32> kAlgorithms{{
    {"1266344556", "13"}, {"1222345645", "13"}, {"1245662356", "14"}, {"1245462356", "14"},
    {"12345666", "135"},  {"12345665", "135"},  {"1234563566", "13"}, {"1234563544", "13"},
    {"1234563522", "13"}, {"1223334546", "14"}, {"1223664546", "14"}, {"1222343536", "13"},
    {"1266343536", "13"}, {"6612344546", "13"}, {"2212344546", "13"}, {"661213153456", "1"},
    {"221213153456", "1"}, {"121314335645", "1"}, {"1223664656", "145"}, {"3323454613", "124"},
    {"3313234656", "1245"}, {"1236465666", "1345"}, {"6613234656", "1245"}, {"36465666", "12345"},
    {"465666", "12345"},  {"66234546", "124"},  {"33234546", "124"},  {"12344555", "136"},
    {"345666", "1235"},   {"344555", "1236"},   {"5666", "12345"},    {"66", "123456"},
}};

}  // namespace

bool Dx7IsCarrier(int algorithm, int op)
{
  const auto& algo = kAlgorithms[static_cast<std::size_t>(Clamp(algorithm, 1, 32) - 1)];
  for (const char* c = algo.carriers; *c != 0; ++c)
    if (*c - '0' == op) return true;
  return false;
}

void LfoShape(Gfx& g, const Rect& rect, int wave, float cycles, float delay, float depth, float phase, Color color)
{
  const int mid = rect.y + rect.height / 2;
  const float half = static_cast<float>(rect.height / 2 - 1);
  g.DottedHLine(rect.x, mid, rect.width, kFaint, 3);
  const int delayX = static_cast<int>(Clamp01(delay) * static_cast<float>(rect.width));
  if (delayX > 0) g.DottedVLine(rect.x + delayX, rect.y, rect.height, kFaint, 2);
  int previousY = mid;
  for (int column = 0; column < rect.width; ++column)
  {
    const float position = static_cast<float>(column) / static_cast<float>(std::max(1, rect.width - 1));
    const float cycle = position * cycles + phase;
    const float p = cycle - std::floor(cycle);
    float value = 0.0f;
    switch (wave)
    {
      case 1: value = 1.0f - 2.0f * p; break;                              // saw down
      case 2: value = 2.0f * p - 1.0f; break;                              // saw up
      case 3: value = p < 0.5f ? 1.0f : -1.0f; break;                      // square
      case 4: value = std::sin(p * 6.2831853f); break;                     // sine
      case 5:                                                              // sample and hold
      {
        const std::uint32_t step = static_cast<std::uint32_t>(std::floor(cycle) + 1000.0f);
        value = static_cast<float>((step * 2654435761u) >> 24) / 127.5f - 1.0f;
        break;
      }
      case 0:
      default: value = p < 0.5f ? 4.0f * p - 1.0f : 3.0f - 4.0f * p; break;  // triangle
    }
    // The delay ramps the depth in over its span.
    const float ramp = delayX > 0 ? Clamp01(static_cast<float>(column) / static_cast<float>(delayX)) : 1.0f;
    const int y = mid - static_cast<int>(value * half * Clamp01(depth) * ramp);
    if (column > 0) g.Line(rect.x + column - 1, previousY, rect.x + column, y, color);
    previousY = y;
  }
}

void BipolarBar(Gfx& g, const Rect& rect, float value, Color on, Color off)
{
  const int centre = rect.x + rect.width / 2;
  g.Dither(rect, off, 4);
  const int extent = static_cast<int>(std::lround(std::max(-1.0f, std::min(1.0f, value)) * static_cast<float>(rect.width / 2)));
  if (extent > 0) g.Dither(Rect{centre, rect.y, extent, rect.height}, on, kDitherLevelOn);
  else if (extent < 0) g.Dither(Rect{centre + extent, rect.y, -extent, rect.height}, on, kDitherLevelOn);
  g.VLine(centre, rect.y - 1, rect.height + 2, kDim);
}

void Dx7Algorithm(Gfx& g, const Rect& rect, int algorithm, int highlightOperator, int feedback, float activity, std::uint32_t now, int scale)
{
  scale = Clamp(scale, 1, 3);
  const auto& algo = kAlgorithms[static_cast<std::size_t>(Clamp(algorithm, 1, 32) - 1)];
  std::array<bool, 7> carrier{};
  for (const char* c = algo.carriers; *c != 0; ++c) carrier[static_cast<std::size_t>(*c - '0')] = true;
  struct Edge { int target; int modulator; };
  std::array<Edge, 8> edges{};
  std::size_t edgeCount = 0;
  std::array<bool, 7> selfFeedback{};
  for (std::size_t index = 0; algo.edges[index] != 0 && algo.edges[index + 1] != 0 && edgeCount < edges.size(); index += 2)
  {
    const int target = algo.edges[index] - '0';
    const int modulator = algo.edges[index + 1] - '0';
    if (target == modulator) selfFeedback[static_cast<std::size_t>(target)] = true;
    else edges[edgeCount++] = {target, modulator};
  }

  // Stack depth: carriers sit on the output bus, each modulator one level above its deepest target.
  std::array<int, 7> level{};
  std::array<int, 7> parent{};
  for (int pass = 0; pass < 6; ++pass)
    for (std::size_t index = 0; index < edgeCount; ++index)
    {
      const auto [target, modulator] = edges[index];
      if (carrier[static_cast<std::size_t>(modulator)]) continue;
      const int candidate = std::min(5, level[static_cast<std::size_t>(target)] + 1);
      if (candidate > level[static_cast<std::size_t>(modulator)])
      {
        level[static_cast<std::size_t>(modulator)] = candidate;
        parent[static_cast<std::size_t>(modulator)] = target;
      }
    }
  for (int op = 1; op <= 6; ++op)
    if (!carrier[static_cast<std::size_t>(op)] && parent[static_cast<std::size_t>(op)] == 0)
      for (std::size_t index = 0; index < edgeCount; ++index)
        if (edges[index].modulator == op) { parent[static_cast<std::size_t>(op)] = edges[index].target; break; }

  // Leaf-width tree layout, carriers left to right.
  std::array<float, 7> slot{};
  float cursor = 0.0f;
  int maxLevel = 0;
  auto layout = [&](auto&& self, int op) -> void {
    maxLevel = std::max(maxLevel, level[static_cast<std::size_t>(op)]);
    const float begin = cursor;
    bool hasChild = false;
    for (int child = 1; child <= 6; ++child)
      if (!carrier[static_cast<std::size_t>(child)] && parent[static_cast<std::size_t>(child)] == op && child != op)
      {
        hasChild = true;
        self(self, child);
      }
    if (!hasChild) cursor += 1.0f;
    slot[static_cast<std::size_t>(op)] = (begin + cursor - 1.0f) / 2.0f;
  };
  for (int op = 1; op <= 6; ++op)
    if (carrier[static_cast<std::size_t>(op)]) layout(layout, op);

  const int boxWidth = 11 * scale;
  const int boxHeight = 9 * scale;
  const float columns = std::max(1.0f, cursor);
  const int rowStep = scale == 1 ? std::min(boxHeight + 6, (rect.height - boxHeight - 4) / std::max(1, maxLevel)) :
      std::min(22 * scale, std::max(1, (rect.height - boxHeight - 10 * scale) / std::max(1, maxLevel)));
  const int treeHeight = boxHeight + maxLevel * rowStep;
  const int bottom = scale == 1 ? rect.y + rect.height - 4 - boxHeight :
      rect.y + (rect.height - treeHeight - 6 * scale) / 2 + maxLevel * rowStep;
  const int busY = scale == 1 ? rect.y + rect.height - 2 : bottom + boxHeight + 4 * scale;
  const Font numberFont = scale >= 3 ? Font::Huge : scale == 2 ? Font::Large : Font::Micro;
  auto boxRect = [&](int op) {
    const float unit = static_cast<float>(rect.width) / columns;
    const int cx = rect.x + static_cast<int>((slot[static_cast<std::size_t>(op)] + 0.5f) * unit);
    return Rect{cx - boxWidth / 2, bottom - level[static_cast<std::size_t>(op)] * rowStep, boxWidth, boxHeight};
  };

  // Output bus under the carriers.
  int busLeft = rect.x + rect.width;
  int busRight = rect.x;
  for (int op = 1; op <= 6; ++op)
  {
    if (!carrier[static_cast<std::size_t>(op)]) continue;
    const Rect box = boxRect(op);
    busLeft = std::min(busLeft, box.x + box.width / 2);
    busRight = std::max(busRight, box.x + box.width / 2);
    g.VLine(box.x + box.width / 2, box.y + box.height, busY - (box.y + box.height), kDim);
  }
  if (busRight >= busLeft) g.HLine(busLeft, busY, busRight - busLeft + 1, activity > 0.02f ? kTab : kDim);

  for (std::size_t index = 0; index < edgeCount; ++index)
  {
    const Rect from = boxRect(edges[index].modulator);
    const Rect to = boxRect(edges[index].target);
    const bool treeEdge = parent[static_cast<std::size_t>(edges[index].modulator)] == edges[index].target;
    if (treeEdge)
    {
      const int x0 = from.x + from.width / 2, y0 = from.y + from.height, x1 = to.x + to.width / 2, y1 = to.y - 1;
      g.Line(x0, y0, x1, y1, kDim);
      if (activity > 0.02f)
      {
        // Signal flowing from modulator to target while the track sounds.
        const float t = std::fmod(static_cast<float>(now) / 420.0f + static_cast<float>(index) * 0.37f, 1.0f);
        const int dx = x0 + static_cast<int>(static_cast<float>(x1 - x0) * t);
        const int dy = y0 + static_cast<int>(static_cast<float>(y1 - y0) * t);
        g.Fill(Rect{dx - scale, dy - scale, 2 * scale, 2 * scale}, kTab);
      }
    }
    else
    {
      // Cross routing (shared modulators, loop feedback): dotted side path.
      const int sideX = std::max(from.x, to.x) + boxWidth + 2 * scale;
      g.DottedHLine(from.x + boxWidth, from.y + boxHeight / 2, sideX - from.x - boxWidth, kDim);
      g.DottedVLine(sideX, std::min(from.y, to.y) + boxHeight / 2, std::abs(to.y - from.y), kDim);
      g.DottedHLine(to.x + boxWidth, to.y + boxHeight / 2, sideX - to.x - boxWidth, kDim);
    }
  }

  for (int op = 1; op <= 6; ++op)
  {
    const Rect box = boxRect(op);
    const bool isCarrier = carrier[static_cast<std::size_t>(op)];
    const bool highlighted = op == highlightOperator;
    const char digit[2] = {static_cast<char>('0' + op), 0};
    if (isCarrier && activity > 0.02f)
      g.Dither(Rect{box.x - 2 * scale, box.y - 2 * scale, box.width + 4 * scale, box.height + 4 * scale}, kTab,
               static_cast<int>(std::min(1.0f, activity) * 10.0f));
    if (isCarrier)
    {
      g.Fill(box, highlighted ? kTab : kInk);
      g.Text(scale == 1 ? box.x + 3 : box.x + (box.width - Gfx::Measure(digit, numberFont)) / 2,
             scale == 1 ? box.y + 1 : box.y + (box.height - Gfx::CapHeight(numberFont)) / 2, digit, kBlack, numberFont);
    }
    else
    {
      g.Fill(box, kBlack);
      g.Stroke(box, highlighted ? kTab : kDim);
      g.Text(scale == 1 ? box.x + 3 : box.x + (box.width - Gfx::Measure(digit, numberFont)) / 2,
             scale == 1 ? box.y + 1 : box.y + (box.height - Gfx::CapHeight(numberFont)) / 2, digit, highlighted ? kTab : kInk, numberFont);
    }
    if (selfFeedback[static_cast<std::size_t>(op)])
    {
      // Feedback loop: a small hook over the top-right corner, brighter with more feedback.
      const Color loop = feedback > 0 ? kInk : kFaint;
      g.HLine(box.x + box.width / 2, box.y - 2 * scale, box.width / 2 + 3 * scale, loop);
      g.VLine(box.x + box.width + 2 * scale, box.y - 2 * scale, box.height / 2 + 2 * scale, loop);
      g.HLine(box.x + box.width, box.y + box.height / 2, 2 * scale, loop);
    }
  }
}

void ScrollIndicator(Gfx& g, const Rect& rect, std::size_t first, std::size_t visible, std::size_t total)
{
  g.DottedVLine(rect.x, rect.y, rect.height, kFaint);
  if (total <= visible || total == 0) return;
  const int thumb = std::max(4, static_cast<int>(rect.height * static_cast<int>(visible) / static_cast<int>(total)));
  const int travel = rect.height - thumb;
  const int top = rect.y + static_cast<int>(travel * static_cast<int>(first) / static_cast<int>(total - visible));
  g.Fill(Rect{rect.x - 1, top, 3, thumb}, kInk);
}

}  // namespace groove::widgets
