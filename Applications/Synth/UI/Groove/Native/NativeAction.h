#pragma once

#include <cstdint>

namespace groove::native {

// Semantic input shared by the native view and the controller. Indices refer to
// domain cells/rows, never physical matrix coordinates or display pixels.
enum class Kind : std::uint8_t {
  None,
  SideDown,        // index = groove::KeyRole (ADD/SUB/ENTER/PLAY)
  SideUp,          // index = groove::KeyRole; always ends a repeat/hold
  Cancel,          // cancel the active gesture; does not navigate or commit
  Back,            // context-aware back/cancel, including modal confirmation
  Enter,           // explicitly activate the focused native item
  Context,         // modal second action (sample options or merge a captured take)
  SelectScene,     // index = groove::Scene
  SelectTrack,     // index = track 0..3
  TextKey,        // ASCII or -1 backspace, -2 left, -3 right, -4 clear
  Search,         // open browser search field
  SelectScenePage, // index = scene, value = page
  RollWindow,     // view: index = time delta in steps, value = pitch delta
  RollNote,       // index = absolute step, value = MIDI note; toggle one note
  RollLane,       // cycle NOTE / LEN / VEL / TIME / HITS
  TrackMapCell,    // index = row * 4 + track: select / mute / solo / seq
  SelectPage,      // index = page in the current scene
  FocusCell,       // index = semantic page cell 0..15; focus only
  ActivateCell,    // index = semantic page cell 0..15; explicit action
  SelectListRow,   // index = absolute row in the current list
  RunMenu,         // index = current menu item 0..15
  SelectStep,      // index = absolute step in the current track
  SelectPattern,   // index = pattern slot in the current track
  SelectSongCell,  // index = arrangement row; value = track
  SelectSessionCell, // index = pattern slot; value = track
  FocusMaster,     // focus device master output volume on MIX
  PerformanceDown, // index=contact ID, value=visible MIDI note, velocity=0 uses keyboard setting
  PerformanceUp,   // index=contact ID; release its captured track/chord
  PerformanceCancelAll,
};

struct Command {
  Kind kind = Kind::None;
  int index = 0;
  int value = 0;
  std::uint8_t velocity = 0;
};

}  // namespace groove::native
