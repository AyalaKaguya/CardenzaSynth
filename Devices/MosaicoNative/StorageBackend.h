#pragma once

#include <cstdint>
#include "esp_err.h"

namespace mosaico::storage {

// One mount owns the NAND/SPI3/VFS for this application's lifetime. Mount never
// formats, grows or repairs an existing volume. All app paths are below RootPath.
esp_err_t Error();
std::uint64_t MountedVolumeBytes();

}  // namespace mosaico::storage
