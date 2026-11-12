#include "Applications/Synth/UI/Groove/Render/GrooveRenderer.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "CardenzaBuildInfo.h"
#include "Applications/Synth/UI/Groove/Render/Effects.h"
#include "Applications/Synth/UI/Groove/Render/SceneComponents.h"
#include "Applications/Synth/UI/Groove/Render/Theme.h"
#include "Applications/Synth/UI/Groove/Render/Widgets.h"

namespace groove::render {

namespace {

using namespace groove::theme;
using widgets::Cell;
using widgets::CellSpan;
using widgets::CellStyle;

constexpr std::uint32_t kRippleMs = 300;
// The playhead flashes the step it enters, then the step stays outlined while it plays.
constexpr std::uint32_t kStepFlashMs = 160;
constexpr std::uint32_t kAfterglowMs = 380;

float ParamFraction(const synth::SynthParameterInfo& info)
{
  if (info.kind != synth::SynthParameterKind::Continuous || info.maxValue <= info.minValue) return -1.0f;
  return static_cast<float>(info.value - info.minValue) / static_cast<float>(info.maxValue - info.minValue);
}

float EngineValue(const synth::ISynthEngine* engine, const char* id, float fallback = 0.0f)
{
  synth::SynthParameterInfo info;
  if (engine == nullptr || !engine->GetParameterInfo(id, info) || info.maxValue <= info.minValue) return fallback;
  return static_cast<float>(info.value - info.minValue) / static_cast<float>(info.maxValue - info.minValue);
}

int EngineRaw(const synth::ISynthEngine* engine, const char* id, int fallback = 0)
{
  synth::SynthParameterInfo info;
  return engine != nullptr && engine->GetParameterInfo(id, info) ? info.value : fallback;
}

// Engine parameter that glides to its new value, so graphs morph instead of jumping.
float Glide(Animator& animator, std::size_t track, const synth::ISynthEngine* engine, const char* id, float fallback = 0.0f)
{
  return GraphParameter(animator, track, engine, id, fallback);
}

bool InArea(const GraphicArea& area, std::size_t index)
{
  const std::size_t row = index / kGridColumns;
  const std::size_t column = index % kGridColumns;
  return area.rows > 0 && row >= area.row && row < area.row + area.rows && column >= area.column && column < area.column + area.columns;
}

std::string Pad2(std::size_t value)
{
  char text[12];
  std::snprintf(text, sizeof(text), "%02u", static_cast<unsigned>(value));
  return text;
}

void GridRipple(Gfx& g, Animator& animator, std::size_t index, Color color)
{
  const std::uint32_t pressed = animator.GridPressedAt(index);
  if (pressed == 0) return;
  fx::Ripple(g, Cell(index / kGridColumns, index % kGridColumns), fx::Progress(animator.Now(), pressed, kRippleMs), color);
}

// How hard, in the four steps of the pad rows: solid, checker, a quarter, an eighth.
constexpr std::array<int, 4> kDrumDensity{{16, 8, 4, 2}};
int DrumDensity(std::uint8_t velocity) { return kDrumDensity[DrumRowForVelocity(velocity)]; }

// The kit's eight drums in one row, like the eight pad columns on the keyboard (a column is a drum).
// With fill (VOICE) they take the whole area, as the keyboard of a melodic track does, less the room
// a hit kicks them up; otherwise (the KIT page's graphic) a strip of at most 24 px in its middle.
void DrawPads(Gfx& g, const Controller& controller, Animator& animator, const Rect& area, const synth::ISynthEngine* engine,
              bool fill = false)
{
  constexpr int kGap = 2;
  constexpr int kKickRoom = 3;  // how far a hit kicks a pad up, at most
  const int padWidth = (area.width - kGap * 7) / 8;
  const int padHeight = fill ? area.height - kKickRoom : std::min(area.height, 24);
  const int left = area.x + (area.width - (padWidth * 8 + kGap * 7)) / 2;
  const int top = fill ? area.y + kKickRoom : area.y + (area.height - padHeight) / 2;
  for (std::size_t pad = 0; pad < 8; ++pad)
  {
    const Rect rect{left + static_cast<int>(pad) * (padWidth + kGap), top, padWidth, padHeight};
    PaintDrumPad(g, rect, controller, animator, engine, pad);
  }
}

void DrawMiniPiano(Gfx& g, const Rect& rect, const Controller& controller, Animator& animator, int baseNote, bool showScale = false,
                   const std::array<bool, 128>* marked = nullptr)
{
  (void)PaintPiano(g, rect, controller, animator, baseNote, showScale, marked);
}

void DrawVoice(Gfx& g, const Controller& controller, Animator& animator, const Rect& area)
{
  const UiState& state = controller.State();
  const Host& host = controller.HostView();
  const std::uint32_t now = animator.Now();
  const auto* engine = host.Engine(state.track);
  const std::string name = engine != nullptr ? engine->CurrentPresetName() : std::string("NO ENGINE");
  const Font font = Gfx::Measure(name, Font::Huge) <= area.width ? Font::Huge : Font::Large;
  const bool huge = font == Font::Huge;
  // The preset name decodes whenever it changes (SUB / ADD, browser, track switch). A name in the
  // huge font sits higher and closer to the info line, so the keyboard or the drum pads below keep
  // one height whatever the name.
  const std::uint32_t changed = std::max(animator.PresetChangedAt(state.track), animator.TrackChangedAt());
  fx::DecodeText(g, area.x, area.y + (huge ? -1 : 2), name, kInk, font, fx::Progress(now, changed, 420), changed / 7 + now / 60, area.width);
  const int infoY = area.y + (huge ? 23 : 21);
  int x = g.Text(area.x, infoY, EngineShortName(host.EngineKind(state.track)), TrackColor(state.track));
  if (engine != nullptr && engine->CurrentPresetId().rfind("user.", 0) == 0) x = g.Text(x + 6, infoY, "USER", kFn);
  if (host.EngineKind(state.track) != synth::SynthEngineKind::Drums)
  {
    const KeyboardSettings& keys = state.keyboard;
    std::string detail = std::string(kLayoutNames[static_cast<std::size_t>(keys.layout)]);
    if (keys.layout != KeyboardLayout::Piano) detail += std::string(" ") + kNoteNames[static_cast<std::size_t>(keys.root)];
    if (keys.layout == KeyboardLayout::Scale) detail += std::string(" ") + kScales[keys.scale].label;
    detail += "  OCT " + std::string(keys.octave > 0 ? "+" : "") + std::to_string(keys.octave);
    g.Text(x + 8, infoY, detail, kDim);
  }
  const int pianoTop = area.y + 33;
  const Rect lower{area.x, pianoTop, area.width, area.y + area.height - pianoTop};
  if (host.EngineKind(state.track) == synth::SynthEngineKind::Drums)
  {
    DrawPads(g, controller, animator, lower, engine, true);
    return;
  }
  const int base = synth::kHomePerformanceBaseMidiNote + state.keyboard.octave * 12;
  DrawMiniPiano(g, lower, controller, animator, base);
}

// The track's insert chain left to right (drive, crush, filter, chorus) and its two sends dropping
// to the master delay and reverb. Active stages fill with their amount; bypassed ones are dotted.
// While the track sounds, signal dots travel the line and into every send that is open.
void DrawFxChain(Gfx& g, const Controller& controller, Animator& animator, const Rect& area)
{
  PaintFxChain(g, controller, animator, area);
}

void DrawSystemInfo(Gfx& g, const Controller& controller, Animator& animator, const Rect& panel);

void DrawGraphic(Gfx& g, const Controller& controller, Animator& animator, const InstrumentPage& page)
{
  const Host& host = controller.HostView();
  const UiState& state = controller.State();
  const std::size_t track = state.track;
  const std::uint32_t now = animator.Now();
  const auto* engine = host.Engine(track);
  const Rect area = CellSpan(page.area.row, page.area.column, page.area.rows, page.area.columns);
  const float activity = host.TrackActivity(track);
  switch (page.graphic)
  {
    case PageGraphic::Voice:
      DrawVoice(g, controller, animator, area);
      break;
    case PageGraphic::Algorithm:
      widgets::Dx7Algorithm(g, Rect{area.x + 4, area.y, area.width - 8, area.height}, EngineRaw(engine, "global.algorithm", 1), 0,
                            EngineRaw(engine, "global.feedback"), activity, now);
      break;
    case PageGraphic::OperatorEnvelope:
    {
      // DX7 envelope: R1..R3 climb to L1..L3, L3 holds while the key is down, R4 returns to L4.
      // Higher rates are shorter segments; the graph is scaled by the operator output.
      const std::string prefix = "op" + std::to_string(page.op) + ".";
      auto value = [&](const char* id, float fallback = 0.0f) { return Glide(animator, track, engine, (prefix + id).c_str(), fallback); };
      const float output = 0.25f + 0.75f * value("output", 1.0f);
      const std::array<float, 4> levels{{value("level1", 1.0f), value("level2", 0.9f), value("sustain"), value("level4")}};
      const std::array<float, 4> rates{{value("attack"), value("decay"), value("rate3", 0.5f), value("release")}};
      float previous = levels[3];
      std::array<float, 5> times{};
      for (std::size_t segment = 0; segment < 3; ++segment)
      {
        times[segment] = (0.06f + (1.0f - rates[segment])) * (0.15f + std::fabs(levels[segment] - previous));
        previous = levels[segment];
      }
      times[3] = 0.35f;
      times[4] = (0.06f + (1.0f - rates[3])) * (0.15f + std::fabs(levels[2] - levels[3]));
      const bool carrier = widgets::Dx7IsCarrier(EngineRaw(engine, "global.algorithm", 1), page.op);
      const Rect graph{area.x + 2, area.y + 2, area.width - 30, area.height - 5};
      widgets::Envelope(g, graph, times, {{levels[0] * output, levels[1] * output, levels[2] * output, levels[2] * output, levels[3] * output}},
                        carrier ? kInk : kDim);
      // Operator tag: number, and whether it is heard (carrier) or modulates another operator.
      const Rect tag{area.x + area.width - 26, area.y + 2, 26, area.height - 4};
      g.Text(tag.x + 2, tag.y + 1, "OP" + std::to_string(page.op), kInk);
      g.Text(tag.x + 2, tag.y + 11, carrier ? "CAR" : "MOD", carrier ? kPlay : kDim);
      if (carrier && activity > 0.02f) g.Dither(Rect{tag.x, tag.y + 19, 22, 2}, kPlay, static_cast<int>(4.0f + activity * 12.0f));
      break;
    }
    case PageGraphic::OperatorLevels:
    {
      // Six operator outputs: carriers (heard) solid, modulators (timbre) dithered.
      const int algorithm = EngineRaw(engine, "global.algorithm", 1);
      g.Text(area.x + 2, area.y, "ALG " + std::to_string(algorithm), kDim);
      g.Text(area.x + area.width - Gfx::Measure("SOLID=HEARD") - 2, area.y, "SOLID=HEARD", kFaint);
      const int barWidth = (area.width - 4 - 5 * 6) / 6;
      const Rect bars{area.x + 2, area.y + 10, area.width - 4, area.height - 20};
      for (int op = 1; op <= 6; ++op)
      {
        const bool carrier = widgets::Dx7IsCarrier(algorithm, op);
        const float level = Glide(animator, track, engine, ("op" + std::to_string(op) + ".output").c_str());
        const int x = bars.x + (op - 1) * (barWidth + 6);
        const int height = std::max(1, static_cast<int>(level * static_cast<float>(bars.height)));
        g.Dither(Rect{x, bars.y, barWidth, bars.height}, kFaint, 2);
        const float flicker = carrier ? fx::OperatorLevelFlicker(now, activity, op) : 0.0f;
        g.Dither(Rect{x, bars.y + bars.height - height, barWidth, height}, carrier ? kInk : kDim,
                 carrier ? kDitherSolid - static_cast<int>(flicker * 4.0f) : kDitherLevelOn - 4);
        g.HLine(x, bars.y + bars.height - height, barWidth, carrier ? kInk : kDim);
        const std::string label = std::to_string(op) + (carrier ? "C" : "M");
        g.Text(x + (barWidth - Gfx::Measure(label)) / 2, bars.y + bars.height + 2, label, carrier ? kInk : kDim);
      }
      break;
    }
    case PageGraphic::Adsr:
    {
      const float sustain = Glide(animator, track, engine, "env.sustain");
      widgets::Envelope(g, Rect{area.x + 2, area.y + 4, area.width - 4, area.height - 6},
                        {{0.05f + Glide(animator, track, engine, "env.attack"), 0.05f + Glide(animator, track, engine, "env.decay"), 0.02f, 0.7f,
                          0.05f + Glide(animator, track, engine, "env.release")}},
                        {{1.0f, sustain, sustain, sustain, 0.0f}}, kInk);
      // While notes sound, a play head runs along the envelope.
      if (activity > 0.02f)
      {
        const float position = std::fmod(static_cast<float>(now) / 900.0f, 1.0f);
        g.VLine(area.x + 2 + static_cast<int>(position * static_cast<float>(area.width - 4)), area.y + 4, area.height - 6, kTab);
      }
      break;
    }
    case PageGraphic::Filter:
    {
      const bool sampler = host.EngineKind(track) == synth::SynthEngineKind::Sampler;
      const float cutoff = Glide(animator, track, engine, sampler ? "filter.cutoff" : "vcf.cutoff", 1.0f);
      const float resonance = Glide(animator, track, engine, sampler ? "filter.resonance" : "vcf.resonance");
      // Resonance shimmers while the track sounds.
      const float shimmer = fx::FilterShimmer(now, activity);
      widgets::FilterCurve(g, Rect{area.x + 2, area.y + 4, area.width - 4, area.height - 6}, cutoff, std::max(0.0f, resonance + shimmer), kInk);
      break;
    }
    case PageGraphic::Oscillator:
    {
      // A running oscilloscope: the waveform scrolls, faster while the track sounds.
      const float phase = fx::OscillatorPhase(now, activity);
      const bool pulse = EngineRaw(engine, "dco.pulse", 1) != 0;
      widgets::OscillatorScope(g, Rect{area.x + 2, area.y + 2, area.width - 4, area.height - 4}, EngineRaw(engine, "dco.saw", 1) != 0,
                               pulse ? Glide(animator, track, engine, "dco.pwm") : -1.0f, Glide(animator, track, engine, "dco.sub"),
                               Glide(animator, track, engine, "dco.noise"), Glide(animator, track, engine, "lfo.rate"),
                               activity > 0.02f ? kTab : kInk, phase);
      break;
    }
    case PageGraphic::Lfo:
    {
      // The LFO itself: shape, speed (cycles across the graph) and the delayed fade-in; it scrolls
      // at a speed that follows the rate.
      const bool dx7 = host.EngineKind(track) == synth::SynthEngineKind::Dx7;
      const float rate = Glide(animator, track, engine, dx7 ? "global.lfo_speed" : "lfo.rate");
      const float delay = dx7 ? 0.0f : 0.6f * Glide(animator, track, engine, "lfo.delay");
      const float depth = dx7 ? std::max(0.25f, std::max(Glide(animator, track, engine, "global.lfo_pitch"), Glide(animator, track, engine, "global.lfo_amp")))
                              : 1.0f;
      const int wave = dx7 ? EngineRaw(engine, "global.lfo_wave") : 0;
      const float cycles = 1.0f + rate * 7.0f;
      const float phase = fx::LfoPhase(now, rate);
      const Rect graph{area.x + 2, area.y + 9, area.width - 4, area.height - 11};
      g.Text(area.x + 2, area.y, dx7 ? "LFO > PITCH / AMP" : "LFO > DCO / VCF", kDim);
      widgets::LfoShape(g, graph, wave, cycles, delay, depth, phase, activity > 0.02f ? kTab : kInk);
      break;
    }
    case PageGraphic::FxChain:
      DrawFxChain(g, controller, animator, area);
      break;
    case PageGraphic::Keys:
    {
      // The pads' two octaves: scale tones dotted, the root barred, held chords lit.
      const int base = static_cast<int>(synth::kHomePerformanceBaseMidiNote) + std::clamp(state.keyboard.octave, -3, 3) * 12;
      DrawMiniPiano(g, area, controller, animator, base - ((base % 12) + 12) % 12, true);
      break;
    }
    case PageGraphic::Chord:
    {
      // What one key of this track plays: the chord on the root, then the chords of every scale degree.
      const KeyboardSettings& keys = state.keyboard;
      const ChordSettings& shape = state.chords[track];
      const int base = static_cast<int>(synth::kHomePerformanceBaseMidiNote) + std::clamp(keys.octave, -3, 3) * 12;
      const int root = base - ((base % 12) + 12) % 12 + keys.root;
      std::array<std::uint8_t, kMaxPadNotes> notes{};
      const std::size_t count = PadNotes(keys, shape, synth::SynthEngineKind::Juno, root, notes);
      std::array<bool, 128> marked{};
      std::string line = NoteName(root) + " PLAYS ";
      for (std::size_t index = 0; index < count; ++index)
      {
        marked[notes[index]] = true;
        line += (index == 0 ? "" : " ") + NoteName(notes[index]);
      }
      g.TextFit(area.x + 2, area.y + 2, area.width - 4, line, kInk);
      // The scale's own chords (stacked thirds), I to VII.
      KeyboardSettings triads = keys;
      triads.lock = ScaleLock::Off;
      const ChordSettings triadShape{ChordType::Triad, 0};
      std::string degrees;
      for (int semitone = 0, degree = 0; semitone < 12 && degree < 7; ++semitone)
      {
        if (!InScale(keys, root + semitone)) continue;
        ++degree;
        std::array<std::uint8_t, kMaxPadNotes> triad{};
        if (PadNotes(triads, triadShape, synth::SynthEngineKind::Juno, root + semitone, triad) < 3) continue;
        const int third = triad[1] - triad[0];
        const int fifth = triad[2] - triad[0];
        // Uppercase font: minor is "-", as in lead sheets.
        const char* quality = third == 3 && fifth == 6 ? "DIM" : third == 3 ? "-" : fifth == 8 ? "+" : "";
        degrees += std::string(degrees.empty() ? "" : " ") + kNoteNames[static_cast<std::size_t>((root + semitone) % 12)] + quality;
      }
      g.TextFit(area.x + 2, area.y + 12, area.width - 4, "IN KEY  " + degrees, kDim);
      // The pads' two octaves from the root's C: every chord of the octave fits on them.
      DrawMiniPiano(g, Rect{area.x, area.y + 24, area.width, area.height - 24}, controller, animator, root - keys.root, true, &marked);
      break;
    }
    case PageGraphic::Arp:
    {
      PaintArpeggiator(g, controller, animator, area);
      break;
    }
    case PageGraphic::System:
      DrawSystemInfo(g, controller, animator, area);
      break;
    case PageGraphic::Waveform:
    {
      const auto sampler = host.Sampler().BuildUiState(track);
      const Rect graph{area.x + 2, area.y + 10, area.width - 4, area.height - 10};
      std::string title = sampler.hasSample ? sampler.sampleName : std::string("NO SAMPLE - FILE OR REC");
      if (sampler.hasSample && sampler.sampleRate > 0)
      {
        char seconds[16];
        std::snprintf(seconds, sizeof(seconds), "  %.2fS", static_cast<double>(sampler.frameCount) / static_cast<double>(sampler.sampleRate));
        title += seconds;
      }
      g.TextFit(area.x + 2, area.y, area.width - 4, title, sampler.hasSample ? kDim : ((now / 600) % 2 ? kFaint : kDim));
      const float cursor = sampler.frameCount > 0 ? static_cast<float>(sampler.previewCursorFrame) / static_cast<float>(sampler.frameCount) : 0.0f;
      widgets::Waveform(g, graph, sampler.waveform.data(), sampler.hasSample ? sampler.waveform.size() : 0,
                        Glide(animator, track, engine, "sample.start"), Glide(animator, track, engine, "sample.end", 1.0f), cursor, sampler.previewing);
      break;
    }
    case PageGraphic::DrumPads:
      DrawPads(g, controller, animator, area, engine);
      break;
    case PageGraphic::None:
    default:
      break;
  }
}

// A page of slots (INSTR pages and SETUP): parameter cells with value bars, action cells, the
// gliding focus frame and the page graphic. `current` animates edits and presses.
void DrawSlotPage(Gfx& g, const Controller& controller, Animator& animator, const InstrumentPage& page, std::uint8_t focus, bool current,
                  std::uint32_t focusKey)
{
  const UiState& state = controller.State();
  const std::uint32_t now = animator.Now();
  for (std::size_t slotIndex = 0; slotIndex < kGridCount; ++slotIndex)
  {
    const Slot& slot = page.slots[slotIndex];
    const Rect cell = Cell(slotIndex / kGridColumns, slotIndex % kGridColumns);
    if (slot.IsAction())
    {
      widgets::ActionCell(g, cell, slot.label, current && state.gridDown[slotIndex], kDim);
      if (current) GridRipple(g, animator, slotIndex, kInk);
      continue;
    }
    if (!slot.IsParameter()) continue;
    synth::SynthParameterInfo info;
    std::string text;
    if (!controller.SlotValue(slot, info, text))
    {
      widgets::ParamCell(g, cell, slot.label, "--", -1.0f, CellStyle::Disabled);
      continue;
    }
    float fraction = ParamFraction(info);
    if (fraction >= 0.0f)
      fraction = animator.Smooth(Animator::Key(slot.id, static_cast<std::uint32_t>(state.track) * 31u + 7u), fraction, 60.0f);
    const float flash = current ? fx::Decay(now, animator.ValueChangedAt(slotIndex), 320) : 0.0f;
    widgets::ParamCell(g, cell, slot.label, text, fraction, slotIndex == focus ? CellStyle::Focused : CellStyle::Normal, flash, now, false);
    if (current) GridRipple(g, animator, slotIndex, kInk);
  }
  // The focus frame glides from cell to cell instead of jumping.
  if (page.slots[focus].IsParameter())
  {
    const Rect target = Cell(focus / kGridColumns, focus % kGridColumns);
    const int x = static_cast<int>(std::lround(animator.Smooth(focusKey, static_cast<float>(target.x), 50.0f)));
    const int y = static_cast<int>(std::lround(animator.Smooth(focusKey + 1, static_cast<float>(target.y), 50.0f)));
    fx::MarchingAnts(g, Rect{x - 1, y - 1, target.width + 2, target.height + 2}, kInk, now);
  }
  if (page.graphic != PageGraphic::None) DrawGraphic(g, controller, animator, page);
  for (std::size_t slotIndex = 0; slotIndex < kGridCount; ++slotIndex)
  {
    if (!page.slots[slotIndex].Empty() || InArea(page.area, slotIndex)) continue;
    const Rect cell = Cell(slotIndex / kGridColumns, slotIndex % kGridColumns);
    g.Pixel(cell.x + cell.width / 2, cell.y + cell.height / 2, kShade);
  }
}

void InstrumentScene(Gfx& g, const Controller& controller, Animator& animator, std::uint8_t pageIndex)
{
  const InstrumentPageList list = controller.InstrumentPages();
  const std::size_t index = std::min<std::size_t>(pageIndex, list.count - 1);
  const UiState& state = controller.State();
  const std::uint8_t focus = state.instrumentFocus[std::min<std::size_t>(index, state.instrumentFocus.size() - 1)];
  DrawSlotPage(g, controller, animator, list.pages[index], focus, index == controller.CurrentPage(),
               Animator::Key("instr-focus", static_cast<std::uint32_t>(state.track) * 16u + static_cast<std::uint32_t>(index)));
}

// SEQ ROLL: the pattern as a piano roll. Steps run left to right (the GRID's 16 keys, row by row),
// pitch bottom to top; each note is a bar as long as it plays, its fill as strong as its velocity.
// The strip at the bottom is the GRID: lit where a step holds notes, white where it is selected.
void DrawPianoRoll(Gfx& g, const Controller& controller, Animator& animator, const synth::SequencerUiState& seq)
{
  PaintPianoRoll(g, controller, animator, seq, PianoRollLayout{Rect{kContentX, kContentY, kContentWidth, kContentHeight}});
}

// The STEP view of the note page, the drum machine's look for every track. Left, a row per drum (a drum
// track's kit) or per pitch (a melodic track: every key the pads play at this octave, used or not, as
// in the piano roll, nothing folded away) over the steps of the page, the whole beat at a glance;
// right, the GRID as it lies under the fingers, for the drum or note the pads chose. A row whose pad
// is down is lit across (a drum's as dense as the pad hits), the row a tapped step writes into; notes
// keep the track's color everywhere.
void DrawStepMachine(Gfx& g, const Controller& controller, Animator& animator, const synth::SequencerUiState& seq)
{
  const int label = controller.DrumTrack() ? 14 : 20;
  const int left = kContentX + label + 16 * 5 + 6;
  StepMachineLayout layout;
  layout.matrix = Rect{kContentX, kContentY, label + 16 * 5, kContentHeight};
  layout.grid = Rect{left, kContentY, kContentRight - left, kContentHeight};
  layout.labelWidth = label;
  PaintStepMachine(g, controller, animator, seq, layout);
}

void SequenceScene(Gfx& g, const Controller& controller, Animator& animator, std::uint8_t page)
{
  const UiState& state = controller.State();
  const std::uint32_t now = animator.Now();
  const auto& sequencer = controller.HostView().Sequencer();
  const auto seq = sequencer.BuildUiState(state.track);
  const auto& track = seq.tracks[state.track];
  switch (page)
  {
    case 0:
    {
      // ROLL or STEP (the machine), drum and melodic tracks alike.
      if (controller.View() == SeqView::Roll) DrawPianoRoll(g, controller, animator, seq);
      else DrawStepMachine(g, controller, animator, seq);
      if (!track.enabled)
      {
        const Rect band{kContentX, kContentY + kContentHeight / 2 - 6, kContentWidth, 12};
        g.Fill(band, kBlack);
        g.TextAligned(band, "SEQUENCE OFF - SEQ > TRACK", (now / 500) % 2 ? kDim : kFaint, Font::Micro, Align::Center);
      }
      break;
    }
    case 1:
    {
      SceneGrid grid;
      grid.area = Rect{kContentX, kContentY, kContentWidth, kContentHeight};
      for (std::size_t index = 0; index < kGridCount; ++index) grid.cells[index] = Cell(index / kGridColumns, index % kGridColumns);
      PaintPatternGrid(g, controller, animator, seq, grid);
      break;
    }
    case 2:
    default:
    {
      char steps[8], swing[8], bpm[8];
      std::snprintf(steps, sizeof(steps), "%u", static_cast<unsigned>(track.stepCount));
      std::snprintf(swing, sizeof(swing), "%u%%", static_cast<unsigned>(seq.swing));
      std::snprintf(bpm, sizeof(bpm), "%u", static_cast<unsigned>(seq.bpm));
      const std::array<const char*, 4> labels{{"SEQ", "STEPS", "SWING", "BPM"}};
      const std::array<std::string, 4> values{{track.enabled ? "ON" : "OFF", steps, swing, bpm}};
      const std::array<float, 4> targets{{-1.0f, static_cast<float>(track.stepCount - 1) / static_cast<float>(synth::kSequencerMaxSteps - 1),
                                          static_cast<float>(seq.swing - 50) / 25.0f, static_cast<float>(seq.bpm - 30) / 270.0f}};
      for (std::size_t index = 0; index < 4; ++index)
      {
        const float fraction = targets[index] < 0.0f ? -1.0f : animator.Smooth(Animator::Key(labels[index], 911u), targets[index]);
        widgets::ParamCell(g, Cell(0, index), labels[index], values[index], fraction,
                           index == state.seqTrackFocus ? CellStyle::Focused : CellStyle::Normal, 0.0f, now);
        GridRipple(g, animator, index, kInk);
      }
      const Rect table = CellSpan(1, 0, 3, 4);
      const Host& host = controller.HostView();
      for (std::size_t row = 0; row < kTrackCount; ++row)
      {
        const int y = table.y + 4 + static_cast<int>(row) * 17;
        const auto& summary = seq.tracks[row];
        const bool currentTrack = row == state.track;
        const Color ink = currentTrack ? kInk : kDim;
        if (currentTrack) g.Fill(Rect{table.x - 2, y - 1, 15, 11}, TrackColor(row));
        g.Text(table.x, y, "T" + std::to_string(row + 1), currentTrack ? kBlack : TrackColor(row), Font::Small);
        g.Text(table.x + 22, y + 1, EngineShortName(host.EngineKind(row)), ink);
        char pattern[16];
        std::snprintf(pattern, sizeof(pattern), "P%02u/%02u", static_cast<unsigned>(summary.activePattern + 1), static_cast<unsigned>(summary.patternCount));
        g.Text(table.x + 74, y + 1, pattern, ink);
        // Step ruler: one tick per step, the playhead in green.
        for (std::size_t step = 0; step < kGridCount; ++step)
        {
          const int x = table.x + 120 + static_cast<int>(step) * 5;
          // A long pattern is cut in as many slices as there are ticks.
          const bool inRange = step < summary.stepCount;
          const bool head = seq.playing && inRange && step == (summary.stepCount <= kGridCount ? summary.currentStep : summary.currentStep * kGridCount / summary.stepCount);
          const Color color = inRange ? (currentTrack ? kDim : kFaint) : kShade;
          g.Fill(Rect{x, y + 2, 3, 5}, head ? kPlay : color);
        }
        if (!summary.enabled) g.Text(table.x + table.width - Gfx::Measure("OFF"), y + 1, "OFF", kRec);
        const float activity = animator.MeterPeak(row);
        if (activity > 0.02f) g.Dither(Rect{table.x - 5, y + 1, 2, 8}, kPlay, static_cast<int>(activity * 16.0f));
      }
      break;
    }
  }
}

// SETUP SYSTEM: a terminal-style status report that types itself out (on every visit).
void DrawSystemInfo(Gfx& g, const Controller& controller, Animator& animator, const Rect& panel)
{
  PaintSystemInfo(g, controller, animator, panel);
}

void ProjectScene(Gfx& g, const Controller& controller, Animator& animator, std::uint8_t page)
{
  const UiState& state = controller.State();
  const Host& host = controller.HostView();
  const std::uint32_t now = animator.Now();
  const auto seq = host.Sequencer().BuildUiState(state.track);
  switch (page)
  {
    case 0:
    {
      const std::array<const char*, 3> actions{{"SAVE", "LOAD", "NEW"}};
      for (std::size_t index = 0; index < actions.size(); ++index)
      {
        widgets::ActionCell(g, Cell(0, index), actions[index], state.gridDown[index], kDim);
        GridRipple(g, animator, index, kInk);
      }
      char bpm[8], swing[8];
      std::snprintf(bpm, sizeof(bpm), "%u", static_cast<unsigned>(seq.bpm));
      std::snprintf(swing, sizeof(swing), "%u%%", static_cast<unsigned>(seq.swing));
      const float beat = seq.playing ? fx::Decay(now, animator.BeatAt(), 150) : 0.0f;
      widgets::ParamCell(g, Cell(1, 0), "BPM", bpm, animator.Smooth(Animator::Key("proj-bpm"), static_cast<float>(seq.bpm - 30) / 270.0f),
                         state.projectFocus == 4 ? CellStyle::Focused : CellStyle::Normal, beat * 0.6f, now);
      widgets::ParamCell(g, Cell(1, 1), "SWING", swing, animator.Smooth(Animator::Key("proj-swing"), static_cast<float>(seq.swing - 50) / 25.0f),
                         state.projectFocus == 5 ? CellStyle::Focused : CellStyle::Normal, 0.0f, now);
      widgets::ActionCell(g, Cell(1, 2), "TAP", state.gridDown[6], kDim);
      // PLAY: a switch, LOOP (each track loops its pattern) or SONG (the arrangement plays).
      const bool song = host.Sequencer().SongMode();
      widgets::ParamCell(g, Cell(1, 3), "PLAY", song ? "SONG" : "LOOP", song ? 1.0f : 0.0f, CellStyle::Normal, 0.0f, now);
      for (std::size_t index = 4; index < 8; ++index) GridRipple(g, animator, index, kInk);
      const Rect info = CellSpan(2, 0, 2, 4);
      const std::string name = state.projectName.empty() ? std::string("UNTITLED") : state.projectName;
      const int x = fx::DecodeText(g, info.x, info.y + 2, name, state.projectName.empty() ? kDim : kInk, Font::Large,
                                   fx::Progress(now, animator.ProjectChangedAt(), 500), animator.ProjectChangedAt() + now / 60, info.width - 60);
      if (state.dirty) g.Text(x + 6, info.y + 6, "UNSAVED", (now / 700) % 2 ? kFn : kDim);
      else if (!state.projectName.empty()) g.Text(x + 6, info.y + 6, "SAVED", kDim);
      for (std::size_t track = 0; track < kTrackCount; ++track)
      {
        const int column = static_cast<int>(track % 2);
        const int row = static_cast<int>(track / 2);
        const int tx = info.x + column * (info.width / 2);
        const int ty = info.y + 24 + row * 12;
        const float activity = animator.MeterPeak(track);
        g.Text(tx, ty, "T" + std::to_string(track + 1), activity > 0.3f ? kInk : TrackColor(track));
        const auto* engine = host.Engine(track);
        g.Text(tx + 16, ty, EngineCode(host.EngineKind(track)), kDim);
        if (engine != nullptr) g.TextFit(tx + 40, ty, info.width / 2 - 44, engine->CurrentPresetName(), kInk);
      }
      break;
    }
    case 1:
    {
      const Rect list{kContentX, kContentY, kContentWidth - 8, kContentHeight};
      if (state.projects.empty())
      {
        g.TextAligned(list, "NO PROJECTS ON THE CARD", (now / 600) % 2 ? kDim : kFaint, Font::Micro, Align::Center);
        break;
      }
      const std::size_t visible = 10;
      const std::size_t first = state.fileCursor >= visible ? state.fileCursor - visible + 1 : 0;
      // The highlight bar glides between rows.
      const float barY = animator.Smooth(Animator::Key("files-cursor"), static_cast<float>(list.y + 1 + static_cast<int>(state.fileCursor - first) * 10), 45.0f);
      g.Fill(Rect{list.x, static_cast<int>(std::lround(barY)), list.width, 9}, kInk);
      for (std::size_t row = 0; row < visible && first + row < state.projects.size(); ++row)
      {
        const std::size_t index = first + row;
        const int y = list.y + 2 + static_cast<int>(row) * 10;
        const bool covered = std::abs(static_cast<float>(y - 1) - barY) < 5.0f;
        const bool current = state.projects[index] == state.projectName;
        g.Text(list.x + 2, y, current ? ">" : " ", covered ? kBlack : kTab);
        g.TextFit(list.x + 10, y, list.width - 12, state.projects[index], covered ? kBlack : kInk);
      }
      widgets::ScrollIndicator(g, Rect{kContentRight - 2, kContentY + 1, 1, kContentHeight - 2}, first, visible, state.projects.size());
      break;
    }
  }
}

// TAPE: the song as a strip of tape. Columns are song rows (time runs left to right), rows are tracks,
// exactly as on the GRID keys. The playing row carries a moving tape head.
void TapeScene(Gfx& g, const Controller& controller, Animator& animator)
{
  SceneGrid grid;
  grid.area = Rect{kContentX, kContentY, kContentWidth, kContentHeight};
  for (std::size_t index = 0; index < kGridCount; ++index) grid.cells[index] = Cell(index / kGridColumns, index % kGridColumns);
  PaintSongGrid(g, controller, animator, grid);
}

void MixScene(Gfx& g, const Controller& controller, Animator& animator)
{
  const UiState& state = controller.State();
  const Host& host = controller.HostView();
  const std::uint32_t now = animator.Now();
  const auto seq = host.Sequencer().BuildUiState(state.track);
  for (std::size_t track = 0; track < kTrackCount; ++track)
  {
    const Rect top = Cell(0, track);
    const Rect bottom = Cell(3, track);
    const bool focused = state.mixFocus == track;
    const bool audible = host.TrackAudible(track);
    const Rect fader{top.x + 1, top.y + 1, 9, bottom.y + bottom.height - top.y - 2};
    PaintMixFader(g, controller, animator, fader, track, focused);

    const int x = top.x + 17;
    const int width = top.x + top.width - x;
    const std::string label = "T" + std::to_string(track + 1);
    if (focused)
    {
      g.Fill(Rect{x - 2, top.y, width + 2, 10}, TrackColor(track));
      g.Text(x, top.y + 2, label, kBlack);
    }
    else g.Text(x, top.y + 2, label, TrackColor(track));
    g.TextFit(x + 15, top.y + 2, width - 15, EngineCode(host.EngineKind(track)), focused ? kBlack : kDim);
    g.Text(x, top.y + 13, std::to_string(host.TrackLevel(track)), audible ? kInk : kDim, Font::Small);

    const auto badge = [&](std::size_t row) { const Rect cell = Cell(row, track); return Rect{x - 2, cell.y + 4, width + 2, 13}; };
    for (std::size_t kind = 0; kind < 3; ++kind)
    {
      PaintMixBadge(g, controller, animator, badge(kind + 1), track, kind);
    }
    for (std::size_t row = 0; row < kRows; ++row) GridRipple(g, animator, row * kGridColumns + track, kInk);
  }
}

// SESSION: the clip grid. Columns are the tracks (in their colors), rows are pattern slots. Each
// cell shows its pattern's steps as ticks; the playing pattern of a track carries a moving playhead
// and a green wash, a queued one crawling yellow ants, the next free slot a "+" to create it.
void SessionScene(Gfx& g, const Controller& controller, Animator& animator)
{
  SceneGrid grid;
  grid.area = Rect{kContentX, kContentY, kContentWidth, kContentHeight};
  for (std::size_t index = 0; index < kGridCount; ++index) grid.cells[index] = Cell(index / kGridColumns, index % kGridColumns);
  // Session labels have always used the compact micro font.
  grid.valueFont = Font::Micro;
  PaintSessionGrid(g, controller, animator, grid);
}

// SENDS: one column per track (like MIX), one row per effect: filter, chorus, delay send, reverb
// send. A cell glows at its right edge while its track sounds into an open send.
void SendsScene(Gfx& g, const Controller& controller, Animator& animator)
{
  const UiState& state = controller.State();
  const Host& host = controller.HostView();
  const std::uint32_t now = animator.Now();
  for (std::size_t index = 0; index < kGridCount; ++index)
  {
    const Rect cell = Cell(index / kGridColumns, index % kGridColumns);
    PaintSendCell(g, controller, animator, cell, index);
    GridRipple(g, animator, index, kInk);
  }
  const Rect focus = Cell(state.sendsFocus / kGridColumns, state.sendsFocus % kGridColumns);
  const std::uint32_t key = Animator::Key("sends-focus");
  const int x = static_cast<int>(std::lround(animator.Smooth(key, static_cast<float>(focus.x), 50.0f)));
  const int y = static_cast<int>(std::lround(animator.Smooth(key + 1, static_cast<float>(focus.y), 50.0f)));
  fx::MarchingAnts(g, Rect{x - 1, y - 1, focus.width + 2, focus.height + 2}, kInk, now);
}

std::string MasterFxText(const Host& host, std::size_t param)
{
  using synth::fx::MasterParam;
  const int value = host.MasterFx(param);
  switch (static_cast<MasterParam>(param))
  {
    case MasterParam::DelayTime: return synth::fx::kDelayDivisions[static_cast<std::size_t>(std::clamp(value, 0, 11))];
    case MasterParam::EqLow:
    case MasterParam::EqHigh:
    {
      const int db = (value - 64) * 12 / 64;
      return (db > 0 ? "+" : "") + std::to_string(db) + "DB";
    }
    case MasterParam::Compressor:
    case MasterParam::Tape:
      return value == 0 ? "OFF" : std::to_string(value);
    default: return std::to_string(value);
  }
}

// FX: the master bus. Row 0 delay (time synced to the tempo), row 1 reverb, row 2 the master
// compressor, EQ and tape saturation; row 3 draws what the two buses are doing.
void FxScene(Gfx& g, const Controller& controller, Animator& animator)
{
  const UiState& state = controller.State();
  const std::uint32_t now = animator.Now();
  for (std::size_t param = 0; param < synth::fx::kMasterParamCount; ++param)
  {
    const Rect cell = Cell(param / kGridColumns, param % kGridColumns);
    PaintMasterFxCell(g, controller, animator, cell, param);
    GridRipple(g, animator, param, kInk);
  }
  const Rect focus = Cell(state.fxFocus / kGridColumns, state.fxFocus % kGridColumns);
  const std::uint32_t focusKey = Animator::Key("fx-focus");
  const int fx0 = static_cast<int>(std::lround(animator.Smooth(focusKey, static_cast<float>(focus.x), 50.0f)));
  const int fy0 = static_cast<int>(std::lround(animator.Smooth(focusKey + 1, static_cast<float>(focus.y), 50.0f)));
  fx::MarchingAnts(g, Rect{fx0 - 1, fy0 - 1, focus.width + 2, focus.height + 2}, kInk, now);

  PaintMasterFxBuses(g, controller, animator, CellSpan(3, 0, 1, 2), CellSpan(3, 2, 1, 2));
}

}  // namespace

void PaintGridRipple(Gfx& g, const Animator& animator, std::size_t index, const Rect& rect, Color color)
{
  const auto pressed = animator.GridPressedAt(index);
  if (pressed != 0 && animator.Now() - pressed < kRippleMs)
    fx::Ripple(g, rect, fx::Progress(animator.Now(), pressed, kRippleMs), color);
}

void PaintFocus(Gfx& g, Animator& animator, const Rect& rect, std::uint32_t key, Color color)
{
  const int x = static_cast<int>(std::lround(animator.Smooth(key, static_cast<float>(rect.x), 50.0f)));
  const int y = static_cast<int>(std::lround(animator.Smooth(key + 1, static_cast<float>(rect.y), 50.0f)));
  fx::MarchingAnts(g, Rect{x - 1, y - 1, rect.width + 2, rect.height + 2}, color, animator.Now());
}

void PaintDrumPad(Gfx& g, const Rect& rect, const Controller& controller, Animator& animator,
                  const synth::ISynthEngine* engine, std::size_t pad, Font font, int labelY, int kickRoom)
{
  if (pad >= kDrumNotes.size()) return;
  static constexpr std::array<const char*, 8> ids{{"kick.level", "snare.level", "closed_hat.level", "open_hat.level",
      "clap.level", "tom.level", "cowbell.level", "maraca.level"}};
  const auto now = animator.Now();
  const bool held = controller.IsChordNoteHeld(kDrumNotes[pad]);
  const float hit = fx::Decay(now, animator.NoteHitAt(kDrumNotes[pad]), kAfterglowMs);
  const float level = Glide(animator, controller.State().track, engine, ids[pad], 0.8f);
  const float strength = static_cast<float>(animator.DrumHitVelocity(pad)) / 127.0f;
  const int density = DrumDensity(animator.DrumHitVelocity(pad));
  const int kick = static_cast<int>(hit * hit * strength * static_cast<float>(kickRoom));
  const Rect body{rect.x, rect.y - kick, rect.width, rect.height};
  const int filled = static_cast<int>(level * static_cast<float>(body.height - 2));
  g.Dither(Rect{body.x + 1, body.y + body.height - 1 - filled, body.width - 2, filled}, kFaint, 4);
  if (held) g.Dither(body, kInk, density);
  else if (hit > 0.0f) g.Dither(body, kInk, static_cast<int>(hit * static_cast<float>(density)));
  g.Stroke(body, held || hit > 0.0f ? kInk : kFaint);
  const int nameWidth = Gfx::Measure(kDrumNames[pad], font);
  const int nameX = body.x + (body.width - nameWidth) / 2;
  if (held || hit > 0.0f)
    g.Fill(Rect{nameX - 1, body.y + labelY - 1, nameWidth + 2, Gfx::CapHeight(font) + 2}, kBlack);
  g.Text(nameX, body.y + labelY, kDrumNames[pad], kInk, font);
  if (hit > 0.0f) fx::Ripple(g, body, 1.0f - hit, kInk);
}

void PaintMixFader(Gfx& g, const Controller& controller, Animator& animator, const Rect& fader,
                   std::size_t track, bool focused)
{
  const auto& host = controller.HostView();
  const bool audible = host.TrackAudible(track);
  const float level = animator.Smooth(Animator::Key("mix-level", static_cast<std::uint32_t>(track)),
      static_cast<float>(host.TrackLevel(track)) / 100.0f);
  widgets::Fader(g, fader, level, audible ? kInk : kFaint, focused);
  if (focused) fx::MarchingAnts(g, Rect{fader.x - 1, fader.y - 1, fader.width + 2, fader.height + 2}, TrackColor(track), animator.Now());
  const float activity = animator.Smooth(Animator::Key("mix-act", static_cast<std::uint32_t>(track)), host.TrackActivity(track), 35.0f);
  const int meter = static_cast<int>(activity * static_cast<float>(fader.height));
  g.Fill(Rect{fader.x + fader.width + 2, fader.y + fader.height - meter, 2, meter}, audible ? kPlay : kFaint);
  const int peakY = fader.y + fader.height - static_cast<int>(animator.MeterPeak(track) * static_cast<float>(fader.height));
  if (animator.MeterPeak(track) > 0.02f) g.HLine(fader.x + fader.width + 1, peakY, 4, audible ? kInk : kFaint);
}

void PaintMixBadge(Gfx& g, const Controller& controller, Animator& animator, const Rect& bounds,
                   std::size_t track, std::size_t kind, Font font)
{
  if (kind >= 3) return;
  const auto& host = controller.HostView();
  const std::array<bool, 3> states{{host.TrackMuted(track), host.TrackSoloed(track), host.Sequencer().TrackEnabled(track)}};
  static constexpr std::array<const char*, 3> names{{"MUTE", "SOLO", "SEQ"}};
  static constexpr std::array<Color, 3> colors{{kRec, kFn, kDim}};
  const float pop = fx::Decay(animator.Now(), animator.MixToggledAt(track, kind), 300);
  Rect rect = bounds;
  if (pop > 0.0f)
  {
    const int grow = static_cast<int>(fx::EaseOutBack(1.0f - pop) * 2.0f * pop);
    rect = Rect{rect.x - grow, rect.y - grow, rect.width + grow * 2, rect.height + grow * 2};
    fx::Ripple(g, rect, 1.0f - pop, colors[kind] == kDim ? kInk : colors[kind]);
  }
  if (font == Font::Micro) widgets::Badge(g, rect, names[kind], states[kind], colors[kind]);
  else
  {
    if (states[kind]) g.Fill(rect, colors[kind]);
    else
    {
      g.DottedHLine(rect.x, rect.y, rect.width, kFaint);
      g.DottedHLine(rect.x, rect.y + rect.height - 1, rect.width, kFaint);
    }
    g.TextAligned(rect, names[kind], states[kind] ? kBlack : kDim, font, Align::Center);
  }
}

void PaintSendCell(Gfx& g, const Controller& controller, Animator& animator, const Rect& cell,
                   std::size_t index, const widgets::ParamCellLayout& layout, bool retainUnavailableFocus)
{
  if (index >= kGridCount) return;
  const auto row = index / kGridColumns;
  const auto track = index % kGridColumns;
  const auto& host = controller.HostView();
  const auto* engine = host.Engine(track);
  const std::string label = "T" + std::to_string(track + 1) + " " + kSendTags[row];
  synth::SynthParameterInfo info;
  const bool focused = controller.State().sendsFocus == index;
  if (engine == nullptr || !engine->GetParameterInfo(kSendIds[row], info))
  {
    widgets::ParamCell(g, cell, label, "--", -1.0f, retainUnavailableFocus && focused, true, layout);
    return;
  }
  const bool off = info.value == info.defaultValue;
  g.Fill(Rect{cell.x - 2, cell.y, 1, cell.height}, TrackColor(track));
  const std::string text = engine->DescribeParameterValue(kSendIds[row]);
  const auto key = Animator::Key(kSendIds[row], static_cast<std::uint32_t>(track) * 53u + 5u);
  if (row == 0)
  {
    widgets::ParamCell(g, cell, label, text, -1.0f, focused, !focused && off, layout);
    const float value = animator.Smooth(key, static_cast<float>(info.value - synth::fx::kFilterCenter) / 64.0f, 60.0f);
    widgets::BipolarBar(g, Rect{cell.x + layout.paddingX, cell.y + cell.height - layout.barBottom,
        cell.width - layout.paddingX - layout.rightPadding, layout.barHeight}, value, kInk, kFaint);
  }
  else
  {
    const float fraction = animator.Smooth(key, static_cast<float>(info.value - info.minValue) / static_cast<float>(info.maxValue - info.minValue), 60.0f);
    widgets::ParamCell(g, cell, label, text, fraction, focused, !focused && off, layout);
  }
  const float activity = host.TrackAudible(track) ? host.TrackActivity(track) : 0.0f;
  if (!off && activity > 0.02f)
    g.Dither(Rect{cell.x + cell.width - 3, cell.y + 1, 2, cell.height - 2}, kTab, static_cast<int>(4.0f + activity * 12.0f));
}

std::string MasterFxDisplayText(const Host& host, std::size_t param) { return MasterFxText(host, param); }

void PaintMasterFxCell(Gfx& g, const Controller& controller, Animator& animator, const Rect& cell,
                       std::size_t param, const widgets::ParamCellLayout& layout)
{
  if (param >= synth::fx::kMasterParamCount) return;
  using synth::fx::MasterParam;
  static constexpr std::array<const char*, synth::fx::kMasterParamCount> labels{{
      "DELAY", "FDBK", "TONE", "RETURN", "REVERB", "DAMP", "LOCUT", "RETURN", "COMP", "LOW", "HIGH", "TAPE"}};
  const auto& host = controller.HostView();
  const auto& info = synth::fx::kMasterParams[param];
  const int value = host.MasterFx(param);
  const bool focused = controller.State().fxFocus == param;
  const bool eq = param == static_cast<std::size_t>(MasterParam::EqLow) || param == static_cast<std::size_t>(MasterParam::EqHigh);
  const auto key = Animator::Key("master-fx", static_cast<std::uint32_t>(param));
  if (eq)
  {
    widgets::ParamCell(g, cell, labels[param], MasterFxText(host, param), -1.0f, focused, false, layout);
    widgets::BipolarBar(g, Rect{cell.x + layout.paddingX, cell.y + cell.height - layout.barBottom,
        cell.width - layout.paddingX - layout.rightPadding, layout.barHeight},
        animator.Smooth(key, static_cast<float>(value - 64) / 64.0f, 60.0f), kInk, kFaint);
  }
  else
  {
    const float fraction = animator.Smooth(key, static_cast<float>(value - info.minValue) / static_cast<float>(info.maxValue - info.minValue), 60.0f);
    widgets::ParamCell(g, cell, labels[param], MasterFxText(host, param), fraction, focused, false, layout);
  }
}

void PaintSystemInfo(Gfx& g, const Controller& controller, Animator& animator, const Rect& panel, const SystemInfoLayout& layout)
{
  const auto& host = controller.HostView();
  const SystemInfo info = host.QuerySystem();
  const auto now = animator.Now();
  const float typed = fx::Clamp01(static_cast<float>(now - animator.CurrentTransition().start) / 600.0f);
  int y = panel.y + 4;
  int line = 0;
  const auto row = [&](const char* label, const std::string& value, Color color) {
    const float reveal = fx::Clamp01(typed * (layout.extended ? 7.0f : 5.0f) - static_cast<float>(line));
    g.Text(panel.x + layout.padding, y, label, kDim, layout.font);
    // TypeText is clipped by the caller's content region, including transitions.
    fx::TypeText(g, panel.x + layout.padding + layout.labelWidth, y, value, color, layout.font, reveal);
    y += layout.rowHeight;
    ++line;
  };
  row("VERSION", cardenza::build::kVersion, kInk);
  if (layout.extended)
  {
    row("BUILD", std::string(cardenza::build::kHash) + (cardenza::build::kDirty ? "+ " : " ") + std::string(cardenza::build::kTimeUtc).substr(5, 11), kDim);
    row("STORAGE", info.storageReady ? "READY" : "UNAVAILABLE", info.storageReady ? kPlay : kRec);
    row("INTERNAL", std::to_string(info.freeHeap / 1024) + "K MIN " + std::to_string(info.minimumFreeHeap / 1024) + "K", kInk);
    row("PSRAM", std::to_string(info.freePsram / 1024) + " / " + std::to_string(info.totalPsram / 1024) + " KB", kInk);
    row("OUTPUT", std::to_string(host.MasterVolume()) + "%", kInk);
  }
  else
  {
    row("SD CARD", info.storageReady ? "READY" : "NOT FOUND", info.storageReady ? kPlay : kRec);
    std::string memory = "N/A";
    if (info.freeHeap != 0 && info.totalPsram != 0)
      memory = std::to_string(info.freeHeap / 1024) + "K MIN " + std::to_string(info.minimumFreeHeap / 1024) + "K  PSRAM " +
               std::to_string(info.freePsram / 1024) + "K";
    else if (info.freeHeap != 0)
      memory = std::to_string(info.freeHeap / 1024) + "K FREE  MIN " + std::to_string(info.minimumFreeHeap / 1024) + "K";
    row("MEMORY", memory, kInk);
    row("BUILD", std::string(cardenza::build::kHash) + (cardenza::build::kDirty ? "+ " : " ") + std::string(cardenza::build::kTimeUtc).substr(5, 11), kDim);
  }
}

PianoGeometry PaintPiano(Gfx& g, const Rect& rect, const Controller& controller, Animator& animator, int baseNote, bool showScale,
                         const std::array<bool, 128>* marked)
{
  PianoGeometry geometry;
  if (rect.width < 15 || rect.height <= 0) return geometry;
  // The pads' two octaves: from baseNote (a C) to the C two octaves up. Outlined white keys, dithered
  // black keys; held notes (whole chords) light up in the track color and fade after release. With
  // showScale, scale tones carry a dot and the root a bar.
  static constexpr std::array<int, 7> kWhite{{0, 2, 4, 5, 7, 9, 11}};
  static constexpr std::array<int, 7> kBlackAfter{{1, 3, -1, 6, 8, 10, -1}};
  const std::uint32_t now = animator.Now();
  const KeyboardSettings& keys = controller.State().keyboard;
  const Color held = TrackColor(controller.State().track);
  const int whiteCount = 15;  // C to C, two octaves
  const int keyWidth = rect.width / whiteCount;
  const int left = rect.x + (rect.width - keyWidth * whiteCount) / 2;
  auto mark = [&](const Rect& key, int note, Color color) {
    if (!showScale || !InScale(keys, note)) return;
    const bool root = ((note - keys.root) % 12 + 12) % 12 == 0;
    if (root) g.Fill(Rect{key.x + 1, key.y + key.height - 3, key.width - 2, 2}, color);
    else g.Pixel(key.x + key.width / 2, key.y + key.height - 3, color);
  };
  for (int index = 0; index < whiteCount; ++index)
  {
    const int note = baseNote + (index / 7) * 12 + kWhite[static_cast<std::size_t>(index % 7)];
    const Rect key{left + index * keyWidth, rect.y, keyWidth - 1, rect.height};
    if (note >= 0 && note < 128) geometry.keys[geometry.count++] = PianoKey{key, note};
    const float glow = fx::Decay(now, animator.NoteHitAt(note), kAfterglowMs);
    const bool down = controller.IsChordNoteHeld(note);
    if (down) g.Fill(key, held);
    else
    {
      if (marked != nullptr && note >= 0 && note < 128 && (*marked)[static_cast<std::size_t>(note)]) g.Dither(key, held, 8);
      if (glow > 0.0f) g.Dither(key, kInk, static_cast<int>(glow * 11.0f));
      g.Stroke(key, kDim);
    }
    mark(key, note, down ? kBlack : kInk);
  }
  for (int index = 0; index < whiteCount; ++index)
  {
    const int black = kBlackAfter[static_cast<std::size_t>(index % 7)];
    if (black < 0 || index + 1 == whiteCount) continue;
    const int note = baseNote + (index / 7) * 12 + black;
    const Rect key{left + index * keyWidth + keyWidth * 2 / 3, rect.y, keyWidth * 2 / 3, rect.height * 3 / 5};
    if (note >= 0 && note < 128) geometry.keys[geometry.count++] = PianoKey{key, note};
    const bool down = controller.IsChordNoteHeld(note);
    const float glow = fx::Decay(now, animator.NoteHitAt(note), kAfterglowMs);
    const bool chordTone = marked != nullptr && note >= 0 && note < 128 && (*marked)[static_cast<std::size_t>(note)];
    g.Fill(key, down || chordTone ? held : kBlack);
    if (!down && !chordTone)
    {
      g.Dither(Rect{key.x + 1, key.y, key.width - 2, key.height - 1}, glow > 0.0f ? held : kDim, glow > 0.0f ? 4 + static_cast<int>(glow * 10.0f) : 8);
      g.Stroke(key, kFaint);
    }
    mark(key, note, kInk);
  }
  // Each octave's C named, as on the pads' bottom left of each pair of rows.
  for (const int octave : {0, 1})
  {
    const int c = baseNote + octave * 12;
    g.Text(left + octave * 7 * keyWidth + 2, rect.y + rect.height - 9, NoteName(c), controller.IsChordNoteHeld(c) ? kBlack : kDim);
  }

  return geometry;
}

void PaintFxChain(Gfx& g, const Controller& controller, Animator& animator, const Rect& area)
{
  const Host& host = controller.HostView();
  const std::size_t track = controller.State().track;
  const auto* engine = host.Engine(track);
  const std::uint32_t now = animator.Now();
  const float activity = host.TrackActivity(track);
  const int lineY = area.y + 12;
  const int startX = area.x + 14;
  const int endX = area.x + area.width - 22;
  g.Text(area.x, lineY - 3, "IN", kDim);
  g.HLine(startX, lineY, endX - startX, kFaint);
  g.Text(endX + 3, lineY - 3, "OUT", kDim);

  struct Stage { const char* tag; const char* id; float amount; };
  const int filterValue = EngineRaw(engine, "fx.filter", synth::fx::kFilterCenter);
  const std::array<Stage, 4> stages{{
      {"DRV", "fx.drive", Glide(animator, track, engine, "fx.drive")},
      {"CRS", "fx.crush", Glide(animator, track, engine, "fx.crush")},
      {"FLT", "fx.filter", std::fabs(static_cast<float>(filterValue - synth::fx::kFilterCenter)) / 64.0f},
      {"CHO", "fx.chorus", static_cast<float>(EngineRaw(engine, "fx.chorus")) / 3.0f},
  }};
  const int pitch = (endX - startX) / 4;
  const int boxWidth = pitch - 8;
  for (std::size_t index = 0; index < stages.size(); ++index)
  {
    const Stage& stage = stages[index];
    const Rect box{startX + 4 + static_cast<int>(index) * pitch, lineY - 9, boxWidth, 18};
    const bool on = stage.amount > 0.001f;
    g.Fill(box, kBlack);
    if (on)
    {
      g.Dither(box, kInk, 2 + static_cast<int>(stage.amount * 10.0f));
      g.Stroke(box, kInk);
    }
    else
    {
      g.DottedHLine(box.x, box.y, box.width, kFaint);
      g.DottedHLine(box.x, box.y + box.height - 1, box.width, kFaint);
      g.DottedVLine(box.x, box.y, box.height, kFaint);
      g.DottedVLine(box.x + box.width - 1, box.y, box.height, kFaint);
    }
    const Rect label{box.x + 1, box.y + 1, box.width - 2, 8};
    if (on) g.Fill(Rect{label.x, label.y, Gfx::Measure(stage.tag) + 4, 9}, kBlack);
    g.Text(label.x + 2, label.y + 1, stage.tag, on ? kInk : kFaint);
    if (index == 2 && on)
    {
      // Filter: a tiny slope, falling for low-pass, rising for high-pass.
      const bool highPass = filterValue > synth::fx::kFilterCenter;
      const int x0 = box.x + box.width - 14;
      for (int dx = 0; dx < 11; ++dx)
      {
        const int height = highPass ? std::min(6, dx) : std::min(6, 10 - dx);
        g.Pixel(x0 + dx, box.y + box.height - 3 - height, kInk);
      }
    }
    if (index == 3 && on)
    {
      const std::string mode = synth::fx::kChorusModes[static_cast<std::size_t>(std::clamp(EngineRaw(engine, "fx.chorus"), 0, 3))];
      g.Text(box.x + box.width - Gfx::Measure(mode) - 2, box.y + 10, mode, kInk);
    }
  }

  // Sends: post-fader taps into the master buses.
  const int tapX = endX - 6;
  const int busY = area.y + 30;
  const std::array<const char*, 2> sendIds{{"fx.delay", "fx.reverb"}};
  const std::array<const char*, 2> sendTags{{"DELAY", "REVERB"}};
  const std::array<std::size_t, 2> buses{{synth::fx::kBusDelay, synth::fx::kBusReverb}};
  const int sendWidth = 70;
  bool anySend = false;
  for (std::size_t send = 0; send < 2; ++send)
  {
    const float amount = Glide(animator, track, engine, sendIds[send]);
    anySend = anySend || amount > 0.001f;
    const Rect box{area.x + 52 + static_cast<int>(send) * (sendWidth + 14), busY + 4, sendWidth, area.height - (busY - area.y) - 5};
    const int dropX = box.x + box.width / 2;
    if (amount > 0.001f)
    {
      g.HLine(std::min(dropX, tapX), busY, std::abs(tapX - dropX) + 1, kDim);
      g.VLine(dropX, busY, 4, kDim);
    }
    g.Stroke(box, amount > 0.001f ? kDim : kShade);
    g.Text(box.x + 3, box.y + 3, sendTags[send], amount > 0.001f ? kInk : kFaint);
    widgets::LevelBar(g, Rect{box.x + 3, box.y + box.height - 5, box.width - 6, 2}, amount, kInk, kFaint);
    // The return meter of the master bus: how much is coming back from it right now.
    const float wet = host.FxReturnLevel(buses[send]);
    if (!host.FxBusAvailable(buses[send])) g.Text(box.x + box.width - Gfx::Measure("NO MEM") - 3, box.y + 3, "NO MEM", kRec);
    else if (wet > 0.01f) g.Dither(Rect{box.x + box.width - 8, box.y + 2, 5, box.height - 9}, kTab, static_cast<int>(4.0f + wet * 12.0f));
  }
  if (anySend) g.VLine(tapX, lineY, busY - lineY + 1, kDim);

  // Signal dots while the track sounds.
  if (activity > 0.02f)
  {
    const float travel = static_cast<float>(now % 900) / 900.0f;
    for (int dot = 0; dot < 3; ++dot)
    {
      const float position = std::fmod(travel + static_cast<float>(dot) / 3.0f, 1.0f);
      const int x = startX + static_cast<int>(position * static_cast<float>(endX - startX));
      g.Fill(Rect{x - 1, lineY - 1, 3, 3}, kTab);
    }
  }
}

void PaintPianoRoll(Gfx& g, const Controller& controller, Animator& animator, const synth::SequencerUiState& seq,
                    const PianoRollLayout& layout)
{
  const UiState& state = controller.State();
  const std::uint32_t now = animator.Now();
  const auto& sequencer = controller.HostView().Sequencer();
  // The roll shows the GRID's page of the pattern.
  const std::size_t length = std::max<std::size_t>(1, seq.tracks[state.track].stepCount);
  const std::size_t first = seq.firstViewStep;
  const std::size_t steps = std::clamp<std::size_t>(length > first ? length - first : 1, 1, kGridCount);
  const Color trackColor = TrackColor(state.track);
  const int kGutter = layout.gutter;
  const int kStrip = layout.strip;
  const int x0 = layout.area.x + kGutter;
  // The page's steps share the width, so a pattern shorter than the GRID fills it too (with a column
  // a sixteenth wide, an 8-step pattern took the left half). The strip under it follows the columns.
  const int column = (layout.area.width - kGutter) / static_cast<int>(steps);
  const int top = layout.area.y + 1;
  const int bottom = (layout.area.y + layout.area.height) - kStrip - 2;

  // Every note of the pattern, to fit the pitch range around them.
  std::array<std::array<synth::SynthSequencer::StepNoteView, 8>, kGridCount> notes{};
  std::array<std::size_t, kGridCount> counts{};
  int low = 128, high = -1;
  for (std::size_t step = 0; step < kGridCount; ++step)
  {
    counts[step] = sequencer.StepNoteEvents(state.track, first + step, notes[step]);
    for (std::size_t index = 0; index < counts[step]; ++index)
    {
      low = std::min<int>(low, notes[step][index].note);
      high = std::max<int>(high, notes[step][index].note);
    }
  }
  // Notes of the page before that still sound on this one.
  std::array<std::array<synth::SynthSequencer::StepNoteView, 8>, kGridCount> carried{};
  std::array<std::size_t, kGridCount> carriedCounts{};
  if (first >= kGridCount)
  {
    for (std::size_t step = 0; step < kGridCount; ++step)
    {
      const std::size_t pulseStart = (first - kGridCount + step) * synth::kSequencerPulsesPerStep;
      std::array<synth::SynthSequencer::StepNoteView, 8> before{};
      const std::size_t count = sequencer.StepNoteEvents(state.track, first - kGridCount + step, before);
      for (std::size_t index = 0; index < count; ++index)
      {
        const std::size_t end = pulseStart + before[index].microstep * synth::kSequencerPulsesPerMicrostep + before[index].lengthPulses;
        if (end <= first * synth::kSequencerPulsesPerStep) continue;
        carried[step][carriedCounts[step]++] = before[index];
        low = std::min<int>(low, before[index].note);
        high = std::max<int>(high, before[index].note);
      }
    }
  }
  // The notes the pads hold (chord tones too): their rows light up, the rows a tapped step writes.
  // How hard: a drum's row lights as dense as its pad hits, a note's as a checker.
  const bool drums = controller.DrumTrack();
  std::array<std::uint8_t, 128> pressed{};
  int pressedLow = 128, pressedHigh = -1;
  for (std::size_t pad = 0; pad < state.padNote.size(); ++pad)
  {
    if (state.padNote[pad] == kNoNote || state.padTrack[pad] != state.track) continue;
    const std::uint8_t velocity = std::max<std::uint8_t>(state.padVelocity[pad], 1);
    const auto press = [&](std::uint8_t note) {
      pressed[note & 0x7F] = std::max(pressed[note & 0x7F], velocity);
      pressedLow = std::min<int>(pressedLow, note & 0x7F);
      pressedHigh = std::max<int>(pressedHigh, note & 0x7F);
    };
    press(state.padNote[pad]);
    for (std::size_t voice = 0; voice < state.padChordCount[pad]; ++voice) press(state.padChord[pad][voice]);
  }
  for (int note = 0; note < 128; ++note)
    if (pressed[static_cast<std::size_t>(note)] == 0 && controller.IsChordNoteHeld(note))
    {
      pressed[static_cast<std::size_t>(note)] = state.keyboard.velocity;
      pressedLow = std::min(pressedLow, note);
      pressedHigh = std::max(pressedHigh, note);
    }
  // A melodic track's roll keeps every key the pads play at this octave, used or not (nothing folds
  // away), widened for notes of the page and held notes outside it. A drum track's fits its hits.
  int padLow = 128, padHigh = -1;
  if (!drums)
    for (std::size_t row = 0; row < kRows; ++row)
      for (std::size_t column = kPadFirstColumn; column < kPadFirstColumn + kPadColumns; ++column)
      {
        int note = 0;
        if (!controller.PadNoteFor(row, column, note)) continue;
        padLow = std::min(padLow, note);
        padHigh = std::max(padHigh, note);
      }
  if (padHigh >= 0)
  {
    low = std::min(high < 0 ? padLow : low, padLow);
    high = std::max(high, padHigh);
  }
  else if (high < 0)
  {
    low = static_cast<int>(synth::kHomePerformanceBaseMidiNote) + std::clamp(state.keyboard.octave, -3, 3) * 12;
    high = low + 12;
  }
  else
  {
    low -= 2;
    high += 2;
    while (high - low < 12)
    {
      --low;
      ++high;
    }
  }
  // A held note outside the range widens it, so its row is on screen.
  if (pressedHigh >= 0)
  {
    low = std::min(low, pressedLow - (padHigh >= 0 ? 0 : 1));
    high = std::max(high, pressedHigh + (padHigh >= 0 ? 0 : 1));
  }
  // At most 41 rows of 2 pixels: too wide a range keeps the 41 around the pads' (or the notes') middle.
  if (high - low > 40)
  {
    const int middle = padHigh >= 0 ? (padLow + padHigh) / 2 : (low + high) / 2;
    low = std::clamp(middle - 20, low, high - 40);
    high = low + 40;
  }
  low = std::max(0, low);
  high = std::min(127, high);
  const int rows = high - low + 1;
  const int rowHeight = std::max(2, (bottom - top) / rows);
  const int height = rowHeight * rows;
  const int y0 = top + (bottom - top - height) / 2;
  auto rowY = [&](int note) { return y0 + (high - note) * rowHeight; };

  // Keyboard rows: black keys shaded, every C labelled.
  for (int note = low; note <= high; ++note)
  {
    const int pitch = note % 12;
    const bool black = pitch == 1 || pitch == 3 || pitch == 6 || pitch == 8 || pitch == 10;
    const int y = rowY(note);
    if (black) g.Dither(Rect{x0, y, column * static_cast<int>(steps), rowHeight}, kShade, 3);
    if (pitch == 0)
    {
      g.DottedHLine(x0, y + rowHeight - 1, column * static_cast<int>(steps), kShade, 2);
      if (rowHeight >= 3 || note % 24 == 0) g.Text(layout.area.x, std::max(top, y + rowHeight - 7), NoteName(note), kFaint, Font::Micro);
    }
  }
  // Held notes: their rows lit across the page (a checker; a drum as dense as its pad hits), their
  // names (a drum's) inverse in the gutter.
  for (int note = low; note <= high; ++note)
  {
    const std::uint8_t held = pressed[static_cast<std::size_t>(note)];
    if (held == 0) continue;
    const int y = rowY(note);
    g.Dither(Rect{x0, y, column * static_cast<int>(steps), rowHeight}, kInk, drums ? DrumDensity(held) : 8);
    const int labelY = std::clamp(y + rowHeight / 2 - 4, top, bottom - 8);
    const std::string_view drum = drums ? DrumName(note) : std::string_view{};
    g.Fill(Rect{layout.area.x, labelY, kGutter - 2, 8}, kInk);
    g.Text(layout.area.x + 1, labelY + 1, drum.empty() ? NoteName(note) : std::string(drum), kBlack, Font::Micro);
  }
  // Beats, the selection and the playhead.
  for (std::size_t step = 0; step <= steps; step += 4)
    for (int y = y0; y < y0 + height; y += 2) g.Pixel(x0 + static_cast<int>(step) * column, y, step % 16 == 0 ? kDim : kShade);
  for (std::size_t step = 0; step < steps; ++step)
    if (seq.steps[step].held) g.Dither(Rect{x0 + static_cast<int>(step) * column, y0, column, height}, kInk, 3);
  // The notes.
  for (std::size_t step = 0; step < kGridCount; ++step)
  {
    const std::size_t pulseStart = step * synth::kSequencerPulsesPerStep;
    for (std::size_t index = 0; index < carriedCounts[step]; ++index)
    {
      const auto& note = carried[step][index];
      if (note.note < low || note.note > high) continue;
      const int inPage = static_cast<int>(pulseStart + note.microstep * synth::kSequencerPulsesPerMicrostep + note.lengthPulses) - static_cast<int>(kGridCount * synth::kSequencerPulsesPerStep);
      const Rect bar{x0, rowY(note.note), std::min(inPage * column / static_cast<int>(synth::kSequencerPulsesPerStep), column * static_cast<int>(steps)), std::max(1, rowHeight - 1)};
      // On a lit row the bar is cut out of the light, solid in the track's color.
      if (pressed[note.note & 0x7F] != 0)
      {
        g.Fill(Rect{bar.x, bar.y - 1, bar.width, bar.height + 2}, kBlack);
        g.Fill(bar, trackColor);
        continue;
      }
      g.Dither(bar, trackColor, 6 + note.velocity * 10 / 127);
    }
  }
  for (std::size_t step = 0; step < steps; ++step)
  {
    for (std::size_t index = 0; index < counts[step]; ++index)
    {
      const auto& note = notes[step][index];
      if (note.note < low || note.note > high) continue;
      const int x = x0 + static_cast<int>(step) * column + static_cast<int>(note.microstep) * column / synth::kSequencerMicrostepCount;
      const int width = std::max(2, static_cast<int>(note.lengthPulses) * column / synth::kSequencerPulsesPerStep - 1);
      const Rect bar{x, rowY(note.note), std::min(width, x0 + column * static_cast<int>(steps) - x), std::max(1, rowHeight - 1)};
      // Notes keep the track's color everywhere: a selected step's (its column is lit) and a lit
      // row's are solid, cut out of the light; the others as dense as they are loud.
      const bool held = seq.steps[step].held;
      if (pressed[note.note & 0x7F] != 0 || held)
      {
        g.Fill(Rect{bar.x, bar.y - 1, bar.width, bar.height + 2}, kBlack);
        g.Fill(bar, trackColor);
        continue;
      }
      g.Dither(bar, trackColor, 6 + note.velocity * 10 / 127);
      g.Fill(Rect{bar.x, bar.y, 1, bar.height}, trackColor);
    }
  }
  if (seq.playing)
  {
    const float head = seq.patternProgress * static_cast<float>(length) - static_cast<float>(first);
    if (head >= 0.0f && head < static_cast<float>(steps)) g.VLine(x0 + static_cast<int>(head * static_cast<float>(column)), y0, height, kPlay);
  }
  // The GRID strip.
  const int stripY = (layout.area.y + layout.area.height) - kStrip;
  for (std::size_t step = 0; step < kGridCount; ++step)
  {
    const Rect cell{x0 + static_cast<int>(step) * column + 1, stripY, column - 2, kStrip - 2};
    if (step >= steps) continue;
    const auto& info = seq.steps[step];
    if (info.held) g.Fill(cell, kInk);
    else if (info.playing && seq.playing) g.Fill(cell, kPlay);
    else if (info.hasEvents) g.Dither(cell, trackColor, 10);
    else g.Stroke(cell, kShade);
    if (step % 4 == 0) g.Text(cell.x, stripY - 8, std::to_string(first + step + 1), kFaint, Font::Micro);
  }
  // An empty pattern says how to fill it, unless a held note's row is to be seen.
  if (layout.showHint && seq.populatedStepCount == 0 && pressedHigh < 0)
  {
    const Rect band{x0, y0 + height / 2 - 10, column * static_cast<int>(steps), 20};
    g.Fill(band, kBlack);
    g.TextAligned(Rect{band.x, band.y, band.width, 10}, "HOLD A PAD + TAP STEPS", kDim, Font::Micro, Align::Center);
    g.TextAligned(Rect{band.x, band.y + 10, band.width, 10}, "TAP STEP + PADS  HOLD OK TAKES", (now / 600) % 2 ? kFaint : kShade, Font::Micro, Align::Center);
  }
}

void PaintStepMachine(Gfx& g, const Controller& controller, Animator& animator, const synth::SequencerUiState& seq,
                      const StepMachineLayout& layout)
{
  const UiState& state = controller.State();
  const std::uint32_t now = animator.Now();
  const auto& sequencer = controller.HostView().Sequencer();
  const std::size_t length = seq.tracks[state.track].stepCount;
  const std::size_t first = seq.firstViewStep;
  const Color trackColor = TrackColor(state.track);
  const bool drums = controller.DrumTrack();
  const bool playingHere = seq.playing && seq.currentStep >= first && seq.currentStep < first + kGridCount;

  // How hard each note is held on the pads (0: not held), chord tones too.
  std::array<std::uint8_t, 128> heldVelocity{};
  for (std::size_t pad = 0; pad < state.padNote.size(); ++pad)
  {
    if (state.padNote[pad] == kNoNote || state.padTrack[pad] != state.track) continue;
    const std::uint8_t velocity = std::max<std::uint8_t>(state.padVelocity[pad], 1);
    const auto hold = [&](std::uint8_t note) { heldVelocity[note & 0x7F] = std::max(heldVelocity[note & 0x7F], velocity); };
    hold(state.padNote[pad]);
    for (std::size_t voice = 0; voice < state.padChordCount[pad]; ++voice) hold(state.padChord[pad][voice]);
  }
  for (int note = 0; note < 128; ++note)
    if (heldVelocity[static_cast<std::size_t>(note)] == 0 && controller.IsChordNoteHeld(note))
      heldVelocity[static_cast<std::size_t>(note)] = state.keyboard.velocity;
  std::array<std::array<synth::SynthSequencer::StepNoteView, 8>, kGridCount> notes{};
  std::array<std::size_t, kGridCount> counts{};
  for (std::size_t step = 0; step < kGridCount; ++step) counts[step] = sequencer.StepNoteEvents(state.track, first + step, notes[step]);
  // The drum or note the GRID shows: the one the pads chose last.
  const int picked = drums ? kDrumNotes[controller.DrumLane()] : (state.noteLane[state.track] == kNoNote ? -1 : state.noteLane[state.track]);

  // The rows. A melodic track's: from the highest pitch down, the pads' range at this octave widened
  // for notes of the page, held and chosen notes outside it; at most 41 rows, around the pads' middle.
  int low = 0, high = 0;
  int rows = static_cast<int>(kDrumNotes.size());
  if (!drums)
  {
    int padLow = 128, padHigh = -1;
    for (std::size_t row = 0; row < kRows; ++row)
      for (std::size_t column = kPadFirstColumn; column < kPadFirstColumn + kPadColumns; ++column)
      {
        int note = 0;
        if (!controller.PadNoteFor(row, column, note)) continue;
        padLow = std::min(padLow, note);
        padHigh = std::max(padHigh, note);
      }
    low = padHigh >= 0 ? padLow : 128;
    high = padHigh;
    const auto widen = [&](int note) {
      low = std::min(low, note);
      high = std::max(high, note);
    };
    for (std::size_t step = 0; step < kGridCount; ++step)
      for (std::size_t slot = 0; slot < counts[step]; ++slot) widen(notes[step][slot].note);
    for (int note = 0; note < 128; ++note)
      if (heldVelocity[static_cast<std::size_t>(note)] != 0) widen(note);
    if (picked >= 0) widen(picked);
    if (high < 0)
    {
      low = static_cast<int>(synth::kHomePerformanceBaseMidiNote) + std::clamp(state.keyboard.octave, -3, 3) * 12;
      high = low + 12;
    }
    if (high - low > 40)
    {
      const int middle = padHigh >= 0 ? (padLow + padHigh) / 2 : (low + high) / 2;
      low = std::clamp(middle - 20, low, high - 40);
      high = low + 40;
    }
    rows = high - low + 1;
  }
  const auto rowOf = [&](int note) {
    if (!drums) return note < low || note > high ? -1 : high - note;
    for (std::size_t lane = 0; lane < kDrumNotes.size(); ++lane)
      if (kDrumNotes[lane] == note) return static_cast<int>(lane);
    return -1;
  };
  const auto noteOf = [&](int row) { return drums ? static_cast<int>(kDrumNotes[static_cast<std::size_t>(row)]) : high - row; };
  const int chosen = picked >= 0 ? rowOf(picked) : -1;

  const int label = layout.labelWidth;
  const int kColumn = layout.columnWidth;
  const int kMatrixWidth = kColumn * static_cast<int>(kGridCount);
  const int rowHeight = drums ? layout.matrix.height / static_cast<int>(kDrumNotes.size()) : std::max(2, layout.matrix.height / rows);
  const int matrixHeight = rowHeight * rows;
  const int x0 = layout.matrix.x + label;
  const int top = layout.matrix.y + (layout.matrix.height - matrixHeight) / 2;
  const int steps = static_cast<int>(std::min<std::size_t>(kGridCount, length > first ? length - first : 0));

  if (layout.showMatrix)
  {
  // Row backgrounds: black keys shaded (melodic), the chosen row, the rows whose pads are down.
  for (int row = 0; row < rows; ++row)
  {
    const int y = top + row * rowHeight;
    const int note = noteOf(row);
    const std::uint8_t held = heldVelocity[static_cast<std::size_t>(note)];
    const int pitch = note % 12;
    if (!drums && (pitch == 1 || pitch == 3 || pitch == 6 || pitch == 8 || pitch == 10)) g.Dither(Rect{x0, y, kMatrixWidth, rowHeight}, kShade, 3);
    // Lit across: a drum as dense as its pad hits (the four pad rows), a note a checker.
    if (held != 0) g.Dither(Rect{x0, y, kMatrixWidth, rowHeight - (drums ? 1 : 0)}, kInk, drums ? DrumDensity(held) : 8);
    else if (row == chosen) g.Dither(Rect{x0, y, kMatrixWidth, rowHeight - (drums ? 1 : 0)}, kShade, 8);
    if (!drums && pitch == 0) g.DottedHLine(x0, y + rowHeight - 1, kMatrixWidth, kShade, 2);
  }
  // Beats (melodic: the rows are too thin for the dots of the drum rows), the selected steps' columns.
  if (!drums)
    for (int step = 0; step <= steps; step += 4)
      for (int y = top; y < top + matrixHeight; y += 2) g.Pixel(x0 + step * kColumn, y, step % 16 == 0 ? kDim : kShade);
  for (int step = 0; step < steps; ++step)
    if (seq.steps[static_cast<std::size_t>(step)].held) g.Dither(Rect{x0 + step * kColumn, top, kColumn, matrixHeight}, kInk, 3);
  // Names: every drum; a melodic track's C rows, and the chosen and held notes inverse.
  for (int row = 0; row < rows; ++row)
  {
    const int y = top + row * rowHeight;
    const int note = noteOf(row);
    const bool lit = row == chosen || heldVelocity[static_cast<std::size_t>(note)] != 0;
    if (drums)
    {
      if (lit) g.Fill(Rect{layout.matrix.x, y, label - 2, rowHeight - 1}, kInk);
      g.Text(layout.matrix.x + 1, y + (rowHeight - 6) / 2, kDrumNames[static_cast<std::size_t>(row)], lit ? kBlack : kDim, Font::Micro);
      for (int step = 0; step < steps; ++step)
      {
        bool hit = false;
        for (std::size_t slot = 0; slot < counts[static_cast<std::size_t>(step)]; ++slot) hit = hit || notes[static_cast<std::size_t>(step)][slot].note == note;
        if (!hit && heldVelocity[static_cast<std::size_t>(note)] == 0) g.Pixel(x0 + step * kColumn + 2, y + rowHeight / 2, step % 4 == 0 ? kFaint : kShade);
      }
      continue;
    }
    if (!lit && note % 12 != 0) continue;
    const int labelY = std::clamp(y + rowHeight / 2 - 4, layout.matrix.y, (layout.matrix.y + layout.matrix.height) - 8);
    if (lit) g.Fill(Rect{layout.matrix.x, labelY, label - 2, 8}, kInk);
    if (lit || rowHeight >= 3 || note % 24 == 0) g.Text(layout.matrix.x + 1, labelY + 1, NoteName(note), lit ? kBlack : kFaint, Font::Micro);
  }
  // The hits, in the track's color: on a lit or chosen row cut out of its light and stronger.
  for (int step = 0; step < steps; ++step)
  {
    const bool head = playingHere && seq.currentStep == first + static_cast<std::size_t>(step);
    for (std::size_t slot = 0; slot < counts[static_cast<std::size_t>(step)]; ++slot)
    {
      const auto& note = notes[static_cast<std::size_t>(step)][slot];
      const int row = rowOf(note.note);
      if (row < 0) continue;
      const int y = top + row * rowHeight;
      const int late = note.microstep;
      const int x = x0 + step * kColumn + late * (kColumn - 3) / (synth::kSequencerMicrostepCount - 1);
      // On the beat it fills the step; off it, a sliver where in the step it falls.
      const Rect mark = drums ? Rect{x, y + 2, late == 0 ? kColumn - 1 : 2, rowHeight - 5}
                              : Rect{x, y, late == 0 ? kColumn - 1 : 2, std::max(1, rowHeight - 1)};
      const std::uint8_t held = heldVelocity[note.note & 0x7F];
      const bool strong = held != 0 || row == chosen || seq.steps[static_cast<std::size_t>(step)].held;
      if (strong) g.Fill(Rect{mark.x - 1, mark.y - (drums ? 1 : 0), mark.width + 2, mark.height + (drums ? 2 : 0)}, kBlack);
      if (head) g.Fill(mark, kPlay);
      else if (drums) g.Dither(mark, trackColor, held != 0 ? std::max(8, DrumDensity(note.velocity)) : DrumDensity(note.velocity));
      else g.Dither(mark, trackColor, strong ? 16 : 6 + note.velocity * 10 / 127);
    }
  }
  if (playingHere)
    for (int y = top; y < top + matrixHeight; y += 2)
      g.Pixel(x0 + static_cast<int>(seq.currentStep - first) * kColumn + 2, y, kPlay);
  // No words over an empty pattern here: the matrix is too narrow for them and they covered the names
  // (the piano roll, which has the width, keeps its hint).

  }

  // The GRID: the same 4 x 4 as the keys, for the chosen drum or note.
  const int left = layout.grid.x;
  const int cellWidth = (layout.grid.width - 3 * layout.gapX) / 4;
  const int gridTop = layout.grid.y + layout.nameHeight;
  const int cellHeight = (layout.grid.height - layout.nameHeight - 3 * layout.gapY) / 4;
  const std::string chosenName = picked < 0 ? std::string("PADS: A NOTE") : drums ? std::string(kDrumNames[controller.DrumLane()]) : NoteName(picked);
  if (layout.nameHeight > 0)
    g.TextFit(left, layout.grid.y + 1, layout.grid.width, chosenName, picked < 0 ? kFaint : kInk, picked < 0 ? Font::Micro : Font::Small);
  for (std::size_t index = 0; index < kGridCount; ++index)
  {
    const Rect cell{left + static_cast<int>(index % 4) * (cellWidth + layout.gapX), gridTop + static_cast<int>(index / 4) * (cellHeight + layout.gapY), cellWidth, cellHeight};
    const std::size_t step = first + index;
    if (step >= length)
    {
      g.Pixel(cell.x + cell.width / 2, cell.y + cell.height / 2, kShade);
      continue;
    }
    const auto& info = seq.steps[index];
    std::uint8_t velocity = 0;
    std::uint8_t late = 0;
    for (std::size_t slot = 0; slot < counts[index] && picked >= 0; ++slot)
      if (notes[index][slot].note == picked && velocity == 0)
      {
        velocity = std::max<std::uint8_t>(notes[index][slot].velocity, 1);
        late = notes[index][slot].microstep;
      }
    const bool marked = info.held;  // selected: inverse
    const float flash = info.playing ? fx::Decay(now, animator.StepPlayedAt(index), kStepFlashMs) : 0.0f;
    if (marked) g.Fill(cell, kInk);
    else if (velocity != 0) g.Dither(cell, trackColor, DrumDensity(velocity));
    else if (info.hasEvents) g.Fill(Rect{cell.x, cell.y + cell.height - 2, cell.width, 2}, kDim);
    if (info.playing && !marked)
    {
      g.Dither(Rect{cell.x + 1, cell.y + 1, cell.width - 2, cell.height - 2}, kPlay, std::max(3, static_cast<int>(flash * 16.0f)));
      g.Stroke(cell, kPlay);
    }
    else if (!marked && velocity == 0) g.DottedHLine(cell.x, cell.y + cell.height - 1, cell.width, index % 4 == 0 ? kDim : kFaint, 1);
    // A hit off the beat: a tick along the bottom, where in the step it falls.
    if (velocity != 0 && late != 0)
      g.Fill(Rect{cell.x + (cell.width - 4) * late / (synth::kSequencerMicrostepCount - 1), cell.y + cell.height - 3, 4, 2}, marked ? kBlack : kInk);
    g.Text(cell.x + 2, cell.y + 2, Pad2(step + 1), marked ? kBlack : (velocity != 0 ? kInk : (index % 4 == 0 ? kDim : kFaint)), layout.stepFont);
    if (layout.showNoteNames)
    {
      g.TextFit(cell.x + 7, cell.y + 28, cell.width - 14, info.lowestNote >= 0 ? NoteName(info.lowestNote) : "--",
                marked ? kBlack : info.hasEvents ? kDim : kFaint, Font::Small);
      PaintGridRipple(g, animator, index, cell, kInk);
    }
  }
}

void PaintArpeggiator(Gfx& g, const Controller& controller, Animator& animator, const Rect& area, Font font, int headingHeight)
{
  const auto& state = controller.State();
  const auto& host = controller.HostView();
  const auto track = state.track;
  // The order this track's arpeggiator plays: the held notes (or the root chord) across the
  // range, as bars by pitch; the step playing now is white.
  const auto seq = host.Sequencer().BuildUiState(track);
  const synth::ArpSettings arp = host.Sequencer().GetArpSettings(track);
  if (arp.latch) g.Text(area.x + area.width - Gfx::Measure("HOLD") - 2, area.y + 1, "HOLD", TrackColor(track));
  std::vector<int> held;
  if (seq.arpActive)
    for (std::size_t index = 0; index < seq.arpNoteCount; ++index) held.push_back(seq.arpNotes[index]);
  if (held.empty())
  {
    const KeyboardSettings& keys = state.keyboard;
    const int base = static_cast<int>(synth::kHomePerformanceBaseMidiNote) + keys.root;
    std::array<std::uint8_t, kMaxPadNotes> notes{};
    const std::size_t count = PadNotes(keys, state.chords[track], synth::SynthEngineKind::Juno, base, notes);
    for (std::size_t index = 0; index < count; ++index) held.push_back(notes[index]);
    if (held.size() < 2) held = {base, base + 4, base + 7};
  }
  std::vector<int> pool;
  for (int octave = 0; octave < std::max<int>(1, arp.octaves); ++octave)
    for (const int note : held) pool.push_back(note + octave * 12);
  std::vector<int> order = pool;
  if (arp.mode != synth::ArpMode::Played) std::sort(order.begin(), order.end());
  if (arp.mode == synth::ArpMode::Down) std::reverse(order.begin(), order.end());
  if (arp.mode == synth::ArpMode::UpDown && order.size() > 2)
    for (std::size_t index = order.size() - 2; index > 0; --index) order.push_back(order[index]);
  if (arp.mode == synth::ArpMode::Random)
    for (std::size_t index = 0; index < order.size(); ++index) std::swap(order[index], order[(index * 7 + 3) % order.size()]);
  const int low = *std::min_element(pool.begin(), pool.end());
  const int high = *std::max_element(pool.begin(), pool.end());
  const Rect lane{area.x + 2, area.y + headingHeight, area.width - 4, area.height - headingHeight - 2};
  const int steps = 16;
  const int width = lane.width / steps;
  const bool off = arp.mode == synth::ArpMode::Off;
  g.Text(area.x + 2, area.y, off ? "ARP OFF" : std::string(synth::kArpModeNames[static_cast<std::size_t>(arp.mode)]) + "  " +
                                   synth::kArpRateNames[std::min<std::size_t>(arp.rate, synth::kArpRateCount - 1)] + "  GATE " + std::to_string(arp.gate) + "%",
         off ? kFaint : kDim, font);
  for (int step = 0; step < steps; ++step)
  {
    const int note = order[static_cast<std::size_t>(step) % order.size()];
    const int height = 3 + (note - low) * (lane.height - 4) / std::max(1, high - low);
    const int barWidth = std::max(1, (width - 1) * std::max<int>(10, arp.gate) / 100);
    const bool now = seq.arpActive && static_cast<int>(seq.arpTick % static_cast<std::uint32_t>(steps)) == step;
    const Rect bar{lane.x + step * width, lane.y + lane.height - height, barWidth, height};
    if (off) g.Dither(bar, kFaint, 6);
    else g.Fill(bar, now ? kInk : TrackColor(track));
  }

    }

void PaintPatternGrid(Gfx& g, const Controller& controller, Animator& animator, const synth::SequencerUiState& seq, const SceneGrid& grid)
{
  const auto& state = controller.State();
  const auto& track = seq.tracks[state.track];
  const auto now = animator.Now();
  for (std::size_t index = 0; index < kGridCount; ++index)
  {
    const Rect cell = grid.cells[index];
    if (index > track.patternCount)
    {
      g.Pixel(cell.x + cell.width / 2, cell.y + cell.height / 2, kShade);
      continue;
    }
    if (index == track.patternCount)
    {
      // The "new" slot breathes to invite a press.
      const float breath = 0.5f + 0.5f * std::sin(static_cast<float>(now) / 380.0f);
      g.DottedHLine(cell.x, cell.y + cell.height - 1, cell.width, kFaint);
      g.Text(cell.x + grid.padding, cell.y + 2, "NEW", breath > 0.5f ? kDim : kFaint, grid.labelFont);
      g.DrawGlyph(cell.x + cell.width - 9, cell.y + 12, Glyph::Plus, breath > 0.5f ? kInk : kDim);
      PaintGridRipple(g, animator, index, cell, kTab);
      continue;
    }
    const bool active = index == track.activePattern;
    const bool queued = index == track.queuedPattern;
    const bool content = seq.patternHasEvents[index];
    Color ink = content ? kInk : kDim;
    if (active)
    {
      g.Fill(cell, kInk);
      ink = kBlack;
      const float landed = fx::Decay(now, animator.PatternChangedAt(state.track), 400);
      if (landed > 0.0f) fx::Ripple(g, cell, 1.0f - landed, kInk);
    }
    else if (content) g.Fill(Rect{cell.x, cell.y + cell.height - 2, cell.width, 2}, kDim);
    else g.DottedHLine(cell.x, cell.y + cell.height - 1, cell.width, kFaint);
    if (queued)
    {
      // Queued: marching ants, plus a bar counting down to the switch at the loop end.
      fx::MarchingAnts(g, Rect{cell.x - 1, cell.y - 1, cell.width + 2, cell.height + 2}, kPlay, now);
      const float remaining = 1.0f - seq.patternProgress;
      g.Fill(Rect{cell.x + 2, cell.y + cell.height - 4, static_cast<int>(remaining * static_cast<float>(cell.width - 4)), 2}, kPlay);
    }
    char label[8];
    std::snprintf(label, sizeof(label), "P%02u", static_cast<unsigned>(index + 1));
    g.Text(cell.x + grid.padding, cell.y + 2, label, ink, grid.valueFont);
    if (active && seq.playing)
    {
      const float beat = fx::Decay(now, animator.BeatAt(), 160);
      g.DrawGlyph(cell.x + cell.width - 8 + static_cast<int>(beat * 2.0f), cell.y + 3, Glyph::Play, kBlack);
    }
    if (queued) g.Text(cell.x + grid.padding, cell.y + grid.detailY, "NEXT", active ? kBlack : kPlay, grid.labelFont);
    else if (active) g.Text(cell.x + grid.padding, cell.y + grid.detailY, std::to_string(track.populatedStepCount) + " STEPS", kBlack, grid.labelFont);
    PaintGridRipple(g, animator, index, cell, active ? kInk : kTab);
  }
}

void PaintSessionGrid(Gfx& g, const Controller& controller, Animator& animator, const SceneGrid& grid)
{
  const UiState& state = controller.State();
  const Host& host = controller.HostView();
  const std::uint32_t now = animator.Now();
  const auto& sequencer = host.Sequencer();
  const auto seq = sequencer.BuildUiState(state.track);
  for (std::size_t index = 0; index < kGridCount; ++index)
  {
    const std::size_t track = index % kGridColumns;
    const std::size_t pattern = state.sessionScroll + index / kGridColumns;
    const Rect cell = grid.cells[index];
    const auto& summary = seq.tracks[track];
    const Color color = TrackColor(track);
    g.Fill(Rect{cell.x - 2, cell.y, 1, cell.height}, color);
    if (pattern >= summary.patternCount)
    {
      if (pattern == summary.patternCount && pattern < synth::kSequencerMaxPatternCount)
      {
        g.DottedHLine(cell.x, cell.y, cell.width, kFaint);
        g.DottedHLine(cell.x, cell.y + cell.height - 1, cell.width, kFaint);
        g.TextAligned(cell, "+", kDim, Font::Small, Align::Center);
      }
      else g.Pixel(cell.x + cell.width / 2, cell.y + cell.height / 2, kShade);
      PaintGridRipple(g, animator, index, cell, kInk);
      continue;
    }
    const bool active = pattern == summary.activePattern;
    const bool playing = active && seq.playing && !summary.songSilent;
    const bool queued = pattern == summary.queuedPattern;
    const bool audible = summary.enabled && !summary.muted && host.TrackAudible(track);
    if (playing) g.Dither(cell, kPlay, 3);
    if (active) g.Stroke(cell, color);
    const std::uint16_t mask = sequencer.PatternStepMask(track, pattern);
    char name[12];
    std::snprintf(name, sizeof(name), "P%02u", static_cast<unsigned>(pattern + 1));
    g.Text(cell.x + grid.padding, cell.y + 3, name, active ? color : (mask != 0 ? kInk : kDim), grid.valueFont);
    if (playing) g.DrawGlyph(cell.x + cell.width - 9, cell.y + 3, Glyph::Play, kPlay);
    else if (queued && (now / 250) % 2 == 0) g.Text(cell.x + cell.width - Gfx::Measure("NXT") - 3, cell.y + 3, "NXT", kFn);
    // Steps as ticks: filled where the pattern has notes, the playhead in white.
    const int left = cell.x + grid.padding;
    const int span = cell.width - 6;
    for (std::size_t step = 0; step < summary.stepCount && step < synth::kSequencerStepCount; ++step)
    {
      const int x = left + static_cast<int>(step) * span / static_cast<int>(synth::kSequencerStepCount);
      const bool on = ((mask >> step) & 1u) != 0;
      const bool head = playing && step == (summary.stepCount <= synth::kSequencerStepCount ? summary.currentStep : summary.currentStep * synth::kSequencerStepCount / summary.stepCount);
      if (head) g.Fill(Rect{x, cell.y + grid.tickY - 2, 2, grid.tickHeight + 2}, kInk);
      else if (on) g.Fill(Rect{x, cell.y + grid.tickY, 2, grid.tickHeight}, audible ? color : kDim);
      else g.Pixel(x, cell.y + grid.tickY + grid.tickHeight - 1, kFaint);
    }
    if (queued) fx::MarchingAnts(g, Rect{cell.x - 1, cell.y - 1, cell.width + 2, cell.height + 2}, kFn, now);
    PaintGridRipple(g, animator, index, cell, kInk);
  }
  // The cursor glides like every focus frame.
  if (state.sessionPattern >= state.sessionScroll && state.sessionPattern < state.sessionScroll + kRows)
  {
    const Rect target = grid.cells[(state.sessionPattern - state.sessionScroll) * kGridColumns + state.sessionTrack];
    const std::uint32_t key = Animator::Key("session-focus");
    const int x = static_cast<int>(std::lround(animator.Smooth(key, static_cast<float>(target.x), 50.0f)));
    const int y = static_cast<int>(std::lround(animator.Smooth(key + 1, static_cast<float>(target.y), 50.0f)));
    fx::MarchingAnts(g, Rect{x - 2, y - 2, target.width + 4, target.height + 4}, kInk, now);
  }
  widgets::ScrollIndicator(g, Rect{grid.area.x + grid.area.width, grid.area.y + 1, 1, grid.area.height - 2}, state.sessionScroll, kRows, synth::kSequencerMaxPatternCount);
}

void PaintSongGrid(Gfx& g, const Controller& controller, Animator& animator, const SceneGrid& grid, bool rowsAreSongRows)
{
  const UiState& state = controller.State();
  const Host& host = controller.HostView();
  const std::uint32_t now = animator.Now();
  const auto& sequencer = host.Sequencer();
  const auto seq = sequencer.BuildUiState(state.track);
  const std::size_t length = seq.songLength;
  for (std::size_t column = 0; column < kGridColumns; ++column)
  {
    const std::size_t row = state.songScroll + column;
    const Rect top = grid.cells[rowsAreSongRows ? column * kGridColumns : column];
    const Rect bottom = grid.cells[rowsAreSongRows ? column * kGridColumns + kGridColumns - 1 : (kRows - 1) * kGridColumns + column];
    const Rect span{top.x, top.y, rowsAreSongRows ? bottom.x + bottom.width - top.x : top.width, rowsAreSongRows ? top.height : bottom.y + bottom.height - top.y};
    if (row > length || row >= synth::kSongMaxRows)
    {
      g.Pixel(span.x + span.width / 2, span.y + span.height / 2, kShade);
      continue;
    }
    if (row == length)
    {
      // The empty column after the song: press to append a row.
      const float breath = 0.5f + 0.5f * std::sin(static_cast<float>(now) / 380.0f);
      g.DottedVLine(span.x, span.y, span.height, kFaint, 2);
      g.Text(span.x + 4, span.y + 2, "ROW", breath > 0.5f ? kDim : kFaint);
      g.DrawGlyph(span.x + span.width / 2 - 2, span.y + span.height / 2 - 3, Glyph::Plus, breath > 0.5f ? kInk : kDim);
      for (std::size_t track = 0; track < kRows; ++track) PaintGridRipple(g, animator, rowsAreSongRows ? column * kGridColumns + track : track * kGridColumns + column, grid.cells[rowsAreSongRows ? column * kGridColumns + track : track * kGridColumns + column], kTab);
      continue;
    }
    const synth::SongRow songRow = sequencer.SongRowAt(row);
    const bool playingRow = seq.songPlaying && row == seq.songRow;
    const bool cued = seq.songCueRow == row;
    if (playingRow)
    {
      // Tape head: a green line sweeps through the row while it plays.
      g.Dither(span, kPlay, 2);
      const int head = span.x + static_cast<int>(seq.songRowProgress * static_cast<float>(span.width - 1));
      g.VLine(head, span.y, span.height, kPlay);
    }
    if (cued) fx::MarchingAnts(g, Rect{span.x - 2, span.y - 2, span.width + 4, span.height + 4}, kFn, now);
    for (std::size_t track = 0; track < kRows; ++track)
    {
      const Rect cell = grid.cells[rowsAreSongRows ? column * kGridColumns + track : track * kGridColumns + column];
      const int pattern = songRow.patterns[track];
      const bool cursor = row == state.songRow && track == state.songTrack;
      Color ink = pattern < 0 ? kFaint : kInk;
      if (cursor)
      {
        g.Fill(cell, kInk);
        ink = kBlack;
      }
      else if (pattern >= 0)
        g.Fill(Rect{cell.x, cell.y + cell.height - 2, cell.width, 2}, playingRow ? kPlay : TrackColor(track));
      else g.DottedHLine(cell.x, cell.y + cell.height - 1, cell.width, kFaint);
      if (track == 0)
      {
        // Row number and length (bars) ride in the first track's cell.
        char label[16];
        std::snprintf(label, sizeof(label), "%02u", static_cast<unsigned>(row + 1));
        g.Text(cell.x + grid.padding, cell.y + 2, label, cursor ? kBlack : (playingRow ? kPlay : kDim));
        std::snprintf(label, sizeof(label), "x%u", static_cast<unsigned>(songRow.repeats));
        const int barsX = cell.x + cell.width - Gfx::Measure(label) - 2;
        // While the cursor edits the row length (BARS lane), its bar count is the highlighted value.
        if (state.songLane == 1 && row == state.songRow)
        {
          g.Fill(Rect{barsX - 2, cell.y + 1, Gfx::Measure(label) + 4, 9}, kFn);
          g.Text(barsX, cell.y + 2, label, kBlack);
        }
        else g.Text(barsX, cell.y + 2, label, cursor ? kBlack : kFn);
      }
      else g.Text(cell.x + grid.padding, cell.y + 2, "T" + std::to_string(track + 1), cursor ? kBlack : TrackColor(track));
      char name[8];
      if (pattern < 0) std::snprintf(name, sizeof(name), "--");
      else std::snprintf(name, sizeof(name), "P%02d", pattern + 1);
      g.Text(cell.x + grid.padding, cell.y + grid.detailY - 2, name, ink, grid.valueFont);
      if (cursor) fx::MarchingAnts(g, Rect{cell.x - 1, cell.y - 1, cell.width + 2, cell.height + 2}, kInk, now);
      PaintGridRipple(g, animator, rowsAreSongRows ? column * kGridColumns + track : track * kGridColumns + column, cell, cursor ? kInk : kTab);
    }
  }
  if (length == 0)
  {
    const Rect hint{grid.area.x + 60, grid.area.y + grid.area.height / 2 - 10, grid.area.width - 60, 20};
    g.TextAligned(hint, rowsAreSongRows ? "TAP A + CELL TO APPEND" : "TAP THE + COLUMN ON THE GRID", (now / 600) % 2 ? kDim : kFaint, Font::Micro, Align::Center);
  }
  // Mode badge, bottom right of the tape.
  const char* mode = seq.songMode ? "SONG" : "LOOP";
  const Rect badge{grid.area.x + grid.area.width - Gfx::Measure(mode) - 4, grid.area.y + grid.area.height - 9, Gfx::Measure(mode) + 4, 9};
  g.Fill(badge, seq.songMode ? kFn : kFaint);
  g.Text(badge.x + 2, badge.y + 1, mode, kBlack);
}

void PaintMasterFxBuses(Gfx& g, const Controller& controller, Animator& animator, const Rect& delayArea, const Rect& reverbArea)
{
  using synth::fx::MasterParam;
  const auto& host = controller.HostView();
  const auto& state = controller.State();
  // Delay: the echo train at the current tempo, each repeat quieter by the feedback.
  const auto seq = host.Sequencer().BuildUiState(state.track);
  const int division = std::clamp(host.MasterFx(static_cast<std::size_t>(MasterParam::DelayTime)), 0, 11);
  const float delayMs = synth::fx::kDelaySixteenths[static_cast<std::size_t>(division)] * 15000.0f / static_cast<float>(std::max<std::uint16_t>(seq.bpm, 30));
  const float feedback = static_cast<float>(host.MasterFx(static_cast<std::size_t>(MasterParam::DelayFeedback))) / 127.0f * 0.95f;
  const float delayReturn = host.FxReturnLevel(synth::fx::kBusDelay);
  char delayLabel[24];
  std::snprintf(delayLabel, sizeof(delayLabel), "%dMS", static_cast<int>(delayMs));
  g.Text(delayArea.x + 1, delayArea.y, delayLabel, kDim);
  const float spanMs = 2000.0f;
  float amplitude = 1.0f;
  for (int repeat = 0; repeat < 24 && amplitude > 0.04f; ++repeat)
  {
    const int x = delayArea.x + 1 + static_cast<int>(static_cast<float>(repeat) * delayMs / spanMs * static_cast<float>(delayArea.width - 2));
    if (x >= delayArea.x + delayArea.width) break;
    const int height = std::max(1, static_cast<int>(amplitude * static_cast<float>(delayArea.height - 9)));
    g.VLine(x, delayArea.y + delayArea.height - height, height, repeat == 0 ? kInk : kTab);
    amplitude *= feedback;
  }
  if (!host.FxBusAvailable(synth::fx::kBusDelay)) g.Text(delayArea.x + delayArea.width - Gfx::Measure("NO MEM"), delayArea.y, "NO MEM", kRec);
  else if (delayReturn > 0.01f) g.Dither(Rect{delayArea.x + delayArea.width - 4, delayArea.y, 3, delayArea.height}, kTab, static_cast<int>(4.0f + delayReturn * 12.0f));

  // Reverb: the decay curve for the current size, its top cut by the damping.
  const float size = static_cast<float>(host.MasterFx(static_cast<std::size_t>(MasterParam::ReverbSize))) / 127.0f;
  const float seconds = 0.3f * std::pow(20.0f, size);
  const float damp = static_cast<float>(host.MasterFx(static_cast<std::size_t>(MasterParam::ReverbDamp))) / 127.0f;
  char reverbLabel[24];
  std::snprintf(reverbLabel, sizeof(reverbLabel), "%.1fS", static_cast<double>(std::min(seconds, 99.0f)));
  g.Text(reverbArea.x + 1, reverbArea.y, reverbLabel, kDim);
  const Rect curve{reverbArea.x + 1, reverbArea.y + 8, reverbArea.width - 6, reverbArea.height - 8};
  int previousY = curve.y;
  for (int column = 0; column < curve.width; ++column)
  {
    const float t = static_cast<float>(column) / static_cast<float>(curve.width) * 6.0f;  // 0..6 s
    const float level = std::max(0.0f, 1.0f - t / std::max(0.1f, seconds));  // -60 dB at the decay time
    const int y = curve.y + curve.height - 1 - static_cast<int>(level * (1.0f - 0.4f * damp) * static_cast<float>(curve.height - 1));
    g.Dither(Rect{curve.x + column, y, 1, curve.y + curve.height - y}, kTab, 4);
    if (column > 0) g.Line(curve.x + column - 1, previousY, curve.x + column, y, kInk);
    previousY = y;
  }
  const float reverbReturn = host.FxReturnLevel(synth::fx::kBusReverb);
  if (!host.FxBusAvailable(synth::fx::kBusReverb)) g.Text(reverbArea.x + reverbArea.width - Gfx::Measure("NO MEM"), reverbArea.y, "NO MEM", kRec);
  else if (reverbReturn > 0.01f) g.Dither(Rect{reverbArea.x + reverbArea.width - 4, reverbArea.y, 3, reverbArea.height}, kTab, static_cast<int>(4.0f + reverbReturn * 12.0f));
}

float GraphParameter(Animator& animator, std::size_t track, const synth::ISynthEngine* engine, const char* id, float fallback)
{
  return animator.Smooth(Animator::Key(id, static_cast<std::uint32_t>(track) * 977u + 13u), EngineValue(engine, id, fallback), 80.0f);
}

void MenuContent(Gfx& g, const Controller& controller, Animator& animator, float t)
{
  // One tile per item, on the GRID key that runs it: the name, what it acts on (or its state), and
  // the FN shortcut in yellow at the top right. Row 3 is the same on every screen.
  const std::uint32_t now = animator.Now();
  const MenuItems menu = controller.Menu();
  for (std::size_t index = 0; index < kGridCount; ++index)
  {
    const std::size_t row = index / kGridColumns;
    const std::size_t column = index % kGridColumns;
    const float tileT = fx::EaseOutBack(fx::Clamp01(t * 1.8f - static_cast<float>(row + column) * 0.09f));
    Rect tile = Cell(row, column);
    tile.y -= static_cast<int>((1.0f - tileT) * 8.0f);
    const MenuItem& item = menu[index];
    if (item.action == MenuAction::None)
    {
      g.Pixel(tile.x + tile.width / 2, tile.y + tile.height / 2, kShade);
      continue;
    }
    const bool usable = item.available;
    // The item just run: solid FN yellow while its key is held and a moment after, so the press is
    // seen on the tile it belongs to (the result is in the footer).
    const auto& press = controller.State().menuPress;
    const bool lit = usable && press.index == index && (press.down || (press.releasedAt != 0 && now - press.releasedAt < kMenuPressMs));
    g.Fill(tile, kBlack);
    if (lit)
    {
      g.Fill(tile, kFn);
      const Color ink = kBlack;
      int keyWidth = 0;
      if (item.key != nullptr)
      {
        keyWidth = Gfx::Measure(item.key) + 3;
        g.Text(tile.x + tile.width - keyWidth, tile.y + 2, item.key, ink);
      }
      g.TextFit(tile.x + 3, tile.y + 2, tile.width - 5 - keyWidth, item.label, ink);
      g.TextFit(tile.x + 3, tile.y + 13, tile.width - 5, item.detail, ink);
      continue;
    }
    if (item.on) g.Fill(tile, kInk);
    else if (usable)
    {
      g.Dither(tile, kFaint, 2);
      g.Stroke(tile, row == kRows - 1 ? kFaint : kDim);
    }
    else
    {
      g.DottedHLine(tile.x, tile.y, tile.width, kShade);
      g.DottedHLine(tile.x, tile.y + tile.height - 1, tile.width, kShade);
    }
    const Color ink = item.on ? kBlack : usable ? kInk : kFaint;
    int keyWidth = 0;
    if (item.key != nullptr)
    {
      keyWidth = Gfx::Measure(item.key) + 3;
      g.Text(tile.x + tile.width - keyWidth, tile.y + 2, item.key, item.on ? kBlack : usable ? kFn : kShade);
    }
    g.TextFit(tile.x + 3, tile.y + 2, tile.width - 5 - keyWidth, item.label, ink);
    g.TextFit(tile.x + 3, tile.y + 13, tile.width - 5, item.detail, item.on ? kBlack : usable ? kDim : kShade);
  }
}

void SceneContent(Gfx& g, const Controller& controller, Animator& animator, Scene scene, std::uint8_t page)
{
  switch (scene)
  {
    case Scene::Instrument: InstrumentScene(g, controller, animator, page); break;
    case Scene::Sequence: SequenceScene(g, controller, animator, page); break;
    case Scene::Tape:
      if (page == kTapeSession) SessionScene(g, controller, animator);
      else if (page == kTapeSong) TapeScene(g, controller, animator);
      else if (page == kTapeMix) MixScene(g, controller, animator);
      else if (page == kTapeSends) SendsScene(g, controller, animator);
      else FxScene(g, controller, animator);
      break;
    case Scene::Setup:
      if (page < kSetupKeys) ProjectScene(g, controller, animator, page);
      else
      {
        // KEYS, CHORD, ARP, SYSTEM: slot pages like INSTR.
        const std::size_t settings = std::min<std::size_t>(page - kSetupKeys, kSettingsPageCount - 1);
        const UiState& state = controller.State();
        DrawSlotPage(g, controller, animator, SettingsPage(settings), state.settingsFocus[settings], page == controller.CurrentPage(),
                     Animator::Key("settings-focus", static_cast<std::uint32_t>(settings)));
      }
      break;
  }
}

}  // namespace groove::render
