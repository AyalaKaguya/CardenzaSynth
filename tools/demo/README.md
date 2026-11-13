# Neon Circuit demo

Generate the original 120-second, 128 BPM Electro House arrangement with the native Drums, Juno and DX7 engines. The Python script requires NumPy; the renderer requires CMake, a C++17 compiler and the AMY submodule.

From the repository root, using a POSIX shell:

```sh
git submodule update --init --recursive
cmake -S tools/demo -B build/demo -DCMAKE_BUILD_TYPE=Release
cmake --build build/demo --parallel 4
python3 tools/demo/compose_demo.py compose outputs/neon-circuit
for track in 0 1 2 3; do
  build/demo/CardenzaDemo outputs/neon-circuit/score.txt "$track" "outputs/neon-circuit/stem-$track.wav" 120
done
python3 tools/demo/compose_demo.py mix outputs/neon-circuit
```

The output includes the score, MIDI, composition metadata, four mono stems, a native mono mix, a processed stereo mix and audio measurements. Stereo effects, ducking, transitions and full-song playback belong to the offline renderer and mix stage.

To export playable examples without rendering audio or installing NumPy, run:

```sh
python tools/demo/export_projects.py
```

The command creates a new `outputs/neon-circuit/projects-<timestamp>/` directory and prints its
manifest with SHA-256 checksums. `--output <new-directory>` selects a destination; an existing
directory is refused. The four Neon Circuit loop projects preserve the score's original notes and
sounds, with eight patterns per track and Drop, Intro, Break or Build selected initially.
`HITS_Demo.ccproj` adds a short 100 BPM drum loop with one, two, three and four hits, and NEW HITS 2.
All five files use project format 8, which preserves HITS; firmware limited to format 7 cannot open
them. The storage's SD layout protocol remains 2.

Copy them to the card's `CardenzaSynth/Projects/` directory and open SETUP > FILES. For MosaicoNative,
use `python mosaico.py native-files put <local-file> Projects/<name>.ccproj --device-id <id>`.
These examples use LOOP mode; SONG arrangement is available separately in the firmware.

The C++ renderer's `export` action also writes four projects using the current storage writer under
`sdcard/CardenzaSynth/Projects/` in its working directory.

Generated audio, captures, firmware and backups under `outputs/` are local artifacts and are ignored by Git.
