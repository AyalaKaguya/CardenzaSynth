#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Settings files of the app storage protocol (docs/app-storage-protocol.md): text, one `key=value`
// per line, `#` comments. Keys are 1..48 characters of [a-z0-9_.] starting with a letter or digit;
// values are printable ASCII up to 200 characters without leading or trailing spaces. Reading is
// tolerant: bad lines are skipped, unknown keys are kept and written back. Files are replaced
// atomically (WriteFileAtomic).
namespace os::storage {

using SettingsEntry = std::pair<std::string, std::string>;

inline constexpr std::size_t kMaxSettingsKey = 48;
inline constexpr std::size_t kMaxSettingsValue = 200;
inline constexpr std::size_t kMaxSettingsFileBytes = 16 * 1024;

bool IsValidSettingsKey(std::string_view key);
bool IsValidSettingsValue(std::string_view value);
// Entries in file order; a repeated key keeps its first position and its last value.
std::vector<SettingsEntry> ParseSettings(std::string_view text, std::size_t* rejectedLines = nullptr);
// `header` is written as comment lines before the entries.
std::string FormatSettings(const std::vector<SettingsEntry>& entries, std::string_view header);

#if defined(CARDENZA_NATIVE_UI)
struct SettingsWriteBatch;
#endif

// One settings store: a RAM cache plus explicit edits to a file on the SD card. Flush rebases the
// edits onto the current card file, preserving untouched keys after the card was out or swapped.
// Without a writable card, edits stay dirty in RAM and are lost when the device powers off.
class Settings {
public:
  // `scope` identifies the store in its file header; `path` is the card file.
  Settings(std::string scope, std::string path);

  const std::string& Scope() const { return scope_; }
  const std::string& Path() const { return path_; }

  // Reads the card file (or its recovery file). Discards unsaved edits. With no readable card,
  // starts with an empty cache, so getters return their supplied defaults.
  void Load();
  bool Loaded() const { return loaded_; }
  // Reads the current card and reapplies pending edits, including removals. Returns false without
  // changing RAM when the card/file is unreadable, oversized or not a regular file. A genuinely
  // missing file is an empty store. Use this when the card is back instead of Load to keep unsaved edits.
  bool RefreshFromCard();

  bool Has(std::string_view key) const;
  std::string GetString(std::string_view key, std::string_view fallback = {}) const;
  // `fallback` when the key is missing, not a number or outside [minimum, maximum].
  int GetInt(std::string_view key, int fallback, int minimum = -2147483647 - 1, int maximum = 2147483647) const;
  // Accepts 1/0, true/false, on/off, yes/no.
  bool GetBool(std::string_view key, bool fallback) const;

  // False (and nothing changes) for an invalid key or value.
  bool SetString(std::string_view key, std::string_view value);
  bool SetInt(std::string_view key, int value);
  bool SetBool(std::string_view key, bool value);
  // Records an explicit removal, even if a key is not cached yet (the card may be absent).
  // False only for an invalid key.
  bool Remove(std::string_view key);

  const std::vector<SettingsEntry>& Entries() const { return entries_; }
  // Changes that are only in RAM.
  bool Dirty() const { return dirty_; }
  // Increases on every change; lets the system write settings a moment after the last change.
  std::uint32_t Revision() const { return revision_; }

  // Re-reads the current card, applies only pending edits and writes at most 16 KB. False leaves
  // all edits dirty; an absent/unwritable card never counts as a successful save. True when no
  // edits remain to save. Callers serialize refresh/edit/flush operations on the owning task.
  bool Flush();

private:
  struct Edit {
    std::string key;
    std::string value;
    bool removed = false;
#if defined(CARDENZA_NATIVE_UI)
    std::uint64_t serial = 0;
#endif
  };

  std::string FileHeader() const;
  void Put(std::string_view key, std::string_view value);
  void RecordEdit(std::string_view key, std::string_view value, bool removed);
  void ApplyEdits(std::vector<SettingsEntry>& entries) const;

  std::string scope_;
  std::string path_;
  std::vector<SettingsEntry> entries_;
  std::vector<Edit> edits_;
  bool loaded_ = false;
  bool dirty_ = false;
  std::uint32_t revision_ = 0;
#if defined(CARDENZA_NATIVE_UI)
  friend struct SettingsWriteBatch;
  std::uint64_t nativeIdentity_ = 0;
  std::uint64_t nativeLoadEpoch_ = 0;
  std::uint64_t nativeEditSerial_ = 0;
#endif
};

#if defined(CARDENZA_NATIVE_UI)
struct SettingsWriteResult {
  bool completed = false;
  // Every captured store was saved. Newer edits may still be dirty in RAM.
  bool saved = false;
  std::uint64_t ioMicros = 0;
};
// The owning/UI task captures pending edits and polls their completion. The
// worker only accesses independent values, never a Settings object or registry.
// False means busy, no edits, no mounted storage, or no available worker; it
// never falls back to synchronous I/O on the caller.
bool BeginSettingsWriteBehind();
SettingsWriteResult PollSettingsWriteBehind();
// Includes a completed batch that the UI has not acknowledged yet.
bool SettingsWriteInFlight();
// Explicit flush, refresh, reload, app handoff and shutdown may wait for I/O.
void DrainSettingsWriteBehind();
#endif

// The device settings, at SystemSettingsPath(). Loaded on first use.
Settings& SystemSettings();
// The settings of an app, at AppFolder::SettingsPath(). Loaded on first use; an invalid id gives
// a store that never writes.
Settings& AppSettings(std::string_view appId);
// Drops an app's cached settings when nothing of them is left only in RAM (after the app is gone).
void ReleaseAppSettings(std::string_view appId);
// Flushes every loaded store. True when nothing is left only in RAM.
bool FlushAllSettings();
// For the system's write-behind: the sum of every loaded store's Revision() and whether any store
// has unsaved changes in RAM.
std::uint32_t SettingsRevision();
bool AnySettingsDirty();
// Refreshes every cached store from the current card, preserving all pending edits. False if any
// store could not be read safely. Does not create stores or write files.
bool RefreshAllSettingsFromCard();
// Explicitly discards pending edits and reloads every cached store. For reset/tests; a card
// coming back uses RefreshAllSettingsFromCard instead.
void ReloadAllSettings();

}  // namespace os::storage
