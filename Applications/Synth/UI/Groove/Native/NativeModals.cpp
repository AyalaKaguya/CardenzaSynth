#include "Applications/Synth/UI/Groove/Native/NativeView.h"

#include <algorithm>
#include <string>

#include "Applications/Synth/UI/Groove/Native/NativeLayout.h"
#include "Applications/Synth/UI/Groove/Render/Theme.h"
#include "Applications/Synth/UI/Groove/Render/ModalComponents.h"
#include "Applications/Synth/UI/Groove/Render/Widgets.h"

namespace groove::native {
using namespace theme;

void View::ModalContent(Gfx& g, const Controller& controller, Animator& animator, const Modal& modal)
{
  render::ModalPanelStyle panel;
  panel.content = layout::kBody;
  panel.spacious = true;
  for (std::size_t index = 0; index < panel.cells.size(); ++index) panel.cells[index] = layout::Cell(index);
  const auto registerGeometry = [&](const render::ModalGeometry& geometry)
  {
    for (std::size_t index = 0; index < geometry.count; ++index)
    {
      const auto& hit = geometry.hits[index];
      switch (hit.role)
      {
        case render::ModalHitRole::ListRow: AddHit(hit.bounds, {Kind::SelectListRow, static_cast<int>(hit.index)}); break;
        case render::ModalHitRole::Cell:
          AddHit(hit.bounds, {modal.kind == ModalKind::SampleRecord && hit.index >= 2 ? Kind::ActivateCell : Kind::FocusCell,
                             static_cast<int>(hit.index)});
          break;
        case render::ModalHitRole::Accept: AddHit(hit.bounds, {Kind::Enter}); break;
        case render::ModalHitRole::Cancel: AddHit(hit.bounds, {Kind::Back}); break;
      }
    }
  };
  switch (modal.kind)
  {
    case ModalKind::TextEntry:
    {
      render::DrawModalTitle(g, modal.title, kTab, panel);
      const Rect name{56, 94, 368, 44};
      g.Stroke(name, kTab);
      const auto cursor = std::min(modal.textCursor, modal.text.size());
      const auto first = cursor > 24 ? cursor - 24 : 0;
      const auto shown = modal.text.substr(first, 28);
      g.Text(name.x + 8, name.y + 15, shown, kInk, Font::Small);
      const int caret = name.x + 8 + Gfx::Measure(modal.text.substr(first, cursor - first), Font::Small);
      if ((animator.Now() / 400) % 2 == 0) g.VLine(caret, name.y + 10, 24, kTab);
      static constexpr std::string_view keys = "1234567890QWERTYUIOPASDFGHJKLZXCVBNM -";
      for (std::size_t key = 0; key < keys.size(); ++key)
      {
        const Rect rect{56 + static_cast<int>(key % 10) * 37, 150 + static_cast<int>(key / 10) * 45, 35, 41};
        widgets::DottedFrame(g, rect, kDim);
        g.TextAligned(rect, keys[key] == ' ' ? "_" : std::string(1, keys[key]), kInk, Font::Large, Align::Center);
        AddHit(rect, {Kind::TextKey, keys[key]});
      }
      const std::array<const char*, 4> labels{{"<", ">", "DEL", "CLEAR"}};
      const std::array<int, 4> values{{-2, -3, -1, -4}};
      for (std::size_t key = 0; key < labels.size(); ++key)
      {
        const Rect rect{56 + static_cast<int>(key) * 94, 336, 86, 36};
        widgets::DottedFrame(g, rect, kDim);
        g.TextAligned(rect, labels[key], kInk, Font::Small, Align::Center);
        AddHit(rect, {Kind::TextKey, values[key]});
      }
      const Rect cancel{56, 384, 180, 40}, accept{244, 384, 180, 40};
      g.Stroke(cancel, kEsc);g.Stroke(accept, kPlay);
      g.TextAligned(cancel, "CANCEL", kEsc, Font::Small, Align::Center);
      g.TextAligned(accept, "OK", kPlay, Font::Small, Align::Center);
      AddHit(cancel, {Kind::Back});AddHit(accept, {Kind::Enter});
      break;
    }
    case ModalKind::Confirm:
      registerGeometry(render::DrawModalConfirm(g, animator, modal, panel));
      break;
    case ModalKind::PresetBrowser:
    case ModalKind::SampleBrowser:
    case ModalKind::SampleOptions:
    {
      render::ModalListStyle list;
      list.panel = panel;
      list.rows = Rect{56, 92, 368, 324};
      list.scroll = Rect{419, 92, 4, 322};
      list.empty = Rect{56, 177, 368, 40};
      list.rowHeight = 36;
      list.highlightHeight = 34;
      list.textOffsetX = 18;
      list.textOffsetY = 10;
      list.markOffsetX = 4;
      list.markOffsetY = 12;
      list.textRightPadding = 20;
      list.rowFont = Font::Large;
      list.markFont = Font::Small;
      list.countFont = Font::Small;
      list.searchable = false; // A dedicated touch action opens the shared search field.
      registerGeometry(render::DrawModalList(g, controller, animator, modal, list));
      if (modal.kind != ModalKind::SampleOptions)
      {
        const Rect search{322, 56, 102, 28};
        g.Fill(search, kBlack);g.Stroke(search, kTab);
        g.TextAligned(search, modal.search.empty() ? "SEARCH" : "FILTER", kTab, Font::Small, Align::Center);
        AddHit(search, {Kind::Search});
      }
      break;
    }
    case ModalKind::EnginePicker:
      registerGeometry(render::DrawModalEnginePicker(g, controller, animator, modal, panel));
      break;
    case ModalKind::SampleRecord:
      registerGeometry(render::DrawModalSampleRecord(g, controller, animator, panel));
      break;
    case ModalKind::Menu:
    {
      const auto items = controller.Menu();
      for (std::size_t i = 0; i < items.size(); ++i)
      {
        if (items[i].action == MenuAction::None) continue;
        Action(g, layout::Cell(i), items[i].label, items[i].detail, items[i].on, kFn, items[i].available);
        if (items[i].available) AddHit(layout::Cell(i), {Kind::RunMenu, static_cast<int>(i)});
      }
      break;
    }
    case ModalKind::Record:
      registerGeometry(render::DrawModalRecord(g, controller, animator, panel));
      break;
    case ModalKind::None:
      break;
  }
}

}  // namespace groove::native
