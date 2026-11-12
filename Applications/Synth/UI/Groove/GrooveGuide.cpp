// The guide's side of the controller: the hint lights (ComputeLeds), where a lesson starts, and the
// state a lesson page checks its steps against. GrooveAppGuide.cpp carries the messages.

#include "Applications/Synth/UI/Groove/GrooveController.h"

#include <cstdio>
#include <string>

namespace groove {

void Controller::SetGuide(bool on)
{
  guide_ = on;
  if (!on) guideHint_ = 0;
}

void Controller::GuideGoTo(Scene scene, int page, std::size_t track)
{
  state_.modals.clear();
  ReleaseAllPads();
  SelectTrack(track);
  GoToScene(scene);
  if (page >= 0) SetPage(static_cast<std::uint8_t>(page));
}

// Where the device stands on every page: each scene's page, each track's INSTR page and the focused
// cells, the drum or note the pads chose, SEQ's views and the song and session cursors. A lesson's
// practice project starts from those of a new start (where the user went, SETUP KEYS or TAPE MIX, would
// make "tap CTRL: the next page" land somewhere else), and the user's come back when the guide ends.
void Controller::CopyView(UiState& to, const UiState& from)
{
  to.page = from.page;
  to.drumLane = from.drumLane;
  to.drumVelocity = from.drumVelocity;
  to.noteLane = from.noteLane;
  to.instrumentFocus = from.instrumentFocus;
  to.trackInstrumentPage = from.trackInstrumentPage;
  to.trackInstrumentFocus = from.trackInstrumentFocus;
  to.projectFocus = from.projectFocus;
  to.seqTrackFocus = from.seqTrackFocus;
  to.mixFocus = from.mixFocus;
  to.sendsFocus = from.sendsFocus;
  to.fxFocus = from.fxFocus;
  to.settingsFocus = from.settingsFocus;
  to.songRow = from.songRow;
  to.songTrack = from.songTrack;
  to.songScroll = from.songScroll;
  to.songLane = from.songLane;
  to.sessionScroll = from.sessionScroll;
  to.sessionPattern = from.sessionPattern;
  to.sessionTrack = from.sessionTrack;
  to.record.noteView = from.record.noteView;
  to.record.drumView = from.record.drumView;
}

std::string Controller::GuideState() const
{
  const auto& sequencer = host_.Sequencer();
  const std::size_t track = state_.track;
  const std::size_t pattern = sequencer.ActivePattern(track);
  // Steps of the current pattern that hold notes (hits on a drum track).
  std::size_t written = 0;
  std::array<synth::SynthSequencer::StepNoteView, 8> notes{};
  const std::size_t steps = sequencer.PatternStepCount(track, pattern);
  for (std::size_t step = 0; step < steps; ++step)
    if (sequencer.StepNoteEvents(track, step, notes) > 0) ++written;
  const Modal* modal = TopModal();
  const int overlay = EscOverlay() ? 1 : TabOverlay() ? 2 : FnOverlay() ? 3 : CtrlOverlay() ? 4 : 0;
  // The preset as a number that changes with it.
  unsigned preset = 0;
  if (const auto* engine = host_.Engine(track))
    for (const char c : engine->CurrentPresetId()) preset = (preset * 31u + static_cast<unsigned char>(c)) % 1000u;
  // The SONG cell under the cursor: its pattern number (P01 = 1), 0 for -- or past the song; its row
  // (from 1) and how many bars that row plays, 0 past the song.
  unsigned songCell = 0, songRow = 0, songBars = 0;
  if (state_.songRow < sequencer.SongLength() && state_.songTrack < kTrackCount)
  {
    const auto row = sequencer.SongRowAt(state_.songRow);
    songCell = static_cast<unsigned>(row.patterns[state_.songTrack] + 1);
    songRow = static_cast<unsigned>(state_.songRow + 1);
    songBars = row.repeats;
  }
  char text[112];
  std::snprintf(text, sizeof(text), "s%up%ut%ue%uy%um%uo%uh%uq%uw%uk%uz%uc%ua%ug%ul%ux%uj%ub%uv%u", static_cast<unsigned>(state_.scene),  // scene: 0 SETUP .. 3 TAPE
                static_cast<unsigned>(CurrentPage()),                                                    // page of the scene, from 0
                static_cast<unsigned>(track),                                                            // track, from 0
                static_cast<unsigned>(host_.EngineKind(track)),                                          // its engine
                sequencer.Playing() ? 1u : 0u,                                                           // transport
                modal == nullptr ? 0u : modal->kind == ModalKind::Menu ? 1u : 2u,                        // FN menu kept open / another pop-up
                static_cast<unsigned>(overlay),                                                          // hold map: ESC TAB FN CTRL
                static_cast<unsigned>(written),                                                          // steps with notes
                static_cast<unsigned>(sequencer.PatternCount(track)),                                    // patterns of the track
                static_cast<unsigned>(pattern),                                                          // the one it plays
                static_cast<unsigned>(state_.keyboard.scale),                                            // scale (kScales)
                static_cast<unsigned>(state_.keyboard.layout),                                           // keyboard layout
                state_.chords[track].chord != ChordType::Off ? 1u : 0u,                                  // the track's chord
                sequencer.GetArpSettings(track).mode != synth::ArpMode::Off ? 1u : 0u,                   // its arpeggiator
                static_cast<unsigned>(sequencer.SongLength()),                                           // song rows
                sequencer.SongMode() ? 1u : 0u,                                                          // SONG or LOOP
                songCell, songRow, songBars, preset);
  return std::string(text) + "n" + PageTab(state_.scene, CurrentPage());
}

}  // namespace groove
