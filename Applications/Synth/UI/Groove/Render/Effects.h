#pragma once

#include <cstdint>
#include <string_view>

#include "Applications/Synth/UI/Groove/Render/Gfx.h"

// Motion primitives. Every effect is a pure function of elapsed time, so frames are reproducible
// and nothing needs a second framebuffer.
namespace groove::fx {

float Clamp01(float value);
float EaseOutCubic(float t);
float EaseInOutCubic(float t);
float EaseOutBack(float t);
// 0..1 progress of an animation started at `start` lasting `duration` ms; 1 when idle or done.
float Progress(std::uint32_t now, std::uint32_t start, std::uint32_t duration);
// Decays from 1 to 0 over `duration` ms after `start` (0 when start is 0).
float Decay(std::uint32_t now, std::uint32_t start, std::uint32_t duration);
// Shared synth graph motion: phases keep advancing even while no notes sound.
float OscillatorPhase(std::uint32_t now, float activity);
float LfoPhase(std::uint32_t now, float rate);
float FilterShimmer(std::uint32_t now, float activity);
float OperatorLevelFlicker(std::uint32_t now, float activity, int op);
// Deterministic per-frame noise.
std::uint32_t Hash(std::uint32_t value);

// Terminal "decode": characters resolve left to right out of random glyphs; the frontier is a block.
int DecodeText(Gfx& g, int x, int y, std::string_view text, Color color, Font font, float progress, std::uint32_t seed, int maxWidth = 0);
// Typewriter reveal with a trailing cursor.
int TypeText(Gfx& g, int x, int y, std::string_view text, Color color, Font font, float progress);
// Dotted outline whose dots crawl around the rectangle.
void MarchingAnts(Gfx& g, const Rect& rect, Color color, std::uint32_t now);
// Rectangle outline expanding out of `rect` and dissolving.
void Ripple(Gfx& g, const Rect& rect, float t, Color color);
// Horizontal scan line at `y` with a dithered afterglow `trail` pixels above it.
void ScanLine(Gfx& g, int x, int width, int y, int trail, Color color);
// Vertical seam with a dithered motion trail on the side it came from.
void Seam(Gfx& g, int x, int top, int height, int direction, Color color);
// Dithered black mask: level 16 hides everything, 0 shows everything.
void Fade(Gfx& g, const Rect& rect, int level);

}  // namespace groove::fx
