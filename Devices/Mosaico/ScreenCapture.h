#pragma once

#include "esp_err.h"

namespace Device::Display { struct FrameBufferView; }

namespace mosaico::capture {

// Register the read-only Iris screenshot backend once during Device::Init.
// Capture allocation is on demand; no input/pointer backend is registered.
esp_err_t Init();

// Call only on the display owner, after a complete frame finished transmitting.
// The Iris worker never reads the application's mutable framebuffer directly.
void FramePresented(const Device::Display::FrameBufferView& frame);

}  // namespace mosaico::capture
