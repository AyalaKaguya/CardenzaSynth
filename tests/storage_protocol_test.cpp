// Protocol 2: the 0.1.0 release's CardenzaSynth layout and SD-only settings; earlier layouts remain
// untouched. The simulator card is taken out and put back without resetting the settings cache, and
// last a run starts without one.
#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

#include "Devices/CardenzaSim/SimulatorInput.h"
#include "Devices/Device.h"
#include "OS/Storage/DeviceSettings.h"
#include "OS/Storage/Settings.h"
#include "OS/Storage/Storage.h"

namespace fs = std::filesystem;

namespace {
std::uint8_t ledMaster = 0;
std::uint8_t backlight = 0;
std::uint8_t outputVolume = 0;
}  // namespace

// DeviceSettings applies values to these; the test only records them.
namespace Device {
namespace LED {
void SetGlobalBrightness(std::uint8_t level) { ledMaster = level; }
}
namespace Display {
void SetBacklight(std::uint8_t level) { backlight = level; }
}
namespace Audio {
void SetOutputVolume(std::uint8_t percent) { outputVolume = percent; }
}
}  // namespace Device

namespace {

using namespace os::storage;

fs::path Card() { return fs::path(Device::Storage::RootPath()); }
fs::path Internal() { return Card().parent_path() / "internal"; }

std::string ReadText(const fs::path& path)
{
  std::ifstream input(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(input), {}};
}

void WriteText(const fs::path& path, const std::string& text)
{
  fs::create_directories(path.parent_path());
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  file << text;
}

void ResetCard()
{
  CardenzaSim::Input::SetCardInserted(true);
  fs::remove_all(Card());
  fs::remove_all(Internal());
  ReloadAllSettings();
}

void TestAppPaths()
{
  const std::string root = Card().generic_string() + "/CardenzaSynth";
  assert(Root() == root);
  assert(kProtocolVersion == 2);
  assert(SystemDirectory() == root + "/Settings");
  assert(SystemSettingsPath() == root + "/Settings/system.cfg");
  assert(SharedDirectory() == root && SharedSamplesDirectory() == root + "/Samples");
  assert(LogDirectory() == root + "/Logs" && AppsDirectory() == root + "/Apps");
  const AppFolder synth("Cardenza/Synth");
  assert(synth.Valid() && synth.Id() == "Cardenza/Synth");
  assert(synth.Root() == root);
  assert(synth.SettingsPath() == root + "/Settings/synth.cfg");
  assert(synth.Projects() == root + "/Projects");
  assert(synth.Presets("DX7") == root + "/Presets/DX7");
  assert(synth.Session() == root + "/Session");
  assert(synth.Folder("Wavetables") == root + "/Wavetables");
  assert(synth.Folder("..").empty() && synth.Folder("a/b").empty() && synth.Presets("").empty());
  for (const char* bad : {"", "Synth", "a/b/c", "../x", "a/..", "a b/c", "a/", "/a", "Cardenza/Synth!",
                          "Author/ThisNameIsLongerThan24Chars"})
  {
    assert(!IsValidAppId(bad));
    assert(!AppFolder(bad).Valid() && AppFolder(bad).Root().empty() && AppFolder(bad).Projects().empty());
  }
  assert(IsValidAppId("me/My-App_2"));

  assert(CardPresent());
  assert(EnsureAppLayout(synth, {"DX7", "Juno"}));
  for (const char* folder : {"Settings", "Samples", "Projects", "Presets/DX7", "Presets/Juno", "Session"})
    assert(fs::is_directory(fs::path(root) / folder));
  assert(!fs::exists(fs::path(root) / "Apps") && !fs::exists(Card() / "Cardenza"));
  const AppFolder other("Other/Tool");
  assert(other.Root() == root + "/Apps/Other/Tool");
  assert(other.SettingsPath() == root + "/Apps/Other/Tool/Settings/app.cfg");
  assert(EnsureAppLayout(other, {}));
  assert(other.Projects() != synth.Projects() && other.Session() != synth.Session());
  assert(!EnsureAppLayout(AppFolder("bad id"), {}));
  assert(!EnsureAppLayout(synth, {"../escape"}));
  std::cout << "app paths passed\n";
}

void TestSettingsFormat()
{
  std::size_t rejected = 0;
  const auto entries = ParseSettings(
      "# comment\r\n"
      "  keys.scale = 3 \r\n"
      "\n"
      "no equals sign\n"
      "Upper=1\n"
      "bad key=1\n"
      "_hidden=1\n"
      "value.tab=a\tb\n"
      "future.key=kept as is\n"
      "keys.scale=4\n"
      "empty=\n"
      "url=http://x/#anchor\n",
      &rejected);
  assert(rejected == 5);
  assert(entries.size() == 4);
  assert(entries[0].first == "keys.scale" && entries[0].second == "4");  // first position, last value
  assert(entries[1].first == "future.key" && entries[1].second == "kept as is");
  assert(entries[2].first == "empty" && entries[2].second.empty());
  assert(entries[3].second == "http://x/#anchor");
  const std::string text = FormatSettings(entries, "line one\nline two");
  assert(text == "# line one\n# line two\nkeys.scale=4\nfuture.key=kept as is\nempty=\nurl=http://x/#anchor\n");
  assert(ParseSettings(text) == entries);
  assert(IsValidSettingsKey("arp.latch") && !IsValidSettingsKey("Arp") && !IsValidSettingsKey(".x") &&
         !IsValidSettingsKey(std::string(kMaxSettingsKey + 1, 'a')));
  assert(IsValidSettingsValue("Cardenza/Synth") && !IsValidSettingsValue(" x") && !IsValidSettingsValue("a\nb") &&
         !IsValidSettingsValue(std::string(kMaxSettingsValue + 1, 'a')));
  std::cout << "settings format passed\n";
}

void TestSettingsRoundTrip()
{
  ResetCard();
  const AppFolder app("Test/RoundTrip");
  Settings settings(app.Id(), app.SettingsPath());
  settings.Load();
  assert(settings.Entries().empty() && !settings.Dirty());
  assert(settings.GetInt("missing", 7) == 7 && settings.GetBool("missing", true) && settings.GetString("missing", "x") == "x");
  assert(settings.SetInt("volume", 42) && settings.SetBool("latch", true) && settings.SetString("last", "Cardenza/Synth"));
  assert(!settings.SetString("Bad Key", "1") && !settings.SetString("ok", "line\nbreak"));
  const std::uint32_t revision = settings.Revision();
  assert(settings.SetInt("volume", 42) && settings.Revision() == revision);  // unchanged value: not dirty again
  assert(settings.Dirty() && settings.Flush() && !settings.Dirty());
  // The app folder is created on demand.
  assert(fs::is_regular_file(app.SettingsPath()));
  const std::string file = ReadText(app.SettingsPath());
  assert(file.find("# Cardenza settings: Test/RoundTrip (storage protocol 2)\n") == 0);
  assert(file.find("\nvolume=42\nlatch=1\nlast=Cardenza/Synth\n") != std::string::npos);
  assert(!fs::exists(app.SettingsPath() + ".tmp") && !fs::exists(app.SettingsPath() + ".bak"));

  // Unknown keys and hand edits survive a load / change / flush cycle; bad lines are dropped.
  WriteText(app.SettingsPath(), file + "from.newer.firmware=yes\nthis line is broken\nvolume = 150\n");
  Settings reloaded(app.Id(), app.SettingsPath());
  reloaded.Load();
  assert(reloaded.GetInt("volume", 5) == 150 && reloaded.GetInt("volume", 5, 0, 100) == 5);  // out of range -> fallback
  assert(reloaded.GetBool("latch", false) && reloaded.GetString("last") == "Cardenza/Synth");
  assert(reloaded.GetBool("from.newer.firmware", false));
  assert(reloaded.SetInt("volume", 99) && reloaded.Flush());
  const std::string rewritten = ReadText(app.SettingsPath());
  assert(rewritten.find("from.newer.firmware=yes\n") != std::string::npos && rewritten.find("broken") == std::string::npos &&
         rewritten.find("volume=99\n") != std::string::npos);
  assert(reloaded.Remove("latch") && reloaded.Flush() && ReadText(app.SettingsPath()).find("latch=") == std::string::npos);
  // Not numbers / not booleans fall back.
  assert(reloaded.SetString("word", "abc") && reloaded.GetInt("word", -1) == -1 && reloaded.GetBool("word", true));
  assert(reloaded.SetString("big", "99999999999") && reloaded.GetInt("big", -1) == -1);
  assert(reloaded.SetString("yes", "On") && reloaded.GetBool("yes", false));
  std::cout << "settings round trip passed\n";
}

void TestAtomicWriteRecovery()
{
  ResetCard();
  assert(EnsureSystemLayout());
  const std::string path = (fs::path(SystemDirectory()) / "atomic.cfg").generic_string();
  assert(WriteFileAtomic(path, "first\n") && ReadText(path) == "first\n");
  assert(WriteFileAtomic(path, "second\n") && ReadText(path) == "second\n");
  assert(!fs::exists(path + ".bak") && !fs::exists(path + ".tmp"));
  // Interrupted between "old -> .bak" and ".tmp -> new": only the backup and a temporary are left.
  fs::rename(path, path + ".bak");
  WriteText(path + ".tmp", "half writ");
  std::string text;
  assert(ReadFileWithBackup(path, text, 1024) && text == "second\n");
  assert(!ReadFileWithBackup(path, text, 3));  // too large
  // The next write replaces the leftovers and removes the stale backup.
  assert(WriteFileAtomic(path, "third\n") && ReadText(path) == "third\n");
  assert(!fs::exists(path + ".bak") && !fs::exists(path + ".tmp"));
  assert(!WriteFileAtomic((fs::path(Root()) / "NoSuchFolder" / "x.cfg").generic_string(), "x"));

  // A settings file that only exists as a backup still loads, and the next flush restores it.
  const std::string settingsPath = SystemSettingsPath();
  assert(WriteFileAtomic(settingsPath, "protocol=2\nsys.volume=33\n"));
  fs::rename(settingsPath, settingsPath + ".bak");
  ReloadAllSettings();
  Settings& system = SystemSettings();
  assert(system.GetInt("sys.volume", 0) == 33);
  assert(system.SetInt("sys.led", 9) && FlushAllSettings());
  assert(fs::is_regular_file(settingsPath) && !fs::exists(settingsPath + ".bak"));
  assert(ReadText(settingsPath).find("sys.volume=33\nsys.led=9\n") != std::string::npos);
  std::cout << "atomic write and backup recovery passed\n";
}

void TestMissingCardRamOnly()
{
  ResetCard();
  Settings& system = SystemSettings();
  assert(system.SetInt("sys.volume", 50) && system.SetString("future.key", "retained") && FlushAllSettings());
  const std::string settingsPath = SystemSettingsPath();
  assert(ReadText(settingsPath).find("sys.volume=50\n") != std::string::npos);

  // Card out: changes stay dirty in RAM; there is no internal persistence or successful save.
  CardenzaSim::Input::SetCardInserted(false);
  assert(!CardPresent() && !CardMounted() && CardFreeBytes() == 0 && !EnsureSystemLayout());
  assert(system.SetInt("sys.volume", 70) && system.SetString("boot.app", "Cardenza/Synth"));
  assert(AnySettingsDirty() && !FlushAllSettings() && system.Dirty());
  assert(!fs::exists(Internal()));
  assert(ReadText(settingsPath).find("sys.volume=50\n") != std::string::npos);  // card untouched
  // A fresh store while the card is absent uses defaults, proving unsaved edits were RAM-only.
  Settings restarted("system", settingsPath);
  restarted.Load();
  assert(restarted.GetInt("sys.volume", 85) == 85 && !restarted.Has("boot.app"));
  assert(!FlushAllSettings() && AnySettingsDirty());
  Settings& app = AppSettings("Test/RamOnly");
  assert(app.SetInt("keys.octave", 2) && !app.Flush() && app.Dirty());
  const std::string appPath = AppFolder("Test/RamOnly").SettingsPath();
  // Model existing files on a card inserted later, including keys never loaded into this cache.
  WriteText(appPath, "keys.octave=0\nkeys.scale=3\nfuture.app=keep\n");
  WriteText(settingsPath, ReadText(settingsPath) + "future.inserted=keep too\n");

  // No ReloadAllSettings: insertion must preserve RAM edits and merge untouched card keys.
  CardenzaSim::Input::SetCardInserted(true);
  assert(RefreshAllSettingsFromCard() && system.Dirty() && app.Dirty());
  assert(system.GetInt("sys.volume", 0) == 70 && system.GetString("future.key") == "retained" &&
         system.GetString("future.inserted") == "keep too");
  assert(app.GetInt("keys.octave", 0) == 2 && app.GetInt("keys.scale", 0) == 3 && app.GetString("future.app") == "keep");
  assert(FlushAllSettings() && !AnySettingsDirty());
  const std::string synced = ReadText(settingsPath);
  assert(synced.find("sys.volume=70\n") != std::string::npos && synced.find("boot.app=Cardenza/Synth\n") != std::string::npos &&
         synced.find("future.inserted=keep too\n") != std::string::npos);
  assert(ReadText(appPath).find("keys.octave=2\n") != std::string::npos && ReadText(appPath).find("future.app=keep\n") != std::string::npos);
  assert(!fs::exists(Internal()));
  ReleaseAppSettings("Test/RamOnly");
  std::cout << "missing card RAM-only settings and late insertion passed\n";
}

void TestDeviceSettings()
{
  ResetCard();
  ApplyDeviceSettings();
  assert(OutputVolume() == kDefaultOutputVolume && KeyLedBrightness() == kDefaultKeyLedBrightness &&
         ScreenBacklight() == kDefaultScreenBacklight);
  assert(ledMaster == kDefaultKeyLedBrightness && backlight == kDefaultScreenBacklight);
  SetOutputVolume(140);  // clamped
  SetKeyLedBrightness(9);
  SetScreenBacklight(3);  // never fully dark
  assert(OutputVolume() == 100 && outputVolume == 100 && KeyLedBrightness() == 9 && ledMaster == 9);
  assert(ScreenBacklight() == kMinScreenBacklight && backlight == kMinScreenBacklight);
  SetOutputVolume(50);
  assert(outputVolume == 50);  // as it is: the device puts the curve on it (Device::Audio::SetOutputVolume)
  SetOutputVolume(0);
  assert(outputVolume == 0);
  SetOutputVolume(60);
  assert(FlushAllSettings());
  const std::string text = ReadText(SystemSettingsPath());
  assert(text.find("sys.volume=60\n") != std::string::npos && text.find("sys.led=9\n") != std::string::npos &&
         text.find("sys.screen=10\n") != std::string::npos);
  // Boot: the values come back from the card and go to the device.
  ledMaster = backlight = outputVolume = 0;
  ReloadAllSettings();
  ApplyDeviceSettings();
  assert(OutputVolume() == 60 && ledMaster == 9 && backlight == kMinScreenBacklight && outputVolume == 60);
  std::cout << "device settings passed\n";
}

void TestScreenTheme()
{
  // sys.theme: a device setting stored by name, DARK unless someone picks another.
  ResetCard();
  ApplyDeviceSettings();
  assert(ScreenTheme() == Theme::Dark && kDefaultTheme == Theme::Dark && !SystemSettings().Has("sys.theme"));
  SetScreenTheme(Theme::Light);
  assert(ScreenTheme() == Theme::Light);
  assert(FlushAllSettings() && ReadText(SystemSettingsPath()).find("sys.theme=light\n") != std::string::npos);
  // It comes back from the card at the next start.
  ReloadAllSettings();
  ApplyDeviceSettings();
  assert(ScreenTheme() == Theme::Light);
  SetScreenTheme(Theme::Retro);
  assert(FlushAllSettings() && ReadText(SystemSettingsPath()).find("sys.theme=retro\n") != std::string::npos);
  // Read tolerantly: a name in any case; anything else (a number, an unknown name) is DARK.
  const auto readBack = [](const std::string& value) {
    WriteText(SystemSettingsPath(), "sys.theme=" + value + "\n");
    ReloadAllSettings();
    ApplyDeviceSettings();
    return ScreenTheme();
  };
  assert(readBack("Light") == Theme::Light && readBack("RETRO") == Theme::Retro && readBack("dark") == Theme::Dark);
  assert(readBack("sepia") == Theme::Dark && readBack("2") == Theme::Dark && readBack("") == Theme::Dark);
  // A value outside the list is stored as the default.
  SetScreenTheme(static_cast<Theme>(7));
  assert(ScreenTheme() == Theme::Dark && SystemSettings().GetString("sys.theme") == "dark");
  std::cout << "screen theme passed\n";
}

void TestLegacyLayoutsRemainUntouched()
{
  ResetCard();
  const AppFolder synth("Cardenza/Synth");
  const fs::path original = Card() / "CardenzaSynth";
  const fs::path previous = Card() / "Cardenza";
  const fs::path oldSynth = previous / "Apps" / "Cardenza" / "Synth";
  const std::vector<std::pair<fs::path, std::string>> legacyFiles{
      {original / "Project" / "ORIGINAL.ccproj", "original project bytes"},
      {original / "Preset" / "DX7" / "original.ccpreset", "original preset bytes"},
      {original / "Sample" / "original.wav", "original sample bytes"},
      {original / "notes.txt", "user notes"},
      {previous / "System" / "settings.cfg", "protocol=1\nsys.volume=61\nfuture.system=untouched\n"},
      {previous / "System" / "settings.cfg.bak", "protocol=1\nsys.volume=62\n"},
      {oldSynth / "settings.cfg", "keys.octave=2\nfuture.synth=untouched\n"},
      {oldSynth / "settings.cfg.tmp", "uncommitted bytes"},
      {oldSynth / "Projects" / "PREVIOUS.ccproj", "previous project bytes"},
      {oldSynth / "Projects" / "RECOVERY.ccproj.bak", "previous recovery bytes"},
      {oldSynth / "Presets" / "Juno" / "previous.ccpreset", "previous preset bytes"},
      {oldSynth / "Session" / "session.cfg", "pending=1\nname=PREVIOUS\n"},
      {previous / "Shared" / "Samples" / "previous.wav", "previous sample bytes"},
      {previous / "Apps" / "Other" / "Tool" / "settings.cfg", "other.option=untouched\n"},
  };
  for (const auto& file : legacyFiles) WriteText(file.first, file.second);
  assert(EnsureAppLayout(synth, {"DX7", "Juno"}));
  Settings& system = SystemSettings();
  Settings& app = AppSettings(synth.Id());
  assert(system.GetInt("sys.volume", 85) == 85 && !system.Has("future.system"));
  assert(app.GetInt("keys.octave", 0) == 0 && !app.Has("future.synth"));
  assert(system.SetInt("sys.volume", 42) && app.SetInt("keys.octave", 1) && FlushAllSettings());
  assert(ReadText(SystemSettingsPath()).find("sys.volume=42\n") != std::string::npos);
  assert(ReadText(SystemSettingsPath()).find("future.system") == std::string::npos);
  assert(ReadText(synth.SettingsPath()).find("keys.octave=1\n") != std::string::npos);
  assert(ReadText(synth.SettingsPath()).find("future.synth") == std::string::npos);
  // The ordinary new-layout APIs neither discover nor move documents from either older layout.
  assert(ListDirectory(synth.Projects()).empty() && ListDirectory(SharedSamplesDirectory()).empty());
  assert(ListDirectory(synth.Presets("DX7")).empty() && ListDirectory(synth.Presets("Juno")).empty());
  assert(ListDirectory(synth.Session()).empty());
  for (const auto& file : legacyFiles) assert(ReadText(file.first) == file.second && fs::is_regular_file(file.first));
  assert(!fs::exists(Internal()));
  ReleaseAppSettings(synth.Id());
  std::cout << "legacy layouts and file bytes remain untouched\n";
}

// A run that starts without a card makes no card operation until the next start, a card put in
// meanwhile included: the system decides it once (os::system::Begin calls DecideCardAtStart).
void TestRunWithoutCard()
{
  ResetCard();
  assert(EnsureSystemLayout());
  const std::string settingsPath = SystemSettingsPath();
  WriteText(settingsPath, "protocol=2\nsys.volume=47\n");
  CardenzaSim::Input::SetCardInserted(false);
  assert(!DecideCardAtStart() && CardMissingThisRun());
  CardenzaSim::Input::SetCardInserted(true);
  assert(!CardPresent() && !CardMounted() && CardCapacityBytes() == 0 && !EnsureSystemLayout());
  assert(!Exists(settingsPath) && !IsDirectory(Root()) && ListDirectory(SystemDirectory()).empty());
  std::string text;
  assert(!ReadFileWithBackup(settingsPath, text, 1024) && !WriteFileAtomic(settingsPath, "x=1\n"));
  assert(!CreateDirectories(JoinPath(Root(), "New")) && !RemoveFile(settingsPath));
  const AppFolder synth("Cardenza/Synth");
  assert(!EnsureAppLayout(synth, {"DX7"}));
  // The settings start from their defaults and stay in RAM.
  ReloadAllSettings();
  Settings& system = SystemSettings();
  assert(system.GetInt("sys.volume", 85) == 85 && system.SetInt("sys.volume", 30));
  assert(!RefreshAllSettingsFromCard() && !FlushAllSettings() && AnySettingsDirty() && system.GetInt("sys.volume", 0) == 30);
  ReportCardIoError();
  assert(ReadText(settingsPath) == "protocol=2\nsys.volume=47\n");
  assert(!fs::exists(fs::path(Root()) / "New") && !fs::exists(settingsPath + ".tmp") && !fs::exists(synth.Presets("DX7")));
  // The next start with the card reads it again.
  assert(DecideCardAtStart() && !CardMissingThisRun());
  ReloadAllSettings();
  assert(SystemSettings().GetInt("sys.volume", 0) == 47);
  std::cout << "run without a card passed\n";
}

}  // namespace

int main()
{
  // The simulator fixes its card folder under the working directory on first use.
  const auto root = fs::temp_directory_path() /
                    ("cardenza-storage-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  fs::create_directories(root);
  fs::current_path(root);
  assert(fs::path(Device::Storage::RootPath()).filename() == "sdcard");
  if (!fs::equivalent(Card().parent_path(), root))
  {
    std::cerr << "Storage tests require an isolated card under their temporary working directory\n";
    return 1;
  }

  TestAppPaths();
  TestSettingsFormat();
  TestSettingsRoundTrip();
  TestAtomicWriteRecovery();
  TestMissingCardRamOnly();
  TestDeviceSettings();
  TestScreenTheme();
  TestLegacyLayoutsRemainUntouched();
  TestRunWithoutCard();
  assert(!fs::exists(Internal()));

  fs::current_path(fs::temp_directory_path());
  fs::remove_all(root);
  std::cout << "Storage protocol passed\n";
  return 0;
}
