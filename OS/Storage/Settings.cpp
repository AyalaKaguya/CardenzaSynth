#include "OS/Storage/Settings.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <sys/stat.h>

#include "OS/Storage/Storage.h"
#if defined(CARDENZA_NATIVE_UI)
#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include "Devices/Device.h"
#endif

namespace os::storage {

namespace {

constexpr std::string_view kSystemScope = "system";

bool IsKeyCharacter(char c)
{
  return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '.';
}

std::string_view Trim(std::string_view text)
{
  while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
  while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) text.remove_suffix(1);
  return text;
}

bool EqualsIgnoreCase(std::string_view text, std::string_view lower)
{
  if (text.size() != lower.size()) return false;
  for (std::size_t index = 0; index < text.size(); ++index)
  {
    char c = text[index];
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    if (c != lower[index]) return false;
  }
  return true;
}

enum class FileState { Missing, Regular, Error };

FileState InspectSettingsFile(const std::string& path, std::size_t& size)
{
  struct stat info{};
  if (stat(path.c_str(), &info) != 0)
  {
    // Only ENOENT is a missing file. In particular, EIO/EACCES/ENOTDIR must never turn an
    // unreadable card or a conflicting path into an apparently empty settings file.
    return errno == ENOENT ? FileState::Missing : FileState::Error;
  }
#if defined(_WIN32)
  const bool regular = (info.st_mode & _S_IFMT) == _S_IFREG;
#else
  const bool regular = S_ISREG(info.st_mode);
#endif
  if (!regular || info.st_size < 0 || static_cast<unsigned long long>(info.st_size) > kMaxSettingsFileBytes)
    return FileState::Error;
  size = static_cast<std::size_t>(info.st_size);
  return FileState::Regular;
}

bool ReadCardSettings(const std::string& path, std::vector<SettingsEntry>& entries)
{
  if (path.empty() || !CardPresent()) return false;
  std::string source = path;
  std::size_t size = 0;
  FileState state = InspectSettingsFile(source, size);
  if (state == FileState::Missing)
  {
    source += ".bak";
    state = InspectSettingsFile(source, size);
  }
  if (state == FileState::Error) return false;
  if (state == FileState::Missing)
  {
    entries.clear();
    return true;
  }
  std::FILE* file = std::fopen(source.c_str(), "rb");
  if (file == nullptr) return false;
  std::string text(size, '\0');
  const std::size_t read = size == 0 ? 0 : std::fread(text.data(), 1, size, file);
  // Reject a short read, an I/O error, or a file that changed size after stat. Do not replace a
  // file from a partial snapshot. Malformed text lines still use the tolerant settings parser.
  const bool complete = read == size && std::fgetc(file) == EOF && std::ferror(file) == 0;
  const bool closed = std::fclose(file) == 0;
  if (!complete || !closed) return false;
  entries = ParseSettings(text);
  return true;
}

std::string ParentPath(const std::string& path)
{
  const std::size_t slash = path.find_last_of('/');
  return slash == std::string::npos ? std::string() : path.substr(0, slash);
}

}  // namespace

bool IsValidSettingsKey(std::string_view key)
{
  if (key.empty() || key.size() > kMaxSettingsKey || key.front() == '.' || key.front() == '_') return false;
  return std::all_of(key.begin(), key.end(), IsKeyCharacter);
}

bool IsValidSettingsValue(std::string_view value)
{
  if (value.size() > kMaxSettingsValue) return false;
  if (!value.empty() && (value.front() == ' ' || value.back() == ' ')) return false;
  return std::all_of(value.begin(), value.end(), [](char c) { return c >= 32 && c < 127; });
}

std::vector<SettingsEntry> ParseSettings(std::string_view text, std::size_t* rejectedLines)
{
  std::vector<SettingsEntry> entries;
  std::size_t rejected = 0;
  while (!text.empty())
  {
    const std::size_t end = text.find('\n');
    std::string_view line = text.substr(0, end);
    text = end == std::string_view::npos ? std::string_view() : text.substr(end + 1);
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    line = Trim(line);
    if (line.empty() || line.front() == '#') continue;
    const std::size_t equals = line.find('=');
    if (equals == std::string_view::npos)
    {
      ++rejected;
      continue;
    }
    const std::string_view key = Trim(line.substr(0, equals));
    const std::string_view value = Trim(line.substr(equals + 1));
    if (!IsValidSettingsKey(key) || !IsValidSettingsValue(value))
    {
      ++rejected;
      continue;
    }
    const auto existing = std::find_if(entries.begin(), entries.end(), [&](const SettingsEntry& entry) { return entry.first == key; });
    if (existing != entries.end()) existing->second = std::string(value);
    else entries.emplace_back(std::string(key), std::string(value));
  }
  if (rejectedLines != nullptr) *rejectedLines = rejected;
  return entries;
}

std::string FormatSettings(const std::vector<SettingsEntry>& entries, std::string_view header)
{
  std::string text;
  while (!header.empty())
  {
    const std::size_t end = header.find('\n');
    text.append("# ");
    text.append(header.substr(0, end));
    text.push_back('\n');
    header = end == std::string_view::npos ? std::string_view() : header.substr(end + 1);
  }
  for (const auto& entry : entries)
  {
    text.append(entry.first);
    text.push_back('=');
    text.append(entry.second);
    text.push_back('\n');
  }
  return text;
}

Settings::Settings(std::string scope, std::string path) : scope_(std::move(scope)), path_(std::move(path))
{
#if defined(CARDENZA_NATIVE_UI)
  static std::atomic<std::uint64_t> nextIdentity{1};
  nativeIdentity_ = nextIdentity.fetch_add(1, std::memory_order_relaxed);
#endif
}

std::string Settings::FileHeader() const
{
  return "Cardenza settings: " + scope_ + " (storage protocol " + std::to_string(kProtocolVersion) +
         ")\nkey=value per line; unknown keys are kept";
}

void Settings::Load()
{
#if defined(CARDENZA_NATIVE_UI)
  DrainSettingsWriteBehind();
  ++nativeLoadEpoch_;
#endif
  entries_.clear();
  edits_.clear();
  dirty_ = false;
  loaded_ = true;
  (void)ReadCardSettings(path_, entries_);
}

bool Settings::RefreshFromCard()
{
#if defined(CARDENZA_NATIVE_UI)
  DrainSettingsWriteBehind();
#endif
  std::vector<SettingsEntry> refreshed;
  if (!ReadCardSettings(path_, refreshed)) return false;
  ApplyEdits(refreshed);
  if (entries_ != refreshed) ++revision_;
  entries_ = std::move(refreshed);
  loaded_ = true;
  return true;
}

bool Settings::Has(std::string_view key) const
{
  return std::any_of(entries_.begin(), entries_.end(), [&](const SettingsEntry& entry) { return entry.first == key; });
}

std::string Settings::GetString(std::string_view key, std::string_view fallback) const
{
  const auto entry = std::find_if(entries_.begin(), entries_.end(), [&](const SettingsEntry& item) { return item.first == key; });
  return entry != entries_.end() ? entry->second : std::string(fallback);
}

int Settings::GetInt(std::string_view key, int fallback, int minimum, int maximum) const
{
  if (!Has(key)) return fallback;
  const std::string text = GetString(key);
  if (text.empty()) return fallback;
  char* end = nullptr;
  errno = 0;
  const long value = std::strtol(text.c_str(), &end, 10);
  if (errno != 0 || end == text.c_str() || *end != '\0' || value < minimum || value > maximum) return fallback;
  return static_cast<int>(value);
}

bool Settings::GetBool(std::string_view key, bool fallback) const
{
  if (!Has(key)) return fallback;
  const std::string text = GetString(key);
  if (text == "1" || EqualsIgnoreCase(text, "true") || EqualsIgnoreCase(text, "on") || EqualsIgnoreCase(text, "yes")) return true;
  if (text == "0" || EqualsIgnoreCase(text, "false") || EqualsIgnoreCase(text, "off") || EqualsIgnoreCase(text, "no")) return false;
  return fallback;
}

void Settings::Put(std::string_view key, std::string_view value)
{
  const auto entry = std::find_if(entries_.begin(), entries_.end(), [&](const SettingsEntry& item) { return item.first == key; });
  if (entry != entries_.end()) entry->second = std::string(value);
  else entries_.emplace_back(std::string(key), std::string(value));
}

void Settings::RecordEdit(std::string_view key, std::string_view value, bool removed)
{
  const auto edit = std::find_if(edits_.begin(), edits_.end(), [&](const Edit& item) { return item.key == key; });
  if (edit != edits_.end())
  {
    edit->value = std::string(value);
    edit->removed = removed;
  }
  else edits_.push_back({std::string(key), std::string(value), removed});
#if defined(CARDENZA_NATIVE_UI)
  // A later edit of the same key must survive an older snapshot's completion,
  // including a remove/set round trip that happens to return to the same value.
  const auto changed = std::find_if(edits_.begin(), edits_.end(), [&](const Edit& item) { return item.key == key; });
  changed->serial = ++nativeEditSerial_;
#endif
  dirty_ = true;
}

void Settings::ApplyEdits(std::vector<SettingsEntry>& entries) const
{
  for (const Edit& edit : edits_)
  {
    const auto entry = std::find_if(entries.begin(), entries.end(), [&](const SettingsEntry& item) { return item.first == edit.key; });
    if (edit.removed)
    {
      if (entry != entries.end()) entries.erase(entry);
    }
    else if (entry != entries.end()) entry->second = edit.value;
    else entries.emplace_back(edit.key, edit.value);
  }
}

bool Settings::SetString(std::string_view key, std::string_view value)
{
  if (!IsValidSettingsKey(key) || !IsValidSettingsValue(value)) return false;
  if (!loaded_) Load();
  const auto entry = std::find_if(entries_.begin(), entries_.end(), [&](const SettingsEntry& item) { return item.first == key; });
  if (entry != entries_.end() && entry->second == value) return true;
  RecordEdit(key, value, false);
  Put(key, value);
  ++revision_;
  return true;
}

bool Settings::SetInt(std::string_view key, int value)
{
  return SetString(key, std::to_string(value));
}

bool Settings::SetBool(std::string_view key, bool value)
{
  return SetString(key, value ? "1" : "0");
}

bool Settings::Remove(std::string_view key)
{
  if (!IsValidSettingsKey(key)) return false;
  if (!loaded_) Load();
  const auto entry = std::find_if(entries_.begin(), entries_.end(), [&](const SettingsEntry& item) { return item.first == key; });
  const auto edit = std::find_if(edits_.begin(), edits_.end(), [&](const Edit& item) { return item.key == key; });
  if (entry == entries_.end() && edit != edits_.end() && edit->removed) return true;
  RecordEdit(key, {}, true);
  if (entry != entries_.end()) entries_.erase(entry);
  ++revision_;
  return true;
}

bool Settings::Flush()
{
#if defined(CARDENZA_NATIVE_UI)
  DrainSettingsWriteBehind();
#endif
  if (!dirty_) return true;
  std::vector<SettingsEntry> merged;
  if (!ReadCardSettings(path_, merged))
  {
    ReportCardIoError();  // A failed pre-save read may be the first sign of a card gone.
    return false;
  }
  ApplyEdits(merged);
  const std::string text = FormatSettings(merged, FileHeader());
  if (text.size() > kMaxSettingsFileBytes || !CreateDirectories(ParentPath(path_)) || !WriteFileAtomic(path_, text)) return false;
  if (entries_ != merged) ++revision_;
  entries_ = std::move(merged);
  edits_.clear();
  dirty_ = false;
  return true;
}

namespace {

struct Registry {
  std::unique_ptr<Settings> system;
  std::map<std::string, std::unique_ptr<Settings>, std::less<>> apps;
};

Registry& Stores()
{
  static Registry registry;
  return registry;
}

}  // namespace

#if defined(CARDENZA_NATIVE_UI)
// All live Settings objects and the registry remain owned by the system task.
// Only this detached value batch is shared with the storage worker.
struct SettingsWriteBatch {
  struct Store {
    std::uint64_t identity = 0;
    std::uint64_t loadEpoch = 0;
    std::string path;
    std::string header;
    std::vector<Settings::Edit> edits;
    std::vector<SettingsEntry> merged;
    bool saved = false;
  };

  std::vector<Store> stores;
  std::atomic<bool> started{false};
  std::atomic<bool> done{false};
  bool saved = false;
  std::uint64_t ioMicros = 0;

  void Capture(const Settings& settings)
  {
    if (!settings.dirty_) return;
    Store store;
    store.identity = settings.nativeIdentity_;
    store.loadEpoch = settings.nativeLoadEpoch_;
    store.path = settings.path_;
    store.header = settings.FileHeader();
    store.edits = settings.edits_;
    stores.push_back(std::move(store));
  }

  static void Apply(const std::vector<Settings::Edit>& edits, std::vector<SettingsEntry>& entries)
  {
    for (const auto& edit : edits)
    {
      const auto entry = std::find_if(entries.begin(), entries.end(), [&](const SettingsEntry& item) { return item.first == edit.key; });
      if (edit.removed)
      {
        if (entry != entries.end()) entries.erase(entry);
      }
      else if (entry != entries.end()) entry->second = edit.value;
      else entries.emplace_back(edit.key, edit.value);
    }
  }

  void Write()
  {
    const auto begin = std::chrono::steady_clock::now();
    saved = true;
    for (auto& store : stores)
    {
      // Rebase only these explicit edits onto the file that exists when this
      // worker actually runs, including keys changed by another client.
      if (!ReadCardSettings(store.path, store.merged))
      {
        ReportCardIoError();
        saved = false;
        continue;
      }
      Apply(store.edits, store.merged);
      const auto text = FormatSettings(store.merged, store.header);
      store.saved = text.size() <= kMaxSettingsFileBytes && CreateDirectories(ParentPath(store.path)) &&
          WriteFileAtomic(store.path, text);
      saved = store.saved && saved;
    }
    ioMicros = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - begin).count());
    done.store(true, std::memory_order_release);
  }

  void Acknowledge(Settings& settings)
  {
    for (auto& store : stores)
    {
      if (!store.saved || store.identity != settings.nativeIdentity_ || store.loadEpoch != settings.nativeLoadEpoch_) continue;
      auto& edits = settings.edits_;
      edits.erase(std::remove_if(edits.begin(), edits.end(), [&](const Settings::Edit& edit) {
        return std::any_of(store.edits.begin(), store.edits.end(), [&](const Settings::Edit& written) {
          return written.key == edit.key && written.serial == edit.serial;
        });
      }), edits.end());
      // Keep UI edits made while I/O was running on top of the newly saved
      // file, while refreshing untouched/unknown keys from its rebase.
      settings.ApplyEdits(store.merged);
      if (settings.entries_ != store.merged) ++settings.revision_;
      settings.entries_ = std::move(store.merged);
      settings.dirty_ = !edits.empty();
      return;
    }
  }
};

namespace {
std::mutex nativeSettingsWriteMutex;
std::shared_ptr<SettingsWriteBatch> nativeSettingsWrite;

void WriteSettingsInBackground()
{
  std::shared_ptr<SettingsWriteBatch> batch;
  {
    std::lock_guard<std::mutex> lock(nativeSettingsWriteMutex);
    batch = nativeSettingsWrite;
  }
  // Device::Background invokes every registered job on each notification.
  if (!batch || batch->started.exchange(true, std::memory_order_acq_rel)) return;
  batch->Write();
}
}  // namespace

bool BeginSettingsWriteBehind()
{
  if (SettingsWriteInFlight() || CardMissingThisRun() || !CardMounted()) return false;
  auto batch = std::make_shared<SettingsWriteBatch>();
  auto& stores = Stores();
  if (stores.system) batch->Capture(*stores.system);
  for (const auto& entry : stores.apps) batch->Capture(*entry.second);
  if (batch->stores.empty()) return false;
  {
    std::lock_guard<std::mutex> lock(nativeSettingsWriteMutex);
    nativeSettingsWrite = batch;
  }
  if (Device::Background::Run(&WriteSettingsInBackground)) return true;
#if !defined(ESP_PLATFORM)
  // The simulator has no persistent device worker. Keep the same asynchronous
  // ownership contract for its native UI and regression harnesses.
  std::thread(&WriteSettingsInBackground).detach();
  return true;
#else
  std::lock_guard<std::mutex> lock(nativeSettingsWriteMutex);
  if (nativeSettingsWrite == batch) nativeSettingsWrite.reset();
  return false;
#endif
}

SettingsWriteResult PollSettingsWriteBehind()
{
  std::shared_ptr<SettingsWriteBatch> batch;
  {
    std::lock_guard<std::mutex> lock(nativeSettingsWriteMutex);
    if (!nativeSettingsWrite || !nativeSettingsWrite->done.load(std::memory_order_acquire)) return {};
    batch = std::move(nativeSettingsWrite);
  }
  auto& stores = Stores();
  if (stores.system) batch->Acknowledge(*stores.system);
  for (auto& entry : stores.apps) batch->Acknowledge(*entry.second);
  return {true, batch->saved, batch->ioMicros};
}

bool SettingsWriteInFlight()
{
  std::lock_guard<std::mutex> lock(nativeSettingsWriteMutex);
  return nativeSettingsWrite != nullptr;
}

void DrainSettingsWriteBehind()
{
  std::shared_ptr<SettingsWriteBatch> batch;
  {
    std::lock_guard<std::mutex> lock(nativeSettingsWriteMutex);
    batch = nativeSettingsWrite;
  }
  if (!batch) return;
  while (!batch->done.load(std::memory_order_acquire)) std::this_thread::sleep_for(std::chrono::milliseconds(1));
  (void)PollSettingsWriteBehind();
}
#endif

Settings& SystemSettings()
{
  auto& stores = Stores();
  if (!stores.system) stores.system = std::make_unique<Settings>(std::string(kSystemScope), SystemSettingsPath());
  if (!stores.system->Loaded()) stores.system->Load();
  return *stores.system;
}

Settings& AppSettings(std::string_view appId)
{
  auto& stores = Stores();
  auto entry = stores.apps.find(appId);
  if (entry == stores.apps.end())
  {
    const AppFolder folder(appId);
    entry = stores.apps.emplace(std::string(appId), std::make_unique<Settings>(std::string(appId), folder.SettingsPath())).first;
  }
  if (!entry->second->Loaded()) entry->second->Load();
  return *entry->second;
}

void ReleaseAppSettings(std::string_view appId)
{
  auto& stores = Stores();
  const auto entry = stores.apps.find(appId);
  if (entry != stores.apps.end() && !entry->second->Dirty()) stores.apps.erase(entry);
}

bool FlushAllSettings()
{
#if defined(CARDENZA_NATIVE_UI)
  DrainSettingsWriteBehind();
#endif
  auto& stores = Stores();
  bool saved = true;
  if (stores.system) saved = stores.system->Flush() && saved;
  for (auto& entry : stores.apps) saved = entry.second->Flush() && saved;
  return saved;
}

std::uint32_t SettingsRevision()
{
  auto& stores = Stores();
  std::uint32_t revision = stores.system ? stores.system->Revision() : 0;
  for (const auto& entry : stores.apps) revision += entry.second->Revision();
  return revision;
}

bool AnySettingsDirty()
{
  auto& stores = Stores();
  if (stores.system && stores.system->Dirty()) return true;
  return std::any_of(stores.apps.begin(), stores.apps.end(), [](const auto& entry) { return entry.second->Dirty(); });
}

bool RefreshAllSettingsFromCard()
{
#if defined(CARDENZA_NATIVE_UI)
  DrainSettingsWriteBehind();
#endif
  auto& stores = Stores();
  bool refreshed = true;
  if (stores.system) refreshed = stores.system->RefreshFromCard() && refreshed;
  for (auto& entry : stores.apps) refreshed = entry.second->RefreshFromCard() && refreshed;
  return refreshed;
}

void ReloadAllSettings()
{
#if defined(CARDENZA_NATIVE_UI)
  DrainSettingsWriteBehind();
#endif
  auto& stores = Stores();
  if (stores.system) stores.system->Load();
  for (auto& entry : stores.apps) entry.second->Load();
}

}  // namespace os::storage
