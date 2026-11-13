# Build the pinned BSP's original drivers for the peripherals this application uses.
# Motion sensors are outside the native synth's API; their binary-only BMI270
# dependency has no library for current ESP-IDF master (6.3).
get_filename_component(CARDENZA_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../.." ABSOLUTE)
set(bsp_source "${CARDENZA_ROOT}/tools/mosaico-vibe/submodule/esp-mosaico-bsp/components/esp-mosaico-bsp")
if(NOT EXISTS "${bsp_source}/onboard/esp_mosaico.c")
    message(FATAL_ERROR "Initialize the pinned ESP-Mosaico BSP submodule")
endif()

# Keep the BSP's public header and pin definitions, with the unused motion API
# omitted. Driver implementations remain in the upstream submodule.
file(READ "${bsp_source}/include/bsp/esp_mosaico.h" bsp_header)
foreach(sensor_header IN ITEMS imu magnetometer)
    set(sensor_include "#include \"bsp/${sensor_header}.h\"")
    string(FIND "${bsp_header}" "${sensor_include}" sensor_include_at)
    if(sensor_include_at EQUAL -1)
        message(FATAL_ERROR "BSP umbrella header changed; review the native peripheral profile")
    endif()
    string(REPLACE "${sensor_include}" "" bsp_header "${bsp_header}")
endforeach()
string(REGEX REPLACE "#define BSP_CAPS_IMU[ \t]+1" "#define BSP_CAPS_IMU 0" bsp_header "${bsp_header}")
string(REGEX REPLACE "#define BSP_CAPS_MAGNETOMETER[ \t]+1" "#define BSP_CAPS_MAGNETOMETER 0" bsp_header "${bsp_header}")
set(profile_include "${CMAKE_CURRENT_BINARY_DIR}/native_include")
file(MAKE_DIRECTORY "${profile_include}/bsp")
file(CONFIGURE OUTPUT "${profile_include}/bsp/esp_mosaico.h" CONTENT "${bsp_header}" @ONLY)

set(bsp_sources)
foreach(driver IN ITEMS esp_mosaico subboard audio nand_flash battery)
    list(APPEND bsp_sources "${bsp_source}/onboard/${driver}.c")
endforeach()
if(CONFIG_BSP_DISPLAY_ENABLE)
    list(APPEND bsp_sources "${bsp_source}/onboard/display.c")
endif()
idf_component_register(
    SRCS ${bsp_sources}
    INCLUDE_DIRS "${profile_include}" "${bsp_source}/include"
    REQUIRES esp_driver_gpio esp_driver_i2c esp_driver_i2s esp_driver_ledc esp_driver_spi
             esp_hw_support efuse esp_lcd esp_system esp_timer soc
)
