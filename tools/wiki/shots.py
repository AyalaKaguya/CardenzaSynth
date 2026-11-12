"""What the wiki shows of the firmware: one script walks the UI in the simulator (CardenzaSimShot) and
takes every screenshot of the manual with the key lights of that moment; CardenzaLearnDump
(tools/wiki/learn_dump.cpp) exports the pages, parameters and shortcuts the reference tables list.
tools/wiki/generate.py runs both. A screenshot is named in the manual's MDX as <Shot name=...>.
"""
import json
import shutil
import subprocess
import sys

# ---------------------------------------------------------------------------------------------
# Screenshots: one script walks the firmware UI; every shot waits until animations have settled.

def grid_key(index):
    return f"{index // 4},{9 + index % 4}"


class Script:
    def __init__(self):
        self.lines = ["wait 140"]

    def act(self, *commands):
        self.lines.extend(commands)
        return self

    def wait(self, frames=45):
        self.lines.append(f"wait {frames}")
        return self

    def shot(self, name, frames=45):
        self.wait(frames)
        self.lines.append(f"screenshot {name}")
        return self

    def scene(self, number):
        # The on-screen scene number: 0 SETUP, 1 INSTR, 2 SEQ, 3 TAPE (ESC map column = number).
        return self.act("down esc", f"tap {grid_key(number)}", "up esc").wait(30)

    def track(self, number):
        return self.act("down tab", f"tap {grid_key(number - 1)}", "up tab").wait(30)

    def page(self, index):
        return self.act("down ctrl", f"tap {grid_key(index)}", "up ctrl").wait(30)


def shot_script():
    s = Script()
    # The boot screen: INSTR VOICE on T1 (the drums). Then SEQ, T1 drums and a first beat (kick,
    # snare, hat).
    s.shot("instr-boot")
    s.scene(2).shot("seq-empty")
    s.track(1)
    s.act("down 3,1").shot("seq-drum-held", 20)
    s.act(*[f"tap {grid_key(i)}" for i in (0, 4, 8, 12)], "up 3,1")
    s.act("down 3,2", f"tap {grid_key(4)}", f"tap {grid_key(12)}", "up 3,2")
    s.act("down 3,3", *[f"tap {grid_key(i)}" for i in range(0, 16, 2)], "up 3,3")
    s.shot("seq-drums")
    # Hold step 1: it is selected while held; OK twice picks VEL and ADD raises it.
    s.act(f"down {grid_key(0)}").wait(20).act("tap enter", "tap enter", "tap del").shot("seq-drum-edit", 10).act(f"up {grid_key(0)}").wait(120)
    s.act("tap space").shot("seq-drums-playing", 40)
    s.act("tap space").wait(40)
    # TAB: next track; the left keys show which one for a moment.
    s.act("tap tab").shot("tab-hint", 22)
    s.shot("seq-keyboard", 60)
    # Notes: hold a pad, tap steps.
    # On a melodic track C3 is pad 3,1: the bottom row is the lower octave's white keys.
    s.act("down 3,1").shot("seq-hold-pad", 20)
    s.act(f"tap {grid_key(0)}").shot("seq-hold-tap", 10)
    s.act(f"tap {grid_key(4)}", f"tap {grid_key(8)}", "up 3,1")
    s.act("down 3,3", f"tap {grid_key(2)}", f"tap {grid_key(10)}", "up 3,3")
    s.act("down 3,5", f"tap {grid_key(12)}", f"tap {grid_key(14)}", "up 3,5")
    s.shot("seq-notes")
    s.act(f"tap {grid_key(4)}")
    s.shot("seq-selected")
    s.act("tap enter", "tap enter")
    s.shot("seq-lane")
    s.act(f"tap {grid_key(4)}").wait(20)
    s.act("tap space").shot("seq-playing", 30)
    s.act("tap space").wait(40)
    # STEP view: OK switches the view; a tapped step stays selected and takes the pads' notes.
    s.act("tap enter").shot("seq-step-view", 30).act(f"tap {grid_key(1)}", "tap 3,2", "tap 3,3")
    s.shot("seq-step", 140)
    # ESC lets the selected step go first: with one selected, OK cycles its edit lane.
    s.act("tap esc", "tap enter").wait(200)
    # Capture: a few phrases, then hold OK for the RECORD panel; OK puts a bar into a new pattern.
    for pad in ("3,1", "3,3", "3,5", "3,3", "3,1"):
        s.act(f"down {pad}").wait(8).act(f"up {pad}").wait(10)
    s.wait(200)
    for pad in ("3,5", "3,6", "3,8"):
        s.act(f"down {pad}").wait(12).act(f"up {pad}").wait(6)
    s.act("down enter").wait(40).act("up enter").wait(10).act(f"tap {grid_key(5)}")
    s.shot("record", 30)
    s.act("tap enter").shot("record-placed", 40)
    s.page(1).shot("seq-pattern")
    s.page(2).shot("seq-track")
    s.page(0)
    s.act("down ctrl", f"tap {grid_key(1)}", "up ctrl").wait(30)
    s.act(f"tap {grid_key(0)}").wait(20)
    s.page(0)
    # Hold maps.
    s.act("down esc").shot("map-esc", 30).act("up esc")
    s.act("down tab").shot("map-tab", 30).act("up tab")
    s.act("down ctrl").shot("map-ctrl", 30).act("up ctrl").wait(30)
    s.act("down fn").shot("fn-held", 30)
    s.act("tap 0,6").shot("fn-quick", 6)
    s.act("tap 1,6").wait(60).act("tap 0,7").shot("fn-volume", 6)
    s.act("tap 1,7").wait(60).act("tap 0,8").shot("fn-octave", 6)
    s.act("tap 1,8").act("up fn").wait(60)
    # A tap on FN keeps the menu open.
    s.act("tap fn").shot("fn-menu-open", 30).act("tap fn").wait(40)
    # INSTR on T2 (Juno): VOICE, CHORD, ARP, DCO, VCF, ENV, LFO, FX; and its FN menu.
    s.scene(1).page(0).shot("instr-voice")
    s.act("down fn").shot("fn-menu-instr", 30).act("up fn").wait(40)
    s.act(f"tap {grid_key(0)}").shot("engine-picker").act("tap esc").wait(30)
    s.act(f"tap {grid_key(1)}").shot("preset-browser")
    # The pads do not type in a browser: FN + F opens the search field.
    s.act("down fn", "tap f", "up fn").wait(10).act("tap b", "tap a", "tap s", "tap s").shot("preset-search", 30)
    s.act("tap enter").shot("preset-filtered", 30).act("tap esc").wait(10).act("tap esc").wait(30)
    # CHORD and ARP: the track's own MIDI effects.
    s.page(1).shot("instr-chord")
    s.act(f"tap {grid_key(0)}").shot("chord-on")
    s.act("down 3,1").shot("chord-held", 20).act("up 3,1").wait(10)
    s.act(*["tap 1,13"] * 12).wait(20)
    s.page(2).shot("instr-arp")
    s.act(f"tap {grid_key(0)}").wait(10)
    s.act("down 3,1", "down 3,3", "down 3,5").shot("arp-running", 50).act("up 3,1", "up 3,3", "up 3,5")
    s.act(f"tap {grid_key(0)}", *["tap 1,13"] * 8).wait(20)
    for index, name in enumerate(["dco", "vcf", "env", "lfo", "fx"], start=3):
        s.page(index).shot(f"instr-juno-{name}")
    # T3 DX7: VOICE, CHORD, ARP, ALGO, LEVELS, OP1-OP6, LFO, FX.
    s.track(3)
    for index, name in [(0, "voice"), (3, "algo"), (4, "levels"), (5, "op1"), (11, "lfo")]:
        s.page(index).shot(f"instr-dx7-{name}")
    # T1 drums (VOICE, KIT, FX), T4 sampler (VOICE, CHORD, ARP, SAMPLE, TONE, FX).
    s.track(1).page(1).shot("instr-drums-kit")
    s.track(4).page(3).shot("instr-sampler-sample")
    s.act(f"tap {grid_key(0)}").shot("sampler-files", 30).act("tap esc").wait(30)
    s.act(f"tap {grid_key(1)}").shot("sampler-rec", 30).act("tap esc").wait(30)
    s.page(4).shot("instr-sampler-tone")
    s.track(2).page(0)
    # SETUP (scene 0, ESC + GRID column 1): KEYS with the keyboard layouts, SYSTEM with the LED ramp.
    s.scene(0)
    s.page(2).shot("setup-keys")
    s.act(f"tap {grid_key(0)}").shot("keys-scale").act(f"tap {grid_key(0)}").shot("keys-chroma")
    s.act(f"tap {grid_key(0)}").wait(20)
    s.page(3).shot("setup-system")
    s.act(f"tap {grid_key(1)}").shot("setup-keyled")
    s.act(f"tap {grid_key(0)}").act("tap esc").wait(40)
    # TAPE: a few song rows first (SESSION capture).
    s.scene(3).page(0).shot("tape-session")
    s.act("tap space").wait(20).act(f"tap {grid_key(5)}").shot("tape-session-queued", 8)
    s.act("tap space").wait(40).act(f"tap {grid_key(1)}").wait(20)
    s.act("down fn", "tap enter", "up fn").wait(10)
    s.act("down fn", "tap enter", "up fn").wait(10)
    s.page(1).shot("tape-song")
    s.page(2).shot("tape-mix")
    s.page(3).shot("tape-sends")
    s.page(4).shot("tape-fx")
    # SETUP PROJECT (SAVE, LOAD, NEW) and FILES; SAVE AS is in the FN menu.
    s.scene(0).page(0).shot("setup-project")
    s.act("down fn", f"tap {grid_key(2)}", "up fn").shot("text-entry", 30)
    s.act("tap esc").wait(30)
    s.act(f"tap {grid_key(2)}").shot("confirm")
    s.act("tap esc").wait(30)
    s.act("down fn").shot("fn-menu-setup", 30).act("up fn").wait(40)
    s.act("tap esc").wait(30)
    # Recording armed while playing.
    s.scene(2).page(0).act("down fn", "tap space", "up fn", "tap space").shot("seq-recording", 30)
    s.act("tap space", "down fn", "tap space", "up fn").wait(40)
    # FN + S: a new project is saved at once as SONG 01, no name field.
    s.act("down fn", "tap s", "up fn").shot("save-done", 30)
    s.wait(60)
    # FILES after the save: the list shows the project.
    s.scene(0).page(1).shot("setup-files")
    s.act("tap esc").wait(30)
    # A long pattern: SEQ menu LEN + twice (16 -> 48 steps), then ADD to its second page.
    s.scene(2).page(0)
    s.act("down fn", f"tap {grid_key(11)}", f"tap {grid_key(11)}", "up fn", "tap del").shot("seq-long", 30)
    # A sound with edits asks before another one replaces it: CUTOFF on VCF, then ADD on VOICE.
    s.scene(1).page(4).act(f"tap {grid_key(0)}", "tap del").wait(20)
    s.page(0).act("tap del").shot("preset-confirm", 30).act("tap esc").wait(30)
    return "\n".join(s.lines) + "\n"


def run_shots(sim_build, out_dir, work):
    shot_tool = sim_build / "Devices" / "CardenzaSim" / "CardenzaSimShot"
    if not shot_tool.exists():
        sys.exit(f"shots: {shot_tool} missing (build the host target CardenzaSimShot)")
    script = work / "learn.script"
    script.write_text(shot_script())
    raw = work / "shots"
    shutil.rmtree(raw, ignore_errors=True)
    # A fresh card every run: settings, projects and the autosaved working state from the last run
    # would change the screens.
    shutil.rmtree(work / "sdcard", ignore_errors=True)
    subprocess.run([str(shot_tool), "--script", str(script), "--out", str(raw), "--scale", "2"], check=True,
                   cwd=work, stdout=subprocess.DEVNULL)
    leds = {}
    img = out_dir / "img"
    img.mkdir(parents=True, exist_ok=True)
    try:
        from PIL import Image  # optional: the simulator writes uncompressed PNGs
    except ImportError:
        Image = None
    for png in sorted(raw.glob("*.png")):
        if Image is None:
            shutil.copyfile(png, img / png.name)
        else:
            # A screen has a handful of colours: a palette PNG is a few KB instead of 390.
            with Image.open(png) as shot:
                shot.convert("RGB").quantize(colors=64, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.NONE).save(
                    img / png.name, optimize=True)
        led_file = png.with_suffix(".leds.txt")
        if led_file.exists():
            rows = [list(map(int, line.split())) for line in led_file.read_text().splitlines()[:4]]
            leds[png.stem] = rows
    (work / "leds.json").write_text(json.dumps(leds))
    return leds


def run_dump(sim_build):
    tool = sim_build / "tests" / "CardenzaLearnDump"
    if not tool.exists():
        sys.exit(f"shots: {tool} missing (build the host target CardenzaLearnDump)")
    out = subprocess.run([str(tool)], check=True, capture_output=True, text=True).stdout
    return json.loads(out)
