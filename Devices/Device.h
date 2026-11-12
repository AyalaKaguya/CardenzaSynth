#pragma once

#include <bitset>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace Device {

namespace Display {

#if defined(CARDENZA_NATIVE_UI)
inline constexpr int kWidth = 480;
inline constexpr int kHeight = 480;
#else
inline constexpr int kWidth = 240;
inline constexpr int kHeight = 135;
#endif
inline constexpr std::size_t kPixelCount = static_cast<std::size_t>(kWidth) * static_cast<std::size_t>(kHeight);

using Pixel = std::uint16_t;

struct FrameBufferView {
  const Pixel* pixels = nullptr;
  std::size_t pixelCount = kPixelCount;
  int width = kWidth;
  int height = kHeight;
};

struct MutableFrameBufferView {
  Pixel* pixels = nullptr;
  std::size_t pixelCount = kPixelCount;
  int width = kWidth;
  int height = kHeight;

  operator FrameBufferView() const {
    return FrameBufferView{pixels, pixelCount, width, height};
  }
};

FrameBufferView CurrentFrameBuffer();
MutableFrameBufferView MutableFrameBuffer();
void Clear(Pixel color = 0);
void Present();

// Power-on level of the firmware HAL's backlight PWM (the light UI needs less than full).
inline constexpr std::uint8_t kDefaultBacklight = 180;

// Backlight PWM level, 0 (off) .. 255 (full). Takes effect immediately, or when the display
// starts if it has not been initialized yet.
void SetBacklight(std::uint8_t level);
std::uint8_t Backlight();

}  // namespace Display

namespace Touch {
enum class Kind : std::uint8_t { Down, Move, Up, Cancel };
struct Event {
  Kind kind = Kind::Cancel;
  std::uint8_t id = 0;
  std::int16_t x = 0;
  std::int16_t y = 0;
};
#if defined(CARDENZA_NATIVE_UI)
// Polled by the system owner. Drivers and remote input enqueue only; they do
// not enter the app or retain pointers to its UI state.
bool Poll(Event& event);
void CancelAll();
#endif
}  // namespace Touch

namespace Keyboard {

inline constexpr int kRows = 4;
inline constexpr int kColumns = 14;
inline constexpr int kKeyCount = kRows * kColumns;

using MatrixKeyState = std::bitset<kKeyCount>;

const MatrixKeyState& State();

}  // namespace Keyboard

namespace Buttons {

bool HomePressed();

}  // namespace Buttons

namespace LED {

inline constexpr int kRows = Keyboard::kRows;
inline constexpr int kColumns = Keyboard::kColumns;
inline constexpr int kLedCount = kRows * kColumns;
inline constexpr std::uint8_t kBrightnessOff = 0;
inline constexpr std::uint8_t kBrightnessMax = 15;
// Key levels are perceived brightness (0 off, 15 full); the device gamma-corrects them
// (Devices/LedLight.h). The global level is the KEY LED setting.

// Stage a complete update without blanking the currently displayed LED frame.
void BeginFrame();
void Present();
std::uint8_t Brightness(std::size_t row, std::size_t column);
void SetKey(std::size_t row, std::size_t column, std::uint8_t brightness);
void Clear();
void Fill(std::uint8_t brightness);
// Master intensity scales each key's level; it does not clip bright states together.
void SetGlobalBrightness(std::uint8_t brightness);
std::uint8_t GlobalBrightness();
void SetEnabled(bool enabled);
bool Enabled();

}  // namespace LED

namespace Audio {

inline constexpr int kOutputChannels = 2;
inline constexpr bool kHasInput = true;
inline constexpr std::uint32_t kDefaultInputSampleRate = 44100;
inline constexpr std::uint8_t kInputChannels = 1;
inline constexpr std::uint8_t kInputBitsPerSample = 16;

struct SampleBufferView {
  const std::int16_t* samples = nullptr;
  std::size_t frameCount = 0;
};

struct InputStreamInfo {
  std::uint32_t sampleRate = kDefaultInputSampleRate;
  std::uint8_t channelCount = kInputChannels;
  std::uint8_t bitsPerSample = kInputBitsPerSample;
};

enum class InputStatus {
  Ready,
  NotReady,
  InvalidArgument,
  Overflow,
};

inline constexpr std::uint8_t kDefaultOutputVolume = 100;

// Master output volume in percent; values above 100 are clamped. The device output stage applies
// it after the synth mix, so it never changes what the synth renders.
void SetOutputVolume(std::uint8_t percent);
std::uint8_t OutputVolume();

InputStreamInfo InputInfo();
InputStatus StartInput();
void StopInput();
InputStatus ReadInputFrames(std::int16_t* destination,
                            std::size_t frameCount,
                            std::size_t& framesRead,
                            std::uint32_t timeoutMs = 0);

// What a computer plays to the device over USB: interleaved stereo int16 frames at 44.1 kHz, its own
// mute and volume already applied. Returns the frames given; 0 when nothing plays. In a block where
// nothing reads it, the device takes it from USB and drops it, so the computer's stream keeps running
// for an app that does not use it.
std::size_t ReadComputerAudio(std::int16_t* stereoFrames, std::size_t frameCount);

// The output task's timing since it started (zeros on hosts, which have no real-time output): blocks
// rendered and the time they took, renders longer than a block (overruns, which the DMA queue covers),
// the times the queue ran dry (underruns: a click), and the longest render since the last call.
struct RenderTiming {
  std::uint32_t blocks = 0;
  std::uint32_t renderMicros = 0;  // wraps (about 71 minutes of rendering): take differences
  std::uint32_t overruns = 0;
  std::uint32_t underruns = 0;
  std::uint32_t longestRenderMicros = 0;
};
RenderTiming Timing();

}  // namespace Audio

// The USB port as a MIDI interface to a computer (USB-MIDI 1.0 event packets on cable 0: the code
// index number, then three MIDI bytes). The device also appears as an audio interface (Audio above,
// and the rendered sound goes to the computer) and a HID device; that is the device layer's business.
namespace USB {
bool Connected();
bool ReadMidi(std::uint8_t packet[4]);
bool WriteMidi(const std::uint8_t packet[4]);
// Messages between the firmware and a page or tool on the computer (the guide link in Applications/Synth/GrooveAppGuide.cpp):
// short 7-bit ASCII, over HID or as SysEx F0 23 7F <bytes> F7 (CardenzaHAL's app channel). Read takes
// the oldest waiting message (false when none); Write is false when it could not be queued.
inline constexpr std::size_t kAppMessageSize = 60;
struct AppMessage {
  std::size_t length = 0;
  char data[kAppMessageSize] = {};
};
bool ReadAppMessage(AppMessage& message);
bool WriteAppMessage(const char* data, std::size_t length);
}  // namespace USB

namespace Storage {

struct FileHandleView {
  const char* path = nullptr;
  bool writable = false;
};

// Low-level storage only: the SD card is the only place anything is kept (nothing in internal flash).
// Folders and files on the card belong to the OS storage protocol (OS/Storage,
// docs/app-storage-protocol.md); apps never call these functions directly.

// Root of the removable card: "/sdcard" on the device, "<working directory>/sdcard" in the
// simulator (beside the executable on Windows).
std::string RootPath();
// Mounts the card when it is not mounted. False when it is missing or cannot be mounted; the device
// may limit how often a missing card is probed.
bool Mount();
bool IsMounted();
// Call after a file operation failed, with the file closed: a card that stopped responding counts
// as missing from now on (Mount() then retries it); a healthy one (a full disk) stays mounted.
void CheckAfterIoError();
// Size and free space of the mounted card in bytes (0 when unknown).
std::uint64_t CapacityBytes();
std::uint64_t FreeBytes();

}  // namespace Storage

// The console log: what the firmware prints (printf, ESP-IDF log lines, the @K key trace). The device
// keeps the newest of it in a small ring that outlives a crash reset, and adds the crash itself (the
// reason, the task and a backtrace) there; OS/System/Log.h takes it out and writes it to the card
// while the LOG setting is on (docs/app-storage-protocol.md). Nothing here allocates.
namespace Log {
// Copies text nobody has taken yet, oldest first, at most `capacity` bytes, and returns how many (0:
// nothing new). `lost` gets the bytes overwritten before anyone took them.
std::size_t Take(char* destination, std::size_t capacity, std::size_t& lost);
// Bytes waiting to be taken (at most the ring's size).
std::size_t Pending();
// A line for the log and the console. The simulator's log holds only what comes through here.
void Write(std::string_view line);
}  // namespace Log

// One task below the UI for slow work nobody may wait for: what goes to the card while the device plays
// (the log, the autosave), a card taking seconds to accept a write then holds up nobody. Run registers
// `job` (once; a few at most) and wakes the task, which calls every registered job in turn, again if
// woken while busy; a job returns at once when it has nothing to do. Call it from the system loop.
// False where there is no such task (the simulator): the caller does the work itself.
namespace Background {
bool Run(void (*job)());
}  // namespace Background

std::string Name();
std::string BackendName();
bool Init();
void Start();

}  // namespace Device
