"""Host tests for the IDF-free core and tools/pull.py, with no board.

    python3 -m unittest discover -s tests

Compiles main/rec_*.c, main/adpcm.c, main/tempo.c, main/multitap.c and main/batt_est.c with tests/harness.c (set CC to
change compilers). pull.py talks to the harness's fake board over a pty.
"""
import array
import math
import os
import pty
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
import wave
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import pull  # noqa: E402

BLOCK = pull.BLOCK_BYTES


def setUpModule():
    global TMP, HARNESS
    os.environ["ASAN_OPTIONS"] = "detect_leaks=0"   # the harness exits without freeing
    TMP = tempfile.mkdtemp(prefix="rr-test-")
    HARNESS = os.path.join(TMP, "harness")
    main = os.path.join(ROOT, "main")
    srcs = [os.path.join(ROOT, "tests", "harness.c")] + [
        os.path.join(main, f) for f in ("rec_ring.c", "rec_store.c", "rec_proto.c", "adpcm.c", "tempo.c", "multitap.c", "batt_est.c")]
    subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-D_DEFAULT_SOURCE", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", "-g", "-I", main, *srcs, "-lm", "-o", HARNESS], check=True)


def tearDownModule():
    shutil.rmtree(TMP, ignore_errors=True)


def harness(*args):
    return subprocess.run([HARNESS, *map(str, args)], check=True, capture_output=True, text=True).stdout


def sine(n, hz, amp=8000):
    return array.array("h", (int(amp * math.sin(2 * math.pi * hz * i / pull.RATE)) for i in range(n)))


def write(path, data):
    with open(path, "wb") as f:
        f.write(data)


def read(path):
    with open(path, "rb") as f:
        return f.read()


class BatteryEstimateTest(unittest.TestCase):
    def hours(self, start, pct, since):
        return float(harness("hours", start, pct, since))

    def test_matches_the_measured_discharge(self):
        # Points from the discharge the table came from: 5 h 57 min to the
        # power-off, with the percent falling slowly at the top. Within 0.5 h:
        # the table has a point every 5 %, and the percent falls in uneven steps.
        self.assertAlmostEqual(self.hours(100, 82, 2.0), 3.97, delta=0.5)
        self.assertAlmostEqual(self.hours(100, 51, 4.5), 1.47, delta=0.1)
        self.assertAlmostEqual(self.hours(100, 29, 5.0), 0.98, delta=0.1)

    def test_a_heavier_load_shortens_it(self):
        # The same fall in half the time: half the hours left.
        self.assertAlmostEqual(self.hours(100, 82, 1.0), self.hours(100, 82, 2.0) / 2, places=2)

    def test_from_part_charged(self):
        # Unplugged at 60 %, at the measured pace: what that discharge had left.
        self.assertAlmostEqual(self.hours(60, 51, (268.2 - 233) / 60), (357 - 268.2) / 60, delta=0.05)

    def test_nothing_to_go_on(self):
        self.assertLess(self.hours(80, 80, 1.0), 0)
        self.assertLess(self.hours(80, 85, 1.0), 0)
        self.assertEqual(self.hours(100, 3, 6.0), 0)


class RingTest(unittest.TestCase):
    def ring(self, pcm, cap, chunk):
        src, out = os.path.join(TMP, "in.raw"), os.path.join(TMP, f"ring-{cap}-{chunk}.blocks")
        write(src, pcm.tobytes())
        harness("ring", cap, chunk, src, out)
        return read(out)

    def test_read_size_doesnt_matter(self):
        pcm = sine(320 * 37 + 100, 523)
        whole = self.ring(pcm, 50, 320)
        self.assertEqual(len(whole), 37 * BLOCK)
        for chunk in (1, 97, 1000, 320 * 40):
            self.assertEqual(self.ring(pcm, 50, chunk), whole, chunk)

    def test_keeps_the_newest_and_decodes_alone(self):
        # 80 blocks into a ring of 30: the last 30 come out, and decode to the
        # input's last 30 blocks without the 50 before them.
        pcm = sine(320 * 80, 700)
        blocks = self.ring(pcm, 30, 160)
        self.assertEqual(len(blocks), 30 * BLOCK)
        out = pull.decode(blocks)
        tail = pcm[-30 * 320:]
        err = sum((a - b) ** 2 for a, b in zip(out, tail)) / len(tail)
        sig = sum(a * a for a in tail) / len(tail)
        self.assertGreater(10 * math.log10(sig / err), 20)   # ADPCM, not noise

    def test_python_decoder_matches_the_c_one(self):
        pcm = array.array("h", ((i * 7919) % 65536 - 32768 for i in range(320 * 20)))   # harsh: big jumps
        pcm += sine(320 * 20, 3000, 30000)
        blocks = self.ring(pcm, 64, 320)
        src, out = os.path.join(TMP, "dec.blocks"), os.path.join(TMP, "dec.raw")
        write(src, blocks)
        harness("decode", src, out)
        self.assertEqual(pull.decode(blocks).tobytes(), read(out))

    def test_save_copies_in_pieces_while_recording(self):
        self.assertEqual(harness("span").strip(), "span ok")

    def test_adpcm_format_is_unchanged(self):
        # The codes for a fixed mix of sounds, and the samples decoded from
        # them, as the board has always made them: saved recordings and
        # pull.py's decoder depend on it.
        self.assertEqual(harness("adpcm").split(), ["505067ed", "63e77a41"])

    def test_crc_is_zlibs(self):
        data = bytes(range(256)) * 41 + b"x"
        path = os.path.join(TMP, "crc.bin")
        write(path, data)
        self.assertEqual(int(harness("crc", path), 16), zlib.crc32(data))


class TempoTest(unittest.TestCase):
    """Faster playback: shorter, same pitch, same loudness."""

    def run_tempo(self, pct, samples):
        src, out = os.path.join(TMP, f"tempo-in-{pct}.raw"), os.path.join(TMP, f"tempo-out-{pct}.raw")
        with open(src, "wb") as f:
            f.write(array.array("h", samples).tobytes())
        harness("tempo", pct, src, out)
        return array.array("h", read(out))

    def test_speeds(self):
        rate, secs = 16000, 3.0
        # A tone whose loudness swells, like a voice: the frames have to line up.
        tone = [int(9000 * (0.6 + 0.4 * math.sin(2 * math.pi * 1.5 * i / rate)) *
                    math.sin(2 * math.pi * 440 * i / rate)) for i in range(int(rate * secs))]
        self.assertEqual(list(self.run_tempo(100, tone)), tone)
        for pct in (150, 200):
            out = self.run_tempo(pct, tone)
            self.assertAlmostEqual(len(out) / rate, secs * 100 / pct, delta=0.06, msg=pct)
            mid = out[len(out) // 4: 3 * len(out) // 4]
            crossings = sum(1 for a, b in zip(mid, mid[1:]) if (a < 0) != (b < 0))
            self.assertAlmostEqual(crossings / 2 / (len(mid) / rate), 440, delta=440 * 0.02, msg=pct)
            rms_in = math.sqrt(sum(v * v for v in tone) / len(tone))
            rms_out = math.sqrt(sum(v * v for v in mid) / len(mid))
            self.assertAlmostEqual(20 * math.log10(rms_out / rms_in), 0, delta=1.0, msg=pct)


class MultitapTest(unittest.TestCase):
    """The naming keypad: presses at times in ms, and what they type."""

    def typed(self, *presses):
        return harness("multitap", *(p if isinstance(p, str) else f"{p[0]}@{p[1]}" for p in presses)).strip()[1:-1]

    def test_typing(self):
        # "Hi": 4 twice is H; after a pause, 4 three times is i.
        self.assertEqual(self.typed((4, 0), (4, 300), (4, 2000), (4, 2300), (4, 2600)), "Hi")
        # Round the key's letters and its digit, back to the start.
        self.assertEqual(self.typed(*[(2, i * 200) for i in range(5)]), "A")
        self.assertEqual(self.typed(*[(2, i * 200) for i in range(4)]), "2")
        # A pause, or another key, keeps the letter; only the first is a capital.
        self.assertEqual(self.typed((2, 0), (2, 1000)), "Aa")
        self.assertEqual(self.typed((2, 0), (3, 100), (0, 200), (8, 300)), "Ad t")
        # Space is 0 then the digit; punctuation on 1.
        self.assertEqual(self.typed((2, 0), (0, 1000), (0, 1200)), "A0")
        self.assertEqual(self.typed((7, 0), (1, 1000), (1, 1200), (1, 1400)), "P?")

    def test_numbers_and_symbols(self):
        # Numbers: one press each, even pressed again quickly, 1 on the top
        # key (abc's), round to 0 on the last (punctuation's).
        self.assertEqual(self.typed("mode=1", (2, 0), (2, 100), (0, 200), (1, 300)), "1190")
        # Symbols, then back to letters, which start a new character.
        self.assertEqual(self.typed((2, 0), "mode=2", (1, 100), (1, 150), "mode=0", (2, 200)), "A&&a")
        # A name, letters, space, number: "Call 2".
        self.assertEqual(self.typed((2, 0), (2, 200), (2, 400), (2, 1500), (5, 2600), (5, 2700), (5, 2800),
                                    (5, 4000), (5, 4100), (5, 4200), (0, 5500), "mode=1", (3, 6000)), "Call 2")


class StoreTest(unittest.TestCase):
    def test_store(self):
        self.assertEqual(harness("store").strip(), "store ok")


class PullTest(unittest.TestCase):
    """pull.py against the fake board: clips 1 and 2 with a time (2 named
    "Grocery list"), 3 without, and recording 4 in two parts, clips 4 and 5."""

    def setUp(self):
        self.dir = tempfile.mkdtemp(dir=TMP)
        self.out = os.path.join(self.dir, "out")
        self.start()

    def start(self, *extra):
        self.blocks = os.path.join(self.dir, "blocks")
        os.makedirs(self.blocks, exist_ok=True)
        master, self.slave = pty.openpty()
        self.dev = subprocess.Popen([HARNESS, "device", self.blocks, *map(str, extra)], stdin=master, stdout=master,
                                    stderr=subprocess.PIPE, text=True)
        os.close(master)
        self.assertEqual(self.dev.stderr.readline().strip(), "ready")
        self.port = os.ttyname(self.slave)

    def stop(self):
        self.dev.kill()
        self.dev.wait()
        self.dev.stderr.close()
        os.close(self.slave)

    def tearDown(self):
        self.stop()

    def pull(self, *args, env=None):
        return subprocess.run([sys.executable, os.path.join(ROOT, "tools", "pull.py"), "--port", self.port,
                               "--out", self.out, *args],
                              capture_output=True, text=True, timeout=60, env={**os.environ, **(env or {})})

    def pcm(self, *seqs):
        """What the board's blocks for these clips decode to, joined."""
        out = b""
        for seq in seqs:
            ref, raw = os.path.join(self.blocks, f"{seq}.blocks"), os.path.join(self.dir, f"{seq}.raw")
            harness("decode", ref, raw)
            out += read(raw)
        return out

    def test_list(self):
        r = self.pull("--list")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn("4 recordings in 5 of 6 slots, up to 60 s a part; clock set", r.stdout)
        self.assertIn("clock not set", r.stdout)
        self.assertRegex(r.stdout, r"clip +2 .*  Grocery list\n")
        self.assertRegex(r.stdout, r"clip +4 +4\.0 s +2 parts")
        self.assertRegex(self.dev.stderr.readline(), r"^time \d+ -?\d+$")

    def test_battery_log(self):
        csv = os.path.join(self.dir, "battery.csv")
        r = self.pull("--battery", "--battery-csv", csv)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn("On battery 2 h 00 min, it powered itself off.", r.stdout)
        self.assertIn("100% (4150 mV) to 90% (3910 mV)", r.stdout)
        self.assertIn("screen on in 13 of 121 minutes (10%)", r.stdout)
        self.assertIn("drain 5.0% an hour: a full battery lasts about 20 h", r.stdout)
        lines = read(csv).decode().splitlines()
        self.assertEqual(lines[0], "minute,mV,percent,screen,recording,usb,off,boot")
        self.assertEqual(len(lines), 122)
        self.assertEqual(lines[-1], "120,3910,90,1,0,0,1,0")
        self.assertFalse(os.path.exists(self.out))   # nothing downloaded

    def test_download_then_delete(self):
        r = self.pull()
        self.assertEqual(r.returncode, 0, r.stderr)
        names = sorted(os.listdir(self.out))
        self.assertEqual(len(names), 4, names)
        self.assertEqual(len([n for n in names if n.startswith("clip3_")]), 1, names)
        for seq, parts in ((1, (1,)), (2, (2,)), (3, (3,)), (4, (4, 5))):
            name = next(n for n in names if f"clip{seq}" in n)
            with wave.open(os.path.join(self.out, name)) as w:
                self.assertEqual((w.getnchannels(), w.getsampwidth(), w.getframerate()), (1, 2, 16000))
                frames = w.readframes(w.getnframes())
            self.assertEqual(frames, self.pcm(*parts), name)

        # Each is marked downloaded on the board, which shows it.
        r = self.pull("--list")
        self.assertEqual(r.stdout.count("downloaded"), 4, r.stdout)
        # A second run fetches nothing; --delete then empties the board, every
        # part, except the protected recording until it isn't.
        r = self.pull("--delete")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn("clip 2: protected, so left on the board", r.stdout)
        self.assertIn("0 new, 4 already here, 3 deleted from the board", r.stdout)
        r = self.pull("--list")
        self.assertIn("1 recording in 1 of 6 slots", r.stdout)
        self.assertRegex(r.stdout, r"clip +2 .*protected downloaded +Grocery list\n")
        r = self.pull("--unprotect", "2")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn("recording 2 no longer protected", r.stdout)
        self.assertIn("1 deleted from the board", self.pull("--delete").stdout)
        self.assertIn("0 recordings in 0 of 6 slots", self.pull("--list").stdout)
        # Keeping one that isn't there, or a later part, is refused.
        self.assertNotEqual(self.pull("--keep", "9").returncode, 0)
        self.assertNotEqual(self.pull("--keep", "5").returncode, 0)

    def test_recording_still_going_is_left(self):
        self.stop()
        self.start(4)
        r = self.pull("--delete")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn("clip 4: still recording, left for next time", r.stdout)
        self.assertIn("3 new, 0 already here, 2 deleted from the board", r.stdout)
        self.assertIn("recording now", self.pull("--list").stdout)

    def test_name_follows_into_the_file(self):
        r = self.pull()
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(len([n for n in os.listdir(self.out) if n.endswith("_clip2_Grocery-list.wav")]), 1)

        # Renamed on the board: the next run renames the file, fetching nothing.
        r = self.pull("--name", "3", "Call w/ Sam: 2pm")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn("recording 3 named 'Call w/ Sam: 2pm'", r.stdout)
        r = self.pull("--name", "2", "")
        self.assertEqual(r.returncode, 0, r.stderr)
        r = self.pull()
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn("0 new, 4 already here", r.stdout)
        names = sorted(os.listdir(self.out))
        self.assertEqual(len(names), 4, names)
        self.assertTrue(any(n.startswith("clip3_") and n.endswith("_Call-w-Sam-2pm.wav") for n in names), names)
        self.assertTrue(any(n.endswith("_clip2.wav") for n in names), names)

        # A name too long, or for a recording that isn't there or is a later part, is refused.
        self.assertNotEqual(self.pull("--name", "3", "x" * 49).returncode, 0)
        self.assertNotEqual(self.pull("--name", "9", "x").returncode, 0)
        self.assertNotEqual(self.pull("--name", "5", "x").returncode, 0)

    def test_screenshot(self):
        out = os.path.join(self.dir, "shot.png")
        r = subprocess.run([sys.executable, os.path.join(ROOT, "tools", "snap.py"), "--port", self.port, out],
                           capture_output=True, text=True, timeout=30)
        self.assertEqual(r.returncode, 0, r.stderr)
        with open(out, "rb") as f:
            png = f.read()
        self.assertEqual(png[:8], b"\x89PNG\r\n\x1a\n")
        w, h = struct.unpack(">II", png[16:24])
        self.assertEqual((w, h), (3, 2))
        start = png.index(b"IDAT") + 4
        rows = zlib.decompress(png[start:start + struct.unpack(">I", png[start - 8:start - 4])[0]])
        pixels = [tuple(rows[y * 10 + 1 + x * 3: y * 10 + 4 + x * 3]) for y in range(2) for x in range(3)]
        self.assertEqual(pixels, [(255, 0, 0), (0, 255, 0), (0, 0, 255), (255, 255, 255), (0, 0, 0), (16, 69, 165)])
        # A screen the board can't show is an error, not a picture.
        r = subprocess.run([sys.executable, os.path.join(ROOT, "tools", "snap.py"), "--port", self.port, out, "player"],
                           capture_output=True, text=True, timeout=30)
        self.assertNotEqual(r.returncode, 0)

    def test_transcripts(self):
        fake = f"{sys.executable} -c \"print('Pick up milk, and eggs on the way home')\" {{wav}}"
        r = self.pull("--transcribe", env={"RR_TRANSCRIBE_CMD": fake})
        self.assertEqual(r.returncode, 0, r.stderr)
        names = sorted(os.listdir(self.out))
        self.assertEqual(len([n for n in names if n.endswith(".txt")]), 4, names)
        # Unnamed recordings take the transcript's first words; a named one keeps its name.
        self.assertTrue(any(n.endswith("_clip1_Pick-up-milk-and-eggs-on.wav") for n in names), names)
        self.assertTrue(any(n.endswith("_clip2_Grocery-list.wav") for n in names), names)
        self.assertTrue(any(n.endswith("_clip2_Grocery-list.txt") for n in names), names)
        with open(os.path.join(self.out, next(n for n in names if "_clip1_" in n and n.endswith(".txt")))) as f:
            self.assertEqual(f.read(), "Pick up milk, and eggs on the way home\n")
        # Named later on the board: the WAV and its transcript move together.
        self.pull("--name", "1", "Shopping")
        r = self.pull()
        self.assertEqual(r.returncode, 0, r.stderr)
        names = sorted(os.listdir(self.out))
        self.assertTrue(any(n.endswith("_clip1_Shopping.wav") for n in names), names)
        self.assertTrue(any(n.endswith("_clip1_Shopping.txt") for n in names), names)
        self.assertEqual(len(names), 8, names)


if __name__ == "__main__":
    unittest.main()
