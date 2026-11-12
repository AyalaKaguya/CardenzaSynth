#pragma once

#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>
#include "Devices/Device.h"

namespace app {

// One app at a time runs on the device (os::system). Lifecycle:
//   construct -> Setup -> frames (key events, Tick, UpdateLeds, RenderFrame) -> Suspend -> destroy.
// The system destroys the app before it creates the next one, so an app never runs in the
// background and gets the whole heap. Setup is also the resume point: an app that saved a session
// in Suspend restores it there. Apps store data only through the OS storage API (OS/Storage, see
// docs/app-storage-protocol.md), in their own folder.
class Application {
public:
  virtual ~Application() = default;
  virtual void Setup(const std::vector<std::string>& args) = 0;
  // Called before the app is destroyed (switch to another app, shutdown): persist the working
  // session and settings here. The system flushes every settings store right after it returns.
  virtual void Suspend() {}
  virtual void Tick() = 0;
  virtual void UpdateLeds() {}
  virtual void RenderFrame() = 0;
  virtual void HandleMatrixKeyEvent(std::size_t row, std::size_t column, bool pressed) {
    (void)row;
    (void)column;
    (void)pressed;
  }
  virtual void HandleHomeButtonEvent(bool pressed) {
    (void)pressed;
  }
  virtual void HandleTouchEvent(const Device::Touch::Event&) {}
};

using ApplicationFactory = std::unique_ptr<Application> (*)();

struct ApplicationInfo {
  std::string id;
  std::string name;
  std::string author;
  bool isSystem = false;
  ApplicationFactory factory = nullptr;
};

inline std::map<std::string, ApplicationInfo>& Registry() {
  static std::map<std::string, ApplicationInfo> registry;
  return registry;
}

inline std::string MakeApplicationId(std::string_view author, std::string_view name) {
  return std::string(author) + "/" + std::string(name);
}

inline bool RegisterApplication(const ApplicationInfo& info) {
  return Registry().emplace(info.id, info).second;
}

inline std::unique_ptr<Application> CreateApplication(const std::string& id) {
  const auto iterator = Registry().find(id);
  if (iterator == Registry().end() || iterator->second.factory == nullptr)
  {
    return nullptr;
  }
  return iterator->second.factory();
}

inline std::vector<ApplicationInfo> ListApplications() {
  std::vector<ApplicationInfo> applications;
  applications.reserve(Registry().size());
  for (const auto& entry : Registry())
  {
    applications.push_back(entry.second);
  }
  return applications;
}

template <typename T> std::unique_ptr<Application> MakeApplication() {
  return std::make_unique<T>();
}

}  // namespace app
