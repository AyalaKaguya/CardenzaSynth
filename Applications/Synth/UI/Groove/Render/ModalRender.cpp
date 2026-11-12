#include "Applications/Synth/UI/Groove/Render/GrooveRenderer.h"
#include "Applications/Synth/UI/Groove/Render/ModalComponents.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <string_view>

#include "Applications/Synth/UI/Groove/Render/Effects.h"
#include "Applications/Synth/UI/Groove/Render/Theme.h"
#include "Applications/Synth/UI/Groove/Render/Widgets.h"

namespace groove::render {

namespace {

using namespace groove::theme;


void Title(Gfx& g, const std::string& title, Color color)
{
  g.TextFit(kContentX, kContentY, kContentWidth - 50, title, color);
}

void DrawTextEntry(Gfx& g, const Controller& controller, Animator& animator, const Modal& modal)
{
  Title(g, modal.title, kTab);
  const Rect box{kContentX, kContentY + 30, kContentWidth, 24};
  g.DottedHLine(box.x, box.y + box.height, box.width, kDim);
  const int advance = Gfx::Advance(Font::Large);
  const std::size_t fit = static_cast<std::size_t>((box.width - advance) / advance);
  // The window of text keeps the cursor in view.
  const std::size_t cursor = std::min(modal.textCursor, modal.text.size());
  const std::size_t first = cursor > fit ? cursor - fit : 0;
  const std::string shown = modal.text.substr(first, fit);
  g.Text(box.x, box.y + 5, shown, kInk, Font::Large);
  const int caretX = box.x + static_cast<int>(cursor - first) * advance;
  const float pop = fx::Decay(animator.Now(), animator.TextTypedAt(), 220);
  if (pop > 0.0f && cursor > first)
  {
    // The character just typed lands with a flash and a ripple.
    const Rect last{caretX - advance, box.y + 3, advance, 18};
    if (pop > 0.6f) g.Fill(last, kTab);
    g.Text(caretX - advance, box.y + 5, modal.text.substr(cursor - 1, 1), pop > 0.6f ? kBlack : kInk, Font::Large);
    fx::Ripple(g, last, 1.0f - pop, kTab);
  }
  // A block at the end, a bar between characters.
  if ((controller.State().now / 400) % 2 == 0)
    g.Fill(cursor < modal.text.size() ? Rect{caretX - 1, box.y + 2, 2, 20} : Rect{caretX + 1, box.y + 4, 10, 16}, kInk);
  char counter[24];
  std::snprintf(counter, sizeof(counter), "%u/%u", static_cast<unsigned>(modal.text.size()), static_cast<unsigned>(modal.maxLength));
  g.Text(kContentRight - Gfx::Measure(counter), kContentY, counter, kDim);
  g.Text(kContentX, kContentY + 70, "A-Z 0-9 SPACE -", kFaint);
  g.Text(kContentX, kContentY + 80, "FN + , / MOVE   FN + ; . START / END   FN + DEL DELETE", kFaint);
}

}  // namespace

using namespace groove::theme;
void ModalGeometry::Add(Rect bounds, ModalHitRole role, std::size_t index)
{
  if (bounds.width > 0 && bounds.height > 0 && count < hits.size()) hits[count++] = {bounds, role, index};
}

void DrawModalTitle(Gfx& g, std::string_view title, Color color, const ModalPanelStyle& style)
{
  const auto& area = style.content;
  g.TextFit(area.x, area.y + (style.spacious ? 2 : 0), area.width - (style.spacious ? 0 : 50), title, color,
            style.spacious ? Font::Large : Font::Micro);
  if (style.spacious) g.DottedHLine(area.x, area.y + 27, area.width, kFaint, 3);
}

void DrawModalWrapped(Gfx& g, Rect rect, std::string_view text, Color color, Font font, int lineGap)
{
  const auto fit = static_cast<std::size_t>(std::max(1, rect.width / Gfx::Advance(font)));
  const int lineHeight = Gfx::LineHeight(font) + lineGap;
  for (int y = rect.y; !text.empty() && y + Gfx::CapHeight(font) <= rect.y + rect.height; y += lineHeight)
  {
    const auto newline = text.find('\n');
    std::size_t length = std::min(fit, text.size());
    if (newline <= length) length = newline;
    else if (length < text.size())
    {
      const auto space = text.substr(0, length).find_last_of(' ');
      if (space != std::string_view::npos && space > fit / 2) length = space;
    }
    g.Text(rect.x, y, text.substr(0, length), color, font);
    text.remove_prefix(length);
    while (!text.empty() && (text.front() == ' ' || text.front() == '\n')) text.remove_prefix(1);
  }
}

ModalGeometry DrawModalList(Gfx& g, const Controller& controller, Animator& animator,
                            const Modal& modal, const ModalListStyle& style)
{
  const ListData data{modal.title, modal.search, modal.Count(), modal.cursor, static_cast<std::uint32_t>(modal.kind),
      controller.State().now, 0, &modal, [](const void* context, std::size_t index, std::string_view& label, bool& marked) {
        const auto* item = static_cast<const Modal*>(context)->Row(index);
        if (item == nullptr) return false;
        label = item->label;
        marked = item->marked;
        return true;
      }};
  return PaintListRows(g, animator, data, style);
}

ModalGeometry PaintListRows(Gfx& g, Animator& animator, const ListData& data, const ModalListStyle& style)
{
  ModalGeometry geometry;
  const auto& area = style.panel.content;
  const Rect rows = style.rows.width > 0 ? style.rows : Rect{area.x, area.y + 12, area.width - 6,
      static_cast<int>(kListVisibleRows) * style.rowHeight};
  const Rect scroll = style.scroll.width > 0 ? style.scroll : Rect{area.x + area.width - 2, rows.y, 1,
      static_cast<int>(kListVisibleRows) * style.rowHeight};
  if (!style.panel.spacious) DrawModalTitle(g, data.title, kTab, style.panel);
  else
  {
    const std::string count = std::to_string(data.count == 0 ? 0 : data.cursor + 1) + "/" + std::to_string(data.count);
    g.TextFit(area.x, area.y + 2, area.width - Gfx::Measure(count, style.countFont) - 8, data.title, kTab, Font::Large);
    g.DottedHLine(area.x, area.y + 27, area.width, kFaint, 3);
  }
  if (style.searchable)
  {
    const int x = g.Text(area.x + 60, area.y, "/", kFn);
    const int end = g.Text(x + 2, area.y, data.search, kInk);
    if ((data.now / 400) % 2 == 0) g.Fill(Rect{end, area.y, 5, 7}, kInk);
    if (data.search.empty()) g.Text(x + 10, area.y, "FN+F SEARCH", kFaint);
  }
  char count[24];
  std::snprintf(count, sizeof(count), "%u/%u", static_cast<unsigned>(data.count == 0 ? 0 : data.cursor + 1),
                static_cast<unsigned>(data.count));
  if (style.panel.spacious)
  {
    const int width = Gfx::Measure(count, style.countFont);
    g.Fill(Rect{area.x + area.width - width - 4, area.y, width + 4, Gfx::LineHeight(style.countFont)}, kBlack);
  }
  g.Text(area.x + area.width - Gfx::Measure(count, style.countFont), area.y, count, kDim, style.countFont);
  if (!style.panel.spacious) g.DottedHLine(area.x, rows.y - 2, area.width, kFaint);
  if (data.count == 0)
  {
    const Rect empty = style.empty.width > 0 ? style.empty : Rect{area.x, rows.y, area.width, 60};
    g.TextAligned(empty, style.searchable && !data.search.empty() ? "NO MATCH" : "EMPTY", kDim, style.rowFont, Align::Center);
    return geometry;
  }
  const std::size_t first = ListFirstRow(data.cursor);
  const float barY = animator.Smooth(Animator::Key("list-cursor", data.identity),
      static_cast<float>(rows.y + static_cast<int>(data.cursor - first) * style.rowHeight), 40.0f);
  g.Fill(Rect{rows.x, static_cast<int>(std::lround(barY)), rows.width, style.highlightHeight}, kInk);
  const auto openedAt = data.openedAt != 0 ? data.openedAt : animator.ModalOpenedAt();
  const float opened = fx::Clamp01(static_cast<float>(animator.Now() - openedAt) / 260.0f);
  for (std::size_t row = 0; row < kListVisibleRows && first + row < data.count; ++row)
  {
    std::string_view rowLabel;
    bool marked = false;
    if (data.row == nullptr || !data.row(data.context, first + row, rowLabel, marked)) continue;
    const int y = rows.y + static_cast<int>(row) * style.rowHeight;
    const bool covered = std::fabs(static_cast<float>(y) - barY) < static_cast<float>(style.rowHeight) * 0.5f;
    if (style.panel.spacious)
      widgets::DottedFrame(g, Rect{rows.x, y, rows.width, style.highlightHeight}, covered ? kBlack : kDim);
    const float reveal = fx::Clamp01(opened * 3.0f - static_cast<float>(row) * 0.25f);
    if (marked) g.Text(rows.x + style.markOffsetX, y + style.markOffsetY, ">", covered ? kBlack : kTab, style.markFont);
    const auto label = rowLabel.substr(0, std::min(rowLabel.size(),
        static_cast<std::size_t>(reveal * static_cast<float>(rowLabel.size()) + 0.5f)));
    g.TextFit(rows.x + style.textOffsetX, y + style.textOffsetY,
              area.width - style.textOffsetX - style.textRightPadding, label, covered ? kBlack : kInk, style.rowFont);
    geometry.Add(Rect{rows.x, y, rows.width, style.highlightHeight}, ModalHitRole::ListRow, first + row);
  }
  widgets::ScrollIndicator(g, scroll, first, kListVisibleRows, data.count);
  return geometry;
}

namespace {
Rect PanelCell(const ModalPanelStyle& style, std::size_t index)
{
  return index < style.cells.size() && style.cells[index].width > 0 ? style.cells[index] : widgets::Cell(index / kGridColumns, index % kGridColumns);
}
Rect PanelSpan(const ModalPanelStyle& style, std::size_t row, std::size_t column, std::size_t rows, std::size_t columns)
{
  const auto first = PanelCell(style, row * kGridColumns + column);
  const auto last = PanelCell(style, (row + rows - 1) * kGridColumns + column + columns - 1);
  return Rect{first.x, first.y, last.x + last.width - first.x, last.y + last.height - first.y};
}
void PanelCellValue(Gfx& g, Rect rect, std::string_view label, std::string_view value, float fraction,
                     bool focused, const ModalPanelStyle& style, Animator* animator = nullptr, std::size_t index = 0)
{
  widgets::ParamCellLayout metrics;
  if (style.spacious)
  {
    metrics.paddingX = metrics.rightPadding = 5;
    metrics.labelY = 4; metrics.labelHeight = 17; metrics.valueY = 32; metrics.barBottom = 5; metrics.barHeight = 3;
    metrics.labelFont = Font::Small; metrics.valueFont = Font::Large;
    metrics.dottedOutline = true;
  }
  if (style.spacious && animator != nullptr)
  {
    const auto now = animator->Now();
    widgets::ParamCell(g, rect, label, value, fraction, focused, false, metrics,
                        fx::Decay(now, animator->ValueChangedAt(index), 320), now);
    const float pressed = fx::Decay(now, animator->GridPressedAt(index), 220);
    if (pressed > 0) fx::Ripple(g, rect, 1.0f - pressed, kTab);
  }
  else widgets::ParamCell(g, rect, label, value, fraction, focused, false, metrics);
}
void PanelAction(Gfx& g, Rect rect, std::string_view label, std::string_view detail, bool focused, Color accent,
                  const ModalPanelStyle& style, Animator* animator = nullptr, std::size_t index = 0)
{
  if (!style.spacious) { widgets::ActionCell(g, rect, label, focused, accent); return; }
  widgets::ActionCellLayout metrics;
  metrics.labelFont = rect.width > 120 ? Font::Large : Font::Small;
  metrics.detailFont = Font::Small;
  metrics.padding = 5;
  metrics.labelY = 6;
  metrics.detailBottom = 21;
  metrics.focusBandHeight = 21;
  metrics.strongOutline = true;
  const auto now = animator != nullptr ? animator->Now() : 0;
  const float flash = animator != nullptr ? fx::Decay(now, animator->GridPressedAt(index), 220) : 0;
  widgets::ActionCell(g, rect, label, detail, focused, false, true, accent, metrics, flash, now);
}
void PanelPrompt(Gfx& g, const ModalPanelStyle& style, std::string_view label, std::string_view detail, Color color)
{
  if (!style.spacious) return;
  const auto& area = style.content;
  const Rect button{area.x + 8, area.y + area.height - 96, area.width - 16, 42};
  g.Fill(button, color);
  g.DrawGlyph(button.x + 12, button.y + 17, Glyph::Enter, kBlack);
  g.TextFit(button.x + 30, button.y + 14, button.width - 42, label, kBlack, Font::Large);
  g.TextFit(button.x, button.y + 57, button.width, detail, kDim, Font::Small);
}
}

ModalGeometry DrawModalConfirm(Gfx& g, Animator& animator, const Modal& modal, const ModalPanelStyle& style)
{
  ModalGeometry geometry;
  const auto& area = style.content;
  const bool notice = modal.confirmPurpose == ConfirmPurpose::Notice;
  const Color accent = !notice && modal.danger ? kRec : kInk;
  const int pad = style.spacious ? 8 : 6;
  const int width = area.width - 2 * pad;
  const int gap = style.spacious ? 12 : 10;
  const int buttonWidth = (width - gap) / 2;
  const Rect yes{area.x + pad, area.y + (style.spacious ? 254 : 56),
                 notice && style.spacious ? width : buttonWidth, style.spacious ? 58 : 26};
  const Rect no{yes.x + buttonWidth + gap, yes.y, buttonWidth, yes.height};
  if (style.spacious)
  {
    DrawModalTitle(g, modal.title, kInk, style);
    if (notice)
    {
      const auto end = modal.detail.find('\n');
      DrawModalWrapped(g, Rect{area.x + pad, area.y + 56, width, 60}, std::string_view(modal.detail).substr(0, end), kRec, Font::Large);
      if (end != std::string::npos)
        DrawModalWrapped(g, Rect{area.x + pad, area.y + 120, width, 116}, std::string_view(modal.detail).substr(end + 1), kDim, Font::Large);
    }
    else DrawModalWrapped(g, Rect{area.x + pad, area.y + 56, width, 180}, modal.detail,
                           modal.danger ? kRec : kDim, Font::Large);
  }
  else
  {
    g.TextFit(area.x + pad, area.y + 14, width, modal.title, kInk, Font::Small);
    if (notice)
    {
      std::string_view detail = modal.detail;
      for (int line = 0; !detail.empty() && line < 2; ++line)
      {
        const auto end = detail.find('\n');
        g.TextFit(area.x + pad, area.y + 30 + line * 10, width, detail.substr(0, end), line == 0 ? kRec : kDim);
        detail = end == std::string_view::npos ? std::string_view() : detail.substr(end + 1);
      }
    }
    else g.TextFit(area.x + pad, area.y + 30, width, modal.detail, modal.danger ? kRec : kDim);
  }
  g.Fill(yes, accent);
  if (modal.danger && !notice)
  {
    const float pulse = 0.5f + 0.5f * std::sin(static_cast<float>(animator.Now()) / 120.0f);
    fx::Ripple(g, yes, 0.2f + pulse * 0.5f, kRec);
  }
  const int labelX = style.spacious ? 30 : 20;
  const int labelY = style.spacious ? 20 : 9;
  g.DrawGlyph(yes.x + (style.spacious ? 12 : 8), yes.y + (style.spacious ? 23 : 10), Glyph::Enter, kBlack);
  g.Text(yes.x + labelX, yes.y + labelY, notice ? "ENTER  OK" : "ENTER  YES", kBlack,
           style.spacious ? Font::Small : Font::Micro);
  geometry.Add(yes, ModalHitRole::Accept);
  if (!notice)
  {
    g.Stroke(no, kDim);
    g.Text(no.x + labelX, no.y + labelY, "ESC  NO", kInk, style.spacious ? Font::Small : Font::Micro);
    geometry.Add(no, ModalHitRole::Cancel);
  }
  if (style.spacious) g.TextFit(area.x + pad, yes.y + yes.height + 24, width,
      notice ? "ESC ALSO CLOSES" : "ESC KEEPS THE CURRENT STATE", kDim, Font::Small);
  return geometry;
}

ModalGeometry DrawModalEnginePicker(Gfx& g, const Controller& controller, Animator& animator,
                                    const Modal& modal, const ModalPanelStyle& style)
{
  ModalGeometry geometry;
  if (!style.spacious) DrawModalTitle(g, "ENGINE  T" + std::to_string(controller.State().track + 1), kTab, style);
  static constexpr std::array<const char*, 4> details{{"6-OPERATOR FM", "ANALOG POLY", "SAMPLE PLAYER", "808 DRUM KIT"}};
  for (std::size_t index = 0; index < modal.items.size() && index < details.size(); ++index)
  {
    const Rect cell = style.spacious ? PanelSpan(style, index / 2 * 2, index % 2 * 2, 2, 2) : PanelCell(style, 4 + index);
    const bool selected = index == modal.cursor;
    if (selected)
    {
      g.Fill(cell, kInk);
      fx::MarchingAnts(g, Rect{cell.x - 2, cell.y - 2, cell.width + 4, cell.height + 4}, kTab, animator.Now());
    }
    else g.Stroke(cell, kFaint);
    const int pad = style.spacious ? 10 : 3;
    g.TextFit(cell.x + pad, cell.y + (style.spacious ? 17 : 3), cell.width - (style.spacious ? 20 : 5),
                modal.items[index].label, selected ? kBlack : kInk, style.spacious ? Font::Large : Font::Small);
    if (modal.items[index].marked) g.Text(cell.x + pad, cell.y + (style.spacious ? 61 : 15), "NOW", selected ? kBlack : kTab,
                                           style.spacious ? Font::Small : Font::Micro);
    if (style.spacious)
      fx::DecodeText(g, cell.x + pad, cell.y + cell.height - 26, details[index], selected ? kBlack : kDim, Font::Small,
          fx::Progress(animator.Now(), animator.ModalOpenedAt(), 400), static_cast<std::uint32_t>(index) + animator.Now() / 50,
          cell.width - 2 * pad);
    geometry.Add(cell, ModalHitRole::ListRow, index);
  }
  if (!style.spacious)
  {
    if (modal.cursor < details.size())
      fx::DecodeText(g, PanelCell(style, 8).x, PanelCell(style, 8).y + 6, details[modal.cursor], kDim, Font::Micro,
          fx::Progress(animator.Now(), animator.ModalOpenedAt(), 400), static_cast<std::uint32_t>(modal.cursor) + animator.Now() / 50);
    g.Text(PanelCell(style, 12).x, PanelCell(style, 12).y + 6, "THE TRACK SOUND IS REPLACED", kFaint);
  }
  return geometry;
}


ModalGeometry DrawModalSampleRecord(Gfx& g, const Controller& controller, Animator& animator,
                                    const ModalPanelStyle& style)
{
  ModalGeometry geometry;
  const auto& area = style.content;
  const auto& state = controller.State();
  const auto& host = controller.HostView();
  const auto sampler = host.Sampler().BuildUiState(state.track);
  const auto* engine = host.Engine(state.track);
  const double maxSeconds = sampler.sampleRate > 0 ? static_cast<double>(sampler.recordMaxFrameCount) / sampler.sampleRate : 5.0;
#if defined(CARDENZA_NATIVE_UI)
  if (sampler.saving)
  {
    DrawModalTitle(g, "SAVING SAMPLE", kTab, style);
    const int x = area.x + (style.spacious ? 8 : 0);
    const int width = area.width - (style.spacious ? 16 : 0);
    const float progress = std::clamp(sampler.savingProgress, 0.0f, 1.0f);
    g.Text(x, area.y + (style.spacious ? 66 : 22), "SAVING", kInk, style.spacious ? Font::Huge : Font::Large);
    g.TextFit(x, area.y + (style.spacious ? 113 : 44), width, sampler.statusText, kDim, style.spacious ? Font::Small : Font::Micro);
    widgets::LevelBar(g, Rect{x, area.y + (style.spacious ? 169 : 62), width, style.spacious ? 10 : 5}, progress, kInk, kFaint);
    if (style.spacious)
    {
      g.Text(x, area.y + 211, std::to_string(static_cast<int>(progress * 100)) + "%", kInk, Font::Huge);
      DrawModalWrapped(g, Rect{x, area.y + 274, width, 75}, "PLEASE WAIT. THE TAKE IS KEPT UNTIL SAVING COMPLETES.", kDim, Font::Small);
    }
    return geometry;  // Saving never exposes actions that can mutate its source.
  }
#endif
  if (sampler.recording)
  {
    const bool blink = (state.now / 300) % 2 == 0;
    const float breath = 0.5f + 0.5f * std::sin(static_cast<float>(animator.Now()) / 110.0f);
    if (style.spacious) DrawModalTitle(g, "RECORDING", kRec, style);
    const int indicatorX = area.x + (style.spacious ? 8 : 0);
    const int indicatorY = area.y + (style.spacious ? 38 : 4);
    g.Dither(Rect{indicatorX - 2, indicatorY - 2, 9, 11}, kRec, static_cast<int>(breath * 8.0f));
    g.DrawGlyph(indicatorX, indicatorY, Glyph::Rec, blink ? kRec : kFaint);
    if (!style.spacious) g.Text(area.x + 10, area.y + 2, "RECORDING", kRec, Font::Small);
    char time[48];
    std::snprintf(time, sizeof(time), style.spacious ? "%.1f / %.0f S" : "%.1f / %.1fS",
                  style.spacious && sampler.sampleRate > 0 ? static_cast<double>(sampler.frameCount) / sampler.sampleRate :
                      static_cast<double>(sampler.recordProgress) * maxSeconds, maxSeconds);
    const int x = area.x + (style.spacious ? 8 : 0);
    g.Text(x, area.y + (style.spacious ? 63 : 22), time, kInk, style.spacious ? Font::Huge : Font::Large);
    widgets::LevelBar(g, Rect{x, area.y + (style.spacious ? 118 : 46), area.width - (style.spacious ? 16 : 0), style.spacious ? 12 : 6},
                        sampler.recordProgress, kRec, kFaint);
    g.Text(x, area.y + (style.spacious ? 171 : 62), style.spacious ? "MIC INPUT" : "INPUT", kDim,
               style.spacious ? Font::Small : Font::Micro);
    widgets::LevelBar(g, style.spacious ? Rect{x, area.y + 206, area.width - 16, 10} : Rect{area.x + 36, area.y + 63, area.width - 36, 5},
                        sampler.inputLevel, sampler.inputLevel > 0.95f ? kRec : kPlay, kFaint);
    PanelPrompt(g, style, "ENTER TO STOP", "ESC CANCELS THE TAKE", kInk);
    if (style.spacious) geometry.Add(Rect{x, area.y + area.height - 96, area.width - 16, 42}, ModalHitRole::Accept);
    return geometry;
  }
  if (sampler.hasDraft)
  {
    synth::SynthParameterInfo start, end;
    const bool hasStart = engine != nullptr && engine->GetParameterInfo("sample.start", start);
    const bool hasEnd = engine != nullptr && engine->GetParameterInfo("sample.end", end);
    const auto* modal = controller.TopModal();
    const auto marker = modal != nullptr ? modal->trimMarker : 0;
    std::uint8_t focus = 0xff;
#if defined(CARDENZA_NATIVE_UI)
    if (style.spacious) focus = controller.NativeFocusedCell();
#endif
    if (!style.spacious)
    {
      DrawModalTitle(g, "TAKE", kTab, style);
      char length[24];
      std::snprintf(length, sizeof(length), "%.2fS", sampler.sampleRate > 0 ? static_cast<double>(sampler.frameCount) / sampler.sampleRate : 0.0);
      g.Text(area.x + area.width - Gfx::Measure(length), area.y, length, kDim);
    }
    PanelCellValue(g, PanelCell(style, 0), "START", hasStart ? std::to_string(start.value) : "--", hasStart ? start.value / 127.0f : -1.0f,
                     focus == 0 || (focus == 0xff && marker == 0), style, &animator, 0);
    PanelCellValue(g, PanelCell(style, 1), "END", hasEnd ? std::to_string(end.value) : "--", hasEnd ? end.value / 127.0f : -1.0f,
                     focus == 1 || (focus == 0xff && marker == 1), style, &animator, 1);
    PanelAction(g, PanelCell(style, 2), style.spacious && sampler.previewing ? "STOP" : "PLAY", "ENTER",
                    style.spacious ? focus == 2 : state.gridDown[2], kPlay, style, &animator, 2);
    PanelAction(g, PanelCell(style, 3), "SAVE", "ENTER", style.spacious ? focus == 3 : state.gridDown[3], kTab, style, &animator, 3);
    for (std::size_t index = 0; index < 4; ++index) geometry.Add(PanelCell(style, index), ModalHitRole::Cell, index);
    const Rect wave = PanelSpan(style, 1, 0, 3, 4);
    const float cursor = sampler.frameCount > 0 ? static_cast<float>(sampler.previewCursorFrame) / static_cast<float>(sampler.frameCount) : 0.0f;
#if defined(CARDENZA_NATIVE_UI)
    if (sampler.waveformPending)
      g.TextAligned(Rect{wave.x + 6, wave.y + 70, wave.width - 12, 68}, "BUILDING WAVEFORM", kDim,
                        style.spacious ? Font::Large : Font::Micro, Align::Center);
    else
#endif
      widgets::Waveform(g, style.spacious ? Rect{wave.x + 6, wave.y + 37, wave.width - 12, wave.height - 64} :
                          Rect{wave.x, wave.y + 4, wave.width, wave.height - 4}, sampler.waveform.data(), sampler.waveform.size(),
                          hasStart ? start.value / 127.0f : 0.0f, hasEnd ? end.value / 127.0f : 1.0f, cursor, sampler.previewing);
    if (style.spacious)
    {
      char duration[48];
      std::snprintf(duration, sizeof(duration), "TAKE  %.2f S  /  %.1f KHZ", sampler.sampleRate == 0 ? 0 :
                      static_cast<double>(sampler.frameCount) / sampler.sampleRate, static_cast<double>(sampler.sampleRate) / 1000);
      g.TextFit(wave.x + 6, wave.y + 8, wave.width - 12, duration, kDim, Font::Small);
      g.TextFit(wave.x + 6, wave.y + wave.height - 17, wave.width - 12, "SELECT START / END THEN ADD-SUB", kDim, Font::Small);
    }
    return geometry;
  }
  DrawModalTitle(g, style.spacious ? "RECORD SAMPLE" : "RECORD SAMPLE  T" + std::to_string(state.track + 1), kTab, style);
  const int x = area.x + (style.spacious ? 8 : 0);
  g.Text(x, area.y + (style.spacious ? 66 : 22), style.spacious ? "MICROPHONE" : "READY", kInk,
             style.spacious ? Font::Huge : Font::Large);
  char limit[32];
  std::snprintf(limit, sizeof(limit), style.spacious ? "%u SECONDS MAXIMUM" : "MIC  %uS MAX",
                  static_cast<unsigned>(synth::SamplerManager::kMaxRecordSeconds));
  g.Text(x, area.y + (style.spacious ? 109 : 44), limit, kDim, style.spacious ? Font::Small : Font::Micro);
  if (!style.spacious) g.Text(area.x, area.y + 62, "INPUT", kDim);
  widgets::LevelBar(g, style.spacious ? Rect{x, area.y + 224, area.width - 16, 8} : Rect{area.x + 36, area.y + 63, area.width - 36, 5},
                      sampler.inputLevel, kPlay, kFaint);
  if (sampler.phase == synth::SamplerTrackPhase::Error)
  {
    if (style.spacious) DrawModalWrapped(g, Rect{x, area.y + 155, area.width - 16, 58}, sampler.statusText, kRec, Font::Large);
    else g.TextFit(area.x, area.y + 78, area.width, sampler.statusText, kRec);
  }
  if (style.spacious)
  {
    const Rect button{x, area.y + area.height - 96, area.width - 16, 80};
    PanelAction(g, button, "RECORD", "ENTER TO START / ESC BACK", true, kRec, style, &animator, 0);
    geometry.Add(button, ModalHitRole::Cell, 0);
  }
  return geometry;
}

ModalGeometry DrawModalRecord(Gfx& g, const Controller& controller, Animator& animator, const ModalPanelStyle& style)
{
  ModalGeometry geometry;
  const auto& state = controller.State();
  const auto& rec = state.record;
  const auto now = animator.Now();
  const bool armed = controller.HostView().Sequencer().RecordArmed();
  const std::array<std::pair<const char*, std::string>, 3> settings{{
      {"REC", armed ? "ON" : "OFF"}, {"SNAP", rec.snap ? "STEPS" : "FREE"},
      {"VIEW", controller.PageTab(Scene::Sequence, 0)}}};
  const Rect empty = PanelCell(style, 3);
  g.DottedHLine(empty.x, empty.y + empty.height - 1, empty.width, kShade);
  for (std::size_t index = 0; index < settings.size(); ++index)
  {
    const Rect cell = PanelCell(style, index);
    PanelCellValue(g, cell, settings[index].first, settings[index].second, -1.0f, rec.focus == index, style, &animator, index);
    if (index == kRecordRec && armed && (now / 400) % 2)
      g.Fill(Rect{cell.x + cell.width - (style.spacious ? 12 : 7), cell.y + (style.spacious ? 6 : 3),
                    style.spacious ? 6 : 4, style.spacious ? 6 : 4}, kRec);
    geometry.Add(cell, ModalHitRole::Cell, index);
  }
  for (std::size_t order = 0; order < kCaptureTakes; ++order)
  {
    const auto slot = kRecordFirstTake + order;
    const Rect cell = PanelCell(style, slot);
    const auto* take = controller.TakeAt(order);
    if (take == nullptr)
    {
      g.DottedHLine(cell.x, cell.y + cell.height - 1, cell.width, kShade);
      if (order == 0) g.Text(cell.x + 2, cell.y + 3, "PLAY PADS", kFaint, Font::Micro);
      continue;
    }
    const bool focused = rec.focus == slot;
    const auto color = TrackColor(take->track);
    if (focused) g.Fill(cell, kInk);
    const auto ink = focused ? kBlack : kInk;
    g.Fill(Rect{cell.x, cell.y, 3, cell.height}, color);
    char label[24];
    std::snprintf(label, sizeof(label), "#%u", static_cast<unsigned>(take->id));
    g.Text(cell.x + (style.spacious ? 8 : 5), cell.y + (style.spacious ? 8 : 2), label, ink,
               style.spacious ? Font::Small : Font::Micro);
    const std::string track = "T" + std::to_string(take->track + 1);
    if (style.spacious) g.Text(cell.x + 8, cell.y + 65, track, focused ? kBlack : color, Font::Small);
    else g.Text(cell.x + cell.width - Gfx::Measure("T4") - 2, cell.y + 2, track, focused ? kBlack : color);
    const auto info = std::to_string(take->count) + (take->count == 1 ? " NOTE" : " NOTES");
    g.TextFit(cell.x + (style.spacious ? 8 : 5), cell.y + (style.spacious ? 32 : 12), cell.width - (style.spacious ? 16 : 6),
                info, focused ? kBlack : kDim, style.spacious ? Font::Small : Font::Micro);
    const auto span = std::max<std::uint32_t>(1, take->endedAt - take->startedAt);
    for (std::size_t index = 0; index < take->count; ++index)
    {
      const auto& note = rec.TakeNote(*take, index);
      const int x = cell.x + 5 + static_cast<int>(static_cast<std::uint64_t>(note.startMs) * static_cast<std::uint64_t>(cell.width - 8) / span);
      g.Pixel(x, cell.y + cell.height - 3 - (note.note % 12) / 4, focused ? kBlack : color);
    }
    geometry.Add(cell, ModalHitRole::Cell, slot);
  }
  const Rect area = PanelSpan(style, 3, 0, 1, 4);
  const auto* take = rec.focus >= kRecordFirstTake ? controller.TakeAt(rec.focus - kRecordFirstTake) : nullptr;
  if (take == nullptr)
  {
    static constexpr std::array<const char*, 3> help{{
      "REC: PLAY + PADS RECORD INTO THE PATTERN (ALSO FN + PLAY)",
      "SNAP: TAKES LAND ON WHOLE STEPS, OR KEEP THEIR TIMING", "VIEW: PIANO ROLL OR STEPS (TAP OK)"}};
    const auto detail = controller.TakeCount() == 0 ? "EVERYTHING THE PADS PLAY IS KEPT HERE AS TAKES" :
        "PICK A TAKE ABOVE TO PUT IT INTO A PATTERN";
    if (style.spacious)
    {
      DrawModalWrapped(g, Rect{area.x + 2, area.y + 4, area.width - 4, 40},
                        rec.focus < help.size() ? help[rec.focus] : "", kDim, Font::Small, 4);
      DrawModalWrapped(g, Rect{area.x + 2, area.y + 51, area.width - 4, area.height - 53}, detail, kFaint, Font::Small, 4);
    }
    else
    {
      g.TextFit(area.x + 2, area.y + 4, area.width - 4, rec.focus < help.size() ? help[rec.focus] : "", kDim, Font::Micro);
      g.TextFit(area.x + 2, area.y + 15, area.width - 4, detail, kFaint, Font::Micro);
    }
    return geometry;
  }
  const auto bars = std::max<std::size_t>(1, controller.TakeBars(*take));
  const auto bar = std::min<std::size_t>(rec.takeBar, bars - 1);
  struct Range { int low = 127, high = 0; std::size_t count = 0; } range;
  controller.ForEachTakeBarNote(*take, bar, [](const Controller::PlacedNote& note, void* context) {
    auto& r = *static_cast<Range*>(context); r.low = std::min<int>(r.low, note.note); r.high = std::max<int>(r.high, note.note); ++r.count;
  }, &range);
  const auto steps = std::max<std::size_t>(1, controller.HostView().Sequencer().TrackStepCount(take->track));
  const int labelWidth = style.spacious ? 108 : 44;
  char label[32];
  std::snprintf(label, sizeof(label), "BAR %u/%u", static_cast<unsigned>(bar + 1), static_cast<unsigned>(bars));
  g.Text(area.x + 2, area.y + 3, label, kInk, style.spacious ? Font::Small : Font::Micro);
  const auto age = (now - take->endedAt) / 1000;
  g.Text(area.x + 2, area.y + (style.spacious ? 24 : 13), age < 60 ? std::to_string(age) + "S AGO" : std::to_string(age / 60) + "M AGO",
             kFaint, style.spacious ? Font::Small : Font::Micro);
  const Rect lane{area.x + labelWidth, area.y + 2, area.width - labelWidth - 2, style.spacious ? area.height - 28 : area.height - 4};
  g.Stroke(lane, kShade);
  struct Drawing { Gfx& g; Rect lane; int low, span; std::size_t steps; Color color; int noteHeight; } drawing{
      g, lane, range.low, std::max(1, range.high - range.low), steps, TrackColor(take->track), style.spacious ? 3 : 2};
  controller.ForEachTakeBarNote(*take, bar, [](const Controller::PlacedNote& note, void* context) {
    auto& target = *static_cast<Drawing*>(context);
    const auto& r = target.lane;
    const auto pulses = target.steps * synth::kSequencerPulsesPerStep;
    const int x = r.x + 1 + static_cast<int>((note.step * synth::kSequencerPulsesPerStep + note.microstep * synth::kSequencerPulsesPerMicrostep) * (r.width - 2) / pulses);
    const int width = std::max(2, static_cast<int>(note.lengthPulses * (r.width - 2) / pulses));
    const int y = r.y + r.height - 3 - (note.note - target.low) * (r.height - 4) / target.span;
    target.g.Fill(Rect{x, y, std::min(width, r.x + r.width - 1 - x), target.noteHeight}, target.color);
  }, &drawing);
  if (range.count == 0) g.TextAligned(lane, "NO NOTES IN THIS BAR", kFaint, style.spacious ? Font::Small : Font::Micro, Align::Center);
  if (style.spacious) g.Text(area.x + 5, area.y + 62, "ADD-SUB BAR / ENTER NEW PATTERN", kDim, Font::Small);
  return geometry;
}

void ModalContent(Gfx& g, const Controller& controller, Animator& animator, const Modal& modal)
{
  switch (modal.kind)
  {
    case ModalKind::TextEntry: DrawTextEntry(g, controller, animator, modal); break;
    case ModalKind::Confirm: (void)DrawModalConfirm(g, animator, modal); break;
    case ModalKind::PresetBrowser:
    case ModalKind::SampleBrowser: (void)DrawModalList(g, controller, animator, modal); break;
    case ModalKind::SampleOptions:
    {
      ModalListStyle style; style.searchable = false;
      (void)DrawModalList(g, controller, animator, modal, style);
      break;
    }
    case ModalKind::EnginePicker: (void)DrawModalEnginePicker(g, controller, animator, modal); break;
    case ModalKind::SampleRecord: (void)DrawModalSampleRecord(g, controller, animator); break;
    case ModalKind::Menu: MenuContent(g, controller, animator, 1.0f); break;
    case ModalKind::Record: (void)DrawModalRecord(g, controller, animator); break;
    case ModalKind::None:
    default: break;
  }
}

const char* ModalHint(const Controller& controller, const Modal& modal)
{
  switch (modal.kind)
  {
    case ModalKind::TextEntry: return "ENTER OK   DEL ERASE   ESC CANCEL";
    case ModalKind::Confirm:
      if (modal.confirmPurpose == ConfirmPurpose::Notice) return "ENTER OR ESC CLOSES";
      return modal.danger ? "ENTER CONFIRM   ESC KEEP" : "ENTER YES   ESC NO";
    case ModalKind::PresetBrowser: return "ENTER LOAD   FN+F SEARCH   ESC BACK";
    case ModalKind::SampleBrowser: return "ENTER LOAD  FN+F SEARCH  FN+ENTER EDIT";
    case ModalKind::SampleOptions: return "ENTER CHOOSE   ESC BACK";
    case ModalKind::EnginePicker: return "GRID OR SUB ADD   ENTER SWITCH";
    case ModalKind::Menu: return "GRID RUNS   FN OR ESC CLOSES";
    case ModalKind::Record: return "+- BAR  OK NEW PATTERN  FN+OK MERGE";
    case ModalKind::SampleRecord:
    {
      const auto sampler = controller.HostView().Sampler().BuildUiState(controller.State().track);
      if (sampler.recording) return "ENTER STOP   ESC CANCEL";
      if (sampler.hasDraft) return "SUB ADD TRIM   ENTER START/END   ESC DISCARD";
      return "ENTER RECORD   ESC CLOSE";
    }
    case ModalKind::None:
    default: return "";
  }
}

}  // namespace groove::render
