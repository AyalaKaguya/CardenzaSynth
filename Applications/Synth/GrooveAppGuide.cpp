// The guide link: a lesson page on a computer (wiki/src/guide/device.ts) talks to the firmware in short
// text messages (Device::USB app messages: over USB, or from the page around the web simulator). The
// protocol is listed below; every message starts with G:
//
//   page -> device                          device -> page
//   G?        protocol and guide state      G!1 g<0|1>
//   GE        start the guide               GE1, or GE0 <rec|card|save|mem> (why not)
//   GX        end it                        GX (also when the device ends it by itself)
//   GP        still here (any G message is) -
//   GQ        the state now                 GS<Controller::GuideState()>
//   GL<pairs> lesson start: r1 a fresh      GS... (s scene, p page, t track 1-4, d<n> the song
//             practice project (with the     lessons' song as lessons 1..n made it, f1 a new project's
//             user's keyboard)                keyboard instead of the user's)
//   GH<keys>  the step's keys, blinking: two hex digits per KeyIndex, none clears; a message holds
//             29 keys at most (kAppMessageSize), G+<keys> adds the rest to the last GH / GT
//   GT<keys>  the same, steady: the step is learned and the user tries more keys
//   GK<kk><0|1> press / release a key       GK<kk><0|1> for every key the user presses
//                                           GD<56 hex digits> the key lights as shown (over USB)
//
// While the guide runs, the user's working state waits in the autosave on the card (it was written when
// the guide started and nothing writes the practice project there); it comes back when the guide ends,
// when the page goes quiet, the cable is pulled, or the app is suspended. Power loss loses only the
// practice project. Without a card there is nowhere to keep it: the guide does not start (GE0 card).

#include "Applications/Synth/GrooveApp.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <string>
#include <utility>

#include "Library/Memory/Memory.h"

#include "Devices/Device.h"
#include "OS/Storage/Storage.h"

namespace apps {

namespace {

// The page says something every few seconds; a page that went away ends the guide.
constexpr std::uint32_t kGuideSilenceMs = 30000;
// The state goes out when it changed, at most this often.
constexpr std::uint32_t kGuideStateMs = 50;
#if defined(ESP_PLATFORM)
// Saving the working state and a fresh project need room (as for the autosave).
constexpr std::size_t kGuideHeapBytes = 16 * 1024;
#endif

int HexDigit(char c)
{
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

int HexByte(const char* text)
{
  const int high = HexDigit(text[0]), low = HexDigit(text[1]);
  return high < 0 || low < 0 ? -1 : high * 16 + low;
}

}  // namespace

void GrooveApp::SendGuide(const std::string& text) { (void)Device::USB::WriteAppMessage(text.data(), text.size()); }

void GrooveApp::SendGuideState(std::uint32_t now, bool always)
{
  if (!guide_ || (!always && now - guideStateAt_ < kGuideStateMs)) return;
  std::string state = "GS" + controller_.GuideState();
  if (!always && state == guideState_) return;
  guideStateAt_ = now;
  guideState_ = state;
  SendGuide(state);
}

// The key lights as the device shows them (perceived level 0-15, one hex digit per KeyIndex), so the
// page draws the keyboard beside the lesson as it looks. When they change, at most every 50 ms, and
// only to a page on the cable: the simulator shows its own.
void GrooveApp::SendGuideLeds(std::uint32_t now)
{
  if (!guide_ || !guideOverUsb_ || now - guideLedsAt_ < kGuideStateMs) return;
  std::array<char, 2 + groove::kKeyCount> leds{};
  leds[0] = 'G';
  leds[1] = 'D';
  for (std::size_t index = 0; index < groove::kKeyCount; ++index)
  {
    const std::uint8_t level = Device::LED::Brightness(index / groove::kColumns, index % groove::kColumns);
    leds[2 + index] = "0123456789abcdef"[level > 15 ? 15 : level];
  }
  if (leds == guideLeds_) return;
  guideLedsAt_ = now;
  guideLeds_ = leds;
  (void)Device::USB::WriteAppMessage(leds.data(), leds.size());
}

void GrooveApp::ServiceGuide(std::uint32_t now)
{
  Device::USB::AppMessage message;
  // A few per tick: the page sends little, and a key it presses runs the whole controller.
  for (int count = 0; count < 8 && Device::USB::ReadAppMessage(message); ++count) HandleGuideMessage(message.data, message.length, now);
  if (!guide_) return;
  // Signed: the message that started the guide may carry a later time than this tick's.
  if ((guideOverUsb_ && !Device::USB::Connected()) || static_cast<std::int32_t>(now - guideHeardAt_) > static_cast<std::int32_t>(kGuideSilenceMs))
  {
    std::printf("Groove: guide ended (%s)\n", guideOverUsb_ && !Device::USB::Connected() ? "unplugged" : "page quiet");
    ExitGuide();
    SendGuide("GX");
    return;
  }
  SendGuideState(now, false);
  SendGuideLeds(now);
}

void GrooveApp::HandleGuideMessage(const char* text, std::size_t length, std::uint32_t now)
{
  if (length < 2 || text[0] != 'G') return;
  guideHeardAt_ = now;
  switch (text[1])
  {
    case '?':
      SendGuide(guide_ ? "G!1 g1" : "G!1 g0");
      return;
    case 'E':
    {
      std::string reason;
      if (!guide_ && !EnterGuide(reason))
      {
        SendGuide("GE0 " + reason);
        return;
      }
      SendGuide("GE1");
      SendGuideState(now, true);
      return;
    }
    case 'X':
      ExitGuide();
      SendGuide("GX");
      return;
    case 'Q':
      SendGuideState(now, true);
      return;
    default:
      break;
  }
  if (!guide_) return;
  switch (text[1])
  {
    case 'L':
      GuideSetup(text + 2, length - 2);
      SendGuideState(now, true);
      return;
    case 'H':
    case 'T':
    case '+':
    {
      std::uint64_t keys = 0;
      for (std::size_t at = 2; at + 1 < length; at += 2)
      {
        const int index = HexByte(text + at);
        if (index >= 0 && static_cast<std::size_t>(index) < groove::kKeyCount) keys |= std::uint64_t{1} << index;
      }
      // All 32 pads do not fit one message: G+ carries the ones after the first 29.
      if (text[1] == '+') controller_.SetGuideHint(controller_.GuideHint() | keys, controller_.GuideHintWaits());
      else controller_.SetGuideHint(keys, text[1] == 'H');
      return;
    }
    case 'K':
    {
      const int index = length >= 5 ? HexByte(text + 2) : -1;
      if (index < 0 || static_cast<std::size_t>(index) >= groove::kKeyCount) return;
      const bool pressed = text[4] == '1';
      const std::uint64_t bit = std::uint64_t{1} << index;
      // A key is pressed or released once, as on the keyboard.
      if (pressed == ((guideInjectedDown_ & bit) != 0)) return;
      guideInjectedDown_ = pressed ? guideInjectedDown_ | bit : guideInjectedDown_ & ~bit;
      guideInjecting_ = true;
      HandleMatrixKeyEvent(static_cast<std::size_t>(index) / groove::kColumns, static_cast<std::size_t>(index) % groove::kColumns, pressed);
      guideInjecting_ = false;
      return;
    }
    default:
      return;
  }
}

bool GrooveApp::EnterGuide(std::string& reason)
{
  if (sampler_.RecordingTrack() >= 0)
  {
    reason = "rec";
    return false;
  }
#if defined(ESP_PLATFORM)
  if (memory::FastFree() < kGuideHeapBytes)
  {
    reason = "mem";
    return false;
  }
#endif
  // The working state goes to the card first: the guide brings it back from there.
  if (!SaveSessionState())
  {
    reason = os::storage::CardMounted() ? "save" : "card";
    return false;
  }
  sessionChanged_ = false;
  const auto& view = controller_.State();
  guideScene_ = view.scene;
  guidePage_ = controller_.CurrentPage();
  guideSongMode_ = sequencer_.SongMode();
  // The user's keyboard (layout, scale, octave, ...) is a setting of theirs: the lessons play with it.
  guideKeyboard_ = controller_.Settings().keyboard;
  controller_.GuideKeepView();
  if (!FreshGuideProject())
  {
    // Nothing was replaced: the working state is still here.
    reason = "mem";
    return false;
  }
  guide_ = true;
  guideOverUsb_ = Device::USB::Connected();
  guideState_.clear();
  guideLeds_ = {};
  guideInjectedDown_ = 0;
  controller_.SetGuide(true);
  std::printf("Groove: guide started\n");
  return true;
}

// An empty practice project on INSTR VOICE, T1, as after power-on.
// A new practice project, with the user's keyboard, or a new project's (newKeyboard: a lesson that
// changes the keyboard, or names the notes of keys).
bool GrooveApp::FreshGuideProject(bool newKeyboard)
{
  sequencer_.Stop();
  sequencer_.SetSongMode(false);
  ReleaseComputerNotes();
  Panic();
  groove::ProjectUiSettings ui;
  std::string message;
  if (!NewProject(ui, message)) return false;
  if (!newKeyboard) ui.keyboard = guideKeyboard_;
  controller_.Restore(ui, "");
  controller_.GuideFreshView();
  controller_.GuideGoTo(groove::Scene::Instrument, 0, synth::kDefaultActiveTrack);
  return true;
}

void GrooveApp::ExitGuide()
{
  if (!guide_) return;
  // Keys the page still holds let go first.
  guideInjecting_ = true;
  for (std::size_t index = 0; index < groove::kKeyCount; ++index)
    if ((guideInjectedDown_ >> index) & 1u) HandleMatrixKeyEvent(index / groove::kColumns, index % groove::kColumns, false);
  guideInjecting_ = false;
  guideInjectedDown_ = 0;
  guide_ = false;
  controller_.SetGuide(false);
  sequencer_.Stop();
  ReleaseComputerNotes();
  Panic();
  if (RestoreSessionState())
  {
    sequencer_.SetSongMode(guideSongMode_);
    controller_.GuideRestoreView();
    controller_.GuideGoTo(guideScene_, guidePage_, controller_.State().track);
  }
  else
  {
    // No working state came back (it was saved when the guide started, so this is a failed read).
    groove::ProjectUiSettings ui;
    std::string message;
    if (NewProject(ui, message)) controller_.Restore(ui, "");
    std::printf("Groove: guide ended, the working state did not come back\n");
  }
  sessionChanged_ = false;
  std::printf("Groove: guide ended\n");
}

void GrooveApp::GuideSetup(const char* fields, std::size_t length)
{
  int scene = -1, page = -1, track = -1, song = 0;
  bool fresh = false, newKeyboard = false;
  for (std::size_t at = 0; at < length;)
  {
    const char field = fields[at++];
    int value = 0;
    bool any = false;
    while (at < length && fields[at] >= '0' && fields[at] <= '9')
    {
      value = value * 10 + (fields[at++] - '0');
      any = true;
    }
    if (!any) continue;
    if (field == 's' && value < static_cast<int>(groove::kSceneCount)) scene = value;
    if (field == 'p') page = value;
    if (field == 't' && value >= 1 && value <= static_cast<int>(synth::kSynthTrackCount)) track = value - 1;
    if (field == 'r') fresh = value != 0;
    if (field == 'd') song = value;
    if (field == 'f') newKeyboard = value != 0;
  }
  if ((fresh || song > 0) && !FreshGuideProject(newKeyboard)) return;
  if (song > 0) FillGuideSong(song);
  const auto& view = controller_.State();
  if (scene < 0 && page < 0 && track < 0) return;
  controller_.GuideGoTo(scene >= 0 ? static_cast<groove::Scene>(scene) : view.scene, page,
                        track >= 0 ? static_cast<std::size_t>(track) : view.track);
}

// The song lessons build one song, each from where the one before ended. d<stage> starts a fresh
// project with what lessons 1..stage made, so a lesson opened on its own has the song so far:
//   1  T1 P01: kick on 1 5 9 13, snare on 5 13, closed hat on 3 7 11 15 (the drum pads' top row)
//   2  T1 P02: P01 with the hat on 1 5 9 13 too; T1 stays on it, as the lesson leaves it
//   3  T3 P01: Am on 1 4 7, F on 9 12 15 (A3 and F3 with the chord key on: TRIAD in C)
//   4  T2 P01: the tune A4 G4 E4 F4 E4 C4 on 1 4 7 9 12 15
void GrooveApp::FillGuideSong(int stage)
{
  auto write = [&](std::size_t track, std::initializer_list<int> steps, std::initializer_list<std::uint8_t> notes, std::uint8_t velocity) {
    for (const int step : steps)
    {
      // Added to what the step holds, as a second pad on a written step does.
      std::array<synth::SynthSequencer::StepNoteView, 8> held{};
      std::array<std::uint8_t, 8> all{};
      std::size_t count = sequencer_.StepNoteEvents(track, static_cast<std::size_t>(step - 1), held);
      for (std::size_t index = 0; index < count; ++index) all[index] = held[index].note;
      for (const std::uint8_t note : notes)
        if (count < all.size()) all[count++] = note;
      (void)sequencer_.SetStepNotes(track, static_cast<std::size_t>(step - 1), all.data(), count, velocity);
    }
  };
  const std::uint8_t kick = groove::kDrumNotes[0], snare = groove::kDrumNotes[1], hat = groove::kDrumNotes[2];
  const std::uint8_t hard = groove::kDrumRowVelocity[0];
  sequencer_.SetActiveTrack(0);
  write(0, {1, 5, 9, 13}, {kick}, hard);
  write(0, {5, 13}, {snare}, hard);
  write(0, {3, 7, 11, 15}, {hat}, hard);
  if (stage >= 2 && sequencer_.CopyPattern(0, 1) && sequencer_.SelectOrCreatePattern(1)) write(0, {1, 5, 9, 13}, {hat}, hard);
  constexpr std::uint8_t kPlayed = 110;  // the keyboard's velocity in a new project
  if (stage >= 3)
  {
    sequencer_.SetActiveTrack(2);
    write(2, {1, 4, 7}, {57, 60, 64}, kPlayed);
    write(2, {9, 12, 15}, {53, 57, 60}, kPlayed);
  }
  if (stage >= 4)
  {
    sequencer_.SetActiveTrack(1);
    static constexpr std::pair<int, std::uint8_t> kTune[] = {{1, 69}, {4, 67}, {7, 64}, {9, 65}, {12, 64}, {15, 60}};
    for (const auto& [step, note] : kTune) write(1, {step}, {note}, kPlayed);
  }
  sequencer_.SetActiveTrack(controller_.State().track);
}

}  // namespace apps
