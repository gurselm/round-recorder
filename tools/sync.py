#!/usr/bin/env python3
"""One click: the round recorder's new recordings onto this computer.

Downloads what's new into ~/Recordings/Round (or --out), with transcripts if
the project's .venv has faster-whisper, tells you how it went in a desktop
notification, and opens the folder. tools/round-recorder.desktop puts it in
the app menu on Linux; on macOS, run it from a Shortcut or the Dock.
"""
import argparse
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)


def notify(title, text):
    if shutil.which("notify-send"):
        subprocess.run(["notify-send", "-a", "Round recorder", title, text])
    elif sys.platform == "darwin":
        subprocess.run(["osascript", "-e", f'display notification {text!r} with title {title!r}'])
    print(f"{title}: {text}")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--out", default=os.path.expanduser("~/Recordings/Round"))
    ap.add_argument("--port", default=os.environ.get("RR_PORT"))
    args = ap.parse_args()

    venv = os.path.join(ROOT, ".venv", "bin", "python")
    python = venv if os.path.exists(venv) else sys.executable
    has_whisper = subprocess.run([python, "-c", "import faster_whisper"], capture_output=True).returncode == 0
    cmd = [python, os.path.join(HERE, "pull.py"), "--out", args.out]
    if has_whisper:
        cmd.append("--transcribe")
    if args.port:
        cmd += ["--port", args.port]
    r = subprocess.run(cmd, capture_output=True, text=True)
    lines = (r.stdout + r.stderr).strip().splitlines()
    if r.returncode:
        notify("Couldn't download", lines[-1] if lines else "pull.py failed")
        return r.returncode
    notify("Recordings downloaded", lines[-1] if lines else "done")
    opener = "open" if sys.platform == "darwin" else "xdg-open"
    if shutil.which(opener):
        subprocess.Popen([opener, args.out])
    return 0


if __name__ == "__main__":
    sys.exit(main())
