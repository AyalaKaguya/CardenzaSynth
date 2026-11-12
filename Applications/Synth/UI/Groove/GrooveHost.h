#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "Applications/Synth/Runtime/Sampler/SamplerManager.h"
#include "Applications/Synth/Runtime/Sequencer/SynthSequencer.h"
#include "Applications/Synth/UI/Groove/GrooveTypes.h"
#include "Library/Synth/Audio/SynthFx.h"
#include "Library/Synth/Core/ISynthEngine.h"
#include "OS/Storage/DeviceSettings.h"

namespace groove {

// UI state that is stored with a project.
// UI state saved with the project: the active track, the keyboard for every track and each
// track's chord effect (the arpeggiators live in the sequencer).
struct ProjectUiSettings {
  std::size_t activeTrack = synth::kDefaultActiveTrack;
  KeyboardSettings keyboard{};
  std::array<ChordSettings, kTrackCount> chords{};
};

enum class SaveResult : std::uint8_t {
  Ok,
  Exists,
  Failed,
};

struct SystemInfo {
  std::size_t freeHeap = 0;         // internal RAM
  std::size_t minimumFreeHeap = 0;
  std::size_t freePsram = 0;
  std::size_t totalPsram = 0;       // 0 = none
  bool storageReady = false;
};

// Everything the UI may do to the runtime. The app implements it; UI tests can drive the real app.
class Host {
public:
  virtual ~Host() = default;

  // Tracks and engines.
  virtual synth::ISynthEngine* Engine(std::size_t track) = 0;
  virtual const synth::ISynthEngine* Engine(std::size_t track) const = 0;
  virtual synth::SynthEngineKind EngineKind(std::size_t track) const = 0;
  virtual bool SwitchEngine(std::size_t track, synth::SynthEngineKind kind, std::string& error) = 0;

  // Live performance notes (pads). The sequencer plays its own notes.
  virtual void NoteOn(std::size_t track, std::uint8_t note, std::uint8_t velocity) = 0;
  virtual void NoteOff(std::size_t track, std::uint8_t note) = 0;
  // Stops the sequencer and silences every engine.
  virtual void Panic() = 0;

  virtual synth::SynthSequencer& Sequencer() = 0;
  virtual const synth::SynthSequencer& Sequencer() const = 0;
  virtual synth::SamplerManager& Sampler() = 0;
  virtual const synth::SamplerManager& Sampler() const = 0;
  // Starts the current sample at its root note.
  virtual void PreviewSample(std::size_t track) = 0;
  virtual std::vector<std::string> ListSamples() const = 0;

  // Mixer. Level is 0..100; solo silences every non-soloed track.
  virtual void SetTrackLevel(std::size_t track, std::uint8_t level) = 0;
  virtual std::uint8_t TrackLevel(std::size_t track) const = 0;
  virtual void SetTrackMuted(std::size_t track, bool muted) = 0;
  virtual bool TrackMuted(std::size_t track) const = 0;
  virtual void SetTrackSoloed(std::size_t track, bool soloed) = 0;
  virtual bool TrackSoloed(std::size_t track) const = 0;
  // True when the track is audible after mute and solo.
  virtual bool TrackAudible(std::size_t track) const = 0;
  virtual void SetMasterVolume(std::uint8_t percent) = 0;
  virtual std::uint8_t MasterVolume() const = 0;
  virtual float TrackActivity(std::size_t track) const = 0;
  virtual float OutputPeak() const = 0;

  // Master effects, indexed by synth::fx::MasterParam (ranges in synth::fx::kMasterParams). They
  // belong to the project. Track inserts and sends are "fx.*" parameters of each track's engine.
  virtual void SetMasterFx(std::size_t param, int value) = 0;
  virtual int MasterFx(std::size_t param) const = 0;
  // Decaying 0..1 level of a bus return (synth::fx::kBusDelay / kBusReverb).
  virtual float FxReturnLevel(std::size_t bus) const = 0;
  virtual bool FxBusAvailable(std::size_t bus) const = 0;

  // Projects.
  virtual std::vector<std::string> ListProjects() const = 0;
  virtual SaveResult SaveProject(const std::string& name, bool overwrite, const ProjectUiSettings& settings, std::string& message) = 0;
  // Renames a saved project on the card (its file only: the project that is open keeps its state).
  // Exists: the new name is another project's; Failed: message says why.
  virtual SaveResult RenameProject(const std::string& from, const std::string& to, std::string& message) = 0;
  virtual bool LoadProject(const std::string& name, ProjectUiSettings& settings, std::string& message) = 0;
  virtual bool NewProject(ProjectUiSettings& settings, std::string& message) = 0;

  // System.
  virtual void SetLedBrightness(std::uint8_t level) = 0;  // 0..15
  virtual std::uint8_t LedBrightness() const = 0;
  virtual void SetBacklight(std::uint8_t level) = 0;  // 0..255
  virtual std::uint8_t Backlight() const = 0;
  // The console log on the card (a device setting, OS/System/Log.h).
  virtual void SetLogging(bool on) = 0;
  virtual bool Logging() const = 0;
  // The screen's palette (a device setting, sys.theme); the renderer applies it every frame.
  virtual void SetScreenTheme(os::storage::Theme theme) = 0;
  virtual os::storage::Theme ScreenTheme() const = 0;
  virtual SystemInfo QuerySystem() const = 0;
};

}  // namespace groove
