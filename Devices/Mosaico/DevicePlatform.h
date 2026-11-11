#pragma once

namespace mosaico::display {

// True only after one complete original UI frame reached the panel transport
// and no later transfer failed. This is transport health, not visual acceptance.
bool Healthy();

}  // namespace mosaico::display
