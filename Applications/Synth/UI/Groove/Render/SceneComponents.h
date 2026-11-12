#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "Applications/Synth/UI/Groove/Render/GrooveRenderer.h"
#include "Applications/Synth/UI/Groove/Render/Widgets.h"

namespace groove::render {

// Geometry is independent of input. Alternate surfaces register only the
// current view's returned rectangles, including when painting outgoing pages.
struct PianoKey {
  Rect rect{};
  int note = -1;
};
struct PianoGeometry {
  std::array<PianoKey, 32> keys{};
  std::size_t count = 0;
};
PianoGeometry PaintPiano(Gfx& g, const Rect& rect, const Controller& controller, Animator& animator,
                         int baseNote, bool showScale = false, const std::array<bool, 128>* marked = nullptr);
void PaintDrumPad(Gfx& g, const Rect& rect, const Controller& controller, Animator& animator,
                  const synth::ISynthEngine* engine, std::size_t pad, Font font = Font::Micro,
                  int labelY = 3, int kickRoom = 3);

struct PianoRollLayout {
  Rect area{};
  int gutter = 20;
  int strip = 9;
  bool showHint = true;
  // Zero keeps the compact auto-fit roll. A touch surface requests fixed rows
  // and receives hit geometry from the same painter; drums use the kit's rows.
  int pitchRows = 0;
  int lowestNote = 48;
  Font labelFont = Font::Micro;
};
struct PianoRollCell {
  Rect bounds{};
  std::size_t step = 0;
  std::uint8_t note = 0;
};
struct PianoRollGeometry {
  std::array<PianoRollCell, 64> cells{};
  std::size_t count = 0;
  Rect matrix{};
  int columnWidth = 0;
  std::array<PianoKey, 8> keys{};
  std::size_t keyCount = 0;
};
void PaintPianoRoll(Gfx& g, const Controller& controller, Animator& animator,
                    const synth::SequencerUiState& seq, const PianoRollLayout& layout,
                    PianoRollGeometry* geometry = nullptr);

struct StepMachineLayout {
  Rect matrix{};              // row labels and the 16 step columns
  Rect grid{};                // 4 by 4 step cells
  int labelWidth = 20;
  int columnWidth = 5;
  int gapX = 2;
  int gapY = 2;
  int nameHeight = 11;
  Font stepFont = Font::Micro;
  bool showNoteNames = false;
  bool showMatrix = true;
};
void PaintStepMachine(Gfx& g, const Controller& controller, Animator& animator,
                      const synth::SequencerUiState& seq, const StepMachineLayout& layout);

struct SceneGrid {
  std::array<Rect, kGridCount> cells{};
  Rect area{};
  Font labelFont = Font::Micro;
  Font valueFont = Font::Small;
  int padding = 3;
  int detailY = 14;
  int tickY = 15;
  int tickHeight = 5;
  bool dottedOutline = false;
};
void PaintPatternGrid(Gfx& g, const Controller& controller, Animator& animator,
                      const synth::SequencerUiState& seq, const SceneGrid& grid);
void PaintSessionGrid(Gfx& g, const Controller& controller, Animator& animator, const SceneGrid& grid);
void PaintSongGrid(Gfx& g, const Controller& controller, Animator& animator, const SceneGrid& grid,
                   bool rowsAreSongRows = false);
void PaintSendCell(Gfx& g, const Controller& controller, Animator& animator, const Rect& rect,
                   std::size_t index, const widgets::ParamCellLayout& layout = {}, bool retainUnavailableFocus = false);
void PaintMixFader(Gfx& g, const Controller& controller, Animator& animator, const Rect& fader,
                   std::size_t track, bool focused);
void PaintMixBadge(Gfx& g, const Controller& controller, Animator& animator, const Rect& rect,
                   std::size_t track, std::size_t kind, Font font = Font::Micro);
void PaintFocus(Gfx& g, Animator& animator, const Rect& rect, std::uint32_t key, Color color);
void PaintGridRipple(Gfx& g, const Animator& animator, std::size_t index, const Rect& rect, Color color);
void PaintFxChain(Gfx& g, const Controller& controller, Animator& animator, const Rect& area);
void PaintArpeggiator(Gfx& g, const Controller& controller, Animator& animator, const Rect& area,
                      Font font = Font::Micro, int headingHeight = 10);
std::string MasterFxDisplayText(const Host& host, std::size_t param);
void PaintMasterFxCell(Gfx& g, const Controller& controller, Animator& animator, const Rect& rect,
                       std::size_t param, const widgets::ParamCellLayout& layout = {});
void PaintMasterFxBuses(Gfx& g, const Controller& controller, Animator& animator,
                        const Rect& delayArea, const Rect& reverbArea);
struct SystemInfoLayout {
  int rowHeight = 11;
  int labelWidth = 60;
  int padding = 0;
  Font font = Font::Micro;
  bool extended = false;
};
void PaintSystemInfo(Gfx& g, const Controller& controller, Animator& animator,
                      const Rect& panel, const SystemInfoLayout& layout = {});

}  // namespace groove::render
