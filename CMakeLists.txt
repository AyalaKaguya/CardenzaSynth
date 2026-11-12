cmake_minimum_required(VERSION 3.20)

if(NOT DEFINED DEVICE)
  message(FATAL_ERROR "You must specify -DDEVICE=<device>")
endif()

set(FAMILY_PATH "${CMAKE_SOURCE_DIR}/Devices/${DEVICE}")
if(NOT EXISTS "${FAMILY_PATH}")
  message(FATAL_ERROR "Unknown device family: ${DEVICE}")
endif()

include(${CMAKE_SOURCE_DIR}/cmake/FirmwareVersion.cmake)
cardenza_read_firmware_version("${CMAKE_SOURCE_DIR}" CARDENZA_VERSION)
project(CardenzaSynth VERSION ${CARDENZA_VERSION} LANGUAGES C CXX)

option(CARDENZA_GROOVE_UI "Build the groovebox UI (Applications/Synth/UI/Groove); OFF builds the previous UI" ON)
option(CARDENZA_NATIVE_UI "Build the Mosaico 480x480 native UI" OFF)
if(CARDENZA_NATIVE_UI)
  add_compile_definitions(CARDENZA_NATIVE_UI=1)
endif()
if(CARDENZA_GROOVE_UI)
  add_compile_definitions(CARDENZA_GROOVE_UI=1)
endif()

include(${CMAKE_SOURCE_DIR}/cmake/CardenzaSynthSourceLists.cmake)

add_custom_target(ValidateArchitecture
  COMMAND ${CMAKE_COMMAND} -DCARDENZA_ROOT=${CMAKE_SOURCE_DIR} -P ${CMAKE_SOURCE_DIR}/cmake/ValidateArchitecture.cmake
  VERBATIM
)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(CardenzaDeviceAudio)
target_include_directories(CardenzaDeviceAudio PUBLIC
  ${PROJECT_SOURCE_DIR}
)

include("${FAMILY_PATH}/Config.cmake")

add_subdirectory(OS)
add_subdirectory(Library)
target_link_libraries(CardenzaDeviceAudio PUBLIC
  CardenzaSynthRuntime
)
add_subdirectory(Applications)
add_subdirectory(Devices)

if(DEVICE STREQUAL "CardenzaSim")
  include(CTest)
  if(BUILD_TESTING)
    add_subdirectory(tests)
  endif()
endif()
