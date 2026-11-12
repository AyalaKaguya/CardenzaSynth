#include "Applications/Synth/UI/Groove/Native/NativeView.h"

#include "Applications/Synth/UI/Groove/Native/NativeLayout.h"
#include "Applications/Synth/UI/Groove/Render/Theme.h"
#include "Applications/Synth/UI/Groove/Render/ModalComponents.h"

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
        case render::ModalHitRole::Cell: AddHit(hit.bounds, {Kind::FocusCell, static_cast<int>(hit.index)}); break;
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
      g.Text(64, 130, "AUTOMATIC NAME", kDim, Font::Small);
      const Rect name{64, 165, 352, 78};
      g.DottedHLine(name.x, name.y + name.height, name.width, kDim, 2);
      g.TextFit(name.x + 12, name.y + 27, name.width - 24, modal.text.empty() ? "NEW" : modal.text, kInk, Font::Huge);
      g.TextFit(64, 271, 352, "ENTER TO USE THIS NAME", kInk, Font::Large);
      g.TextFit(64, 307, 352, "ESC TO CANCEL", kDim, Font::Small);
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
      list.searchable = false;  // Native naming and search remain unchanged.
      registerGeometry(render::DrawModalList(g, controller, animator, modal, list));
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
