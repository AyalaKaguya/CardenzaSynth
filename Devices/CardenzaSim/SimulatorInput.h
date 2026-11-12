#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include "Devices/Device.h"

namespace CardenzaSim::Input {

void SetMatrixKey(std::size_t row, std::size_t column, bool pressed);
void SetHomeButton(bool pressed);
void ClearMatrixKeys();
#if defined(CARDENZA_NATIVE_UI)
void SendTouch(const Device::Touch::Event& event);
#endif
// Simulates taking the SD card out (false) and putting it back (true, the default). Out when the system
// starts (os::system::Begin), the whole run is one without a card, as on the device.
void SetCardInserted(bool inserted);
// A microphone for hosts without capture hardware: StartInput succeeds and frames of a test tone
// become readable as they are fed. False: no microphone at all, not even the host's own (a Windows
// host has one), so a test sees the device without one.
void SetMicrophonePresent(bool present);
void FeedMicrophone(std::size_t frames);

// A computer on the USB port (Device::USB): plug it in or out, send the device MIDI packets, and take
// what the device sent back.
void SetComputerConnected(bool connected);
void SendComputerMidi(const std::uint8_t packet[4]);
bool TakeDeviceMidi(std::uint8_t packet[4]);
// App messages (Device::USB::ReadAppMessage / WriteAppMessage): the guide page or a test talks to the
// firmware. They work without SetComputerConnected (the browser is the computer).
void SendComputerApp(const std::string& message);
bool TakeDeviceApp(std::string& message);

}  // namespace CardenzaSim::Input
