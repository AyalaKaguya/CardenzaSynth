#include "Applications/Synth/UI/Groove/Native/NativeView.h"

#include <algorithm>
#include <cstdio>
#include <string>

#include "Applications/Synth/UI/Groove/Native/NativeLayout.h"
#include "Applications/Synth/UI/Groove/Render/GrooveRenderer.h"
#include "Applications/Synth/UI/Groove/Render/SceneComponents.h"
#include "Applications/Synth/UI/Groove/Render/Effects.h"
#include "Applications/Synth/UI/Groove/Render/Theme.h"
#include "Applications/Synth/UI/Groove/Render/Widgets.h"

namespace groove::native {
namespace {
using namespace theme;
constexpr std::array<KeyRole, 4> kRight{{KeyRole::Add, KeyRole::Sub, KeyRole::Enter, KeyRole::Play}};
constexpr std::array<const char*, 4> kLeftNames{{"ESC", "TAB", "FN", "CTRL"}};
constexpr std::array<const char*, 4> kRightNames{{"ADD", "SUB", "ENTER", "PLAY"}};
constexpr std::array<Color, 4> kLeftColors{{kEsc, kTab, kFn, kCtrl}};
constexpr std::array<Glyph, 4> kLeftGlyphs{{Glyph::Back, Glyph::Tab, Glyph::Menu, Glyph::Pages}};

void KeyCap(Gfx& g, const Rect& rect, Color color, bool active, bool right, int density = 4)
{
  for (int y = rect.y; y < rect.y + rect.height; ++y)
  {
    const int inset = layout::ScreenInset(y);
    const int x = right ? rect.x - inset : rect.x + inset;
    const Rect line{x, y, rect.width, 1};
    if (active) g.Fill(line, color);
    else g.Dither(line, color, density);
    g.Pixel(right ? x : x + rect.width - 1, y, color);
  }
}

void ClearCorners(Gfx& g)
{
  for (int y = 0; y < layout::kCornerClearance; ++y)
  {
    const int inset = layout::ScreenInset(y);
    for (const int row : {y, layout::kHeight - 1 - y})
    {
      g.Fill(Rect{0, row, inset, 1}, kBlack);
      g.Fill(Rect{layout::kWidth - inset, row, inset, 1}, kBlack);
    }
  }
}

void MapTile(Gfx& g, const Rect& rect, std::string_view label, std::string_view detail, bool selected, Color accent, std::uint32_t now)
{
  render::DrawMapTile(g, rect, selected, true, accent, now);
  g.TextFit(rect.x + 8, rect.y + 10, rect.width - 16, label, selected ? kBlack : kInk,
            rect.width > 120 ? Font::Large : Font::Small);
  g.TextFit(rect.x + 8, rect.y + rect.height - 23, rect.width - 16, detail, selected ? kBlack : kDim, Font::Small);
  if (!selected) g.Text(rect.x + rect.width - 12, rect.y + 11, ">", accent, Font::Small);
}

void Boot(Gfx& g, const Controller& controller, Animator& animator)
{
  g.Fill(layout::kCenter, kBlack);
  render::BootStyle style;
  style.bounds = layout::kCenter;style.titleY = 178;style.subtitleY = 219;
  style.dashY = 255;style.dashLeft = 187;style.dashPitch = 28;style.dashWidth = 22;style.dashHeight = 5;
  style.subtitleFont = Font::Small;
  render::DrawBoot(g, animator, style);
  (void)controller;
}


}  // namespace

void View::AddHit(const Rect& rect, Command command, bool side)
{
  if (!registerHits_) return;
  if (hitCount_ < hits_.size()) hits_[hitCount_++] = Hit{rect, command, Layer::None, side, false};
}

void View::AddLayerHit(const Rect& rect, Layer layer)
{
  if (!registerHits_) return;
  if (hitCount_ < hits_.size()) hits_[hitCount_++] = Hit{rect, {}, layer, false, true};
}

void View::AddPerformanceHit(const Rect& rect, int note)
{
  if (!registerHits_) return;
  if (note >= 0 && note < 128 && performanceHitCount_ < performanceHits_.size())
    performanceHits_[performanceHitCount_++] = PerformanceHit{rect, note};
}

int View::PerformanceNoteAt(int x, int y) const
{
  for (std::size_t index = performanceHitCount_; index > 0; --index)
    if (layout::Contains(performanceHits_[index - 1].rect, x, y)) return performanceHits_[index - 1].note;
  return -1;
}

void View::CancelPerformance(Controller& controller, std::uint32_t nowMs, bool clear)
{
  for (auto& pointer : performancePointers_)
  {
    if (!pointer.active) continue;
    if (pointer.note >= 0) (void)controller.Dispatch({Kind::PerformanceUp, pointer.id}, nowMs);
    pointer.note = -1;
    pointer.cancelled = true;
    if (clear) pointer.active = false;
  }
}

std::uint32_t View::Generation(const Controller& controller)
{
  const auto& state = controller.State();
  const auto* modal = controller.TopModal();
  std::uint32_t value = 2166136261u;
  const auto mix = [&](std::uint32_t part) { value = (value ^ part) * 16777619u; };
  mix(static_cast<std::uint32_t>(state.scene));
  mix(controller.CurrentPage());
  mix(static_cast<std::uint32_t>(state.track));
  mix(static_cast<std::uint32_t>(state.modals.size()));
  mix(static_cast<std::uint32_t>(modal == nullptr ? ModalKind::None : modal->kind));
  mix(state.sessionAt);
  mix(static_cast<std::uint32_t>(controller.HostView().EngineKind(state.track)));
  mix(static_cast<std::uint32_t>(state.keyboard.octave));
  const auto engine = reinterpret_cast<std::uintptr_t>(controller.HostView().Engine(state.track));
  mix(static_cast<std::uint32_t>(engine));
  mix(static_cast<std::uint32_t>(static_cast<std::uint64_t>(engine) >> 32));
  return value;
}

void View::Cell(Gfx& g, const Rect& rect, std::string_view label, std::string_view value,
                float fraction, bool focused, Color accent, bool available)
{
  widgets::ParamCellLayout metrics;
  metrics.paddingX = 5;
  metrics.rightPadding = 5;
  metrics.labelY = 4;
  metrics.labelHeight = 17;
  metrics.valueY = 32;
  metrics.barBottom = 5;
  metrics.barHeight = 3;
  metrics.labelFont = Font::Small;
  metrics.valueFont = Font::Large;
  widgets::ParamCell(g, rect, label, value, fraction, focused, !available, metrics);
  (void)accent;
}

void View::Action(Gfx& g, const Rect& rect, std::string_view label, std::string_view detail,
                  bool focused, Color accent, bool available)
{
  widgets::ActionCellLayout metrics;
  metrics.labelFont = rect.width > 120 ? Font::Large : Font::Small;
  metrics.detailFont = Font::Small;
  metrics.padding = 5;
  metrics.labelY = 6;
  metrics.detailBottom = 21;
  metrics.focusBandHeight = 21;
  const bool pressed = pointerDown_ && !pointerCancelled_ && !pressed_.side &&
      pressed_.rect.x == rect.x && pressed_.rect.y == rect.y && pressed_.rect.width == rect.width;
  widgets::ActionCell(g, rect, label, detail, focused, pressed, available, accent, metrics);
}

void View::SetLayer(Controller& controller, Layer layer, std::uint32_t nowMs)
{
  if (layer == layer_) return;
  hitCount_ = 0;
  performanceHitCount_ = 0;
  if (layer_ != Layer::None)
  {
    controller.NotifyNativeOverlay(static_cast<std::size_t>(layer_) - 1, false, nowMs);
    closingLayer_ = layer_;
    layerClosedAt_ = nowMs;
  }
  layer_ = layer;
  if (layer != Layer::None)
  {
    closingLayer_ = Layer::None;
    layerOpenedAt_ = nowMs;
    controller.NotifyNativeOverlay(static_cast<std::size_t>(layer) - 1, true, nowMs);
  }
}

void View::Header(Gfx& g, const Controller& controller, Animator& animator)
{
  const auto& state = controller.State();
  const auto sequence = controller.HostView().Sequencer().BuildUiState(state.track);
  const auto now = animator.Now();
  constexpr Font font = Font::Small;
  constexpr int y = 23;
  constexpr int left = 56;
  constexpr int rightEdge = 424;
  constexpr int ruleY = 46;
  constexpr int ruleWidth = rightEdge - left;
  const Color color = TrackColor(state.track);
  const auto* engine = controller.HostView().Engine(state.track);
  const auto& transition = animator.CurrentTransition();
  const float sceneDecode = transition.kind == Animator::TransitionKind::Scene ? fx::Progress(now, transition.start, 320) : 1;
  int x = left;
  if (layer_ == Layer::Menu || controller.MenuOpen())
  {
    x = g.Text(x, y, "FN", kFn, font);
    const auto openedAt = layer_ == Layer::Menu ? layerOpenedAt_ : animator.ModalOpenedAt();
    x = fx::DecodeText(g, x + 4, y, "MENU", kInk, font, fx::Progress(now, openedAt, 320), now / 40);
  }
  else
  {
    x = g.Text(x, y, std::to_string(static_cast<int>(state.scene)), kEsc, font);
    x = fx::DecodeText(g, x + 4, y, kSceneNames[static_cast<std::size_t>(state.scene)], kInk, font, sceneDecode, now / 40);
  }
  x += 8;
  const std::string track = "T" + std::to_string(state.track + 1);
  const int grow = static_cast<int>(fx::Decay(now, animator.TrackChangedAt(), 360) * 4);
  const bool noteFlash = fx::Decay(now, animator.LastNoteAt(), 160) > 0.5f;
  g.Fill(Rect{x - 3 - grow, y - 4, Gfx::Measure(track, font) + 6 + grow * 2, 17}, noteFlash ? kInk : color);
  x = g.Text(x, y, track, kBlack, font) + 7;
  x = g.Text(x, y, EngineShortName(controller.HostView().EngineKind(state.track)), kDim, font) + 6;

  // Reserve the complete status cluster first. Only the preset yields width.
  int right = rightEdge;
  if (controller.Guide())
  {
    right -= Gfx::Measure("GUIDE", font);
    g.Text(right, y, "GUIDE", kInk, font);
    right -= 7;
  }
  else if (state.dirty)
  {
    right -= Gfx::Measure("*", font);
    g.Text(right, y, "*", (now / 700) % 2 ? kFn : kDim, font);
    right -= 5;
  }
  const std::string bpm = std::to_string(sequence.bpm);
  right -= Gfx::Measure(bpm, font);
  const float beat = sequence.playing ? fx::Decay(now, animator.BeatAt(), 150) : 0;
  g.Text(right, y, bpm, beat > 0.3f ? animator.Downbeat() ? kPlay : kInk : sequence.playing ? kInk : kDim, font);
  right -= 12;
  g.DrawGlyph(right, y + 1, sequence.playing ? Glyph::Play : Glyph::Stop, sequence.playing ? kPlay : kFaint);
  right -= 5;
  if (sequence.recordArmed)
  {
    right -= Gfx::Measure("REC", font) + 3;
    if ((now / 250) % 2 || !sequence.playing) g.Text(right, y, "REC", kRec, font);
  }
  if (sequence.songMode && sequence.songLength > 0)
  {
    char row[12];
    std::snprintf(row, sizeof(row), "R%02u", static_cast<unsigned>(sequence.songRow + 1));
    right -= Gfx::Measure(row, font) + 5;
    g.Text(right, y, row, kFn, font);
  }
  if (engine != nullptr && right - x > 8)
  {
    const auto changed = std::max(animator.PresetChangedAt(state.track), animator.TrackChangedAt());
    fx::DecodeText(g, x, y, engine->CurrentPresetName(), kInk, font, fx::Progress(now, changed, 380), changed, right - x - 8);
  }

  g.DottedHLine(left, ruleY, ruleWidth, color, 2);
  if (sequence.songPlaying && sequence.songLength > 0)
  {
    for (std::size_t row = 0; row <= sequence.songLength; ++row)
      g.Pixel(left + static_cast<int>(row * ruleWidth / sequence.songLength), ruleY - 1, kFn);
    const float position = (static_cast<float>(sequence.songRow) + sequence.songRowProgress) / static_cast<float>(sequence.songLength);
    const int progress = std::clamp(static_cast<int>(position * (ruleWidth - 1)), 0, ruleWidth - 1);
    g.HLine(left, ruleY, progress + 1, kFn);
    g.Fill(Rect{left + progress - 1, ruleY - 1, 3, 3}, kInk);
  }
  else if (sequence.playing)
  {
    const int progress = std::clamp(static_cast<int>(sequence.patternProgress * (ruleWidth - 1)), 0, ruleWidth - 1);
    g.HLine(left, ruleY, progress + 1, kPlay);
    g.Fill(Rect{left + progress - 1, ruleY - 1, 3, 3}, kInk);
  }
  else if (animator.StopAt() != 0 && now - animator.StopAt() < kRewindMs)
  {
    const float rewind = fx::EaseOutCubic(fx::Progress(now, animator.StopAt(), kRewindMs));
    const int progress = std::clamp(static_cast<int>(animator.StopProgress() * (1 - rewind) * (ruleWidth - 1)), 0, ruleWidth - 1);
    g.HLine(left, ruleY, progress + 1, kPlay);
    for (int tail = 1; tail <= 10 && progress + tail < ruleWidth; ++tail)
      g.Dither(Rect{left + progress + tail, ruleY - 1, 1, 1}, kPlay, 12 - tail);
    g.Fill(Rect{left + progress - 1, ruleY - 1, 3, 3}, kInk);
  }
}

void View::Sides(Gfx& g, const Controller& controller, Animator& animator)
{
  const bool modal = controller.TopModal() != nullptr;
  const ModalKind modalKind = modal ? controller.TopModal()->kind : ModalKind::None;
  const bool saving = controller.HostView().Sampler().SavingTrack() >= 0;
  const bool context = modalKind == ModalKind::SampleBrowser || modalKind == ModalKind::Record;
  const std::array<Layer, 4> left{{Layer::Scenes, Layer::Tracks, Layer::Menu, Layer::Pages}};
  const bool playing = controller.HostView().Sequencer().Playing();
  Glyph add = Glyph::Up;
  Glyph sub = Glyph::Down;
  switch (controller.AdjustKeysMean())
  {
    case AdjustMeaning::Sideways: add = Glyph::Right; sub = Glyph::Left; break;
    case AdjustMeaning::Erase: add = Glyph::Delete; sub = Glyph::None; break;
    case AdjustMeaning::None: add = Glyph::None; sub = Glyph::None; break;
    default: break;
  }
  const bool recArmed = controller.HostView().Sequencer().RecordArmed();
  const std::array<Glyph, 4> rightGlyphs{{add, sub, Glyph::Enter, recArmed && !playing ? Glyph::Rec : Glyph::Play}};
  for (std::size_t row = 0; row < 4; ++row)
  {
    const Rect l = layout::Side(false, row);
    const Rect r = layout::Side(true, row);
    const bool selected = layer_ == left[row];
    const bool usable = !saving && (!modal || row == 0 || (row == 2 && (context || modalKind == ModalKind::Menu)));
    const bool leftDown = pointerDown_ && !pointerCancelled_ && pressed_.localLayer && pressed_.layer == left[row];
    const Color leftColor = usable ? kLeftColors[row] : kShade;
    const Rect leftCap{layout::kEdgePadding, l.y + 3, 10, 114};
    const float leftLed = animator.LedShown(KeyIndex(row, 0));
    const int leftDensity = std::clamp(static_cast<int>(1 + leftLed), 1, 16);
    KeyCap(g, leftCap, leftColor, selected || leftDown, false, leftDensity);
    if (!selected && !leftDown) g.Fill(Rect{leftCap.x + 1, l.y + 53, 8, 11}, kBlack);
    g.DrawGlyph(leftCap.x + 2, l.y + 55, kLeftGlyphs[row], selected || leftDown ? kBlack : leftColor);
    g.TextAligned(Rect{20, l.y + 54, 26, 12}, modal && row == 0 ? "BACK" : kLeftNames[row],
                  usable ? selected ? kLeftColors[row] : kDim : kShade, Font::Micro, Align::Center);
    if (usable)
    {
      if (modal && row == 0) AddHit(l, {Kind::Back});
      else if (modal && row == 2 && context) AddHit(l, {Kind::Context});
      else if (!modal) AddLayerHit(l, left[row]);
      else if (row == 2 && modalKind == ModalKind::Menu) AddHit(l, {Kind::Back});
    }

    const bool rightUsable = !saving && (layer_ == Layer::None || row == 3);
    const bool down = pointerDown_ && !pointerCancelled_ && pressed_.side && pressed_.command.index == static_cast<int>(kRight[row]);
    const Color rightColor = !rightUsable ? kShade : row == 3 ? recArmed ? kRec : playing ? kPlay : kDim : kDim;
    const Rect rightCap{layout::kWidth - layout::kEdgePadding - 10, r.y + 3, 10, 114};
    const float rightLed = animator.LedShown(KeyIndex(row, kColumns - 1));
    KeyCap(g, rightCap, down && rightColor == kDim ? kInk : rightColor, down, true,
           std::clamp(static_cast<int>(1 + rightLed), 1, 16));
    if (!down) g.Fill(Rect{rightCap.x + 1, r.y + 53, 8, 11}, kBlack);
    g.DrawGlyph(rightCap.x + 2, r.y + 55, rightGlyphs[row], down ? kBlack : rightColor);
    g.TextAligned(Rect{433, r.y + 54, 30, 12}, row == 3 && playing ? "STOP" : kRightNames[row],
                  rightUsable ? down ? kInk : rightColor : kShade, Font::Micro, Align::Center);
    if (rightUsable) AddHit(r, {Kind::SideDown, static_cast<int>(kRight[row])}, true);
  }
}

void View::Tabs(Gfx& g, const Controller& controller, Animator& animator)
{
  g.DottedHLine(56, 433, 368, kShade);
  render::PageTabsStyle style;
  style.left = 58;
  style.right = 422;
  style.y = 451;
  style.font = Font::Small;
  style.gap = 9;
  style.highlightOffsetY = -2;
  style.highlightHeight = 13;
  style.arrowSpace = 9;
  style.counterGap = 11;
  style.patternCounterGap = 8;
  const auto tabs = render::DrawPageTabs(g, controller, animator, style);
  if (controller.TopModal() != nullptr || controller.HostView().Sampler().SavingTrack() >= 0) return;
  const auto hit = [&](const Rect& rect, std::size_t page)
  {
    AddHit(Rect{rect.x, 432, rect.width, 32}, {Kind::SelectPage, static_cast<int>(page)});
  };
  for (const auto& tab : tabs.tabs) hit(tab.bounds, tab.page);
  if (!tabs.tabs.empty())
  {
    if (tabs.moreBefore) hit(tabs.previous, tabs.tabs.front().page - 1);
    if (tabs.moreAfter) hit(tabs.next, tabs.tabs.back().page + 1);
  }
}

void View::Layers(Gfx& g, const Controller& controller, Animator& animator, Layer layer, float progress)
{
  const auto index = static_cast<std::size_t>(layer) - 1;
  const Color accent = kLeftColors[index];
  const Rect source{layout::kEdgePadding, static_cast<int>(index) * 120 + 3, 10, 114};
  (void)render::GrowOverlay(g, layout::kCenter, layout::kCenter, source, accent, progress);
  if (layer == Layer::Scenes)
  {
    static constexpr std::array<const char*, 4> detail{{"PROJECT / DEVICE", "SOUND / PARAMETERS", "PATTERNS / STEPS", "SONG / MIXER"}};
    for (std::size_t i = 0; i < 4; ++i)
    {
      const Rect rect = render::DropInTile(layout::Span(i / 2 * 2, i % 2 * 2, 2, 2), progress, i * 2, 12);
      const bool current = static_cast<std::size_t>(controller.State().scene) == i;
      MapTile(g, rect, std::to_string(i) + " " + kSceneNames[i], detail[i], current, kEsc, animator.Now());
      render::PaintGridRipple(g, animator, i, rect, kEsc);
      const auto count = controller.PageCount(static_cast<Scene>(i));
      for (std::size_t page = 0; page < count && page < 6; ++page)
        g.TextFit(rect.x + 8, rect.y + 37 + static_cast<int>(page) * 16, rect.width - 16,
                  controller.PageTab(static_cast<Scene>(i), page), current ? kBlack : kDim, Font::Small);
      AddHit(rect, {Kind::SelectScene, static_cast<int>(i)});
    }
  }
  else if (layer == Layer::Tracks)
  {
    for (std::size_t track = 0; track < kTrackCount; ++track)
    {
      const Rect rect = render::DropInTile(layout::Span(track, 0, 1, 4), progress, track, 12);
      const auto* engine = controller.HostView().Engine(track);
      MapTile(g, rect, "T" + std::to_string(track + 1) + "  " + std::string(EngineShortName(controller.HostView().EngineKind(track))),
             engine == nullptr ? "NO ENGINE" : engine->CurrentPresetName(), controller.State().track == track, TrackColor(track), animator.Now());
      widgets::LevelBar(g, Rect{rect.x + 198, rect.y + 52, 154, 5}, animator.MeterPeak(track),
                        controller.State().track == track ? kBlack : TrackColor(track), controller.State().track == track ? kDim : kFaint);
      render::PaintGridRipple(g, animator, track, rect, kTab);
      AddHit(rect, {Kind::SelectTrack, static_cast<int>(track)});
    }
  }
  else if (layer == Layer::Pages)
  {
    const auto count = controller.PageCount(controller.State().scene);
    for (std::size_t page = 0; page < count && page < 16; ++page)
    {
      const Rect rect = render::DropInTile(layout::Cell(page), progress, page / 4 + page % 4, 12);
      MapTile(g, rect, controller.PageTab(controller.State().scene, page), controller.PageName(controller.State().scene, page),
             controller.CurrentPage() == page, kCtrl, animator.Now());
      render::PaintGridRipple(g, animator, page, rect, kCtrl);
      AddHit(rect, {Kind::SelectPage, static_cast<int>(page)});
    }
  }
  else if (layer == Layer::Menu)
  {
    const auto menu = controller.Menu();
    for (std::size_t i = 0; i < menu.size(); ++i)
    {
      if (menu[i].action == MenuAction::None) continue;
      const Rect rect = render::DropInTile(layout::Cell(i), progress, i / 4 + i % 4, 12);
      Action(g, rect, menu[i].label, menu[i].detail, menu[i].on, kFn, menu[i].available);
      render::PaintGridRipple(g, animator, i, rect, kFn);
      if (menu[i].available) AddHit(rect, {Kind::RunMenu, static_cast<int>(i)});
    }
  }
}

void View::Render(Gfx& g, Controller& controller, Animator& animator)
{
  const auto& state = controller.State();
  const auto now = animator.Now();
  const auto generation = Generation(controller);
  if (renderGeneration_ != generation) { SetLayer(controller, Layer::None, now); closingLayer_ = Layer::None; }
  hitCount_ = 0;
  performanceHitCount_ = 0;
  renderGeneration_ = generation;
  g.SetOrigin(0, 0);g.ResetClip();g.Clear(kBlack);
  const auto sinceBoot = now - animator.BootAt();
  if (sinceBoot < kBootMs - 300)
  {
    g.SetClip(layout::kCenter);Boot(g, controller, animator);g.ResetClip();
  }
  else
  {
    struct Context { View* view; const Controller* controller; Animator* animator; } context{this, &controller, &animator};
    render::ContentPainters painters;
    painters.context = &context;
    painters.scene = [](void* value, Gfx& painter, Scene scene, std::uint8_t page, bool current) {
      auto& ctx = *static_cast<Context*>(value);
      ctx.view->registerHits_ = current && !ctx.animator->TransitionActive() && ctx.view->layer_ == Layer::None &&
          ctx.animator->ClosingModal() == nullptr;
      ctx.view->SceneContent(painter, *ctx.controller, *ctx.animator, scene, page);
    };
    painters.modal = [](void* value, Gfx& painter, const Modal& modal, bool current) {
      auto& ctx = *static_cast<Context*>(value);
      ctx.view->registerHits_ = current && ctx.animator->Now() - ctx.animator->ModalOpenedAt() >= kModalOpenMs;
      ctx.view->ModalContent(painter, *ctx.controller, *ctx.animator, modal);
    };
    render::ContentFrameStyle content;
    content.area = layout::kCenter;content.sceneGap = 10;content.pageTravel = layout::kCenter.width / 2 + 16;
    render::DrawContentFrame(g, controller, animator, content, painters);
    g.SetOrigin(0, 0);g.SetClip(layout::kCenter);
    render::DrawRecordFrame(g, Rect{50, 50, 380, 380}, controller, animator);
    if (layer_ != Layer::None)
    {
      hitCount_ = 0;performanceHitCount_ = 0;
      const float progress = fx::EaseOutCubic(fx::Progress(now, layerOpenedAt_, kOverlayGrowMs));
      registerHits_ = progress >= 0.99f;
      Layers(g, controller, animator, layer_, progress);
    }
    else if (closingLayer_ != Layer::None && now - layerClosedAt_ < kOverlayCloseMs)
    {
      hitCount_ = 0;performanceHitCount_ = 0;registerHits_ = false;
      const float progress = 1 - fx::EaseInOutCubic(fx::Progress(now, layerClosedAt_, kOverlayCloseMs));
      Layers(g, controller, animator, closingLayer_, progress);
    }
    else closingLayer_ = Layer::None;
  }
  g.SetOrigin(0, 0);g.ResetClip();registerHits_ = true;
  // The established status bar and compact page strip stay in their positions.
  Header(g, controller, animator);Tabs(g, controller, animator);Sides(g, controller, animator);
  const bool menuVisible = layer_ != Layer::None || controller.MenuOpen();
  if (!menuVisible && controller.TopModal() == nullptr && sinceBoot >= kBootMs)
  {
    render::HudStyle hud;
    hud.box = Rect{64, 374, 352, 48};hud.labelFont = Font::Small;hud.valueFont = Font::Large;
    hud.labelY = 7;hud.valueY = 23;hud.meterWidth = 130;hud.meterHeight = 6;hud.meterY = 27;
    hud.valueMeterReserve = 160;
    if (state.toast.text.empty()) render::DrawHud(g, controller, animator, hud);
    render::ToastStyle toast;
    toast.band = Rect{56, 464, 368, 14};toast.textX = 65;toast.textY = 466;
    toast.markerX = 56;toast.markerY = 465;toast.maxCharacters = 58;
    (void)render::DrawToast(g, controller, animator, toast);
  }
  g.SetClip(layout::kCenter);
  render::DrawSessionAndPanic(g, layout::kCenter, controller, animator);
  render::DrawBootHandoff(g, layout::kCenter, animator);
  g.ResetClip();ClearCorners(g);
}

void View::EndPointer(Controller& controller, std::uint32_t nowMs)
{
  if (pointerDown_ && pressed_.side && !pointerCancelled_)
    (void)controller.Dispatch({Kind::SideUp, pressed_.command.index}, nowMs);
  pointerDown_ = false;
  pointerCancelled_ = false;
  pressed_ = {};
}

void View::Cancel(Controller& controller, std::uint32_t nowMs)
{
  CancelPerformance(controller, nowMs, true);
  (void)controller.Dispatch({Kind::PerformanceCancelAll}, nowMs);
  (void)controller.Dispatch({Kind::Cancel}, nowMs);
  pointerDown_ = false;
  pointerCancelled_ = false;
  pressed_ = {};
  SetLayer(controller, Layer::None, nowMs);
  closingLayer_ = Layer::None;
}

void View::Tick(Controller& controller, std::uint32_t nowMs)
{
  const auto generation = Generation(controller);
  if (generation != renderGeneration_ && layer_ != Layer::None) SetLayer(controller, Layer::None, nowMs);
  for (auto& pointer : performancePointers_)
  {
    if (!pointer.active || pointer.cancelled) continue;
    if (pointer.generation != generation || layer_ != Layer::None || controller.TopModal() != nullptr ||
        controller.HostView().Sampler().SavingTrack() >= 0)
    {
      if (pointer.note >= 0) (void)controller.Dispatch({Kind::PerformanceUp, pointer.id}, nowMs);
      pointer.note = -1;
      pointer.cancelled = true;
    }
  }
  if (pointerDown_ && !pointerCancelled_ && pressGeneration_ != Generation(controller))
  {
    (void)controller.Dispatch({Kind::Cancel}, nowMs);
    pointerCancelled_ = true;
  }
}

bool View::Pointer(Controller& controller, const Device::Touch::Event& event, std::uint32_t nowMs)
{
  if (event.kind == Device::Touch::Kind::Cancel)
  {
    Cancel(controller, nowMs);
    return true;
  }
  for (auto& pointer : performancePointers_)
  {
    if (!pointer.active || pointer.id != event.id) continue;
    if (event.kind == Device::Touch::Kind::Up)
    {
      if (pointer.note >= 0) (void)controller.Dispatch({Kind::PerformanceUp, pointer.id}, nowMs);
      pointer = {};
      return true;
    }
    if (event.kind != Device::Touch::Kind::Move) return true;
    const auto generation = Generation(controller);
    if (!pointer.cancelled && (pointer.generation != generation || renderGeneration_ != generation ||
        layer_ != Layer::None || controller.TopModal() != nullptr || controller.HostView().Sampler().SavingTrack() >= 0))
    {
      if (pointer.note >= 0) (void)controller.Dispatch({Kind::PerformanceUp, pointer.id}, nowMs);
      pointer.note = -1;
      pointer.cancelled = true;
    }
    if (pointer.cancelled) return true;
    const int note = PerformanceNoteAt(event.x, event.y);
    if (note == pointer.note) return true;
    if (pointer.note >= 0) (void)controller.Dispatch({Kind::PerformanceUp, pointer.id}, nowMs);
    pointer.note = -1;
    if (note >= 0 && controller.Dispatch({Kind::PerformanceDown, pointer.id, note}, nowMs)) pointer.note = note;
    return true;
  }
  if (event.kind == Device::Touch::Kind::Down)
  {
    if (pointerDown_) return false;
    if (renderGeneration_ != Generation(controller)) return false;
    if (layer_ == Layer::None && controller.TopModal() == nullptr && controller.HostView().Sampler().SavingTrack() < 0)
    {
      const int note = PerformanceNoteAt(event.x, event.y);
      if (note >= 0)
      {
        for (auto& pointer : performancePointers_)
        {
          if (pointer.active) continue;
          if (!controller.Dispatch({Kind::PerformanceDown, event.id, note}, nowMs)) return false;
          controller.NotifyNativeInput(nowMs);
          pointer = {true, false, event.id, note, renderGeneration_};
          return true;
        }
        return false;
      }
    }
    for (std::size_t i = hitCount_; i > 0; --i)
    {
      if (!layout::Contains(hits_[i - 1].rect, event.x, event.y)) continue;
      controller.NotifyNativeInput(nowMs);
      CancelPerformance(controller, nowMs);
      pressed_ = hits_[i - 1];
      pointerId_ = event.id;
      pointerDown_ = true;
      pointerCancelled_ = false;
      pressGeneration_ = Generation(controller);
      if (pressed_.side)
      {
        SetLayer(controller, Layer::None, nowMs);
        (void)controller.Dispatch(pressed_.command, nowMs);
        // ENTER can open a modal immediately. The release still belongs to this
        // gesture, but repeat must not carry into the new view.
        if (Generation(controller) != pressGeneration_)
        {
          (void)controller.Dispatch({Kind::Cancel}, nowMs);
          pointerCancelled_ = true;
        }
      }
      else if (!pressed_.localLayer && pressed_.command.index >= 0)
      {
        int cell = -1;
        switch (pressed_.command.kind)
        {
          case Kind::FocusCell: case Kind::RunMenu: case Kind::SelectPattern: cell = pressed_.command.index; break;
          case Kind::SelectStep: cell = pressed_.command.index % static_cast<int>(kGridCount); break;
          case Kind::SelectSongCell:
            cell = (pressed_.command.index - static_cast<int>(controller.State().songScroll)) * 4 + pressed_.command.value; break;
          case Kind::SelectSessionCell:
            cell = (pressed_.command.index - static_cast<int>(controller.State().sessionScroll)) * 4 + pressed_.command.value; break;
          case Kind::SelectScene: case Kind::SelectTrack:
            if (layer_ != Layer::None) cell = pressed_.command.index;
            break;
          case Kind::SelectPage:
            if (layer_ == Layer::Pages) cell = pressed_.command.index;
            break;
          default: break;
        }
        if (cell >= 0 && cell < static_cast<int>(kGridCount)) controller.NotifyNativeCellPress(static_cast<std::size_t>(cell), nowMs);
      }
      return true;
    }
    return false;
  }
  if (!pointerDown_ || pointerId_ != event.id) return false;
  if (event.kind == Device::Touch::Kind::Move)
  {
    if (!pointerCancelled_ && (!layout::Contains(pressed_.rect, event.x, event.y) || pressGeneration_ != Generation(controller)))
    {
      (void)controller.Dispatch({Kind::Cancel}, nowMs);
      pointerCancelled_ = true;
    }
    return true;
  }
  if (event.kind == Device::Touch::Kind::Up)
  {
    const Hit hit = pressed_;
    const bool activate = !pointerCancelled_ && pressGeneration_ == Generation(controller) && layout::Contains(hit.rect, event.x, event.y);
    EndPointer(controller, nowMs);
    if (activate && !hit.side)
    {
      if (hit.localLayer)
      {
        SetLayer(controller, layer_ == hit.layer ? Layer::None : hit.layer, nowMs);
      }
      else
      {
        (void)controller.Dispatch(hit.command, nowMs);
        SetLayer(controller, Layer::None, nowMs);
      }
    }
    return true;
  }
  return false;
}

}  // namespace groove::native
