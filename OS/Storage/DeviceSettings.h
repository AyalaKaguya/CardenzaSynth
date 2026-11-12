#pragma once

#include <cstddef>
#include <cstdint>

// Device-wide settings: kept in Settings/system.cfg (sys.volume, sys.led, sys.screen, sys.log,
// sys.theme), applied to the device at boot and shared by every app. Apps change them only through
// these functions; projects never carry them.
namespace os::storage {

// About -10 dB on the volume's curve (SetOutputVolume): as loud as 50 was on the old 20 dB scale.
inline constexpr std::uint8_t kDefaultOutputVolume = 70;
inline constexpr std::uint8_t kDefaultKeyLedBrightness = 15;
inline constexpr std::uint8_t kDefaultScreenBacklight = 180;
inline constexpr std::uint8_t kMinScreenBacklight = 10;
// The log on the card (OS/System/Log.h) is on unless someone turns it off.
inline constexpr bool kDefaultLogEnabled = true;

// The screen's colors. DARK is light on black (the shipped screen), LIGHT the same colors as ink on
// white paper, RETRO a backlit green LCD. Stored by name (`dark`, `light`, `retro`); anything else
// reads as the default.
enum class Theme : std::uint8_t {
  Dark,
  Light,
  Retro,
};
inline constexpr std::size_t kThemeCount = 3;
inline constexpr Theme kDefaultTheme = Theme::Dark;

// Reads the system settings and applies volume, key LEDs and backlight to the device (boot).
void ApplyDeviceSettings();

// Output volume 0..100 by ear: 0 mutes, 1..100 follow -60 (1 - v / 100)^1.5 dB (Device::Audio::
// SetOutputVolume): about -59 dB at 1, -21 dB at 50, 0 dB at 100, the quiet end wide, the top fine.
void SetOutputVolume(std::uint8_t percent);
std::uint8_t OutputVolume();
// Master brightness of the key LEDs, 0..15.
void SetKeyLedBrightness(std::uint8_t level);
std::uint8_t KeyLedBrightness();
// Screen backlight, kMinScreenBacklight..255.
void SetScreenBacklight(std::uint8_t level);
std::uint8_t ScreenBacklight();
// The console log written to the card (CardenzaSynth/Logs).
void SetLogEnabled(bool enabled);
bool LogEnabled();
// The screen theme. The app that draws applies it (only it knows its colors).
void SetScreenTheme(Theme theme);
Theme ScreenTheme();

}  // namespace os::storage
