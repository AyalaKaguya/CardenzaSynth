#include "Applications/Synth/UI/Groove/Render/Animator.h"

#include "Applications/Synth/UI/Groove/Render/Effects.h"
#include "Applications/Synth/UI/Groove/Render/Theme.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace groove {

namespace {


// Direction of a page change: CTRL steps forward and wraps, Fn + CTRL steps back.
int PageDirection(std::size_t from, std::size_t to, std::size_t count)
{
  const int delta = static_cast<int>(to) - static_cast<int>(from);
  if (count > 2 && std::abs(delta) > static_cast<int>(count) / 2) return delta > 0 ? -1 : 1;
  return delta >= 0 ? 1 : -1;
}

// Direction of a scene change: 1 down, -1 up (GrooveRenderer). A tap on ESC walks the ring (INSTR, SEQ,
// TAPE, INSTR ...) and always goes down, TAPE back to INSTR too; a step back round the ring goes up.
// SETUP is above the ring: going there goes up, leaving it (ESC) down.
int SceneDirection(Scene from, Scene to)
{
  if (IsMusicScene(from) && to == NextMusicScene(from)) return 1;
  if (IsMusicScene(to) && from == NextMusicScene(to)) return -1;
  return static_cast<int>(to) > static_cast<int>(from) ? 1 : -1;
}

}  // namespace

std::uint32_t Animator::Key(const char* text, std::uint32_t salt)
{
  std::uint32_t hash = 2166136261u ^ salt;
  for (const char* c = text; c != nullptr && *c != 0; ++c) hash = (hash ^ static_cast<std::uint8_t>(*c)) * 16777619u;
  return hash == 0 ? 1 : hash;
}

bool Animator::TransitionActive() const
{
  return transition_.kind != TransitionKind::None && now_ - transition_.start < transition_.duration;
}

float Animator::Smooth(std::uint32_t key, float target, float timeConstantMs)
{
  SmoothEntry* slot = nullptr;
  SmoothEntry* oldest = &smooth_[0];
  for (auto& entry : smooth_)
  {
    if (entry.used && entry.key == key) { slot = &entry; break; }
    if (!entry.used || entry.time < oldest->time) oldest = &entry;
  }
  if (slot == nullptr)
  {
    *oldest = SmoothEntry{key, target, now_, true};
    return target;
  }
  const float dt = static_cast<float>(now_ - slot->time);
  slot->time = now_;
  // A long gap (page not shown for a while) snaps instead of sweeping from a stale value.
  if (dt > 500.0f) slot->value = target;
  else slot->value += (target - slot->value) * (1.0f - std::exp(-dt / timeConstantMs));
  if (std::fabs(target - slot->value) < 0.002f) slot->value = target;
  return slot->value;
}

#if defined(CARDENZA_NATIVE_UI)
void Animator::NotifyCellPress(std::size_t index, std::uint32_t now)
{
  if (index < gridPressedAt_.size()) gridPressedAt_[index] = std::max(gridPressedAt_[index], now == 0 ? 1 : now);
  NotifyInput(now);
}

void Animator::NotifyPerformanceNote(std::size_t track, const std::uint8_t* notes, std::size_t count,
                                     std::uint8_t velocity, std::uint32_t now)
{
  NotifyInput(now);
  if (track != track_ || notes == nullptr || count == 0) return;
  const auto at = now == 0 ? 1 : now;
  for (std::size_t index = 0; index < count; ++index)
  {
    const auto note = notes[index];
    if (note >= noteHitAt_.size()) continue;
    noteHitAt_[note] = std::max(noteHitAt_[note], at);
    if (nativeDrumTrack_)
      for (std::size_t lane = 0; lane < kDrumNotes.size(); ++lane)
        if (kDrumNotes[lane] == note && at >= drumHits_[lane].at)
          drumHits_[lane] = DrumHit{at, static_cast<std::uint8_t>(DrumRowForVelocity(velocity)), velocity};
  }
  lastNoteAt_ = std::max(lastNoteAt_, at);
}

void Animator::NotifyOverlay(std::size_t index, bool active, std::uint32_t now)
{
  if (index >= overlay_.size()) return;
  const auto at = now == 0 ? 1 : now;
  if (active) overlayAt_[index] = at;
  else overlayClosedAt_[index] = at;
  NotifyInput(now);
}

void Animator::NotifyInput(std::uint32_t now)
{
  if (now != 0) lastInputAt_ = std::max(lastInputAt_, now);
}

void Animator::ConsumeNativePresentation(const Controller& controller)
{
  const auto& events = controller.NativePresentation();
  const auto& state = controller.State();
  const auto modal = static_cast<std::uint8_t>(controller.TopModal() == nullptr ? ModalKind::None : controller.TopModal()->kind);
  std::uint8_t overlayMask = 0;
  for (std::size_t index = 0; index < events.overlays.size(); ++index)
    if (events.overlays[index]) overlayMask |= static_cast<std::uint8_t>(1u << index);
  // The bounded ring intentionally keeps the latest events after a long UI
  // stall. Each note event retains its whole chord, including a one-tick tap.
  const auto available = std::min<std::uint64_t>(events.serial - std::min(events.serial, nativePresentationSerial_), events.history.size());
  const auto first = events.serial - available;
  for (auto serial = first + 1; serial <= events.serial; ++serial)
  {
    const auto& event = events.history[(serial - 1) % events.history.size()];
    if (event.serial != serial) continue;
    switch (event.kind)
    {
      case NativePresentationKind::CellPress:
        // A press on an outgoing page or modal must not flash a cell in the
        // new view if navigation completed before this animation update.
        if (event.scene == state.scene && event.page == controller.CurrentPage() && event.track == state.track &&
            event.modal == modal && event.overlayMask == overlayMask) NotifyCellPress(event.index, event.at);
        break;
      case NativePresentationKind::PerformanceNote:
        NotifyPerformanceNote(event.track, event.notes.data(), event.count, event.velocity, event.at);
        break;
      case NativePresentationKind::Overlay:
        NotifyOverlay(event.index, event.active, event.at);
        break;
    }
  }
  nativePresentationSerial_ = events.serial;
  NotifyInput(events.lastInputAt);
  // Physical overlays and touch maps can coexist as presentation sources.
  const std::array<bool, 4> physical{{controller.EscOverlay(), controller.TabOverlay(), controller.FnOverlay(), controller.CtrlOverlay()}};
  for (std::size_t index = 0; index < overlay_.size(); ++index) overlay_[index] = physical[index] || events.overlays[index];
}
#endif

void Animator::Update(const Controller& controller, std::uint32_t now)
{
  const UiState& state = controller.State();
  const Host& host = controller.HostView();
  const auto& sequencer = host.Sequencer();
  const auto seq = sequencer.BuildUiState(state.track);
  const float dt = lastFrame_ == 0 ? 16.0f : static_cast<float>(std::min<std::uint32_t>(now - lastFrame_, 100));
  lastFrame_ = now;
  now_ = now;
#if defined(CARDENZA_NATIVE_UI)
  nativeDrumTrack_ = host.EngineKind(state.track) == synth::SynthEngineKind::Drums;
#endif

  if (!primed_)
  {
    primed_ = true;
    bootAt_ = now;
    scene_ = state.scene;
    pages_ = state.page;
    track_ = state.track;
    projectName_ = state.projectName;
    padNotes_ = state.padNote;
    for (std::size_t track = 0; track < kTrackCount; ++track)
    {
      if (const auto* engine = host.Engine(track)) presets_[track] = engine->CurrentPresetName();
      patterns_[track] = seq.tracks[track].activePattern;
    }
    fingerprintTrack_ = state.track;
    fingerprintPattern_ = seq.activePattern;
    for (std::size_t step = 0; step < kGridCount; ++step) fingerprints_[step] = seq.steps[step].contentFingerprint;
#if defined(CARDENZA_NATIVE_UI)
    ConsumeNativePresentation(controller);
#endif
    return;
  }

  // Scene and page transitions.
  const bool trackChanged = state.track != track_;
  if (state.scene != scene_)
  {
    transition_ = Transition{TransitionKind::Scene, scene_, pages_[static_cast<std::size_t>(scene_)], SceneDirection(scene_, state.scene),
                             now, kSceneSlideMs};
  }
  else if (!trackChanged && state.page[static_cast<std::size_t>(state.scene)] != pages_[static_cast<std::size_t>(state.scene)] &&
           controller.TopModal() == nullptr)
  {
    const std::size_t from = pages_[static_cast<std::size_t>(state.scene)];
    const std::size_t to = state.page[static_cast<std::size_t>(state.scene)];
    transition_ = Transition{TransitionKind::Page, state.scene, static_cast<std::uint8_t>(from),
                             PageDirection(from, to, controller.PageCount(state.scene)), now, kPageSlideMs};
  }
  scene_ = state.scene;
  pages_ = state.page;
  if (trackChanged)
  {
    trackChangedAt_ = now;
    track_ = state.track;
  }

  // Modals and hold overlays.
  const Modal* modal = controller.TopModal();
  const std::size_t depth = state.modals.size();
  const ModalKind kind = modal != nullptr ? modal->kind : ModalKind::None;
  if (depth > modalDepth_ || (depth > 0 && kind != modalKind_)) modalOpenedAt_ = now;
  // The closing modal is no longer in the controller, so keep a copy of the top one whenever it
  // changes; when it goes away, that copy folds shut.
  if ((depth < modalDepth_ || (depth == modalDepth_ && kind != modalKind_)) && lastModalValid_)
  {
    closingModal_ = lastModal_;
    closingModalAt_ = now;
  }
  if (modal != nullptr)
  {
    const std::uint32_t signature = static_cast<std::uint32_t>(depth) ^ static_cast<std::uint32_t>(modal->kind) << 4 ^
                                    static_cast<std::uint32_t>(modal->cursor) << 8 ^ static_cast<std::uint32_t>(modal->text.size()) << 16 ^
                                    static_cast<std::uint32_t>(modal->search.size()) << 21 ^ static_cast<std::uint32_t>(modal->items.size()) << 24 ^
                                    static_cast<std::uint32_t>(modal->trimMarker) << 31;
    if (!lastModalValid_ || signature != lastModalSignature_ || depth != modalDepth_)
    {
      // What the fold needs: the rows on screen, not a copy of a long list.
      static_cast<ModalFields&>(lastModal_) = *modal;
      lastModal_.itemCount = modal->Count();
      lastModal_.firstItem = ListFirstRow(modal->cursor);
      lastModal_.items.clear();
      for (std::size_t index = lastModal_.firstItem; index < lastModal_.firstItem + kListVisibleRows; ++index)
      {
        if (const ListItem* row = modal->Row(index)) lastModal_.items.push_back(*row);
      }
      lastModalSignature_ = signature;
      lastModalValid_ = true;
    }
  }
  else lastModalValid_ = false;
  if (modal != nullptr && modal->kind == ModalKind::TextEntry)
  {
    if (modal->text.size() > textLength_) textTypedAt_ = now;
    textLength_ = modal->text.size();
  }
  else textLength_ = 0;
  modalDepth_ = depth;
  modalKind_ = kind;
  std::array<bool, 4> overlay{{controller.EscOverlay(), controller.TabOverlay(), controller.FnOverlay(), controller.CtrlOverlay()}};
#if defined(CARDENZA_NATIVE_UI)
  for (std::size_t index = 0; index < overlay.size(); ++index) overlay[index] = overlay[index] || controller.NativePresentation().overlays[index];
#endif
  for (std::size_t index = 0; index < overlay.size(); ++index)
  {
    if (overlay[index] && !overlay_[index]) overlayAt_[index] = now;
    if (!overlay[index] && overlay_[index]) overlayClosedAt_[index] = now;
    overlay_[index] = overlay[index];
  }
  if (!state.toast.text.empty() && state.toast.text != toast_) toastAt_ = now;
  if (state.toast.text.empty() && !toast_.empty())
  {
    endedToast_ = toast_;
    endedToastKind_ = toastKind_;
    toastEndedAt_ = now;
  }
  toast_ = state.toast.text;
  toastKind_ = state.toast.kind;
  if (state.projectName != projectName_) projectChangedAt_ = now;
  projectName_ = state.projectName;

  // Keys and notes.
  for (std::size_t index = 0; index < kGridCount; ++index)
  {
    if (state.gridDown[index] && !gridDown_[index]) gridPressedAt_[index] = now;
    gridDown_[index] = state.gridDown[index];
  }
  for (std::size_t index = 0; index < state.padNote.size(); ++index)
  {
    const std::uint8_t note = state.padNote[index];
    if (note != kNoNote && note != padNotes_[index] && note < 128)
    {
      // The key and every note of its chord.
      noteHitAt_[note] = now;
      for (std::size_t voice = 0; voice < state.padChordCount[index]; ++voice) noteHitAt_[state.padChord[index][voice] & 0x7F] = now;
      lastNoteAt_ = now;
    }
  }
  padNotes_ = state.padNote;
  // From the controller, not from the pads being down: a tap can be over within a frame.
  for (std::size_t lane = 0; lane < drumHits_.size(); ++lane)
    if (state.drumPadHits[lane].at > drumHits_[lane].at) drumHits_[lane] = state.drumPadHits[lane];

  // Sequencer: playhead flash, beats, content stamps, pattern switches.
  if (seq.playing != playing_) playToggledAt_ = now;
  if (!seq.playing && playing_)
  {
    stopAt_ = now;
    stopProgress_ = lastProgress_;
    stopStep_ = step_ % kGridCount;
  }
  playing_ = seq.playing;
  if (seq.playing) lastProgress_ = seq.patternProgress;
  if (seq.playing && seq.currentStep != step_)
  {
    if (seq.currentStep % 4 == 0)
    {
      beatAt_ = now;
      downbeat_ = seq.currentStep == 0;
    }
    // The notes the sequencer plays on this track light the pads (and the on-screen keyboards).
    std::array<std::uint8_t, 8> notes{};
    const std::size_t count = sequencer.StepNotes(seq.currentStep, notes);
    for (std::size_t index = 0; index < count; ++index) sequencedAt_[notes[index] & 0x7F] = now;
    if (host.EngineKind(state.track) == synth::SynthEngineKind::Drums)
    {
      std::array<synth::SynthSequencer::StepNoteView, 8> hits{};
      const std::size_t hitCount = sequencer.StepNoteEvents(state.track, seq.currentStep, hits);
      for (std::size_t slot = 0; slot < hitCount; ++slot)
        for (std::size_t lane = 0; lane < kDrumNotes.size(); ++lane)
          if (kDrumNotes[lane] == hits[slot].note)
            drumHits_[lane] = DrumHit{now == 0 ? 1 : now, static_cast<std::uint8_t>(DrumRowForVelocity(hits[slot].velocity)), hits[slot].velocity};
    }
  }
  if (seq.playing) step_ = seq.currentStep;
  // Every arpeggiator note glows on its pad (and on the on-screen keyboards) when it is on this track.
  if (seq.arpTick != arpTick_)
  {
    arpTick_ = seq.arpTick;
    if (seq.arpLastNote >= 0 && seq.arpTrack == state.track) sequencedAt_[static_cast<std::size_t>(seq.arpLastNote) & 0x7F] = now;
  }
  // Flipping the page shows other steps: nothing changed.
  const bool samePage = fingerprintPage_ == seq.viewPage;
  const bool samePattern = fingerprintTrack_ == state.track && fingerprintPattern_ == seq.activePattern && samePage;
  for (std::size_t step = 0; step < kGridCount; ++step)
  {
    const auto& cell = seq.steps[step];
    if (cell.playing && !stepPlaying_[step]) stepPlayedAt_[step] = now;
    stepPlaying_[step] = cell.playing;
    if (!cell.held && stepHeld_[step] && !trackChanged && samePage) deselectedAt_[step] = now;
    stepHeld_[step] = cell.held;
    if (samePattern && cell.contentFingerprint != fingerprints_[step]) stepChangedAt_[step] = now;
    fingerprints_[step] = cell.contentFingerprint;
  }
  fingerprintTrack_ = state.track;
  fingerprintPattern_ = seq.activePattern;
  fingerprintPage_ = seq.viewPage;
  for (std::size_t track = 0; track < kTrackCount; ++track)
  {
    if (seq.tracks[track].activePattern != patterns_[track]) patternChangedAt_[track] = now;
    patterns_[track] = seq.tracks[track].activePattern;
    if (const auto* engine = host.Engine(track))
    {
      std::string preset = engine->CurrentPresetName();
      if (preset != presets_[track])
      {
        presetChangedAt_[track] = now;
        presets_[track] = std::move(preset);
      }
    }
  }

  // INSTR and the SETUP settings pages: flash a slot whose value changed (keyed per track and page so
  // switching does not flash).
  const InstrumentPage* settings = modal == nullptr ? controller.SettingsPageShown() : nullptr;
  if ((state.scene == Scene::Instrument && modal == nullptr) || settings != nullptr)
  {
    const InstrumentPage* page = settings != nullptr ? settings : controller.CurrentInstrumentPage();
    const std::uint32_t pageKey = settings != nullptr ? 0x5E700000u + controller.CurrentPage()
                                                     : static_cast<std::uint32_t>(state.track * 64 + controller.CurrentPage() + 1) ^
                                                           static_cast<std::uint32_t>(host.EngineKind(state.track)) << 16;
    for (std::size_t slot = 0; slot < kGridCount; ++slot)
    {
      synth::SynthParameterInfo info;
      std::string text;
      const int value = controller.SlotValue(page->slots[slot], info, text) ? info.value : 0;
      if (pageKey == slotPageKey_ && value != slotValues_[slot]) valueChangedAt_[slot] = now;
      slotValues_[slot] = value;
    }
    slotPageKey_ = pageKey;
  }
  else slotPageKey_ = 0;

  // Mixer toggles and meters with a falling peak hold.
  for (std::size_t track = 0; track < kTrackCount; ++track)
  {
    const std::array<bool, 3> flags{{host.TrackMuted(track), host.TrackSoloed(track), seq.tracks[track].enabled}};
    for (std::size_t kindIndex = 0; kindIndex < flags.size(); ++kindIndex)
    {
      if (flags[kindIndex] != mixFlags_[track * 3 + kindIndex]) mixToggledAt_[track * 3 + kindIndex] = now;
      mixFlags_[track * 3 + kindIndex] = flags[kindIndex];
    }
    const float activity = host.TrackActivity(track);
    meterPeak_[track] = std::max(activity, meterPeak_[track] - dt * 0.0012f);
  }
  outputPeak_ = std::max(host.OutputPeak(), outputPeak_ - dt * 0.0008f);
#if defined(CARDENZA_NATIVE_UI)
  ConsumeNativePresentation(controller);
#endif
}

void Animator::DecorateLeds(const Controller& controller, const LedFrame& frame, std::array<std::uint8_t, kKeyCount>& levels) const
{
  // Begin() publishes LEDs before the first Tick() starts the boot timeline. Keep that frame dark
  // instead of briefly showing the working keyboard.
  if (!primed_)
  {
    levels.fill(0);
    return;
  }
  const UiState& state = controller.State();
  const Host& host = controller.HostView();
  const std::uint32_t now = now_;
  auto raise = [&](std::size_t index, float level) {
    const int value = std::clamp(static_cast<int>(std::lround(level)), 0, 15);
    if (value > levels[index]) levels[index] = static_cast<std::uint8_t>(value);
  };

  // Blink = waiting: on and off. The guide's hints the same at half the speed. Steady keys keep their level.
  const bool blinkOn = (now / 250) % 2 == 0;
  const bool hintOn = (now / 500) % 2 == 0;
  for (std::size_t index = 0; index < kKeyCount; ++index)
  {
    const LedCell& cell = frame[index];
    const bool off = (cell.effect == LedEffect::Blink && !blinkOn) || (cell.effect == LedEffect::Hint && !hintOn);
    levels[index] = off ? 0 : cell.level;
  }

  // Boot: two light waves sweep the keyboard, then the working lights fade in.
  const std::uint32_t sinceBoot = now - bootAt_;
  if (primed_ && sinceBoot < kBootMs)
  {
    const float t = static_cast<float>(sinceBoot) / static_cast<float>(kBootMs);
    const float fadeIn = fx::Clamp01((t - 0.8f) / 0.2f);
    for (std::size_t row = 0; row < kRows; ++row)
      for (std::size_t column = 0; column < kColumns; ++column)
      {
        const std::size_t index = KeyIndex(row, column);
        float wave = 0.0f;
        for (int pass = 0; pass < 2; ++pass)
        {
          const float front = (t * 1.8f - static_cast<float>(pass) * 0.45f) * 18.0f - 2.0f + static_cast<float>(row) * 0.6f;
          const float distance = static_cast<float>(column) - front;
          wave = std::max(wave, 15.0f * std::exp(-distance * distance / 3.0f));
        }
        levels[index] = static_cast<std::uint8_t>(std::max(wave, static_cast<float>(levels[index]) * fadeIn));
      }
    return;
  }

  // Panic: everything flares and dies away.
  const float panic = fx::Decay(now, state.panicAt, kPanicMs);
  if (panic > 0.0f)
    for (std::size_t index = 0; index < kKeyCount; ++index) levels[index] = static_cast<std::uint8_t>(std::lround(15.0f * panic * panic));

  // Session replaced: a bright row wipe top to bottom.
  const float session = fx::Progress(now, state.sessionAt, kSessionSweepMs);
  if (state.sessionAt != 0 && session < 1.0f)
  {
    const float front = session * 5.0f - 0.5f;
    for (std::size_t row = 0; row < kRows; ++row)
    {
      const float level = 15.0f * std::max(0.0f, 1.0f - std::fabs(static_cast<float>(row) - front) / 1.2f);
      for (std::size_t column = 0; column < kColumns; ++column) raise(KeyIndex(row, column), level);
    }
  }

  // Screen changes introduce the GRID block (it mirrors the 4x4 on screen): one sweep corner to
  // corner. The pads keep playing the same keyboard, so they stay as they are.
  if (TransitionActive())
  {
    const float p = fx::Progress(now, transition_.start, transition_.duration);
    const float gridFront = p * 9.0f - 1.0f;
    for (std::size_t row = 0; row < kRows; ++row)
      for (std::size_t column = 0; column < kGridColumns; ++column)
      {
        const float diagonal = transition_.direction > 0 ? static_cast<float>(row + column) : static_cast<float>(row + kGridColumns - 1 - column);
        const float distance = diagonal - gridFront;
        if (distance <= 0.0f && distance > -2.5f) raise(KeyIndex(row, kGridFirstColumn + column), 14.0f * (1.0f + distance / 2.5f));
      }
  }

  // Track change: the left column says which track is now active, top key = T1 ... bottom = T4.
  const float trackShown = fx::Decay(now, trackChangedAt_, 800);
  if (trackShown > 0.0f)
    for (std::size_t row = 0; row < kRows; ++row)
      levels[KeyIndex(row, 0)] = row == state.track ? 15 : static_cast<std::uint8_t>(static_cast<float>(levels[KeyIndex(row, 0)]) * (1.0f - trackShown));

  // Edge sweep: while nobody plays, a light now and then runs down both edge columns, row by row,
  // so the strips on screen (which mirror these keys) read as the keys beside them.
  std::uint32_t lastPress = 0;
  for (const std::uint32_t pressed : state.keyPressedAt) lastPress = std::max(lastPress, pressed);
#if defined(CARDENZA_NATIVE_UI)
  lastPress = std::max(lastPress, lastInputAt_);
#endif
  bool leftHeld = false;
  for (const auto& key : state.left) leftHeld = leftHeld || key.down;
#if defined(CARDENZA_NATIVE_UI)
  for (const bool overlay : overlay_) leftHeld = leftHeld || overlay;
#endif
  if (primed_ && now - bootAt_ >= kBootMs && !leftHeld && now - lastPress >= kEdgeSweepIdleMs)
  {
    const std::uint32_t phase = (now - bootAt_) % kEdgeSweepPeriodMs;
    for (std::size_t row = 0; row < kRows; ++row)
    {
      const std::uint32_t start = static_cast<std::uint32_t>(row) * 90u;
      if (phase < start || phase - start >= 260u) continue;
      const float glow = 15.0f * (1.0f - static_cast<float>(phase - start) / 260.0f);
      raise(KeyIndex(row, 0), glow);
      raise(KeyIndex(row, kColumns - 1), glow);
    }
  }

  // Hold maps: the GRID keys they accept come on corner to corner, mirroring the tiles on screen.
  for (std::size_t map = 0; map < overlay_.size(); ++map)
  {
    if (!overlay_[map]) continue;
    const std::uint32_t age = now - overlayAt_[map];
    for (std::size_t row = 0; row < kRows; ++row)
      for (std::size_t column = 0; column < kGridColumns; ++column)
        if (age < static_cast<std::uint32_t>(row + column) * 30u) levels[KeyIndex(row, kGridFirstColumn + column)] = 0;
  }

  const Modal* modal = controller.TopModal();
  const bool steps = state.scene == Scene::Sequence && controller.CurrentPage() == 0 && modal == nullptr;
  if (steps)
  {
    const float rewind = fx::Progress(now, stopAt_, kRewindMs);
    for (std::size_t step = 0; step < kGridCount; ++step)
    {
      const std::size_t index = KeyIndex(step / kGridColumns, kGridFirstColumn + step % kGridColumns);
      raise(index, 15.0f * fx::Decay(now, stepChangedAt_[step], 300));
      if (stopAt_ != 0 && rewind < 1.0f)
      {
        const float head = static_cast<float>(stopStep_) * (1.0f - fx::EaseOutCubic(rewind));
        const float distance = static_cast<float>(step) - head;
        if (distance >= -0.5f && step <= stopStep_) raise(index, 14.0f * std::max(0.0f, 1.0f - distance / 3.0f) * (1.0f - rewind));
      }
    }
  }
  if ((state.scene == Scene::Instrument || controller.SettingsPageShown() != nullptr) && modal == nullptr)
    for (std::size_t slot = 0; slot < kGridCount; ++slot)
      raise(KeyIndex(slot / kGridColumns, kGridFirstColumn + slot % kGridColumns), 15.0f * fx::Decay(now, valueChangedAt_[slot], 300));
  if (state.scene == Scene::Sequence && controller.CurrentPage() == 1 && modal == nullptr)
  {
    const auto seq = host.Sequencer().BuildUiState(state.track);
    const std::size_t active = seq.tracks[state.track].activePattern;
    raise(KeyIndex(active / kGridColumns, kGridFirstColumn + active % kGridColumns), 15.0f * fx::Decay(now, patternChangedAt_[state.track], 400));
  }

  // A note the sequencer plays on this track lights its pad full for a moment, like a press. Only
  // pads that are lit at all (keys that can sound), and not while a map or pop-up owns the pads.
  const bool padsPlay = modal == nullptr && !controller.CellPickerActive() && !controller.EscOverlay() && !controller.TabOverlay() && !controller.CtrlOverlay() &&
                        !state.left[2].down;
  const bool drums = host.EngineKind(state.track) == synth::SynthEngineKind::Drums;
  ledStruck_.fill(false);
  if (padsPlay)
    for (std::size_t row = 0; row < kRows; ++row)
      for (std::size_t column = kPadFirstColumn; column < kPadFirstColumn + kPadColumns; ++column)
      {
        int note = 0;
        const std::size_t index = KeyIndex(row, column);
        if (frame[index].level == 0 || !controller.PadNoteFor(row, column, note)) continue;
        if (drums)
        {
          // At rest a drum pad is half, as bright as a melodic pad: half is already the dimmest duty
          // the HAL has, so a resting gradient could only be brighter. A hit runs along its column and
          // strikes, it does not glide in.
          const std::size_t lane = column - kPadFirstColumn;
          // The bar of a held drum grows with the wave: a key waits until it arrives.
          if (levels[index] != kLedHalf && !controller.IsPadDown(row, column) && !DrumWaveArrived(lane, row)) levels[index] = kLedHalf;
          const float wave = DrumWave(lane, row);
          if (wave > 0.0f)
          {
            raise(index, 15.0f * wave);
            ledStruck_[index] = wave > 0.2f;
          }
          continue;
        }
        raise(index, 15.0f * fx::Decay(now, sequencedAt_[static_cast<std::size_t>(note)], 200));
      }

  // Error: the left column blinks twice.
  if (!toast_.empty() && toastKind_ == ToastKind::Error && now - toastAt_ < 360 && ((now - toastAt_) / 90) % 2 == 0)
    for (std::size_t row = 0; row < kRows; ++row) raise(KeyIndex(row, 0), 15.0f);

}

namespace {
constexpr float kDrumWaveRowMs = 28.0f;  // How fast a hit runs along its column.
constexpr float kDrumWaveFadeMs = 180.0f;
}  // namespace

bool Animator::DrumWaveArrived(std::size_t lane, std::size_t row) const
{
  if (lane >= drumHits_.size() || drumHits_[lane].at == 0) return true;
  const DrumHit& hit = drumHits_[lane];
  return static_cast<float>(now_ - hit.at) >= std::fabs(static_cast<float>(row) - static_cast<float>(hit.row)) * kDrumWaveRowMs;
}

float Animator::DrumWave(std::size_t lane, std::size_t row) const
{
  if (lane >= drumHits_.size() || drumHits_[lane].at == 0) return 0.0f;
  const DrumHit& hit = drumHits_[lane];
  const float age = static_cast<float>(now_ - hit.at);
  const float rows = std::fabs(static_cast<float>(row) - static_cast<float>(hit.row));
  const float arrival = rows * kDrumWaveRowMs;
  if (age < arrival || age > 900.0f) return 0.0f;
  const float power = static_cast<float>(hit.velocity) / 127.0f;
  const float reach = 1.0f - rows * (0.54f - 0.44f * power);  // Hard: all the way. Soft: a key or two.
  if (reach <= 0.0f) return 0.0f;
  // Every hit strikes hard where it lands; how hard it was shows in how far it goes.
  return reach * std::exp(-(age - arrival) / kDrumWaveFadeMs);
}

void Animator::SmoothLeds(std::array<std::uint8_t, kKeyCount>& levels)
{
  const float dt = ledFrame_ == 0 ? 1000.0f : static_cast<float>(std::min<std::uint32_t>(now_ - ledFrame_, 200));
  ledFrame_ = now_ == 0 ? 1 : now_;
  const float rise = 1.0f - std::exp(-dt / 22.0f);
  const float fall = 1.0f - std::exp(-dt / 60.0f);
  for (std::size_t index = 0; index < kKeyCount; ++index)
  {
    const float target = levels[index];
    float& shown = ledShown_[index];
    shown += (target - shown) * (target > shown ? (ledStruck_[index] ? 1.0f : rise) : fall);
    if (std::fabs(target - shown) < 0.3f) shown = target;
    levels[index] = static_cast<std::uint8_t>(std::clamp(static_cast<int>(std::lround(shown)), 0, 15));
  }
}

}  // namespace groove
