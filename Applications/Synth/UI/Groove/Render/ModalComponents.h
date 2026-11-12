#pragma once

#include <array>
#include <cstddef>
#include <string_view>

#include "Applications/Synth/UI/Groove/GrooveController.h"
#include "Applications/Synth/UI/Groove/Render/Animator.h"
#include "Applications/Synth/UI/Groove/Render/Gfx.h"
#include "Applications/Synth/UI/Groove/Render/Theme.h"

namespace groove::render {

// Paint and input use the same geometry. The renderer never dispatches an
// action; a display adapter maps these domain roles to its input vocabulary.
enum class ModalHitRole : std::uint8_t { ListRow, Cell, Accept, Cancel };
struct ModalHit {
  Rect bounds{};
  ModalHitRole role = ModalHitRole::Cell;
  std::size_t index = 0;
};
struct ModalGeometry {
  std::array<ModalHit, kGridCount> hits{};
  std::size_t count = 0;
  void Add(Rect bounds, ModalHitRole role, std::size_t index = 0);
};

// Default metrics preserve the compact renderer. Wider displays provide their
// content rectangle and cell rectangles, retaining their larger touch targets.
struct ModalPanelStyle {
  Rect content{theme::kContentX, theme::kContentY, theme::kContentWidth, theme::kContentHeight};
  std::array<Rect, kGridCount> cells{};
  bool spacious = false;
};
struct ModalListStyle {
  ModalPanelStyle panel{};
  Rect rows{};
  Rect scroll{};
  Rect empty{};
  int rowHeight = 10;
  int highlightHeight = 9;
  int textOffsetX = 10;
  int textOffsetY = 1;
  int markOffsetX = 2;
  int markOffsetY = 1;
  int textRightPadding = 8;
  Font rowFont = Font::Micro;
  Font markFont = Font::Micro;
  Font countFont = Font::Micro;
  bool searchable = true;
};
struct ListData {
  std::string_view title;
  std::string_view search;
  std::size_t count = 0;
  std::size_t cursor = 0;
  std::uint32_t identity = 0;
  std::uint32_t now = 0;
  std::uint32_t openedAt = 0;
  const void* context = nullptr;
  bool (*row)(const void* context, std::size_t index, std::string_view& label, bool& marked) = nullptr;
};

void DrawModalTitle(Gfx& g, std::string_view title, Color color, const ModalPanelStyle& style = {});
void DrawModalWrapped(Gfx& g, Rect rect, std::string_view text, Color color,
                       Font font, int lineGap = 8);
ModalGeometry DrawModalList(Gfx& g, const Controller& controller, Animator& animator,
                            const Modal& modal, const ModalListStyle& style = {});
// A project list can use the same painter without copying its file-name list.
ModalGeometry PaintListRows(Gfx& g, Animator& animator, const ListData& data,
                             const ModalListStyle& style = {});
ModalGeometry DrawModalConfirm(Gfx& g, Animator& animator, const Modal& modal,
                               const ModalPanelStyle& style = {});
ModalGeometry DrawModalEnginePicker(Gfx& g, const Controller& controller, Animator& animator,
                                    const Modal& modal, const ModalPanelStyle& style = {});
ModalGeometry DrawModalSampleRecord(Gfx& g, const Controller& controller, Animator& animator,
                                    const ModalPanelStyle& style = {});
ModalGeometry DrawModalRecord(Gfx& g, const Controller& controller, Animator& animator,
                              const ModalPanelStyle& style = {});

}  // namespace groove::render
