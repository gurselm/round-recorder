#!/usr/bin/env python3
"""Downloads the round recorder's saved recordings over USB as WAV files.

    python3 tools/pull.py                  # new recordings into ./recordings; sets the board's clock
    python3 tools/pull.py --list           # what's on the board
    python3 tools/pull.py --delete         # and delete each from the board once it's saved here
    python3 tools/pull.py --name 4 "Grocery list"   # name recording 4 (--list shows the numbers)
    python3 tools/pull.py --protect 4      # never replace recording 4 when the board is full (--unprotect undoes it)
    python3 tools/pull.py --transcribe     # and a .txt transcript beside each WAV
    python3 tools/pull.py --battery        # how long the battery lasted since it last came off USB
    python3 tools/pull.py --port /dev/ttyACM1

A recording longer than the board holds at once comes in parts; they're
joined into one WAV. One still being recorded is left for the next run. A
recording's name goes into its file name; without one, a transcript's first
words do. Renaming a recording already downloaded renames its files on the
next run, rather than downloading it again.

--transcribe runs Whisper on this computer: RR_TRANSCRIBE_CMD if set (a
command with {wav} in it, printing the text), else faster-whisper if Python
can import it (model RR_WHISPER_MODEL, "base" by default), else whisper.cpp's
whisper-cli with the model in WHISPER_CPP_MODEL. RR_WHISPER_LANG (en, tr, ...)
sets the language instead of letting Whisper guess it from the first 30 s.

Without --port it uses the only USB serial port there is. With several boards
plugged in, pass the recorder's (ls /dev/serial/by-id/ shows each one's MAC).
Only the standard library: Linux or macOS.
"""
import argparse
import array
import base64
import glob
import os
import re
import select
import shlex
import shutil
import struct
import subprocess
import sys
import termios
import time
import wave
import zlib

RATE = 16000
BLOCK_SAMPLES = 320
BLOCK_BYTES = 4 + BLOCK_SAMPLES // 2

STEPS = [
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88,
    97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658,
    724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660,
    4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818,
    18500, 20350, 22385, 24623, 27086, 29794, 32767,
]
INDEX_STEP = [-1, -1, -1, -1, 2, 4, 6, 8]


def decode(data):
    """The board's ADPCM blocks (main/rec_ring.h) to 16-bit PCM samples."""
    out = array.array("h")
    for off in range(0, len(data) - BLOCK_BYTES + 1, BLOCK_BYTES):
        pred = struct.unpack_from("<h", data, off)[0]
        index = min(data[off + 2], 88)
        for byte in data[off + 4:off + BLOCK_BYTES]:
            for code in (byte & 15, byte >> 4):
                st = STEPS[index]
                diff = st >> 3
                if code & 4:
                    diff += st
                if code & 2:
                    diff += st >> 1
                if code & 1:
                    diff += st >> 2
                pred = max(-32768, min(32767, pred - diff if code & 8 else pred + diff))
                index = max(0, min(88, index + INDEX_STEP[code & 7]))
                out.append(pred)
    return out


def write_wav(path, samples):
    if sys.byteorder == "big":
        samples.byteswap()
    tmp = path + ".part"
    with wave.open(tmp, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(samples.tobytes())
    os.replace(tmp, path)


class Board:
    def __init__(self, path):
        self.fd = os.open(path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
        a = termios.tcgetattr(self.fd)
        a[0] = 0                       # iflag: raw
        a[1] = 0                       # oflag
        # No HUPCL: closing mustn't drop DTR/RTS, which can reset the chip.
        a[2] = (a[2] & ~(termios.HUPCL | termios.CSIZE | termios.PARENB)) | termios.CS8 | termios.CREAD | termios.CLOCAL
        a[3] = 0                       # lflag: no echo, no line editing
        a[4] = a[5] = termios.B115200  # ignored by USB, but must be valid
        a[6][termios.VMIN] = 0
        a[6][termios.VTIME] = 0
        termios.tcsetattr(self.fd, termios.TCSANOW, a)
        termios.tcflush(self.fd, termios.TCIFLUSH)
        self.buf = b""

    def close(self):
        os.close(self.fd)

    def send(self, line):
        data = b"\n" + line.encode() + b"\n"
        while data:
            select.select([], [self.fd], [], 2)
            data = data[os.write(self.fd, data):]

    def line(self, timeout):
        """The next "RR ..." line, without the "RR "; log lines in between are skipped."""
        deadline = time.monotonic() + timeout
        while True:
            while b"\n" in self.buf:
                raw, self.buf = self.buf.split(b"\n", 1)
                raw = raw.strip(b"\r")
                if raw.startswith(b"RR "):
                    return raw[3:].decode("ascii", "replace")
            left = deadline - time.monotonic()
            if left <= 0:
                raise TimeoutError("the board didn't answer")
            if select.select([self.fd], [], [], left)[0]:
                try:
                    self.buf += os.read(self.fd, 65536)
                except BlockingIOError:
                    pass

    def ask(self, line, *expect, timeout=3.0):
        """Sends a command and returns the first reply starting with one of expect
        (or ERR, which raises). Leftovers from an interrupted run are skipped."""
        self.send(line)
        while True:
            reply = self.line(timeout)
            if reply.startswith("ERR"):
                raise RuntimeError(f"{line}: {reply[4:]}")
            if reply.startswith(expect):
                return reply

    def get(self, slot):
        self.send(f"RR GET {slot}")
        return self.receive(f"slot {slot}")

    def receive(self, what):
        """DATA lines up to their END: (data, size, crc) as the board says them."""
        data = bytearray()
        while True:
            reply = self.line(5.0)
            if reply.startswith("DATA "):
                data += base64.b64decode(reply[5:])
            elif reply.startswith("END "):
                size, crc = reply[4:].split()
                return bytes(data), int(size), int(crc, 16)
            elif reply.startswith("ERR"):
                raise RuntimeError(f"{what}: {reply[4:]}")


def fields(reply):
    # The name, which may have spaces, runs from " name=" to the end.
    reply, _, name = reply.partition(" name=")
    f = dict(w.split("=", 1) for w in reply.split() if "=" in w)
    f["name"] = name
    return f


def find_port():
    ports = sorted(glob.glob("/dev/ttyACM*") + glob.glob("/dev/cu.usbmodem*"))
    if len(ports) == 1:
        return ports[0]
    if not ports:
        sys.exit("No board found: plug the recorder in with a data cable, or pass --port.")
    named = sorted(glob.glob("/dev/serial/by-id/*"))
    hint = "\n  ".join(f"{n} -> {os.path.realpath(n)}" for n in named) or ", ".join(ports)
    sys.exit(f"Several serial ports; pass the recorder's with --port:\n  {hint}")


def recordings(clips):
    """Clips (as the board lists them) as recordings, oldest first, each with its
    parts in order. A recording is numbered, named and timed by its first part,
    or its earliest one left if that's gone."""
    by_first = {}
    for c in clips:
        by_first.setdefault(c["first"], []).append(c)
    recs = []
    for first, parts in by_first.items():
        parts.sort(key=lambda c: int(c["seq"]))
        head = parts[0]
        whole = head["seq"] == first
        recs.append({"seq": first, "parts": parts, "whole": whole, "time": head["time"], "crc": head["crc"],
                     "name": head["name"] if whole else "", "kept": whole and head.get("kept") == "1",
                     "pulled": whole and head.get("pulled") == "1",
                     "blocks": sum(int(c["blocks"]) for c in parts)})
    recs.sort(key=lambda r: int(r["seq"]))
    return recs


def clip_base(rec):
    """The part of the file name that identifies the recording, whatever its name."""
    t = int(rec["time"])
    if t:
        return time.strftime("%Y-%m-%d_%H-%M-%S", time.localtime(t)) + f"_clip{rec['seq']}"
    # The board's clock wasn't set when it was saved: the CRC tells it from a
    # recording with the same number from before the board was wiped.
    return f"clip{rec['seq']}_{int(rec['crc'], 16):08x}"


def safe(text, words=None):
    if words:
        text = " ".join(re.findall(r"\w+", text)[:words])
    return re.sub(r"[^\w-]+", "-", text).strip("-")[:40]


def clip_file(rec, transcript=""):
    tag = safe(rec["name"]) or safe(transcript, words=6)
    return clip_base(rec) + (f"_{tag}" if tag else "") + ".wav"


def downloaded(folder, rec):
    """The file this recording was downloaded to before, under any name, or None."""
    base = clip_base(rec)
    for path in [os.path.join(folder, base + ".wav")] + sorted(glob.glob(os.path.join(glob.escape(folder), glob.escape(base) + "_*.wav"))):
        if os.path.exists(path):
            return path
    return None


def transcriber():
    """A function from a WAV's path to its text, from what this computer has."""
    cmd = os.environ.get("RR_TRANSCRIBE_CMD")
    if cmd:
        def run(path):
            args = [a.replace("{wav}", path) for a in shlex.split(cmd)]
            return subprocess.run(args, check=True, capture_output=True, text=True).stdout.strip()
        return run
    try:
        from faster_whisper import WhisperModel
    except ImportError:
        WhisperModel = None
    if WhisperModel:
        import numpy
        model = WhisperModel(os.environ.get("RR_WHISPER_MODEL", "base"), device="cpu", compute_type="int8")

        def run(path):
            # Already 16 kHz mono, as Whisper takes it: no decoder needed.
            with wave.open(path) as w:
                pcm = numpy.frombuffer(w.readframes(w.getnframes()), dtype="<i2")
            # Not conditioned on its own last words, which can loop it on one phrase.
            segments, _ = model.transcribe(pcm.astype(numpy.float32) / 32768, vad_filter=True,
                                           condition_on_previous_text=False,
                                           language=os.environ.get("RR_WHISPER_LANG") or None)
            return " ".join(s.text.strip() for s in segments)
        return run
    cli, model_path = shutil.which("whisper-cli"), os.environ.get("WHISPER_CPP_MODEL")
    if cli and model_path:
        def run(path):
            return subprocess.run([cli, "-m", model_path, "-f", path, "-nt", "-np"], check=True,
                                  capture_output=True, text=True).stdout.strip()
        return run
    sys.exit("--transcribe needs Whisper: in a venv, pip install faster-whisper and run pull.py with its "
             "python, or set RR_TRANSCRIBE_CMD or WHISPER_CPP_MODEL (see pull.py --help).")


def move(src, dst):
    """Renames a WAV, and its transcript with it."""
    if src == dst:
        return
    os.replace(src, dst)
    if os.path.exists(src[:-4] + ".txt"):
        os.replace(src[:-4] + ".txt", dst[:-4] + ".txt")
    print(f"  {src} -> {os.path.basename(dst)}")


def read_text(wav):
    try:
        with open(wav[:-4] + ".txt", encoding="utf-8") as f:
            return f.read()
    except FileNotFoundError:
        return ""


BATT_USB, BATT_CHARGING, BATT_SCREEN, BATT_RECORDING, BATT_OFF, BATT_BOOT = 1, 2, 4, 8, 16, 32


def battery(board, csv_path=None):
    """The board's battery log (main/batt_log.h): an entry a minute from when
    it last came off USB. Prints how it went and, from the drain, how long a
    full battery lasts; with csv_path, writes the entries there too."""
    board.send("RR BATT")
    rows = []
    while True:
        reply = board.line(5.0)
        if reply.startswith("B "):
            rows.append(tuple(int(v) for v in reply[2:].split()))
        elif reply.startswith("END"):
            break
        elif reply.startswith("ERR"):
            raise RuntimeError(f"battery log: {reply[4:]}")
    if csv_path:
        with open(csv_path, "w") as f:
            f.write("minute,mV,percent,screen,recording,usb,off,boot\n")
            for m, mv, pct, fl in rows:
                f.write(f"{m},{mv},{pct},{int(bool(fl & BATT_SCREEN))},{int(bool(fl & BATT_RECORDING))},"
                        f"{int(bool(fl & BATT_USB))},{int(bool(fl & BATT_OFF))},{int(bool(fl & BATT_BOOT))}\n")
        print(f"{len(rows)} entries written to {csv_path}")
    if not rows:
        print("No battery log yet: it starts when the board comes off USB.")
        return
    # The entries on battery: up to USB coming back, if it did.
    on_batt = [r for r in rows if not r[3] & BATT_USB]
    first, last = on_batt[0] if on_batt else rows[0], (on_batt or rows)[-1]
    minutes = last[0] - first[0]
    end = ("it powered itself off" if last[3] & BATT_OFF else
           "back on USB" if rows[-1][3] & BATT_USB else "still on battery")
    screen = sum(1 for r in on_batt if r[3] & BATT_SCREEN)
    recording = sum(1 for r in on_batt if r[3] & BATT_RECORDING)
    boots = sum(1 for r in on_batt if r[3] & BATT_BOOT)
    print(f"On battery {minutes // 60} h {minutes % 60:02d} min, {end}.")
    print(f"  {first[2]}% ({first[1]} mV) to {last[2]}% ({last[1]} mV)")
    n = max(len(on_batt), 1)
    print(f"  screen on in {screen} of {len(on_batt)} minutes ({100 * screen // n}%), "
          f"recording in {recording}" + (f", turned on again {boots}x" if boots else ""))
    fallen = first[2] - last[2]
    if minutes >= 30 and fallen >= 3:
        per_hour = fallen / (minutes / 60)
        print(f"  drain {per_hour:.1f}% an hour: a full battery lasts about {100 / per_hour:.0f} h used like this")
    else:
        print("  too short to judge the drain yet (half an hour and 3% at least)")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--port", default=os.environ.get("RR_PORT"))
    ap.add_argument("--out", default="recordings", help="folder for the WAV files (default: ./recordings)")
    ap.add_argument("--list", action="store_true", help="only list the recordings on the board")
    ap.add_argument("--delete", action="store_true",
                    help="delete each recording from the board once it's saved here, except protected ones")
    ap.add_argument("--name", nargs=2, metavar=("CLIP", "NAME"), help='name a recording, by its number; "" clears it')
    ap.add_argument("--protect", "--keep", dest="keep", metavar="CLIP",
                    help="never replace this recording when the board is full")
    ap.add_argument("--unprotect", "--unkeep", dest="unkeep", metavar="CLIP", help="let a save replace it again")
    ap.add_argument("--transcribe", action="store_true", help="write a .txt transcript beside each WAV")
    ap.add_argument("--battery", action="store_true",
                    help="only show the battery log: how long the battery lasted since it last came off USB")
    ap.add_argument("--battery-csv", metavar="FILE", help="with --battery, also write the log's entries to FILE")
    args = ap.parse_args()
    only_board = args.list or args.name or args.keep or args.unkeep or args.battery
    transcribe = transcriber() if args.transcribe and not only_board else None

    port = args.port or find_port()
    try:
        board = Board(port)
    except OSError as e:
        sys.exit(f"Can't open {port}: {e}")
    try:
        hello = board.ask("RR HELLO", "HELLO")
        if not hello.startswith("HELLO round-recorder"):
            sys.exit(f"{port} isn't a round recorder: {hello}")
        info = fields(hello)
        going = info.get("recording", "0")
        now = time.time()
        offset = -(time.altzone if time.localtime(now).tm_isdst > 0 else time.timezone)
        board.ask(f"RR TIME {int(now)} {offset}", "OK")
        if args.battery:
            battery(board, args.battery_csv)
            return

        board.send("RR LIST")
        clips = []
        while True:
            reply = board.line(3.0)
            if reply.startswith("CLIP "):
                clips.append(fields(reply))
            elif reply.startswith("END"):
                break
            elif reply.startswith("ERR"):
                raise RuntimeError(reply)
        recs = recordings(clips)

        if args.name:
            seq, name = args.name
            rec = next((r for r in recs if r["seq"] == seq), None)
            if not rec:
                raise RuntimeError(f"no recording {seq} on the board (--list shows them)")
            if not rec["whole"]:
                raise RuntimeError(f"recording {seq}'s first part is gone, and its name with it")
            board.ask(f"RR NAME {rec['parts'][0]['slot']} {seq} {name}", "OK")
            print(f"recording {seq} named {name!r}" if name else f"recording {seq}'s name cleared")
            return 0
        if args.keep or args.unkeep:
            seq = args.keep or args.unkeep
            rec = next((r for r in recs if r["seq"] == seq), None)
            if not rec:
                raise RuntimeError(f"no recording {seq} on the board (--list shows them)")
            if not rec["whole"]:
                raise RuntimeError(f"recording {seq}'s first part is gone, which is what protects it")
            board.ask(f"RR KEEP {rec['parts'][0]['slot']} {seq} {1 if args.keep else 0}", "OK")
            print(f"recording {seq} protected" if args.keep else f"recording {seq} no longer protected")
            return 0

        print(f"{port}: {len(recs)} recording{'' if len(recs) == 1 else 's'} in {len(clips)} of {info['slots']} "
              f"slots, up to {info['seconds']} s a part; clock set")
        if args.list:
            for r in recs:
                when = time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(int(r["time"]))) if int(r["time"]) else "clock not set"
                parts = f"{len(r['parts'])} parts" if len(r["parts"]) > 1 else ""
                note = "recording now" if r["seq"] == going else r["name"]
                marks = " ".join(m for m, on in (("protected", r["kept"]), ("downloaded", r["pulled"])) if on)
                print(f"  clip {r['seq']:>4}  {r['blocks'] / 50:6.1f} s  {parts:8}  {when:19}  {marks:20}  {note}".rstrip())
            return 0

        os.makedirs(args.out, exist_ok=True)
        got = kept = deleted = 0
        for r in recs:
            if r["seq"] == going:
                print(f"  clip {r['seq']}: still recording, left for next time")
                continue
            path = downloaded(args.out, r)
            if path:
                kept += 1
            else:
                started = time.monotonic()
                data = b""
                for c in r["parts"]:
                    part, size, crc = board.get(c["slot"])
                    if len(part) != size or size != int(c["blocks"]) * BLOCK_BYTES or zlib.crc32(part) != crc \
                            or crc != int(c["crc"], 16):
                        raise RuntimeError(f"clip {c['seq']} arrived damaged; nothing deleted, run again")
                    data += part
                path = os.path.join(args.out, clip_file(r))
                write_wav(path, decode(data))
                got += 1
                print(f"  {path}  ({len(data) / 50 / BLOCK_BYTES:.1f} s, {time.monotonic() - started:.1f} s to fetch)")
            if transcribe and not os.path.exists(path[:-4] + ".txt"):
                text = transcribe(path)
                with open(path[:-4] + ".txt.part", "w", encoding="utf-8") as f:
                    f.write(text + "\n")
                os.replace(path[:-4] + ".txt.part", path[:-4] + ".txt")
                print(f"  {os.path.basename(path)[:-4]}.txt: {text[:70]}{'...' if len(text) > 70 else ''}")
            move(path, os.path.join(args.out, clip_file(r, read_text(path))))
            if r["whole"] and not r["pulled"]:
                # Shown on the board: what's safely here. Someone at the board
                # may have deleted it meanwhile: that's no reason to stop.
                try:
                    board.ask(f"RR PULLED {r['parts'][0]['slot']} {r['seq']}", "OK")
                except RuntimeError:
                    print(f"  clip {r['seq']}: gone from the board before it could be marked downloaded")
                    continue
            if args.delete and r["kept"]:
                print(f"  clip {r['seq']}: protected, so left on the board (--unprotect {r['seq']} first to delete it)")
            elif args.delete:
                for c in r["parts"]:
                    board.ask(f"RR DEL {c['slot']}", "OK")
                deleted += 1
        print(f"{got} new, {kept} already here" + (f", {deleted} deleted from the board" if args.delete else ""))
        return 0
    except (RuntimeError, TimeoutError, subprocess.CalledProcessError) as e:
        print(f"Error: {e}", file=sys.stderr)
        return 1
    finally:
        board.close()


if __name__ == "__main__":
    sys.exit(main())
