"""A Cardenza on this computer’s USB, from the command line: the host side of CardenzaHAL::USB and of
this firmware’s app channel. Any firmware on CardenzaHAL answers list, command and console.

Talks only to devices with 203 Systems’ vendor ID: the HID system channel (report 0xCB), the firmware’s
app reports (0xFF), MIDI ports and audio devices whose name contains "Cardenza".

usage: python3 tools/usb/cardenza_usb.py list | command <identify|reboot|launcher|download> | stats
       | console <seconds> | test | guide

  list      the Cardenza’s HID, MIDI and audio devices
  command   restart it (reboot), into the 203 Launcher (launcher) or into ROM download mode for esptool
            (download), or ask who it is (identify); over HID, else over MIDI SysEx F0 23 <code> F7
  stats     what has gone through its USB MIDI and audio (this firmware)
  console   its console and log for that many seconds, over HID
  test      plays and records through its USB audio and checks the counts (this firmware)
  guide     walks the Learn guide link (SysEx F0 23 7F) through a start, a lesson, a key and the end

Needs: pip install hidapi (HID), sounddevice numpy (list audio, test). MIDI goes through macOS CoreMIDI
(no package); elsewhere the MIDI parts report that and the HID parts still work.
"""

import collections
import contextlib
import os
import re
import struct
import sys
import threading
import time

VENDOR_ID = 0x0203
REPORT_SYSTEM = 0xCB
REPORT_APP = 0xFF
REPORT_SIZE = 63
COMMANDS = {"launcher": 0x40, "reboot": 0x41, "download": 0x42, "console": 0x43, "identify": 0x44}
NAME = "Cardenza"


def hid_devices():
    import hid
    return [d for d in hid.enumerate(VENDOR_ID, 0) if d.get("usage_page") == 0xFF00]


def open_hid():
    import hid
    devices = hid_devices()
    if not devices:
        return None, "no Cardenza HID device"
    device = hid.device()
    try:
        device.open_path(devices[0]["path"])
    except OSError as error:
        return None, "HID open failed: %s" % error
    device.set_nonblocking(False)
    return device, None


def send(device, report_id, payload):
    data = bytes(payload[:REPORT_SIZE]).ljust(REPORT_SIZE, b"\0")
    device.write(bytes([report_id]) + data)


def receive(device, report_id, seconds):
    end = time.time() + seconds
    while time.time() < end:
        data = device.read(REPORT_SIZE + 1, 100)
        if data and data[0] == report_id:
            return bytes(data[1:])
    return None


class CoreMidi:
    """The Cardenza's MIDI ports through macOS CoreMIDI (ctypes: no package to build)."""

    def __init__(self):
        self._closed = True
        import ctypes
        import ctypes.util
        self.c = ctypes
        cm = ctypes.cdll.LoadLibrary(ctypes.util.find_library("CoreMIDI"))
        cf = ctypes.cdll.LoadLibrary(ctypes.util.find_library("CoreFoundation"))
        u32, vp = ctypes.c_uint32, ctypes.c_void_p
        cf.CFStringCreateWithCString.restype = vp
        cf.CFStringCreateWithCString.argtypes = [vp, ctypes.c_char_p, u32]
        cf.CFStringGetCString.restype = ctypes.c_bool
        cf.CFStringGetCString.argtypes = [vp, ctypes.c_char_p, ctypes.c_long, u32]
        cf.CFRelease.argtypes = [vp]
        cf.CFRelease.restype = None
        self.read_proc_type = ctypes.CFUNCTYPE(None, vp, vp, vp)
        cm.MIDIClientCreate.argtypes = [vp, vp, vp, ctypes.POINTER(u32)]
        cm.MIDIOutputPortCreate.argtypes = [u32, vp, ctypes.POINTER(u32)]
        cm.MIDIInputPortCreate.argtypes = [u32, vp, self.read_proc_type, vp, ctypes.POINTER(u32)]
        cm.MIDIPortConnectSource.argtypes = [u32, u32, vp]
        cm.MIDIPortDisconnectSource.argtypes = [u32, u32]
        cm.MIDIPortDispose.argtypes = [u32]
        cm.MIDIGetNumberOfSources.restype = ctypes.c_ulong
        cm.MIDIGetNumberOfDestinations.restype = ctypes.c_ulong
        cm.MIDIGetSource.restype = u32
        cm.MIDIGetSource.argtypes = [ctypes.c_ulong]
        cm.MIDIGetDestination.restype = u32
        cm.MIDIGetDestination.argtypes = [ctypes.c_ulong]
        cm.MIDIObjectGetStringProperty.argtypes = [u32, vp, ctypes.POINTER(vp)]
        cm.MIDISend.argtypes = [u32, u32, vp]
        for name in ("MIDIClientCreate", "MIDIOutputPortCreate", "MIDIInputPortCreate",
                     "MIDIPortConnectSource", "MIDIPortDisconnectSource", "MIDIPortDispose",
                     "MIDIObjectGetStringProperty", "MIDISend"):
            getattr(cm, name).restype = ctypes.c_int32
        self.cm, self.cf = cm, cf
        self.lock = threading.Lock()
        self.received = collections.deque()
        self.client, self.out, self.inp = u32(), u32(), u32()
        self.source = self.destination = None
        self.proc = self.read_proc_type(self._read)  # kept alive as long as the port
        self._closed = False
        try:
            with self._string("cardenza-usb") as name:
                self._check(cm.MIDIClientCreate(name, None, None, ctypes.byref(self.client)), "MIDIClientCreate")
            # CoreMIDI initializes property constants when its first client is created.
            self.name_property = vp.in_dll(cm, "kMIDIPropertyDisplayName").value
            if not self.name_property:
                raise OSError("CoreMIDI display-name property is not initialized")
            with self._string("out") as name:
                self._check(cm.MIDIOutputPortCreate(self.client, name, ctypes.byref(self.out)), "MIDIOutputPortCreate")
            with self._string("in") as name:
                self._check(cm.MIDIInputPortCreate(self.client, name, self.proc, None, ctypes.byref(self.inp)), "MIDIInputPortCreate")
            self.destination = self._find(cm.MIDIGetNumberOfDestinations, cm.MIDIGetDestination)
            self.source = self._find(cm.MIDIGetNumberOfSources, cm.MIDIGetSource)
            if self.source is not None:
                self._check(cm.MIDIPortConnectSource(self.inp, self.source, None), "MIDIPortConnectSource")
        except BaseException:
            self.close()
            raise

    @staticmethod
    def _check(status, operation):
        if status:
            raise OSError("%s: OSStatus %s" % (operation, status))

    @contextlib.contextmanager
    def _string(self, text):
        value = self.cf.CFStringCreateWithCString(None, text.encode(), 0x08000100)
        if not value:
            raise OSError("CFStringCreateWithCString failed")
        try:
            yield value
        finally:
            self.cf.CFRelease(value)

    def close(self):
        if self._closed:
            return
        self._closed = True
        if self.inp.value:
            if self.source is not None:
                self.cm.MIDIPortDisconnectSource(self.inp, self.source)
            self.cm.MIDIPortDispose(self.inp)
            self.inp.value = 0
        if self.out.value:
            self.cm.MIDIPortDispose(self.out)
            self.out.value = 0
        # CoreMIDI has stopped using the callback; only now release its Python owner.
        self.proc = None
        # Clients are process-owned and automatically disposed at process exit. Apple
        # discourages disposing the last client before creating another in the same process.

    def __del__(self):
        try:
            self.close()
        except Exception:
            pass

    def name(self, endpoint):
        value = self.c.c_void_p()
        if self.cm.MIDIObjectGetStringProperty(endpoint, self.name_property, self.c.byref(value)) != 0 or not value.value:
            return ""
        buffer = self.c.create_string_buffer(256)
        self.cf.CFStringGetCString(value, buffer, 256, 0x08000100)
        self.cf.CFRelease(value)
        return buffer.value.decode(errors="replace")

    def _find(self, count, get):
        for index in range(count()):
            endpoint = get(index)
            if NAME in self.name(endpoint):
                return endpoint
        return None

    def _read(self, packet_list, _proc_ref, _source_ref):
        if self._closed or not packet_list:
            return
        # MIDIPacketList: UInt32 count, then packets of UInt64 time, UInt16 length, data, 4-byte aligned.
        address = packet_list
        count = self.c.c_uint32.from_address(address).value
        packet = address + 4
        for _ in range(count):
            length = self.c.c_uint16.from_address(packet + 8).value
            data = self.c.string_at(packet + 10, length)
            with self.lock:
                self.received.extend(data)
            packet = (packet + 10 + length + 3) & ~3

    def send(self, data):
        if self._closed or self.destination is None:
            return False
        data = bytes(data)
        packet_list = self.c.create_string_buffer(struct.pack("<IQH", 1, 0, len(data)) + data)
        return self.cm.MIDISend(self.out, self.destination, packet_list) == 0

    def take(self):
        with self.lock:
            data = bytes(self.received)
            self.received.clear()
        return data


def midi():
    if sys.platform != "darwin":
        return None, "MIDI here only on macOS (CoreMIDI)"
    try:
        port = CoreMidi()
    except (OSError, AttributeError, ValueError) as error:
        return None, "CoreMIDI: %s" % error
    if port.destination is None or port.source is None:
        port.close()
        return None, "no Cardenza MIDI port"
    return port, None


def sysex_command(code, seconds=1.5):
    port, error = midi()
    if port is None:
        return None, error
    try:
        port.take()
        if not port.send([0xF0, 0x23, code, 0xF7]):
            return None, "CoreMIDI send failed"
        end = time.monotonic() + seconds
        buffered = b""
        while time.monotonic() < end:
            buffered += port.take()
            start = buffered.find(bytes([0xF0, 0x24]))
            if start >= 0:
                stop = buffered.find(b"\xf7", start)
                if stop > start:
                    return buffered[start + 2:stop], None
            time.sleep(0.01)
        return None, "no SysEx answer"
    finally:
        port.close()


def guide():
    """The guide link (Applications/Synth/GrooveAppGuide.cpp) as the Learn page uses it, over SysEx
    F0 23 7F <text> F7: start, a lesson start, the state, a key from the page, the end."""
    port, error = midi()
    if port is None:
        raise SystemExit("usb: %s" % error)
    failures = []
    buffered = [b""]

    def check(name, ok, detail=""):
        print("%s %s %s" % ("ok  " if ok else "FAIL", name, detail))
        if not ok:
            failures.append(name)

    def exchange(text, seconds=0.8):
        port.send(bytes([0xF0, 0x23, 0x7F]) + text.encode("ascii") + b"\xf7")
        answers = []
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            buffered[0] += port.take()
            while True:
                data = buffered[0]
                start = data.find(bytes([0xF0, 0x24, 0x7F]))
                stop = data.find(b"\xf7", start) if start >= 0 else -1
                if stop < 0:
                    break
                answers.append(data[start + 3:stop].decode("ascii", "replace"))
                buffered[0] = data[stop + 1:]
            time.sleep(0.01)
        return answers

    def field(answers, name):
        states = [a[2:].split("n", 1)[0] for a in answers if a.startswith("GS")]
        for state in reversed(states):
            found = [int(v) for k, v in re.findall(r"([a-z])(\d+)", state) if k == name]
            if found:
                return found[0]
        return None

    try:
        port.take()
        hello = exchange("G?")
        check("guide link", any(a.startswith("G!1") for a in hello), " ".join(hello))
        started = exchange("GE", 3.0)
        check("guide starts", "GE1" in started, " ".join(a for a in started if not a.startswith("GS")))
        lesson = exchange("GLs2p0t1", 1.0)
        check("lesson start: SEQ, T1", field(lesson, "s") == 2 and field(lesson, "t") == 0, (lesson or [""])[-1])
        exchange("GH0d1b", 0.3)
        asked = exchange("GQ")
        check("state on request", field(asked, "s") == 2, (asked or [""])[-1])
        exchange("GK0e1", 0.3)
        moved = exchange("GK0e0", 1.0)
        check("TAB from the page: next track", field(moved, "t") == 1, (moved or [""])[-1])
        ended = exchange("GX", 3.0)
        check("guide ends, the working state is back", "GX" in ended, " ".join(a for a in ended if not a.startswith("GS")))
    finally:
        port.close()
    print("failures: %d" % len(failures))
    return 1 if failures else 0


def command(name):
    code = COMMANDS[name]
    device, error = open_hid()
    if device is not None:
        send(device, REPORT_SYSTEM, [code])
        answer = receive(device, REPORT_SYSTEM, 1.5)
        device.close()
        if answer is not None:
            return "hid", answer
        error = "no HID answer"
    answer, sysex_error = sysex_command(code)
    if answer is not None:
        return "sysex", answer
    raise SystemExit("usb: %s; %s" % (error, sysex_error))


def stats(device=None):
    own = device is None
    if own:
        device, error = open_hid()
        if device is None:
            raise SystemExit("usb: " + error)
    send(device, REPORT_APP, [ord("S")])
    answer = receive(device, REPORT_APP, 1.5)
    if own:
        device.close()
    return answer.rstrip(b"\0").decode(errors="replace") if answer else None


def console(seconds):
    device, error = open_hid()
    if device is None:
        raise SystemExit("usb: " + error)
    send(device, REPORT_SYSTEM, [COMMANDS["console"], 1])
    end = time.time() + seconds
    try:
        while time.time() < end:
            data = device.read(REPORT_SIZE + 1, 100)
            if data and data[0] == REPORT_SYSTEM and data[1] == (COMMANDS["console"] | 0x80):
                sys.stdout.write(bytes(data[3:3 + data[2]]).decode(errors="replace"))
                sys.stdout.flush()
    finally:
        send(device, REPORT_SYSTEM, [COMMANDS["console"], 0])
        device.close()


def list_all():
    try:
        for d in hid_devices():
            print("hid: %04x:%04x %s %s serial=%s interface=%s" % (d["vendor_id"], d["product_id"], d.get("manufacturer_string"), d.get("product_string"), d.get("serial_number"), d.get("interface_number")))
    except ImportError:
        print("hid: hidapi not installed (pip install hidapi)")
    port, error = midi()
    try:
        print("midi: %s" % ("out=%s in=%s" % (port.name(port.destination), port.name(port.source)) if port else error))
    finally:
        if port:
            port.close()
    try:
        import sounddevice
        for index, d in enumerate(sounddevice.query_devices()):
            if NAME in d["name"]:
                print("audio: #%d %s in=%d out=%d rate=%d" % (index, d["name"], d["max_input_channels"], d["max_output_channels"], d["default_samplerate"]))
    except ImportError:
        print("audio: sounddevice not installed (pip install sounddevice numpy)")


def audio_device(direction):
    import sounddevice
    key = "max_input_channels" if direction == "in" else "max_output_channels"
    for index, d in enumerate(sounddevice.query_devices()):
        if NAME in d["name"] and d[key] >= 2:
            return index
    return None


STEP = "starting"


def step(name):
    """Says what runs now, so a watchdog timeout can tell where it stopped."""
    global STEP
    STEP = name
    print("...  " + name, flush=True)


def watchdog(seconds):
    """Ends the process when an action runs too long, e.g. a USB audio
    stream that never starts, wherever it blocks."""
    def expire():
        print("usb: timed out after %d s during: %s" % (seconds, STEP), flush=True)
        os._exit(124)
    timer = threading.Timer(seconds, expire)
    timer.daemon = True
    timer.start()


def parse_stats(text):
    """Keep complete counters when the fixed HID report truncates a trailing field."""
    values = {}
    for item in (text or "").split():
        name, separator, value = item.partition("=")
        if separator:
            try:
                values[name] = int(value, 10)
            except ValueError:
                pass
    return values


def test():
    import numpy
    import sounddevice
    failures = []

    def check(name, ok, detail=""):
        print("%s %s %s" % ("ok  " if ok else "FAIL", name, detail))
        if not ok:
            failures.append(name)

    step("identify over HID")
    transport, answer = command("identify")
    check("identify (%s)" % transport, bool(answer) and answer[0] == 0xC4, answer[2:].rstrip(b"\0").decode(errors="replace") if answer else "")
    step("identify over SysEx")
    answer, error = sysex_command(COMMANDS["identify"])
    check("identify (sysex)", answer is not None and answer[0] == (0xC4 & 0x7F), error or "")

    step("HID statistics")
    device, error = open_hid()
    check("hid open", device is not None, error or "")
    before = stats(device) if device else None
    print("     " + str(before))

    # MIDI in: notes on channel 1 reach the firmware (and it plays them on track 1).
    step("MIDI ports")
    port, error = midi()
    check("midi ports", port is not None, error or "")
    # Audio to the computer: record while the notes play.
    recorded = None
    in_device = audio_device("in")
    check("audio input device", in_device is not None)
    recording_started = time.time()
    if in_device is not None:
        step("recording from the device")
        recording = sounddevice.rec(int(2.0 * 44100), samplerate=44100, channels=2, dtype="int16", device=in_device)
        time.sleep(0.3)
    try:
        if port is not None:
            step("notes over MIDI")
            for note in (60, 64, 67):
                port.send([0x90, note, 100])
            time.sleep(1.0)
            for note in (60, 64, 67):
                port.send([0x80, note, 0])
    finally:
        if port:
            port.close()
    if in_device is not None:
        # A fixed time, never sounddevice.wait(): a stream that delivers nothing would wait for ever.
        time.sleep(max(0.0, recording_started + 2.2 - time.time()))
        sounddevice.stop()
        recorded = numpy.asarray(recording, dtype=numpy.float64)
        rms = float(numpy.sqrt(numpy.mean(recorded ** 2))) if recorded.size else 0.0
        check("audio from the device", rms > 50.0, "rms=%.0f peak=%d" % (rms, int(numpy.abs(recorded).max()) if recorded.size else 0))

    # Audio from the computer: a 1 kHz tone at -12 dBFS for 2 s.
    out_device = audio_device("out")
    check("audio output device", out_device is not None)
    if out_device is not None:
        step("playing to the device")
        t = numpy.arange(int(2.0 * 44100)) / 44100.0
        tone = (0.25 * 32767 * numpy.sin(2 * numpy.pi * 1000.0 * t)).astype(numpy.int16)
        sounddevice.play(numpy.stack([tone, tone], axis=1), samplerate=44100, device=out_device)
        time.sleep(2.2)
        sounddevice.stop()

    step("HID statistics again")
    after = stats(device) if device else None
    print("     " + str(after))
    if before and after:
        b, a = parse_stats(before), parse_stats(after)
        required = {"midi_in", "to_pc", "from_pc", "peak"}
        missing = (required - b.keys()) | (required - a.keys())
        check("USB statistics fields", not missing, "missing: " + ", ".join(sorted(missing)) if missing else "")
        if not missing:
            check("midi into the firmware", a["midi_in"] - b["midi_in"] >= 6, "%s -> %s" % (b["midi_in"], a["midi_in"]))
            check("frames to the computer", a["to_pc"] > b["to_pc"], "%s -> %s" % (b["to_pc"], a["to_pc"]))
            check("frames from the computer", a["from_pc"] - b["from_pc"] > 44100, "%s -> %s peak=%s" % (b["from_pc"], a["from_pc"], a["peak"]))
            check("level from the computer", 6000 < a["peak"] < 10000, "peak=%s (expect about 8192)" % a["peak"])
    if device:
        device.close()
    print("failures: %d" % len(failures))
    return 1 if failures else 0


def main():
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    action = sys.argv[1]
    limits = {"list": 30, "command": 20, "stats": 15, "test": 90, "guide": 30}
    watchdog(limits.get(action, 0) or (float(sys.argv[2]) + 15 if action == "console" and len(sys.argv) == 3 else 30))
    if action == "list":
        list_all()
    elif action == "command" and len(sys.argv) == 3 and sys.argv[2] in COMMANDS:
        transport, answer = command(sys.argv[2])
        print("%s answer: %s" % (transport, answer.rstrip(b"\0").hex()))
    elif action == "stats":
        print(stats())
    elif action == "console" and len(sys.argv) == 3:
        console(float(sys.argv[2]))
    elif action == "test":
        raise SystemExit(test())
    elif action == "guide":
        raise SystemExit(guide())
    else:
        raise SystemExit(__doc__)


if __name__ == "__main__":
    main()
