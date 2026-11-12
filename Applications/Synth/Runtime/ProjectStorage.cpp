#include "Applications/Synth/Runtime/ProjectStorage.h"

#include "Library/Memory/Memory.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <new>
#include <ostream>
#include <set>
#include <streambuf>
#include <sys/stat.h>
#if defined(ESP_PLATFORM)
#include <dirent.h>
#else
#include <filesystem>
#endif

#include "Library/Synth/Core/SynthStorage.h"

namespace synth {
namespace {

// The largest project the device can hold: every step of every pattern of every track with eight
// notes is about 2.6 MB of text (patterns live in PSRAM since 2026-11-04; at 1 MB such a project saved
// and never opened again).
constexpr std::size_t kMaxProjectBytes = 4 * 1024 * 1024;
// Version 2 adds the mixer, the display backlight, swing and per-track step counts.
// Version 3 adds the song arrangement (SONG and R lines after the sequencer tracks).
// Version 4 adds the master effects (FX line after SYSTEM: a count, then one value per parameter).
// Version 5 was only written by unreleased development builds: a PLAY line after FX with the play
// settings of the time. It still loads (the PLAY line is skipped).
// Version 6 makes the play settings project data: after FX a KEYS line (scale lock, velocity) and
// one PLAYFX line per track (chord, voicing, arp mode, rate, octaves, gate, latch); the UI line's
// octave, root, scale and layout are the project's keyboard.
// Version 7 gives every pattern its own length, up to 256 steps: the T line ends with the length of
// each pattern instead of the one length of the track.
// Version 8 adds HITS (a note fired 1..4 times in its step): the T line gains the track's new-note
// hits after its new-note length, and every note line a seventh number, the note's hits. It is read
// already and written once kProjectWriteVersion becomes 8 (ProjectStorage.h); older files read as one
// hit per note. The next format change after it takes 9.
constexpr int kMinProjectVersion = 1;
static_assert(kProjectWriteVersion == 7 || kProjectWriteVersion == 8, "This writer knows formats 7 and 8.");
static_assert(kProjectReadVersion == 8, "This reader knows formats 1 to 8.");
constexpr unsigned kMaxMixerLevel = 100;

std::string ProjectPath(const std::string& name) {
  return storage::ProjectDirectory() + "/" + name + ".ccproj";
}

bool Exists(const std::string& path) {
  struct stat info{};
  return stat(path.c_str(), &info) == 0;
}

bool ValidText(const std::string& text, std::size_t maximum) {
  return text.size() <= maximum && std::all_of(text.begin(), text.end(), [](unsigned char c) { return c >= 32 && c < 127; });
}

bool IsProjectEngineKind(int kind) {
  switch (kind) {
    case static_cast<int>(SynthEngineKind::Dx7):
    case static_cast<int>(SynthEngineKind::Juno):
    case static_cast<int>(SynthEngineKind::Sampler):
    case static_cast<int>(SynthEngineKind::Drums):
      return true;
    default:
      return false;
  }
}

bool ValidMetadata(const ProjectData& project) {
  if (!project.sequence || !SynthSequencer::ValidateState(*project.sequence) || project.activeTrack >= kSynthTrackCount ||
      project.octave < -3 || project.octave > 3 || project.root < 0 || project.root > 11 || project.scale >= static_cast<std::size_t>(kProjectScaleCount) ||
      project.layout < 0 || project.layout > 2 || project.ledBrightness < 0 || project.ledBrightness > 15 ||
      project.mixer.master > kMaxMixerLevel || project.lock < 0 || project.lock >= kProjectLockCount || project.velocity < 1 ||
      project.velocity > 127) return false;
  for (const auto& play : project.play)
    if (play.chord < 0 || play.chord >= kProjectChordCount || play.voicing < 0 || play.voicing >= kProjectVoicingCount ||
        static_cast<std::size_t>(play.arp.mode) >= kArpModeCount || play.arp.rate >= kArpRateCount || play.arp.octaves < 1 ||
        play.arp.octaves > kArpMaxOctaves || play.arp.gate < kArpMinGate || play.arp.gate > kArpMaxGate) return false;
  for (std::size_t index = 0; index < fx::kMasterParamCount; ++index)
    if (project.masterFx[index] < fx::kMasterParams[index].minValue || project.masterFx[index] > fx::kMasterParams[index].maxValue) return false;
  for (std::size_t track = 0; track < kSynthTrackCount; ++track) {
    if (project.mixer.level[track] > kMaxMixerLevel) return false;
    const auto& state = project.engines[track];
    if (!IsProjectEngineKind(static_cast<int>(project.kinds[track])) ||
        state.basePresetId.empty() || !ValidText(state.basePresetId, 128) || !ValidText(state.presetId, 128) ||
        !ValidText(state.presetName, 64) || state.values.size() > 128) return false;
    const auto& sample = project.samples[track];
    if (!sample.empty() && (project.kinds[track] != SynthEngineKind::Sampler || !ValidText(sample, 96) ||
        sample == "." || sample == ".." || sample.find_first_of("/\\:\r\n") != std::string::npos)) return false;
    for (const auto& parameter : state.values)
      if (parameter.first.empty() || !ValidText(parameter.first, 64)) return false;
  }
  return true;
}

bool ReadExpected(std::istream& file, const char* expected) {
  char token[32]{};
  return static_cast<bool>(file >> std::setw(sizeof(token)) >> token) && std::string(token) == expected;
}

// Bound fields before allocation, including corrupt files on constrained firmware heaps.
bool ReadQuoted(std::istream& file, std::string& value, std::size_t maximum) {
  file >> std::ws;
  if (file.get() != '"') return false;
  value.clear();
  for (;;) {
    int c = file.get();
    if (c == '"') return true;
    if (c == '\\') c = file.get();
    if (c < 32 || c >= 127 || value.size() >= maximum) return false;
    value.push_back(static_cast<char>(c));
  }
}

}  // namespace

bool IsProjectNameValid(const std::string& name) {
  if (name.empty() || name.size() > 24 || name.front() == ' ' || name.back() == ' ') return false;
  for (const unsigned char c : name)
    if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == ' ' || c == '_' || c == '-')) return false;
  std::string upper = name;
  std::transform(upper.begin(), upper.end(), upper.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
  if (upper == "CON" || upper == "PRN" || upper == "AUX" || upper == "NUL" ||
      (upper.size() == 4 && (upper.compare(0, 3, "COM") == 0 || upper.compare(0, 3, "LPT") == 0) && upper[3] >= '1' && upper[3] <= '9')) return false;
  return true;
}

std::vector<std::string> ListProjects() {
  std::vector<std::string> result;
  auto add = [&](const std::string& filename) {
    std::string candidate = filename;
    if (candidate.size() > 11 && candidate.substr(candidate.size() - 11) == ".ccproj.bak") candidate.resize(candidate.size() - 4);
    if (candidate.size() > 7 && candidate.substr(candidate.size() - 7) == ".ccproj") {
      const std::string name = candidate.substr(0, candidate.size() - 7);
      if (IsProjectNameValid(name)) result.push_back(name);
    }
  };
  const std::string directory = storage::ProjectDirectory();
#if defined(ESP_PLATFORM)
  if (DIR* dir = opendir(directory.c_str())) {
    while (auto* entry = readdir(dir)) add(entry->d_name);
    closedir(dir);
  }
#else
  std::error_code error;
  for (std::filesystem::directory_iterator it(directory, error), end; !error && it != end; it.increment(error)) {
    if (it->is_regular_file(error)) add(it->path().filename().string());
  }
#endif
  std::sort(result.begin(), result.end());
  result.erase(std::unique(result.begin(), result.end()), result.end());
  return result;
}

namespace {

void WriteProjectText(std::ostream& file, const ProjectData& project, int version) {
  // Format 8's HITS; format 7 has none (the notes are written as plain ones).
  const bool hits = version >= 8;
  file << "CARDENZA_PROJECT " << version << "\nBUILD CardenzaSynth 1\nUI " << project.activeTrack << ' ' << project.octave << ' '
       << project.root << ' ' << project.scale << ' ' << project.layout << ' ' << project.ledBrightness << '\n';
  file << "MASTER " << static_cast<int>(project.mixer.master) << '\n';
  for (std::size_t track = 0; track < kSynthTrackCount; ++track)
    file << "MIX " << track << ' ' << static_cast<int>(project.mixer.level[track]) << ' ' << project.mixer.muted[track] << ' '
         << project.mixer.soloed[track] << '\n';
  file << "SYSTEM " << static_cast<int>(project.backlight) << '\n';
  file << "FX " << project.masterFx.size();
  for (const auto value : project.masterFx) file << ' ' << static_cast<int>(value);
  file << '\n';
  file << "KEYS " << project.lock << ' ' << project.velocity << '\n';
  for (std::size_t track = 0; track < kSynthTrackCount; ++track) {
    const auto& play = project.play[track];
    file << "PLAYFX " << track << ' ' << play.chord << ' ' << play.voicing << ' ' << static_cast<int>(play.arp.mode) << ' '
         << static_cast<int>(play.arp.rate) << ' ' << static_cast<int>(play.arp.octaves) << ' ' << static_cast<int>(play.arp.gate) << ' '
         << (play.arp.latch ? 1 : 0) << '\n';
  }
  for (std::size_t track = 0; track < kSynthTrackCount; ++track) {
    const auto& state = project.engines[track];
    file << "TRACK " << track << ' ' << static_cast<int>(project.kinds[track]) << ' ' << std::quoted(state.basePresetId) << ' '
         << std::quoted(state.presetId) << ' ' << std::quoted(state.presetName) << ' ' << std::quoted(project.samples[track]) << '\n';
    file << "PARAM " << state.values.size() << '\n';
    for (const auto& parameter : state.values) file << std::quoted(parameter.first) << ' ' << parameter.second << '\n';
  }
  file << "SEQ " << project.sequence->bpm << ' ' << static_cast<int>(project.sequence->swing) << '\n';
  for (const auto& track : project.sequence->tracks) {
    file << "T " << track.patternCount << ' ' << track.activePattern << ' ' << static_cast<int>(track.defaultVelocity) << ' '
         << track.defaultLengthPulses;
    if (hits) file << ' ' << static_cast<int>(track.defaultHits);
    file << ' ' << track.enabled << ' ' << track.muted;
    for (std::size_t p = 0; p < track.patternCount; ++p) file << ' ' << track.patternSteps[p];
    file << '\n';
    std::size_t count = 0;
    for (std::size_t p = 0; p < track.patternCount; ++p) {
      const auto& steps = track.patterns[p].steps;
      for (std::size_t s = steps.NextInUse(0); s < steps.size(); s = steps.NextInUse(s + 1))
        for (const auto& note : steps[s].notes) count += note.active;
    }
    file << "NOTES " << count << '\n';
    for (std::size_t p = 0; p < track.patternCount; ++p)
      for (std::size_t s = track.patterns[p].steps.NextInUse(0); s < kSequencerMaxSteps; s = track.patterns[p].steps.NextInUse(s + 1))
        for (const auto& note : track.patterns[p].steps[s].notes)
          if (note.active) {
            file << p << ' ' << s << ' ' << static_cast<int>(note.note) << ' ' << static_cast<int>(note.velocity) << ' ' << note.lengthPulses
                 << ' ' << static_cast<int>(note.microstep);
            if (hits) file << ' ' << static_cast<int>(note.Hits());
            file << '\n';
          }
  }
  // The song mode switch is a performance control and is not stored.
  file << "SONG " << project.sequence->songLength << ' ' << project.sequence->songLoop << '\n';
  for (std::size_t row = 0; row < project.sequence->songLength; ++row) {
    const auto& songRow = project.sequence->song[row];
    file << 'R';
    for (const auto pattern : songRow.patterns) file << ' ' << static_cast<int>(pattern);
    file << ' ' << static_cast<int>(songRow.repeats) << '\n';
  }
  file << "END\n";
}

// Hashes what goes through it (FNV-1a) and keeps none of it.
class HashBuffer : public std::streambuf {
public:
  HashBuffer() { setp(buffer_, buffer_ + sizeof(buffer_)); }
  std::uint32_t Hash() {
    Drain();
    return hash_;
  }

protected:
  int_type overflow(int_type c) override {
    Drain();
    if (!traits_type::eq_int_type(c, traits_type::eof())) {
      *pptr() = traits_type::to_char_type(c);
      pbump(1);
    }
    return traits_type::not_eof(c);
  }

private:
  void Drain() {
    for (const char* at = pbase(); at < pptr(); ++at) hash_ = (hash_ ^ static_cast<unsigned char>(*at)) * 16777619u;
    setp(buffer_, buffer_ + sizeof(buffer_));
  }

  char buffer_[64];
  std::uint32_t hash_ = 2166136261u;
};

ProjectResult WriteProject(const std::string& path, const ProjectData& project, int version = kProjectWriteVersion) {
  const bool existing = Exists(path);
  const std::string temporary = path + ".tmp";
  const std::string backup = path + ".bak";
  // Binary: the same bytes on every host (a Windows text stream wrote CRLF).
  std::ofstream file(temporary, std::ios::trunc | std::ios::binary);
  if (!file) return ProjectResult::StorageError;
  WriteProjectText(file, project, version);
  file.flush();
  const bool written = file.good();
  file.close();
  if (!written || file.fail()) { std::remove(temporary.c_str()); return ProjectResult::StorageError; }
  // FAT and Windows cannot replace an existing file with rename. Preserve a recoverable backup.
  if (existing) {
    if (Exists(backup) && std::remove(backup.c_str()) != 0) return ProjectResult::StorageError;
    if (std::rename(path.c_str(), backup.c_str()) != 0) return ProjectResult::StorageError;
  }
  if (std::rename(temporary.c_str(), path.c_str()) != 0) {
    if (existing) std::rename(backup.c_str(), path.c_str());
    return ProjectResult::StorageError;
  }
  if (existing) std::remove(backup.c_str());
  return ProjectResult::Ok;
}

ProjectResult ReadProject(const std::string& path, ProjectData& destination) {
  // A backup is recoverable after interruption between the two rename operations.
  const std::string source = Exists(path) ? path : path + ".bak";
  struct stat info{};
  if (stat(source.c_str(), &info) != 0) return ProjectResult::StorageError;
  if (info.st_size <= 0 || static_cast<std::size_t>(info.st_size) > kMaxProjectBytes) return ProjectResult::Invalid;
  std::ifstream file(source);
  if (!file) return ProjectResult::StorageError;
  ProjectData project;
  project.sequence.reset(new (memory::slow) SynthSequencer::ProjectState);
  if (!project.sequence) return ProjectResult::NoMemory;
  int version = 0;
  int build = 0;
  if (!ReadExpected(file, "CARDENZA_PROJECT") || !(file >> version) || version < kMinProjectVersion || version > kProjectReadVersion ||
      !ReadExpected(file, "BUILD") || !ReadExpected(file, "CardenzaSynth") || !(file >> build) || build != 1 || !ReadExpected(file, "UI") ||
      !(file >> project.activeTrack >> project.octave >> project.root >> project.scale >> project.layout >> project.ledBrightness)) return ProjectResult::Invalid;
  if (version >= 2) {
    unsigned master = 0, backlight = 0;
    if (!ReadExpected(file, "MASTER") || !(file >> master) || master > kMaxMixerLevel) return ProjectResult::Invalid;
    project.mixer.master = static_cast<std::uint8_t>(master);
    for (std::size_t track = 0; track < kSynthTrackCount; ++track) {
      std::size_t index = 0;
      unsigned level = 0, muted = 0, soloed = 0;
      if (!ReadExpected(file, "MIX") || !(file >> index >> level >> muted >> soloed) || index != track || level > kMaxMixerLevel ||
          muted > 1 || soloed > 1) return ProjectResult::Invalid;
      project.mixer.level[track] = static_cast<std::uint8_t>(level);
      project.mixer.muted[track] = muted != 0;
      project.mixer.soloed[track] = soloed != 0;
    }
    if (!ReadExpected(file, "SYSTEM") || !(file >> backlight) || backlight > 255) return ProjectResult::Invalid;
    project.backlight = static_cast<std::uint8_t>(backlight);
  }
  if (version >= 4) {
    // Later versions may append parameters; known ones are read, unknown ones skipped.
    std::size_t count = 0;
    if (!ReadExpected(file, "FX") || !(file >> count) || count > 64) return ProjectResult::Invalid;
    for (std::size_t index = 0; index < count; ++index) {
      int value = 0;
      if (!(file >> value)) return ProjectResult::Invalid;
      if (index >= fx::kMasterParamCount) continue;
      if (value < fx::kMasterParams[index].minValue || value > fx::kMasterParams[index].maxValue) return ProjectResult::Invalid;
      project.masterFx[index] = static_cast<std::uint8_t>(value);
    }
  }
  if (version == 5) {
    // Development builds stored the play settings of the time here.
    std::string ignored;
    if (!ReadExpected(file, "PLAY") || !std::getline(file, ignored) || ignored.size() > 128) return ProjectResult::Invalid;
  }
  if (version >= 6) {
    int lock = 0, velocity = 0;
    if (!ReadExpected(file, "KEYS") || !(file >> lock >> velocity) || lock < 0 || lock >= kProjectLockCount || velocity < 1 ||
        velocity > 127) return ProjectResult::Invalid;
    project.lock = lock;
    project.velocity = velocity;
    for (std::size_t track = 0; track < kSynthTrackCount; ++track) {
      std::size_t index = 0;
      int chord = 0, voicing = 0, mode = 0, rate = 0, octaves = 0, gate = 0, latch = 0;
      if (!ReadExpected(file, "PLAYFX") || !(file >> index >> chord >> voicing >> mode >> rate >> octaves >> gate >> latch) || index != track ||
          chord < 0 || chord >= kProjectChordCount || voicing < 0 || voicing >= kProjectVoicingCount || mode < 0 ||
          mode >= static_cast<int>(kArpModeCount) || rate < 0 || rate >= static_cast<int>(kArpRateCount) || octaves < 1 ||
          octaves > kArpMaxOctaves || gate < kArpMinGate || gate > kArpMaxGate || latch < 0 || latch > 1) return ProjectResult::Invalid;
      auto& play = project.play[track];
      play.chord = chord;
      play.voicing = voicing;
      play.arp.mode = static_cast<ArpMode>(mode);
      play.arp.rate = static_cast<std::uint8_t>(rate);
      play.arp.octaves = static_cast<std::uint8_t>(octaves);
      play.arp.gate = static_cast<std::uint8_t>(gate);
      play.arp.latch = latch != 0;
    }
    project.hasPlay = true;
  }
  for (std::size_t track = 0; track < kSynthTrackCount; ++track) {
    std::size_t index = 0, parameterCount = 0;
    int kind = 0;
    auto& state = project.engines[track];
    if (!ReadExpected(file, "TRACK") || !(file >> index >> kind) || !ReadQuoted(file, state.basePresetId, 128) ||
        !ReadQuoted(file, state.presetId, 128) || !ReadQuoted(file, state.presetName, 64) || !ReadQuoted(file, project.samples[track], 96) ||
        index != track || !IsProjectEngineKind(kind) ||
        !ReadExpected(file, "PARAM") || !(file >> parameterCount) || parameterCount > 128) return ProjectResult::Invalid;
    project.kinds[track] = static_cast<SynthEngineKind>(kind);
    for (std::size_t p = 0; p < parameterCount; ++p) {
      std::string id;
      int value = 0;
      if (!ReadQuoted(file, id, 64) || !(file >> value) || !state.values.emplace(id, value).second) return ProjectResult::Invalid;
    }
  }
  unsigned bpm = 0, swing = 50;
  if (!ReadExpected(file, "SEQ") || !(file >> bpm) || bpm < 30 || bpm > 300 ||
      (version >= 2 && (!(file >> swing) || swing < 50 || swing > 75))) return ProjectResult::Invalid;
  project.sequence->bpm = static_cast<std::uint16_t>(bpm);
  project.sequence->swing = static_cast<std::uint8_t>(swing);
  for (auto& track : project.sequence->tracks) {
    unsigned velocity = 0, length = 0, enabled = 0, muted = 0, hits = 1;
    std::size_t noteCount = 0;
    if (!ReadExpected(file, "T") || !(file >> track.patternCount >> track.activePattern >> velocity >> length) ||
        (version >= 8 && !(file >> hits)) || !(file >> enabled >> muted) ||
        track.patternCount < 1 || track.patternCount > kSequencerMaxPatternCount || track.activePattern >= track.patternCount ||
        velocity < 1 || velocity > 127 || length < 1 || length > kSequencerMaxSteps * kSequencerPulsesPerStep || hits < 1 ||
        hits > kSequencerMaxHits || enabled > 1 || muted > 1) return ProjectResult::Invalid;
    // Before version 7 all the patterns of a track shared one length.
    if (version >= 7) {
      for (std::size_t p = 0; p < track.patternCount; ++p)
        if (!(file >> track.patternSteps[p]) || track.patternSteps[p] < 1 || track.patternSteps[p] > kSequencerMaxSteps) return ProjectResult::Invalid;
    } else if (version >= 2) {
      std::uint16_t steps = 0;
      if (!(file >> steps) || steps < 1 || steps > kSequencerStepCount) return ProjectResult::Invalid;
      track.patternSteps.fill(steps);
    }
    if (!ReadExpected(file, "NOTES") || !(file >> noteCount) || noteCount > track.patternCount * kSequencerMaxSteps * 8) return ProjectResult::Invalid;
    track.defaultVelocity = static_cast<std::uint8_t>(velocity);
    track.defaultLengthPulses = static_cast<std::uint16_t>(length);
    track.defaultHits = static_cast<std::uint8_t>(hits);
    track.enabled = enabled != 0;
    track.muted = muted != 0;
    for (std::size_t n = 0; n < noteCount; ++n) {
      std::size_t p = 0, s = 0;
      unsigned pitch = 0, microstep = 0;
      hits = 1;  // before version 8 every note is a single hit
      if (!(file >> p >> s >> pitch >> velocity >> length >> microstep) || (version >= 8 && !(file >> hits)) || p >= track.patternCount ||
          s >= kSequencerMaxSteps || pitch > 127 || velocity < 1 || velocity > 127 || length < 1 || length > kSequencerMaxSteps * kSequencerPulsesPerStep ||
          microstep >= kSequencerMicrostepCount || hits < 1 || hits > kSequencerMaxHits) return ProjectResult::Invalid;
      if (!track.patterns.Ensure(p)) return ProjectResult::NoMemory;
      auto* step = track.patterns[p].steps.TryEdit(s);
      if (step == nullptr) return ProjectResult::NoMemory;
      auto& notes = step->notes;
      auto slot = std::find_if(notes.begin(), notes.end(), [](const auto& note) { return !note.active; });
      if (slot == notes.end()) return ProjectResult::Invalid;
      *slot = {static_cast<std::uint8_t>(pitch), static_cast<std::uint8_t>(velocity), static_cast<std::uint16_t>(length), static_cast<std::uint8_t>(microstep), true,
               static_cast<std::uint8_t>(hits)};
    }
  }
  // Versions 1 and 2 load with an empty, looping song (the ProjectState defaults).
  if (version >= 3) {
    std::size_t length = 0;
    unsigned loop = 0;
    if (!ReadExpected(file, "SONG") || !(file >> length >> loop) || length > kSongMaxRows || loop > 1) return ProjectResult::Invalid;
    project.sequence->songLength = length;
    project.sequence->songLoop = loop != 0;
    for (std::size_t row = 0; row < length; ++row) {
      auto& songRow = project.sequence->song[row];
      if (!ReadExpected(file, "R")) return ProjectResult::Invalid;
      for (std::size_t track = 0; track < kSynthTrackCount; ++track) {
        int pattern = 0;
        if (!(file >> pattern) || pattern < -1 || pattern >= static_cast<int>(project.sequence->tracks[track].patternCount)) return ProjectResult::Invalid;
        songRow.patterns[track] = static_cast<std::int8_t>(pattern);
      }
      unsigned repeats = 0;
      if (!(file >> repeats) || repeats < 1 || repeats > kSongMaxRepeats) return ProjectResult::Invalid;
      songRow.repeats = static_cast<std::uint8_t>(repeats);
    }
  }
  if (!ReadExpected(file, "END") || !ValidMetadata(project)) return ProjectResult::Invalid;
  file >> std::ws;
  if (!file.eof()) return ProjectResult::Invalid;
  destination = std::move(project);
  return ProjectResult::Ok;
}

}  // namespace

ProjectResult SaveProjectFile(const std::string& name, const ProjectData& project, bool overwrite) {
  if (!IsProjectNameValid(name) || !ValidMetadata(project)) return ProjectResult::Invalid;
  if (!storage::EnsureLayout()) return ProjectResult::StorageError;
  const std::string path = ProjectPath(name);
  if ((Exists(path) || Exists(path + ".bak")) && !overwrite) return ProjectResult::Exists;
  return WriteProject(path, project);
}

ProjectResult LoadProjectFile(const std::string& name, ProjectData& destination) {
  if (!IsProjectNameValid(name)) return ProjectResult::Invalid;
  return ReadProject(ProjectPath(name), destination);
}

ProjectResult RenameProjectFile(const std::string& from, const std::string& to) {
  if (!IsProjectNameValid(from) || !IsProjectNameValid(to)) return ProjectResult::Invalid;
  if (from == to) return ProjectResult::Ok;
  if (!storage::EnsureLayout()) return ProjectResult::StorageError;
  const std::string source = ProjectPath(from);
  const std::string target = ProjectPath(to);
  const bool file = Exists(source);
  const bool backup = Exists(source + ".bak");
  if (!file && !backup) return ProjectResult::Invalid;
  // FAT (and Windows) ignore case: there, a name that differs only in case is the same file, so it
  // goes through a step name, and it is not a name taken by another project.
  const bool caseOnly = std::equal(from.begin(), from.end(), to.begin(), to.end(), [](unsigned char a, unsigned char b) {
    return std::toupper(a) == std::toupper(b);
  });
  if (!caseOnly && (Exists(target) || Exists(target + ".bak"))) return ProjectResult::Exists;
  const auto move = [caseOnly](const std::string& a, const std::string& b) {
    if (!caseOnly) return std::rename(a.c_str(), b.c_str()) == 0;
    const std::string step = b + ".ren";
    return std::rename(a.c_str(), step.c_str()) == 0 && std::rename(step.c_str(), b.c_str()) == 0;
  };
  if (file && !move(source, target)) return ProjectResult::StorageError;
  if (backup && !move(source + ".bak", target + ".bak")) {
    // Never half renamed: a backup left under the old name would list that name again.
    if (file) move(target, source);
    return ProjectResult::StorageError;
  }
  return ProjectResult::Ok;
}

ProjectResult SaveProjectFileAt(const std::string& path, const ProjectData& project) {
  if (path.empty() || !ValidMetadata(project)) return ProjectResult::Invalid;
  return WriteProject(path, project);
}

std::uint32_t ProjectFingerprint(const ProjectData& project) {
  if (!project.sequence) return 0;
  HashBuffer buffer;
  std::ostream text(&buffer);
  WriteProjectText(text, project, kProjectWriteVersion);
  return buffer.Hash() | 1u;  // never 0, which means none
}

ProjectResult LoadProjectFileAt(const std::string& path, ProjectData& destination) {
  if (path.empty()) return ProjectResult::Invalid;
  return ReadProject(path, destination);
}

#if defined(CARDENZA_PROJECT_TEST_HOOKS)
ProjectResult SaveProjectFileAtVersionForTest(const std::string& path, const ProjectData& project, int version) {
  if (path.empty() || !ValidMetadata(project) || version < 7 || version > kProjectReadVersion) return ProjectResult::Invalid;
  return WriteProject(path, project, version);
}
#endif

}  // namespace synth
