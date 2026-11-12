add_executable(CardenzaCoreTests
  core_test.cpp
  ${PROJECT_SOURCE_DIR}/Applications/Synth/Runtime/ProjectStorage.cpp
  ${PROJECT_SOURCE_DIR}/Applications/Synth/Runtime/Sequencer/SynthSequencer.cpp
  ${PROJECT_SOURCE_DIR}/OS/Framework/Midi/MidiPacket.cpp
)
target_include_directories(CardenzaCoreTests PRIVATE ${PROJECT_SOURCE_DIR})
add_test(NAME CardenzaCoreTests COMMAND CardenzaCoreTests)

# Arpeggiator: the sequencer built with its test hooks, so the clock can be stepped by hand.
add_executable(CardenzaArpTests
  arp_test.cpp
  ${PROJECT_SOURCE_DIR}/Applications/Synth/Runtime/Sequencer/SynthSequencer.cpp
  ${PROJECT_SOURCE_DIR}/OS/Framework/Midi/MidiPacket.cpp
)
target_include_directories(CardenzaArpTests PRIVATE ${PROJECT_SOURCE_DIR})
target_compile_definitions(CardenzaArpTests PRIVATE CARDENZA_SEQUENCER_TEST_HOOKS=1)
if(MSVC)
  target_compile_definitions(CardenzaArpTests PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
  target_link_libraries(CardenzaArpTests PRIVATE pthread)
endif()
add_test(NAME CardenzaArpTests COMMAND CardenzaArpTests)

# HITS (a note fired 1..4 times in its step): playback on the hand-stepped clock, the lane edits, and
# the project formats 7 and 8 (the storage's test hook writes format 8 before the bump).
add_executable(CardenzaHitsTests
  hits_test.cpp
  ${PROJECT_SOURCE_DIR}/Applications/Synth/Runtime/ProjectStorage.cpp
  ${PROJECT_SOURCE_DIR}/Applications/Synth/Runtime/Sequencer/SynthSequencer.cpp
  ${PROJECT_SOURCE_DIR}/OS/Framework/Midi/MidiPacket.cpp
)
target_include_directories(CardenzaHitsTests PRIVATE ${PROJECT_SOURCE_DIR})
target_compile_definitions(CardenzaHitsTests PRIVATE CARDENZA_SEQUENCER_TEST_HOOKS=1 CARDENZA_PROJECT_TEST_HOOKS=1)
if(MSVC)
  target_compile_definitions(CardenzaHitsTests PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
  target_link_libraries(CardenzaHitsTests PRIVATE pthread)
endif()
add_test(NAME CardenzaHitsTests COMMAND CardenzaHitsTests)

# The SD-only storage protocol (2) on the simulator's card, with the card taken out and put back, and a
# run that starts without one.
cardenza_collect_storage_cpp_sources(${PROJECT_SOURCE_DIR} storage_test_sources)
add_executable(CardenzaStorageTests
  storage_protocol_test.cpp
  ${storage_test_sources}
  ${PROJECT_SOURCE_DIR}/Devices/CardenzaSim/Storage.cpp
)
target_include_directories(CardenzaStorageTests PRIVATE ${PROJECT_SOURCE_DIR})
add_test(NAME CardenzaStorageTests COMMAND CardenzaStorageTests)

# Settings on a card with faults: no card, insertion and replacement, I/O errors and oversized files.
# The test supplies its own Device::Storage.
add_executable(CardenzaSettingsSdOnlyTests
  settings_sd_only_test.cpp
  ${PROJECT_SOURCE_DIR}/OS/Storage/Storage.cpp
  ${PROJECT_SOURCE_DIR}/OS/Storage/Settings.cpp
)
target_include_directories(CardenzaSettingsSdOnlyTests PRIVATE ${PROJECT_SOURCE_DIR})
add_test(NAME CardenzaSettingsSdOnlyTests COMMAND CardenzaSettingsSdOnlyTests)

add_executable(CardenzaSampleStreamTests
  sample_stream_test.cpp
  ${PROJECT_SOURCE_DIR}/Library/Synth/Audio/SampleStream.cpp
)
target_include_directories(CardenzaSampleStreamTests PRIVATE ${PROJECT_SOURCE_DIR})
add_test(NAME CardenzaSampleStreamTests COMMAND CardenzaSampleStreamTests)

add_executable(CardenzaSamplerTests
  sampler_test.cpp
  ${PROJECT_SOURCE_DIR}/Applications/Synth/Runtime/Sampler/SamplerManager.cpp
  ${PROJECT_SOURCE_DIR}/Library/Synth/Audio/SampleStream.cpp
)
target_include_directories(CardenzaSamplerTests PRIVATE ${PROJECT_SOURCE_DIR})
add_test(NAME CardenzaSamplerTests COMMAND CardenzaSamplerTests)

set(cardenza_ui_tests)
if(NOT CARDENZA_GROOVE_UI)
add_executable(CardenzaUiTests
  ui_workflow_test.cpp
  ${PROJECT_SOURCE_DIR}/OS/UI/Canvas.cpp
  ${PROJECT_SOURCE_DIR}/OS/UI/BuiltInFonts.cpp
  ${PROJECT_SOURCE_DIR}/Applications/Synth/UI/Render/SynthUiRenderPrimitives.cpp
  ${PROJECT_SOURCE_DIR}/Applications/Synth/UI/SynthUiModel.cpp
  ${PROJECT_SOURCE_DIR}/Applications/Synth/UI/Menu/SynthUiMenu.cpp
  ${PROJECT_SOURCE_DIR}/Applications/Synth/UI/Dx7/Dx7UiModel.cpp
  ${PROJECT_SOURCE_DIR}/Applications/Synth/UI/Juno/JunoUiModel.cpp
  ${PROJECT_SOURCE_DIR}/Applications/Synth/UI/Sampler/SamplerUiModel.cpp
)
target_include_directories(CardenzaUiTests PRIVATE ${PROJECT_SOURCE_DIR})
add_test(NAME CardenzaUiTests COMMAND CardenzaUiTests)

cardenza_collect_synth_app_cpp_sources(${PROJECT_SOURCE_DIR} project_workflow_app_sources)
# The synth's storage folders are stubbed by the test (a flat scratch layout).
list(FILTER project_workflow_app_sources EXCLUDE REGEX "Renderer\\.cpp$|/UI/Render/|/Runtime/SynthStorage\\.cpp$")
cardenza_collect_synth_core_cpp_sources(${PROJECT_SOURCE_DIR} project_workflow_engine_sources)
cardenza_collect_midi_framework_cpp_sources(${PROJECT_SOURCE_DIR} project_workflow_midi_sources)
add_executable(CardenzaProjectTests
  project_workflow_test.cpp
  ${project_workflow_app_sources}
  ${project_workflow_engine_sources}
  ${project_workflow_midi_sources}
  ${PROJECT_SOURCE_DIR}/OS/MIDI/MIDI.cpp
  ${PROJECT_SOURCE_DIR}/Library/Synth/Audio/SampleStream.cpp
)
target_include_directories(CardenzaProjectTests PRIVATE ${PROJECT_SOURCE_DIR})
add_test(NAME CardenzaProjectTests COMMAND CardenzaProjectTests)

add_executable(CardenzaSequencerRenderTests sequencer_render_test.cpp)
target_link_libraries(CardenzaSequencerRenderTests PRIVATE CardenzaSystem CardenzaSimDevice)
add_test(NAME CardenzaSequencerRenderTests COMMAND CardenzaSequencerRenderTests)
set(cardenza_ui_tests CardenzaUiTests CardenzaProjectTests CardenzaSequencerRenderTests)
endif()

if(CARDENZA_GROOVE_UI)
  add_executable(CardenzaGrooveUiTests groove_ui_test.cpp)
  target_link_libraries(CardenzaGrooveUiTests PRIVATE CardenzaSystem CardenzaSimDevice)
  add_test(NAME CardenzaGrooveUiTests COMMAND CardenzaGrooveUiTests)
  list(APPEND cardenza_ui_tests CardenzaGrooveUiTests)

  # Exports pages, parameters and shortcuts as JSON for the Learn site's reference tables (tools/wiki/generate.py).
  add_executable(CardenzaLearnDump ${PROJECT_SOURCE_DIR}/tools/wiki/learn_dump.cpp)
  target_link_libraries(CardenzaLearnDump PRIVATE CardenzaSystem CardenzaSimDevice)

  # App switching and the groovebox's settings and session on the real system; and a start without a
  # card, which touches no card for the whole run.
  add_executable(CardenzaAppLifecycleTests app_lifecycle_test.cpp)
  target_link_libraries(CardenzaAppLifecycleTests PRIVATE CardenzaSystem CardenzaSimDevice)
  add_test(NAME CardenzaAppLifecycleTests COMMAND CardenzaAppLifecycleTests)
  add_test(NAME CardenzaNoCardLifecycleTests COMMAND CardenzaAppLifecycleTests --no-card)
  add_test(NAME CardenzaVolatileBootTests COMMAND CardenzaAppLifecycleTests --volatile-no-card)
  list(APPEND cardenza_ui_tests CardenzaAppLifecycleTests)
endif()

add_executable(CardenzaLedTests led_feedback_test.cpp)
target_include_directories(CardenzaLedTests PRIVATE ${PROJECT_SOURCE_DIR})
add_test(NAME CardenzaLedTests COMMAND CardenzaLedTests)

# The pure calculations the HAL runs on the device (key debounce and the key-count guard, LED frames,
# backlight curve), and the HAL's own sources against fake ESP-IDF drivers (hal_faults).
add_executable(CardenzaHalLogicTests cardenza_hal_logic_test.cpp)
target_include_directories(CardenzaHalLogicTests PRIVATE ${PROJECT_SOURCE_DIR})
add_test(NAME CardenzaHalLogicTests COMMAND CardenzaHalLogicTests)
add_executable(CardenzaKeyCountGuardTests key_count_guard_test.cpp)
target_include_directories(CardenzaKeyCountGuardTests PRIVATE ${PROJECT_SOURCE_DIR})
add_test(NAME CardenzaKeyCountGuardTests COMMAND CardenzaKeyCountGuardTests)
add_subdirectory(hal_faults)

# Headless screenshot runner: replay a real script against a scratch sdcard, and reject an
# unknown key target with its line number.
add_test(NAME CardenzaSimShotSmoke
  COMMAND CardenzaSimShot --script ${CMAKE_CURRENT_SOURCE_DIR}/groove/tour.script
          --out ${CMAKE_CURRENT_BINARY_DIR}/shot-smoke --root ${CMAKE_CURRENT_BINARY_DIR}/shot-smoke-root --scale 2
)
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/shot-unknown-key.script "wait 1\ntap nosuchkey\n")
add_test(NAME CardenzaSimShotRejectsUnknownKey
  COMMAND CardenzaSimShot --script ${CMAKE_CURRENT_BINARY_DIR}/shot-unknown-key.script
          --out ${CMAKE_CURRENT_BINARY_DIR}/shot-unknown-key
)
set_tests_properties(CardenzaSimShotRejectsUnknownKey PROPERTIES
  PASS_REGULAR_EXPRESSION "shot-unknown-key\\.script:2: unknown key target 'nosuchkey'"
)

foreach(target CardenzaCoreTests CardenzaStorageTests CardenzaSettingsSdOnlyTests CardenzaSamplerTests ${cardenza_ui_tests} CardenzaLedTests)
  if(MSVC)
    target_compile_options(${target} PRIVATE /UNDEBUG)
    target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS)
  else()
    target_compile_options(${target} PRIVATE -UNDEBUG)
    target_link_libraries(${target} PRIVATE pthread)
  endif()
endforeach()

# MSVC: a failed assert ends the test at once instead of waiting on the debug runtime's dialog
# (WindowsTestGuard.cpp).
if(MSVC)
  get_property(cardenza_test_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
  foreach(target ${cardenza_test_targets})
    get_target_property(target_type ${target} TYPE)
    if(target_type STREQUAL "EXECUTABLE")
      target_sources(${target} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/WindowsTestGuard.cpp)
    endif()
  endforeach()
endif()
