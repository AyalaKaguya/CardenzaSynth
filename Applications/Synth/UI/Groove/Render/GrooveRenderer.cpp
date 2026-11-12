#include "Applications/Synth/UI/Groove/Render/GrooveRenderer.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

#include "Applications/Synth/UI/Groove/Render/Effects.h"
#include "Applications/Synth/UI/Groove/Render/Theme.h"
#include "Applications/Synth/UI/Groove/Render/Widgets.h"

namespace groove {

namespace {

using namespace groove::theme;

// The left keys' colors, read through the palette in use.
constexpr std::array<const Color*, 4> kStripColors{{&kEsc, &kTab, &kFn, &kCtrl}};
const Rect kContentArea{kContentX - 2, kContentY - 2, kContentWidth + 4, kContentHeight + 4};

Rect StripRect(std::size_t row)
{
  const int pitch = kScreenHeight / 4 + 1;  // 34
  return Rect{0, static_cast<int>(row) * pitch, kStripWidth, pitch - 3};
}

// A key cap at the screen edge: dithered body, a solid edge facing the content, clipped corners.
void DrawKeyCap(Gfx& g, const Rect& cap, Color color, int level, bool edgeOnRight)
{
  g.Dither(cap, color, level);
  g.VLine(edgeOnRight ? cap.x + cap.width - 1 : cap.x, cap.y, cap.height, color);
  g.Pixel(cap.x, cap.y, kBlack);
  g.Pixel(cap.x + cap.width - 1, cap.y, kBlack);
  g.Pixel(cap.x, cap.y + cap.height - 1, kBlack);
  g.Pixel(cap.x + cap.width - 1, cap.y + cap.height - 1, kBlack);
}

bool StripSolid(float led) { return led >= 13.5f; }

// Light behind the icons on both edges, so that the icons are what you see: off barely there, half
// a little more, then up to solid with the key's LED.
int CapDither(float led)
{
  const float level = led <= kLedHalf ? 1.0f + led * (2.0f / kLedHalf) : 3.0f + (led - kLedHalf) * (13.0f / (kLedFull - kLedHalf));
  return std::clamp(static_cast<int>(std::lround(level)), 1, 16);
}

// The four left caps stand for the four left keys, row for row, each with its icon in its color: ESC
// scenes and back, TAB tracks, FN the menu, CTRL pages. Where you are is in the header and the page
// tabs, not here.
void DrawStrips(Gfx& g, const Controller& controller, Animator& animator)
{
  static constexpr std::array<Glyph, 4> kIcons{{Glyph::Back, Glyph::Tab, Glyph::Menu, Glyph::Pages}};
  const UiState& state = controller.State();
  const std::uint32_t now = animator.Now();
  for (std::size_t row = 0; row < 4; ++row)
  {
    const Rect cap = StripRect(row);
    const Color color = *kStripColors[row];
    const auto& key = state.left[row];
    const float led = animator.LedShown(KeyIndex(row, 0));
    const bool solid = !key.down && StripSolid(led);
    if (solid) g.Fill(cap, color);
    else g.Dither(cap, color, CapDither(led));
    if (key.down)
    {
      // Pressed: the cap fills from the bottom in a quick sweep.
      const float fill = fx::EaseOutCubic(fx::Progress(now, key.pressedAt == 0 ? 1 : key.pressedAt, 110));
      const int height = static_cast<int>(fill * static_cast<float>(cap.height));
      g.Fill(Rect{cap.x, cap.y + cap.height - height, cap.width, height}, color);
    }
    const Glyph icon = kIcons[row];
    const int x = cap.x + (cap.width - Gfx::GlyphWidth(icon)) / 2;
    const int y = cap.y + (cap.height - 7) / 2;
    const bool dark = solid || key.down;
    if (!dark) g.Fill(Rect{cap.x, y - 1, cap.width, 9}, kBlack);
    g.DrawGlyph(x, y, icon, dark ? kBlack : color);
  }
}

// The four right caps stand for ADD, SUB, ENTER and PLAY, top to bottom.
// The right caps mirror ADD / SUB / ENTER / PLAY like the left ones: off faint, half 30 %, full
// solid with a black glyph. PLAY is green while playing and red with recording armed.
void DrawGlyphColumn(Gfx& g, const Controller& controller, Animator& animator)
{
  const std::uint32_t now = animator.Now();
  const auto& sequencer = controller.HostView().Sequencer();
  const bool playing = sequencer.Playing();
  const bool recArmed = sequencer.RecordArmed();
  // The icons on ADD and SUB say what they do here: up / down for a value and for a list alike (ADD,
  // the upper key, raises the value and goes up the list).
  Glyph add = Glyph::Up;
  Glyph sub = Glyph::Down;
  switch (controller.AdjustKeysMean())
  {
    case AdjustMeaning::List: add = Glyph::Up; sub = Glyph::Down; break;
    case AdjustMeaning::Sideways: add = Glyph::Right; sub = Glyph::Left; break;
    case AdjustMeaning::Erase: add = Glyph::Delete; sub = Glyph::None; break;
    case AdjustMeaning::None: add = Glyph::None; sub = Glyph::None; break;
    case AdjustMeaning::Value:
    default: break;
  }
  const std::array<Glyph, 4> glyphs{{add, sub, Glyph::Enter, Glyph::Play}};
  for (std::size_t row = 0; row < 4; ++row)
  {
    const Rect strip = StripRect(row);
    const Rect cap{kGlyphColumnX, strip.y, kScreenWidth - kGlyphColumnX, strip.height};
    const float led = animator.LedShown(KeyIndex(row, kColumns - 1));
    Color body = kDim;
    Glyph glyph = glyphs[row];
    if (row == 3)
    {
      if (recArmed) body = kRec;
      else if (playing) body = kPlay;
      if (recArmed && !playing) glyph = Glyph::Rec;
    }
    const int x = cap.x + (cap.width - Gfx::GlyphWidth(glyph)) / 2;
    const int y = cap.y + (cap.height - 7) / 2 - (row == 3 ? static_cast<int>(fx::Decay(now, animator.PlayToggledAt(), 260) * 3.0f) : 0);
    if (StripSolid(led))
    {
      g.Fill(cap, body == kDim ? kInk : body);
      g.DrawGlyph(x, y, glyph, kBlack);
      continue;
    }
    DrawKeyCap(g, cap, body, CapDither(led), false);
    if (glyph == Glyph::None) continue;
    g.Fill(Rect{cap.x, y - 1, cap.width, 9}, kBlack);
    g.DrawGlyph(x, y, glyph, led < 0.5f ? kDim : body == kDim ? kInk : body);
  }
}

void DrawHeader(Gfx& g, const Controller& controller, Animator& animator)
{
  const UiState& state = controller.State();
  const Host& host = controller.HostView();
  const std::uint32_t now = animator.Now();
  const auto seq = host.Sequencer().BuildUiState(state.track);

  // Scene label decodes in with every scene change.
  const auto& transition = animator.CurrentTransition();
  const float sceneDecode = transition.kind == Animator::TransitionKind::Scene ? fx::Progress(now, transition.start, 320) : 1.0f;
  int x = kContentX;
  if (controller.MenuOpen())
  {
    x = g.Text(x, kHeaderY, "FN", kFn);
    x = fx::DecodeText(g, x + 3, kHeaderY, "MENU", kInk, Font::Micro, fx::Progress(now, animator.ModalOpenedAt(), 320), now / 40);
  }
  else
  {
    // Scenes are numbered from 0 (SETUP).
    const char sceneNumber[2] = {static_cast<char>('0' + static_cast<int>(state.scene)), 0};
    x = g.Text(x, kHeaderY, sceneNumber, kEsc);
    x = fx::DecodeText(g, x + 3, kHeaderY, kSceneNames[static_cast<std::size_t>(state.scene)], kInk, Font::Micro, sceneDecode, now / 40);
  }
  x += 6;

  // Track label: a block in the track's own color; it widens on a track change and flashes white
  // on every note played.
  const std::string track = "T" + std::to_string(state.track + 1);
  const float trackFlash = fx::Decay(now, animator.TrackChangedAt(), 360);
  const float notePulse = fx::Decay(now, animator.LastNoteAt(), 160);
  const int grow = static_cast<int>(trackFlash * 4.0f);
  g.Fill(Rect{x - 2 - grow, 0, Gfx::Measure(track) + 4 + grow * 2, 10}, notePulse > 0.5f ? kInk : TrackColor(state.track));
  x = g.Text(x, kHeaderY, track, kBlack);
  x += 5;
  const auto* engine = host.Engine(state.track);
  x = g.Text(x, kHeaderY, EngineShortName(host.EngineKind(state.track)), kDim);
  x += 4;

  // Right cluster: REC, transport, BPM (pulses on the beat), unsaved marker.
  char bpm[8];
  std::snprintf(bpm, sizeof(bpm), "%u", static_cast<unsigned>(seq.bpm));
  int right = kContentRight;
  // A lesson page on a computer drives the device (the guide): its working state is set aside.
  if (controller.Guide())
  {
    right -= Gfx::Measure("GUIDE");
    g.Text(right, kHeaderY, "GUIDE", kInk);
    right -= 6;
  }
  else if (state.dirty)
  {
    g.Text(right - 6, kHeaderY, "*", (now / 700) % 2 ? kFn : kDim);
    right -= 9;
  }
  right -= Gfx::Measure(bpm);
  const float beat = seq.playing ? fx::Decay(now, animator.BeatAt(), 150) : 0.0f;
  g.Text(right, kHeaderY, bpm, beat > 0.3f ? (animator.Downbeat() ? kPlay : kInk) : (seq.playing ? kInk : kDim));
  right -= 9;
  if (seq.playing) g.DrawGlyph(right, kHeaderY, Glyph::Play, kPlay);
  else g.DrawGlyph(right, kHeaderY, Glyph::Stop, kFaint);
  right -= 4;
  if (seq.recordArmed)
  {
    right -= Gfx::Measure("REC") + 2;
    if ((now / 250) % 2 || !seq.playing) g.Text(right, kHeaderY, "REC", kRec);
  }
  if (seq.songMode && seq.songLength > 0)
  {
    char song[12];
    std::snprintf(song, sizeof(song), "R%02u", static_cast<unsigned>(seq.songRow + 1));
    right -= Gfx::Measure(song) + 4;
    g.Text(right, kHeaderY, song, kFn);
  }
  if (engine != nullptr)
  {
    const std::uint32_t changed = std::max(animator.PresetChangedAt(state.track), animator.TrackChangedAt());
    fx::DecodeText(g, x, kHeaderY, engine->CurrentPresetName(), kInk, Font::Micro, fx::Progress(now, changed, 380), changed,
                   right - x - 6);
  }

  // Rule: dotted in the track's color; while playing, the loop (or song) progress with a head.
  g.DottedHLine(kContentX, kHeaderRuleY, kContentWidth, TrackColor(state.track), 2);
  if (seq.songPlaying && seq.songLength > 0)
  {
    // Song mode: the rule is the whole tape, one tick per row, the head at the song position.
    for (std::size_t row = 0; row <= seq.songLength; ++row)
      g.Pixel(kContentX + static_cast<int>(row * static_cast<std::size_t>(kContentWidth) / seq.songLength), kHeaderRuleY - 1, kFn);
    const float position = (static_cast<float>(seq.songRow) + seq.songRowProgress) / static_cast<float>(seq.songLength);
    const int progress = static_cast<int>(position * static_cast<float>(kContentWidth));
    g.HLine(kContentX, kHeaderRuleY, std::max(1, progress), kFn);
    g.Fill(Rect{kContentX + progress - 1, kHeaderRuleY - 1, 3, 3}, kInk);
  }
  else if (seq.playing)
  {
    const int progress = static_cast<int>(seq.patternProgress * static_cast<float>(kContentWidth));
    g.HLine(kContentX, kHeaderRuleY, std::max(1, progress), kPlay);
    g.Fill(Rect{kContentX + progress - 1, kHeaderRuleY - 1, 3, 3}, kInk);
  }
  else if (animator.StopAt() != 0 && now - animator.StopAt() < kRewindMs)
  {
    // Stop: the progress line rewinds to the start with its comet head.
    const float rewind = fx::EaseOutCubic(fx::Progress(now, animator.StopAt(), kRewindMs));
    const int progress = static_cast<int>(animator.StopProgress() * (1.0f - rewind) * static_cast<float>(kContentWidth));
    g.HLine(kContentX, kHeaderRuleY, std::max(1, progress), kPlay);
    for (int tail = 1; tail <= 10; ++tail) g.Dither(Rect{kContentX + progress + tail, kHeaderRuleY - 1, 1, 1}, kPlay, 12 - tail);
    g.Fill(Rect{kContentX + progress - 1, kHeaderRuleY - 1, 3, 3}, kInk);
  }
}

// Page tabs. When they do not all fit, a window of tabs around the current page is shown with
// arrows where more pages continue, and a counter (current / total) on the right.
void DrawTabs(Gfx& g, const Controller& controller, Animator& animator)
{
  (void)render::DrawPageTabs(g, controller, animator);
}

void DrawFooter(Gfx& g, const Controller& controller, Animator& animator)
{
  const UiState& state = controller.State();
  const std::uint32_t now = animator.Now();
  g.DottedHLine(kContentX, kFooterY - 3, kContentWidth, kShade, 1);
  render::ToastStyle toastStyle;
  toastStyle.band = Rect{kFooterX - 2, kFooterY - 2, kFooterWidth + 4, 11};
  toastStyle.textX = kFooterX + 8;toastStyle.textY = kFooterY;
  toastStyle.markerX = kFooterX;toastStyle.markerY = kFooterY - 1;
  toastStyle.maxCharacters = kFooterWidth / 6 - 2;
  if (!state.toast.text.empty())
  {
    (void)render::DrawToast(g, controller, animator, toastStyle, false);
    return;
  }
  const float toastExit = fx::Progress(now, animator.ToastEndedAt(), kToastExitMs);
  const bool exiting = animator.ToastEndedAt() != 0 && toastExit < 1;
  if (const Modal* modal = controller.TopModal())
  {
    if (modal->kind == ModalKind::Menu)
    {
      // The open menu: whose menu it is, and how to leave.
      const std::string page = kSceneNames[static_cast<std::size_t>(state.scene)] + std::string(" ") +
                               controller.PageTab(state.scene, controller.CurrentPage());
      g.Fill(Rect{kFooterX - 2, kFooterY - 1, Gfx::Measure(page) + 4, 9}, kFn);
      g.Text(kFooterX, kFooterY, page, kBlack);
      g.Text(kFooterRight - Gfx::Measure("FN / ESC CLOSE"), kFooterY, "FN / ESC CLOSE", kFaint);
      return;
    }
    g.TextFit(kFooterX, kFooterY, kFooterWidth, render::ModalHint(controller, *modal), kDim);
    return;
  }
  const auto& sequencer = controller.HostView().Sequencer();
  if (state.scene == Scene::Sequence && controller.CurrentPage() == 0)
  {
    const auto seq = sequencer.BuildUiState(state.track);
    if (seq.hasHeldSteps)
    {
      // Step inspector: lane chooser (ENTER) with a gliding highlight, and the notes or the value.
      static constexpr std::array<const char*, 5> kLanes{{"NOTE", "LEN", "VEL", "TIME", "HITS"}};
      std::array<int, kLanes.size()> laneX{};
      int x = kFooterX;
      for (std::size_t lane = 0; lane < kLanes.size(); ++lane)
      {
        laneX[lane] = x;
        x += Gfx::Measure(kLanes[lane]) + 6;
      }
      const std::size_t lane = static_cast<std::size_t>(seq.editLane);
      const float boxX = animator.Smooth(Animator::Key("lane"), static_cast<float>(laneX[lane] - 2), 45.0f);
      const float boxW = animator.Smooth(Animator::Key("lane", 1), static_cast<float>(Gfx::Measure(kLanes[lane]) + 4), 45.0f);
      g.Fill(Rect{static_cast<int>(std::lround(boxX)), kFooterY - 1, static_cast<int>(std::lround(boxW)), 9}, kInk);
      for (std::size_t index = 0; index < kLanes.size(); ++index)
      {
        const bool covered = laneX[index] >= boxX - 1 && laneX[index] + Gfx::Measure(kLanes[index]) <= boxX + boxW + 1;
        g.Text(laneX[index], kFooterY, kLanes[index], covered ? kBlack : kFaint);
      }
      std::string notes = seq.noteNames;
      if (controller.HostView().EngineKind(state.track) == synth::SynthEngineKind::Drums)
      {
        notes = seq.primaryLowestNote >= 0 ? std::string(DrumName(seq.primaryLowestNote)) : std::string{};
        if (seq.noteCount > 1) notes += "+" + std::to_string(seq.noteCount - 1);
      }
      std::string detail = seq.valueLabel.empty() ? notes : seq.valueLabel;
      if (seq.editLane == synth::SequencerEditLane::Note) detail = notes;
      g.TextFit(x + 4, kFooterY, kFooterRight - x - 4, detail, kInk);
      return;
    }
  }
  if (state.scene == Scene::Tape && controller.CurrentPage() == kTapeMix)
  {
    const Host& host = controller.HostView();
    const bool focused = state.mixFocus >= kTrackCount;
    g.Text(kFooterX, kFooterY, "MASTER", focused ? kInk : kDim);
    if (focused) fx::MarchingAnts(g, Rect{kFooterX - 2, kFooterY - 2, Gfx::Measure("MASTER") + 4, 11}, kInk, now);
    const float master = animator.Smooth(Animator::Key("master"), static_cast<float>(host.MasterVolume()) / 100.0f);
    widgets::LevelBar(g, Rect{kFooterX + 44, kFooterY + 1, 88, 5}, master, focused ? kInk : kDim, kFaint);
    g.Text(kFooterX + 136, kFooterY, std::to_string(host.MasterVolume()), focused ? kInk : kDim);
    g.Text(kFooterX + 160, kFooterY, "OUT", kFaint);
    const float peak = animator.OutputPeakHold();
    const Rect meter{kFooterX + 180, kFooterY + 1, kFooterRight - kFooterX - 180, 5};
    widgets::LevelBar(g, meter, host.OutputPeak(), host.OutputPeak() > 0.95f ? kRec : kPlay, kFaint);
    g.VLine(meter.x + static_cast<int>(peak * static_cast<float>(meter.width - 1)), meter.y - 1, meter.height + 2, peak > 0.95f ? kRec : kInk);
    return;
  }
  DrawTabs(g, controller, animator);
  if (exiting) (void)render::DrawToast(g, controller, animator, toastStyle);

}

// Hold maps cover the content area with tiles laid out exactly like the GRID keys under the right
// hand: the tile under a key is what that key does while the left key stays down.
const Rect kMapArea{kContentX - 2, kContentY - 2, kContentWidth + 4, kContentHeight + 4};

// The map grows out of the strip of the key that summoned it, over a darkening scrim.
void BeginOverlay(Gfx& g, std::size_t index, Rect& panel, Color accent, float t)
{
  panel = render::GrowOverlay(g, kContentArea, panel, StripRect(index), accent, t);
}

// The header line names the map and how to use it while the map is up.
void DrawMapTitle(Gfx& g, const std::string& title, const std::string& hint, Color accent, float t, std::uint32_t now)
{
  g.Fill(Rect{kContentX - 1, 0, kContentWidth + 2, kHeaderRuleY + 1}, kBlack);
  const int x = fx::DecodeText(g, kContentX, kHeaderY, title, accent, Font::Micro, t * 1.5f, now / 50);
  g.TextFit(std::max(x + 8, kContentRight - Gfx::Measure(hint)), kHeaderY, kContentRight - x - 8, hint, kDim);
  g.HLine(kContentX, kHeaderRuleY, static_cast<int>(t * static_cast<float>(kContentWidth)), accent);
}

// Tiles drop into place one after another along the GRID diagonal.
Rect DropIn(const Rect& rect, float t, std::size_t order)
{
  return render::DropInTile(rect, t, order);
}

// One map tile. `current` is drawn inverse with crawling ants in the map's color; a tile the key
// cannot use right now stays dotted.
void MapTile(Gfx& g, const Rect& tile, bool current, bool usable, Color accent, std::uint32_t now, Color fill = kInk)
{
  render::DrawMapTile(g, tile, current, usable, accent, now, fill);
}

void PressRipple(Gfx& g, Animator& animator, std::size_t index, const Rect& tile, Color accent)
{
  const std::uint32_t pressed = animator.GridPressedAt(index);
  if (pressed != 0 && animator.Now() - pressed < 300) fx::Ripple(g, tile, fx::Progress(animator.Now(), pressed, 300), accent);
}

// Parameter feedback: the name, the new value in large type and a bar, popping up over everything
// for about a second after an FN function or a SETUP reset. It rises in and sinks out.
void DrawHud(Gfx& g, const Controller& controller, Animator& animator)
{
  render::HudStyle style;
  style.box = Rect{kContentX + 4, kContentBottom - 27, kContentWidth - 8, 26};
  render::DrawHud(g, controller, animator, style);
}

// ESC: four tall tiles, one per GRID column, each listing its scene's pages. The first, 0 SETUP
// (project and global settings), is reached only from here; the other three are the ring a tap on
// ESC walks.
void DrawSceneOverlay(Gfx& g, const Controller& controller, Animator& animator, float t)
{
  const std::uint32_t now = animator.Now();
  const UiState& state = controller.State();
  Rect panel = kMapArea;
  DrawMapTitle(g, "ESC  SCENES", "TAP = 1>2>3", kEsc, t, now);
  BeginOverlay(g, 0, panel, kEsc, t);
  for (std::size_t scene = 0; scene < kSceneCount; ++scene)
  {
    const Rect tile = DropIn(widgets::CellSpan(0, scene, 4, 1), t, scene * 2);
    const bool current = static_cast<std::size_t>(state.scene) == scene;
    const bool global = static_cast<Scene>(scene) == Scene::Setup;
    MapTile(g, tile, current, true, kEsc, now);
    // SETUP stands apart from the ring: a dotted rule down its right edge.
    if (global && !current) g.DottedVLine(tile.x + tile.width - 2, tile.y + 2, tile.height - 4, kEsc, 2);
    const Color ink = current ? kBlack : kInk;
    const char number[2] = {static_cast<char>('0' + scene), 0};
    g.Text(tile.x + 4, tile.y + 4, number, current ? kBlack : global ? kDim : kEsc, Font::Large);
    g.Text(tile.x + 4, tile.y + 21, kSceneNames[scene], ink, Font::Small);
    // The pages of that scene, the one it opens on marked.
    const Scene target = static_cast<Scene>(scene);
    const std::size_t count = controller.PageCount(target);
    const std::size_t open = state.page[scene];
    const std::size_t lines = static_cast<std::size_t>((tile.height - 36) / 8);
    const std::size_t first = count > lines && open >= lines ? std::min(open - lines / 2, count - lines) : 0;
    for (std::size_t line = 0; line < lines && first + line < count; ++line)
    {
      const std::size_t pageIndex = first + line;
      const int y = tile.y + 34 + static_cast<int>(line) * 8;
      const bool marked = pageIndex == std::min(open, count - 1);
      if (marked) g.Fill(Rect{tile.x + 2, y - 1, tile.width - 4, 8}, current ? kBlack : kDim);
      g.TextFit(tile.x + 4, y, tile.width - 8, controller.PageTab(target, pageIndex), marked ? (current ? kInk : kBlack) : (current ? kBlack : kDim));
    }
    for (std::size_t row = 0; row < kRows; ++row) PressRipple(g, animator, row * kGridColumns + scene, tile, kEsc);
  }
  g.ResetClip();
}

// TAB: one column per track, laid out like MIX: select, mute, solo, sequencer.
void DrawTrackOverlay(Gfx& g, const Controller& controller, Animator& animator, float t)
{
  const Host& host = controller.HostView();
  const std::uint32_t now = animator.Now();
  const auto seq = host.Sequencer().BuildUiState(controller.State().track);
  Rect panel = kMapArea;
  DrawMapTitle(g, "TAB  TRACKS", "ADD/SUB = NEXT/PREV", kTab, t, now);
  BeginOverlay(g, 1, panel, kTab, t);
  for (std::size_t track = 0; track < kTrackCount; ++track)
  {
    const bool current = track == controller.State().track;
    const Rect tile = DropIn(widgets::Cell(0, track), t, track);
    MapTile(g, tile, current, true, kTab, now, TrackColor(track));
    if (!current) g.Fill(Rect{tile.x, tile.y, tile.width, 2}, TrackColor(track));
    const Color ink = current ? kBlack : kInk;
    g.Text(tile.x + 3, tile.y + 2, "T" + std::to_string(track + 1), current ? kBlack : TrackColor(track));
    g.Text(tile.x + 18, tile.y + 2, EngineCode(host.EngineKind(track)), current ? kBlack : kDim);
    if (const auto* engine = host.Engine(track)) g.TextFit(tile.x + 3, tile.y + 11, tile.width - 6, engine->CurrentPresetName(), ink);
    const float level = animator.Smooth(Animator::Key("ovl-level", static_cast<std::uint32_t>(track)), static_cast<float>(host.TrackLevel(track)) / 100.0f);
    widgets::LevelBar(g, Rect{tile.x + 3, tile.y + tile.height - 3, tile.width - 10, 2}, level, ink, current ? kBlack : kFaint);
    const float activity = animator.MeterPeak(track);
    if (activity > 0.02f) g.Dither(Rect{tile.x + tile.width - 5, tile.y + 2, 3, tile.height - 4}, current ? kBlack : kPlay, static_cast<int>(activity * 16.0f));
    PressRipple(g, animator, track, tile, kTab);

    const std::array<bool, 3> states{{host.TrackMuted(track), host.TrackSoloed(track), seq.tracks[track].enabled}};
    const std::array<const char*, 3> names{{"MUTE", "SOLO", "SEQ"}};
    const std::array<Color, 3> colors{{kRec, kFn, kDim}};
    for (std::size_t kind = 0; kind < 3; ++kind)
    {
      const Rect cell = DropIn(widgets::Cell(kind + 1, track), t, track + kind + 1);
      MapTile(g, cell, false, true, kTab, now);
      widgets::Badge(g, Rect{cell.x + 3, cell.y + 5, cell.width - 6, 13}, names[kind], states[kind], colors[kind]);
      PressRipple(g, animator, (kind + 1) * kGridColumns + track, cell, kTab);
    }
  }
  g.ResetClip();
}

// CTRL: every page of the current scene on its GRID key.
void DrawPageOverlay(Gfx& g, const Controller& controller, Animator& animator, float t)
{
  const UiState& state = controller.State();
  const std::uint32_t now = animator.Now();
  const std::size_t count = std::min(controller.PageCount(state.scene), kGridCount);
  const std::size_t current = controller.CurrentPage();
  Rect panel = kMapArea;
  char title[32];
  std::snprintf(title, sizeof(title), "CTRL  %s PAGES", kSceneNames[static_cast<std::size_t>(state.scene)]);
  DrawMapTitle(g, title, "TAP CTRL = NEXT", kCtrl, t, now);
  BeginOverlay(g, 3, panel, kCtrl, t);
  for (std::size_t index = 0; index < kGridCount; ++index)
  {
    const std::size_t row = index / kGridColumns;
    const std::size_t column = index % kGridColumns;
    const Rect tile = DropIn(widgets::Cell(row, column), t, row + column);
    if (index >= count)
    {
      g.Pixel(tile.x + tile.width / 2, tile.y + tile.height / 2, kShade);
      continue;
    }
    const bool isCurrent = index == current;
    MapTile(g, tile, isCurrent, true, kCtrl, now);
    // The page number sits top right when the tab leaves room for it.
    const std::string tab = controller.PageTab(state.scene, index);
    char number[12];
    std::snprintf(number, sizeof(number), "%02u", static_cast<unsigned>(index + 1));
    // Long tabs drop to the small terminal font rather than truncate.
    const Font tabFont = Gfx::Measure(tab, Font::Small) <= tile.width - 5 ? Font::Small : Font::Micro;
    if (Gfx::Measure(tab, tabFont) + Gfx::Measure(number) + 8 <= tile.width)
      g.Text(tile.x + tile.width - Gfx::Measure(number) - 2, tile.y + 2, number, isCurrent ? kBlack : kCtrl);
    g.TextFit(tile.x + 3, tile.y + (tabFont == Font::Small ? 2 : 3), tile.width - 5, tab, isCurrent ? kBlack : kInk, tabFont);
    g.TextFit(tile.x + 3, tile.y + 14, tile.width - 5, controller.PageName(state.scene, index), isCurrent ? kBlack : kDim);
    PressRipple(g, animator, index, tile, kCtrl);
  }
  g.ResetClip();
}

// FN held: the menu of the current screen on the GRID (FN + GRID runs an item), whose screen it is in
// the header, and the other FN combinations in the footer. With the menu kept open (a tap on FN) the
// menu is the modal on screen instead.
void DrawFnOverlay(Gfx& g, const Controller& controller, Animator& animator, float t)
{
  if (controller.MenuOpen()) return;
  const UiState& state = controller.State();
  const std::uint32_t now = animator.Now();
  const std::string page = kSceneNames[static_cast<std::size_t>(state.scene)] + std::string(" ") +
                           controller.PageTab(state.scene, controller.CurrentPage());
  DrawMapTitle(g, "FN  MENU", page, kFn, t, now);

  // Footer: what the last item or shortcut did while it is fresh, else the FN combinations besides
  // the menu, key in yellow, what it does in white.
  g.Fill(Rect{kContentX - 1, kFooterY - 2, kContentWidth + 2, 11}, kBlack);
  if (!state.toast.text.empty())
  {
    const Color color = state.toast.kind == ToastKind::Error ? kRec : state.toast.kind == ToastKind::Ok ? kPlay : kInk;
    g.Dither(Rect{kFooterX, kFooterY - 1, 4, 9}, color, 12);
    fx::TypeText(g, kFooterX + 8, kFooterY, state.toast.text.substr(0, static_cast<std::size_t>(kFooterWidth / 6 - 2)), color, Font::Micro,
                 fx::Progress(now, animator.ToastAt(), 260));
  }
  const auto entries = controller.FnEntries();
  const std::size_t shown = static_cast<std::size_t>(fx::Clamp01(t * 1.3f) * static_cast<float>(entries.size()) + 0.5f);
  int footerX = kFooterX;
  std::size_t drawn = 0;
  for (const auto& entry : entries)
  {
    if (!entry.available || std::string_view(entry.key) == "GRID") continue;
    const int width = Gfx::Measure(entry.key) + 4 + Gfx::Measure(entry.label);
    if (footerX + width > kFooterRight) continue;
    if (drawn++ < shown && state.toast.text.empty())
    {
      const int labelX = g.Text(footerX, kFooterY, entry.key, kFn) + 4;
      g.Text(labelX, kFooterY, entry.label, kInk);
    }
    footerX += width + 8;
  }

  Rect panel = kMapArea;
  BeginOverlay(g, 2, panel, kFn, t);
  render::MenuContent(g, controller, animator, t);
  g.ResetClip();
}

void DrawContent(Gfx& g, const Controller& controller, Animator& animator)
{
  struct Context { const Controller* controller; Animator* animator; } context{&controller, &animator};
  render::ContentPainters painters;
  painters.context = &context;
  painters.scene = [](void* value, Gfx& painter, Scene scene, std::uint8_t page, bool) {
    const auto& ctx = *static_cast<Context*>(value);
    render::SceneContent(painter, *ctx.controller, *ctx.animator, scene, page);
  };
  painters.modal = [](void* value, Gfx& painter, const Modal& modal, bool) {
    const auto& ctx = *static_cast<Context*>(value);
    render::ModalContent(painter, *ctx.controller, *ctx.animator, modal);
  };
  render::DrawContentFrame(g, controller, animator, {}, painters);
}

void DrawRecordVignette(Gfx& g, const Controller& controller, Animator& animator)
{
  render::DrawRecordFrame(g, Rect{kContentX - 3, kContentY - 3, kContentWidth + 6, kContentHeight + 6}, controller, animator);
}

void DrawBoot(Gfx& g, Animator& animator)
{
  render::DrawBoot(g, animator, {});
}

}  // namespace

void render::DrawMapTile(Gfx& g, const Rect& tile, bool current, bool usable, Color accent, std::uint32_t now, Color fill)
{
  if (current)
  {
    g.Fill(tile, fill);
    fx::MarchingAnts(g, Rect{tile.x - 2, tile.y - 2, tile.width + 4, tile.height + 4}, accent, now);
    return;
  }
  g.Fill(tile, kBlack);
  if (usable)
  {
    g.Dither(tile, kFaint, 2);
    g.Stroke(tile, kDim);
  }
  else
  {
    g.DottedHLine(tile.x, tile.y, tile.width, kShade);
    g.DottedHLine(tile.x, tile.y + tile.height - 1, tile.width, kShade);
  }
}

void render::DrawBoot(Gfx& g, Animator& animator, const BootStyle& style)
{
  const std::uint32_t now = animator.Now();
  const std::uint32_t elapsed = now - animator.BootAt();
  const float t = static_cast<float>(elapsed) / static_cast<float>(kBootMs);
  // Scan line sweeping a noise field, the logotype decoding behind it.
  for (int y = 0; y < style.bounds.height; y += 3)
  {
    const std::uint32_t noise = fx::Hash(static_cast<std::uint32_t>(y) * 131u + now / 33);
    g.Dither(Rect{style.bounds.x, style.bounds.y + y, style.bounds.width, 2}, kShade, static_cast<int>(noise % 4));
  }
  const char* title = "CARDENZA";
  const int titleX = style.bounds.x + (style.bounds.width - Gfx::Measure(title, Font::Huge)) / 2;
  fx::DecodeText(g, titleX, style.titleY, title, kInk, Font::Huge, fx::Clamp01(t * 2.2f - 0.1f), now / 45);
  const char* subtitle = "GROOVEBOX";
  const int subtitleX = style.bounds.x + (style.bounds.width - Gfx::Measure(subtitle, style.subtitleFont)) / 2;
  fx::DecodeText(g, subtitleX, style.subtitleY, subtitle, kTab, style.subtitleFont, fx::Clamp01(t * 2.6f - 0.8f), now / 45);
  // Four colored dashes: the four hidden keys, landing one by one.
  for (std::size_t index = 0; index < 4; ++index)
  {
    const float landed = fx::EaseOutBack(fx::Clamp01(t * 3.0f - 0.9f - static_cast<float>(index) * 0.18f));
    const int x = style.dashLeft + static_cast<int>(index) * style.dashPitch;
    const int y = style.dashY + static_cast<int>((1.0f - landed) * 20.0f);
    if (landed > 0.0f) g.Dither(Rect{x, y, style.dashWidth, style.dashHeight}, *kStripColors[index], 10);
  }
  const int scan = static_cast<int>(fx::Clamp01(t * 1.4f) * static_cast<float>(style.bounds.height + 10));
  fx::ScanLine(g, style.bounds.x, style.bounds.width, style.bounds.y + scan, 8, kDim);
}

void render::DrawContentFrame(Gfx& g, const Controller& controller, Animator& animator, const ContentFrameStyle& style, const ContentPainters& painters)
{
  if (painters.scene == nullptr || painters.modal == nullptr) return;
  const UiState& state = controller.State();
  const std::uint32_t now = animator.Now();
  g.SetClip(style.area);
  if (const Modal* modal = controller.TopModal())
  {
    painters.modal(painters.context, g, *modal, true);
    // Modals unfold from the centre line between two bright scan lines.
    const float t = fx::EaseOutCubic(fx::Progress(now, animator.ModalOpenedAt(), kModalOpenMs));
    if (t < 1.0f)
    {
      const int middle = style.area.y + style.area.height / 2;
      const int half = static_cast<int>(t * static_cast<float>(style.area.height / 2));
      g.Fill(Rect{style.area.x, style.area.y, style.area.width, middle - half - style.area.y}, kBlack);
      g.Fill(Rect{style.area.x, middle + half, style.area.width, style.area.y + style.area.height - middle - half}, kBlack);
      g.HLine(style.area.x, middle - half, style.area.width, kInk);
      g.HLine(style.area.x, middle + half, style.area.width, kInk);
    }
    if (const Modal* closing = animator.ClosingModal(); closing != nullptr && closing != modal)
    {
      const float fold = fx::EaseInOutCubic(fx::Progress(now, animator.ClosingModalAt(), kModalCloseMs));
      const int middle = style.area.y + style.area.height / 2;
      const int half = static_cast<int>((1.0f - fold) * static_cast<float>(style.area.height / 2));
      if (half > 0 && animator.ClosingModalAt() > animator.ModalOpenedAt())
      {
        const Rect band{style.area.x, middle - half, style.area.width, half * 2};
        g.SetClip(band);
        g.Fill(band, kBlack);
        painters.modal(painters.context, g, *closing, false);
        g.SetClip(style.area);
        g.HLine(band.x, band.y, band.width, kInk);
        g.HLine(band.x, band.y + band.height - 1, band.width, kInk);
      }
    }
    g.ResetClip();
    return;
  }

  const auto& transition = animator.CurrentTransition();
  if (animator.TransitionActive() && transition.kind == Animator::TransitionKind::Scene)
  {
    // Scenes are floors, pages are rooms along one: a scene switch moves up and down, a page switch
    // sideways. Going down (direction 1: every tap on ESC, round the ring and out of SETUP; Animator's
    // SceneDirection) the new floor rises from below and the one left goes up out of sight; going up,
    // the other way. A line in the ESC color is the floor between them.
    const int kFloorGap = style.sceneGap;
    const float p = fx::EaseInOutCubic(fx::Progress(now, transition.start, transition.duration));
    const int direction = transition.direction;
    const int distance = style.area.height + kFloorGap;
    const int outgoing = -direction * static_cast<int>(p * static_cast<float>(distance));
    const int incoming = direction * static_cast<int>((1.0f - p) * static_cast<float>(distance));
    g.SetOrigin(0, outgoing);
    painters.scene(painters.context, g, transition.fromScene, transition.fromPage, false);
    g.SetOrigin(0, incoming);
    painters.scene(painters.context, g, state.scene, controller.CurrentPage(), true);
    g.SetOrigin(0, 0);
    const int floorY = direction > 0 ? style.area.y + incoming - kFloorGap / 2 - 1
                                     : style.area.y + incoming + style.area.height + kFloorGap / 2;
    g.HLine(style.area.x, floorY, style.area.width, kEsc);
  }
  else if (animator.TransitionActive())
  {
    const float p = fx::EaseInOutCubic(fx::Progress(now, transition.start, transition.duration));
    const int distance = style.pageTravel;
    const int direction = transition.direction;
    const int outgoing = -direction * static_cast<int>(p * static_cast<float>(distance));
    const int incoming = direction * static_cast<int>((1.0f - p) * static_cast<float>(distance));
    g.SetOrigin(outgoing, 0);
    painters.scene(painters.context, g, transition.fromScene, transition.fromPage, false);
    g.SetOrigin(0, 0);
    // Pages overlap: the outgoing page sinks into the dither while the new one covers it.
    fx::Fade(g, style.area, static_cast<int>(p * 14.0f));
    const int left = direction > 0 ? style.area.x + incoming : style.area.x;
    const int right = direction > 0 ? style.area.x + style.area.width : style.area.x + style.area.width + incoming;
    g.Fill(Rect{left, style.area.y, right - left, style.area.height}, kBlack);
    g.SetOrigin(incoming, 0);
    painters.scene(painters.context, g, state.scene, controller.CurrentPage(), true);
    g.SetOrigin(0, 0);
    // The seam rides the leading edge of the incoming page.
    const int seamX = direction > 0 ? style.area.x + incoming : style.area.x + style.area.width + incoming - 1;
    fx::Seam(g, seamX, style.area.y, style.area.height, direction, kCtrl);
  }
  else
  {
    painters.scene(painters.context, g, state.scene, controller.CurrentPage(), true);
  }

  // Ending: a closed modal folds shut toward the centre line over whatever lies beneath.
  if (const Modal* closing = animator.ClosingModal())
  {
    const float t = fx::EaseInOutCubic(fx::Progress(now, animator.ClosingModalAt(), kModalCloseMs));
    const int middle = style.area.y + style.area.height / 2;
    const int half = static_cast<int>((1.0f - t) * static_cast<float>(style.area.height / 2));
    if (half > 0)
    {
      const Rect band{style.area.x, middle - half, style.area.width, half * 2};
      g.SetClip(band);
      g.Fill(band, kBlack);
      painters.modal(painters.context, g, *closing, false);
      g.SetClip(style.area);
      g.HLine(band.x, band.y, band.width, kInk);
      g.HLine(band.x, band.y + band.height - 1, band.width, kInk);
    }
  }

  // Track change: a cyan scan line refreshes the content top to bottom.
  const float wipe = fx::Progress(now, animator.TrackChangedAt(), kTrackWipeMs);
  if (animator.TrackChangedAt() != 0 && wipe < 1.0f)
  {
    const int y = style.area.y + static_cast<int>(fx::EaseOutCubic(wipe) * static_cast<float>(style.area.height));
    fx::Fade(g, Rect{style.area.x, y, style.area.width, style.area.y + style.area.height - y}, 11);
    fx::ScanLine(g, style.area.x, style.area.width, y, 6, kTab);
  }
  g.ResetClip();
}

void render::DrawHud(Gfx& g, const Controller& controller, Animator& animator, const HudStyle& style)
{
  const auto& hud = controller.State().hud;
  const auto now = animator.Now();
  if (hud.at == 0 || now - hud.at >= kHudMs) return;
  const auto age = now - hud.at;
  const float in = fx::EaseOutBack(fx::Clamp01(static_cast<float>(age) / 140.0f));
  const float out = fx::Clamp01(static_cast<float>(static_cast<int>(age) - static_cast<int>(kHudMs - 220)) / 220.0f);
  Rect box = style.box;
  box.y += static_cast<int>((1.0f - in) * 8.0f + out * 6.0f);
  g.Fill(Rect{box.x - 2, box.y - 2, box.width + 4, box.height + 4}, kBlack);
  g.Fill(Rect{box.x, box.y, 3, box.height}, kFn);
  g.Stroke(box, kFn);
  g.TextFit(box.x + style.padding, box.y + style.labelY, box.width - style.padding * 2, hud.label, kFn, style.labelFont);
  const bool meter = hud.fraction >= 0.0f || hud.bipolar;
  g.TextFit(box.x + style.padding, box.y + style.valueY,
      box.width - (meter ? style.valueMeterReserve : style.padding * 2), hud.value, kInk, style.valueFont);
  if (meter)
  {
    const Rect bar{box.x + box.width - style.padding - style.meterWidth, box.y + style.meterY, style.meterWidth, style.meterHeight};
    if (hud.bipolar) widgets::BipolarBar(g, bar, std::clamp(hud.fraction, -1.0f, 1.0f), kInk, kFaint);
    else widgets::LevelBar(g, bar, hud.fraction, kInk, kFaint);
  }
  if (out > 0) fx::Fade(g, Rect{box.x - 2, box.y - 2, box.width + 4, box.height + 4}, static_cast<int>(out * 16));
}

bool render::DrawToast(Gfx& g, const Controller& controller, Animator& animator, const ToastStyle& style, bool drawExit)
{
  const auto& state = controller.State();
  const auto now = animator.Now();
  const bool active = !state.toast.text.empty();
  const float exit = fx::Progress(now, animator.ToastEndedAt(), kToastExitMs);
  if (!active && (!drawExit || animator.ToastEndedAt() == 0 || exit >= 1)) return false;
  const auto& text = active ? state.toast.text : animator.EndedToast();
  const auto kind = active ? state.toast.kind : animator.EndedToastKind();
  const Color color = kind == ToastKind::Error ? kRec : kind == ToastKind::Ok ? kPlay : kInk;
  const auto age = now - animator.ToastAt();
  const int lift = active ? static_cast<int>((1.0f - fx::EaseOutBack(fx::Progress(now, animator.ToastAt(), 220))) * 9) :
      static_cast<int>(fx::EaseInOutCubic(exit) * 11);
  const int shake = active && kind == ToastKind::Error && age < 260 ?
      static_cast<int>(std::sin(static_cast<float>(age) / 18) * 4 * (1.0f - static_cast<float>(age) / 260)) : 0;
  const float typed = active ? fx::Progress(now, animator.ToastAt(), 260) : 1.0f - exit;
  const auto clip = g.Clip();
  g.SetClip(style.band);
  const float pulse = kind == ToastKind::Error ? 0.5f + 0.5f * std::sin(static_cast<float>(now) / 70) : 1;
  g.Fill(Rect{style.band.x, style.band.y + lift, style.band.width, style.band.height}, kBlack);
  g.Dither(Rect{style.markerX + shake, style.markerY + lift, 4, style.markerHeight}, color, static_cast<int>(6 + 6 * pulse));
  fx::TypeText(g, style.textX + shake, style.textY + lift, text.substr(0, static_cast<std::size_t>(style.maxCharacters)), color, style.font, typed);
  g.SetClip(clip);
  return true;
}

void render::DrawRecordFrame(Gfx& g, const Rect& area, const Controller& controller, Animator& animator, int thickness)
{
  const auto& sequencer = controller.HostView().Sequencer();
  if (!sequencer.RecordArmed() || !sequencer.Playing()) return;
  const float pulse = 0.5f + 0.5f * std::sin(static_cast<float>(animator.Now()) / 140);
  const int level = 3 + static_cast<int>(pulse * 6);
  g.Dither(Rect{area.x, area.y, area.width, thickness}, kRec, level);
  g.Dither(Rect{area.x, area.y + area.height - thickness, area.width, thickness}, kRec, level);
  g.Dither(Rect{area.x, area.y, thickness, area.height}, kRec, level);
  g.Dither(Rect{area.x + area.width - thickness, area.y, thickness, area.height}, kRec, level);
}

void render::DrawSessionSweep(Gfx& g, const Rect& bounds, const Controller& controller, Animator& animator)
{
  const auto& state = controller.State();
  const auto now = animator.Now();
  const float session = fx::Progress(now, state.sessionAt, kSessionSweepMs);
  if (state.sessionAt != 0 && session < 1)
  {
    const int y = bounds.y + static_cast<int>(fx::EaseInOutCubic(session) * (bounds.height + 12));
    fx::Fade(g, Rect{bounds.x, y, bounds.width, std::max(0, bounds.y + bounds.height - y)}, 12);
    fx::ScanLine(g, bounds.x, bounds.width, y, 10, kInk);
  }

}

void render::DrawPanic(Gfx& g, const Rect& bounds, const Controller& controller, Animator& animator)
{
  const auto& state = controller.State();
  const auto now = animator.Now();
  const float panic = fx::Decay(now, state.panicAt, kPanicMs);
  if (panic > 0)
  {
    g.Dither(bounds, kInk, static_cast<int>(panic * panic * 14));
    const Rect stamp{bounds.x + bounds.width / 2 - 62, bounds.y + bounds.height / 2 - 15, 124, 30};
    g.Fill(stamp, kBlack);g.Stroke(stamp, kRec);
    g.TextAligned(stamp, "ALL STOP", panic > 0.5f || (now / 60) % 2 ? kRec : kInk, Font::Large, Align::Center);
  }
}

void render::DrawSessionAndPanic(Gfx& g, const Rect& bounds, const Controller& controller, Animator& animator)
{
  DrawSessionSweep(g, bounds, controller, animator);
  DrawPanic(g, bounds, controller, animator);
}

void render::DrawBootHandoff(Gfx& g, const Rect& bounds, Animator& animator)
{
  const auto sinceBoot = animator.Now() - animator.BootAt();
  if (sinceBoot >= kBootMs - 300 && sinceBoot < kBootMs)
  {
    const float t = static_cast<float>(sinceBoot - (kBootMs - 300)) / 300;
    fx::Fade(g, bounds, 16 - static_cast<int>(t * 16));
  }
}

Rect render::GrowOverlay(Gfx& g, const Rect& content, const Rect& panel, const Rect& source, Color accent, float t)
{
  fx::Fade(g, content, static_cast<int>(t * kDitherScrim));
  const int originY = source.y + source.height / 2;
  const int edge = source.x + source.width;
  const Rect grown{edge + 2 + static_cast<int>((panel.x - edge - 2) * t),
      originY + static_cast<int>((panel.y - originY) * t),
      std::max(1, static_cast<int>(panel.width * t)), std::max(1, static_cast<int>(panel.height * t))};
  g.Fill(grown, kBlack);g.Stroke(grown, accent);
  if (t < 1) g.HLine(edge, originY, grown.x - edge, accent);
  g.SetClip(grown);
  return grown;
}

Rect render::DropInTile(const Rect& rect, float t, std::size_t order, int distance)
{
  const float tileT = fx::EaseOutBack(fx::Clamp01(t * 1.8f - static_cast<float>(order) * 0.09f));
  return Rect{rect.x, rect.y - static_cast<int>((1.0f - tileT) * distance), rect.width, rect.height};
}

render::PageTabsLayout render::DrawPageTabs(Gfx& g, const Controller& controller, Animator& animator, const PageTabsStyle& style)
{
  PageTabsLayout result;
  const UiState& state = controller.State();
  const std::size_t count = controller.PageCount(state.scene);
  if (count == 0) return result;
  const std::size_t current = std::min<std::size_t>(controller.CurrentPage(), count - 1);
  std::vector<std::string> tabs;
  std::vector<int> widths;
  int total = 0;
  for (std::size_t page = 0; page < count; ++page)
  {
    tabs.push_back(controller.PageTab(state.scene, page));
    widths.push_back(Gfx::Measure(tabs.back(), style.font) + style.gap);
    total += widths.back();
  }
  std::size_t first = 0;
  std::size_t last = count - 1;
  int left = style.left;
  int right = style.right;
  // A long pattern: which of its pages the GRID shows, yellow once flipped by hand.
  const bool stepPages = controller.State().scene == Scene::Sequence && current == 0;
  if (stepPages)
  {
    const auto seq = controller.HostView().Sequencer().BuildUiState(controller.State().track);
    if (seq.viewPageCount > 1)
    {
      const std::string pages = std::to_string(seq.viewPage + 1) + "/" + std::to_string(seq.viewPageCount);
      g.Text(style.right - Gfx::Measure(pages, style.font), style.y, pages, seq.viewFollow ? kDim : kFn, style.font);
      right -= Gfx::Measure(pages, style.font) + style.patternCounterGap;
    }
  }
  if (total > right - left)
  {
    char counter[24];
    std::snprintf(counter, sizeof(counter), "%02u/%02u", static_cast<unsigned>(current + 1), static_cast<unsigned>(count));
    g.Text(right - Gfx::Measure(counter, style.font), style.y, counter, kFaint, style.font);
    right -= Gfx::Measure(counter, style.font) + style.counterGap;
    // Grow the window from the current tab, alternating right and left, while it fits. Room for
    // the "<" is kept only when tabs are hidden on the left: the first tab stays flush left.
    auto grow = [&]() {
      first = last = current;
      int used = widths[current];
      for (bool grew = true; grew;)
      {
        grew = false;
        if (last + 1 < count && used + widths[last + 1] <= right - left) { used += widths[++last]; grew = true; }
        if (first > 0 && used + widths[first - 1] <= right - left) { used += widths[--first]; grew = true; }
      }
    };
    grow();
    if (first > 0)
    {
      left = style.left + style.arrowSpace;
      grow();
    }
    if (first > 0)
    {
      g.Text(style.left, style.y, "<", kCtrl, style.font);
      result.moreBefore = true;
      result.previous = Rect{style.left - style.highlightPadding, style.y + style.highlightOffsetY, style.arrowSpace, style.highlightHeight};
    }
    if (last + 1 < count)
    {
      g.Text(right + 2, style.y, ">", kCtrl, style.font);
      result.moreAfter = true;
      result.next = Rect{right + 2, style.y + style.highlightOffsetY, style.arrowSpace, style.highlightHeight};
    }
  }
  std::vector<int> positions(count, 0);
  int x = left;
  for (std::size_t page = first; page <= last; ++page)
  {
    positions[page] = x;
    x += widths[page];
  }
  // The violet highlight glides to the current tab.
  const std::uint32_t key = Animator::Key("tabs", static_cast<std::uint32_t>(state.scene) * 7 + static_cast<std::uint32_t>(state.track));
  const float boxX = animator.Smooth(key, static_cast<float>(positions[current] - style.highlightPadding), 55.0f);
  const float boxW = animator.Smooth(key + 1, static_cast<float>(Gfx::Measure(tabs[current], style.font) + style.highlightPadding * 2), 55.0f);
  g.Fill(Rect{static_cast<int>(std::lround(boxX)), style.y + style.highlightOffsetY, static_cast<int>(std::lround(boxW)), style.highlightHeight}, kCtrl);
  for (std::size_t page = first; page <= last; ++page)
  {
    const int tabX = positions[page];
    const int tabW = Gfx::Measure(tabs[page], style.font);
    const bool covered = tabX >= boxX - 1 && tabX + tabW <= boxX + boxW + 1;
    g.Text(tabX, style.y, tabs[page], covered ? kBlack : (page == current ? kInk : kDim), style.font);
    result.tabs.push_back(PageTabPosition{page, Rect{tabX - style.highlightPadding, style.y + style.highlightOffsetY,
        widths[page], style.highlightHeight}});
  }
  return result;
}

void RenderFrame(Gfx& g, const Controller& controller, Animator& animator)
{
  theme::Use(controller.HostView().ScreenTheme());
  const std::uint32_t now = animator.Now();
  g.Clear(kBlack);
  const std::uint32_t sinceBoot = now - animator.BootAt();
  if (sinceBoot < kBootMs - 300)
  {
    DrawBoot(g, animator);
    return;
  }

  // Typing: no edge strips, the whole keyboard is the keyboard (ESC cancels).
  const Modal* top = controller.TopModal();
  if (top == nullptr || top->kind != ModalKind::TextEntry)
  {
    DrawStrips(g, controller, animator);
    DrawGlyphColumn(g, controller, animator);
  }
  DrawHeader(g, controller, animator);
  DrawContent(g, controller, animator);
  DrawRecordVignette(g, controller, animator);
  DrawFooter(g, controller, animator);

  // Hold maps grow out of their strip while held and shrink back into it on release.
  const std::array<bool, 4> active{{controller.EscOverlay(), controller.TabOverlay(), controller.FnOverlay(), controller.CtrlOverlay()}};
  for (std::size_t index = 0; index < active.size(); ++index)
  {
    float t = 0.0f;
    if (active[index]) t = fx::EaseOutCubic(fx::Progress(now, animator.OverlayAt(index), kOverlayGrowMs));
    else if (animator.OverlayClosedAt(index) != 0 && now - animator.OverlayClosedAt(index) < kOverlayCloseMs)
      t = 1.0f - fx::EaseInOutCubic(fx::Progress(now, animator.OverlayClosedAt(index), kOverlayCloseMs));
    if (t <= 0.0f) continue;
    if (index == 0) DrawSceneOverlay(g, controller, animator, t);
    else if (index == 1) DrawTrackOverlay(g, controller, animator, t);
    else if (index == 2) DrawFnOverlay(g, controller, animator, t);
    else DrawPageOverlay(g, controller, animator, t);
  }

  DrawHud(g, controller, animator);

  render::DrawSessionSweep(g, Rect{kStripWidth + 1, 0, kScreenWidth - kStripWidth - 1, kScreenHeight}, controller, animator);
  render::DrawPanic(g, Rect{0, 0, kScreenWidth, kScreenHeight}, controller, animator);
  render::DrawBootHandoff(g, Rect{0, 0, kScreenWidth, kScreenHeight}, animator);

}

}  // namespace groove
