get_filename_component(CARDENZA_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
include("${CARDENZA_ROOT}/cmake/CardenzaSynthSourceLists.cmake")
set(CARDENZA_GROOVE_UI ON)
cardenza_collect_common_cpp_sources("${CARDENZA_ROOT}" common_sources)
cardenza_collect_amy_c_sources("${CARDENZA_ROOT}" amy_sources)
idf_component_register(
    SRCS ${common_sources} ${amy_sources}
         "../Device.cpp" "../DeviceAudio.cpp" "../app_main.cpp"
         "../ScreenCapture.cpp" "../Diagnostics.cpp"
    INCLUDE_DIRS "${CARDENZA_ROOT}" "${CARDENZA_ROOT}/Library/third_party/amy/src"
    REQUIRES esp-mosaico-bsp esp_mosaico_app_recovery esp_iris
             espressif__esp_lcd_touch esp_driver_uart esp_lcd esp_timer esp_psram esp_mm freertos pthread heap log vfs
)
cardenza_target_build_info(${COMPONENT_LIB} "${CARDENZA_ROOT}")
target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
set_property(TARGET ${COMPONENT_LIB} PROPERTY C_STANDARD 11)
set_property(TARGET ${COMPONENT_LIB} PROPERTY C_STANDARD_REQUIRED ON)
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    CARDENZA_GROOVE_UI=1 CARDENZA_MOSAICO=1
    AMY_WAVETABLE AMY_NO_MINIAUDIO AMY_NO_IRAM_PLACEMENT MALLOC_CAPS_DEFINED
    # Match this target's BSP I2S_CHANNEL_DEFAULT_CONFIG: six 240-frame descriptors.
    CARDENZA_AUDIO_QUEUE_FRAMES=1440
)
target_compile_options(${COMPONENT_LIB} PRIVATE $<$<COMPILE_LANGUAGE:C>:-fsingle-precision-constant>)
