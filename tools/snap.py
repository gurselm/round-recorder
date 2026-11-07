#!/usr/bin/env python3
"""Saves the round recorder's screen as a PNG.

    python3 tools/snap.py screen.png             # what it shows now
    python3 tools/snap.py list.png list          # after opening a screen
    python3 tools/snap.py --all shots/           # every screen, one PNG each
    python3 tools/snap.py t.png --tap 233,233     # tap there first (x,y[,ms]; repeat for more)

Screens: main, help, help-topic (the first), list, settings, dial (the mic
gain's), confirm (power off), name, keyboard, manage, player, intro (the first
run) and aod (the dim ring, drawn at full brightness). Name, keyboard, manage
and player show the newest recording; player starts it playing, and stops it
once taken. A snapshot wakes the screen. --port as for pull.py. Only the
standard library.
"""
import argparse
import os
import struct
import sys
import time
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pull  # noqa: E402

SCREENS = ["main", "help", "help-topic", "list", "settings", "dial", "confirm", "delete-all", "delete-downloaded", "tour", "name", "keyboard", "manage", "player",
           "intro", "aod"]


def png(w, h, rgb565):
    """RGB565 pixels, little-endian, as a PNG file's bytes."""
    rows = bytearray()
    for y in range(h):
        rows.append(0)   # no filter
        for v in struct.unpack_from(f"<{w}H", rgb565, y * w * 2):
            r, g, b = v >> 11, (v >> 5) & 63, v & 31
            rows += bytes((r << 3 | r >> 2, g << 2 | g >> 4, b << 3 | b >> 2))

    def chunk(kind, body):
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body))

    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(bytes(rows), 9)) + chunk(b"IEND", b""))


def snap(board, screen, path):
    board.send(f"RR SNAP {screen}".rstrip())
    while True:
        reply = board.line(15.0)   # it opens the screen and waits for it to settle first
        if reply.startswith("ERR"):
            raise RuntimeError(f"{screen or 'screen'}: {reply[4:]}")
        if reply.startswith("SNAP "):
            w, h = map(int, reply.split()[1:3])
            break
    data, size, crc = board.receive("screenshot")
    if len(data) != size or size != w * h * 2 or zlib.crc32(data) != crc:
        raise RuntimeError("the screenshot arrived damaged; take it again")
    with open(path, "wb") as f:
        f.write(png(w, h, data))
    print(f"{path}  ({w}x{h})")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--port", default=os.environ.get("RR_PORT"))
    ap.add_argument("--all", metavar="DIR", help="every screen into DIR")
    ap.add_argument("out", nargs="?", help="the PNG to write")
    ap.add_argument("screen", nargs="?", default="", help="open this screen first (the board says if it has none)")
    ap.add_argument("--tap", action="append", default=[], metavar="X,Y[,MS]",
                    help="after opening the screen, press there, as a finger would; repeat for more")
    args = ap.parse_args()
    if not args.all and not args.out:
        ap.error("give a PNG to write, or --all DIR")

    port = args.port or pull.find_port()
    board = pull.Board(port)
    try:
        if args.all:
            os.makedirs(args.all, exist_ok=True)
            for screen in SCREENS:
                snap(board, screen, os.path.join(args.all, f"{screen}.png"))
            snap(board, "main", os.path.join(args.all, "main.png"))   # and leave it there
        elif args.tap:
            if args.screen:
                snap(board, args.screen, os.devnull)   # opens it
            for t in args.tap:
                x, y, *ms = t.split(",")
                board.ask(f"RR TAP {x} {y} {ms[0] if ms else 120}", "OK")
                time.sleep((int(ms[0]) if ms else 120) / 1000 + 0.5)   # pressed, released, and handled
            snap(board, "", args.out)
        else:
            snap(board, args.screen, args.out)
        return 0
    except (RuntimeError, TimeoutError) as e:
        print(f"Error: {e}", file=sys.stderr)
        return 1
    finally:
        board.close()


if __name__ == "__main__":
    sys.exit(main())
