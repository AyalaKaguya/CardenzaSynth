"""Export upstream Neon Circuit loop projects without rendering or playing audio.

Uses the unmodified upstream musical definitions and pattern expression. Compact
engine states store their named factory base plus the original overrides;
PagedSynthEngine::RestoreState supplies omitted factory parameter values.
"""
from __future__ import annotations

import argparse
import ast
from collections import Counter
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import re
import shlex
import subprocess


ROOT = Path(__file__).resolve().parents[2]


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def read_projects(path):
    """Independently consume every field written below, following format 8."""
    tokens = shlex.split(path.read_text(encoding="utf-8"))
    position = 0

    def take(count=1):
        nonlocal position
        values = tokens[position:position + count]
        require(len(values) == count, "Truncated project")
        position += count
        return values[0] if count == 1 else values

    def expect(word):
        require(take() == word, "Unexpected field: " + word)

    expect("CARDENZA_PROJECT"); require(take() == "8", "Wrong format")
    expect("BUILD"); require(take(2) == ["CardenzaSynth", "1"], "Wrong product")
    expect("UI"); require(take(6) == ["1", "0", "4", "1", "1", "15"], "Wrong UI defaults")
    expect("MASTER"); take()
    for track in range(4):
        expect("MIX"); require(int(take()) == track, "Wrong mix track"); take(3)
    expect("SYSTEM"); take()
    expect("FX"); take(int(take()))
    expect("KEYS"); take(2)
    for track in range(4):
        expect("PLAYFX"); require(int(take()) == track, "Wrong play track"); take(7)
    engines = []
    for track in range(4):
        expect("TRACK"); require(int(take()) == track, "Wrong engine track")
        kind, base, preset, name, sample = take(5)
        require(sample == "", "Unexpected sample dependency")
        expect("PARAM")
        values = {}
        for _ in range(int(take())):
            key, value = take(2)
            require(key not in values, "Duplicate parameter")
            values[key] = int(value)
        engines.append((int(kind), base, preset, name, values))
    expect("SEQ"); tempo, swing = map(int, take(2))
    tracks = []
    for _ in range(4):
        expect("T"); count, active, velocity, length, hits, enabled, muted = map(int, take(7))
        require(count == 8 and 0 <= active < 8 and velocity == 110 and length == 24 and hits == 1 and enabled == 1 and muted == 0, "Invalid track")
        require(list(map(int, take(count))) == [16] * 8, "Wrong pattern lengths")
        expect("NOTES")
        events = [tuple(map(int, take(7))) for _ in range(int(take()))]
        tracks.append((active, events))
    expect("SONG"); require(take(2) == ["0", "1"], "Expected original loop export")
    expect("END"); require(position == len(tokens), "Unexpected trailing fields")
    return engines, tempo, swing, tracks


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=ROOT / "outputs/neon-circuit" / datetime.now(timezone.utc).strftime("projects-%Y%m%dT%H%M%SZ"))
    arguments = parser.parse_args()
    source = (ROOT / "tools/demo/compose_demo.py").read_bytes()
    header = (ROOT / "Applications/Synth/Runtime/ProjectStorage.h").read_text(encoding="utf-8")
    require(re.search(r"kProjectWriteVersion\s*=\s*8\s*;", header), "Enable project format 8 before exporting these examples")
    tree = ast.parse(source.decode("utf-8"))
    wanted = {"BPM", "ROOTS", "CHORDS", "HOOK", "PATCHES", "PARAMS"}
    definitions = [node for node in tree.body if
                   (isinstance(node, ast.Assign) and any(isinstance(target, ast.Name) and target.id in wanted for target in node.targets)) or
                   (isinstance(node, ast.FunctionDef) and node.name == "make_bar")]
    compose = next(node for node in tree.body if isinstance(node, ast.FunctionDef) and node.name == "compose")
    pattern_expression = next(node for node in compose.body if isinstance(node, ast.Assign) and
                              any(isinstance(target, ast.Name) and target.id == "patterns" for target in node.targets))
    module = ast.Module(body=definitions + [pattern_expression], type_ignores=[])
    namespace = {}
    exec(compile(module, "upstream/tools/demo/compose_demo.py", "exec"), namespace)

    patches = namespace["PATCHES"]
    parameters = namespace["PARAMS"]
    patterns = namespace["patterns"]
    require(len(patterns) == 8 and len(patches) == 4, "Unexpected upstream composition")
    sources = {
        1: ROOT / "Library/Synth/Core/Engines/Dx7/Dx7SynthEngine.cpp",
        2: ROOT / "Library/Synth/Core/Engines/Juno/JunoSynthEngine.cpp",
        4: ROOT / "Library/Synth/Core/Engines/Drums/DrumsSynthEngine.cpp",
    }
    for (kind, preset, _), values in zip(patches, parameters):
        engine = sources[kind].read_text(encoding="utf-8")
        for key, value in values.items():
            if kind == 4:
                declarations = engine + (sources[4].with_suffix(".h")).read_text(encoding="utf-8")
                require('"' + key + '"' in declarations and 0 <= value <= 127, "Unknown drum parameter: " + key)
            else:
                match = re.search(r'RegisterParameter\(\{"' + re.escape(key) + r'",.*?SynthParameterKind::\w+,\s*(-?\d+),\s*(-?\d+)', engine)
                require(match is not None and int(match[1]) <= value <= int(match[2]), "Invalid engine parameter: " + key)
        if kind != 4:
            catalog = sources[kind].with_name("Dx7FactoryPresets.cpp" if kind == 1 else "JunoFactoryPresets.cpp").read_text(encoding="utf-8")
            require('"' + preset + '"' in catalog, "Missing factory preset: " + preset)

    events = [[] for _ in range(4)]
    for pattern, tracks in enumerate(patterns):
        for track, notes in enumerate(tracks):
            for step, pitch, velocity, duration in notes:
                index = int(step)
                micro = round((step - index) * 6)
                length = min(round(duration * 24), 384 - index * 24 - micro * 4)
                event = (pattern, index, pitch, velocity, max(1, length), micro, 1)
                require(0 <= index < 16 and 0 <= pitch <= 127 and 1 <= velocity <= 127 and 0 <= micro < 6, "Invalid note")
                if track == 0:
                    require(pitch in (36, 38, 42, 46, 39, 43, 56, 70), "Invalid drum note")
                events[track].append(event)
    for track in events:
        require(max(Counter((event[0], event[1]) for event in track).values(), default=0) <= 8, "Too many notes in one step")
        require(len({(event[0], event[1], event[2]) for event in track}) == len(track), "Duplicate pitch on one step")

    output = arguments.output.expanduser().resolve()
    output.mkdir(parents=True, exist_ok=False)
    manifest = {"composition": "Neon Circuit", "revision": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip(),
                "source": "tools/demo/compose_demo.py", "source_sha256": hashlib.sha256(source).hexdigest(),
                "format": 8, "bpm": namespace["BPM"], "tracks": 4, "patterns_per_track": 8,
                "song_rows": 0, "state_encoding": "original factory base plus original explicit overrides", "files": []}
    for section, active in (("Drop", 0), ("Intro", 5), ("Break", 6), ("Build", 7)):
        lines = ["CARDENZA_PROJECT 8", "BUILD CardenzaSynth 1", "UI 1 0 4 1 1 15", "MASTER 80"]
        lines.extend(f"MIX {track} 100 0 0" for track in range(4))
        lines += ["SYSTEM 180", "FX 12 7 56 72 100 76 52 36 100 0 64 64 0", "KEYS 0 110"]
        lines.extend(f"PLAYFX {track} 0 0 0 3 1 50 0" for track in range(4))
        for track, ((kind, preset, name), values) in enumerate(zip(patches, parameters)):
            lines.append(f"TRACK {track} {kind} {json.dumps(preset)} {json.dumps(preset)} {json.dumps(name)} \"\"")
            lines.append(f"PARAM {len(values)}")
            lines.extend(f"{json.dumps(key)} {value}" for key, value in sorted(values.items()))
        lines.append(f"SEQ {namespace['BPM']} 50")
        for track in events:
            lines.append(f"T 8 {active} 110 24 1 1 0 " + " ".join(["16"] * 8))
            lines.append(f"NOTES {len(track)}")
            lines.extend(" ".join(map(str, note)) for note in track)
        lines += ["SONG 0 1", "END"]
        path = output / f"NeonCircuit_{section}.ccproj"
        path.write_bytes(("\n".join(lines) + "\n").encode("utf-8"))
        loaded_engines, bpm, swing, loaded_tracks = read_projects(path)
        require(bpm == namespace["BPM"] and swing == 50, "Tempo differs after export")
        for track in range(4):
            kind, preset, name = patches[track]
            require(loaded_engines[track] == (kind, preset, preset, name, parameters[track]), "Engine settings differ after export")
            require(loaded_tracks[track] == (active, events[track]), "Notes differ after export")
        data = path.read_bytes()
        manifest["files"].append({"name": path.name, "active_pattern": active, "bytes": len(data), "sha256": hashlib.sha256(data).hexdigest(),
                                  "notes_per_track": [len(track) for track in events]})
    # A small playable ratchet example, separate from the unchanged Neon Circuit score.
    lines = (output / "NeonCircuit_Drop.ccproj").read_text(encoding="utf-8").split("\nSEQ ", 1)[0].splitlines()
    lines[2] = "UI 0 0 4 1 1 15"
    lines.append("SEQ 100 50")
    hits_notes = [(0, 0, 36, 96, 24, 0, 1), (0, 4, 38, 90, 24, 0, 2),
                  (0, 8, 36, 96, 24, 0, 3), (0, 12, 38, 90, 24, 0, 4),
                  (0, 2, 42, 72, 6, 0, 1), (0, 6, 42, 72, 6, 0, 2),
                  (0, 10, 42, 72, 6, 0, 3), (0, 14, 42, 72, 6, 0, 4)]
    for track in range(4):
        lines.append(f"T 1 0 110 24 {2 if track == 0 else 1} {1 if track == 0 else 0} 0 16")
        lines.append(f"NOTES {len(hits_notes) if track == 0 else 0}")
        if track == 0:
            lines.extend(" ".join(map(str, note)) for note in sorted(hits_notes))
    lines.extend(["SONG 0 1", "END"])
    data = ("\n".join(lines) + "\n").encode("utf-8")
    hits_file = output / "HITS_Demo.ccproj"
    hits_file.write_bytes(data)
    manifest["files"].append({"name": hits_file.name, "bytes": len(data), "sha256": hashlib.sha256(data).hexdigest(),
                              "bpm": 100, "notes_per_track": [8, 0, 0, 0], "hits": [1, 2, 3, 4], "default_hits": 2})
    (output / "manifest.json").write_text(json.dumps(manifest, indent=2), encoding="utf-8")
    print(json.dumps({"output": str(output), **manifest}, indent=2))


if __name__ == "__main__":
    main()
