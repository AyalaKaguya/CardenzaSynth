#pragma once

#include <cstdint>
#include <vector>

#include "Applications/Synth/UI/Groove/GrooveController.h"
#include "Applications/Synth/UI/Groove/Render/Animator.h"
#include "Applications/Synth/UI/Groove/Render/Gfx.h"
#include "Applications/Synth/UI/Groove/Render/Theme.h"

namespace groove {

// Draws one frame. The animator must have been updated for this frame.
void RenderFrame(Gfx& g, const Controller& controller, Animator& animator);

namespace render {

// The original normalized graph parameter glide, shared by display layouts.
float GraphParameter(Animator& animator, std::size_t track, const synth::ISynthEngine* engine,
                     const char* id, float fallback = 0.0f);

struct ContentFrameStyle {
  Rect area{8, 12, 223, 112};
  int sceneGap = 6;
  int pageTravel = 119;
};
struct ContentPainters {
  void* context = nullptr;
  void (*scene)(void*, Gfx&, Scene, std::uint8_t, bool) = nullptr;
  void (*modal)(void*, Gfx&, const Modal&, bool) = nullptr;
};
// Paint callbacks receive false for outgoing/closing content. They must not
// register input targets for those historical views.
void DrawContentFrame(Gfx& g, const Controller& controller, Animator& animator,
                      const ContentFrameStyle& style, const ContentPainters& painters);
void DrawRecordFrame(Gfx& g, const Rect& content, const Controller& controller, Animator& animator, int thickness = 2);
void DrawSessionAndPanic(Gfx& g, const Rect& bounds, const Controller& controller, Animator& animator);
void DrawSessionSweep(Gfx& g, const Rect& bounds, const Controller& controller, Animator& animator);
void DrawPanic(Gfx& g, const Rect& bounds, const Controller& controller, Animator& animator);
void DrawBootHandoff(Gfx& g, const Rect& bounds, Animator& animator);
Rect GrowOverlay(Gfx& g, const Rect& content, const Rect& panel, const Rect& source, Color accent, float progress);
Rect DropInTile(const Rect& rect, float progress, std::size_t order, int distance = 8);
void DrawMapTile(Gfx& g, const Rect& rect, bool selected, bool usable, Color accent, std::uint32_t now,
                  Color fill = theme::kInk, bool dottedOutline = false);
struct BootStyle {
  Rect bounds{0, 0, 240, 135};
  int titleY = 40;
  int subtitleY = 70;
  int dashY = 88;
  int dashLeft = 82;
  int dashPitch = 20;
  int dashWidth = 16;
  int dashHeight = 4;
  Font subtitleFont = Font::Micro;
};
void DrawBoot(Gfx& g, Animator& animator, const BootStyle& style);

struct HudStyle {
  Rect box{14, 95, 211, 26};
  Font labelFont = Font::Micro;
  Font valueFont = Font::Large;
  int padding = 8;
  int labelY = 3;
  int valueY = 11;
  int meterWidth = 58;
  int meterHeight = 6;
  int meterY = 15;
  int valueMeterReserve = 82;
};
void DrawHud(Gfx& g, const Controller& controller, Animator& animator, const HudStyle& style);
struct ToastStyle {
  Rect band{10, 123, 219, 11};
  int textX = 20;
  int textY = 125;
  int markerX = 12;
  int markerY = 124;
  int markerHeight = 9;
  int maxCharacters = 33;
  Font font = Font::Micro;
};
bool DrawToast(Gfx& g, const Controller& controller, Animator& animator, const ToastStyle& style, bool drawExit = true);

struct PageTabsStyle {
  int left = 12;
  int right = 227;
  int y = 125;
  Font font = Font::Micro;
  int gap = 7;
  int highlightPadding = 2;
  int highlightOffsetY = -1;
  int highlightHeight = 9;
  int arrowSpace = 7;
  int counterGap = 9;
  int patternCounterGap = 6;
};

struct PageTabPosition {
  std::size_t page = 0;
  Rect bounds{};
};
struct PageTabsLayout {
  std::vector<PageTabPosition> tabs;
  Rect previous{};
  Rect next{};
  bool moreBefore = false;
  bool moreAfter = false;
};

// The original compact page strip. Its returned geometry is also the input
// map for touch targets, so alternate surfaces share layout and rendering.
PageTabsLayout DrawPageTabs(Gfx& g, const Controller& controller, Animator& animator,
                            const PageTabsStyle& style = {});

// Scene content for an explicit page, so transitions can draw the outgoing page too (SceneRender.cpp).
void SceneContent(Gfx& g, const Controller& controller, Animator& animator, Scene scene, std::uint8_t page);
// The FN panel page (held FN or SETUP), drawn like an INSTR page (SceneRender.cpp).
// The FN menu of the current screen on the GRID cells (held FN and the open menu); `t` drops the tiles in.
void MenuContent(Gfx& g, const Controller& controller, Animator& animator, float t);
// Modal content replaces the content area (ModalRender.cpp).
void ModalContent(Gfx& g, const Controller& controller, Animator& animator, const Modal& modal);
const char* ModalHint(const Controller& controller, const Modal& modal);

}  // namespace render

}  // namespace groove
