# App storage protocol (version 2)

Date: 2026-11-05. CardenzaSynth keeps everything it stores on the SD card, under `/CardenzaSynth`, in
the layout the 0.1.0 release wrote: settings, projects, presets, samples, the working state and the
log. The SD card is the only persistent store: nothing is kept in internal flash (NVS is neither
read, written nor erased). Without a card at the start the device runs, says so, and saves nothing
until it restarts with one (section 3).

This reference defines paths, formats, ownership and compatibility. Code: `OS/Storage/` (API),
`OS/System/` (lifecycle, the log), `Devices/Device.h` (`Device::Storage`, the low-level card).

## 1. Layout on the SD card

```text
<card>/CardenzaSynth/
  Settings/
    system.cfg                 device settings and the storage protocol version (the OS's)
    synth.cfg                  the groovebox's settings (SELECT)
  Projects/
    <name>.ccproj              projects
  Presets/
    DX7/user.ccpreset          the user preset bank of each engine
    Juno/user.ccpreset
    Sampler/user.ccpreset
    Drums/user.ccpreset
  Samples/
    <name>.wav                 samples, referenced by file name; any app may use them
  Session/
    autosave.ccproj            the working state (the autosave), in the project format
    autosave.txt               its generation, name and unsaved mark
    restoring.txt              starts begun with the autosave and not finished (while one runs)
    take-<track>.wav           a sample recording in progress or not saved yet
  Logs/
    log.txt                    the console log while sys.log is on (log.1.txt: the one before)
  Apps/<Author>/<Name>/        any other app, created when it first needs it
    Settings/app.cfg
    Projects/ Presets/ Session/ and folders of its own
```

- `<card>` is `/sdcard` on the device, `<working directory>/sdcard` in the simulator (the desktop
  simulator starts in its executable's folder) and `/sdcard` (IndexedDB) in the browser simulator.
- The built-in groovebox's app id is `Cardenza/Synth`, the primary app: its `AppFolder` is the product
  root itself, with `Settings/synth.cfg` as its settings file. The id is not a disk path.
- Other apps use `Apps/<Author>/<Name>/`; both id parts are 1–24 characters of `A–Z a–z 0–9 _ -`. A
  card with only the groovebox has no `Apps/` folder.
- Ownership: only the OS writes `Settings/system.cfg` and `Logs/`. An app writes its own settings and
  folders, and `Samples/`, which every app shares: renaming or deleting a sample affects every project
  that names it.
- FAT long file names are required (`CardenzaSynth`, `.ccproj`, `.ccpreset`, `.bak` / `.tmp` files).
  File names the user sees stay ASCII.

## 2. File formats

| Content | Format | Compatibility |
|---|---|---|
| `Settings/*.cfg` | ASCII text, `key=value` lines | Unknown keys survive a read / write cycle |
| `Projects/*.ccproj`, `Session/autosave.ccproj` | Text beginning with `CARDENZA_PROJECT 7` | Reads v1–v8; writes v7 (v8 from the next test build) |
| `Session/autosave.txt`, `Session/restoring.txt` | `key=value` lines | Written whole by the groovebox; other keys are not kept |
| `Presets/<engine>/user.ccpreset` | Text beginning with `CARDENZA_PRESETS 1 "<engine-id>"` | Preset format 1 |
| `Samples/*.wav`, `Session/take-*.wav` | Mono PCM16 WAV | File names and project references stay valid |
| `Logs/log*.txt` | Text, one line per console line | Read by people |

Storage protocol 2 describes the layout and how settings are kept; it does not version the project,
preset or WAV formats. Project format 6 added the keyboard and each track's chord and arpeggiator;
format 7 gives every pattern its own length (up to 256 steps). Version 5 was only written by
unreleased development builds (its `PLAY` line is skipped). Format 8 adds HITS (a note fired 1–4
times in its step): each `T` line gains the track's new-note hits after its new-note length, and every
note line a seventh number, the note's hits. It is read already and written from the one format bump
of the next test build (`kProjectWriteVersion`); until then projects are written as format 7 and keep
no hits. Older formats read as one hit per note. The 0.1.0 release reads formats 1–6: a project saved
by this firmware is format 7.

### Settings syntax

```text
# Cardenza settings: Cardenza/Synth (storage protocol 2)
# key=value per line; unknown keys are kept
keys.select=1
```

- One `key=value` per line. Lines starting with `#` after optional spaces are comments. Spaces around
  the key and the value are ignored; CRLF line ends are accepted.
- Keys are 1–48 characters of `a–z 0–9 _ .` and start with a letter or digit. Use dotted groups such
  as `keys.scale`, `arp.rate` and `sys.volume`.
- Values are at most 200 printable ASCII characters, without leading or trailing spaces. Numbers are
  decimal integers. Booleans are written as `1` or `0`; reading also accepts `true/false`, `on/off`
  and `yes/no`. Enumeration indices are only ever appended to.
- Malformed lines are skipped. A repeated key keeps its first position and its last value. A value
  that is not a number or is out of range reads as the caller's default. Unknown keys are written back
  unchanged, so a newer and an older firmware can share a card.
- A settings file is at most 16 KB. `Settings/system.cfg` holds `protocol=2`.

### File replacement

`WriteFileAtomic` writes and closes `<file>.tmp`, moves an existing file to `<file>.bak` (FAT cannot
rename over a file), renames the temporary file into place and removes the backup. Readers use
`<file>.bak` when an interrupted replacement left the file itself missing. Projects, preset banks
and the autosave are replaced the same way. Callers check the result: this keeps a file from being
cut short in place; it cannot save a failing card or a write cut off inside the card.

## 3. The card, and a start without one

The system looks for the card once, as it starts (`os::system::Begin` calls
`os::storage::DecideCardAtStart` before any app is set up).

- **No card at the start**: the whole run makes no card operation, until the device restarts.
  Nothing probes for a card (no `Device::Storage::Mount`), `CardPresent()` and `CardMounted()` are
  false, the file helpers refuse every path on the card, no log is written and nothing is saved; a
  card put in meanwhile changes nothing. Settings start from their defaults and are kept in RAM for
  the run; changing them works as usual. The groovebox opens a notice after its boot animation, `NO
  SD CARD` / `NOTHING WILL BE SAVED`, which ENTER or ESC closes. Saving a project says `NO SD CARD`;
  the project list is empty; the guide does not start (`GE0 card`).
- **A card at the start**: everything below. The card may still go and come back during the run: a
  failed file operation (the file closed) calls `ReportCardIoError`, and when the card no longer
  answers the device counts it as missing (a card that answers, a full one say, stays mounted).
  `CardPresent()` mounts it again on demand; the device probes a missing card at most every five
  seconds, and only when something waits for it (settings or the autosave to write, a sample to load).

## 4. Settings persistence

Settings live in RAM (`os::storage::Settings`): the values read from the card plus the edits made
since. A change marks its store dirty. The system writes dirty settings one second after the last
change, when an app is suspended and on an orderly shutdown; apps leave that to the system.

`Flush()` reads the card file again, applies only this device's edits (removals included) and writes
the result: keys nobody changed here keep what the card has, after a card swap or an edit on a
computer too. When the read fails (no card, an unreadable or oversized file, a folder in its place),
nothing is written, the edits stay dirty and `Flush()` is false; the system tries again five seconds
later. When the card is mounted again after it was out, the stores are refreshed from it
(`RefreshAllSettingsFromCard`, which keeps the edits) and the device settings apply what it holds.
Settings not written are lost when the power goes. `Load()` and `ReloadAllSettings()` discard
pending edits: for tests, not for a card coming back.

## 5. Device settings

`Settings/system.cfg`, owned by the OS:

| Key | Meaning |
|---|---|
| `protocol` | storage protocol version of the card: 2 |
| `sys.volume` | output volume 0–100 by ear: 0 mutes, v is −60 (1 − v/100)^1.5 dB (about −59 dB at 1, −21 dB at 50, −10 dB at 70, 0 dB at 100; `Device::Audio::SetOutputVolume`), default 70 |
| `sys.volume.rev` | the meaning `sys.volume` was saved under, now 3; below that the volume goes back to the default, once (2: the output got louder, the 0.1.0 release saved below it; 3: the 60 dB curve, 2026-11-06, which replaced a −20–0 dB scale) |
| `sys.led` | key LED master brightness 0–15, default 15 |
| `sys.screen` | screen backlight 10–255, default 180 |
| `sys.log` | `1` writes the console log to `Logs/` (OS/System/Log.h), `0` stops it; default 1 |
| `sys.theme` | the screen's colors: `dark` (light on black, the default), `light` (ink on white paper) or `retro` (a backlit green LCD), in any case; any other value reads as `dark` |
| `boot.app` | last non-system app; started at boot when it is still registered |

Volume, key LEDs, screen, the log and the theme are device settings: the OS applies them at boot
(the theme is applied by the app that draws, from `os::storage::ScreenTheme`, before each frame) and
every app changes them only through `os::storage::SetOutputVolume / SetKeyLedBrightness /
SetScreenBacklight / SetLogEnabled / SetScreenTheme`. Projects never carry them.

### The log

`Logs/` belongs to the OS. The device keeps everything the firmware prints (its console) in a small
ring that outlives a crash reset (`Device::Log`, RTC memory on the device), including the crash
itself: reason, task and backtrace as `!!!` lines. While `sys.log` is on and the card is mounted, the
system appends what is new to `Logs/log.txt` every five seconds (every second when the ring is half
full), once at boot right after the settings are read, and before an app switch or shutdown; on the
device the append runs on its background task (`Device::Background`), never while a sample plays from
the card (`SetCardStreaming`). Past 512 KB the file becomes `log.1.txt`, replacing the one before. A
line starts with the seconds since boot; every start begins with `==== <app> <version> (elf <hash>)
started`. There is no heap buffer for it, and a failed write loses only that text. Nothing is written while
nothing is printed, and the firmware's periodic reports print only when they have news (otherwise every
five minutes), so an idle device leaves the card alone. A run without a
card writes no log: the ring keeps the newest lines. Users send these two files when something went
wrong.

## 6. App lifecycle and the working state

- One app runs at a time. `os::system::SwitchApplication(id)` switches between frames: the running
  app's `Suspend()` is called, every settings store is flushed, the app is destroyed (so the next app
  gets the whole heap), then the new app is constructed and `Setup()` runs. Keys held during the
  switch are not delivered to the new app (neither press nor release).
- `Setup()` is the resume point: an app keeps its working state in its `Session/` folder and restores
  it there. There is no warning before a power cut, so an app that wants to survive one writes that
  state as it changes, not only in `Suspend()`.
- Boot starts `boot.app` when it is registered and not a system app, else `Cardenza/Synth`.
- The HOME side button belongs to the running app.
- An app that reads the card in real time (a sample played from its file) says so with
  `SetCardStreaming`: what writes only to keep a record (the log, the autosave) waits meanwhile.

## 7. The groovebox (`Cardenza/Synth`)

| What | Where |
|---|---|
| projects `<name>.ccproj` | `Projects/` |
| user preset banks `user.ccpreset` | `Presets/{DX7,Juno,Sampler,Drums}/` |
| samples (WAV) | `Samples/` |
| app preferences (SELECT) | `Settings/synth.cfg` |
| working state (autosave) | `Session/autosave.ccproj` and `autosave.txt` |
| sample take | `Session/take-<track>.wav`: a sample recording in progress or not saved yet; removed when it is saved or discarded |
| volume, key LEDs, screen | `Settings/system.cfg` (device settings) |

App settings keys: `keys.select` (0 ONE, 1 MULTI: the ROLL selection mode), a preference restored at
start and written a second after a change and on suspend. Older firmware also kept the keyboard
(`keys.layout`, `keys.scale`, `keys.root`, `keys.octave`, `keys.lock`, `keys.velocity`) and one
global chord and arpeggiator (`keys.chord`, `keys.voicing`, `arp.*`) here: the keyboard values are
still read at start (they only matter until a session or project brings its own) and nothing writes
them any more; the old chord and arp keys are ignored and kept.

Projects keep the project: engines and sounds, samples by file name, patterns and song, tempo and
swing, mixer levels / mute / solo, the master effects and the play settings: the keyboard (the UI
line's octave, root, scale and layout, and a `KEYS` line with scale lock and velocity) and each
track's chord and arpeggiator (one `PLAYFX` line per track). Loading a format 6 or 7 project applies
them; older files have none and leave the current ones in place; NEW starts from the defaults. The UI
line's LED brightness, `MASTER` and `SYSTEM` (backlight) are device settings: still written, ignored
when a project loads. Named projects are only written by SAVE.

**Autosave.** The groovebox keeps its working state on the card, so the power can go at any moment.
There is one autosave: `autosave.ccproj` is the whole project in the project format (sounds keep only
what differs from their preset), `autosave.txt` says its generation, name and whether it was unsaved.
The project is written first, each file replaced atomically, so a cut between them leaves the new
project under the name of the one before.

- When: after a key, once the keys have rested 1.5 s with none held, playing or not; it is written on
  the device's background task, so the music and the screen do not wait for the card. While keys keep
  coming, or a sample plays from the card, it waits 60 s at most from the first change it does not
  have. A key that changed nothing writes nothing (the written state is compared first), and nothing
  is written while nothing changes. `Suspend()` writes it right away; so do the guide before it
  starts, room for a load or NEW (below), and the browser page when it is hidden.
- When it fails: the screen says `AUTOSAVE FAILED: SD CARD ERROR` (or `NO SD CARD` when the card is
  out) once, the card is asked whether it still answers (`ReportCardIoError`), and the write is tried
  again two seconds later, then every 30 seconds after three failures in a row. A failure does not
  start the wait over: the changes are due 60 s after the first of them, however many writes failed.
  A card that went out is looked for while the changes wait, when nothing plays.
- At the start `Setup()` restores it, with its name and unsaved state. `restoring.txt` counts the
  starts begun with it and not finished: at 3 it is skipped once and the count goes, so an autosave
  cannot keep crashing the start. One that does not read makes a clean start. A sample it names that
  is not there is kept as a name and loaded when the card is mounted again. A sound this firmware no
  longer knows starts from the engine's first preset.
- Room: two projects do not always fit in RAM side by side. A load or NEW that does not fit first
  writes the autosave and lets go of the current patterns, and brings them back from it when it still
  fails; without a card (or during a lesson) it says `NO MEMORY` instead.
- The guide (a lesson page driving the device, Applications/Synth/GrooveAppGuide.cpp) writes it before it starts,
  writes nothing while its practice project runs, and restores it when it ends. Without a card the
  guide does not start.
- Without a card at the start there is no autosave: nothing is restored and nothing written.

The 0.1.0 release wrote `Session/session.ccproj` and `session.cfg` on suspend instead. They are
neither read nor removed.

## 8. Older layouts and formats

The runtime uses only the paths of section 1, which are the 0.1.0 release's: its settings, projects,
preset banks and samples are read and written in place. Development builds between 0.1.0 and this
firmware used `<card>/Cardenza/{System,Shared/Samples,Apps/Cardenza/Synth}` (protocol 1) and internal
flash (NVS); the firmware before 0.1.0 used `<card>/CardenzaSynth/{Project,Preset,Sample}`. None of
those folders or records is read, moved, changed or deleted, and nothing is imported from them
automatically: a migration is discussed with the user before it is written. Format compatibility is
separate from the folders: project formats 1–7, preset format 1 and compatible WAV files are read
wherever this firmware looks.

## 9. API

`OS/Storage/Storage.h` (namespace `os::storage`):

| API | Responsibility |
|---|---|
| `kProtocolVersion` (2), `kPrimaryAppId` | the protocol and the app that owns the product root |
| `Root()` | `<card>/CardenzaSynth` |
| `SystemDirectory()`, `SystemSettingsPath()` | `Settings/`, `Settings/system.cfg` |
| `SharedDirectory()`, `SharedSamplesDirectory()` | the root and `Samples/` |
| `AppsDirectory()`, `LogDirectory()` | `Apps/`, `Logs/` |
| `AppFolder(id)` | `Root()`, `SettingsPath()`, `Projects()`, `Presets(group)`, `Session()`, `Folder(name)` of an app; empty for an invalid id |
| `DecideCardAtStart()`, `CardMissingThisRun()` | the start's decision (the system's) and whether this run has no card |
| `CardPresent()`, `CardMounted()` | the card, mounting it on demand / not |
| `ReportCardIoError()` | after a failed file operation: the device looks at the card |
| `SetCardStreaming()`, `CardStreaming()` | a reader in real time; record writers wait |
| `CardCapacityBytes()`, `CardFreeBytes()` | capacity and free space |
| `EnsureSystemLayout()`, `EnsureAppLayout(app, groups)` | create the folders on a writable card |
| `WriteFileAtomic`, `ReadFileWithBackup`, `RemoveFile` | file replacement, the interrupted-write fallback, removal |
| `JoinPath`, `Exists`, `IsDirectory`, `IsFile`, `FileSize`, `CreateDirectories`, `RemoveEmptyDirectory`, `ListDirectory` | portable path and file helpers |

`OS/Storage/Settings.h`: `Settings` (`Load`, `RefreshFromCard`, `Has`, `GetString` / `GetInt` /
`GetBool` with defaults and ranges, `SetString` / `SetInt` / `SetBool` / `Remove` in RAM, `Dirty`,
`Revision`, `Flush`), `SystemSettings()`, `AppSettings(id)`, `ReleaseAppSettings(id)`,
`FlushAllSettings()`, `SettingsRevision()`, `AnySettingsDirty()`, `RefreshAllSettingsFromCard()`,
`ReloadAllSettings()`, `ParseSettings`, `FormatSettings`.

`OS/Storage/DeviceSettings.h`: `ApplyDeviceSettings()` and the device settings of section 5.
`OS/System/Log.h` (namespace `os::log`): `Service(now)` (the system loop), `Flush()`, `FilePath()`,
`PreviousFilePath()`. `OS/System/System.h` (namespace `os::system`): `Begin`, `RunFrame`,
`ActiveApplication`, `ActiveApplicationId`, `SwitchApplication(id)`, `BootApplicationId()`,
`Shutdown()`, `kSettingsWriteDelayMs` (1000). `Applications/Application.h`: `virtual void Suspend()`
next to `Setup`, `Tick`, `UpdateLeds`, `RenderFrame` and the key handlers; register an app with
`app::RegisterApplication({id, name, author, isSystem, factory})`.

`Devices/Device.h`, the device layer only: `Device::Storage` (`RootPath()`, `Mount()`,
`IsMounted()`, `CheckAfterIoError()`, `CapacityBytes()`, `FreeBytes()`; nothing in internal flash)
and `Device::Log` (`Take`, `Pending`, `Write`, the console ring the OS writes out). Apps never call
`Device::Storage` (ValidateArchitecture refuses it, and any internal-flash API).

## 10. Device and simulators

- FAT long file names are enabled (`CONFIG_FATFS_LFN_HEAP`).
- The device does not initialise, read, write or erase NVS; the partition table is unchanged, so it
  stays compatible with the Launcher (whose own settings live there).
- The desktop simulator's card is `sdcard/` beside its executable; `CardenzaSim::Input::SetCardInserted(false)`
  takes it out (out when the system starts: a run without a card). `CardenzaSimShot --no-card` starts
  without it.
- The browser simulator mounts `/sdcard` as IDBFS with `autoPersist`, so every write reaches
  IndexedDB; the page also flushes settings and the autosave when it is hidden. Its card is always
  present; without IndexedDB the page says "memory only" and nothing survives a reload.

## 11. Writing an app

1. Pick an id `Author/Name` and register a factory.
2. Keep `AppFolder folder("Author/Name")`; call `EnsureAppLayout(folder, {your preset groups})`
   before writing, and write only below `folder` (and `SharedSamplesDirectory()` for audio).
3. Put settings in `AppSettings("Author/Name")` with dotted keys, read them with defaults and ranges,
   and let the system write them.
4. Save documents with `WriteFileAtomic` (or the same tmp / bak scheme) and read them with
   `ReadFileWithBackup`; check every result and report a failed save; call `ReportCardIoError` after a
   failed file operation of your own; version your own formats and keep reading old versions.
5. Keep the working state in `folder.Session()`: write it as it changes and in `Suspend()`, restore it
   in `Setup()`.
6. Check `CardMissingThisRun()`: without a card at the start nothing can be saved, and the user should
   know before working.
7. Change volume, key LEDs and screen only through `os::storage::Set…` so every app agrees on them.
