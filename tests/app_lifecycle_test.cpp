// App lifecycle on the real system, storage on the SD card only (protocol 2, docs/app-storage-protocol.md):
// boot on a card the 0.1.0 release wrote, change device settings, play settings and the session in the
// groovebox, keep the working state on the card through power cuts, card trouble and app switches, and
// check where everything was stored. `--no-card`: a start without a card, which touches no card for the
// whole run and says so.
#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <string>
#include <vector>

#include "Applications/Application.h"
#include "Applications/Synth/GrooveApp.h"
#include "Applications/Synth/Runtime/Sampler/SamplerWav.h"
#include "Applications/Synth/UI/Groove/Render/Animator.h"
#include "Devices/CardenzaSim/SimulatorInput.h"
#include "Devices/Device.h"
#include "OS/Storage/DeviceSettings.h"
#include "OS/Storage/Settings.h"
#include "OS/Storage/Storage.h"
#include "OS/System/Log.h"
#include "OS/System/System.h"

namespace fs = std::filesystem;

namespace {

std::uint32_t gNow = 10000;
std::uint32_t FakeClock() { return gNow; }

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

bool Contains(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

// What a power cut would leave on the card right now, and that state being what the next start finds.
void CopyFolder(const fs::path& from, const fs::path& to)
{
  fs::remove_all(to);
  fs::create_directories(to.parent_path());
  fs::copy(from, to, fs::copy_options::recursive);
}

// When each file in a folder was last written.
std::map<std::string, fs::file_time_type> WriteTimes(const fs::path& folder)
{
  std::map<std::string, fs::file_time_type> times;
  for (const auto& entry : fs::directory_iterator(folder)) times[entry.path().filename().string()] = entry.last_write_time();
  return times;
}

// Every file below a folder with what it holds.
std::map<std::string, std::string> Snapshot(const fs::path& folder)
{
  std::map<std::string, std::string> files;
  if (!fs::exists(folder)) return files;
  for (const auto& entry : fs::recursive_directory_iterator(folder))
    files[fs::relative(entry.path(), folder).generic_string()] = entry.is_regular_file() ? ReadText(entry.path()) : std::string("<dir>");
  return files;
}

void Frames(int count = 1)
{
  for (int index = 0; index < count; ++index)
  {
    gNow += 16;
    os::system::RunFrame();
  }
}

void Key(std::size_t row, std::size_t column, bool pressed)
{
  CardenzaSim::Input::SetMatrixKey(row, column, pressed);
  Frames();
}

void Tap(std::size_t row, std::size_t column)
{
  Key(row, column, true);
  Key(row, column, false);
}

apps::GrooveApp& Groove()
{
  auto* groove = dynamic_cast<apps::GrooveApp*>(os::system::ActiveApplication());
  assert(groove != nullptr);
  groove->UseClock(&FakeClock);
  return *groove;
}

// A second app to switch to; a system app, so it is never remembered as the boot app.
class BlankApp : public app::Application {
public:
  void Setup(const std::vector<std::string>&) override {}
  void Tick() override {}
  void RenderFrame() override {}
};

constexpr const char* kBlankId = "Test/Blank";

void RegisterBlankApp()
{
  app::ApplicationInfo blank;
  blank.id = kBlankId;
  blank.name = "Blank";
  blank.author = "Test";
  blank.isSystem = true;
  blank.factory = &app::MakeApplication<BlankApp>;
  assert(app::RegisterApplication(blank));
}

void SwitchTo(const std::string& id)
{
  assert(os::system::SwitchApplication(id));
  Frames();
  assert(os::system::ActiveApplicationId() == id);
}

// FN + S: the project saved under its name, or the first free SONG n.
void QuickSave()
{
  Key(2, 0, true);
  Tap(2, 3);
  Key(2, 0, false);
}

// No card at the start: the whole run makes no card operation, even after a card goes in, and the
// screen says that nothing will be saved.
void TestNoCardBoot(const fs::path& root, bool volatileSession = false)
{
  const fs::path card = root / "sdcard";
  const fs::path product = card / "CardenzaSynth";
  // A card with settings, a project and an autosave: none of it may be read or changed.
  WriteText(product / "Settings" / "system.cfg", "protocol=2\nsys.volume=47\nsys.volume.rev=2\nsys.led=5\nboot.app=Cardenza/Synth\n");
  WriteText(product / "Settings" / "synth.cfg", "keys.select=1\n");
  WriteText(product / "Projects" / "ON CARD.ccproj", "CARDENZA_PROJECT 4\n");
  WriteText(product / "Session" / "autosave.txt", "generation=3\nname=ON CARD\ndirty=0\n");
  WriteText(product / "Session" / "autosave.ccproj", "CARDENZA_PROJECT 7\n");
  const auto before = Snapshot(card);

  CardenzaSim::Input::SetCardInserted(false);
  RegisterBlankApp();
  os::system::UseClock(&FakeClock);
  os::system::BootOptions boot;
  if (volatileSession) boot.applicationArgs.emplace_back("--volatile-session");
  assert(os::system::Begin(boot));
  assert(os::storage::CardMissingThisRun() && !os::storage::CardPresent() && !os::storage::CardMounted());
  auto* groove = &Groove();
  // The defaults, not the card's.
  assert(groove->MasterVolume() == os::storage::kDefaultOutputVolume && groove->LedBrightness() == os::storage::kDefaultKeyLedBrightness);
  assert(!groove->Ui().Settings().keyboard.multiSelect && groove->Ui().State().projectName.empty());
  assert(!groove->QuerySystem().storageReady);

  // A deliberate volatile host with no keyboard can show the original UI
  // without a modal to dismiss. It must still refuse storage for the whole run.
  if (volatileSession)
  {
    assert(groove->Ui().TopModal() == nullptr);
    Frames(static_cast<int>(groove::kBootMs / 16) + 120);
    assert(groove->Ui().TopModal() == nullptr && os::storage::CardMissingThisRun());
    CardenzaSim::Input::SetCardInserted(true);
    Frames(700);
    assert(!os::storage::CardMounted());
    assert(groove->ListProjects().empty());
    groove->FlushSession();
    os::system::Shutdown();
    assert(Snapshot(card) == before && !fs::exists(root / "internal"));
    std::cout << "Volatile no-card boot passed\n";
    return;
  }

  // The notice is there at the start and still there after the boot animation, until ENTER.
  const auto notice = [&]() {
    const groove::Modal* modal = groove->Ui().TopModal();
    return modal != nullptr && modal->kind == groove::ModalKind::Confirm && modal->confirmPurpose == groove::ConfirmPurpose::Notice &&
           modal->title == "NO SD CARD" && Contains(modal->detail, "NOTHING WILL BE SAVED");
  };
  assert(notice());
  Frames(static_cast<int>(groove::kBootMs / 16) + 120);
  assert(notice());

  // A card put in now changes nothing until the next start.
  CardenzaSim::Input::SetCardInserted(true);
  Frames(700);
  assert(!os::storage::CardPresent() && !os::storage::CardMounted() && !groove->QuerySystem().storageReady);
  assert(notice());
  Tap(2, 13);  // ENTER: read
  assert(groove->Ui().TopModal() == nullptr);

  // Nothing on the card is listed or loaded, and saving says why it cannot.
  assert(groove->ListProjects().empty() && groove->ListSamples().empty());
  std::string message;
  groove::ProjectUiSettings ui;
  assert(!groove->LoadProject("ON CARD", ui, message) && message == "NO SD CARD");
  QuickSave();
  assert(groove->Ui().State().toast.text == "NO SD CARD" && groove->Ui().State().projectName.empty());
  // The guide has nowhere to keep the working state.
  CardenzaSim::Input::SendComputerApp("GE");
  Frames(2);
  std::string reply, said;
  while (CardenzaSim::Input::TakeDeviceApp(said)) reply += said + "|";
  assert(Contains(reply, "GE0 card"));

  // Work as usual: settings and the session stay in RAM, nothing is written, no log either.
  groove->SetMasterVolume(70);
  groove->SetLedBrightness(4);
  groove->Sequencer().SetTempo(133);
  for (int beat = 0; beat < 10; ++beat)
  {
    Tap(1, 1);
    Frames(20);
  }
  Device::Log::Write("test: no card");
  gNow += 70000;
  Frames(400);
  groove->FlushSession();
  assert(os::storage::AnySettingsDirty() && !os::log::Flush());
  assert(groove->MasterVolume() == 70 && groove->LedBrightness() == 4 && groove->Sequencer().Tempo() == 133);
  // An app switch and back keeps the settings in RAM; the working state has nowhere to wait.
  SwitchTo(kBlankId);
  SwitchTo("Cardenza/Synth");
  groove = &Groove();
  assert(groove->MasterVolume() == 70 && os::storage::AnySettingsDirty());
  assert(notice());
  os::system::Shutdown();
  assert(os::system::ActiveApplication() == nullptr);

  assert(Snapshot(card) == before && !fs::exists(root / "internal"));
  std::cout << "No-card lifecycle passed\n";
}

}  // namespace

int main(int argc, char** argv)
{
  const auto root = fs::temp_directory_path() /
                    ("cardenza-lifecycle-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  fs::create_directories(root);
  fs::current_path(root);
  if (!fs::equivalent(fs::path(Device::Storage::RootPath()).parent_path(), root))
  {
    std::cerr << "Lifecycle tests require an isolated card under their temporary working directory\n";
    return 1;
  }
  if (argc == 2 && (std::string(argv[1]) == "--no-card" || std::string(argv[1]) == "--volatile-no-card"))
  {
    TestNoCardBoot(root, std::string(argv[1]) == "--volatile-no-card");
    fs::current_path(fs::temp_directory_path());
    fs::remove_all(root);
    return 0;
  }
  assert(argc == 1);
  const fs::path card = root / "sdcard";
  const fs::path product = card / "CardenzaSynth";
  const fs::path systemSettings = product / "Settings" / "system.cfg";
  const fs::path synthSettings = product / "Settings" / "synth.cfg";
  const fs::path sessionFolder = product / "Session";

  // A card the 0.1.0 release wrote: its files are read where they are.
  WriteText(product / "Samples" / "kick.wav", "RIFF not really a wave");
  WriteText(product / "Projects" / "OLD SONG.ccproj", "CARDENZA_PROJECT 4\n");
  // Its volume was on another curve (no sys.volume.rev): everyone starts again from the default, once.
  WriteText(systemSettings, "protocol=2\nsys.volume=85\nsys.led=9\nfuture.system=keep\n");
  WriteText(synthSettings, "keys.select=1\nfuture.synth=keep\n");
  // What development builds wrote in between (protocol 1, Cardenza/): neither read nor changed.
  const fs::path between = card / "Cardenza";
  WriteText(between / "System" / "settings.cfg", "protocol=1\nsys.led=3\n");
  WriteText(between / "Apps" / "Cardenza" / "Synth" / "Projects" / "BETWEEN.ccproj", "CARDENZA_PROJECT 7\n");
  const auto betweenFiles = Snapshot(between);

  RegisterBlankApp();
  os::system::UseClock(&FakeClock);
  assert(os::system::Begin());
  assert(!os::storage::CardMissingThisRun() && os::storage::CardMounted());
  assert(os::system::ActiveApplicationId() == "Cardenza/Synth");
  auto* groove = &Groove();
  assert(groove->Ui().TopModal() == nullptr);  // no notice with a card
  assert(groove->MasterVolume() == 70 && os::storage::kDefaultOutputVolume == 70);
  assert(groove->LedBrightness() == 9 && groove->Ui().Settings().keyboard.multiSelect);

  // The release's project and sample are there; the folders in between are not read.
  const auto projects = groove->ListProjects();
  assert(std::find(projects.begin(), projects.end(), "OLD SONG") != projects.end());
  assert(std::find(projects.begin(), projects.end(), "BETWEEN") == projects.end());
  const auto samples = groove->ListSamples();
  assert(std::find(samples.begin(), samples.end(), "kick.wav") != samples.end());
  std::string text = ReadText(systemSettings);
  assert(Contains(text, "protocol=2\n") && Contains(text, "future.system=keep\n") && Contains(text, "boot.app=Cardenza/Synth\n"));
  for (const char* folder : {"Settings", "Samples", "Projects", "Presets/DX7", "Presets/Drums", "Session"}) assert(fs::is_directory(product / folder));
  assert(!fs::exists(product / "Apps") && !fs::exists(root / "internal"));

  // The console log (OS/System/Log.h): on by default, written to CardenzaSynth/Logs every few seconds.
  const fs::path logFile = product / "Logs" / "log.txt";
  assert(os::storage::LogEnabled() && os::log::FilePath() == logFile.generic_string());
  gNow += os::log::kWriteIntervalMs;
  Frames(1);
  Device::Log::Write("test: first line");
  Frames(1);
  assert(!Contains(ReadText(logFile), "test: first line"));  // not every frame
  gNow += os::log::kWriteIntervalMs;
  Frames(1);
  assert(Contains(ReadText(logFile), "test: first line\n") && Device::Log::Pending() == 0);
  // Off: nothing goes to the card, the newest lines wait in the ring and come when it is on again.
  groove->SetLogging(false);
  assert(!os::storage::LogEnabled());
  Device::Log::Write("test: while off");
  gNow += os::log::kWriteIntervalMs;
  Frames(1);
  assert(!Contains(ReadText(logFile), "while off") && !os::log::Flush());
  groove->SetLogging(true);
  assert(os::log::Flush() && Contains(ReadText(logFile), "test: while off\n"));
  // More than the ring holds before a write: the file says what is missing.
  for (int line = 0; line < 2000; ++line) Device::Log::Write(std::string(60, 'x'));
  assert(os::log::Flush() && Contains(ReadText(logFile), "bytes lost here]"));
  // Past its size the file becomes log.1.txt and a new one starts.
  for (int round = 0; round < 12 && !fs::exists(product / "Logs" / "log.1.txt"); ++round)
  {
    for (int line = 0; line < 900; ++line) Device::Log::Write(std::string(60, 'y'));
    // One write takes a bounded amount: the rest waits for the next.
    while (Device::Log::Pending() > 0) assert(os::log::Flush());
  }
  assert(fs::exists(product / "Logs" / "log.1.txt") && fs::file_size(product / "Logs" / "log.1.txt") > static_cast<std::uintmax_t>(os::log::kMaxFileBytes));
  Device::Log::Write("test: after the rotation");
  assert(os::log::Flush() && Contains(ReadText(logFile), "after the rotation") &&
         fs::file_size(logFile) < static_cast<std::uintmax_t>(os::log::kMaxFileBytes));

  // Device settings, play settings and the session.
  groove->SetMasterVolume(60);
  groove->SetLedBrightness(7);
  groove->SetBacklight(150);
  assert(Device::LED::GlobalBrightness() == 7 && Device::Display::Backlight() == 150);
  groove->Sequencer().SetTempo(133);
  const std::uint8_t note = 64;
  groove->Sequencer().PressStep(3, &note, 1);
  groove->Sequencer().ReleaseStep(3);
  synth::ArpSettings arp;
  arp.mode = synth::ArpMode::Up;
  arp.rate = 2;
  arp.gate = 70;
  groove->Sequencer().SetArpSettings(synth::kDefaultActiveTrack, arp);  // T1's arpeggiator
  Key(0, 0, true);  // ESC + GRID column 1: SETUP
  Tap(0, 9);
  Key(0, 0, false);
  assert(groove->Ui().State().scene == groove::Scene::Setup);
  while (groove->Ui().CurrentPage() != groove::kSetupKeys) Tap(3, 0);  // CTRL to KEYS
  Tap(0, 12);  // OCTAVE
  Tap(0, 13);  // ADD
  Tap(0, 0);   // ESC: back
  assert(groove->Ui().State().keyboard.octave == 1 && groove->Ui().State().scene != groove::Scene::Setup);
  // Fn + S on an unnamed project saves it right away as the first free SONG n (no name field).
  QuickSave();
  assert(groove->Ui().State().projectName == "SONG 01");
  assert(fs::is_regular_file(product / "Projects" / "SONG 01.ccproj"));
  // Changes after the save live only in the session.
  groove->SetTrackLevel(1, 42);

  // Settings are written a second after the last change.
  Frames(70);
  text = ReadText(systemSettings);
  assert(Contains(text, "sys.volume=60\n") && Contains(text, "sys.led=7\n") && Contains(text, "sys.screen=150\n"));
  assert(Contains(text, "sys.volume.rev=3\n") && Contains(text, "future.system=keep\n"));
  // The play settings are project data (since format 6): the keyboard and each track's chord and arp.
  text = ReadText(synthSettings);
  assert(Contains(text, "future.synth=keep\n") && !Contains(text, "arp.") && !Contains(text, "keys.octave"));
  text = ReadText(product / "Projects" / "SONG 01.ccproj");
  assert(text.find("CARDENZA_PROJECT 7\n") == 0 && Contains(text, "\nUI 0 1 0 0 0 ") && Contains(text, "\nKEYS 0 110\n"));
  assert(Contains(text, "\nPLAYFX 0 0 0 1 2 1 70 0\n") && Contains(text, "\nPLAYFX 1 0 0 0 3 1 50 0\n"));

  // A short HOME press does nothing in the groovebox: the key is unassigned for now.
  const std::uint32_t panic = groove->Ui().State().panicAt;
  CardenzaSim::Input::SetHomeButton(true);
  Frames();
  CardenzaSim::Input::SetHomeButton(false);
  Frames();
  assert(os::system::ActiveApplicationId() == "Cardenza/Synth" && groove->Ui().State().panicAt == panic);

  // Holding HOME stays with the app: this firmware has no launcher of its own.
  CardenzaSim::Input::SetHomeButton(true);
  Frames(80);
  CardenzaSim::Input::SetHomeButton(false);
  Frames();
  assert(os::system::ActiveApplicationId() == "Cardenza/Synth");
  groove = &Groove();

  // Another app: the groovebox is suspended (the autosave written) and destroyed.
  SwitchTo(kBlankId);
  const fs::path autosave = sessionFolder / "autosave.ccproj";
  const fs::path autosaveInfo = sessionFolder / "autosave.txt";
  assert(Contains(ReadText(autosaveInfo), "name=SONG 01\n") && Contains(ReadText(autosave), "\nSEQ 133 "));
  // A system app does not change the boot app.
  assert(Contains(ReadText(systemSettings), "boot.app=Cardenza/Synth\n"));

  // Back to the groovebox, as it was.
  SwitchTo("Cardenza/Synth");
  groove = &Groove();
  assert(groove->Sequencer().Tempo() == 133 && groove->Sequencer().BuildUiState(synth::kDefaultActiveTrack).steps[3].hasEvents);
  assert(groove->TrackLevel(1) == 42 && groove->Ui().State().projectName == "SONG 01");
  assert(groove->MasterVolume() == 60 && groove->LedBrightness() == 7 && groove->Backlight() == 150);
  assert(groove->Ui().State().keyboard.octave == 1 && groove->Sequencer().GetArpSettings(synth::kDefaultActiveTrack) == arp);
  assert(groove->Sequencer().GetArpSettings(1) == synth::ArpSettings{});
  assert(!fs::exists(sessionFolder / "restoring.txt"));

  // The power can go at any moment. Nothing tells the app: what counts is what is on the card then.
  const fs::path cut = root / "power-cut";
  const auto powerCut = [&]() { CopyFolder(sessionFolder, cut / "session"); };
  const auto restart = [&]() {
    // The switch stands in for the restart; the card is put back to what the cut left.
    SwitchTo(kBlankId);
    CopyFolder(cut / "session", sessionFolder);
    SwitchTo("Cardenza/Synth");
    groove = &Groove();
    Frames();
  };
  const auto keepWorking = [&](std::uint16_t tempo) {
    groove->Sequencer().SetTempo(tempo);
    Tap(1, 1);  // any key: it may have changed something
  };
  const auto autosaved = [&](std::uint16_t tempo) { return Contains(ReadText(autosave), "\nSEQ " + std::to_string(tempo) + " "); };
  // A moment after the last key the work is kept, as the unsaved state it is.
  keepWorking(141);
  Frames(140);
  assert(autosaved(141));
  powerCut();
  // This one is too recent: the cut takes it.
  keepWorking(97);
  Frames(3);
  assert(!autosaved(97));
  restart();
  assert(groove->Sequencer().Tempo() == 141 && groove->TrackLevel(1) == 42);
  assert(groove->Ui().State().projectName == "SONG 01");
  // Again right after the start, before anything else happened: nothing was used up.
  powerCut();
  restart();
  assert(groove->Sequencer().Tempo() == 141 && groove->Ui().State().projectName == "SONG 01");
  // FlushSession (the web page closing) writes a change at once, without waiting for it to settle.
  keepWorking(133);
  Frames(3);
  groove->FlushSession();
  powerCut();
  restart();
  assert(groove->Sequencer().Tempo() == 133);
  keepWorking(141);
  groove->FlushSession();
  std::cout << "Autosave: " << fs::file_size(autosave) << " bytes" << std::endl;

  // A key that changes nothing writes nothing.
  {
    Tap(1, 1);
    Frames(140);
    const auto before = WriteTimes(sessionFolder);
    Tap(1, 1);
    Frames(140);
    assert(WriteTimes(sessionFolder) == before);
    assert(groove->Sequencer().InsertNote(1, 0, 5, 0, 72, 100, 24));
    Tap(1, 1);
    Frames(140);
    assert(WriteTimes(sessionFolder) != before && Contains(ReadText(autosave), "\n0 5 72 100 24 0\n"));
    assert(groove->Sequencer().ClearPatternEvents(1, 0));
    Tap(1, 1);
    Frames(140);
  }

  // Playing or not: the card is written on the device's background task, so the music does not wait.
  {
    groove->Sequencer().Play();
    keepWorking(150);
    Frames(140);
    assert(autosaved(150));
    // Keys in a row put it off, also with the transport stopped...
    groove->Sequencer().Stop();
    keepWorking(151);
    for (int beat = 0; beat < 20; ++beat)
    {
      Tap(1, 1);
      Frames(20);
    }
    assert(!autosaved(151));
    // ...but a minute at most after the first change it does not have.
    for (int second = 0; second < 61; ++second)
    {
      gNow += 1000;
      Tap(1, 1);
    }
    assert(autosaved(151));
    Frames(140);
  }

  // A write that fails is tried again, and says so once. The wait does not start over: keys that never
  // stop do not keep a failed autosave from the card for good.
  {
    const fs::path obstacle = sessionFolder / "autosave.ccproj.tmp";
    fs::create_directory(obstacle);  // the project's temporary file cannot be made
    keepWorking(152);
    Frames(140);
    assert(!autosaved(152) && groove->Ui().State().toast.text == "AUTOSAVE FAILED: SD CARD ERROR");
    fs::remove(obstacle);
    Frames(200);  // the next try, two seconds later
    assert(autosaved(152) && os::storage::CardMounted());
    fs::create_directory(obstacle);
    keepWorking(153);
    // A key every second: never settled, so only the longest wait brings the first try, and it fails.
    for (int second = 0; second < 60; ++second)
    {
      gNow += 1000;
      Tap(1, 1);
    }
    assert(!autosaved(153) && groove->Ui().State().toast.text == "AUTOSAVE FAILED: SD CARD ERROR");
    fs::remove(obstacle);
    for (int second = 0; second < 10; ++second)
    {
      gNow += 1000;
      Tap(1, 1);
    }
    assert(autosaved(153));
    Frames(140);
  }

  // The card out: the changes wait for it (and the screen says so once), then go to it.
  {
    CardenzaSim::Input::SetCardInserted(false);
    Frames(140);
    keepWorking(154);
    Frames(140);
    assert(!autosaved(154) && groove->Ui().State().toast.text == "AUTOSAVE FAILED: NO SD CARD");
    assert(!groove->QuerySystem().storageReady);
    CardenzaSim::Input::SetCardInserted(true);
    Frames(400);
    assert(autosaved(154) && groove->QuerySystem().storageReady);
  }

  // A sample the autosave names that is not there (the card was swapped, say) is kept as a name and
  // comes back when the card does.
  {
    const std::string tiny = (product / "Samples" / "tiny.wav").string();
    const std::array<std::int16_t, 64> frames{{1000, -1000}};
    assert(synth::sampler::WriteMono16Wav(tiny, frames.data(), frames.size(), 44100));
    assert(groove->Sampler().SelectBrowserSample(3, "tiny.wav"));
    Tap(1, 1);
    Frames(140);
    assert(Contains(ReadText(autosave), "\"tiny.wav\""));
    fs::rename(tiny, tiny + ".out");
    powerCut();
    restart();
    assert(groove->Sampler().BuildUiState(3).sampleFileName.empty());
    // Saved again meanwhile, it still names the sample.
    keepWorking(145);
    Frames(140);
    assert(autosaved(145) && Contains(ReadText(autosave), "\"tiny.wav\""));
    CardenzaSim::Input::SetCardInserted(false);
    Frames(140);
    fs::rename(tiny + ".out", tiny);
    CardenzaSim::Input::SetCardInserted(true);
    Frames(140);
    assert(groove->Sampler().BuildUiState(3).sampleFileName == "tiny.wav");
    Tap(1, 1);
    Frames(140);
    fs::remove(tiny);
    powerCut();
    restart();
  }

  // A large project is kept whole: every pattern of every track.
  {
    auto& sequencer = groove->Sequencer();
    for (std::size_t track = 0; track < 4; ++track)
      for (std::size_t pattern = 0; pattern < 16; ++pattern)
      {
        assert(sequencer.QueuePattern(track, pattern));
        for (std::size_t step = 0; step < 64; ++step) assert(sequencer.InsertNote(track, pattern, step, 0, 60 + step % 12, 100, 24));
      }
    for (std::size_t track = 0; track < 4; ++track) assert(sequencer.QueuePattern(track, 0));
    Tap(1, 1);
    Frames(140);
    powerCut();
    restart();
    for (std::size_t track = 0; track < 4; ++track)
      for (std::size_t pattern = 0; pattern < 16; ++pattern) assert(groove->Sequencer().PatternStepMask(track, pattern) != 0);
    groove::ProjectUiSettings fresh;
    std::string message;
    assert(groove->NewProject(fresh, message));
    groove->Sequencer().SetTempo(133);
    Tap(1, 1);
    Frames(140);
  }

  // An autosave that does not make sense: a clean start, no crash.
  powerCut();
  WriteText(cut / "session" / "autosave.ccproj", "CARDENZA_PROJECT 7\nnonsense\n");
  restart();
  assert(groove->Sequencer().Tempo() == 120);
  // A state that keeps crashing the start is not tried forever: two starts that did not finish are
  // tried once more, three are skipped, and the count goes.
  groove->Sequencer().SetTempo(146);
  Tap(1, 1);
  Frames(140);
  powerCut();
  WriteText(cut / "session" / "restoring.txt", "starts=2\n");
  restart();
  assert(groove->Sequencer().Tempo() == 146 && !fs::exists(sessionFolder / "restoring.txt"));
  WriteText(cut / "session" / "restoring.txt", "starts=3\n");
  restart();
  assert(groove->Sequencer().Tempo() == 120 && !fs::exists(sessionFolder / "restoring.txt"));
  groove->Sequencer().SetTempo(133);

  // A sample recording survives too: at most the last second is gone, and its screen comes back.
  CardenzaSim::Input::SetMicrophonePresent(true);
  const auto startRecording = [&]() {
    // T4, the sampler: FN + GRID opens its record screen, ENTER starts.
    Key(1, 0, true);
    Tap(0, 12);
    Key(1, 0, false);
    Key(2, 0, true);
    Tap(2, 10);
    Key(2, 0, false);
    assert(groove->Ui().TopModal() != nullptr && groove->Ui().TopModal()->kind == groove::ModalKind::SampleRecord);
    Tap(2, 13);
    assert(groove->Sampler().RecordingTrack() == 3);
  };
  startRecording();
  CardenzaSim::Input::FeedMicrophone(60000);
  Frames(2);
  assert(groove->Sampler().RecordingTrack() == 3);
  Frames();
  powerCut();
  assert(fs::is_regular_file(cut / "session" / "take-4.wav"));
  restart();
  {
    const auto take = groove->Sampler().BuildUiState(3);
    assert(take.hasDraft && !take.recording && take.frameCount >= 44100 && take.frameCount <= 60000);
    assert(groove->Ui().TopModal() != nullptr && groove->Ui().TopModal()->kind == groove::ModalKind::SampleRecord && groove->Ui().State().track == 3);
    // Discarded for good.
    Tap(0, 0);
    Tap(2, 13);
    assert(!groove->Sampler().BuildUiState(3).hasDraft && groove->Ui().TopModal() == nullptr);
    assert(!fs::exists(sessionFolder / "take-4.wav"));
  }
  // An app switch finishes the recording into a take: nothing is cut off.
  startRecording();
  CardenzaSim::Input::FeedMicrophone(50000);
  Frames(2);
  SwitchTo(kBlankId);
  SwitchTo("Cardenza/Synth");
  groove = &Groove();
  Frames();
  assert(groove->Sampler().BuildUiState(3).hasDraft && groove->Sampler().BuildUiState(3).frameCount == 50000);
  Tap(0, 0);
  Tap(2, 13);
  CardenzaSim::Input::SetMicrophonePresent(false);
  Key(1, 0, true);  // back to T2
  Tap(0, 10);
  Key(1, 0, false);

  // Loading a project brings back its play settings; device settings stay. NEW starts from the
  // default play settings.
  std::string message;
  groove::ProjectUiSettings ui;
  groove->Sequencer().SetArpSettings(synth::kDefaultActiveTrack, synth::ArpSettings{});
  assert(groove->LoadProject("SONG 01", ui, message));
  assert(ui.keyboard.octave == 1 && groove->TrackLevel(1) == 100 && groove->Sequencer().Tempo() == 133);
  assert(groove->MasterVolume() == 60 && groove->LedBrightness() == 7 && groove->Backlight() == 150);
  assert(groove->Sequencer().GetArpSettings(synth::kDefaultActiveTrack) == arp);
  assert(groove->NewProject(ui, message));
  assert(ui.keyboard.octave == 0 && ui.activeTrack == synth::kDefaultActiveTrack && groove->MasterVolume() == 60);
  assert(groove->Sequencer().GetArpSettings(synth::kDefaultActiveTrack) == synth::ArpSettings{});

  assert(!os::system::SwitchApplication("Nobody/Here"));

  // Card out: edited device settings stay in RAM (nothing in internal flash) until the card is back.
  CardenzaSim::Input::SetCardInserted(false);
  groove->SetMasterVolume(30);
  Frames(70);
  assert(!groove->QuerySystem().storageReady && os::storage::AnySettingsDirty());
  assert(!fs::exists(root / "internal") && Contains(ReadText(systemSettings), "sys.volume=60\n"));
  // A card edited on a computer meanwhile: its other settings are kept.
  WriteText(systemSettings, ReadText(systemSettings) + "future.computer=kept\n");
  CardenzaSim::Input::SetCardInserted(true);
  gNow += 6000;
  Frames(70);
  assert(groove->QuerySystem().storageReady && !os::storage::AnySettingsDirty());
  text = ReadText(systemSettings);
  assert(Contains(text, "sys.volume=30\n") && Contains(text, "future.computer=kept\n") && !fs::exists(root / "internal"));

  // While nothing happens nothing is written, however long the device stays on.
  {
    Frames(300);
    const auto snapshot = [&]() {
      std::map<std::string, fs::file_time_type> times;
      for (const auto& entry : fs::recursive_directory_iterator(product))
        if (entry.is_regular_file()) times[entry.path().string()] = entry.last_write_time();
      return times;
    };
    const auto before = snapshot();
    Frames(4000);
    assert(snapshot() == before);
  }

  // An orderly shutdown suspends the app: the session is waiting for the next start.
  os::system::Shutdown();
  assert(os::system::ActiveApplication() == nullptr);
  assert(fs::is_regular_file(autosave) && fs::is_regular_file(autosaveInfo));
  // Nothing went anywhere else: not the folders of the builds in between, not internal flash.
  assert(Snapshot(between) == betweenFiles && !fs::exists(root / "internal"));

  fs::current_path(fs::temp_directory_path());
  fs::remove_all(root);
  std::cout << "App lifecycle passed\n";
  return 0;
}
