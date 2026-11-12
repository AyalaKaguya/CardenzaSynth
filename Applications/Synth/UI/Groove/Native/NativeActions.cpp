#include "Applications/Synth/UI/Groove/GrooveController.h"

#if defined(CARDENZA_NATIVE_UI)

#include <algorithm>
#include <cstdio>

namespace groove {

void Controller::NotifyNativeInput(std::uint32_t nowMs)
{
  nativePresentation_.lastInputAt = nowMs == 0 ? 1 : nowMs;
}

void Controller::RecordNativePresentation(NativePresentationEvent event)
{
  event.serial = ++nativePresentation_.serial;
  if (event.kind != NativePresentationKind::PerformanceNote) event.track = static_cast<std::uint8_t>(state_.track);
  event.scene = state_.scene;
  event.page = CurrentPage();
  event.modal = static_cast<std::uint8_t>(TopModal() == nullptr ? ModalKind::None : TopModal()->kind);
  for (std::size_t index = 0; index < nativePresentation_.overlays.size(); ++index)
    if (nativePresentation_.overlays[index]) event.overlayMask |= static_cast<std::uint8_t>(1u << index);
  nativePresentation_.history[(event.serial - 1) % nativePresentation_.history.size()] = event;
  NotifyNativeInput(event.at);
}

void Controller::NotifyNativeCellPress(std::size_t index, std::uint32_t nowMs)
{
  if (index >= kGridCount) return;
  NativePresentationEvent event;
  event.kind = NativePresentationKind::CellPress;
  event.at = nowMs == 0 ? 1 : nowMs;
  event.index = static_cast<std::uint8_t>(index);
  RecordNativePresentation(event);
}

void Controller::NotifyNativeOverlay(std::size_t index, bool active, std::uint32_t nowMs)
{
  if (index >= nativePresentation_.overlays.size() || nativePresentation_.overlays[index] == active) return;
  nativePresentation_.overlays[index] = active;
  NativePresentationEvent event;
  event.kind = NativePresentationKind::Overlay;
  event.at = nowMs == 0 ? 1 : nowMs;
  event.index = static_cast<std::uint8_t>(index);
  event.active = active;
  RecordNativePresentation(event);
}

void Controller::NotifyNativePerformance(std::size_t track, const std::uint8_t* notes, std::size_t count,
                                        std::uint8_t velocity, std::uint32_t nowMs)
{
  if (track >= kTrackCount || notes == nullptr || count == 0) return;
  NativePresentationEvent event;
  event.kind = NativePresentationKind::PerformanceNote;
  event.track = static_cast<std::uint8_t>(track);
  event.at = nowMs == 0 ? 1 : nowMs;
  event.count = static_cast<std::uint8_t>(std::min(count, event.notes.size()));
  event.velocity = velocity;
  std::copy_n(notes, event.count, event.notes.begin());
  RecordNativePresentation(event);
}

std::size_t Controller::NativeNoteOwners(std::size_t track, std::uint8_t note, bool arp) const
{
  std::size_t owners = 0;
  for (const auto& contact : nativeContacts_)
    if (contact.active && contact.track == track && contact.arp == arp)
      for (std::size_t index = 0; index < contact.count; ++index) owners += contact.notes[index] == note;
  for (std::size_t pad = 0; pad < state_.padNote.size(); ++pad)
    if (state_.padNote[pad] != kNoNote && state_.padTrack[pad] == track && state_.padArp[pad] == arp)
      for (std::size_t index = 0; index < state_.padChordCount[pad]; ++index) owners += state_.padChord[pad][index] == note;
  return owners;
}

bool Controller::NativePerformanceHeld(std::size_t track, std::uint8_t note) const
{
  return track < kTrackCount && note < 128 &&
      (nativeDirectHeld_[track][note / 32].load(std::memory_order_acquire) & (1u << (note % 32))) != 0;
}

void Controller::NativePublishPerformance()
{
  std::array<std::array<std::uint32_t, 4>, kTrackCount> held{};
  for (const auto& contact : nativeContacts_)
    if (contact.active && !contact.arp)
      for (std::size_t index = 0; index < contact.count; ++index)
      {
        const auto note = contact.notes[index];
        held[contact.track][note / 32] |= 1u << (note % 32);
      }
  for (std::size_t pad = 0; pad < state_.padNote.size(); ++pad)
    if (state_.padNote[pad] != kNoNote && !state_.padArp[pad])
      for (std::size_t index = 0; index < state_.padChordCount[pad]; ++index)
      {
        const auto note = state_.padChord[pad][index];
        held[state_.padTrack[pad]][note / 32] |= 1u << (note % 32);
      }
  for (std::size_t track = 0; track < kTrackCount; ++track)
    for (std::size_t word = 0; word < 4; ++word)
      nativeDirectHeld_[track][word].store(held[track][word], std::memory_order_release);
}

void Controller::NativeSoundPerformance(std::size_t track, std::uint8_t note, std::uint8_t velocity, bool on, bool arp)
{
  const auto owners = NativeNoteOwners(track, note, arp);
  if (on ? owners > 1 : owners != 0) return;
  auto& sequencer = host_.Sequencer();
  if (arp)
  {
    if (on) sequencer.ArpNoteOn(track, note, velocity);
    else sequencer.ArpNoteOff(track, note);
  }
  else if (on)
  {
    host_.NoteOn(track, note, velocity);
    CaptureNoteOn(track, note, velocity);
  }
  else
  {
    std::array<std::uint32_t, 4> sequenced{};
    sequencer.AddSoundingNotes(track, sequenced);
    const auto& audition = state_.stepAudition;
    const bool stepHeld = audition.track == track &&
        std::find(audition.notes.begin(), audition.notes.begin() + audition.count, note) != audition.notes.begin() + audition.count;
    if (!stepHeld && (sequenced[note / 32] & (1u << (note % 32))) == 0) host_.NoteOff(track, note);
    sequencer.RecordNoteOff(track, note);
    CaptureNoteOff(track, note);
  }
}

bool Controller::NativeNoteUp(std::uint8_t id)
{
  for (auto& contact : nativeContacts_)
  {
    if (!contact.active || contact.id != id) continue;
    const auto released = contact;
    contact = NativeContact{};
    NativePublishPerformance();
    for (std::size_t index = 0; index < released.count; ++index)
      NativeSoundPerformance(released.track, released.notes[index], released.velocity, false, released.arp);
    return true;
  }
  return false;
}

void Controller::NativeReleasePerformance()
{
  for (auto& contact : nativeContacts_) if (contact.active) NativeNoteUp(contact.id);
}

bool Controller::NativeNoteDown(std::uint8_t id, std::uint8_t note, std::uint8_t velocity)
{
  for (const auto& contact : nativeContacts_)
    if (contact.active && contact.id == id && contact.baseNote == note && contact.track == state_.track) return true;
  NativeNoteUp(id);
  if (TopModal() != nullptr || host_.Sampler().SavingTrack() >= 0) return false;
  NativeContact* fresh = nullptr;
  for (auto& contact : nativeContacts_) if (!contact.active) { fresh = &contact; break; }
  if (fresh == nullptr) return false;
  std::array<std::uint8_t, kMaxPadNotes> chord{};
  const auto count = PadNotes(state_.keyboard, state_.chords[state_.track], ActiveKind(), note, chord);
  if (count == 0) return false;
  const auto kind = ActiveKind();
  auto& sequencer = host_.Sequencer();
  const bool drums = kind == synth::SynthEngineKind::Drums;
  velocity = velocity == 0 ? state_.keyboard.velocity : std::min<std::uint8_t>(velocity, 127);
  bool editing = false;
  if (state_.scene == Scene::Sequence && CurrentPage() == 0)
  {
    // Touch contacts retain their immediate toggle semantics; PadForSequence
    // now owns the physical keyboard's indexed hold/release edit state.
    editing = sequencer.HasHeldSteps();
    if (editing && sequencer.ToggleNotesOnSelectedSteps(chord.data(), count, drums ? velocity : std::uint8_t{0}))
      MarkDirty();
  }
  const bool audition = !editing || !sequencer.Playing();
  *fresh = NativeContact{true, id, static_cast<std::uint8_t>(state_.track), note, static_cast<std::uint8_t>(audition ? count : 0), velocity,
      audition && !drums && sequencer.GetArpSettings(state_.track).mode != synth::ArpMode::Off, chord};
  NativePublishPerformance();
  NotifyNativePerformance(state_.track, chord.data(), audition ? count : 0, velocity, state_.now);
  state_.lastTrackNoteAt[state_.track] = state_.now;
  if (drums)
  {
    for (std::size_t lane = 0; lane < kDrumNotes.size(); ++lane)
      if (kDrumNotes[lane] == note)
      {
        state_.drumLane[state_.track] = static_cast<std::uint8_t>(lane);
        state_.drumVelocity[state_.track] = velocity;
        state_.drumPadHits[lane] = DrumHit{state_.now == 0 ? 1 : state_.now, 3, velocity};
      }
  }
  else state_.noteLane[state_.track] = note;
  for (std::size_t index = 0; index < fresh->count; ++index)
  {
    NativeSoundPerformance(fresh->track, fresh->notes[index], velocity, true, fresh->arp);
    if (!editing && !fresh->arp && sequencer.RecordArmed() && sequencer.Playing() &&
        NativeNoteOwners(fresh->track, fresh->notes[index], false) == 1)
    {
      sequencer.RecordNoteOn(fresh->track, fresh->notes[index], velocity);
      MarkDirty();
    }
  }
  if (audition && fresh->arp && sequencer.RecordArmed() && sequencer.Playing()) MarkDirty();
  return true;
}

void Controller::NativeCancelGesture()
{
  NativeReleasePerformance();
  state_.repeat = RepeatState{};
  state_.record.okDown = false;
  state_.record.okHeld = false;
  state_.cellPress = CellPress{};
}

bool Controller::NativeFocusCell(std::size_t index)
{
  if (index >= kGridCount) return false;
  if (Modal* modal = Top())
  {
    if (modal->kind == ModalKind::Menu)
    {
      if (Menu()[index].action == MenuAction::None) return false;
      modal->cursor = index;
    }
    else if (modal->kind == ModalKind::SampleRecord)
    {
      if (index > 3) return false;
      modal->cursor = index;
      if (index < 2 && host_.Sampler().BuildUiState(state_.track).hasDraft)
        modal->trimMarker = static_cast<std::uint8_t>(index);
    }
    else if (modal->kind == ModalKind::Record)
    {
      if (index > kRecordView && (index < kRecordFirstTake || index >= kRecordFirstTake + TakeCount())) return false;
      if (state_.record.focus != index) state_.record.takeBar = 0;
      state_.record.focus = static_cast<std::uint8_t>(index);
    }
    else return false;
    nativeFocusedCell_ = static_cast<std::uint8_t>(index);
    return true;
  }

  switch (state_.scene)
  {
    case Scene::Instrument:
    {
      const InstrumentPage* page = CurrentInstrumentPage();
      if (page == nullptr || page->slots[index].Empty()) return false;
      state_.instrumentFocus[std::min<std::size_t>(CurrentPage(), state_.instrumentFocus.size() - 1)] = static_cast<std::uint8_t>(index);
      break;
    }
    case Scene::Setup:
      if (const InstrumentPage* page = SettingsPageShown())
      {
        if (page->slots[index].Empty()) return false;
        state_.settingsFocus[(CurrentPage() - kSetupKeys) % kSettingsPageCount] = static_cast<std::uint8_t>(index);
      }
      else if (CurrentPage() == kSetupProject)
      {
        if (index > 7 || index == 3) return false;
        state_.projectFocus = static_cast<std::uint8_t>(index);
      }
      else return false;
      break;
    case Scene::Sequence:
      if (CurrentPage() != 2 || index > 3) return false;
      state_.seqTrackFocus = static_cast<std::uint8_t>(index);
      break;
    case Scene::Tape:
      if (CurrentPage() == kTapeMix)
      {
        SelectTrack(index % kGridColumns);
        state_.mixFocus = static_cast<std::uint8_t>(index % kGridColumns);
      }
      else if (CurrentPage() == kTapeSends)
      {
        SelectTrack(index % kGridColumns);
        state_.sendsFocus = static_cast<std::uint8_t>(index);
      }
      else if (CurrentPage() == kTapeFx)
      {
        if (index >= synth::fx::kMasterParamCount) return false;
        state_.fxFocus = static_cast<std::uint8_t>(index);
      }
      else return false;
      break;
  }
  nativeFocusedCell_ = static_cast<std::uint8_t>(index);
  return true;
}

void Controller::NativeEnter()
{
  if (Modal* modal = Top())
  {
    if (modal->kind == ModalKind::Menu) RunMenu(modal->cursor);
    else if (modal->kind == ModalKind::SampleRecord)
    {
      if (host_.Sampler().BuildUiState(state_.track).hasDraft) SampleRecordGrid(modal->cursor);
      else SampleRecordEnter();
    }
    else if (modal->kind == ModalKind::Record && state_.record.focus <= kRecordView) RecordGrid(state_.record.focus);
    else ModalEnter();
    return;
  }
  if (state_.scene == Scene::Instrument)
  {
    const InstrumentPage* page = CurrentInstrumentPage();
    if (page != nullptr)
    {
      const Slot& slot = page->slots[InstrumentFocus()];
      if (slot.IsAction()) { RunSlotAction(slot.action); return; }
    }
  }
  else if (state_.scene == Scene::Setup && CurrentPage() == kSetupProject)
  {
    // Native project action tiles are focused first, then explicitly confirmed.
    // BPM and swing have no ENTER action; ADD/SUB edit their value.
    if (state_.projectFocus != 4 && state_.projectFocus != 5) ProjectGrid(state_.projectFocus);
    return;
  }
  else if (state_.scene == Scene::Tape && CurrentPage() == kTapeMix && nativeFocusedCell_ < kGridCount && nativeFocusedCell_ / kGridColumns != 0)
  {
    MixGrid(nativeFocusedCell_);
    return;
  }
  else if (state_.scene == Scene::Tape && CurrentPage() == kTapeSession)
  {
    const std::size_t count = host_.Sequencer().PatternCount(state_.sessionTrack);
    if (state_.sessionPattern > count) return;
    if (host_.Sequencer().SongMode()) host_.Sequencer().SetSongMode(false);
    const bool creating = state_.sessionPattern == count;
    if (host_.Sequencer().QueuePattern(state_.sessionTrack, state_.sessionPattern))
    {
      if (creating) MarkDirty();
      SelectTrack(state_.sessionTrack);
    }
    return;
  }
  else if (state_.scene == Scene::Tape && CurrentPage() == kTapeSong && state_.songRow == host_.Sequencer().SongLength())
  {
    auto& sequencer = host_.Sequencer();
    synth::SongRow fresh{};
    if (sequencer.SongLength() > 0) fresh = sequencer.SongRowAt(sequencer.SongLength() - 1);
    else for (std::size_t track = 0; track < kTrackCount; ++track) fresh.patterns[track] = static_cast<std::int8_t>(sequencer.ActivePattern(track));
    if (sequencer.InsertSongRow(state_.songRow, fresh)) MarkDirty();
    return;
  }
  Enter();
}

bool Controller::Dispatch(const native::Command& command, std::uint32_t nowMs)
{
  using native::Kind;
  state_.now = nowMs;
  const auto index = command.index < 0 ? static_cast<std::size_t>(-1) : static_cast<std::size_t>(command.index);
  if (host_.Sampler().SavingTrack() >= 0 && command.kind != Kind::Cancel && command.kind != Kind::SideUp &&
      command.kind != Kind::PerformanceUp && command.kind != Kind::PerformanceCancelAll)
  {
    ShowToast("SAVING - PLEASE WAIT");
    return false;
  }
  switch (command.kind)
  {
    case Kind::None: return false;
    case Kind::PerformanceDown:
      if (index > 255 || command.value < 0 || command.value > 127) return false;
      return NativeNoteDown(static_cast<std::uint8_t>(index), static_cast<std::uint8_t>(command.value), command.velocity);
    case Kind::PerformanceUp:
      return index <= 255 && NativeNoteUp(static_cast<std::uint8_t>(index));
    case Kind::PerformanceCancelAll:
      NativeReleasePerformance();
      return true;
    case Kind::Cancel:
      NativeCancelGesture();
      return true;
    case Kind::Back:
      NativeCancelGesture();
      Back();
      nativeFocusedCell_ = 0xFF;
      return true;
    case Kind::Enter:
      NativeEnter();
      return true;
    case Kind::Context:
      if (const Modal* modal = TopModal(); modal != nullptr && (modal->kind == ModalKind::SampleBrowser || modal->kind == ModalKind::Record))
      {
        ModalFnEnter();
        return true;
      }
      return false;
    case Kind::SideDown:
    {
      const auto role = static_cast<KeyRole>(command.index);
      if (role == KeyRole::Add || role == KeyRole::Sub || role == KeyRole::Play || role == KeyRole::Enter)
      {
        const std::size_t row = role == KeyRole::Add ? 0 : role == KeyRole::Sub ? 1 : role == KeyRole::Enter ? 2 : 3;
        state_.keyPressedAt[KeyIndex(row, kColumns - 1)] = nowMs == 0 ? 1 : nowMs;
        if (role == KeyRole::Enter && !OkDeferred()) NativeEnter();
        else PressRight(role);
        return true;
      }
      if (role == KeyRole::Esc || role == KeyRole::Tab || role == KeyRole::Fn || role == KeyRole::Ctrl)
      {
        PressLeft(role == KeyRole::Esc ? 0 : role == KeyRole::Tab ? 1 : role == KeyRole::Fn ? 2 : 3);
        return true;
      }
      return false;
    }
    case Kind::SideUp:
    {
      const auto role = static_cast<KeyRole>(command.index);
      if (role == KeyRole::Add || role == KeyRole::Sub || role == KeyRole::Enter || role == KeyRole::Play) ReleaseRight(role);
      else if (role == KeyRole::Esc || role == KeyRole::Tab || role == KeyRole::Fn || role == KeyRole::Ctrl)
        ReleaseLeft(role == KeyRole::Esc ? 0 : role == KeyRole::Tab ? 1 : role == KeyRole::Fn ? 2 : 3);
      else return false;
      return true;
    }
    case Kind::SelectScene:
      if (index >= kSceneCount || TopModal() != nullptr) return false;
      NativeCancelGesture();
      GoToScene(static_cast<Scene>(index));
      nativeFocusedCell_ = 0xFF;
      return true;
    case Kind::SelectTrack:
      if (index >= kTrackCount || TopModal() != nullptr) return false;
      NativeCancelGesture();
      SelectTrack(index);
      nativeFocusedCell_ = 0xFF;
      return true;
    case Kind::TextKey:
    {
      Modal* modal = Top();
      if (modal == nullptr || modal->kind != ModalKind::TextEntry) return false;
      auto& text = modal->text;
      auto& cursor = modal->textCursor;
      cursor = std::min(cursor, text.size());
      if (command.index == -1 && cursor > 0) text.erase(--cursor, 1);
      else if (command.index == -2 && cursor > 0) --cursor;
      else if (command.index == -3 && cursor < text.size()) ++cursor;
      else if (command.index == -4) { text.clear(); cursor = 0; }
      else if (((command.index >= 'A' && command.index <= 'Z') || (command.index >= '0' && command.index <= '9') ||
                command.index == '-' || (command.index == ' ' && cursor > 0)) && text.size() < modal->maxLength)
        text.insert(cursor++, 1, static_cast<char>(command.index));
      return true;
    }
    case Kind::Search:
      if (const Modal* modal = TopModal(); modal != nullptr &&
          (modal->kind == ModalKind::PresetBrowser || modal->kind == ModalKind::SampleBrowser))
      {
        const auto initial = modal->search;
        OpenTextEntry(TextPurpose::Search, "SEARCH", initial, 16);
        return true;
      }
      return false;
    case Kind::SelectScenePage:
      if (index >= kSceneCount || command.value < 0 ||
          static_cast<std::size_t>(command.value) >= PageCount(static_cast<Scene>(index)) || TopModal() != nullptr) return false;
      NativeCancelGesture();
      GoToScene(static_cast<Scene>(index));
      SetPage(static_cast<std::uint8_t>(command.value));
      nativeFocusedCell_ = 0xFF;
      return true;
    case Kind::RollWindow: return false; // viewport belongs to the display adapter
    case Kind::RollLane:
      if (TopModal() != nullptr || state_.scene != Scene::Sequence || CurrentPage() != 0) return false;
      host_.Sequencer().CycleEditLane();
      return true;
    case Kind::RollNote:
    {
      auto& sequencer = host_.Sequencer();
      if (TopModal() != nullptr || state_.scene != Scene::Sequence || CurrentPage() != 0 ||
          index >= sequencer.TrackStepCount(state_.track) || command.value < 0 || command.value > 127) return false;
      const auto note = static_cast<std::uint8_t>(command.value);
      if (!state_.keyboard.multiSelect) sequencer.ClearSelectedSteps();
      sequencer.SetStepSelected(index, true);
      state_.noteLane[state_.track] = note;
      if (sequencer.ToggleStepNotes(state_.track, index, &note, 1)) MarkDirty();
      return true;
    }
    case Kind::TrackMapCell:
      if (index >= kGridCount || TopModal() != nullptr) return false;
      NativeCancelGesture();
      MixGrid(index);
      return true;
    case Kind::SelectPage:
      if (index >= PageCount(state_.scene) || TopModal() != nullptr) return false;
      NativeCancelGesture();
      SetPage(static_cast<std::uint8_t>(index));
      nativeFocusedCell_ = 0xFF;
      return true;
    case Kind::FocusCell: return NativeFocusCell(index);
    case Kind::ActivateCell:
      if (!NativeFocusCell(index)) return false;
      NativeEnter();
      return true;
    case Kind::SelectListRow:
      if (Modal* modal = Top())
      {
        if (modal->kind != ModalKind::PresetBrowser && modal->kind != ModalKind::SampleBrowser && modal->kind != ModalKind::EnginePicker && modal->kind != ModalKind::SampleOptions) return false;
        if (index >= modal->Count()) return false;
        modal->cursor = index;
        if (modal->kind == ModalKind::PresetBrowser) FillPresetRows(*modal);
        nativeFocusedCell_ = 0xFF;
        return true;
      }
      if (state_.scene != Scene::Setup || CurrentPage() != kSetupFiles || index >= state_.projects.size()) return false;
      state_.fileCursor = index;
      return true;
    case Kind::RunMenu:
      if (index >= kGridCount || (TopModal() != nullptr && !MenuOpen())) return false;
      NativeCancelGesture();
      RunMenu(index);
      return true;
    case Kind::SelectStep:
    {
      if (TopModal() != nullptr || state_.scene != Scene::Sequence || CurrentPage() != 0 || index >= host_.Sequencer().TrackStepCount(state_.track)) return false;
      auto& sequencer = host_.Sequencer();
      if (!state_.keyboard.multiSelect) sequencer.ClearSelectedSteps();
      sequencer.SetStepSelected(index, true);
      sequencer.SetViewPage(state_.track, index / kGridCount);
      return true;
    }
    case Kind::SelectPattern:
    {
      if (TopModal() != nullptr || state_.scene != Scene::Sequence || CurrentPage() != 1 || index > host_.Sequencer().PatternCount(state_.track)) return false;
      const bool creating = index == host_.Sequencer().PatternCount(state_.track);
      if (!host_.Sequencer().QueuePattern(state_.track, index)) return false;
      if (creating) MarkDirty();
      return true;
    }
    case Kind::SelectSongCell:
      if (TopModal() != nullptr || state_.scene != Scene::Tape || CurrentPage() != kTapeSong || index > host_.Sequencer().SongLength() || index >= synth::kSongMaxRows || command.value < 0 || command.value >= static_cast<int>(kTrackCount)) return false;
      if (state_.songRow == index && state_.songTrack == static_cast<std::size_t>(command.value)) state_.songLane ^= 1u;
      else state_.songLane = 0;
      state_.songRow = index;
      state_.songTrack = static_cast<std::size_t>(command.value);
      if (index < state_.songScroll) state_.songScroll = index;
      if (index >= state_.songScroll + kGridColumns) state_.songScroll = index - kGridColumns + 1;
      return true;
    case Kind::SelectSessionCell:
      if (TopModal() != nullptr || state_.scene != Scene::Tape || CurrentPage() != kTapeSession || index >= synth::kSequencerMaxPatternCount || command.value < 0 || command.value >= static_cast<int>(kTrackCount)) return false;
      state_.sessionPattern = index;
      state_.sessionTrack = static_cast<std::size_t>(command.value);
      if (index < state_.sessionScroll) state_.sessionScroll = index;
      if (index >= state_.sessionScroll + kRows) state_.sessionScroll = index - kRows + 1;
      SessionGrid((index - state_.sessionScroll) * kGridColumns + static_cast<std::size_t>(command.value));
      return true;
    case Kind::FocusMaster:
      if (TopModal() != nullptr || state_.scene != Scene::Tape || CurrentPage() != kTapeMix) return false;
      state_.mixFocus = static_cast<std::uint8_t>(kTrackCount);
      nativeFocusedCell_ = 0xFF;
      return true;
  }
  return false;
}

}  // namespace groove

#endif
