#pragma once

#include <array>
#include "Applications/Synth/UI/Groove/Render/Gfx.h"

namespace groove::native::layout {

inline constexpr int kWidth = 480;
inline constexpr int kHeight = 480;
inline constexpr int kSide = 48;
inline constexpr int kHeader = 48;
inline constexpr int kFooter = 48;
// Visual clearance for the rounded bezel, not a measured panel specification.
// Keep the full side touch targets; only their painting follows this contour.
inline constexpr int kCornerClearance = 48;
inline constexpr int kEdgePadding = 6;
inline constexpr auto kCornerInsets = [] {
  std::array<int, kCornerClearance> result{};
  for (int y = 0; y < kCornerClearance; ++y)
  {
    const int dy = kCornerClearance - y;
    int dx = kCornerClearance;
    while (dx * dx + dy * dy > kCornerClearance * kCornerClearance) --dx;
    result[static_cast<std::size_t>(y)] = kCornerClearance - dx;
  }
  return result;
}();
inline int ScreenInset(int y) {
  const int edge = y < kHeight / 2 ? y : kHeight - 1 - y;
  return edge >= 0 && edge < kCornerClearance ? kCornerInsets[static_cast<std::size_t>(edge)] : 0;
}
inline constexpr Rect kCenter{48, 48, 384, 384};
inline constexpr Rect kBody{56, 56, 368, 368};
inline constexpr int kGap = 8;
inline constexpr int kCell = 86;
inline constexpr int kPitch = kCell + kGap;

inline Rect Cell(std::size_t index) {
  return Rect{kBody.x + static_cast<int>(index % 4) * kPitch,
              kBody.y + static_cast<int>(index / 4) * kPitch, kCell, kCell};
}
inline Rect Span(std::size_t row, std::size_t column, std::size_t rows, std::size_t columns) {
  const Rect cell = Cell(row * 4 + column);
  return Rect{cell.x, cell.y, static_cast<int>(columns) * kPitch - kGap, static_cast<int>(rows) * kPitch - kGap};
}
inline Rect Side(bool right, std::size_t index) { return Rect{right ? 432 : 0, static_cast<int>(index) * 120, 48, 120}; }
inline Rect Tab(std::size_t index) { return Rect{48 + static_cast<int>(index) * 96, 432, 96, 48}; }
inline bool Contains(const Rect& rect, int x, int y) {
  return x >= rect.x && y >= rect.y && x < rect.x + rect.width && y < rect.y + rect.height;
}

}  // namespace groove::native::layout
