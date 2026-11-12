#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

#include "Applications/Synth/UI/Groove/GrooveController.h"

namespace groove {

inline constexpr std::uint32_t kSceneSlideMs = 260;
inline constexpr std::uint32_t kPageSlideMs = 190;
inline constexpr std::uint32_t kTrackWipeMs = 280;
inline constexpr std::uint32_t kModalOpenMs = 170;
inline constexpr std::uint32_t kOverlayGrowMs = 150;
inline constexpr std::uint32_t kBootMs = 1700;
// Every kEdgeSweepPeriodMs, once no key has been pressed for kEdgeSweepIdleMs, a light runs down
// both edge columns: the eight edge keys and their strips on screen together.
inline constexpr std::uint32_t kEdgeSweepPeriodMs = 10000;
inline constexpr std::uint32_t kEdgeSweepIdleMs = 4000;
inline constexpr std::uint32_t kOverlayCloseMs = 130;
inline constexpr std::uint32_t kModalCloseMs = 150;
inline constexpr std::uint32_t kToastExitMs = 220;
inline constexpr std::uint32_t kRewindMs = 320;
inline constexpr std::uint32_t kPanicMs = 650;
inline constexpr std::uint32_t kSessionSweepMs = 420;


// Render-side motion state. It diffs the controller once per frame and keeps the timestamps of
// everything worth animating; the renderer turns those into motion. Keeping this out of the
// controller leaves UI behaviour independent of animation.
class Animator {
public:
  enum class TransitionKind : std::uint8_t { None, Scene, Page };

  struct Transition {
    TransitionKind kind = TransitionKind::None;
    Scene fromScene = Scene::Sequence;
    std::uint8_t fromPage = 0;
    int direction = 1;
    std::uint32_t start = 0;
    std::uint32_t duration = 0;
  };

  void Update(const Controller& controller, std::uint32_t now);

  // Turns the controller's LED cells (level + effect) into motion and adds the event layer
  // (ripples, sweeps, trails, playback notes).
  void DecorateLeds(const Controller& controller, const LedFrame& frame, std::array<std::uint8_t, kKeyCount>& levels) const;
  // Fades each key toward its new level: quick rise, slower fall.
  void SmoothLeds(std::array<std::uint8_t, kKeyCount>& levels);

  // The level each key LED shows after effects and fading (0..15). The edge strips on screen
  // mirror their keys with it.
  float LedShown(std::size_t index) const { return index < kKeyCount ? ledShown_[index] : 0.0f; }

  std::uint32_t Now() const { return now_; }
  std::uint32_t BootAt() const { return bootAt_; }
  const Transition& CurrentTransition() const { return transition_; }
  bool TransitionActive() const;
  std::uint32_t TrackChangedAt() const { return trackChangedAt_; }
  std::uint32_t ModalOpenedAt() const { return modalOpenedAt_; }
  std::uint32_t OverlayAt(std::size_t index) const { return overlayAt_[index]; }  // 0 ESC, 1 TAB, 2 FN, 3 CTRL
  std::uint32_t ToastAt() const { return toastAt_; }
  std::uint32_t StepPlayedAt(std::size_t step) const { return stepPlayedAt_[step]; }
  std::uint32_t StepChangedAt(std::size_t step) const { return stepChangedAt_[step]; }
  std::uint32_t GridPressedAt(std::size_t index) const { return gridPressedAt_[index]; }
  // Latest of: the performer played the note, or the sequencer played it on the current track.
  std::uint32_t NoteHitAt(int note) const
  {
    if (note < 0 || note >= 128) return 0;
    return std::max(noteHitAt_[static_cast<std::size_t>(note)], sequencedAt_[static_cast<std::size_t>(note)]);
  }
  // A drum hit runs along its column of key lights from the row it was played on: 0..1 for that lane
  // and row right now. The harder the hit, the further it goes.
  float DrumWave(std::size_t lane, std::size_t row) const;
  bool DrumWaveArrived(std::size_t lane, std::size_t row) const;
  // How hard the drum was last hit, by a pad or the sequencer.
  std::uint8_t DrumHitVelocity(std::size_t lane) const { return lane < drumHits_.size() && drumHits_[lane].at != 0 ? drumHits_[lane].velocity : 127; }
  std::uint32_t LastNoteAt() const { return lastNoteAt_; }
  std::uint32_t BeatAt() const { return beatAt_; }
  bool Downbeat() const { return downbeat_; }
  std::uint32_t PlayToggledAt() const { return playToggledAt_; }
  std::uint32_t PresetChangedAt(std::size_t track) const { return presetChangedAt_[track]; }
  std::uint32_t ProjectChangedAt() const { return projectChangedAt_; }
  std::uint32_t PatternChangedAt(std::size_t track) const { return patternChangedAt_[track]; }
  std::uint32_t ValueChangedAt(std::size_t slot) const { return valueChangedAt_[slot]; }
  std::uint32_t MixToggledAt(std::size_t track, std::size_t kind) const { return mixToggledAt_[track * 3 + kind]; }
  std::uint32_t TextTypedAt() const { return textTypedAt_; }
  float MeterPeak(std::size_t track) const { return meterPeak_[track]; }
  // Endings.
  std::uint32_t OverlayClosedAt(std::size_t index) const { return overlayClosedAt_[index]; }
  bool OverlayActive(std::size_t index) const { return index < overlay_.size() && overlay_[index]; }
  const Modal* ClosingModal() const { return closingModalAt_ != 0 && now_ - closingModalAt_ < kModalCloseMs ? &closingModal_ : nullptr; }
  std::uint32_t ClosingModalAt() const { return closingModalAt_; }
  std::uint32_t ToastEndedAt() const { return toastEndedAt_; }
  const std::string& EndedToast() const { return endedToast_; }
  ToastKind EndedToastKind() const { return endedToastKind_; }
  std::uint32_t StopAt() const { return stopAt_; }
  float StopProgress() const { return stopProgress_; }
  std::size_t StopStep() const { return stopStep_; }
  std::uint32_t DeselectedAt(std::size_t step) const { return deselectedAt_[step]; }
  float OutputPeakHold() const { return outputPeak_; }

  // Exponential smoothing toward `target`, keyed by any stable id; ~70 ms time constant by default.
  float Smooth(std::uint32_t key, float target, float timeConstantMs = 70.0f);
  static std::uint32_t Key(const char* text, std::uint32_t salt = 0);

#if defined(CARDENZA_NATIVE_UI)
  // Presentation notifications share the existing animation clocks. They do
  // not synthesize controller keys, note ownership or project mutations.
  void NotifyCellPress(std::size_t index, std::uint32_t now);
  void NotifyPerformanceNote(std::size_t track, const std::uint8_t* notes, std::size_t count,
                             std::uint8_t velocity, std::uint32_t now);
  void NotifyOverlay(std::size_t index, bool active, std::uint32_t now);
  void NotifyInput(std::uint32_t now);
#endif

private:


  struct SmoothEntry {
    std::uint32_t key = 0;
    float value = 0.0f;
    std::uint32_t time = 0;
    bool used = false;
  };

  std::uint32_t now_ = 0;
  std::uint32_t lastFrame_ = 0;
  std::uint32_t bootAt_ = 0;
  bool primed_ = false;

  Scene scene_ = Scene::Sequence;
  std::array<std::uint8_t, kSceneCount> pages_{};
  std::size_t track_ = 0;
  std::size_t modalDepth_ = 0;
  ModalKind modalKind_ = ModalKind::None;
  std::array<bool, 4> overlay_{};
  std::string toast_;
  std::string projectName_;
  std::array<std::string, kTrackCount> presets_{};
  std::array<std::size_t, kTrackCount> patterns_{};
  std::array<std::uint32_t, kGridCount> fingerprints_{};
  std::size_t fingerprintTrack_ = 0;
  std::size_t fingerprintPattern_ = 0;
  std::array<bool, kGridCount> stepPlaying_{};
  std::array<bool, kGridCount> gridDown_{};
  std::array<std::uint8_t, kRows * kPadColumns> padNotes_{};
  std::array<int, kGridCount> slotValues_{};
  std::uint32_t slotPageKey_ = 0;
  std::array<bool, kTrackCount * 3> mixFlags_{};
  bool playing_ = false;
  std::uint32_t arpTick_ = 0;
  std::size_t step_ = 0;
  std::size_t fingerprintPage_ = 0;
  std::size_t textLength_ = 0;

  Transition transition_{};
  std::uint32_t trackChangedAt_ = 0;
  std::uint32_t modalOpenedAt_ = 0;
  std::array<std::uint32_t, 4> overlayAt_{};
  std::uint32_t toastAt_ = 0;
  std::array<std::uint32_t, kGridCount> stepPlayedAt_{};
  std::array<std::uint32_t, kGridCount> stepChangedAt_{};
  std::array<std::uint32_t, kGridCount> gridPressedAt_{};
  std::array<std::uint32_t, 128> noteHitAt_{};
  std::array<std::uint32_t, 128> sequencedAt_{};
  std::array<DrumHit, kDrumNotes.size()> drumHits_{};
  mutable std::array<bool, kKeyCount> ledStruck_{};  // Rises at once instead of gliding: an impact.
  std::array<float, kKeyCount> ledShown_{};
  std::uint32_t ledFrame_ = 0;
  std::uint32_t lastNoteAt_ = 0;
  std::uint32_t beatAt_ = 0;
  bool downbeat_ = false;
  std::uint32_t playToggledAt_ = 0;
  std::array<std::uint32_t, kTrackCount> presetChangedAt_{};
  std::uint32_t projectChangedAt_ = 0;
  std::array<std::uint32_t, kTrackCount> patternChangedAt_{};
  std::array<std::uint32_t, kGridCount> valueChangedAt_{};
  std::array<std::uint32_t, kTrackCount * 3> mixToggledAt_{};
  std::uint32_t textTypedAt_ = 0;
  std::array<float, kTrackCount> meterPeak_{};
  float outputPeak_ = 0.0f;

  std::array<std::uint32_t, 4> overlayClosedAt_{};
  Modal closingModal_{};
  Modal lastModal_{};
  bool lastModalValid_ = false;
  std::uint32_t lastModalSignature_ = 0;
  std::uint32_t closingModalAt_ = 0;
  ToastKind toastKind_ = ToastKind::Info;
  std::string endedToast_;
  ToastKind endedToastKind_ = ToastKind::Info;
  std::uint32_t toastEndedAt_ = 0;
  std::uint32_t stopAt_ = 0;
  float stopProgress_ = 0.0f;
  std::size_t stopStep_ = 0;
  float lastProgress_ = 0.0f;
  std::array<bool, kGridCount> stepHeld_{};
  std::array<std::uint32_t, kGridCount> deselectedAt_{};

  std::array<SmoothEntry, 96> smooth_{};
#if defined(CARDENZA_NATIVE_UI)
  void ConsumeNativePresentation(const Controller& controller);
  std::uint64_t nativePresentationSerial_ = 0;
  std::uint32_t lastInputAt_ = 0;
  bool nativeDrumTrack_ = false;
#endif
};

}  // namespace groove
