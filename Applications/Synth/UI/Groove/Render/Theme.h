#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "OS/Storage/DeviceSettings.h"
#include "OS/UI/Canvas.h"

// Terminal / neo-minimal / dither palette and layout grid.
namespace groove::theme {

using ui::Color;
using ui::Rgb565;

// The screen's colors, in three palettes (SETUP > SYSTEM > THEME, os::storage::Theme). Drawing
// code names a color by what it means, never by what it is: kBlack is the field a panel is cleared
// to and kInk the mark that reads on it, with kShade, kFaint and kDim the steps between them, in
// that order. DARK draws light on black; LIGHT and RETRO draw dark ink on paper with the same calls.
// A dither of a color over the field reads as a tint of it on every palette. Key LEDs are light,
// not screen colors: they have no theme.
struct Palette {
  Color field;  // kBlack: what a panel is cleared to
  Color ink;    // kInk: text and marks; inverse selection is ink behind field-colored text
  Color dim;    // kDim: secondary text
  Color faint;  // kFaint: empty slots, ticks, unselected outlines
  Color shade;  // kShade: barely there
  // One color per left key; the same hue marks what that key controls everywhere.
  Color esc;   // scenes / back
  Color tab;   // tracks
  Color fn;    // second functions
  Color ctrl;  // pages
  Color play;
  Color rec;
  // One color per track, everywhere a track is named or drawn (header, TAB strip, maps, MIX, SENDS,
  // TAPE rows). Kept apart from the four left-key colors and from play green / record red.
  std::array<Color, 4> tracks;
};

// The shipped screen: a terminal's colors on black, each bright enough to glow.
inline constexpr Palette kDarkPalette{
    Rgb565(0, 0, 0),        // field
    Rgb565(232, 232, 224),  // ink
    Rgb565(132, 132, 126),  // dim
    Rgb565(58, 58, 54),     // faint
    Rgb565(26, 26, 24),     // shade
    Rgb565(255, 90, 54),    // esc red-orange
    Rgb565(54, 214, 230),   // tab cyan
    Rgb565(255, 208, 48),   // fn yellow
    Rgb565(160, 112, 255),  // ctrl violet
    Rgb565(72, 255, 120),   // play green
    Rgb565(255, 52, 68),    // rec red
    {{Rgb565(255, 92, 176), Rgb565(72, 152, 255), Rgb565(176, 236, 56), Rgb565(255, 164, 40)}},  // pink, blue, lime, amber
};

// The same screen on white paper: black ink and the same hues, each darkened to about a sixth of the
// paper's luminance, so that it reads as a mark on the paper (about 4.5:1) and ink still reads on it
// (about 3:1), as the dark screen's hues sit between black and its ink. Kept as bright as on black,
// the yellow, cyan, lime and green vanished on white. The grey steps mirror the dark ones.
inline constexpr Palette kLightPalette{
    Rgb565(236, 236, 228),  // field
    Rgb565(20, 20, 18),     // ink
    Rgb565(100, 100, 94),   // dim
    Rgb565(176, 176, 168),  // faint
    Rgb565(212, 212, 204),  // shade
    Rgb565(185, 63, 36),    // esc red-orange
    Rgb565(25, 115, 124),   // tab cyan
    Rgb565(138, 112, 21),   // fn yellow, as ochre
    Rgb565(112, 77, 180),   // ctrl violet
    Rgb565(28, 116, 51),    // play green
    Rgb565(194, 37, 49),    // rec red
    {{Rgb565(171, 59, 116), Rgb565(46, 101, 173), Rgb565(86, 118, 23), Rgb565(152, 96, 19)}},
};

// A backlit green LCD: pale green paper, near-black ink and the shades of that ink between them; a
// dither reads as the panel's own dot grid. The hues are dyes, a little greyed, midway in luminance
// between the paper and the ink like LIGHT's.
inline constexpr Palette kRetroPalette{
    Rgb565(188, 210, 160),  // field
    Rgb565(14, 24, 14),     // ink
    Rgb565(70, 90, 58),     // dim
    Rgb565(136, 160, 114),  // faint
    Rgb565(166, 190, 140),  // shade
    Rgb565(153, 67, 45),    // esc
    Rgb565(38, 102, 107),   // tab
    Rgb565(120, 99, 32),    // fn
    Rgb565(98, 76, 150),    // ctrl
    Rgb565(35, 102, 48),    // play
    Rgb565(156, 55, 52),    // rec
    {{Rgb565(141, 62, 98), Rgb565(52, 90, 141), Rgb565(79, 106, 32), Rgb565(132, 88, 34)}},
};

constexpr const Palette& PaletteOf(os::storage::Theme theme)
{
  switch (theme)
  {
    case os::storage::Theme::Light: return kLightPalette;
    case os::storage::Theme::Retro: return kRetroPalette;
    case os::storage::Theme::Dark:
    default: return kDarkPalette;
  }
}

namespace detail {
// The palette the screen draws with, changed only by Use(). Constant-initialized, so a color read
// before the first frame is the dark one.
inline Palette current = kDarkPalette;
inline os::storage::Theme currentTheme = os::storage::kDefaultTheme;
}  // namespace detail

// Read-only names of the palette in use: one load per drawing call, nothing per pixel.
inline const Color& kBlack = detail::current.field;
inline const Color& kInk = detail::current.ink;
inline const Color& kDim = detail::current.dim;
inline const Color& kFaint = detail::current.faint;
inline const Color& kShade = detail::current.shade;
inline const Color& kEsc = detail::current.esc;
inline const Color& kTab = detail::current.tab;
inline const Color& kFn = detail::current.fn;
inline const Color& kCtrl = detail::current.ctrl;
inline const Color& kPlay = detail::current.play;
inline const Color& kRec = detail::current.rec;
inline Color TrackColor(std::size_t track) { return detail::current.tracks[track & 3u]; }

// Switches the palette; RenderFrame calls it with the stored setting before each frame, so a change
// (on SETUP > SYSTEM, or a card that comes back with another system.cfg) shows on the next frame.
inline void Use(os::storage::Theme theme)
{
  if (theme == detail::currentTheme) return;
  detail::currentTheme = theme;
  detail::current = PaletteOf(theme);
}

// Ordered dither densities (0..16 of a 4x4 Bayer cell).
inline constexpr int kDitherIdle = 3;
inline constexpr int kDitherLatched = 8;
inline constexpr int kDitherSolid = 16;
inline constexpr int kDitherLevelOn = 12;
inline constexpr int kDitherLevelOff = 2;
inline constexpr int kDitherFill = 6;
inline constexpr int kDitherScrim = 10;
inline constexpr int kDitherPanel = 2;

inline constexpr int kScreenWidth = 240;
inline constexpr int kScreenHeight = 135;
inline constexpr int kStripWidth = 8;
inline constexpr int kGlyphColumnX = 232;
inline constexpr int kContentX = 10;
inline constexpr int kContentRight = 229;
inline constexpr int kContentWidth = kContentRight - kContentX;
inline constexpr int kHeaderY = 1;
inline constexpr int kHeaderRuleY = 11;
inline constexpr int kContentY = 14;
inline constexpr int kContentBottom = 122;
inline constexpr int kContentHeight = kContentBottom - kContentY;
inline constexpr int kFooterY = 125;
// Footer items and their highlight blocks (2 pixels around the text) stay 2 pixels clear of the
// edge caps on both sides.
inline constexpr int kFooterX = kContentX + 2;
inline constexpr int kFooterRight = kContentRight - 2;
inline constexpr int kFooterWidth = kFooterRight - kFooterX;

// The 4x4 cell grid mirrors the GRID keys (matrix columns 9..12).
inline constexpr int kCellGap = 3;
inline constexpr int kCellWidth = (kContentWidth - 3 * kCellGap) / 4;
inline constexpr int kCellHeight = (kContentHeight - 3 * kCellGap) / 4;

}  // namespace groove::theme
