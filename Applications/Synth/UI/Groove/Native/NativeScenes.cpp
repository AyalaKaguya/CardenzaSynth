#include "Applications/Synth/UI/Groove/Native/NativeView.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <string>

#include "CardenzaBuildInfo.h"
#include "Applications/Synth/UI/Groove/Native/NativeLayout.h"
#include "Applications/Synth/UI/Groove/Render/Effects.h"
#include "Applications/Synth/UI/Groove/Render/GrooveRenderer.h"
#include "Applications/Synth/UI/Groove/Render/ModalComponents.h"
#include "Applications/Synth/UI/Groove/Render/SceneComponents.h"
#include "Applications/Synth/UI/Groove/Render/Theme.h"
#include "Applications/Synth/UI/Groove/Render/Widgets.h"
#include "Library/Synth/Audio/SynthFx.h"

namespace groove::native {
namespace {
using namespace theme;

float Fraction(const synth::SynthParameterInfo& info)
{
  return info.kind == synth::SynthParameterKind::Continuous && info.maxValue > info.minValue ?
      static_cast<float>(info.value - info.minValue) / static_cast<float>(info.maxValue - info.minValue) : -1.0f;
}

int Raw(const synth::ISynthEngine* engine, const char* id, int fallback = 0)
{
  synth::SynthParameterInfo info;
  return engine != nullptr && engine->GetParameterInfo(id, info) ? info.value : fallback;
}

std::string Pattern(std::size_t index)
{
  char text[12];
  std::snprintf(text, sizeof(text), "P%02u", static_cast<unsigned>(index + 1));
  return text;
}

Rect Inset(const Rect& rect, int pad = 8)
{
  return Rect{rect.x + pad, rect.y + pad, std::max(0, rect.width - pad * 2), std::max(0, rect.height - pad * 2)};
}

template <typename RegisterHit>
void PianoDiagram(Gfx& g, const Rect& area, const Controller& controller, Animator& animator, RegisterHit registerHit,
                  const std::array<bool, 128>* marked = nullptr, bool showScale = false)
{
  const int base = static_cast<int>(synth::kHomePerformanceBaseMidiNote) + controller.State().keyboard.octave * 12;
  const auto geometry = render::PaintPiano(g, area, controller, animator, base, showScale, marked);
  for (std::size_t index = 0; index < geometry.count; ++index)
    registerHit(geometry.keys[index].rect, geometry.keys[index].note);
}

widgets::ParamCellLayout NativeCellMetrics()
{
  widgets::ParamCellLayout layout;
  layout.paddingX = layout.rightPadding = 5;
  layout.labelY = 4;
  layout.labelHeight = 17;
  layout.valueY = 32;
  layout.barBottom = 5;
  layout.barHeight = 3;
  layout.labelFont = Font::Small;
  layout.valueFont = Font::Large;
  return layout;
}

render::SceneGrid NativeGrid()
{
  render::SceneGrid grid;
  grid.area = layout::kBody;
  for (std::size_t index = 0; index < kGridCount; ++index) grid.cells[index] = layout::Cell(index);
  grid.labelFont = Font::Small;
  grid.valueFont = Font::Large;
  grid.padding = 7;
  grid.detailY = 46;
  grid.tickY = 48;
  grid.tickHeight = 15;
  return grid;
}

void SystemInfo(Gfx& g, const Rect& rect, const Controller& controller, Animator& animator)
{
  render::SystemInfoLayout metrics;
  metrics.rowHeight = rect.height / 6;
  metrics.labelWidth = 106;
  metrics.padding = 6;
  metrics.font = Font::Small;
  metrics.extended = true;
  render::PaintSystemInfo(g, controller, animator, rect, metrics);
}

}  // namespace

void View::Graphic(Gfx& g, const Controller& controller, Animator& animator,
                   const InstrumentPage& page, const Rect& area)
{
  if (area.width <= 0 || area.height <= 0) return;
  const auto& state = controller.State();
  const auto& host = controller.HostView();
  const auto* engine = host.Engine(state.track);
  const Color color = kInk;
  const Rect graph = Inset(area);
  const Rect oldClip = g.Clip();
  const Rect moved{area.x + g.OriginX(), area.y + g.OriginY(), area.width, area.height};
  const int clipX = std::max(oldClip.x, moved.x);
  const int clipY = std::max(oldClip.y, moved.y);
  g.SetClip(Rect{clipX, clipY, std::max(0, std::min(oldClip.x + oldClip.width, moved.x + moved.width) - clipX),
                std::max(0, std::min(oldClip.y + oldClip.height, moved.y + moved.height) - clipY)});
  const auto now = animator.Now();
  const float activity = host.TrackActivity(state.track);
  const auto value = [&](const char* id, float fallback = 0.0f) {
    return render::GraphParameter(animator, state.track, engine, id, fallback);
  };
  const auto performanceHit = [this](const Rect& rect, int note) { AddPerformanceHit(rect, note); };
  switch (page.graphic)
  {
    case PageGraphic::Voice:
    {
      const std::string preset = engine == nullptr ? "NO ENGINE" : engine->CurrentPresetName();
      const auto changed = std::max(animator.PresetChangedAt(state.track), animator.TrackChangedAt());
      fx::DecodeText(g, graph.x, graph.y + 2, preset, kInk,
                     Gfx::Measure(preset, Font::Huge) <= graph.width ? Font::Huge : Font::Large,
                     fx::Progress(now, changed, 420), changed / 7 + now / 60, graph.width);
      g.TextFit(graph.x, graph.y + 34, graph.width, std::string(EngineShortName(host.EngineKind(state.track))) + "  /  T" +
                std::to_string(state.track + 1), TrackColor(state.track), Font::Small);
      const Rect lower{graph.x, graph.y + 72, graph.width, std::max(0, graph.height - 72)};
      if (host.EngineKind(state.track) == synth::SynthEngineKind::Drums)
      {
        for (std::size_t drum = 0; drum < 8; ++drum)
        {
          const Rect pad{lower.x + static_cast<int>(drum % 4) * (lower.width / 4), lower.y + static_cast<int>(drum / 4) * (lower.height / 2),
                         lower.width / 4 - 6, lower.height / 2 - 6};
          AddPerformanceHit(pad, kDrumNotes[drum]);
          render::PaintDrumPad(g, pad, controller, animator, engine, drum, Font::Large, 12, 0);
        }
      }
      else if (lower.height > 0) PianoDiagram(g, lower, controller, animator, performanceHit);
      break;
    }
    case PageGraphic::Algorithm:
      widgets::Dx7Algorithm(g, graph, Raw(engine, "global.algorithm", 1), 0, Raw(engine, "global.feedback"), activity, now, 3);
      break;
    case PageGraphic::OperatorLevels:
    {
      const int algorithm = Raw(engine, "global.algorithm", 1);
      g.Text(graph.x, graph.y, "ALGORITHM " + std::to_string(algorithm), kDim, Font::Small);
      const int width = graph.width / 6;
      const int height = graph.height - 42;
      for (int op = 1; op <= 6; ++op)
      {
        const bool carrier = widgets::Dx7IsCarrier(algorithm, op);
        const float level = value(("op" + std::to_string(op) + ".output").c_str());
        const Rect bar{graph.x + (op - 1) * width + 6, graph.y + 26, width - 12, height};
        g.Dither(bar, kFaint, 2);
        const int fill = static_cast<int>(level * height);
        const float flicker = carrier ? fx::OperatorLevelFlicker(now, activity, op) : 0.0f;
        g.Dither(Rect{bar.x, bar.y + height - fill, bar.width, fill}, carrier ? kInk : kDim,
                 carrier ? kDitherSolid - static_cast<int>(flicker * 4.0f) : 8);
        g.TextAligned(Rect{bar.x, bar.y + height + 6, bar.width, 18}, std::to_string(op) + (carrier ? "C" : "M"),
                      carrier ? kInk : kDim, Font::Small, Align::Center);
      }
      break;
    }
    case PageGraphic::OperatorEnvelope:
    {
      const std::string prefix = "op" + std::to_string(page.op) + ".";
      const auto opValue = [&](const char* id, float fallback = 0.0f) { return value((prefix + id).c_str(), fallback); };
      const float output = 0.25f + 0.75f * opValue("output", 1.0f);
      const std::array<float, 4> levels{{opValue("level1", 1), opValue("level2", 0.9f), opValue("sustain"), opValue("level4")}};
      const std::array<float, 4> rates{{opValue("attack"), opValue("decay"), opValue("rate3", 0.5f), opValue("release")}};
      std::array<float, 5> times{};
      float last = levels[3];
      for (std::size_t i = 0; i < 3; ++i) { times[i] = (0.06f + 1.0f - rates[i]) * (0.15f + std::fabs(levels[i] - last)); last = levels[i]; }
      times[3] = 0.35f;
      times[4] = (0.06f + 1.0f - rates[3]) * (0.15f + std::fabs(levels[2] - levels[3]));
      const bool carrier = widgets::Dx7IsCarrier(Raw(engine, "global.algorithm", 1), page.op);
      widgets::Envelope(g, Rect{graph.x, graph.y + 18, graph.width, graph.height - 18}, times,
                        {{levels[0] * output, levels[1] * output, levels[2] * output, levels[2] * output, levels[3] * output}},
                        carrier ? kInk : kDim);
      g.Text(graph.x, graph.y, "OP" + std::to_string(page.op) + (carrier ? " CARRIER" : " MODULATOR"), kDim, Font::Small);
      if (carrier && activity > 0.02f)
        g.Dither(Rect{graph.x + graph.width - 22, graph.y + 12, 22, 2}, kPlay, static_cast<int>(4.0f + activity * 12.0f));
      break;
    }
    case PageGraphic::Adsr:
    {
      const float sustain = value("env.sustain");
      widgets::Envelope(g, graph, {{0.05f + value("env.attack"), 0.05f + value("env.decay"), 0.02f, 0.7f,
                                 0.05f + value("env.release")}}, {{1.0f, sustain, sustain, sustain, 0.0f}}, color);
      if (activity > 0.02f)
      {
        const float position = std::fmod(static_cast<float>(now) / 900.0f, 1.0f);
        g.VLine(graph.x + static_cast<int>(position * static_cast<float>(graph.width)), graph.y, graph.height, kTab);
      }
      break;
    }
    case PageGraphic::Filter:
    {
      const bool sampler = host.EngineKind(state.track) == synth::SynthEngineKind::Sampler;
      const float resonance = value(sampler ? "filter.resonance" : "vcf.resonance");
      widgets::FilterCurve(g, graph, value(sampler ? "filter.cutoff" : "vcf.cutoff", 1.0f),
                           std::max(0.0f, resonance + fx::FilterShimmer(now, activity)), color);
      break;
    }
    case PageGraphic::Oscillator:
      widgets::OscillatorScope(g, graph, Raw(engine, "dco.saw", 1) != 0, Raw(engine, "dco.pulse", 1) != 0 ? value("dco.pwm") : -1,
                               value("dco.sub"), value("dco.noise"), value("lfo.rate"),
                               activity > 0.02f ? kTab : kInk, fx::OscillatorPhase(now, activity));
      break;
    case PageGraphic::Lfo:
    {
      const bool dx7 = host.EngineKind(state.track) == synth::SynthEngineKind::Dx7;
      const float rate = value(dx7 ? "global.lfo_speed" : "lfo.rate");
      const float depth = dx7 ? std::max(0.25f, std::max(value("global.lfo_pitch"), value("global.lfo_amp"))) : 1.0f;
      widgets::LfoShape(g, graph, dx7 ? Raw(engine, "global.lfo_wave") : 0, 1 + rate * 7,
                        dx7 ? 0 : 0.6f * value("lfo.delay"), depth, fx::LfoPhase(now, rate),
                        activity > 0.02f ? kTab : kInk);
      break;
    }
    case PageGraphic::Waveform:
    {
      const auto sampler = host.Sampler().BuildUiState(state.track);
      g.TextFit(graph.x, graph.y, graph.width, sampler.hasSample ? sampler.sampleName : "NO SAMPLE / FILE OR REC", kDim, Font::Small);
      const float cursor = sampler.frameCount > 0 ? static_cast<float>(sampler.previewCursorFrame) / static_cast<float>(sampler.frameCount) : 0;
      if (sampler.waveformPending)
        g.TextAligned(Rect{graph.x, graph.y + 48, graph.width, graph.height - 48}, "BUILDING WAVEFORM", kDim, Font::Large, Align::Center);
      else
        widgets::Waveform(g, Rect{graph.x, graph.y + 24, graph.width, graph.height - 24}, sampler.waveform.data(), sampler.hasSample ? sampler.waveform.size() : 0,
                          value("sample.start"), value("sample.end", 1), cursor, sampler.previewing);
      break;
    }
    case PageGraphic::Keys:
      PianoDiagram(g, graph, controller, animator, performanceHit, nullptr, true);
      break;
    case PageGraphic::Chord:
    {
      const auto& keys = state.keyboard;
      std::array<std::uint8_t, kMaxPadNotes> notes{};
      const int root = static_cast<int>(synth::kHomePerformanceBaseMidiNote) + keys.octave * 12 + keys.root;
      const auto count = PadNotes(keys, state.chords[state.track], synth::SynthEngineKind::Juno, root, notes);
      std::array<bool, 128> marked{};
      std::string text;
      for (std::size_t i = 0; i < count; ++i) { marked[notes[i]] = true; text += (i == 0 ? "" : " ") + NoteName(notes[i]); }
      g.TextFit(graph.x, graph.y, graph.width, text, color, Font::Small);
      PianoDiagram(g, Rect{graph.x, graph.y + 30, graph.width, graph.height - 30}, controller, animator, performanceHit, &marked, true);
      break;
    }
    case PageGraphic::Arp:
      render::PaintArpeggiator(g, controller, animator, graph, Font::Small, 28);
      break;
    case PageGraphic::DrumPads:
      for (std::size_t pad = 0; pad < 8; ++pad)
      {
        const Rect rect{graph.x + static_cast<int>(pad % 4) * graph.width / 4, graph.y + static_cast<int>(pad / 4) * graph.height / 2,
                        graph.width / 4 - 5, graph.height / 2 - 5};
        AddPerformanceHit(rect, kDrumNotes[pad]);
        render::PaintDrumPad(g, rect, controller, animator, engine, pad, Font::Small, 8, 0);
      }
      break;
    case PageGraphic::FxChain:
      render::PaintFxChain(g, controller, animator, graph);
      break;
    case PageGraphic::System:
      SystemInfo(g, area, controller, animator);
      break;
    case PageGraphic::None:
      break;
  }
  g.SetClip(oldClip);
}

void View::SlotPage(Gfx& g, const Controller& controller, Animator& animator, const InstrumentPage& page, std::uint8_t focus, bool current)
{
  const auto& state = controller.State();
  const Color color = state.scene == Scene::Setup ? kCtrl : TrackColor(state.track);
  const auto metrics = NativeCellMetrics();
  for (std::size_t index = 0; index < page.slots.size(); ++index)
  {
    const auto& slot = page.slots[index];
    if (slot.Empty()) continue;
    const Rect rect = layout::Cell(index);
    if (slot.IsAction()) Action(g, rect, slot.label, "ENTER", index == focus, color);
    else
    {
      synth::SynthParameterInfo info;
      std::string text;
      const bool available = controller.SlotValue(slot, info, text);
      float fraction = available ? Fraction(info) : -1.0f;
      if (fraction >= 0.0f)
        fraction = animator.Smooth(Animator::Key(slot.id, static_cast<std::uint32_t>(state.track) * 31u + 7u), fraction, 60.0f);
      const float flash = current ? fx::Decay(animator.Now(), animator.ValueChangedAt(index), 320) : 0.0f;
      widgets::ParamCell(g, rect, slot.label == nullptr ? "" : slot.label, available ? text : "--", fraction,
                        index == focus, !available, metrics, flash, animator.Now(), false);
    }
    if (current) render::PaintGridRipple(g, animator, index, rect, kInk);
    AddHit(rect, {Kind::FocusCell, static_cast<int>(index)});
  }
  if (current && focus < page.slots.size() && page.slots[focus].IsParameter())
    render::PaintFocus(g, animator, layout::Cell(focus), Animator::Key("native-slot-focus",
        static_cast<std::uint32_t>(state.track * 64 + controller.CurrentPage() + 1) ^ static_cast<std::uint32_t>(state.scene) << 16), kInk);
  if (page.graphic != PageGraphic::None && page.area.rows > 0 && page.area.columns > 0)
    Graphic(g, controller, animator, page, page.graphic == PageGraphic::System ? layout::Span(1, 0, 3, 4) :
            layout::Span(page.area.row, page.area.column, page.area.rows, page.area.columns));
}

void View::Sequence(Gfx& g, const Controller& controller, Animator& animator, std::uint8_t page)
{
  const auto& state = controller.State();
  const auto& sequencer = controller.HostView().Sequencer();
  const auto seq = sequencer.BuildUiState(state.track);
  const Color color = TrackColor(state.track);
  if (page == 1)
  {
    const auto grid = NativeGrid();
    render::PaintPatternGrid(g, controller, animator, seq, grid);
    for (std::size_t pattern = 0; pattern < 16 && pattern <= seq.patternCount; ++pattern)
      AddHit(grid.cells[pattern], {Kind::SelectPattern, static_cast<int>(pattern)});
    return;
  }
  if (page == 2)
  {
    static constexpr std::array<const char*, 4> ids{{"proj.seq", "proj.steps", "proj.swing", "proj.bpm"}};
    static constexpr std::array<const char*, 4> labels{{"SEQ", "STEPS", "SWING", "BPM"}};
    const std::array<std::string, 4> values{{seq.tracks[state.track].enabled ? "ON" : "OFF", std::to_string(seq.tracks[state.track].stepCount),
        std::to_string(seq.swing) + "%", std::to_string(seq.bpm)}};
    for (std::size_t i = 0; i < 4; ++i)
    {
      synth::SynthParameterInfo info;
      std::string text;
      const bool has = controller.SettingValue(ids[i], info, text);
      float fraction = has ? Fraction(info) : -1.0f;
      if (fraction >= 0.0f) fraction = animator.Smooth(Animator::Key(labels[i], 911u), fraction);
      Cell(g, layout::Cell(i), labels[i], values[i], fraction, state.seqTrackFocus == i, color);
      render::PaintGridRipple(g, animator, i, layout::Cell(i), kInk);
      AddHit(layout::Cell(i), {Kind::FocusCell, static_cast<int>(i)});
    }
    render::PaintFocus(g, animator, layout::Cell(state.seqTrackFocus), Animator::Key("native-seq-track-focus"), kInk);
    for (std::size_t track = 0; track < 4; ++track)
    {
      const Rect row{56, 161 + static_cast<int>(track) * 64, 368, 54};
      const auto& summary = seq.tracks[track];
      g.Text(row.x + 4, row.y + 4, "T" + std::to_string(track + 1), TrackColor(track), Font::Large);
      g.TextFit(row.x + 48, row.y + 6, 170, EngineShortName(controller.HostView().EngineKind(track)), kDim, Font::Small);
      g.TextAligned(Rect{row.x + 220, row.y + 5, 142, 18}, Pattern(summary.activePattern) + " / " + std::to_string(summary.stepCount), kInk, Font::Small, Align::Right);
      widgets::LevelBar(g, Rect{row.x + 4, row.y + 32, 356, 5}, summary.stepCount > 0 ? static_cast<float>(summary.currentStep) / static_cast<float>(summary.stepCount) : 0, TrackColor(track), kShade);
      if (!summary.enabled) g.Text(row.x + 282, row.y + 42, "OFF", kRec, Font::Small);
      if (animator.MeterPeak(track) > 0.02f)
        g.Dither(Rect{row.x - 4, row.y + 4, 2, 40}, kPlay, static_cast<int>(animator.MeterPeak(track) * 16.0f));
      g.DottedHLine(row.x, row.y + 52, row.width, kFaint, 3);
    }
    return;
  }

  render::StepMachineLayout scene;
  scene.matrix = Rect{56, 56, 368, 120};
  scene.grid = Rect{56, 228, 368, 196};
  scene.labelWidth = controller.DrumTrack() ? 32 : 28;
  scene.columnWidth = (scene.matrix.width - scene.labelWidth) / 16;
  scene.gapX = 8;
  scene.gapY = 4;
  scene.nameHeight = 0;
  scene.stepFont = Font::Large;
  scene.showNoteNames = true;
  scene.showMatrix = controller.View() == SeqView::Step;
  render::PaintStepMachine(g, controller, animator, seq, scene);
  if (controller.View() == SeqView::Roll)
    render::PaintPianoRoll(g, controller, animator, seq, render::PianoRollLayout{scene.matrix, 28, 9, false});
  g.TextFit(56, 187, 368, "PAGE " + std::to_string(seq.viewPage + 1) + "/" + std::to_string(seq.viewPageCount) + "  " +
            std::string(synth::SequencerEditLaneLabel(seq.editLane)) + " " + seq.valueLabel, kDim, Font::Small);
  g.TextFit(56, 205, 368, seq.noteNames.empty() ? "SELECT STEP / ADD-SUB TO EDIT" : seq.noteNames, color, Font::Small);
  for (std::size_t step = 0; step < kGridCount; ++step)
  {
    const auto absolute = seq.firstViewStep + step;
    const Rect rect{56 + static_cast<int>(step % 4) * 94, 228 + static_cast<int>(step / 4) * 50, 86, 46};
    if (absolute < seq.tracks[state.track].stepCount) AddHit(rect, {Kind::SelectStep, static_cast<int>(absolute)});
  }
  if (!seq.tracks[state.track].enabled)
  {
    const Rect band{56, 101, 368, 22};
    g.Fill(band, kBlack);
    g.TextAligned(band, "SEQUENCE OFF - SEQ > TRACK", (animator.Now() / 500) % 2 ? kDim : kFaint, Font::Small, Align::Center);
  }
}

void View::Tape(Gfx& g, const Controller& controller, Animator& animator, std::uint8_t page)
{
  const auto& state = controller.State();
  const auto& host = controller.HostView();
  const auto seq = host.Sequencer().BuildUiState(state.track);
  if (page == kTapeSession || page == kTapeSong)
  {
    const auto grid = NativeGrid();
    if (page == kTapeSession) render::PaintSessionGrid(g, controller, animator, grid);
    else render::PaintSongGrid(g, controller, animator, grid, true);
    for (std::size_t row = 0; row < 4; ++row)
    {
      const auto absolute = (page == kTapeSong ? state.songScroll : state.sessionScroll) + row;
      for (std::size_t track = 0; track < 4; ++track)
      {
        const bool exists = page == kTapeSong ? absolute <= seq.songLength && absolute < synth::kSongMaxRows :
            absolute <= seq.tracks[track].patternCount && absolute < synth::kSequencerMaxPatternCount;
        if (exists) AddHit(grid.cells[row * 4 + track], {page == kTapeSong ? Kind::SelectSongCell : Kind::SelectSessionCell,
            static_cast<int>(absolute), static_cast<int>(track)});
      }
    }
    return;
  }
  if (page == kTapeMix)
  {
    for (std::size_t track = 0; track < 4; ++track)
    {
      const int x = 56 + static_cast<int>(track) * 94;
      const Color color = TrackColor(track);
      const bool selected = state.mixFocus == track && controller.NativeFocusedCell() < 4;
      const Rect gain{x, 56, 86, 176};
      if (selected) g.Fill(Rect{x + 5, 62, 76, 17}, color);
      g.Text(x + 8, 66, "T" + std::to_string(track + 1), selected ? kBlack : color, Font::Small);
      g.TextAligned(Rect{x + 4, 89, 78, 28}, std::to_string(host.TrackLevel(track)), host.TrackAudible(track) ? kInk : kDim,
                    Font::Large, Align::Center);
      render::PaintMixFader(g, controller, animator, Rect{x + 31, 128, 24, 87}, track, selected);
      AddHit(gain, {Kind::FocusCell, static_cast<int>(track)});
      for (std::size_t row = 0; row < 3; ++row)
      {
        const Rect rect{x, 240 + static_cast<int>(row) * 48, 86, 40};
        const std::size_t index = (row + 1) * 4 + track;
        render::PaintMixBadge(g, controller, animator, rect, track, row, Font::Small);
        if (controller.NativeFocusedCell() == index)
          render::PaintFocus(g, animator, rect, Animator::Key("native-mix-focus"), TrackColor(track));
        render::PaintGridRipple(g, animator, index, rect, kInk);
        AddHit(rect, {Kind::FocusCell, static_cast<int>(index)});
      }
    }
    const Rect master{56, 384, 368, 40};
    if (state.mixFocus == 4) g.Fill(Rect{56, 392, 99, 17}, kInk);
    else g.DottedHLine(master.x, master.y + master.height - 1, master.width, kFaint, 2);
    g.Text(64, 396, "MASTER", state.mixFocus == 4 ? kBlack : kDim, Font::Small);
    g.Text(177, 394, std::to_string(host.MasterVolume()) + "%", kInk, Font::Large);
    widgets::LevelBar(g, Rect{260, 401, 152, 5}, host.OutputPeak(), kPlay, kShade);
    const float peak = animator.OutputPeakHold();
    g.VLine(260 + static_cast<int>(peak * 151.0f), 400, 7, peak > 0.95f ? kRec : kInk);
    AddHit(master, {Kind::FocusMaster});
    return;
  }
  if (page == kTapeSends)
  {
    const auto metrics = NativeCellMetrics();
    for (std::size_t index = 0; index < 16; ++index)
    {
      const auto rect = layout::Cell(index);
      render::PaintSendCell(g, controller, animator, rect, index, metrics, true);
      render::PaintGridRipple(g, animator, index, rect, kInk);
      AddHit(rect, {Kind::FocusCell, static_cast<int>(index)});
    }
    render::PaintFocus(g, animator, layout::Cell(state.sendsFocus), Animator::Key("native-sends-focus"), kInk);
    return;
  }
  const auto metrics = NativeCellMetrics();
  for (std::size_t index = 0; index < synth::fx::kMasterParamCount; ++index)
  {
    const auto rect = layout::Cell(index);
    render::PaintMasterFxCell(g, controller, animator, rect, index, metrics);
    render::PaintGridRipple(g, animator, index, rect, kInk);
    AddHit(rect, {Kind::FocusCell, static_cast<int>(index)});
  }
  render::PaintFocus(g, animator, layout::Cell(state.fxFocus), Animator::Key("native-fx-focus"), kInk);
  render::PaintMasterFxBuses(g, controller, animator, layout::Span(3, 0, 1, 2), layout::Span(3, 2, 1, 2));
}

void View::Setup(Gfx& g, const Controller& controller, Animator& animator, std::uint8_t page)
{
  const auto& state = controller.State();
  const auto& host = controller.HostView();
  if (page >= kSetupKeys)
  {
    const auto index = std::min<std::size_t>(page - kSetupKeys, kSettingsPageCount - 1);
    SlotPage(g, controller, animator, SettingsPage(index), state.settingsFocus[index],
             state.scene == Scene::Setup && page == controller.CurrentPage());
    return;
  }
  if (page == kSetupFiles)
  {
    render::ModalListStyle style;
    style.panel.content = layout::kBody;
    style.panel.spacious = true;
    style.rows = Rect{56, 92, 368, 322};
    style.scroll = Rect{419, 92, 4, 322};
    style.empty = Rect{56, 174, 368, 40};
    style.rowHeight = 36;
    style.highlightHeight = 34;
    style.textOffsetX = 18;
    style.textOffsetY = 10;
    style.markOffsetX = 4;
    style.markOffsetY = 12;
    style.textRightPadding = 20;
    style.rowFont = Font::Large;
    style.markFont = style.countFont = Font::Small;
    style.searchable = false;
    render::ListData data;
    data.title = "PROJECTS";
    data.count = state.projects.size();
    data.cursor = state.fileCursor;
    data.identity = 0x1000u + static_cast<std::uint32_t>(Scene::Setup);
    data.now = state.now;
    data.openedAt = animator.CurrentTransition().start;
    data.context = &state;
    data.row = [](const void* context, std::size_t index, std::string_view& label, bool& marked) {
      const auto& state = *static_cast<const UiState*>(context);
      if (index >= state.projects.size()) return false;
      label = state.projects[index];
      marked = label == state.projectName;
      return true;
    };
    const auto geometry = render::PaintListRows(g, animator, data, style);
    if (data.count == 0)
    {
      g.Fill(style.empty, kBlack);
      g.TextAligned(style.empty, host.QuerySystem().storageReady ? "NO PROJECTS" : "STORAGE UNAVAILABLE", kDim, Font::Large, Align::Center);
    }
    for (std::size_t index = 0; index < geometry.count; ++index)
      AddHit(geometry.hits[index].bounds, {Kind::SelectListRow, static_cast<int>(geometry.hits[index].index)});
    return;
  }
  static constexpr std::array<const char*, 3> actions{{"SAVE", "LOAD", "NEW"}};
  for (std::size_t i = 0; i < actions.size(); ++i)
  {
    Action(g, layout::Cell(i), actions[i], "ENTER", controller.NativeFocusedCell() == i, kEsc);
    render::PaintGridRipple(g, animator, i, layout::Cell(i), kInk);
    AddHit(layout::Cell(i), {Kind::FocusCell, static_cast<int>(i)});
  }
  const auto seq = host.Sequencer().BuildUiState(state.track);
  const std::array<std::string, 4> values{{std::to_string(seq.bpm), std::to_string(seq.swing) + "%", "TAP", seq.songMode ? "SONG" : "LOOP"}};
  const std::array<const char*, 4> labels{{"BPM", "SWING", "TEMPO", "PLAY"}};
  for (std::size_t i = 0; i < 4; ++i)
  {
    const auto index = i + 4;
    Cell(g, layout::Cell(index), labels[i], values[i], i == 0 ? static_cast<float>(seq.bpm - 30) / 270.0f : i == 1 ? static_cast<float>(seq.swing - 50) / 25.0f : -1,
         state.projectFocus == index, kEsc);
    render::PaintGridRipple(g, animator, index, layout::Cell(index), kInk);
    AddHit(layout::Cell(index), {Kind::FocusCell, static_cast<int>(index)});
  }
  const Rect details = layout::Span(2, 0, 2, 4);
  const std::string projectName = state.projectName.empty() ? "UNTITLED PROJECT" : state.projectName;
  fx::DecodeText(g, details.x + 4, details.y + 7, projectName, kInk,
                 Gfx::Measure(projectName, Font::Huge) <= details.width - 8 ? Font::Huge : Font::Large,
                 fx::Progress(animator.Now(), animator.ProjectChangedAt(), 500), animator.ProjectChangedAt() + animator.Now() / 60, details.width - 8);
  g.Text(details.x + 4, details.y + 44, state.dirty ? "UNSAVED CHANGES" : "PROJECT READY",
         state.dirty && (animator.Now() / 700) % 2 ? kFn : kDim, Font::Small);
  for (std::size_t track = 0; track < 4; ++track)
  {
    const int y = details.y + 76 + static_cast<int>(track) * 23;
    g.Text(details.x + 4, y, "T" + std::to_string(track + 1), animator.MeterPeak(track) > 0.3f ? kInk : TrackColor(track), Font::Small);
    g.TextFit(details.x + 44, y, 88, EngineShortName(host.EngineKind(track)), kDim, Font::Small);
    if (const auto* engine = host.Engine(track))
      g.TextFit(details.x + 136, y, details.width - 140, engine->CurrentPresetName(), kInk, Font::Small);
  }
}

void View::SceneContent(Gfx& g, const Controller& controller, Animator& animator, Scene scene, std::uint8_t page)
{
  const auto& state = controller.State();
  switch (scene)
  {
    case Scene::Instrument:
    {
      const auto pages = controller.InstrumentPages();
      if (pages.count == 0) break;
      const auto index = std::min<std::size_t>(page, pages.count - 1);
      SlotPage(g, controller, animator, pages.pages[index], state.instrumentFocus[std::min(index, state.instrumentFocus.size() - 1)],
               state.scene == scene && page == controller.CurrentPage());
      break;
    }
    case Scene::Sequence: Sequence(g, controller, animator, page); break;
    case Scene::Tape: Tape(g, controller, animator, page); break;
    case Scene::Setup: Setup(g, controller, animator, page); break;
  }
}

}  // namespace groove::native
