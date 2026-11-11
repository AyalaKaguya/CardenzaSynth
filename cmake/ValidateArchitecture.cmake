if(NOT DEFINED CARDENZA_ROOT)
    get_filename_component(CARDENZA_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
endif()

function(cardenza_require_file relative_path)
    if(NOT EXISTS "${CARDENZA_ROOT}/${relative_path}")
        message(FATAL_ERROR "Required architecture file is missing: ${relative_path}")
    endif()
endfunction()

function(cardenza_forbid_path relative_path)
    if(EXISTS "${CARDENZA_ROOT}/${relative_path}")
        message(FATAL_ERROR "Forbidden architecture path still exists: ${relative_path}")
    endif()
endfunction()

function(cardenza_require_text relative_path expected_text)
    cardenza_require_file("${relative_path}")
    file(READ "${CARDENZA_ROOT}/${relative_path}" file_content)
    string(FIND "${file_content}" "${expected_text}" match_index)
    if(match_index EQUAL -1)
        message(FATAL_ERROR "${relative_path} does not contain expected architecture marker: ${expected_text}")
    endif()
endfunction()

function(cardenza_forbid_text relative_path forbidden_text)
    cardenza_require_file("${relative_path}")
    file(READ "${CARDENZA_ROOT}/${relative_path}" file_content)
    string(FIND "${file_content}" "${forbidden_text}" match_index)
    if(NOT match_index EQUAL -1)
        message(FATAL_ERROR "${relative_path} contains forbidden architecture coupling: ${forbidden_text}")
    endif()
endfunction()

cardenza_require_file("AGENT.md")

# With PSRAM the firmware is built with -mdisable-hardware-atomics: GCC turns std::atomic_flag::test_and_set
# into a plain load and store there (no library call exists for it), which froze the device (2026-11-04).
file(GLOB_RECURSE cardenza_own_sources
    "${CARDENZA_ROOT}/Applications/*.cpp" "${CARDENZA_ROOT}/Applications/*.h"
    "${CARDENZA_ROOT}/OS/*.cpp" "${CARDENZA_ROOT}/OS/*.h"
    "${CARDENZA_ROOT}/Library/Synth/*.cpp" "${CARDENZA_ROOT}/Library/Synth/*.h"
    "${CARDENZA_ROOT}/Library/Memory/*.h"
    "${CARDENZA_ROOT}/Devices/*.h" "${CARDENZA_ROOT}/Devices/Cardenza/*.cpp" "${CARDENZA_ROOT}/Devices/Cardenza/*.h")
foreach(source IN LISTS cardenza_own_sources)
    if(source MATCHES "/(managed_components|build|CMakeFiles)/")
        continue()
    endif()
    file(READ "${source}" source_text)
    string(FIND "${source_text}" "atomic_flag " flag_index)
    string(FIND "${source_text}" ".test_and_set(" test_index)
    if(NOT flag_index EQUAL -1 OR NOT test_index EQUAL -1)
        message(FATAL_ERROR "${source} uses std::atomic_flag / test_and_set, which is not atomic in the PSRAM build: use std::atomic<bool>::exchange")
    endif()
endforeach()
cardenza_forbid_path("Devices/CardenzaSim/WebUI")
cardenza_forbid_path("Library/Synth/UI")
cardenza_forbid_path("third_party/amy")

set(cardenza_firmware_only_sources
    "Devices/Cardenza/CardenzaHAL/cardenzaHAL.cpp"
    "Devices/Cardenza/CardenzaHAL/audio/audio.cpp"
    "Devices/Cardenza/CardenzaHAL/audio/codec/es8156.cpp"
    "Devices/Cardenza/CardenzaHAL/display/display.cpp"
    "Devices/Cardenza/CardenzaHAL/ir/ir.cpp"
    "Devices/Cardenza/CardenzaHAL/keyboard/internal/keyboard_hardware.h"
    "Devices/Cardenza/CardenzaHAL/mic/mic.cpp"
    "Devices/Cardenza/CardenzaHAL/storage/storage.cpp"
    "Devices/Cardenza/CardenzaHAL/system/system.cpp"
)

foreach(relative_path IN LISTS cardenza_firmware_only_sources)
    cardenza_require_file("${relative_path}")
    file(READ "${CARDENZA_ROOT}/${relative_path}" firmware_source)
    if(firmware_source MATCHES "#[ \t]*(if|ifdef|ifndef)[^\n]*ESP_PLATFORM")
        message(FATAL_ERROR "${relative_path} must use native ESP-IDF APIs without host fallback guards")
    endif()
endforeach()

cardenza_require_file("Devices/Cardenza/CardenzaHAL/keyboard/types.h")
cardenza_require_file("Devices/Cardenza/CardenzaHAL/keyboard/layout.h")
cardenza_require_file("Devices/Cardenza/CardenzaHAL/keyboard/internal/keyboard_key_scan.cpp")
cardenza_require_file("Devices/Cardenza/CardenzaHAL/keyboard/internal/keyboard_led_scan.cpp")
cardenza_require_file("Devices/CardenzaSim/DesktopHost/DesktopSimulatorInput.cpp")
cardenza_require_file("Devices/CardenzaSim/DesktopHost/DesktopSimulatorRender.cpp")
cardenza_require_file("Applications/Synth/Runtime/SynthRack.cpp")
cardenza_require_file("Applications/Synth/GrooveApp.cpp")
cardenza_require_file("Applications/Synth/UI/Groove/GrooveController.cpp")
cardenza_require_file("Applications/Synth/UI/Groove/Render/Gfx.cpp")
cardenza_require_file("Library/third_party/amy/src/amy.c")

# The HAL builds as its own component (its sources, TinyUSB and the rest come with it).
cardenza_require_text("Devices/Cardenza/CMakeLists.txt" "\"\${CARDENZA_HAL_DIR}\"")
cardenza_require_text("Devices/Cardenza/CMakeLists.txt" "\${CARDENZA_HAL_COMPONENT}")
cardenza_require_text("cmake/CardenzaSynthSourceLists.cmake" "DesktopHost/DesktopSimulatorInput.cpp")
cardenza_require_text("cmake/CardenzaSynthSourceLists.cmake" "DesktopHost/DesktopSimulatorRender.cpp")
cardenza_require_text("cmake/CardenzaSynthSourceLists.cmake" "Applications/Synth/Runtime/SynthRack.cpp")
cardenza_require_text("cmake/CardenzaSynthSourceLists.cmake" "Applications/Synth/UI/Groove/GrooveController.cpp")
cardenza_require_text("cmake/CardenzaSynthSourceLists.cmake" "Applications/Synth/GrooveApp.cpp")
cardenza_require_text("cmake/CardenzaSynthSourceLists.cmake" "Library/third_party/amy/src")
cardenza_require_text(".gitmodules" "path = Library/third_party/amy")
cardenza_require_text("Library/Synth/CMakeLists.txt" "Library/third_party/amy/src")
cardenza_require_text("Devices/Cardenza/CMakeLists.txt" "Library/third_party/amy/src")

cardenza_forbid_text("Devices/CardenzaSim/DesktopHost.cpp" "PressMatrixKey")
cardenza_forbid_text("Devices/CardenzaSim/DesktopHost.cpp" "ReleaseMatrixKey")
cardenza_forbid_text("Devices/CardenzaSim/DesktopHost/DesktopSimulatorInput.cpp" "SynthApp")

# The headless screenshot runner is portable and drives the system only through the device facade.
cardenza_require_text("cmake/CardenzaSynthSourceLists.cmake" "Headless/HeadlessShot.cpp")
foreach(relative_path
        "Devices/CardenzaSim/Headless/HeadlessScript.cpp"
        "Devices/CardenzaSim/Headless/HeadlessShot.cpp"
        "Devices/CardenzaSim/Headless/PngWriter.cpp")
    cardenza_forbid_text("${relative_path}" "windows.h")
    cardenza_forbid_text("${relative_path}" "Windows.h")
    cardenza_forbid_text("${relative_path}" "Applications/Synth")
endforeach()

file(READ "${CARDENZA_ROOT}/Devices/Cardenza/CMakeLists.txt" cardenza_cmake)
if(cardenza_cmake MATCHES "add_subdirectory\\(CardenzaHAL\\)")
    message(FATAL_ERROR "Cardenza root CMake path must not expose the ESP-IDF HAL as a host subdirectory")
endif()

# The groovebox UI is portable: it reaches the runtime only through groove::Host and draws only
# into the framebuffer, so it builds unchanged for the simulator, tests and firmware.
file(GLOB_RECURSE cardenza_groove_ui_sources RELATIVE "${CARDENZA_ROOT}"
     "${CARDENZA_ROOT}/Applications/Synth/UI/Groove/*.cpp" "${CARDENZA_ROOT}/Applications/Synth/UI/Groove/*.h")
foreach(relative_path ${cardenza_groove_ui_sources})
    cardenza_forbid_text("${relative_path}" "freertos/")
    cardenza_forbid_text("${relative_path}" "esp_")
    cardenza_forbid_text("${relative_path}" "Devices/Cardenza/")
endforeach()

# Storage protocol (docs/app-storage-protocol.md): apps and the synth library reach the card only
# through OS/Storage; the device layer only provides the card root, mounting and capacity.
cardenza_require_file("docs/app-storage-protocol.md")
foreach(relative_path
        "OS/Storage/Storage.cpp"
        "OS/Storage/Settings.cpp"
        "OS/Storage/DeviceSettings.cpp"
        "Devices/CardenzaSim/Storage.cpp")
    cardenza_require_file("${relative_path}")
    cardenza_require_text("cmake/CardenzaSynthSourceLists.cmake" "${relative_path}")
endforeach()
foreach(old_function "SampleDirectoryPath" "ProjectDirectoryPath" "PresetDirectoryPath" "EnsureDirectoryLayout")
    cardenza_forbid_text("Devices/Device.h" "${old_function}")
endforeach()
cardenza_forbid_text("Devices/Cardenza/Device.cpp" "/sdcard/CardenzaSynth")
file(GLOB_RECURSE cardenza_storage_client_sources RELATIVE "${CARDENZA_ROOT}"
     "${CARDENZA_ROOT}/Applications/*.cpp" "${CARDENZA_ROOT}/Applications/*.h"
     "${CARDENZA_ROOT}/Library/Synth/*.cpp" "${CARDENZA_ROOT}/Library/Synth/*.h")
foreach(relative_path ${cardenza_storage_client_sources})
    cardenza_forbid_text("${relative_path}" "Device::Storage")
endforeach()

# Persistence is SD-only: nothing is kept in internal flash (NVS). Owned sources only: the HAL
# submodule and third-party code are not ours, and the partition table stays as it is for the Launcher.
cardenza_forbid_text("Devices/Cardenza/CMakeLists.txt" "nvs_flash")
foreach(source_root "Devices" "OS" "Applications" "Library/Synth")
    file(GLOB_RECURSE cardenza_persistence_sources RELATIVE "${CARDENZA_ROOT}"
         "${CARDENZA_ROOT}/${source_root}/*.c" "${CARDENZA_ROOT}/${source_root}/*.cc"
         "${CARDENZA_ROOT}/${source_root}/*.cpp" "${CARDENZA_ROOT}/${source_root}/*.h"
         "${CARDENZA_ROOT}/${source_root}/*.hpp")
    foreach(relative_path ${cardenza_persistence_sources})
        if(relative_path MATCHES "^Devices/Cardenza/CardenzaHAL/" OR
           relative_path MATCHES "(^|/)(managed_components|build|CMakeFiles)/")
            continue()
        endif()
        file(READ "${CARDENZA_ROOT}/${relative_path}" persistence_source)
        if(persistence_source MATCHES "ReadInternal|WriteInternal|EraseInternal|ListInternal|kInternalKeyMax|kInternalValueMax")
            message(FATAL_ERROR "${relative_path} contains a removed internal-storage API; persistence must use the SD card")
        endif()
        string(TOLOWER "${persistence_source}" persistence_source_lower)
        if(persistence_source_lower MATCHES "(^|[^a-z0-9_])nvs[_./]")
            message(FATAL_ERROR "${relative_path} contains an NVS dependency; persistence must use the SD card")
        endif()
    endforeach()
endforeach()

message(STATUS "CardenzaSynth architecture guardrails passed")
