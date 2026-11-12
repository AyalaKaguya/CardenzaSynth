#include "OS/System/System.h"

#include <chrono>
#include <cstdio>
#include <memory>
#include <utility>

#include "Applications/BuiltInApplications.h"
#include "Devices/Device.h"
#include "OS/Storage/DeviceSettings.h"
#include "OS/Storage/Settings.h"
#include "OS/Storage/Storage.h"
#include "OS/System/Log.h"

namespace os::system {

namespace {

constexpr const char* kBootAppKey = "boot.app";
constexpr const char* kProtocolKey = "protocol";
// A card that is out or failed writes are tried again this often at most: settings not written stay
// in RAM meanwhile (nothing is kept in internal flash).
constexpr std::uint32_t kCardRetryMs = 5000;

struct SystemState {
  std::unique_ptr<app::Application> application;
  std::string activeId;
  std::string pendingId;
  std::vector<std::string> bootArgs;
  Device::Keyboard::MatrixKeyState previousKeys{};
  // Keys held when the app changed: the new app sees neither their press nor their release.
  Device::Keyboard::MatrixKeyState ignoredKeys{};
  bool previousHome = false;
  // The current HOME press belongs to the app that ran before a switch.
  bool ignoreHome = false;
  bool started = false;
  std::uint32_t (*clock)() = nullptr;
  std::uint32_t settingsRevision = 0;
  std::uint32_t settingsChangedAt = 0;
  std::uint32_t settingsTriedAt = 0;
  bool settingsWriteFailed = false;
  // The card's settings are in the stores and its folders are there; false while it is out.
  bool cardReady = false;
  std::uint32_t cardTriedAt = 0;
};

SystemState& State() {
  static SystemState state;
  return state;
}

// Every physical key change also goes to the USB console as one line, "@K<row>,<column>,<0|1>"
// ("@H<0|1>" for HOME), raw changes, so a mirror sees every press and its release. The one-page
// Learn manual played them on its live device over Web Serial; the wiki that replaced it does not.
void ReportKey(std::size_t row, std::size_t column, bool pressed) {
#if defined(ESP_PLATFORM)
  std::printf("@K%u,%u,%u\n", static_cast<unsigned>(row), static_cast<unsigned>(column), pressed ? 1u : 0u);
#else
  (void)row;
  (void)column;
  (void)pressed;
#endif
}

void ReportHome(bool pressed) {
#if defined(ESP_PLATFORM)
  std::printf("@H%u\n", pressed ? 1u : 0u);
#else
  (void)pressed;
#endif
}

std::uint32_t Now() {
  const auto& state = State();
  if (state.clock != nullptr) return state.clock();
  using namespace std::chrono;
  return static_cast<std::uint32_t>(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

const app::ApplicationInfo* FindApplication(const std::string& id) {
  const auto& registry = app::Registry();
  const auto entry = registry.find(id);
  return entry == registry.end() ? nullptr : &entry->second;
}

void UpdateApplicationLeds(app::Application& application) {
  Device::LED::BeginFrame();
  Device::LED::Clear();
  application.UpdateLeds();
  Device::LED::Present();
}

void DispatchMatrixKeyEvents(app::Application& application) {
  auto& state = State();
  const Device::Keyboard::MatrixKeyState& currentState = Device::Keyboard::State();

  for (std::size_t index = 0; index < currentState.size(); ++index)
  {
    if (currentState[index] == state.previousKeys[index])
    {
      continue;
    }
    ReportKey(index / static_cast<std::size_t>(Device::Keyboard::kColumns), index % static_cast<std::size_t>(Device::Keyboard::kColumns),
              currentState[index]);
    if (state.ignoredKeys[index])
    {
      state.ignoredKeys.reset(index);
      continue;
    }

    const std::size_t row = index / static_cast<std::size_t>(Device::Keyboard::kColumns);
    const std::size_t column = index % static_cast<std::size_t>(Device::Keyboard::kColumns);
    application.HandleMatrixKeyEvent(row, column, currentState[index]);
  }

  state.previousKeys = currentState;
}

void DispatchHomeButtonEvents(app::Application& application) {
  auto& state = State();
  const bool pressed = Device::Buttons::HomePressed();
  if (pressed == state.previousHome)
  {
    return;
  }
  ReportHome(pressed);
  state.previousHome = pressed;
  if (state.ignoreHome)
  {
    if (!pressed) state.ignoreHome = false;
    return;
  }
  application.HandleHomeButtonEvent(pressed);
}

void RememberApplication(const std::string& id) {
  const app::ApplicationInfo* info = FindApplication(id);
  if (info != nullptr && !info->isSystem) (void)os::storage::SystemSettings().SetString(kBootAppKey, id);
}

bool StartApplication(const std::string& id, const std::vector<std::string>& args) {
  auto application = app::CreateApplication(id);
  if (!application) return false;
  auto& state = State();
  state.application = std::move(application);
  state.activeId = id;
  state.application->Setup(args);
  RememberApplication(id);
  return true;
}

// The card there again (it was out, or stopped answering and was mounted again): its settings come
// back with the edits not written yet on top, and the device settings follow it. `force` asks the
// device to mount it now (it limits how often a missing card is probed); otherwise at most every
// kCardRetryMs. Never in a run that started without a card.
bool PrepareCard(bool force = false) {
  auto& state = State();
  if (os::storage::CardMissingThisRun()) return false;
  if (!os::storage::CardMounted()) state.cardReady = false;
  if (state.cardReady) return true;
  const std::uint32_t now = Now();
  if (!force && now - state.cardTriedAt < kCardRetryMs) return false;
  state.cardTriedAt = now;
  if (!os::storage::CardPresent() || !os::storage::EnsureSystemLayout()) return false;
  // One store that cannot be read keeps what it has and refuses to write (Settings::Flush); the
  // others go on.
  (void)os::storage::RefreshAllSettingsFromCard();
  (void)os::storage::SystemSettings().SetInt(kProtocolKey, os::storage::kProtocolVersion);
  state.cardReady = true;
  os::storage::ApplyDeviceSettings();
  return true;
}

bool FlushSettings(bool force = false) {
  auto& state = State();
  if (os::storage::CardMissingThisRun()) return false;
#if defined(CARDENZA_NATIVE_UI)
  const auto began = std::chrono::steady_clock::now();
#endif
  const bool prepared = PrepareCard(force);
#if defined(CARDENZA_NATIVE_UI)
  const auto preparedAt = std::chrono::steady_clock::now();
#endif
  state.settingsTriedAt = Now();
  state.settingsWriteFailed = !prepared || !os::storage::FlushAllSettings();
  if (state.settingsWriteFailed) state.cardReady = false;
#if defined(CARDENZA_NATIVE_UI)
  const auto finished = std::chrono::steady_clock::now();
  const auto duration = std::chrono::duration_cast<std::chrono::microseconds>(finished - began).count();
  if (duration >= 20000)
    std::printf("system: slow settings flush at=%u prepare=%lldus write=%lldus ok=%u\n", static_cast<unsigned>(state.settingsTriedAt),
        static_cast<long long>(std::chrono::duration_cast<std::chrono::microseconds>(preparedAt - began).count()),
        static_cast<long long>(std::chrono::duration_cast<std::chrono::microseconds>(finished - preparedAt).count()),
        state.settingsWriteFailed ? 0u : 1u);
#endif
  return !state.settingsWriteFailed;
}

void PerformSwitch(const std::string& id) {
  auto& state = State();
  if (id == state.activeId && state.application) return;
  const std::string from = state.activeId;
  if (state.application)
  {
    (void)PrepareCard(true);
    state.application->Suspend();
    (void)FlushSettings();
    (void)os::log::Flush();
    // Destroy before create: the next app gets the whole heap.
    state.application.reset();
    os::storage::ReleaseAppSettings(from);
  }
  state.activeId.clear();

  Device::LED::BeginFrame();
  Device::LED::Clear();
  Device::LED::Present();
  Device::Display::Clear();

  bool started = StartApplication(id, {});
  if (!started && !from.empty()) started = StartApplication(from, {});
  // Neither the new nor the old app came up: fall back to the default app.
  if (!started) (void)StartApplication(app::DefaultApplicationId(), {});

  state.ignoredKeys = Device::Keyboard::State();
  state.previousKeys = state.ignoredKeys;
  state.ignoreHome = Device::Buttons::HomePressed();
  state.previousHome = state.ignoreHome;
  (void)FlushSettings();
}

void WriteBehindSettings() {
  auto& state = State();
#if defined(CARDENZA_NATIVE_UI)
  // The system task owns settings caches. File I/O uses a value snapshot on
  // the background worker; completion only acknowledges unchanged edits here.
  const auto completed = os::storage::PollSettingsWriteBehind();
  if (completed.completed)
  {
    state.settingsTriedAt = Now();
    state.settingsWriteFailed = !completed.saved;
    if (!completed.saved) state.cardReady = false;
    if (!completed.saved || completed.ioMicros >= 20000)
      std::printf("system: background settings flush io=%lluus ok=%u\n",
          static_cast<unsigned long long>(completed.ioMicros), completed.saved ? 1u : 0u);
  }
#endif
  // A run that started without a card keeps its settings in RAM: nothing to do.
  if (os::storage::CardMissingThisRun()) return;
  const std::uint32_t now = Now();
#if defined(CARDENZA_NATIVE_UI)
  const std::uint32_t revision = os::storage::SettingsRevision();
  if (revision != state.settingsRevision)
  {
    state.settingsRevision = revision;
    state.settingsChangedAt = now;
  }
  // A storage refresh would wait for the in-flight write. Ordinary UI ticks
  // simply defer it; forced application handoff/flush still drains explicitly.
  if (os::storage::SettingsWriteInFlight()) return;
#endif
  // A card mounted again (by anyone who looked for it) brings its settings back. Nobody probes for a
  // missing card here unless a setting waits to be written.
  if (!os::storage::CardMounted()) state.cardReady = false;
  else if (!state.cardReady) (void)PrepareCard();
#if !defined(CARDENZA_NATIVE_UI)
  const std::uint32_t revision = os::storage::SettingsRevision();
  if (revision != state.settingsRevision)
  {
    state.settingsRevision = revision;
    state.settingsChangedAt = now;
  }
#endif
  if (!os::storage::AnySettingsDirty() || now - state.settingsChangedAt < kSettingsWriteDelayMs) return;
  // On failure try again after a while instead of every frame.
  if (state.settingsWriteFailed && now - state.settingsTriedAt < kCardRetryMs) return;
#if defined(CARDENZA_NATIVE_UI)
  state.settingsTriedAt = now;
  if (!os::storage::BeginSettingsWriteBehind()) state.settingsWriteFailed = true;
#else
  (void)FlushSettings();
#endif
}

}  // namespace

bool Begin(const BootOptions& options) {
  auto& state = State();
  if (state.started)
  {
    return state.application != nullptr;
  }

  if (!Device::Init())
  {
    return false;
  }

  app::RegisterBuiltInApplications();

  // The card is looked for once, before any app is set up: a run that starts without one makes no
  // card operation at all, and saves nothing, until the device restarts (os::storage::CardMissingThisRun).
  if (os::storage::DecideCardAtStart())
  {
    (void)os::storage::SystemSettings().SetInt(kProtocolKey, os::storage::kProtocolVersion);
    state.cardReady = os::storage::EnsureSystemLayout();
    if (state.cardReady) (void)os::storage::RefreshAllSettingsFromCard();
  }
  else std::printf("system: no SD card at the start: nothing is read from or saved to a card until a restart\n");
  state.cardTriedAt = Now();
  os::storage::ApplyDeviceSettings();
  // The end of the previous run (a crash too) goes to the card before anything can go wrong again.
  (void)os::log::Flush();

  const std::string bootId = BootApplicationId();
  if (!StartApplication(bootId, options.applicationArgs) &&
      (bootId == app::DefaultApplicationId() || !StartApplication(app::DefaultApplicationId(), options.applicationArgs)))
  {
    return false;
  }

  Device::Start();
  state.previousKeys = Device::Keyboard::State();
  state.previousHome = Device::Buttons::HomePressed();
  state.started = true;
  (void)FlushSettings();
  state.settingsRevision = os::storage::SettingsRevision();

  UpdateApplicationLeds(*state.application);
  Device::Display::Clear();
  state.application->RenderFrame();
  Device::Display::Present();
  return true;
}

void RunFrame(bool draw) {
  auto& state = State();
  if (!state.pendingId.empty())
  {
    const std::string id = std::move(state.pendingId);
    state.pendingId.clear();
    PerformSwitch(id);
  }

  app::Application* application = ActiveApplication();
  if (application == nullptr)
  {
    return;
  }

  DispatchMatrixKeyEvents(*application);
  DispatchHomeButtonEvents(*application);
#if defined(CARDENZA_NATIVE_UI)
  Device::Touch::Event touch;
  for (unsigned count = 0; count < 32 && Device::Touch::Poll(touch); ++count)
    application->HandleTouchEvent(touch);
#endif
  application->Tick();
  UpdateApplicationLeds(*application);
  if (draw)
  {
    application->RenderFrame();
    Device::Display::Present();
  }
  WriteBehindSettings();
  os::log::Service(Now());
}

app::Application* ActiveApplication() {
  return State().application.get();
}

const std::string& ActiveApplicationId() {
  return State().activeId;
}

bool SwitchApplication(const std::string& id) {
  const app::ApplicationInfo* info = FindApplication(id);
  if (info == nullptr || info->factory == nullptr) return false;
  State().pendingId = id;
  return true;
}

std::string BootApplicationId() {
  const std::string remembered = os::storage::SystemSettings().GetString(kBootAppKey);
  const app::ApplicationInfo* info = remembered.empty() ? nullptr : FindApplication(remembered);
  return info != nullptr && !info->isSystem && info->factory != nullptr ? remembered : app::DefaultApplicationId();
}

void Shutdown() {
  auto& state = State();
  (void)PrepareCard(true);
  if (state.application)
  {
    state.application->Suspend();
    (void)FlushSettings();
    state.application.reset();
    os::storage::ReleaseAppSettings(state.activeId);
    state.activeId.clear();
  }
  (void)FlushSettings();
  (void)os::log::Flush();
}

void UseClock(std::uint32_t (*clock)()) {
  State().clock = clock;
}

}  // namespace os::system
