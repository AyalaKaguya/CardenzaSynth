#include "Applications/Synth/UI/Groove/Render/Effects.h"

#include <algorithm>
#include <cmath>

#include "Applications/Synth/UI/Groove/Render/Theme.h"

namespace groove::fx {

namespace {

constexpr std::string_view kNoise = "#%&*+=/<>?[]{}^~$@!";

}  // namespace

float Clamp01(float value)
{
  return std::max(0.0f, std::min(1.0f, value));
}

float EaseOutCubic(float t)
{
  t = Clamp01(t);
  const float inverse = 1.0f - t;
  return 1.0f - inverse * inverse * inverse;
}

float EaseInOutCubic(float t)
{
  t = Clamp01(t);
  return t < 0.5f ? 4.0f * t * t * t : 1.0f - std::pow(-2.0f * t + 2.0f, 3.0f) / 2.0f;
}

float EaseOutBack(float t)
{
  t = Clamp01(t);
  constexpr float c1 = 1.70158f;
  constexpr float c3 = c1 + 1.0f;
  const float u = t - 1.0f;
  return 1.0f + c3 * u * u * u + c1 * u * u;
}

float Progress(std::uint32_t now, std::uint32_t start, std::uint32_t duration)
{
  if (start == 0 || duration == 0 || now < start) return now < start ? 0.0f : 1.0f;
  return Clamp01(static_cast<float>(now - start) / static_cast<float>(duration));
}

float Decay(std::uint32_t now, std::uint32_t start, std::uint32_t duration)
{
  if (start == 0 || now < start || now - start >= duration) return 0.0f;
  return 1.0f - static_cast<float>(now - start) / static_cast<float>(duration);
}

float OscillatorPhase(std::uint32_t now, float activity)
{
  const float speed = activity > 0.02f ? 1.0f / 380.0f : 1.0f / 2400.0f;
  return std::fmod(static_cast<float>(now) * speed, 1.0f);
}

float LfoPhase(std::uint32_t now, float rate)
{
  return std::fmod(static_cast<float>(now) * (0.0002f + rate * 0.0012f), 1.0f);
}

float FilterShimmer(std::uint32_t now, float activity)
{
  return activity > 0.02f ? 0.04f * std::sin(static_cast<float>(now) / 45.0f) * activity : 0.0f;
}

float OperatorLevelFlicker(std::uint32_t now, float activity, int op)
{
  return activity > 0.02f ? activity * (0.5f + 0.5f * std::sin(static_cast<float>(now) / 40.0f + static_cast<float>(op))) : 0.0f;
}

std::uint32_t Hash(std::uint32_t value)
{
  value ^= value >> 16;
  value *= 0x7feb352dU;
  value ^= value >> 15;
  value *= 0x846ca68bU;
  value ^= value >> 16;
  return value;
}

int DecodeText(Gfx& g, int x, int y, std::string_view text, Color color, Font font, float progress, std::uint32_t seed, int maxWidth)
{
  const int advance = Gfx::Advance(font);
  std::size_t length = text.size();
  if (maxWidth > 0) length = std::min(length, static_cast<std::size_t>(std::max(0, maxWidth / advance)));
  if (progress >= 1.0f) return g.Text(x, y, text.substr(0, length), color, font);
  const float resolved = Clamp01(progress) * static_cast<float>(length + 2);
  int cursor = x;
  for (std::size_t index = 0; index < length; ++index)
  {
    const float position = static_cast<float>(index);
    if (position < resolved - 2.0f)
    {
      const char glyph[2] = {text[index], 0};
      g.Text(cursor, y, glyph, color, font);
    }
    else if (position < resolved)
    {
      // The frontier: a solid block that leaves resolved characters behind it.
      g.Fill(Rect{cursor, y, advance - 1, Gfx::CapHeight(font)}, color);
    }
    else if (text[index] != ' ')
    {
      const std::uint32_t noise = Hash(seed * 131u + static_cast<std::uint32_t>(index) * 977u);
      const char glyph[2] = {kNoise[noise % kNoise.size()], 0};
      g.Text(cursor, y, glyph, theme::kFaint, font);
    }
    cursor += advance;
  }
  return cursor;
}

int TypeText(Gfx& g, int x, int y, std::string_view text, Color color, Font font, float progress)
{
  const std::size_t shown = static_cast<std::size_t>(Clamp01(progress) * static_cast<float>(text.size()) + 0.5f);
  const int end = g.Text(x, y, text.substr(0, shown), color, font);
  if (shown < text.size()) g.Fill(Rect{end, y, Gfx::Advance(font) - 1, Gfx::CapHeight(font)}, color);
  return end;
}

void MarchingAnts(Gfx& g, const Rect& rect, Color color, std::uint32_t now)
{
  if (rect.width < 2 || rect.height < 2) return;
  const int perimeter = 2 * (rect.width + rect.height) - 4;
  const int phase = static_cast<int>(now / 70) % 4;
  for (int index = 0; index < perimeter; ++index)
  {
    if (((index + phase) & 3) >= 2) continue;
    int x = 0, y = 0;
    if (index < rect.width) { x = rect.x + index; y = rect.y; }
    else if (index < rect.width + rect.height - 1) { x = rect.x + rect.width - 1; y = rect.y + index - rect.width + 1; }
    else if (index < 2 * rect.width + rect.height - 2) { x = rect.x + rect.width - 2 - (index - rect.width - rect.height + 1); y = rect.y + rect.height - 1; }
    else { x = rect.x; y = rect.y + rect.height - 2 - (index - 2 * rect.width - rect.height + 2); }
    g.Pixel(x, y, color);
  }
}

void Ripple(Gfx& g, const Rect& rect, float t, Color color)
{
  if (t <= 0.0f || t >= 1.0f) return;
  const int grow = static_cast<int>(EaseOutCubic(t) * 9.0f);
  const Rect ring{rect.x - grow, rect.y - grow, rect.width + grow * 2, rect.height + grow * 2};
  const int level = static_cast<int>((1.0f - t) * 16.0f);
  g.Dither(Rect{ring.x, ring.y, ring.width, 1}, color, level);
  g.Dither(Rect{ring.x, ring.y + ring.height - 1, ring.width, 1}, color, level);
  g.Dither(Rect{ring.x, ring.y, 1, ring.height}, color, level);
  g.Dither(Rect{ring.x + ring.width - 1, ring.y, 1, ring.height}, color, level);
}

void ScanLine(Gfx& g, int x, int width, int y, int trail, Color color)
{
  g.HLine(x, y, width, color);
  for (int row = 1; row <= trail; ++row)
  {
    const int level = 12 - row * 12 / (trail + 1);
    g.Dither(Rect{x, y - row, width, 1}, color, level);
  }
}

void Seam(Gfx& g, int x, int top, int height, int direction, Color color)
{
  g.VLine(x, top, height, color);
  g.VLine(x + (direction > 0 ? 1 : -1), top, height, color);
  for (int column = 1; column <= 10; ++column)
  {
    const int level = 12 - column;
    g.Dither(Rect{x - direction * (column + 1), top, 1, height}, color, level);
  }
}

void Fade(Gfx& g, const Rect& rect, int level)
{
  if (level <= 0) return;
  g.Dither(rect, theme::kBlack, std::min(16, level));
}

}  // namespace groove::fx
