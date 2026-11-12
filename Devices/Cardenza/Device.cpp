#include "Devices/Device.h"
#include "Devices/Cardenza/DeviceLog.h"
#include "Devices/KeySettle.h"
#include "Devices/LedFrameBuffer.h"
#include "Devices/LedLight.h"
#include "Library/Memory/Memory.h"

#include <atomic>
#include <new>
#include <cstdlib>
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <string_view>
#include <utility>

#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <esp_vfs_fat.h>

#include "cardenzaHAL.h"
#include "usb/usb.h"
#include "Devices/Cardenza/DeviceUsbStats.h"

namespace {

alignas(4) std::array<Device::Display::Pixel, Device::Display::kPixelCount> frameBuffer{};
Device::Keyboard::MatrixKeyState keyboardState{};
Device::LED::detail::LedFrameBuffer keyboardLedFrames;

// The HAL mounts the card here. The folders below it belong to the OS storage protocol.
constexpr char kCardRoot[] = "/sdcard";
// A missing card is probed again at most this often (a failed SDMMC mount takes a while).
constexpr std::int64_t kMountRetryUs = 5 * 1000 * 1000;
std::int64_t lastMountAttemptUs = 0;
bool mountAttempted = false;

constexpr bool IsValidMatrixPosition(std::size_t row, std::size_t column) {
  return row < static_cast<std::size_t>(Device::LED::kRows) &&
         column < static_cast<std::size_t>(Device::LED::kColumns);
}

constexpr std::size_t MatrixIndex(std::size_t row, std::size_t column) {
  return row * static_cast<std::size_t>(Device::LED::kColumns) + column;
}

// What the HAL holds now. Key levels are perceived brightness, gamma-mapped to the HAL's linear duty
// (Devices/LedLight.h) and sent as one frame; the master level goes as it is, the HAL corrects it.
Device::LED::detail::LedFrame halLeds{};
bool halLedsKnown = false;

void PublishLedFrame(bool changed) {
  if (!changed) return;
  const auto& frame = keyboardLedFrames.Current();
  if (!halLedsKnown || frame.levels != halLeds.levels) {
    CardenzaHAL::Keyboard::LedFrame duties{};
    for (std::size_t index = 0; index < frame.levels.size(); ++index) duties[index] = Device::LED::KeyLevel(frame.levels[index]);
    CardenzaHAL::Keyboard::setLEDFrame(duties);
  }
  if (!halLedsKnown || frame.master != halLeds.master) CardenzaHAL::Keyboard::setLEDGlobalBrightness(frame.master);
  if (!halLedsKnown || frame.enabled != halLeds.enabled) (void)CardenzaHAL::Keyboard::enableLED(frame.enabled);
  halLeds = frame;
  halLedsKnown = true;
}

}  // namespace

namespace Device {

namespace Display {

FrameBufferView CurrentFrameBuffer() {
  return FrameBufferView{frameBuffer.data(), frameBuffer.size(), kWidth, kHeight};
}

MutableFrameBufferView MutableFrameBuffer() {
  return MutableFrameBufferView{frameBuffer.data(), frameBuffer.size(), kWidth, kHeight};
}

void Clear(Pixel color) {
  frameBuffer.fill(color);
}

namespace {

// A whole frame takes the panel 17 ms (SPI at 40 MHz, the HAL sending eight rows at a time and waiting
// for each), most of a frame at 50 fps, and with the audio busy the UI task waited behind the
// render for every band: 25-30 ms (the stress test, 2026-11-04). So only what changed is sent. Every
// row is hashed in segments of 30 pixels and compared with the last frame's; each band of eight rows
// sends its changed rows across the span of its changed segments.
constexpr int kSegmentPixels = 30;
constexpr int kSegments = kWidth / kSegmentPixels;
constexpr int kBandRows = 8;
constexpr int kBands = (kHeight + kBandRows - 1) / kBandRows;
static_assert(kSegments * kSegmentPixels == kWidth && kSegments <= 32 && kSegmentPixels % 2 == 0);
// Every segment starts on a word boundary (the buffer is alignas(4)), so it is hashed a word at a time.
static_assert((kWidth * sizeof(Pixel)) % 4 == 0 && (kSegmentPixels * sizeof(Pixel)) % 4 == 0);
// kHeight * kSegments hashes, read once a frame: PSRAM. Null: whole frames.
std::uint32_t* segmentHashes = nullptr;
bool segmentHashesTried = false;
// One band is sent whole every frame, whether it changed or not, a different one each time: the whole
// screen comes again every kBands frames (about 0.34 s at 50 fps) for about 1 ms of SPI a frame, so
// nothing the panel lost or a hash missed stays on screen. A whole frame at once would take 17 ms.
int refreshBand = 0;

// ESP-IDF builds with -fno-builtin-memcpy: std::memcpy is a call to the ROM even for one word, and
// hashing a frame that way took 3.6 ms of the UI's 20 (5.5 ms with the audio busy). __builtin_memcpy
// of an aligned word is one load: 0.4 ms (2026-11-05).
std::uint32_t HashSegment(const Pixel* pixels)
{
  std::uint32_t hash = 2166136261u;
  const auto* bytes = static_cast<const unsigned char*>(__builtin_assume_aligned(pixels, 4));
  for (int index = 0; index < kSegmentPixels; index += 2)
  {
    std::uint32_t word;
    __builtin_memcpy(&word, __builtin_assume_aligned(bytes + 2 * index, 4), sizeof(word));
    hash = (hash ^ word) * 16777619u;
  }
  return hash ^ (hash >> 15);
}

}  // namespace

void Present() {
  // The HAL waits for what it sends. Its first frame, and the one after a failed send, is always the
  // whole screen; it also turns the backlight on and applies its level when nothing changed.
  if (!segmentHashesTried)
  {
    segmentHashesTried = true;
    segmentHashes = new (memory::slow) std::uint32_t[static_cast<std::size_t>(kHeight) * kSegments]();
  }
  if (segmentHashes == nullptr)
  {
    (void)CardenzaHAL::Display::Present(frameBuffer.data());
    return;
  }
  std::array<CardenzaHAL::Display::Region, kBands> regions{};
  std::size_t count = 0;
  for (int band = 0; band < kHeight; band += kBandRows)
  {
    std::uint32_t changed = 0;
    int first = -1, last = -1;
    for (int y = band; y < std::min(band + kBandRows, kHeight); ++y)
    {
      std::uint32_t row = 0;
      for (int segment = 0; segment < kSegments; ++segment)
      {
        const std::uint32_t hash = HashSegment(frameBuffer.data() + y * kWidth + segment * kSegmentPixels);
        std::uint32_t& previous = segmentHashes[y * kSegments + segment];
        if (hash == previous) continue;
        previous = hash;
        row |= 1u << segment;
      }
      if (row == 0) continue;
      if (first < 0) first = y;
      last = y;
      changed |= row;
    }
    if (band / kBandRows == refreshBand)
    {
      changed = (1u << kSegments) - 1;
      first = band;
      last = std::min(band + kBandRows, kHeight) - 1;
    }
    if (changed == 0) continue;
    const int left = __builtin_ctz(changed);
    const int right = 31 - __builtin_clz(changed);
    const CardenzaHAL::Display::Region region{left * kSegmentPixels, first, (right - left + 1) * kSegmentPixels, last - first + 1};
    CardenzaHAL::Display::Region* above = count > 0 ? &regions[count - 1] : nullptr;
    if (above != nullptr && above->x == region.x && above->width == region.width && above->y + above->height == region.y)
      above->height += region.height;
    else
      regions[count++] = region;
  }
  refreshBand = (refreshBand + 1) % kBands;
  (void)CardenzaHAL::Display::Present(frameBuffer.data(), regions.data(), count);
}

void SetBacklight(std::uint8_t level) {
  // Perceived brightness; the HAL applies the gamma and keeps the level until the first frame.
  (void)CardenzaHAL::Display::SetBrightness(level);
}

std::uint8_t Backlight() {
  return CardenzaHAL::Display::GetBrightness();
}

}  // namespace Display

namespace Keyboard {

namespace {
KeySettle<static_cast<std::size_t>(kRows)> leftKeySettle;
std::uint32_t reportedGlitches = 0;
std::uint32_t reportedStuck = 0;
std::int64_t lastKeyReportAt = 0;
}  // namespace

const MatrixKeyState& State() {
  keyboardState.reset();
  CardenzaHAL::Keyboard::updateKeyList();
  for (const auto& key : CardenzaHAL::Keyboard::keyList())
  {
    if (key.y < 0 || key.y >= kRows || key.x < 0 || key.x >= kColumns)
    {
      continue;
    }

    const std::size_t index = static_cast<std::size_t>(key.y) * static_cast<std::size_t>(kColumns) + static_cast<std::size_t>(key.x);
    keyboardState.set(index);
  }
  // ESC, TAB, FN and CTRL decide what every other key means: only settled readings count.
  const std::int64_t now = esp_timer_get_time();
  for (std::size_t row = 0; row < static_cast<std::size_t>(kRows); ++row)
  {
    const std::size_t index = row * static_cast<std::size_t>(kColumns);
    keyboardState.set(index, leftKeySettle.Update(row, keyboardState[index], now));
  }
  // What the filter did, for telling a bad contact or signal from a bug. At most once a second.
  if ((leftKeySettle.Glitches() != reportedGlitches || leftKeySettle.Stuck() != reportedStuck) && now - lastKeyReportAt >= 1000000)
  {
    lastKeyReportAt = now;
    reportedGlitches = leftKeySettle.Glitches();
    reportedStuck = leftKeySettle.Stuck();
    std::printf("keyboard: left column glitches=%u stuck=%u\n", static_cast<unsigned>(reportedGlitches), static_cast<unsigned>(reportedStuck));
  }
  return keyboardState;
}

}  // namespace Keyboard

namespace Buttons {

bool HomePressed() {
  return CardenzaHAL::System::GetFuncButton();
}

}  // namespace Buttons

namespace LED {

void BeginFrame() { keyboardLedFrames.BeginFrame(); }
void Present() { PublishLedFrame(keyboardLedFrames.Present()); }

std::uint8_t Brightness(std::size_t row, std::size_t column) {
  if (!IsValidMatrixPosition(row, column)) {
    return kBrightnessOff;
  }

  return keyboardLedFrames.Current().levels[MatrixIndex(row, column)];
}

void SetKey(std::size_t row, std::size_t column, std::uint8_t brightness) {
  if (!IsValidMatrixPosition(row, column)) {
    return;
  }

  PublishLedFrame(keyboardLedFrames.SetKey(row, column, brightness));
}

void Clear() {
  PublishLedFrame(keyboardLedFrames.Fill(kBrightnessOff));
}

void Fill(std::uint8_t brightness) {
  PublishLedFrame(keyboardLedFrames.Fill(brightness));
}

void SetGlobalBrightness(std::uint8_t brightness) {
  PublishLedFrame(keyboardLedFrames.SetMaster(brightness));
}

std::uint8_t GlobalBrightness() {
  return keyboardLedFrames.Current().master;
}

void SetEnabled(bool enabled) {
  PublishLedFrame(keyboardLedFrames.SetEnabled(enabled));
}

bool Enabled() {
  return keyboardLedFrames.Current().enabled;
}

}  // namespace LED

namespace USB {

namespace {

std::atomic<std::uint32_t> g_midiIn{0}, g_midiOut{0};

// App messages from the computer, from the USB task (HandleAppReport) to the app (ReadAppMessage).
constexpr std::size_t kAppQueue = 8;
AppMessage g_appMessages[kAppQueue];
std::atomic<std::size_t> g_appHead{0}, g_appTail{0};

// The HAL's app reports (0xFF, or SysEx F0 23 7F): 'S' answers with what has gone through USB so far
// (tools/usb/cardenza_usb.py stats and test read it); everything else is for the app.
void HandleAppReport(const std::uint8_t* report, std::size_t length)
{
  if (length == 0) return;
  if (report[0] != 'S')
  {
    const std::size_t tail = g_appTail.load(std::memory_order_relaxed);
    const std::size_t next = (tail + 1) % kAppQueue;
    if (next == g_appHead.load(std::memory_order_acquire)) return;  // full: the newest goes
    AppMessage& message = g_appMessages[tail];
    // HID reports come padded with zeros: the text ends at the first one.
    message.length = 0;
    while (message.length < length && message.length < kAppMessageSize && report[message.length] != 0)
    {
      message.data[message.length] = static_cast<char>(report[message.length] & 0x7F);
      ++message.length;
    }
    g_appTail.store(next, std::memory_order_release);
    return;
  }
  const Audio::ComputerAudioStats audio = Audio::ComputerStats();
  char text[CardenzaHAL::USB::kReportSize] = {};
  // One report (62 characters at most); the heap and the endpoint counters are in the console.
  const int size = std::snprintf(text, sizeof(text), "S midi_in=%lu midi_out=%lu to_pc=%lu from_pc=%lu peak=%u",
                                 static_cast<unsigned long>(g_midiIn.load()), static_cast<unsigned long>(g_midiOut.load()),
                                 static_cast<unsigned long>(audio.framesToComputer), static_cast<unsigned long>(audio.framesFromComputer),
                                 static_cast<unsigned>(audio.peakFromComputer));
  (void)CardenzaHAL::USB::SendAppReport(reinterpret_cast<const std::uint8_t*>(text), static_cast<std::size_t>(size > 0 ? size : 0));
}

}  // namespace

bool Connected() { return CardenzaHAL::USB::Mounted(); }

bool ReadMidi(std::uint8_t packet[4])
{
  if (!CardenzaHAL::USB::MidiRead(packet)) return false;
  g_midiIn.fetch_add(1, std::memory_order_relaxed);
  return true;
}

bool WriteMidi(const std::uint8_t packet[4])
{
  if (!CardenzaHAL::USB::MidiWrite(packet)) return false;
  g_midiOut.fetch_add(1, std::memory_order_relaxed);
  return true;
}

bool ReadAppMessage(AppMessage& message)
{
  const std::size_t head = g_appHead.load(std::memory_order_relaxed);
  if (head == g_appTail.load(std::memory_order_acquire)) return false;
  message = g_appMessages[head];
  g_appHead.store((head + 1) % kAppQueue, std::memory_order_release);
  return true;
}

bool WriteAppMessage(const char* data, std::size_t length)
{
  if (!CardenzaHAL::USB::Mounted()) return false;
  return CardenzaHAL::USB::SendAppReport(reinterpret_cast<const std::uint8_t*>(data), std::min(length, kAppMessageSize));
}

void InstallAppChannel() { CardenzaHAL::USB::SetAppReportHandler(&HandleAppReport); }

}  // namespace USB

namespace Storage {

std::string RootPath() {
  return kCardRoot;
}

bool Mount() {
  if (CardenzaHAL::Storage::IsMounted()) {
    return true;
  }
  const std::int64_t now = esp_timer_get_time();
  if (mountAttempted && now - lastMountAttemptUs < kMountRetryUs) {
    return false;
  }
  mountAttempted = true;
  lastMountAttemptUs = now;
  return CardenzaHAL::Storage::Init();
}

bool IsMounted() {
  return CardenzaHAL::Storage::IsMounted();
}

void CheckAfterIoError() {
  (void)CardenzaHAL::Storage::CheckAfterIoError();
}

std::uint64_t CapacityBytes() {
  return IsMounted() ? CardenzaHAL::Storage::GetCapacity() : 0;
}

std::uint64_t FreeBytes() {
  std::uint64_t total = 0;
  std::uint64_t free = 0;
  return IsMounted() && esp_vfs_fat_info(kCardRoot, &total, &free) == ESP_OK ? free : 0;
}

}  // namespace Storage

std::string Name() {
  return "Cardenza";
}

std::string BackendName() {
  return "hardware-hal";
}

bool Init() {
  const bool initialized = CardenzaHAL::Init();
  // One composite device: MIDI, audio both ways, HID, and the HAL's system channel (console, restart
  // into the Launcher or the ROM download mode).
  CardenzaHAL::USB::Config usb;
  // What a computer lists the device, its MIDI ports and its audio as. Host tools and the guide page
  // find it by "Cardenza" in the name.
  usb.product = "203 Systems Cardenza";
  // 0203:2040 (the HAL's default is 0x10C0). Host tools match the VID and the name, not this.
  usb.productId = 0x2040;
  // No HID keyboard: this firmware types nothing, and with one macOS lets host tools open the HID
  // interface only with the Input Monitoring permission.
  usb.keyboard = false;
  if (CardenzaHAL::USB::Init(usb)) USB::InstallAppChannel();
  // The USB console reopened stdout and stderr on itself: the log ring goes in front of it again.
  Log::Hook();
  if (!initialized) {
    return false;
  }

  // CardenzaHAL::Init has just tried to mount the card; Storage::Mount retries later. The SD card is
  // the only storage: internal flash holds the firmware and nothing else of ours.
  mountAttempted = true;
  lastMountAttemptUs = esp_timer_get_time();
  return true;
}

void Start() {}

}  // namespace Device

// libstdc++'s nothrow new calls plain new and catches what it throws, but this firmware is built
// without exceptions: the throw itself aborts, so every `new (std::nothrow)` that ran out of memory
// rebooted the device. These replace them for the whole program: out of memory is a null pointer.
// A large block must not take the internal RAM the small plain allocations need: below the floor it
// goes to PSRAM (slower, but there). Any block goes to PSRAM when internal RAM has none, and is null
// only when PSRAM is full too. Small nothrow blocks get no floor: libraries use them for what they
// need (a flash library refused to write when they had one, 2026-11-03); our own optional small
// pieces check for themselves (the effects' choruses and buses). Data that belongs in PSRAM asks for
// it (Library/Memory/Memory.h); malloc itself never goes there, so C code and the drivers stay in
// internal RAM.
namespace {

constexpr std::size_t kLargeAllocationBytes = 4 * 1024;
constexpr std::size_t kHeapFloorBytes = 20 * 1024;

// Internal RAM, else PSRAM; a failure is logged (Log::NoteFailedAllocation) only when both are full.
void* AllocateInternalOrSlow(std::size_t size)
{
  return heap_caps_malloc_prefer(size, 2, MALLOC_CAP_DEFAULT | MALLOC_CAP_INTERNAL, memory::kSlowCaps);
}

void* AllocateOrNull(std::size_t size)
{
  if (size == 0) size = 1;
  if (size >= kLargeAllocationBytes && memory::FastFree() < size + kHeapFloorBytes)
    return heap_caps_malloc(size, memory::kSlowCaps);
  return AllocateInternalOrSlow(size);
}

}  // namespace

// Plain new as well: internal RAM, PSRAM when that is full, and the end of the program (as before:
// the firmware has no exceptions) only when both are.
void* operator new(std::size_t size)
{
  if (void* block = AllocateInternalOrSlow(size != 0 ? size : 1)) return block;
  std::abort();
}

void* operator new[](std::size_t size)
{
  return operator new(size);
}

void* operator new(std::size_t size, const std::nothrow_t&) noexcept
{
  return AllocateOrNull(size);
}

void* operator new[](std::size_t size, const std::nothrow_t&) noexcept
{
  return AllocateOrNull(size);
}
