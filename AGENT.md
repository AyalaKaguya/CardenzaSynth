# CardenzaSynth agent instructions

## CardenzaHAL: changed upstream, consumed here

`Devices/Cardenza/CardenzaHAL` is the git submodule https://github.com/203-Systems/CardenzaHAL, the shared Cardenza hardware layer: every firmware (this one, Cardenza-MPE, ...) builds on it instead of carrying drivers of its own.

- **Changes go upstream.** Since 2026-11-02 (user decision) we maintain it: work in a separate clone `~/projects/CardenzaHAL`, push to upstream `main`, then `make hal-update` here and commit only the new submodule pointer. Changes there reach every consumer: keep them generic and compatible, and say what changed.
- **The submodule is a consumer copy.** Never edit, commit or push inside `Devices/Cardenza/CardenzaHAL`; never pin it to a commit that is not on upstream `main`. `make HAL_DEV=~/projects/CardenzaHAL DEVICE=Cardenza build` builds against the clone instead, to test a HAL change before it is pushed; such a build may go on the test board but is never shipped (no Launcher bin for users).
- **Reuse it.** Use what the HAL provides through its public headers, included from the HAL root (`"cardenzaHAL.h"`, `"keyboard/keyboard.h"`, `"audio/audio.h"`, `"mic/mic.h"`, `"usb/usb.h"`): keyboard scan and key LEDs (`setLED`, `setLEDGlobalBrightness`, `enableLED`), audio TX with mono / stereo `Init` and codec volume, mic RX channel, display panel handles and backlight, SD card mount, Func button, IR, USB (MIDI, audio, HID, update commands). Do not re-implement a driver or a HAL feature in this repository.
- **Missing or wrong in the HAL?** Fix it in the HAL clone. `Devices/Cardenza` (the device layer, ours) is for what is specific to this firmware; `Devices/LedLight.h` models the LED scan for the simulators.
- **Enforced.** `make hal-check` runs before every firmware build and the CMake configure checks too: they fail when the submodule has local changes or a commit that is not on upstream, and they set the submodule's push URL to `no-push://…` (the clone is where pushes come from).
- The HAL keeps its own rules in its `AGENT.md` (ESP-IDF only, native handles, no copyleft code); follow them when changing it.

## Workflow

- Do not start builds from the agent unless the user explicitly asks for one.
- Prefer static validation such as `get_errors`, targeted searches, and diff inspection when editing the Cardenza HAL layer.

### ESP-Mosaico target

- `Devices/Mosaico` is a separate ESP32-S31 BSP backend. It keeps the original 240x135 UI at 1:1, uses BSP audio output, and intentionally has no keyboard, microphone or application filesystem in the minimal version. Other targets keep their existing behavior.
- `tools/mosaico-vibe` pins the upstream workspace and its BSP/utils dependencies. Use the root `mosaico.py` launcher for its build/device workflows; see `Devices/Mosaico/README.md`. For Mosaico targets, the user requires the latest official ESP-IDF master at every build: fetch upstream, update the SDK and required submodules, then record that build's SHA. A recorded SHA is evidence for one build, never a pin for future builds. This overrides the dependency's historical SDK pin and earlier SDK-preservation guidance. Preserve unrelated SDK installations and OP-Ay1 project files.
- Preserve Mosaico's retained Vibe Mode ABI and partition prefix. Its system metadata is owned by the retained integration; the Cardenza SD-only application storage rules do not authorize replacing that system layout or introducing application persistence here. Device installation is a separate operation through the product CLI.

## Storage Rules

The spec is `docs/app-storage-protocol.md`; keep it in sync with behaviour changes.

- The SD card is the only persistent store (storage protocol 2, the 0.1.0 release's layout, decided by the user 2026-11-05): everything is below `<card>/CardenzaSynth/` (`Settings/system.cfg` and `synth.cfg`, `Projects/`, `Presets/<engine>/`, `Samples/`, `Session/`, `Logs/`; other apps in `Apps/<Author>/<Name>/`). Nothing goes to internal flash: no NVS, no internal records (ValidateArchitecture refuses them and `nvs_flash`). Do not change the partition table: it has to stay compatible with the Launcher.
- Apps store data only through the OS storage API (`OS/Storage`: `os::storage::AppFolder`, `AppSettings`, `SharedSamplesDirectory`, `WriteFileAtomic`), in their own app folder (the groovebox, the primary app, is the product root). Never hard-code card paths and never call `Device::Storage` from app code (enforced by ValidateArchitecture).
- `Device::Storage` is the low-level card (root, mount, free space, `CheckAfterIoError`). Folder knowledge belongs to `OS/Storage`, not to the device layer. After a failed file operation call `os::storage::ReportCardIoError()` (from the system loop) so the device can notice a card that stopped answering.
- The system decides once, at its start (`os::system::Begin`, `os::storage::DecideCardAtStart`), whether the run has a card. A run without one makes no card operation at all until a restart (no mount probe, `CardPresent()` / `CardMounted()` false, the file helpers refuse card paths, no log, no saves; a card put in later changes nothing), and the groovebox shows a notice after the boot animation; saving says NO SD CARD. Anything new that touches the card must go through `os::storage` or check `CardMissingThisRun()`.
- Settings are `key=value` text read tolerantly (unknown keys kept); change them in RAM and let the system write them (one second after the last change, on suspend). They are a RAM cache plus this device's edits: a flush reads the card file again and applies only the edits (keys changed elsewhere survive), writes nothing when that read fails, and `RefreshAllSettingsFromCard` merges a card that comes back. Volume, key LEDs, screen, log and theme are device settings (`os::storage::Set…`); projects never carry them. The keyboard, chords and arpeggiators are project data, not settings.
- An app persists its working state in `Suspend()` (its `Session/` folder) and restores it in `Setup()`; the system destroys the app before it creates the next one. The groovebox also writes it during use because users may pull the power at any moment: one autosave on the card (`SessionStore`: `Session/autosave.ccproj` in the project format plus `autosave.txt`), taken once the keys rest (1.5 s, playing or not; 60 s at most while keys keep coming or a sample streams) and written on the device's background task (`Device::Background`), never twice the same (`ProjectFingerprint`). A failed write is retried, calls `ReportCardIoError`, and never starts the wait over. `Setup` restores it; `Session/restoring.txt` counts starts that did not finish, and at 3 it is skipped once. New project data must be added to `CaptureProject` (and so to the project format), and every way to change a project must go through a key event (that is what starts a write).
- Groovebox project files are format 7 (format 6 made the play settings project data: keyboard, per-track chord and arpeggiator; format 7 gives every pattern its own length, up to 256 steps); version 5 was an unreleased development format with a PLAY line.
- Format 8 waits for the one format bump of the next test build (the user, 2026-11-11: features that change the format wait for it together). It adds HITS: each T line's new-note hits after its new-note length, and a seventh number on each note line, the note's hits. This build reads format 8 and writes 7 (`kProjectWriteVersion` in `Applications/Synth/Runtime/ProjectStorage.h` is the one constant to raise; the writer for 8 is there and tested). Until then a project saved, and the autosave, keeps no hits: a reload or a restart (or a Learn guide session) gives every note one hit again. Further format changes join format 8 before the bump; after it the next one takes 9.
- The 0.1.0 release's files are ours: its settings, projects, presets and samples are read and written in place (a project saved again becomes format 7, which 0.1.0 cannot open). The layouts in between (`<card>/Cardenza/`, protocol 1) and before 0.1.0 (`CardenzaSynth/{Project,Preset,Sample}`), and 0.1.0's `Session/session.*`, are neither read nor changed; any migration is discussed with the user first.
- Sample recordings are written straight to `Session/take-<track>.wav`, synced about once a second, and come back at the next start until saved or discarded. Samples longer than `sampler::RamSampleFrameLimit()` (2 s with PSRAM) play from their file through `audio::SampleStream` (blocks read through the file's descriptor: never read through an unbuffered `FILE`, newlib reads it a byte at a time, a 2 KB block took 33 ms and the stream worker held core 0 at 76% above the UI, 2026-11-04); rename, replace or delete a sample file only under an `audio::SampleFileGuard` (FAT refuses that for an open file).

## Memory

Two RAMs, `Library/Memory/Memory.h`. **Internal RAM** (about 280 KB of heap) is fast, DMA-capable and the only RAM usable while the flash is written (nothing of ours writes it any more: storage is on the card only): task stacks, driver state, the audio path (AMY's oscillators, blocks and event pool, the effects, the screen buffer) and every buffer the SD card reads into or writes from (its DMA cannot reach PSRAM; into PSRAM the driver falls back to one sector per command). **PSRAM**: the chip is an ESP32-S3R2 with 2 MB of quad PSRAM on the bus the flash code runs over (verified 2026-11-04; until then these notes said the board had none and it was off). It holds what is read per step, per screen or on demand: pattern steps (`StepStore` pages, `PatternData`), project snapshots (`ProjectState`), engines' parameter lists, presets and factory order (`PagedSynthEngine`), samples in RAM (`AllocateFrames`, up to `RamSampleFrameLimit()`, 2 s). Nothing touched per sample, by an interrupt or by the card's DMA goes there. It joins the heap with `MALLOC_CAP_SPIRAM` alone (`AddPsramToHeap` in `Devices/Cardenza/app_main.cpp`, `CONFIG_SPIRAM_USE_MEMMAP`): malloc and `MALLOC_CAP_DEFAULT` never reach it, so C code and the drivers stay internal; our code asks with `new (memory::slow)` or `memory::SlowAllocator` and checks with `memory::SlowRoom`. Measure internal RAM with `memory::FastFree()`: `heap_caps_get_free_size(MALLOC_CAP_8BIT)` counts PSRAM too. With PSRAM, ESP-IDF builds everything with `-mdisable-hardware-atomics`: atomics become library calls, except `std::atomic_flag::test_and_set`, which becomes a plain load and store (no library call exists for it) and froze the device as the effects' spinlock (2026-11-04). Never use `atomic_flag` (ValidateArchitecture refuses it); spin on `std::atomic<bool>::exchange`.

AMY's oscillator state goes through `cardenza_amy_osc_malloc` (internal RAM while 64 KB stay free, PSRAM after: in PSRAM a voice renders about a sixth slower) and is freed with its voice; AMY's event pool blocks the same way (`cardenza_amy_pool_malloc`); both through the fork's allocation hooks (`amy_external_osc_alloc_hook`, `amy_external_delta_alloc_hook`, set in `EnsureAmyInitialized`; `Library/Synth/Audio/Amy/AmyEmbeddedStubs.c`). Everything of a size is allocated when it is used and given back when it is not: steps of a pattern by page (`SynthSequencer::StepStore`, copy-on-write, a snapshot shares pages), the sample cache, the microphone ring (only while recording), delay and reverb lines. Never add large static buffers. With `-fno-exceptions` a failed `new` would abort the device, so `Devices/Cardenza/Device.cpp` replaces them: plain `new` spills into PSRAM when internal RAM is full and aborts only when both are; `new (std::nothrow)` returns null, and a nothrow block of 4 KB or more goes to PSRAM rather than leave less than 20 KB of internal RAM (the small plain allocations need that room). Use `new (std::nothrow)` for anything large and report the failure on screen. Small nothrow blocks have no floor there (libraries use them); optional pieces of ours check for themselves (pattern pages keep 256 KB of PSRAM, `kPatternSlowReserveBytes`, or 24 KB internal without PSRAM; the effects at 20 KB internal, `FxRoom`). Nothing per audio block may allocate. Host builds have one RAM and cannot show any of this: test running out of memory on the board.
An engine is hundreds of plain allocations: build one only through `GrooveApp::BuildEngine` (it checks the room with the size of that kind, learnt from what the heap gets back when one is destroyed, `DropEngine`; how much the heap shrinks while one is built counts other tasks too), never several beside the ones playing (`ReplaceSession` checks a whole project first, then swaps one track at a time; four new engines beside four old rebooted the board on loading a 0.1.0 project). Keep an engine's blocks small: its parameters are a deque (a DX7's list in one block is 9.5 KB, which a fragmented heap does not have); a new engine states its count with `ReserveParameters(count)`; preset values are stored compactly (`PresetValue`). The effects give their memory back while a project is built (`synth::audio::SuspendEffects` / `ResumeEffects`, which allocates before it returns), and a load or NEW that does not fit first parks the current patterns in the autosave on the card (`GrooveApp::MakeRoom`, `RestoreSessionPatterns` when it still fails; without a card, or during a lesson, it is NO MEMORY). `GrooveApp::UseHeapProbe` lets host tests run out of memory on purpose.

## Log

- Everything the firmware prints is the log: the device keeps it in an RTC ring that survives a crash (with the crash's `!!!` lines and backtrace, `Devices/Cardenza/DeviceLog.cpp`), and `OS/System/Log` appends it to `CardenzaSynth/Logs/log.txt` while SETUP > SYSTEM > LOG (`sys.log`, on by default) is on and the run has a card. Users send that file; read it before guessing.
- Print a line where something happens that would explain a problem (a load, a save, a refusal, a failure: `Groove: …`, `ui: …`), never per audio block or per frame. Periodic reports (`audio timing`, `usb audio`, `Groove: render` / `leds`) print when they have news and otherwise every 5 minutes: every line goes to the card, and printed every few seconds they kept an idle device writing the log every 5 s. Keep the writer cheap: no heap buffer, bounded appends, on the device's background task below the UI (`Device::Background`: a card busy streaming a sample took seconds per append, which froze the UI when it wrote from the system loop; the UI loop gives it and the idle task a tick after three frames late in a row, `Devices/Cardenza/app_main.cpp`, or long frames left them nothing), and none while a sample plays from the card (`os::storage::SetCardStreaming`). The autosave prints `Groove: autosave ...` when it fails, is slow (half a second on the card) or comes after failures.
- Keep the ELF of every bin given to anyone: the backtrace is decoded with it (`xtensa-esp32s3-elf-addr2line -pfiaC -e CardenzaSynthFirmware.elf <addresses>`); the log's first line of every start names the version (`version.txt`), the Git commit (`+`: built with local changes) and the ELF hash. A release raises `version.txt` (major.minor.patch, read by CMake into the app description and `CardenzaBuildInfo.h`); SETUP > SYSTEM shows the version and the build. CI (`.github/workflows/firmware.yml`, ESP-IDF 5.3.5) builds every push and runs the host tests; a push to main publishes a Nightly to the Launcher hub and a tag `vX.Y.Z` (equal to `version.txt`) the release, its bin on the GitHub Release; the run's artifact keeps the bin's ELF for 90 days. It needs the repository secret `LAUNCHER_APP_TOKEN` (the Cardenza Synth App Token); the submodules are public.

## Sound Rules

Keep the audio and mixing constraints below in sync with behaviour changes.

- All three render paths end in `WriteOutputNoLock`: the line input (a computer playing over USB, `SetLineInput`; for now the app installs none and the device drops what nobody reads, the user 2026-11-06) joins at unity, then `kOutputGain`, then the limiter. Never clip or saturate anywhere after the mix, and keep levels equal between the paths.
- `PresetLevels.inc` evens out the factory presets; regenerate it when a patch, the Juno mapping or a gain changes (`PRINT_PRESET_LEVELS=1 SynthRegression > Library/Synth/Audio/Amy/PresetLevels.inc`; clear the file before compiling). Across an AMY update carry it instead (`tools/audio/carry_preset_levels.py`: each preset moves only by how much its raw level moved) and compare the two builds' presets with `tools/audio/compare_preset_profiles.py`. `kJuno` is ours and may differ from AMY's ROM: `VoiceJunoPatchNoLock` sends it over the ROM string.
- Voices render on two cores (`RenderAmyVoicesNoLock`) and the instruction cache is 32 KB because of it. A voice stays whole on one core: its first oscillator holds the filter and the envelope and renders the others, which sound raw when anything renders them on their own; each sounding voice is a run of oscillators, and both cores take runs from a shared list (heaviest first) until none is left. AMY's hottest code is in IRAM (`Devices/Cardenza/audio_hot.lf`, code only, about 25 KB of internal RAM; what our engines never run or run only at note-on stays in flash) and its C sources build with `-fsingle-precision-constant` (its `1.0` literals made the per-block control code software double precision). The sequencer clock is a separate task on absolute time: nothing in rendering may hold it up. Rendering stays within the block: on the board each engine's cost per voice is learnt, the model is corrected by the measured render times (`g_budgetScale`), and when the next block would not fit (92% of it on average) the least needed voices fade out over one block (`KeepVoicesWithinBudgetNoLock`, `VoiceFadeVictimNoLock`): always from the track with the most voices sounding, its let-go tails first, then its quietest held note. A track gives way only to itself: one track's notes never fade or take another's voices (the user, 2026-11-04). A voice is let go when AMY has set its note-off clock and unset its note-on clock (`VoiceReleased`; comparing the two clocks never found one, and from 1e9ce9b to 2026-11-04 the budget faded held notes: notes a listener missed). A note not heard yet is left out (stopped before its first sample, no click) only when its block would run the output dry, and only the busiest track's. `RenderLoad` remains the safety net (`Groove: too much to render`) and fades the same way. Polyphony is what gives way, never the timing: it must never slow the clock or run the queue dry, and nothing heard may stop without a fade. Within a track a new note takes the voice of an older one when all are busy, a let-go one first (`VoiceCountFor`: DX7 4, Juno and drums 6, sampler 4; AMY 24 voices and 192 oscillators): fine by the user, and more voices only gave the budget more to fade. `GivenUpVoiceCounts` and the `Groove: render ... lost held= unheard= stolen= tails=` line count what was given up; a held note lost means the device ran out of time (`CurrentRenderModel` gives each engine's cost per voice).
- Setting up an engine or a preset costs AMY about 20 ms (it builds every voice). Each track's ROM patch is parsed once into memory patch 1024 + track (`CachedAmyPatchNoLock`); on the board the render thread applies one such heavy event only when the output is far enough ahead (`HeavyEventFitsNoLock`, the later events of that track wait, the others go on; while one waits the voice budget shrinks to 60% so the output gains the lead, and after 300 ms it goes anyway: an overloaded device never got ahead, and the notes queued behind an engine switch took the last internal RAM). The event queue is in PSRAM and holds at most 384 events (later note-ons count as not heard, note-offs always go), and building a project holds the output (`synth::audio::OutputHold` in `ReplaceSession` and `MakeRoom`: fade out, silence without the render lock, fade in). Nothing on the UI thread may render a block or hold the render lock for long while the output plays (`WarmUp` only applies events during a hold).
- AMY is the 203-Systems fork (`Library/third_party/amy`, https://github.com/203-Systems/amy), kept on upstream shorepine/amy (merged to 1.2.208 on 2026-11-05) with our commits on top; nothing patches it at build time. Change it like the HAL: in a separate clone (`~/projects/amy`), pushed to the fork's `main`, then commit the new submodule pointer here; never commit inside the submodule. Ours on top: the fixed-point 24 dB lowpass (the Juno's) with exact 64-bit products (half the time and more precise), a modulation source's control step once a block, hooks for where oscillator state and the event pool go, the event pool's block size (`MAX_DELTA_BLOCKS` / `DELTA_BLOCK_SIZE`, set in `cmake/CardenzaSynthSourceLists.cmake`), a ROM patch's text (`amy_builtin_patch_command`, for each track's memory patch 1024 + track), the PCM stretch window allocated on first use, and `AMY_NO_IRAM_PLACEMENT` (our `audio_hot.lf` places the hot path, not AMY's `IRAM_ATTR`). Upstream itself frees a released voice's oscillators and takes the amplitudes' log2 / 10^x from tables. Check a change to AMY's DSP against the original on the host (outputs, a double precision reference) and run `Library/Synth/tests` (`SynthRegression`, `SynthAmyControlTests`, the fork's own checks) before relying on it; AMY keeps its own notes in its `CLAUDE.md` (generated files to regenerate when `amy.h` changes). AMY no longer maps GM drum notes to its PCM kit: each drum pad names its sample (`SendAmyNoteEventNoLock`).
- Hot code on this chip: ESP-IDF builds everything with `-fno-builtin-memcpy` (and `-memset`), so `std::memcpy` is a call to the ROM even for one word: in a loop use `__builtin_memcpy` (with `__builtin_assume_aligned` it is one load; the screen hash took 3.6 ms a frame through `std::memcpy`, 0.4 ms after, `Devices/Cardenza/Device.cpp`). Its FPU has no division (a float divide is a ROM call, some 80 cycles: none per sample), 64-bit atomics are library calls behind one lock shared by both cores with interrupts off (keep atomics the render thread or the card worker use 32-bit), and a function called per sample and voice costs more than the sound (render a voice a block at a time, `RenderSamplerVoiceBlock`).
- Every note someone starts needs an owner that ends it, known to `GrooveApp::EndOrphanNotes` (`AddHeldPadNotes`, `AddSoundingNotes`, `Previewing`, `computerNotes_` for USB MIDI): that safety net silences notes nobody owns and logs it, so a new source of notes must be added there. Never send AMY note 0 (`SendAmyNoteEventNoLock`). `TestNoStuckNotes` fails when the net has to act.
- `make DEVICE=Cardenza stress` builds the on-board stress test (`Applications/Synth/StressTest.cpp`; never ship it): extreme projects, old project files and everything on the card loaded and played, live operations and a key storm, each scenario measured (`stress:` lines: underruns, notes lost, UI frames, late clock pulses, RAM). `STRESS_ONLY=card:DEMO-2,sampler STRESS_SECONDS=60` runs those alone. Its results survive restarts in RTC memory (`RTC_NOINIT_ATTR`), read after the run: an object kept there must be trivially default constructible, or its constructor clears it at every start. `STRESS_TRACE=worst STRESS_LOOPS=5` traces the heap in that scenario until it gets low (`stress: H` lines: internal RAM still held, by caller; decode with addr2line and the build's ELF). `stress: load` lines give each task's share of the CPU (run-time stats, on in stress builds only); the summary repeats as one short line a scenario (`stress: S`), since the USB console drops long bursts. Run it after any change to the audio path, memory or loading, and add a scenario for anything new a user can make heavy. Check audio load on the board, not on the host: flash timing decides it. The flash runs quad I/O at 120 MHz with the PSRAM at 120 MHz too (`sdkconfig.defaults`; the S3 has one clock for both). Quad I/O is what the Launcher's bootloader runs: the bootloader, not the app, sets how the cache reads the flash, and in DIO the cost of a block outside the voices was about 40% higher, more than AMY 1.2.208 had added; 120 MHz is tuned by the app at its start and took another 10-20% off (2026-11-05). `audio timing` in the log gives avg / max per 1024 blocks and `underruns`, the times the DMA queue ran dry (a click); 1024 blocks must take 5.94 s. It is printed for an interval with underruns and otherwise every 50 intervals (5 minutes). `Groove: render ... faded=` counts the voices the budget let go.

## Groovebox UI Rules

Keep the UI rules below in sync with behaviour changes.

- Fixed key roles: left column ESC / TAB / FN / CTRL (icons on the screen's left edge, in the key's color, no labels and no state marks: where you are is in the header and the page tabs), right column ADD / SUB / ENTER / PLAY (ADD and SUB's icons follow `Controller::AdjustKeysMean`: up / down for a value and for a list alike, ADD, the upper key, raising the value and going up the list; sideways and erase have their own; a new screen says which it is there), columns 1–8 always play the current track, columns 9–12 mirror the on-screen 4x4 cell grid.
- Four scenes in two kinds, numbered as on screen: 0 SETUP comes first (the project and files, plus settings for every track and the device: KEYS, SYSTEM), outside the ring, reached only with ESC + GRID column 1 (or the FN menu); ESC there goes back to the scene it came from. 1 INSTR (the current track's sound, with its own CHORD and ARP pages), 2 SEQ (what it plays) and 3 TAPE (all tracks together: session, song, mix, sends, master FX) form the ring a tap on ESC walks. The device boots on INSTR VOICE, every project on T1 (`kDefaultActiveTrack`, the drum machine). New music pages go in scenes 1–3 by what they act on (one track's sound / one track's notes / all tracks); new global or device settings go in SETUP. INSTR pages run in signal order: VOICE, CHORD, ARP (not on drums), engine pages, FX.
- Holding a left key turns the GRID into a map of what that key switches (ESC scenes by column, TAB tracks laid out like MIX, CTRL every page of the scene, FN the menu of the current screen); choices go on the GRID, not on the pads or number keys. Keep each scene at 16 pages or fewer so the CTRL map fits.
- Colors carry meaning: ESC red-orange = scenes / back, TAB cyan = tracks, FN yellow = second functions, CTRL violet = pages, green = playing, red = recording / danger. Selection is terminal inverse (ink background, field-colored text). Area fills use the 4x4 Bayer dither, not flat blocks. The screen has three themes (SETUP > SYSTEM > THEME, `sys.theme`: DARK, LIGHT, RETRO; the palette table under 视觉系统): draw with the names in `Render/Theme.h` by meaning (`kBlack` the field, `kInk` the mark), never a literal color or a color kept in a static, and check a visual change on all three.
- Nothing on the first level types: anything that needs a name (save as, rename) goes in the FN menu, the plain action picks a free name by itself (`FreePresetName`, `FreeTakeName`, `SONG nn`).
- The note page's step keys follow MatrixOS's sequencer, the same in ROLL and STEP and on drum tracks (only the drawing differs; `Controller::StepKeyPress`): a step is selected while its key is down (pads edit its notes, OK cycles the edit lane, ADD / SUB edit it, steps held together are edited together), a tap keeps it selected (a tap on the step selected alone lets it go), a hold lets it go at the release; pads held + a step pressed toggle their notes there (`SynthSequencer::ToggleStepNotes`). On selected steps a pad's notes are one group (a chord as one, `UiState::padEdit`): a pad none of whose notes a selected step holds puts them in as it goes down; a lit pad (a selected step holds any of them) acts at its release: a plain tap takes them out (`ToggleNotesOnSelectedSteps`: all or none, a step that holds any of the notes loses them all and gains none), never the press or the hold. Holding a lit pad turns ADD / SUB to its notes alone, one rule for every edit lane (`Controller::AdjustHeldPadNotes`; the user, 2026-11-11): the current lane (OK) for those notes only, on each selected step that holds them. NOTE moves them a key of the keyboard (a semitone; the next scale note on SCALE or with a scale lock; FN an octave), past notes the step already holds, a chord pad's notes becoming the next key's chord, and keeps moving the same notes while the pad is down; on a drum track it moves a held hit to the next / previous drum of the kit in pad order (BD SD CH OH CP TM CB MA, `kDrumNotes`; FN the same), past drums the step already holds, keeping its velocity, length and timing, never onto a pitch that is not one of the kit's pads. LEN / VEL / TIME / HITS change only their values (`SynthSequencer::AdjustNotesOnSelectedSteps`, which shares `AdjustLaneLocked` with the whole-step edit: a new lane goes there, not into the gesture). This is the only way to edit one note of a chord (the note scope, FN + ENTER / SCOPE, was removed, the user 2026-11-11): a note out of the pads' reach is reached by changing the octave (FN + 8 / I). A pad that met ADD / SUB or any other key but a pad keeps its notes at its release; ADD / SUB with a pad held never edit whole steps or flip the page, not even while they repeat after the pad let go. Nothing writes and moves on by itself (no step-record cursor): an untouched step is a rest.
- HITS (the user's 鼓点连击, 2026-11-11) is the fifth edit lane (OK: NOTE → LEN → VEL → TIME → HITS; not called ROLL, which names the piano-roll view): a note fires 1–4 times in its step, evenly and on whole pulses (12 / 8 / 6 apart: 32nds, sixteenth triplets, 64ths), the first hit on the note's own TIME and swing, the others after it. Each hit sounds for the note's length, up to the next hit; the last one to the end of a longer note, so a long note stutters in its first step and then rings (`SequencerHitGatePulses`). Hits fall at most two steps past their own (`MaxHitReachPulses`: TIME, swing and the spread), and the clock looks that far back for them, past the loop wrap too, with no state of its own (stop, mute and edits act at once; every hit's note-off is scheduled like any note's, the off before the next hit's on). Edited like every lane: ADD / SUB on the selected steps (1–4, no wrap, FN to the end), a held lit pad + ADD / SUB for its notes alone (HUD `HITS E3` `3`); an empty selected step edits NEW HITS, what notes written by hand start with (per track, like NEW LEN / NEW VEL). Recording (REC, a take, step recording, the guide's demo songs) writes one hit; copy, paste, DUP and quantise carry it. A note keeps its hits less one in two spare bits of its last byte (`NoteEvent::extraHits`; zero, a cleared slot, is one hit): a page of steps stays 768 bytes. On screen the piano roll cuts a note into its hits (a piece per hit as long as it sounds, each starting with a solid column in the track's colour, a black pixel between); the STEP view's GRID cells cut the step the same way; the drum machine's marks are 4 pixels wide, too narrow to cut in time, so they stack a stroke per hit (black between, like the strokes that mark a roll in drum notation); a melodic STEP row, too thin for the stack, cuts its mark once (how many: the GRID beside, the edit strip). Key LEDs do not change.
- Drum and melodic tracks both have the ROLL and STEP views (OK switches; each kind keeps its own, `RecordState::noteView` / `drumView`: melodic tracks start on ROLL, drum tracks on STEP). STEP is drawn as a machine (`DrawStepMachine`): a row per drum, or per note of the page, over the steps, beside the GRID for the row the pads chose; on a drum track it is the drum machine (`Controller::DrumLayout()`). Drum pads are drums and velocities: what they put into steps goes in at their row's velocity; the NOTE lane moves a hit only between the kit's drums (a held pad + ADD / SUB, see above), and ADD / SUB on whole drum steps leave the NOTE lane alone. There is no drum-only press logic (no tap-adds, no hold-to-edit velocity).
- Long lists are not kept in memory: a `Modal` holds the rows on screen (`firstItem`, `itemCount`, `Row()`), the preset browser asks the engine for them, and the Animator's snapshot copies only those. A list of 128 names is a contiguous block the device may not have, and a failed plain allocation reboots it.
- Holding a parameter's GRID cell turns the pads into its values (`Controller::CellParameter`, `CellPickerActive`): a new page with parameter cells adds them to `CellParameter` and `SetCellParameter`. A cell's own press action on a focused switch or choice runs on release (`CellPress::stepOnRelease`), so holding it never changes the value first.
- A pattern is 1–256 steps, shown one page (16 steps, `SequencerUiState::firstViewStep`) at a time: sequencer step numbers are absolute, `steps[]` is the page on the GRID. FN + ADD / SUB and the SEQ menu flip pages and set the length; the view follows the playhead until flipped by hand.
- The microphone takes the speaker's clock pin (GPIO43), so the device layer hands it over (`PauseOutputForInput` / `ResumeOutputAfterInput`) and the controller cancels a recording when its screen is gone.
- FN is the menu of the current screen (`Controller::Menu()`, `RunMenu`): held FN shows it on the GRID (FN + GRID runs an item), a tap keeps it open (`ModalKind::Menu`, GRID runs and closes). Rows 0–2 are the page's actions, row 3 is the same everywhere (SAVE, TAP, REC, SETUP / BACK). A new action for a screen goes into its menu (with its FN letter, if any); settings do not go in the menu, they go on a SETUP page. FN + CTRL is the previous page, FN + ADD / SUB big steps. In input screens (`FnYellowMode()`) FN is the yellow keycap layer instead (`; . , /` arrows, text cursor).
- Every Fn letter shortcut must be the `key` of a menu item, and every other Fn combination on the edge keys must appear in `Controller::FnEntries()` so the menu footer documents it.
- The left column is filtered in the device layer (`Devices/KeySettle.h`): a state must settle for 12 ms, and a key reading as held for over 15 s counts as released until it lets go. The filter logs what it did, which is how to tell a bad signal from a bug.
- Key LEDs are driven by CardenzaHAL (see the top of this file); it drives each key at 16 linear duties and gamma-corrects only the master level, so `Device::LED` key levels are perceived brightness and the device layer gamma-maps them (gamma 2.6, `Devices/LedLight.h`, also the simulators' model). Every key is off (`groove::kLedOff`, nothing there), half (`kLedHalf` = 6, the dimmest glow, HAL duty 1/15, there but not chosen) or full (`kLedFull` = 15, chosen / held / sounding / playing); Blink is the only effect (waiting). The Learn guide's hint: the keys a step asks for are full (not a pad the keyboard leaves silent: PIANO's gaps stay dark), its practice projects keep the user's keyboard (a lesson start's f1 takes a new project's) and every other key at most half, as the lesson page frames them and dims the rest; they blink at half Blink's speed while the step waits for the user (`LedEffect::Hint`) and stay lit once it is learned; an edge key in it goes full too (`Controller::ComputeLeds`, `GrooveApp::UpdateLeds`). Do not add in-between levels, breathing or press ripples, and never let an animation keep a key lit that the state says is off. One exception, asked for by the user: the pads of a drum track, where a column is a drum and its rows are velocities (`kDrumRowVelocity`): they rest at half like any pad (no brighter than a melodic track's: half is the HAL's dimmest duty, so a resting gradient only made them brighter, 2026-11-05), a held pad lights its column from the bottom up to its row, and a hit runs along the column (`Animator::DrumWave`). That spread is for the key lights only: on screen the pads keep their blocks and show how hard by dither density. The screen's edge strips mirror the edge-key LEDs through `Animator::LedShown`.
- While FN is held no key may do its plain function: a key either has an FN function or does nothing (and its LED is off). Every FN shortcut and menu item reports its result with `Controller::ShowHud` (name, value, bar) or a toast.
- Settings that apply to every track (keyboard layout, scale, system) live on the SETUP settings pages (`SettingsPage`, `UiState::settingsFocus`) as `keys.*` / `proj.*` / `sys.*` slots. Each track's MIDI effects are its own: the chord (`UiState::chords`, `chord.*` slots) and the arpeggiator (one per track in `SynthSequencer`, `arp.*` slots) act on the current track and sit on its INSTR pages. All are handled by `SettingValue` / `AdjustSetting`.
- The Learn site (`wiki/`, Docusaurus, laid out like 203 Systems' Matrix wiki; `make learn` builds it into `build/web/site/learn`, deployed with the web simulator) opens on the guide (`docs/learn/guide.json`, pages in `wiki/src/guide`, animations `wiki/src/guide/guide-anim.js`, lessons checked by `tools/wiki/lessons.py`): short lessons, few words and pictures, and a Cardenza on USB that follows them through the guide link (the guide pages ask for MIDI as they open and connect by themselves, no button, no simulator beside the lessons; one connection for the whole site, `wiki/src/guide/device.ts`, and leaving the lessons ends the guide on the device); the manual is its reference. Every device lesson step has `keys`, `done` and `demo`; `make learn` runs the demos on the firmware and fails when a step does not complete, so a behaviour change that breaks a lesson shows there. A new state the lessons check goes into `Controller::GuideState` and the guide rules here.
- The Learn manual (`wiki/docs/*.mdx`, its Chinese in `wiki/i18n/zh-Hans/docusaurus-plugin-content-docs/current/`) is an internal preview until release: do not update it with behaviour changes, only when asked. When it is updated it documents every key, page and setting in Chinese and English (the same chapters and section ids, `{#id}` on every `##` / `###`; `<Keywords k="..." />` and `docs/learn/synonyms.json` keep it searchable). In Chinese the product is 鸣片 (card paths, `CardenzaHAL` / `CardenzaSynth` and names the device or the Launcher show stay as they are). Reference tables and screenshots are generated from the code (`tools/wiki/learn_dump.cpp`, the shot script in `tools/wiki/shots.py`, both run by `tools/wiki/generate.py`); the build fails on broken links or anchors, unknown screenshots, section ids that differ between the languages or search regressions (`wiki/scripts/search.test.js`). Pictures follow the product's look (`wiki/src/components/cardenza`, usable on every page without an import): highlights with numbered markers and a numbered legend (`<Mark>` / `<KeyMark>`), key caps as the web simulator draws them (`<Key k="FN+6" />`), step strips (`<Strip>`); no hand-drawn rings or arrows. The device and its keys are drawn on the board itself (`wiki/static/img/device/cardenza-pcb.svg`, the PCB drawing: switches `key-KEY1`..`56` row by row, their LEDs, `key-FUNC` = HOME; `pcb.tsx`): a screenshot is shown in the device's display, the top of the board magnified about 3 times so the screen reads, over the board's keys; the key lights glow on a dimmed board, marked keys are framed and the rest faded, and the device map (`board.tsx`, the whole board at its own scale) shows what each key and screen area does on hover.
- Tracks are identified by color everywhere (`theme::TrackColor`); never color a specific track with a role color.
- Track effects are `fx.*` engine parameters registered by `PagedSynthEngine::RegisterTrackFxParameters()`; the master bus is `Host::MasterFx` (project data). Both are defined in `Library/Synth/Audio/SynthFx.h`.
- Danger (overwrite, delete, discard) always confirms with ENTER = yes, ESC = no.
- The UI must not include ESP-IDF headers or device HAL code (enforced by ValidateArchitecture); use `groove::Host`.
- Check visual changes with `CardenzaSimShot` screenshots before flashing.
- The device sends only what changed on screen (`Device::Display::Present` hashes each row in 30-pixel segments): a whole frame is 17 ms of SPI, most of a 20 ms frame (the screen runs at 50 fps; keys, LEDs and the app's tick every 10 ms, `os::system::RunFrame(draw)`). FreeRTOS ticks at 1000 Hz (`sdkconfig.defaults`, as 0.1.0 did): the HAL waits for each 8-row band it sends, and esp_lcd does not yield from the transfer's completion, so the waiting UI runs at the next tick; at 100 Hz a band took 10 ms instead of 1 and the UI drew at under 30 fps with frames up to 160 ms (2026-11-05). Never lower it. One band of eight rows is sent whole every frame, a different one each time, so the screen is refreshed completely about every 0.34 s. Screens that redraw everything each frame cost the full 17 ms again.

## Architecture

CardenzaSynth is organized around a small set of ownership boundaries. The firmware target is the source of truth for Cardenza hardware behavior; desktop code exists to simulate that device boundary, not to keep alternate hardware implementations alive.

### Dependency Direction

Allowed high-level direction:

1. `Applications` depend on `OS`, `Library`, and the public `Devices/Device.h` facade.
2. `Applications/Synth` owns the Cardenza groovebox product runtime, including `GrooveApp`, track rack wiring, mixer state and the groovebox UI.
3. `OS` depends on `Devices/Device.h` and application interfaces.
4. `Library` owns reusable domain code and vendor dependencies used by shared runtimes. Synth engine/core code must not include concrete device HAL headers.
5. `Devices/Cardenza` owns ESP-IDF firmware and hardware HAL code.
6. `Devices/CardenzaSim` owns desktop simulation of the public device facade.

Concrete rules:

- `Devices/Cardenza/CardenzaHAL` is the read-only CardenzaHAL submodule (see the hard rule at the top of `AGENT.md`): never edit, commit or push it; pull with `make hal-update`.
- Host simulation should go through `Devices/CardenzaSim` and mutate/query the public device facade.
- Application input should flow through `Device::Keyboard::State()` and `os::system::RunFrame()` dispatch, not through app-specific key-handler calls from simulator code.
- Synth engines expose synth concepts. Device audio transport is connected by provider/callback registration rather than engine public interfaces.
- App-specific synth UI/model/renderer code lives under `Applications/Synth/UI`, not under `Library/Synth`.
- Board pins live in the HAL's `board/pins.h` (read-only).
- Vendored AMY sources live under `Library/third_party/amy`.
- Files on the card follow the storage protocol (`docs/app-storage-protocol.md`): everything is on the SD card below `CardenzaSynth/`, nothing in internal flash; apps use `OS/Storage` and their own folder (the groovebox uses the product root, other apps `Apps/<Author>/<Name>/`); `Device::Storage` only provides the card root, mounting and capacity. The synth library reaches its folders through `synth::storage` (`Library/Synth/Core/SynthStorage.h`), which the hosting app defines.

### Build Entrypoints

- Cardenza firmware: open an exported ESP-IDF shell, then run `make DEVICE=Cardenza build`. The Makefile changes into `Devices/Cardenza` and invokes `idf.py` directly.
- Desktop simulator: `make DEVICE=CardenzaSim build`. On Linux, `CardenzaSimShot` replays key scripts headlessly and writes PNG screenshots (`tests/groove/tour.script`).
- Browser simulator: `make web` (`DEVICE=CardenzaWeb`, Emscripten from `~/emsdk`), then `make web-serve`. `Devices/CardenzaWeb` hosts the CardenzaSim facade in a page; the build has no threads, so its one coupling to the app is pumping the groove sequencer clock (`SynthSequencer::PumpClock`, `__EMSCRIPTEN__` only).
- Architecture guardrail check: `make validate-architecture`.

The root CMake project is for desktop-style targets such as `CardenzaSim`. The Cardenza hardware target is firmware-only and should be configured through ESP-IDF.

### Current Module Shape

- `Applications/Synth/Runtime`: app-owned synth rack and track wiring.
- `Applications/Synth/UI/Groove`: the groovebox UI. `GrooveController` (input, navigation, scenes, modals, LEDs) talks to the runtime only through `groove::Host`; `Render/` draws straight into the framebuffer. The previous UI (`Applications/Synth/UI/*` outside `Groove`, `SynthApp`) remains buildable with `-DCARDENZA_GROOVE_UI=OFF` until it is removed.
- `OS/Storage`: the storage protocol (paths, settings files, device settings, whether the run has a card). `OS/System`: app lifecycle and switching. The firmware has no launcher of its own; do not add one (the device has its own Launcher).
- `Library/Synth/Core`: engine interfaces, engine implementations, and factory.
- `Library/Synth/Audio`: runtime audio bridge, with AMY-specific code under `Audio/Amy`.
- `Library/third_party/amy`: vendored AMY sources consumed by the shared synth audio runtime.
- `Devices/Cardenza/CardenzaHAL`: the CardenzaHAL submodule (read-only): ESP-IDF modules for system, display, audio, keyboard, storage, mic and IR.
- `Devices/CardenzaSim/DesktopHost`: Win32 simulator layout, drawing, window lifecycle, input, and rendering modules.

`cmake/CardenzaSynthSourceLists.cmake` is still the compatibility source registry for ESP-IDF and root CMake collection. Prefer adding owned CMake targets or local source lists as modules mature instead of growing unrelated flat lists.


### Cardenza HAL

- `Devices/Cardenza` sources are ESP-IDF-only. Do not add `ESP_PLATFORM` preprocessor guards or fallback typedefs in that tree.
- CardenzaHAL (`Devices/Cardenza/CardenzaHAL`) is an upstream submodule we only consume: never edit, commit or push it, reuse its API instead of re-implementing drivers, and pull updates with `make hal-update` (hard rule at the top of `AGENT.md`).

### Agent Workflow

- Prefer static validation (`get_errors`, targeted searches, diff inspection) when changing the Cardenza device layer.

## Groovebox design reference

更新：2026-11-06。本节维护当前交互、页面和协议；与前面的项目规则一并更新。源代码在 `Applications/Synth/UI/Groove/`、`Applications/Synth/GrooveApp.*`。旧构建验收数据不作为当前版本的性能保证。

### 硬件

- 屏幕 240×135，RGB565，50 fps（20 ms 一帧）；按键、键灯和应用的 Tick 每 10 ms 一次，屏幕隔一次画一次。只发变化的行，另外每帧轮流整条重发一带（8 行），约 0.34 秒整屏刷新一遍。FreeRTOS 的 tick 是 1000 Hz（`sdkconfig.defaults`，和 0.1.0 一样）：HAL 每发一条 8 行的带子都要等 DMA 完成，esp_lcd 在完成中断里不切换任务，等着的界面要到下一个 tick 才醒；100 Hz 时一条带子要 10 ms（本来 1 ms），界面画不到 30 fps，一帧最长 160 ms（2026-11-05）。
- 键盘 4 行 × 14 列，每键一颗单色 LED（0–15 级）。另有侧边 HOME 键（GPIO0）。
- 实机超过 4 个矩阵按键时，HAL 保留已确认键并屏蔽可疑新按键（矩阵串扰会读出没按的键）；松键仍通过原有 3 ms 去抖处理。被屏蔽键稳定松开后才能重新按下，不补发仍按住的可疑键；HOME 独立，不计入上限。HAL 的纯计算（去抖、按键数上限、LED 帧、背光曲线）和它的驱动代码（假 ESP-IDF 驱动下注入失败）在主机上测（`tests/cardenza_hal_logic_test.cpp`、`tests/key_count_guard_test.cpp`、`tests/hal_faults`）。
- 单声道扬声器输出；麦克风；SD 卡。

```text
列:   0     1     2     3   4   5   6   7   8  |  9   10  11  12 |  13
行0  ESC    1     2     3   4   5   6   7   8  |  9   0   -   =  |  ADD(del)
行1  TAB    Q     W     E   R   T   Y   U   I  |  O   P   [   ]  |  SUB(\)
行2  FN   shift   A     S   D   F   G   H   J  |  K   L   ;   '  |  ENTER
行3  CTRL  opt   alt    Z   X   C   V   B   N  |  M   ,   .   /  |  PLAY(space)
         └──────── 演奏区 PADS 4×8 ────────┘  └─ 网格 GRID 4×4 ─┘
```

- **PADS（第 1–8 列）**：任何场景都演奏当前轨。文本输入时整块键盘按键帽字符打字。
- **GRID（第 9–12 列）**：屏幕中央 4×4 内容区与它一一对应，物理位置即屏幕位置：音序场景是 16 个 step，其它场景是当前页的 16 个直达槽（参数、Pattern、轨道、动作）。
- **左侧 4 键**：屏幕左边缘的 4 个键帽与它们逐行对齐，和右侧一样用图标表示，不写文字：ESC 返回箭头、TAB 跳到竖线的箭头（轨道）、FN 四宫格（菜单）、CTRL 叠放的两页（页面），图标用这个键的颜色。键帽底色是该键颜色的浅 dither，亮度和这个键的灯同步（`CapDither`，灭时几乎看不见、半亮也很浅、全亮时实色，图标变成底色）；按下时从下往上填成实色。当前在哪个场景、哪条轨、哪一页看状态行和底行的页签，键帽上不再显示。底行的页签和它的高亮块与左右两侧的键帽之间至少留 2 像素底色的边（`kFooterX` / `kFooterRight`）。
- **右侧 4 键**：自上而下为 ADD / SUB / ENTER / PLAY，屏幕右边缘是同样造型的 4 个键帽，dither 很浅，图标最突出。ADD 和 SUB 的图标说明它们此刻做什么（`Controller::AdjustKeysMean`）：调参数值和在列表里上下移动都是 ▲ / ▼（列表：INSTR 的 VOICE 页切音色、音色和采样浏览、TAPE 的 SESSION、SETUP 的 FILES），ADD 是上面的键，数值往大调、列表往上走（2026-11-03 起调数值也不再是 + / −）；横向移动是 → / ←（SEQ 音符页没有选中 step 时翻 GRID 的页、引擎选择、按住 CTRL 翻页、按住 TAB 换轨）；文本输入时最上面的键（键帽印 del）是删除，下面的键没有图标；确认框和常驻菜单里两个键不起作用，也没有图标。

### 全局按键

| 键 | 单击 | 按住 |
|---|---|---|
| ESC | 在音乐场景之间循环：1 INSTR → 2 SEQ → 3 TAPE → 1 INSTR，永远不进 0 SETUP；在 SETUP 里是回到进来之前的场景（弹窗打开时是取消；SEQ 有选中的 step 时先取消选择） | 场景地图：GRID 的四列就是四个场景（0 SETUP、1 INSTR、2 SEQ、3 TAPE），按该列任意键跳转；第一列 SETUP 只能从这里进 |
| TAB | 切到下一轨（1→2→3→4→1） | 轨道地图：GRID 每列一条轨道，行 0 选轨、行 1 静音、行 2 独奏、行 3 音序开关（与 MIX 页同一布局）；TAB + ADD / SUB 前后切轨 |
| FN | 打开 / 关闭当前画面的菜单（常驻，见下方「FN 菜单」） | 当前画面的菜单：FN + GRID 执行菜单项；PADS 上的字母键和四周的功能键是第二功能，底行列出。输入画面里按住 FN 是键帽上的黄色丝印层（方向键等） |
| CTRL | 切到下一个子页面（松开时生效）；FN + CTRL 上一个 | 页面地图：GRID 的 16 个键依次是当前场景的全部页面，按哪个就跳到哪页，松开后停在新页面；CTRL + ADD / SUB 前后翻页 |
| ADD / SUB | 加 / 减；列表中上一项 / 下一项 | 自动连发，持续按住会加速 |
| ENTER | 确认、进入、开关 | — |
| PLAY | 播放 / 停止 | — |
| HOME 侧键 | 暂不分配功能（全部停止用 Fn + ESC） | — |

- 按住阈值 250 ms。单击在松开时生效；ESC / TAB / CTRL / FN 的地图在越过阈值时立即出现。
- 统一规则：**按住左侧任一键时，用右侧 4×4（GRID）选择**。地图上的每个格子就在它对应的那个键的位置；按住期间 PADS 不发声。只有特殊组合留在左侧相关键上（Fn + 数字 / 字母、Fn + CTRL、TAB / CTRL + ADD / SUB）。
- **按住 FN 时，任何键都不做它不按 FN 时的事**：没有 Fn 功能的键什么也不做（灯也是灭的），有 Fn 功能的键只做 Fn 功能。菜单项和字母功能执行后在屏幕底部弹出一条反馈（名称、新值、刻度条，约 1 秒）。
- 修饰键按住期间被组合使用过，松开时不再触发单击动作。
- 自动连发：400 ms 后每 70 ms 一次，连发 12 次后步长 ×5。

#### FN 菜单

FN 是**当前画面的菜单**。按住 FN 时内容区换成菜单：4×4 的格子就在 GRID 上，FN + GRID 执行那一格；单击 FN 让同一个菜单常驻，GRID 执行一项后菜单关闭，再单击 FN 或按 ESC 也关闭（常驻时 ADD / SUB / ENTER 不起作用，演奏区照常发声）。状态行是「FN MENU」和这是哪个画面的菜单（如 SEQ ROLL）。

每格写名称、作用对象或状态，右上角黄色字是它的 Fn 快捷键（`OK` = Fn + ENTER，`PLAY` = Fn + PLAY）。用不了的项是虚线、灯灭；能用的半亮；开着的开关全亮反显。行 0–2 属于当前页面，行 3 在所有画面相同：

| 画面 | 行 0–2 |
|---|---|
| INSTR（所有页） | PRESETS 浏览、SAVE 存音色、SAVE AS、ENGINE；RESET（OK，焦点参数恢复默认，只在参数页，CHORD / ARP 页也是）；Sampler 另有 SAMPLES、RECORD、PREVIEW |
| SEQ（所有页） | COPY（C）、PASTE（V）、CLEAR（X）、QUANT（Q）：对选中的 step，没有选中时对整个 Pattern；DUP（D）复制到下一个空位；VIEW（ROLL / STEP）；RECORD（RECORD 面板）；第 8 格空着；PAGE − / PAGE +（长 Pattern 翻页，音符页）；LEN − / LEN +（当前 Pattern 的长度） |
| TAPE SESSION | LAUNCH（发射光标所在行）、ADD（OK，正在播的 Pattern 记成 SONG 新一行） |
| TAPE SONG | COPY / PASTE（插在光标行后）/ DELETE / DUP 一行（C / V / X / D）；PLAY（OK，SONG / LOOP） |
| TAPE MIX | UNMUTE ALL、UNSOLO ALL |
| TAPE SENDS / FX | RESET（OK） |
| SETUP PROJECT / FILES | NEW、LOAD、SAVE AS、RENAME（PROJECT 页改当前工程的名字，FILES 页改光标所在的工程：卡上的文件直接改名，不留旧名，打开的工程照常继续；名字已被别的工程用了就提示 TAKEN，不覆盖；没存过的新工程是起名并保存） |
| SETUP KEYS / SYSTEM | RESET（OK，焦点设置恢复默认） |
| 行 3（所有画面） | SAVE（S，存工程）、TAP（T，Tap tempo，显示 BPM）、REC（PLAY，录音待命）、SETUP（进入 0 SETUP；在 SETUP 里是 BACK，回到进来之前的场景） |

- 快捷调节：最常用的三项不用找页面，按住 FN 直接调。演奏区最右边紧挨 GRID 的 2×3 块，上排加、下排减（和右侧 ADD 在 SUB 上面一样）：`6` / `Y` 调 BPM，`7` / `U` 调主音量，`8` / `I` 调八度。按住连续调，屏幕底部弹出新值，这 6 个键在按住 FN 时半亮。
- Fn + ADD / SUB 是大步调整：宽范围参数 ×10（BPM 一次 10），设置 ×5。
- 底行列出菜单以外的 Fn 组合（Fn + ESC / TAB / CTRL / PLAY / ADD / SUB 和快捷调节）。

**输入画面里的 FN**：文字输入、预设 / 采样列表、确认框等弹窗打开时，按住 FN 不出菜单，而是键帽上的黄色丝印层：`;` `.` `,` `/` 是上、下、左、右，ADD / SUB 一次翻一页。文字输入里 Fn + `,` `/` 左右移动光标，Fn + `;` `.` 跳到开头 / 结尾，Fn + del 删除光标后的字符（不按 FN 的 del 删除光标前的字符）；光标是字符之间的竖线，在末尾时是方块。

PADS 与四周按键上的第二功能：

| Fn + | 功能 |
|---|---|
| GRID | 执行菜单里这一格 |
| 6 / Y | BPM + / −（快捷键，任何画面都能用，按住连续调） |
| 7 / U | 主音量 + / −（同上） |
| 8 / I | 八度 + / −（同上） |
| ADD / SUB | 大步调整（可连发；输入画面里一次翻一页） |
| CTRL | 上一页 |
| ESC | 全部停止：停止音序、所有 ARP 和所有发声 |
| TAB | 上一轨 |
| ENTER | INSTR（含 CHORD / ARP）/ SENDS / FX / SETUP 设置页：当前参数恢复默认值；SESSION：把正在播的 Pattern 记成 SONG 的新一行；SONG：SONG / LOOP 切换；采样列表：打开改名 / 删除 |
| PLAY | 实时录音开关（REC） |
| C / V / X / Q / D | 菜单里带这个字母的项：复制 / 粘贴 / 清除（需确认）/ 量化 / 复制到下一个空位（TAPE SONG 中对整行） |
| T | Tap tempo |
| S | 保存工程 |

### 场景与子页面

四个场景，分两类，编号就是屏幕上的编号。最前面是 **0 SETUP**（工程与全局设置：工程、文件、键盘、系统），不在循环里，只能按住 ESC + GRID 第一列进入（或 FN 菜单的 SETUP 项），在里面单击 ESC 回到进来之前的场景。后面三个是音乐本身，单击 ESC 按 1 → 2 → 3 循环：**1 INSTR 乐器**（当前轨的声音，包括它自己的和弦与琶音器）、**2 SEQ 音序器**（当前轨弹什么）、**3 TAPE 走带**（四轨合在一起：Clip、整首歌的编排、混音、发送和总线效果）。按住 ESC + GRID 列切换场景；CTRL 单击在场景内循环子页面，按住 CTRL 打开页面地图直接跳页。页签显示在屏幕底部；页面放不下时只显示当前页附近的一段，两端用 `<` `>` 提示，右侧显示「当前 / 总数」。开机进入 **1 INSTR** 的第一页 VOICE，当前轨是 T1（鼓机，`kDefaultActiveTrack`）；NEW 和载入的工程也从 T1 开始。旋律轨（T2 起）的演奏区是钢琴排列：白键、黑键，缺口不亮。默认四轨为 T1 DRUMS、T2 JUNO、T3 DX7、T4 SAMPLER；NEW 也使用这一组合。每条轨道各自记住自己停在 INSTR 的哪一页。

#### 0 SETUP（工程与全局设置）

排在最前面：前两页是工程（保存、载入、速度），后两页是全局设置（所有轨道共用的键盘、设备）。按住 ESC + GRID 第一列进入，CTRL 换页，单击 ESC 回到进来之前的场景。演奏区照常发声，改设置能马上听到。

| 子页面 | 内容 | GRID | ADD / SUB / ENTER |
|---|---|---|---|
| PROJECT | 工程名、是否未保存、四轨摘要（引擎、音色） | 行 0：SAVE / LOAD（跳到 FILES）/ NEW（SAVE AS、RENAME 在 FN 菜单：要打字的操作都不放在一级界面）；行 1：BPM / SWING / TAP / PLAY（LOOP ↔ SONG 开关，SONG 时灯全亮） | 调整 BPM / SWING；ENTER 保存（焦点在 TAP 时是打拍） |
| FILES | 工程列表 | — | 选择；ENTER 载入（未保存时确认） |
| KEYS | 键盘预览（中间一个八度为主，两边各半个八度、颜色更暗：F 到 F）：音阶音带点、根音带横线，按下的音用当前轨颜色亮起 | LAYOUT、SCALE、ROOT、OCTAVE / VELO、LOCK、SELECT | 选中的设置：ADD / SUB 调整，ENTER 切换选项，Fn + ENTER 恢复默认；再按一次已选中的格子切换选项或开关 |
| SYSTEM | 音频后端、SD 卡、内存、构建日期（每次进来逐行打出） | VOLUME、KEY LED、SCREEN、LOG / THEME | 同上 |

键盘（LAYOUT、SCALE、ROOT、OCTAVE、VELO、LOCK）属于工程：随工程保存和载入，改动后工程变为未保存；NEW 的工程从默认键盘开始（PIANO、C MAJOR）。SELECT 是使用习惯，存在应用设置里（卡上 `CardenzaSynth/Settings/synth.cfg`）。BPM / SWING 属于工程，PLAY 与 REC 是演奏状态；SYSTEM 是设备设置（`CardenzaSynth/Settings/system.cfg`，所有应用共用）。当前轨的步数在 SEQ TRACK 页，和弦与 ARP 在每条轨道的 INSTR 页。

各项的取值：

- KEYS：LAYOUT PIANO（默认，黑白键）/ SCALE / CHROMA；SCALE 14 种音阶；ROOT C–B；OCTAVE −3…+3；VELO 演奏力度 1–127；LOCK OFF / MUTE / SNAP；SELECT ONE / MULTI。
- PROJECT：BPM 30–300；SWING 50–75 %；PLAY LOOP / SONG。
- SYSTEM：VOLUME 主音量 0–100，按听感：v 对应 -60 (1 - v/100)^1.5 dB（1 约 -59 dB，25 约 -39，50 约 -21，默认 70 约 -10，100 是 0 dB），小音量一端宽、大音量一端细（`Device::Audio::SetOutputVolume`；2026-11-06 以前只有 -20..0 dB，1 也很响；`sys.volume.rev` 3，旧的音量回到默认一次）；KEY LED 按键灯 0–15（选中它时，演奏区从左到右是亮度梯（感知等级 0、6、7、8、9、11、13、15，校正后是 HAL 的 0、1、2、3、4、7、10、15），方便用眼睛判断）；SCREEN 屏幕亮度；LOG 日志 ON / OFF（默认 ON，见本文 Log 规则）；THEME 屏幕配色 DARK / LIGHT / RETRO（默认 DARK，见「调色板与主题」）。

待机（屏保）已去掉（2026-11-04，用户要求）：机器一直显示当前的画面和键灯。

- 键盘布局：PIANO 是两个完整的八度（2026-11-04，用户要求；以前上下各错一行，最高和最低的八度都不完整）：每两行一个八度，白键 C D E F G A B C 在下、黑键在上（C# 在 D 上面），下面两行从左下角的 C3 开始，上面两行高一个八度（缺口处不发声）；屏幕上的键盘画同样的两个八度（C3 到 C5，`DrawMiniPiano`）。SCALE 只放音阶内的音，每行高一个八度；CHROMA 每格一个半音。
- 音阶：MAJOR、MINOR、DORIAN、MIXOLYD、MIN PENT、MAJ PENT、BLUES、HARM MIN、PHRYGIAN、LYDIAN、LOCRIAN、MEL MIN、WHOLE、IN SEN。
- LOCK：PIANO / CHROMA 布局下音阶外的键，MUTE 不发声（灯灭），SNAP 落到下方最近的音阶音。

#### 1 INSTR（乐器）

| 子页面 | 内容 |
|---|---|
| VOICE | 引擎、音色名（大字）、实时键盘（鼓轨是一行八个鼓垫，和键盘一样填满名字下面的地方）、八度 / 音阶。ADD / SUB 直接切换下一个 / 上一个音色并立即可试听；ENTER 打开音色浏览（可输入搜索）。GRID：ENGINE / PRESET；存音色、起名都在 FN 菜单 |
| CHORD | 这条轨道的和弦效果：根音上的一个键弹出哪几个音（键盘上标出），以及这个调里每一级的三和弦（小三和弦写作 `D-`，减三和弦 `BDIM`）。GRID：CHORD、VOICING |
| ARP | 这条轨道的琶音器：16 步的演奏顺序，按音高画成柱，宽度是 GATE；正在发声的一步为白色；ARP 关闭时整幅变暗。GRID：ARP（模式）、RATE、RANGE、GATE / HOLD |
| 引擎参数页 | 按引擎分组。GRID 槽直接选中参数，ADD / SUB 调值，Fn+ENTER 恢复默认；开关和选项再按一次同一格即切换。空槽显示该组的图形 |
| FX | 每个引擎都有：DRIVE、CRUSH、FILTER、RESO / CHORUS、DELAY、REVERB，下半屏是信号链图 |

页面按信号顺序排：VOICE（音色）→ CHORD → ARP（MIDI 效果，按键先经过它们）→ 引擎参数页 → FX。鼓轨没有 CHORD 和 ARP（一个键就是一个鼓）。

**按住参数格选值**：所有页面上的参数格（INSTR 各页、SETUP 的 KEYS / SYSTEM、BPM / SWING、MIX 第一行的音量、SENDS、FX）按住 0.2 秒，演奏区就变成它的档位：每个档位一个键，半亮，当前值全亮，没有档位的键灭；按下直接设到那一档，HUD 显示名字和值，松开参数格后演奏区变回键盘。档位多于 32 个时均匀分布在全部键上（范围是偶数时用 31 个键，这样正中间正好落在一个键上）。短按还是原来的行为：第一次选中，再按一次切换开关 / 步进选项（现在在松开时才执行，按住选值不会先跳一档）。按住期间按左列键，以左列键为准，不进入选值。

**HOLD（琶音器保持）**：ARP 页的 HOLD 格按一下就切换，不用先选中。打开后松手琶音继续；所有键松开后再按，换成新的一组音；关掉立刻停止（只留下还按着的音）。打开时这一格的灯全亮，图形右上角显示 HOLD，切轨后继续跑，FN + ESC 全部停止。

**和弦与琶音器是每条轨道自己的效果**：每条轨道有自己的 CHORD / VOICING 和自己的一台 ARP（模式、速度、范围、GATE、HOLD），互不影响；几条轨道的 ARP 可以同时在跑（例如 T2 的 HOLD 琶音继续，切到 T3 再弹另一组）。它们属于工程（随工程保存和载入），不随音色变，换音色或换引擎时保留。键盘布局、音阶、根音、八度、锁定和力度对所有轨道通用，在 0 SETUP KEYS。

- CHORD：CHORD OFF / TRIAD / 7TH / MAJ / MIN / SUS4 / MAJ7 / MIN7 / DOM7 / POWER / OCTAVE；VOICING ROOT / INV1 / INV2 / SPREAD。TRIAD / 7TH 在音阶内叠三度（音阶外的键借用大三和弦 / 属七）；其余为固定和弦。VOICING 的转位把最低的音升高八度，SPREAD 把第二个音升高八度。
- ARP：ARP OFF / UP / DOWN / UP-DN / RANDOM / ORDER；RATE 1/4 / 1/8 / 1/8T / 1/16 / 1/16T / 1/32；RANGE 1–4 个八度；GATE 10–100 %；HOLD 松手后继续。按住的键（含和弦展开后的每个音）按顺序轮流发声，最多 16 个音。第一下立刻发声；停止时按 BPM 自由运行，播放时对齐节拍（1/16 跟随 Swing），按 PLAY / 停止不会断拍。UP / DOWN / UP-DN 从刚弹过的音继续走，到两端不重复；ORDER 按按下的顺序；RANGE 向上扩展八度。HOLD 打开时松手继续，全部松开后再按会换成新的一组。REC 打开且在播放时，ARP 弹出的音按 GATE 长度录进它那条轨道的 Pattern。ARP 的音在演奏区和屏幕键盘上同样留下余光。换轨不会停下别的轨道的 ARP；全部停止和载入工程时所有 ARP 停下。

引擎参数页（括号内是 CTRL 地图上的说明）：

- DX7，共 13 页：VOICE、CHORD、ARP、ALGO（算法、反馈、移调，附算法图）、LEVELS（六个算子输出，附电平柱：实心是能听到的载波，dither 是调制器）、OP1–OP6（OUT、CRS、FINE、DTN / MODE（RATIO / FIXED）、AMOD / R1–R4 / L1–L4，附这一算子的包络与 CAR / MOD 标记）、LFO（SPEED、WAVE、PMD、AMD、P SENS，附波形）、FX。
- Juno，共 8 页：VOICE、CHORD、ARP、DCO（RANGE、SAW、PULSE、PWM、PW SRC、SUB、NOISE、LFO，附示波器）、VCF（CUTOFF、RESO、ENV、POL、LFO、KBD，附滤波曲线）、ENV（ADSR、VCA 模式 ENV / GATE、LEVEL，附包络）、LFO（RATE、DELAY，附延迟渐入的波形）、FX。Juno 预设自带的 Chorus I / II 映射到 FX 页的 CHORUS。
- Sampler，共 6 页：VOICE、CHORD、ARP、SAMPLE（文件、录音、裁剪、根音、循环、增益，附波形）、TONE（滤波、包络、电平）、FX。
- Drums，共 3 页：VOICE、KIT（八鼓电平 + Master，附鼓垫）、FX（没有 CHORD / ARP）。VOICE 和 KIT 上的鼓垫是一排八个小格（BD SD CH OH CP TM CB MA），和键盘上演奏区的八列一一对应（2026-11-03，用户要求；以前是两排四个，对不上键盘）。

Sampler 文件与录音流程沿用原规则：录音前确认、未保存草稿阻止替换、命名重名时确认覆盖、被其它轨引用的文件不能删除。

**采样长度**：最长 5 分钟。2 秒（88200 帧，176 KB）以内的采样整段读进 PSRAM（每个发声的音每块按顺序读一次，不影响速度；PSRAM 只剩 256 KB 时改为边读边播；没有 PSRAM 的板子上是 186 ms、8192 帧、16 KB，放在内部内存），更长的从 SD 卡边读边播：后台任务按块（1024 帧）预读每个发声音符当前和后面的块，并常驻起始点的两块，所以按下就响；块缓存只在有长采样时存在（一个 16 KB，最多 32 KB），放在内部内存，因为 SD 卡的 DMA 只能读进内部内存。卡来不及时这一小段保持上一个采样值，不会卡住音频；保存工程等写卡操作可能造成很短的空隙。

**录音**：有卡时直接写到 `Session/take-<轨道>.wav`，最长 60 秒，约每秒同步一次；断电恢复仍取决于 SD 卡和文件系统写入是否完成；写满或卡满自动停止并保留已录内容（CARD FULL）；没有卡时放进剩余内存。麦克风的时钟和喇叭的 WS 共用 GPIO43，所以录音期间喇叭静音、解码器待机（音序器继续走），结束后恢复。麦克风只在录音画面开着时工作：离开画面（换场景、换轨）即取消这次录音。

#### 2 SEQ（音序器）

| 子页面 | 内容 |
|---|---|
| DRUMS（鼓轨的 STEP 视图，鼓机） | 鼓引擎的轨道和音符轨一样有 ROLL / STEP 两种视图，OK 单击切换（鼓轨默认 STEP，音符轨默认 ROLL，各记各的），按键规则和下面的 ROLL / STEP 完全一样。STEP 视图就是鼓机（`Controller::DrumLayout()`，标签 DRUMS）：左边 8 条鼓 × 当前页 16 步的矩阵（一击越重 dither 越密，四档：实心 / 一半 / 1/4 / 1/8，`DrumDensity`），右边 4×4 是 PADS 选中的那面鼓在 GRID 上的 16 步，选中的步反显。PADS 是鼓和力度（列是鼓，行是力度，同时发声）：按住鼓 PAD 再点 step 放进这面鼓（有就去掉），力度是这个 PAD 那一行的力度；点 step 只是选中它（不加音），之后按鼓 PAD 放进这面鼓，已有的鼓在松开 PAD 时去掉（规则同下面的 ROLL / STEP）；按住一步不再单独调力度，力度、微步和连击用 OK 选到 VEL / TIME / HITS 再用 ADD / SUB 改（按住一面已有的鼓的 PAD 时只改这一击）。NOTE 这一项：按住一面已有的鼓的 PAD 再按 ADD / SUB，这一击换到按 PAD 顺序（BD SD CH OH CP TM CB MA）的下一面 / 上一面鼓，跳过这一步里已有的鼓，力度、时值、微步不变，屏幕底部弹出鼓名（FN + ADD / SUB 也是一面）；只会换到套鼓里的鼓，不会移到不是鼓的音高。没有按住 PAD 时 NOTE 在鼓轨上不动（提示 PADS PICK DRUMS）。按住鼓 PAD 时，这面鼓的整行亮起，亮带的密度跟着按下的力度走（四档），鼓名反显，这一行的击打挖出黑底、用轨道颜色画；只是选定（松开后）的鼓是暗一些的底纹，这一行的击打仍是轨道颜色（不变白）。偏离正拍的一击：矩阵里画成细条，位置就是它在这一步里的位置；右边 GRID 格底部有一个小刻度。鼓轨的 ROLL 视图是钢琴卷帘，亮起的行写鼓名 |
| ROLL / STEP（音符页，两种视图） | 两种视图画法不同，按键完全一样，OK 单击切换。**ROLL 钢琴卷帘（音符轨默认）**：横向是当前页的 16 步（GRID 的 16 键按行排列；长 Pattern 见下面「长 Pattern」；不到 16 步的 Pattern 也铺满宽度，底部小格跟着列走），纵向是音高（音符轨固定显示 PADS 在当前八度能弹到的全部音，用没用到都在，不折叠；这一页上有超出的音或按着的音时往外扩，最多 41 行；鼓轨按用到的鼓自动缩放），每个音符画成和它时值一样长的条，用轨道颜色，力度越大越实（选中的步和亮起的行上实心，不变白）；底部一条小格对应 GRID：有音符的步亮，选中的步白色。按住 PADS 时，按下的音（和弦音也算）所在的行整行亮起（棋盘 dither；鼓轨按力度四档），左边音名反显，这一行上的音符条挖出黑底、用轨道颜色实心画；空 Pattern 的提示在按着 PADS 时让开。**STEP**：和鼓机一样的画法（`DrawStepMachine`）：左边每个音高一行（和钢琴卷帘一样不折叠：PADS 在当前八度能弹到的全部音，用没用到都在；这一页上有超出的音、按着的音或上次按的音时往外扩，最多 41 行，从高到低），黑键行带暗底纹，左边标出 C、上次按的音和按着的音，右边 4×4 是上次按的那个音在 GRID 上的 16 步（力度四档），选中的步反显，矩阵里选中的步整列淡亮。**GRID 的 step 键**（参考 MatrixOS 的 Sequencer，`Controller::StepKeyPress`）：按下就选中这一步，停着时按住期间听得到这一步的音；有选中时 PADS 编辑选中的步（停着时发声），一个键弹出的几个音（开着和弦时）算一组（`UiState::padEdit`）：选中的步里没有这组音的键，按下就加进去；亮着的键（选中的步里有其中任何一个音）在松开时才起作用，只是点一下，松开时删掉（每个选中的步各自判断，已经有其中任何一个音就把这一组全删掉、一个也不加，一个都没有才全部加上，`SynthSequencer::ToggleNotesOnSelectedSteps`），按住不会先删。**按住亮着的键，ADD / SUB 只改这个键的音**（所有编辑项同一条规则，`Controller::AdjustHeldPadNotes`，用户 2026-11-11）：改当前的编辑项（底行白字，OK 循环），每个含有这些音的选中步都改。NOTE 是移动音高，一次一个键盘上的键（PIANO / CHROMA 半音；SCALE 布局或开着 LOCK 时是音阶里的下一个音；FN + ADD / SUB 一个八度），跳过这一步里已有的音，和弦键整组变成相邻那个键的和弦；按住期间再按继续移动同一组音（哪怕它已经在别的键上），停着时听得到移动后的音，屏幕底部弹出音名，按住的行跟着走。LEN / VEL / TIME / HITS 只改这些音的时值 / 力度 / 微步 / 连击，底部弹出「VEL G3」和新值（`SynthSequencer::AdjustNotesOnSelectedSteps`，和整步编辑共用 `AdjustLaneLocked`：以后新加的编辑项加在那里，手势不用改）。鼓轨的 NOTE 把按住的这一击换到相邻的鼓（见上面的鼓机），LEN / VEL / TIME / HITS 改按住的这一击。改和弦里的一个音只有这一种办法（原来的 SCOPE、FN + ENTER 选音已去掉，用户 2026-11-11）：演奏区够不着的音，先用 FN + 8 / I 换八度。按住期间按过 ADD / SUB 或 PADS 以外的键，松开时不删；按住 PAD 时 ADD / SUB 不改整步、不翻页，PAD 先松开而 ADD / SUB 还在连发时也一样。没有按住 PAD 时，OK 循环编辑项 NOTE → LEN → VEL → TIME → HITS，ADD / SUB 调整整步的所有音，同时按住几步就一起编辑。**短按**（0.25 秒内松开，期间没按别的键）选中并保持，手可以离开 GRID 再弹 PADS；短按唯一选中的那一步放开它，短按别的步换成那一步（SETUP KEYS 页 SELECT = MULTI 时短按是加入 / 移出选中）；ESC 取消选择。**按住**（超过 0.25 秒，或按住时按了别的键）只在按住期间选中，松开就放开（按之前已经选中的步保持选中）。**按住 PADS 再点 step**：把这些音（含和弦音）作为一组写进那一步；那一步已经有其中任何一个音，就把这一组全删掉、不加新的（`ToggleStepNotes`）；按住 PADS 时，GRID 上含有这些音的步全亮。写入后不会自动前进，没写的步就是休止。选中的步的音在演奏区全亮，播放时播放头那一步的音也亮。REC 打开并播放时，没有选中 step 时弹 PADS 实时录进 Pattern。 |
| PATTERN | 当前轨 16 个 Pattern 槽 + 四轨正在播放的 Pattern。GRID 单击选择；播放中会排队到当前循环结束再切换；空槽单击即新建 |
| TRACK | 行 0：SEQ 开关、STEPS（当前 Pattern 的长度，1–256；ADD / SUB 一步，FN + ADD / SUB 一页）、SWING、BPM；下方是四轨总览：引擎、Pattern、步数标尺与播放位置（长 Pattern 压缩成 16 格） |

**HITS（鼓点连击，用户 2026-11-11）**：一个音在它自己那一步里连击 1–4 下。它是第五个编辑项（底行白字 NOTE LEN VEL TIME HITS，OK 循环；不叫 ROLL，ROLL 已经是钢琴卷帘视图的名字），改法和其它编辑项一样：选中 step 后 ADD / SUB 调整整步的所有音（1–4，到头不绕回，FN + ADD / SUB 一下到头）；按住亮着的 PAD 再按 ADD / SUB 只改这个键的音（底部弹出「HITS E3」和新值）；选中的是空步时调 NEW HITS（这一轨手写新音的初始值，和 NEW LEN / NEW VEL 一样）。按住 PAD 连点 step 不会改连击（那仍是放进 / 去掉音）。录音（REC、take、课程写入的歌）写入的都是 1 下；复制、粘贴、DUP、量化都带着它。时间：几下平均落在整 pulse 上（2 / 3 / 4 下相隔 12 / 8 / 6 个 pulse，即 32 分、16 分三连音、64 分），第一下就是这个音本身（带它的 TIME 和 Swing），其余紧跟其后；每一下响这个音的时值，最长到下一下开始，最后一下响到较长的音结束：长音在第一步里连击，然后延续下去（`SequencerHitGatePulses`）。最晚的一下可以落到两步之后，跨过 Pattern 结尾也照样响。显示：钢琴卷帘把这个音切成几段（每段和它响的一样长，开头一列实色，段间一像素黑缝）；STEP 视图右边的 GRID 格把这一步同样切成几段；鼓机矩阵里一击只有 4 像素宽，放不下时间上的切分，改成竖着叠几道横线（像鼓谱上表示滚奏的斜线），几道就是几下；音符轨 STEP 矩阵的行太细，只切一刀表示连击，几下看右边 GRID 或底行。键灯不变。存储：每个音最后一个字节的两个空位（存的是下数减一，0 就是普通的一下），每页 step 仍是 768 字节。**工程格式**：HITS 要格式 8 才存，这一版读得懂格式 8，但仍写格式 7（等准备测试版时统一升级一次，`kProjectWriteVersion`），所以现在保存后再载入、重启后从自动保存恢复、或上完一次课程，HITS 都会回到 1 下。

**长 Pattern**：每个 Pattern 有自己的长度（1–256 步），轨道循环它正在播的 Pattern 的长度；新建 Pattern 时长度沿用当前的，复制 Pattern 时一起复制。GRID 一次显示一页（16 步），页码在底行右端（如 `2/4`，手动翻过时是黄色）。

- 翻页：ADD / SUB（音符页没有选中的 step 时，图标是 → / ←，提示 PAGE n/m；有选中时 ADD / SUB 调选中的步），或 FN 菜单行 2 的 PAGE − / PAGE +。SELECT = ONE 时翻页会取消选择（选中的步不在眼前）。
- 换 Pattern：FN + ADD / SUB 换到下一个 / 上一个 Pattern（音符页没有选中时；PATTERN 页上不按 FN 也是），屏幕提示 PATTERN n/m 和 Pxx；播放中排队到这一轨循环结束再换，提示 NEXT PATTERN。
- 设长度：FN 菜单行 2 的 LEN − / LEN +，一下跳到下一个整页（1、2、4、8、16、32、48 … 256）。
- 播放时 GRID 跟着播放头翻页；手动翻页后停在那一页，下次 PLAY 恢复跟随；有选中的 step 时不自动翻页。
- 内存：step 按页存放在 PSRAM（每页 768 字节；音序器每一步只读一步，不影响时钟），只有写了音符的页才占内存，清空后归还；工程快照（保存、切应用）也在 PSRAM，与当前工程共用页面，改动时才复制。PSRAM 只剩 256 KB 时（没有 PSRAM 的板子上：内部内存只剩 24 KB 时）这个音不写入，屏幕提示 MEMORY FULL；四轨每个 Pattern 都写满 256 步也只要约 790 KB。载入工程时如果两个工程放不下，当前工程先进自动保存再释放，新工程载入失败时恢复。

**OK 是录音键**（音符页，鼓轨也一样）：单击在 ROLL 与 STEP 之间切换；按住 0.45 秒打开 **RECORD 面板**；有选中的 step 时单击仍是循环编辑项。OK 的键灯 ROLL 时半亮、STEP 时全亮。

RECORD 面板（GRID 4×4）：
- 行 0：REC（实时录音开关，同 Fn+PLAY）、SNAP（放进 Pattern 的 take 对齐整步 STEPS / 保留原来的时间 FREE）、VIEW（ROLL / STEP），第四格空着。点一下切换，ADD / SUB 也可以调。
- 行 1–2：**无感录制（Capture）的 take**，最新的在最前，最多 8 个。弹 PADS 时一直在后台记着（不用开 REC），停顿超过 2.5 秒、换轨或超过 60 秒就开始新的一个。8 段共用 400 个音：一段最多 400 个音，新的音需要位置时最旧的段被挤掉。每格显示编号、轨道、音符数和一条小时间线。
- 行 3：选中 take 的那一小节放进 Pattern 后的样子，「BAR n/m」和多久之前弹的。ADD / SUB 选择第几小节（按这一轨的步数切分）。
- OK：把这一小节放进这一轨的**新 Pattern**（原有的 Pattern 一个音都不改），并切到那一轨；Fn+OK：合并进正在用的 Pattern。播放中弹的 take 按小节线对齐；停止时弹的 take 从第一个音开始，按当时的 BPM 计时。ESC 关闭。

实时录音：REC 打开（RECORD 面板或 Fn+PLAY）后，播放中没有选中 step 时按 PADS 按当前位置写入当前 Pattern（叠录），量化到微步。

#### 3 TAPE（走带：四轨的 Pattern 编排与混音）

| 子页面 | 内容 | GRID | ADD / SUB / ENTER |
|---|---|---|---|
| SESSION | 像 Ableton Live 的 Session 视图：每列一条轨道（轨道色），每行一个 Pattern 槽，四轨同时显示。每格画出这个 Pattern 的步（超过 16 步时压缩成 16 格）；正在播的格子绿色底、▶ 和走动的播放头，排队的格子黄色蚂蚁线和 NXT，最后一个 Pattern 之后的空格是「+」 | 点格子 = 让这一轨播这个 Pattern（停止时立即切换，播放中在该轨循环结束时切换）；点「+」新建；从 SESSION 发射会退出 SONG 模式 | ADD / SUB 上下移动并滚动；ENTER 发射光标所在的整行（所有有这个 Pattern 的轨道）；Fn+ENTER 把当前每轨正在播（或排队）的 Pattern 记录成 SONG 的新一行 |
| SONG | 整首歌像一段磁带：每列是一行编排（从左往右是时间），每行是一条轨道，格子里是这一段要播的 Pattern（`--` 表示该轨静音）和小节数（xN）。播放中的一列有绿色磁头扫过；状态行的点线变成整首歌的进度 | 列 = 4 个连续的编排行，行 = 轨道 1–4（每行带该轨的颜色）；点格子选中，再点一次同一格在「Pattern」和「小节数」之间切换（小节数编辑时 xN 高亮）；点最后一列之后的空列追加一行 | ADD / SUB 换这一格的 Pattern，或改这一行的小节数；ENTER 从这一行开始播（自动进入 SONG 模式）；Fn+ENTER 切换 SONG / LOOP |
| MIX | 四个轨道推子、活动表与峰值、MUTE / SOLO / SEQ，底部主音量与输出电平 | 列 = 轨道；行 0 选中、行 1 静音、行 2 独奏、行 3 序列开关 | ADD / SUB 调音量；ENTER 在选中轨与 MASTER 之间切换 |
| SENDS | 四轨的效果总览：每列一条轨道，行 0 FILTER（双极：中心为关，左低通右高通）、行 1 CHORUS、行 2 DELAY 发送、行 3 REVERB 发送。关闭的格子变暗；轨道发声且发送打开时格子右缘发光 | 点格子选中（同时选中该轨） | ADD / SUB 调整；ENTER：CHORUS 换模式，其它格子关闭 / 恢复原值；Fn+ENTER 默认 |
| FX | 总线效果（属于工程）：行 0 DELAY（按速度同步的音符时值、FDBK、TONE、RETURN），行 1 REVERB（SIZE、DAMP、LOCUT、RETURN），行 2 COMP、LOW / HIGH 搁架 EQ（±12 dB）、TAPE 饱和；行 3 画出当前速度下的回声序列与混响衰减时间，以及两路返回电平 | 点格子选中 | ADD / SUB 调整；ENTER 在 DELAY 上切换时值；Fn+ENTER 默认 |

SONG 模式：编排非空时，播放从选中的行开始，每行播放 xN 小节（1 小节 = 16 步，SESSION 记下的新行默认覆盖它最长的 Pattern）后进入下一行，所有轨道在行首切到该行的 Pattern 并对齐；`--` 的轨道在这一行静音。结尾回到第一行循环。LOOP 模式即各轨循环自己的当前 Pattern。工程文件 v3 起保存编排，v4 起保存总线效果，v6 起保存键盘与每轨的和弦、ARP，v8 保存 HITS（已经读得懂，下一次准备测试版时才开始写）（开发版曾写过带 PLAY 行的 v5，读取时跳过该行）；SONG / LOOP 开关是演奏状态，不存。

#### 效果器

每条轨道：引擎 → DRIVE（软饱和）→ CRUSH（降采样 + 降位深）→ FILTER（DJ 滤波：64 为关，往下低通、往上高通，RESO 共振）→ CHORUS（Juno 式 I / II / I+II）→ 轨道推子 → 发送到 DELAY 与 REVERB。这些都是引擎参数（`fx.*`），随音色和工程保存，换引擎或换预设时回到默认（Juno 预设带自己的 Chorus）。

总线：两路发送先抽取到 11.025 kHz 再处理。DELAY 是按 BPM 同步的磁带延迟（反馈环里有 TONE 低通和 80 Hz 高通，换时值时像磁带一样滑音；时值超过约 740 ms 时自动减半一个音符时值），REVERB 是 4 线反馈延迟网络（带宽约 4.5 kHz，偏暗），然后是搁架 EQ、压缩和 TAPE 饱和。

效果缓冲按需分配，不在音频块内分配；分配失败显示 NO MEM。内部 RAM、PSRAM 和大块分配的边界遵循本文 Memory 规则，具体实现见 `Library/Synth/Audio/Fx/SynthFxDsp.cpp`。

音频渲染、复音预算和事件队列遵循本文的 Sound Rules；内存分配遵循 Memory。性能必须在板上验证，主机结果不能代替实机音频测试。

### 叠加层

按住左侧键出现的都是**地图**：内容区换成与 GRID 一一对应的格子，格子画在它对应的那个键的位置；状态行换成地图标题和用法提示；GRID 的灯只亮可选的键（当前项全亮，其他可选项半亮），其余按键全暗。地图从对应的左侧色条里长出来，松开时缩回去。

- **场景地图（按住 ESC，橙）**：四列高卡片，每列是一个场景：编号、名称和它的页面列表（标出进入时的页）；第一列 0 SETUP 右边一条虚线、编号暗色，表示它不在单击 ESC 的循环里。状态行提示「TAP = 1>2>3」。
- **轨道地图（按住 TAB，青）**：每列一条轨道：顶格是轨号、引擎、音色、音量条和活动表，下面三格是 MUTE / SOLO / SEQ。
- **页面地图（按住 CTRL，紫）**：当前场景的所有页面，每格是页签、编号和一句说明（如 OP1「TUNE EG」、FX「INSERTS」）。
- **FN 菜单（按住 FN，黄）**：状态行是「FN MENU」和这是哪个画面的菜单，内容区是 16 格菜单（与单击 FN 常驻的菜单相同），底行列出菜单以外的 Fn 组合。菜单常驻时按住 FN 不再叠一层；输入画面里按住 FN 不出菜单。
- **确认框**：危险操作（覆盖、删除、丢弃未保存）一律 ENTER 确认、ESC 取消，后果文字用红色。音色改过参数（和这个音色载入时的值不同，`ISynthEngine::HasEdits`）时，换音色（VOICE 的 ADD / SUB、音色浏览里的 ENTER，重新载入同一个也算）或换引擎都先问一句（`LOAD <音色>?` / `SWITCH TO <引擎>?`，「EDITS TO <音色> WILL BE LOST」）：ENTER 换，ESC 保留，从浏览或引擎选择进来的回到那里。教程里不问：练习工程结束就丢，课程的下一步也不会提到这个框。
- **提示条**：保存成功、错误等写在底部状态行，2 秒后恢复页签。

### 视觉系统

#### 风格

终端 + 新极简 + dither：底色一片（DARK 是纯黑）；等宽大写字；大量留白；只有状态才用颜色；面积填充一律用 4×4 Bayer 有序抖动，不用纯色块。

#### 调色板与主题

屏幕有三套配色（主题），在 SETUP > SYSTEM > THEME 选，是设备设置 `sys.theme`（`dark` / `light` / `retro`，默认 DARK）：**DARK** 黑底亮字（出厂的样子）；**LIGHT** 白纸黑字，同样的色相，每个颜色压暗到纸面亮度的六分之一左右，在纸上能读（约 4.5:1），黑字压在色块上也能读（约 3:1），就像 DARK 的颜色夹在黑底和亮字之间（DARK 的黄、青、黄绿、播放绿原样放到白纸上就看不见了）；**RETRO** 背光绿色液晶，浅绿纸面、近黑墨色，颜色是略灰的染料，亮度和 LIGHT 一样取在纸面与墨色中间，dither 读起来就像液晶自己的点阵。

画图的代码只说颜色的含义、不说具体颜色（`Render/Theme.h`）：`kBlack` 是底色（面板清成的颜色），`kInk` 是写在上面的记号，`kShade` / `kFaint` / `kDim` 是两者之间的三级，顺序在三个主题里一样，所以同一份绘图代码在 DARK 上是黑底亮字、在 LIGHT 和 RETRO 上是纸上墨字。颜色在画图时从当前调色板读（`theme::Use` 每帧开始按设置换调色板，换一次只是复制一个小结构；读颜色是一次内存读取，不是每个像素一次）；不要在静态变量或 `constexpr` 里存颜色值。主题只管屏幕：键灯是光，不分主题；屏幕两侧的边条仍然按键灯亮度画（`Animator::LedShown`），在浅色底上同样从几乎看不见到实色。

| 名称 | 用途 | DARK | LIGHT | RETRO |
|---|---|---|---|---|
| Black | 底色 | 0,0,0 | 236,236,228 | 188,210,160 |
| Ink | 主文字、反显的底 | 232,232,224 | 20,20,18 | 14,24,14 |
| Dim | 次要文字 | 132,132,126 | 100,100,94 | 70,90,58 |
| Faint | 空槽、刻度、未选中轮廓 | 58,58,54 | 176,176,168 | 136,160,114 |
| Shade | 几乎看不见的底纹 | 26,26,24 | 212,212,204 | 166,190,140 |
| ESC 红橙 | ESC 条、返回 | 255,90,54 | 185,63,36 | 153,67,45 |
| TAB 青 | TAB 条、轨道地图 | 54,214,230 | 25,115,124 | 38,102,107 |
| FN 黄 | FN 条、Fn 功能字 | 255,208,48 | 138,112,21 | 120,99,32 |
| CTRL 紫 | CTRL 条、页签 | 160,112,255 | 112,77,180 | 98,76,150 |
| Play 绿 | 播放、游标 | 72,255,120 | 28,116,51 | 35,102,48 |
| Rec 红 | 录音、危险 | 255,52,68 | 194,37,49 | 156,55,52 |
| T1 粉 | 轨道 1 | 255,92,176 | 171,59,116 | 141,62,98 |
| T2 蓝 | 轨道 2 | 72,152,255 | 46,101,173 | 52,90,141 |
| T3 黄绿 | 轨道 3 | 176,236,56 | 86,118,23 | 79,106,32 |
| T4 琥珀 | 轨道 4 | 255,164,40 | 152,96,19 | 132,88,34 |

选中使用终端反显（Ink 底、底色的字）：DARK 上是白底黑字，LIGHT / RETRO 上是黑底浅字。三级灰在三个主题里和底色的对比相同（Dim 约 5:1，Faint 约 1.8:1，Shade 约 1.2:1）；改颜色时保持这个关系，新颜色在 LIGHT 和 RETRO 上也取在纸面与墨色亮度中间。

轨道颜色在所有出现轨道的地方保持一致：状态行的轨号色块和下面的点线、TAB 色条里的四个点（当前轨加 Ink 框）、轨道地图、MIX 推子标题、SENDS 每格左缘、TAPE 每行的色条和轨号、SEQ TRACK 页的轨道表、SETUP KEYS 键盘预览里按下的音。

#### 布局（像素）

| 区域 | 范围 |
|---|---|
| 左侧 4 条 dither | x 0–5，按物理行四等分，条间 3 px |
| 右侧符号列 | x 233–239 |
| 状态行 | y 0–9：场景号与名称、轨道、引擎·音色、REC / 播放 / BPM |
| 分隔点线 | y 11 |
| 内容区 | x 10–228，y 14–121 |
| 页签 / 提示行 | y 125–134 |

GRID 对应的 4×4 单元格为 52×24 px，间距 3 px。

#### 字体

Micro 5×7（步进 6）用于终端正文；Small 6×9 用于强调；Large 10×14 用于数值；Huge 15×21 用于 BPM、音色名等主信息。全部大写。

#### dither 用法

- 两侧键帽：平时 4/16 密度，按住时由下往上填满；没有无意义的滚动，键帽里只显示状态点。
- 电平与推子：值以内为 12/16，值以外为 2/16。
- 包络、滤波、波形：曲线下方 6/16 填充，曲线本身实线。
- 叠加层：背景先整体压暗（黑色 10/16 覆盖），面板 2/16 灰底。
- 场景切换：新画面按 Bayer 阈值在 6 帧内逐步显现。

### 按键灯

单色 LED，由 CardenzaHAL 驱动（只读依赖，不在本仓库里改）：HAL 的每个键等级 0–15 是 16 档线性占空比（4 个二进制加权的扫描平面），人眼却不是线性的，所以 `Device::LED` 的等级是感知亮度，设备层用 gamma 2.6 换算成 HAL 的等级（`Devices/LedLight.h`），总亮度是独立的 LED_EN 硬件 PWM（固定 gamma 2 补偿；LED 引脚 EN47 / CLK48 / DAT21 由 HAL 定好），不再给按键等级封顶。模拟器和说明书用 `Devices/LedLight.h` 里同样的模型；界面只用三种状态：

| 状态 | 等级 | 含义 | 例子 |
|---|---|---|---|
| 灭 | 0 | 这里什么都没有 | 空 step、发不出声的键（钢琴缺口、LOCK = MUTE 的音阶外音）、当前画面没有的功能、关闭的发送 |
| 半亮 | 6（感知亮度；伽马校正后是 HAL 最暗的一档，1/15 的点亮时间） | 有东西，没选中 | 有音符的 step、能发声的键、选项、参数、可执行的动作、已有的 Pattern |
| 全亮 | 15（100 %） | 选中或正在发生 | 选中的 step、焦点参数、当前页 / 场景 / 轨道 / Pattern、按住或正在发声的键、播放头、开着的开关（静音、独奏、音序开） |
| 闪烁 | 15 / 0 | 等待中 | 排队的 Pattern、已提示跳转的编排行、确认框的 ENTER、录音待命时的 PLAY |

没有呼吸、细闪这类效果，也没有不同的中间亮度：一个键是什么状态，只看灭 / 半亮 / 全亮。

#### 各状态下哪些键亮

| 状态 | 左列 | 演奏区 | GRID | 右列 |
|---|---|---|---|---|
| 平常 | ESC / TAB / FN 半亮；CTRL 半亮（只有一页时灭）；按住的键全亮 | 见下方「演奏区」 | 见各场景 | ADD / SUB 可调时半亮、否则灭；ENTER 有动作时半亮、否则灭；PLAY 停止半亮、播放全亮、录音待命闪烁；按下时全亮 |
| 弹窗 | ESC 半亮；TAB、CTRL 灭（FN 菜单常驻时 FN 全亮，ADD / SUB / ENTER 灭） | 同平常（搜索浏览器：能输入的字母数字键半亮） | 弹窗里可选的格子半亮，当前项全亮 | 同平常；确认框的 ENTER 闪烁 |
| 切轨后约 0.8 s | 只有对应行全亮（T1 = ESC 行 … T4 = CTRL 行），其它灭 | 不变 | 不变 | 不变 |
| 按住 ESC | 只有 ESC 全亮 | 全灭 | 四列场景：当前场景那一列全亮，其它三列半亮（第一列是 SETUP） | 全灭 |
| 按住 TAB | 只有 TAB 全亮 | 全灭 | 每列一轨：行 0 当前轨全亮、其它半亮；静音 / 独奏 / 音序开着全亮、关着半亮 | ADD / SUB 半亮，其它灭 |
| 按住 CTRL | 只有 CTRL 全亮 | 全灭 | 每页一个键：当前页全亮，其它页半亮；没有页的键灭 | ADD / SUB 半亮，其它灭 |
| 按住 FN | FN 全亮；ESC、TAB 半亮，CTRL 在有多页时半亮 | 菜单里能用的字母项和快捷调节的 6 个键半亮；其它灭 | 菜单项：能用的半亮，开着的开关全亮，用不了或空格灭 | ADD / SUB 半亮；ENTER 在有 Fn+ENTER 的画面半亮、否则灭；PLAY 半亮（录音已开全亮） |
| 输入画面里按住 FN | FN 全亮 | 全灭 | 方向键 `;` `,` `.` `/` 半亮，其它灭 | ADD / SUB 半亮，其它灭 |
| 文本输入 | ESC、FN 半亮，TAB、CTRL 灭 | 字符键半亮 | 字符键半亮 | 删除键（右上）和 ENTER 有字时半亮、否则灭；空格半亮 |

各场景的 GRID：

| 画面 | GRID |
|---|---|
| INSTR 参数页（含 CHORD / ARP） | 参数和动作半亮，焦点参数全亮，空格灭 |
| SEQ STEPS | 当前页：空 step 灭，有音符的半亮，选中的全亮；按住 PADS（没有选中）时含有这些音的 step 全亮；播放时播放头全亮走过；超出这个 Pattern 长度的灭 |
| SEQ PATTERN | 已有的 Pattern 半亮，正在用的全亮，排队的闪烁；「+」和空位灭 |
| SEQ TRACK | 四个设置半亮，焦点全亮 |
| TAPE SESSION | 每轨当前的 Pattern 全亮，其它已有 Pattern 半亮，排队的闪烁，没有 Pattern 的灭 |
| TAPE SONG | 有 Pattern 的格子半亮，静音（`--`）的灭；选中的格子和正在播的那一行全亮；提示跳转的行闪烁 |
| TAPE MIX | 同按住 TAB（行 0 是 MIX 的焦点轨） |
| TAPE SENDS | 开着的发送半亮，关着的灭，焦点全亮 |
| TAPE FX | 参数半亮，焦点全亮 |
| SETUP PROJECT | 动作半亮，焦点（BPM / SWING）全亮，PLAY 在 SONG 时全亮 |
| SETUP FILES | 灭（用 ADD / SUB 选） |
| SETUP KEYS / SYSTEM | 设置半亮，焦点全亮（KEY LED 焦点时演奏区是亮度梯） |

#### 演奏区

演奏区永远是当前轨的键盘，所有画面规则一样：

- 鼓轨的演奏键：横向每列一种鼓，纵向是力度，最上面一行最重（127 / 100 / 70 / 40，`kDrumRowVelocity`）。静止时和旋律轨的演奏键一样是半亮（半亮已是 HAL 最暗的一档占空比，静止时的渐变只会比旋律轨更亮）；按下时这一列从最下面亮到按下的那一行，力度越大亮得越多；一击从按下的键沿这一列上下扩散，重击跑满整列，轻击只到邻近一两个键（`Animator::DrumWave`，只在键灯上）。屏幕上还是原来的鼓垫方块，力度用 dither 疏密表示：越重越密，四档对应演奏键的四行，档与档拉开（实心 / 棋盘一半 / 1/4 / 1/8，`DrumDensity`；原来按力度连续变化的密度，相邻两档只差 3/16，在屏幕上分不出来）。VOICE 的鼓垫、SEQ 鼓机页的矩阵和 GRID 都用这四档。SEQ 的鼓机页上，按演奏键同时选定 GRID 写入的鼓和力度，音序器播到的鼓也按它的力度显示。这是键灯「只有灭 / 半亮 / 全亮」规则唯一的例外。
- 能发声的键半亮：PIANO 下从上往下第二、四行是白键，第一、三行是黑键，黑键行的缺口（C# 前、E 与 F 之间、A# 后）灭；SCALE、CHROMA 每个键都半亮；鼓轨每个鼓垫半亮。
- 发不出声的键灭：缺口、LOCK = MUTE 时音阶外的音。
- 按住的键全亮；按下的和弦的其它组成音、ARP 正在弹的音、音序器在这一轨弹出的音全亮（音序器的音约 0.2 s 后回到半亮）。
- SEQ 选中 step 时（音符轨和鼓轨都支持按住或短按），演奏区编辑这个 step：它里面的音全亮。

#### 屏幕两侧和按键灯同步

屏幕左右两侧的 4 + 4 个色块就是两侧 8 个键的灯：每个色块读取对应键灯此刻的亮度（`Animator::LedShown`），灭时只剩很淡的 dither（2/16），半亮时 30 % dither（5/16），全亮时变成实色块（右侧的字形变成底色）。浅色主题上也一样：边条的 dither 从几乎看不见到实色，跟键灯的亮度走，不跟主题。按住、切轨提示、地图、闪烁，在屏幕和键上永远一致。

屏幕边条通过 `Animator::LedShown` 跟随键灯；修改灯光时遵循 Groovebox UI Rules，不能让动画把应灭的键持续点亮。

### 动效

所有动效都是时间的纯函数（`Render/Effects.*`），不需要第二块帧缓冲。`Render/Animator.*` 每帧比对界面状态，记录各类事件发生的时刻，渲染时再把这些时刻换算成动画。界面逻辑因此不依赖动画。逐帧录像见 `docs/groovebox/motion/*.gif`，由 `tests/groove/motion.script` 生成。

| 时机 | 动效 |
|---|---|
| 开机 | 噪点场中扫描线掠过，CARDENZA 逐字解码，四色短条依次落下，然后经 Bayer 阈值溶解进界面（约 1.7 s） |
| 场景切换 | 场景是楼层、页面是同一层的房间：场景上下换，页面左右翻。按 ESC 一律往下：新场景从下方升上来、旧的往上移出画面（像在电梯门口看楼层），环 INSTR → SEQ → TAPE → INSTR 每一步都是往下（TAPE 回到 INSTR 也是），沿环往回走是往上；SETUP 在环的上面，进去往上，按 ESC 出来往下（`Animator` 的 `SceneDirection`），260 ms；两层之间一条橙色（ESC 色）的线是楼板。没有数字撕裂（用户 2026-11-06：不好看，而且场景和页面都横着走分不清）。状态行的场景名重新解码 |
| 翻页（CTRL） | 新页从侧面覆盖进来，旧页后退并沉入 dither，接缝为紫色；页签高亮滑向新页 |
| 换轨（TAB） | 青色扫描线自上而下刷新内容；轨号反显闪一下；音色名解码 |
| 按住 ESC / TAB / CTRL / FN | 地图从对应色条长出，有一根牵引线；背景逐级压暗；状态行换成地图标题并解码出来；格子沿 GRID 对角线依次落下；按下的格子扩散一圈波纹 |
| 弹窗 | 从中线向上下展开，两条亮线收边；列表逐行打出，光标条平滑滑动；新输入的字符闪一下并带波纹 |
| GRID 按下 | 对应格子向外扩散一圈波纹 |
| 参数调整 | 电平条平滑过渡；数值反显弹一下再淡出；聚焦格子是流动的虚线框；包络、滤波、波形等图形随数值形变 |
| 写入或删除音符 | 该 step 反显闪光，随后溶解（盖章效果） |
| 播放 | 游标走到哪一格，那一格绿色闪一下，随后保持绿框直到下一格；走过的 step 不留余晖；每拍左侧色条和 BPM 脉动，每小节第一拍更强；状态行的进度线只有头，不带拖尾 |
| 录音 | 内容区四周红色虚框呼吸；REC 字样闪烁；FN 色条脉动 |
| Pattern 排队 | 目标格为绿色流动虚框，底部倒计时条随循环剩余时间缩短 |
| 演奏 | 状态行轨号随每个音脉动；钢琴键、键位图、鼓垫松开后余光渐暗；鼓垫被击打时上跳；DX7 算法图里信号点沿连线流向载波；示波器随发声加速滚动 |
| 混音 | 推子平滑移动；活动表带下落的峰值保持线；MUTE / SOLO / SEQ 切换时按钮弹跳并带波纹 |
| 提示 | 从底部弹起，文字逐字打出；错误提示的色块闪烁 |
| 参数反馈（Fn 功能、菜单项、恢复默认） | 一条黄框从底部升起：名称、大号新值、刻度条（八度这类双向值用居中的刻度），约 1.1 s 后下沉淡出；连续按会一直更新 |
| 切轨 | 状态行轨号色块变宽一下；左列按键灯只亮对应的那一行（T1 顶 … T4 底） |
| 常驻 | 只保留有含义的：未保存标记、空状态提示缓慢闪烁。没有纯装饰的流动效果 |

#### 结束动画

| 时机 | 动效 |
|---|---|
| 松开 ESC / TAB / CTRL / FN | 地图缩回所属的色条 |
| 关闭弹窗（确认、取消、ESC） | 弹窗向中线折叠，露出下面的画面；叠层弹窗关闭时折叠在下一层之上 |
| 提示到时 | 逐字倒退并下落，露出页签 |
| 停止播放 | 绿色光头从停止处倒带回第 1 步，状态行进度线收回 |
| 取消选择 | 反白块溶解，外框向内收拢 |
| 载入 / 新建工程 | 一道亮扫描线自上而下刷新整屏 |
| Fn + ESC 全部停止 | 整屏闪白，中央盖上 ALL STOP，随后熄灭 |

#### 按键灯动效

按键灯先表达状态：灭 / 半亮 / 全亮；闪烁表示等待，教程提示使用半速闪烁。鼓轨的力度扩散是规则中明确允许的例外。开机、切换和错误等瞬时动画以 `Render/Animator.cpp` 为实现入口，不能改变输入语义或持续覆盖状态灯。

`tools/groove-motion-gif.py` 把逐帧截图和按键灯状态合成为 GIF（画面在上，键盘灯光在下）。



### 压力测试使用

`make DEVICE=Cardenza stress` 编一个带压力测试的固件（`Applications/Synth/StressTest.cpp`，`CARDENZA_STRESS_TEST`，绝不发布），配置和正式版相同，另加任务看门狗（卡死 5 秒就变成带调用栈的崩溃记录）。在 ESP-IDF 环境里用 `cd build/Cardenza-stress && esptool.py --chip esp32s3 -p <端口> write_flash @flash_args` 烧上去，开机 20 秒后自动开始，日志里是 `stress:` 开头的行，跑完每 20 秒重复一遍汇总。它在卡上写 `ZZ-ST-` 开头的工程和采样，挨个载入并播放；设备重启（崩溃或看门狗）后从下一个场景接着跑，并打印上次崩溃的 `!!!` 行。自动保存会变成最后一个测试工程。

- 场景：空工程；旧格式 v1（音色不存在）；0.1.0 正式版的 DEMO-2（格式 6）；四轨 DX7 每步六音和弦；四轨 Juno 长尾音八音和弦；300 BPM 鼓机八条全开加两轨；四轨 1/32 琶音四个八度；采样（1.5 秒进 PSRAM、20 秒从卡上流式）；效果器全部开到最大；最大工程（四轨 × 16 个 Pattern × 256 步 × 每步 8 音，文件 2.6 MB）；播放中的操作（切引擎、换音色、载入、新建、存盘、自动保存、改主效果和音量）；按键风暴（垫子成串按放、GRID、ESC、TAB、CTRL，不按 ENTER、ADD、SUB、FN、PLAY）；以上叠加；卡上每一个工程（含 0.1.0 的 `CardenzaSynth/Projects`）。
- 每个场景量：音频每块平均和最长时间、DMA 放空（`underruns`）、丢掉的音（按住的、还没出声的、被抢的，和提前收掉的尾音）、界面最长一帧和超过 34 ms 的帧（掉帧）、音序器脉冲最晚多少、晚 5 ms 以上的次数、内部 RAM 和 PSRAM 的最低值、操作最长多久和操作期间的放空。通过：放空 0、按住的音和新音一个不丢、晚的脉冲 0、界面最长一帧 < 250 ms、内部 RAM 最低 ≥ 12 KB、没有出错。`STRESS_ONLY=card:DEMO-2,sampler STRESS_SECONDS=60` 只跑这几个。

### USB

USB 口是 CardenzaHAL 提供的一个复合设备（`usb/usb.h`）：MIDI、双向音频和 HID。设备层开机时启动它。

- **MIDI**：电脑可以弹各轨：通道 1–4 对应轨道 1–4，其他通道弹当前轨。这些音符有主人（`GrooveApp::computerNotes_`，计入 `EndOrphanNotes`），电脑断开时结束；CC 120 / 123 也会结束它们，其他 CC 交给该轨的引擎。设备弹出的音（按键和音序器）也发给电脑，轨道 n 走通道 n。
- **音频**：电脑录到的是设备的声音，在设备音量之前（单声道复制到两个声道）。USB 音频两个方向的通道都在，但电脑播放的声音 app 目前不用（用户 2026-11-06：先只当电脑的麦克风）：`GrooveApp` 不装 line input，设备层把一块里没人读的从 USB 收下丢掉，好让电脑的播放流一直走着（HAL 的反馈看它的 FIFO 有多满，一直满着电脑就会降到最慢）。装上（`synth::audio::SetLineInput(&Device::Audio::ReadComputerAudio)`）时，它在输出增益和限幅器之前以单位增益并入混音，带电脑自己的静音和音量，于是也回到电脑录到的声音里。电脑里设备叫「203 Systems Cardenza」，仍有它的扬声器，播进去听不到。
- **控制台与更新**：stdout、stderr 和日志都走 HAL 的系统通道；电脑在那里读日志，也可以让设备重启进 Launcher 或 ROM 下载模式（报告 0xCB 或 SysEx F0 23，与 Matrix OS 相同）。电脑这边的命令行工具是 `tools/usb/cardenza_usb.py`（list、command identify / reboot / launcher / download、console、stats、test、guide）：板子没有串口时，`command download` 让它进 ROM 下载模式再刷。
- 内存：静态约 8.5 KB，外加 4 KB 任务栈，不管有没有接电脑都占着。

### 教程模式

Learn 网站（`wiki/`）的入门教程（课程在 `docs/learn/guide.json`，页面在 `wiki/src/guide`，在固件上的校验是 `tools/wiki/lessons.py`）一课一课地带用户操作，设备跟着课程走：网页让设备去这一课开始的画面，让这一步要按的键闪烁，设备把每个按键和当前状态报回网页，网页据此判断这一步做完没有。设备是接在电脑上的 Cardenza（Web MIDI）：网页一打开就向浏览器要 MIDI（SysEx）权限，用户允许后，现在接着的或之后插上的 Cardenza 都自动连上；没有设备时课程只是图，用「下一步」往前走。

- **通道**：短的 ASCII 文本，都以 `G` 开头（`Device::USB::ReadAppMessage` / `WriteAppMessage`）。真机上走 CardenzaHAL 的应用通道：HID 报告 0xFF，或 SysEx `F0 23 7F <文本> F7`，回复 `F0 24 7F <文本> F7`（网页用后者）。网页模拟器导出同样的文本（`cardenza_web_app_send` / `cardenza_web_app_take`，`cardenzaEmbed.guide` / `listen`），教程的浏览器测试用它扮演一台 USB 上的 Cardenza。
- **消息**（`Applications/Synth/GrooveAppGuide.cpp`）：`G?` 问版本（`G!1 g0|1`）；`GE` 开始（`GE1`，或 `GE0 rec|card|save|mem` 说明为什么不行：在录音、没有卡、卡写失败、内存不够）；`GX` 结束；`GP` 心跳；`GQ` 要状态；`GL` 这一课的起点（`s` 场景、`p` 页、`t` 轨道 1–4、`r1` 新的练习工程（用用户进教程前的键盘设置：排列、音阶、主音、八度……，教程跟着用户的设置走，不改它；`f1` 则用新工程的键盘 PIANO、C 大调，给改键盘设置或按指定音的课用：音阶、电钢琴和弦、旋律）、`d<n>` 「做第一首歌」前 n 课做出的歌：1 鼓 P01，2 加上 P02，3 加上 T3 电钢琴和弦，4 加上 T2 旋律，单独打开某一课也能接着做）；`GH` 这一步的键，闪（每个键两位十六进制的 `KeyIndex`，空则全灭；一条消息最多 60 个字符，装 29 个键，其余的用 `G+` 追加到上一条 `GH` / `GT` 里，32 个琴键就是两条）；`GT` 同样的键，常亮（这一步学会了）；`GK<kk><0|1>` 网页按键 / 用户按键（网页按的键不报回，也不算用户完成了这一步：说明书的「演示」用它在设备上做一遍这一步，再让设备回到这一步开始时）；`GD<56 位十六进制>` 设备的按键灯（每个键一位，0–15 是看到的亮度，变了才发，最多 20 次每秒，只发给 USB 上的网页；现在的教程页不显示它，灯在机器上看）。状态 `GS` 是字母加数字（`Controller::GuideState`：场景、页、轨道、引擎、播放、弹窗、按住的地图、这一轨 Pattern 有音符的步数、Pattern 数、当前 Pattern、音阶、键盘排列、和弦、琶音、SONG 段数、SONG 模式、SONG 光标格的 Pattern、光标所在的段和它的小节数、音色），最后是页签名。变了才发，最多 20 次每秒。
- **用户的工程不动**：开始时先把工作状态写进卡上的自动保存（没有卡就拒绝，`GE0 card`；写不进去 `GE0 save`），再换成一个新的练习工程；教程期间自动保存不写，也不为练习工程腾地方。网页说结束、30 秒没有消息、USB 线拔掉、或 app 被挂起时，从自动保存恢复工作状态，回到原来的画面和 SONG / LOOP。断电只丢练习工程。
- **提示**：这一步要按的键全亮（键盘上不出声的琴键除外：PIANO 的缺口不亮，网页的图上也淡掉，网页按机器的状态 `z` 排列、`e` 引擎判断），其他键最多半亮，和网页的图一样（要按的框出，其余变暗）；提示里的边缘键也全亮（平时边缘键不超过最暗的亮度），屏幕边条跟着变。用户还没做对时这些键慢闪（每 0.5 秒亮灭一次，`GH`，`LedEffect::Hint`）；做对了（学会了）就常亮（`GT`），让用户再多按几个键试试（用户要求，2026-11-04：不要一直闪）。网页不自动翻页：「学会了」出现在说明文字下面（不遮挡图），「下一步」从灰色变亮，用户自己点。没有提示时一切照常。屏幕右上角显示 GUIDE。
- **课程和固件对得上**：构建说明书时，每一课的每一步都在固件上（CardenzaSimShot 的 `app` 命令）按它的示范按键跑一遍，完成条件不满足就构建失败；每一步的截图就是这次跑出来的。
- **启动器和固件课程**：由 `docs/learn/guide.json` 的 `hidden` 控制可见性；课程动画在 `wiki/src/guide/guide-anim.js`。启动器来自独立项目 `203-Systems/203-launcher`，更新课程时核对它的当前流程。
