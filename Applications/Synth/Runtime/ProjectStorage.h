#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "Applications/Synth/Runtime/Sequencer/SynthSequencer.h"
#include "Library/Synth/Audio/SynthFx.h"
#include "Library/Synth/Core/ISynthEngine.h"

namespace synth {

static_assert(kSynthTrackCount == 4, "Mixer defaults list one level per track.");

// Scale indices the UI line may hold (readers of format v4 and earlier accepted 0..7 only).
inline constexpr int kProjectScaleCount = 14;
inline constexpr int kProjectScaleCountV4 = 8;
// Ranges of the play settings stored from version 6 (the groovebox's chords, voicings, scale locks).
inline constexpr int kProjectChordCount = 11;
inline constexpr int kProjectVoicingCount = 4;
inline constexpr int kProjectLockCount = 3;

struct ProjectData {
  // Project file v6. Version 1 files load with these mixer and backlight defaults; versions 1
  // and 2 load with an empty song; versions 1 to 3 load with the default master effects.
  // Version 5 files (unreleased development builds) load too; their PLAY line is skipped.
  //
  // From version 6 the play settings belong to the project (hasPlay): the keyboard (the UI line's
  // octave, root, scale and layout, plus KEYS: scale lock and velocity) and each track's chord and
  // arpeggiator (PLAYFX lines). Older files have none; the groovebox then keeps the ones it has.
  // LED brightness (UI line), MASTER and SYSTEM (backlight) are device settings
  // (docs/app-storage-protocol.md): still written, ignored when a project loads.
  struct MixerData {
    std::array<std::uint8_t, kSynthTrackCount> level{{100, 100, 100, 100}};  // 0..100
    std::array<bool, kSynthTrackCount> muted{};
    std::array<bool, kSynthTrackCount> soloed{};
    std::uint8_t master = 80;  // 0..100
  };

  std::array<SynthEngineKind, kSynthTrackCount> kinds{};
  std::array<SynthEngineState, kSynthTrackCount> engines{};
  std::array<std::string, kSynthTrackCount> samples{};
  std::unique_ptr<SynthSequencer::ProjectState> sequence;
  std::size_t activeTrack = kDefaultActiveTrack;
  int octave = 0;
  int root = 0;
  // Defaults of the previous UI (minor, scale layout).
  std::size_t scale = 1;
  int layout = 1;
  int ledBrightness = 15;
  MixerData mixer;
  std::uint8_t backlight = 180;  // 0..255
  // Master bus effects, indexed by fx::MasterParam.
  std::array<std::uint8_t, fx::kMasterParamCount> masterFx = DefaultMasterFx();
  // Play settings (version 6).
  struct TrackPlay {
    int chord = 0;    // 0..kProjectChordCount - 1
    int voicing = 0;  // 0..kProjectVoicingCount - 1
    ArpSettings arp{};
  };
  bool hasPlay = false;
  int lock = 0;        // 0..kProjectLockCount - 1
  int velocity = 110;  // 1..127
  std::array<TrackPlay, kSynthTrackCount> play{};

  static std::array<std::uint8_t, fx::kMasterParamCount> DefaultMasterFx()
  {
    std::array<std::uint8_t, fx::kMasterParamCount> values{};
    for (std::size_t index = 0; index < values.size(); ++index) values[index] = static_cast<std::uint8_t>(fx::kMasterParams[index].defaultValue);
    return values;
  }
};

// The project format this build writes, and the newest it reads. Format 8 adds HITS and is read
// already; it is written from the one format bump of the next test build on (kProjectWriteVersion = 8,
// AGENT.md Storage Rules): until then a project saved, the autosave included, keeps no hits.
inline constexpr int kProjectWriteVersion = 7;
inline constexpr int kProjectReadVersion = 8;

enum class ProjectResult { Ok, Exists, Invalid, StorageError, NoMemory };

bool IsProjectNameValid(const std::string& name);
// Named projects live in storage::ProjectDirectory() as <name>.ccproj.
std::vector<std::string> ListProjects();
ProjectResult SaveProjectFile(const std::string& name, const ProjectData& project, bool overwrite);
ProjectResult LoadProjectFile(const std::string& name, ProjectData& project);
// Renames <from>.ccproj (and its backup) to <to>.ccproj on the card; nothing else changes. Exists: <to>
// is another project's name; Invalid: a bad name, or no project <from>. A name that differs only in
// case is renamed too (FAT would see the same file).
ProjectResult RenameProjectFile(const std::string& from, const std::string& to);
// The same format at an explicit path (the autosave). Saving replaces the file atomically
// and keeps a backup until the new file is in place; loading falls back to that backup.
ProjectResult SaveProjectFileAt(const std::string& path, const ProjectData& project);
ProjectResult LoadProjectFileAt(const std::string& path, ProjectData& project);
// A hash of the bytes the project's file would hold, never 0: tells whether a file written from it would
// change anything. Costs about what writing the file costs, without the card.
std::uint32_t ProjectFingerprint(const ProjectData& project);
#if defined(CARDENZA_PROJECT_TEST_HOOKS)
// Test only: writes format `version` (7 or 8) instead of kProjectWriteVersion.
ProjectResult SaveProjectFileAtVersionForTest(const std::string& path, const ProjectData& project, int version);
#endif

}  // namespace synth
