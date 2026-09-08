#!/usr/bin/env python3
"""
Flash a prepped character pack via USB (pio run -t uploadfs).

On this board USB is not merely faster than the BLE drop target - it is the
only transport that works for a full-size pack. The desktop sends 256-byte
chunks and waits for an ack on each, which tops out around 3 KB/s, so a
~570 KB pack needs about three minutes and the transfer times out first.
USB writes the whole filesystem image in a few seconds.

Usage:
  python3 tools/flash_character.py characters/bufo
"""
import json, os, shutil, subprocess, sys
from pathlib import Path

PROJECT = Path(__file__).resolve().parent.parent
DATA    = PROJECT / "data" / "characters"
CAP     = 1_800_000
# This fork defines three environments (knob, paneltest, inputtest), so the
# target has to be named or PlatformIO would build all of them.
ENV     = "knob"


def pio_bin() -> str:
    """PlatformIO is often installed in its own venv and not on PATH."""
    found = shutil.which("pio")
    if found:
        return found
    venv = Path.home() / ".platformio" / "penv" / "bin" / "pio"
    if venv.exists():
        return str(venv)
    sys.exit("pio not found - install PlatformIO Core or put pio on PATH")


def flash(src: Path) -> None:
    if not (src / "manifest.json").exists():
        sys.exit(f"no manifest.json in {src} — run tools/prep_character.py first")
    name = json.loads((src / "manifest.json").read_text())["name"]

    total = sum(f.stat().st_size for f in src.iterdir() if f.is_file())
    if total > CAP:
        sys.exit(f"{total:,} bytes — over the {CAP:,} LittleFS cap")

    # uploadfs flashes everything under data/; the firmware only reads one
    # character at a time, so a stale sibling just wastes partition space.
    if DATA.exists():
        shutil.rmtree(DATA)
    dst = DATA / name
    shutil.copytree(src, dst)
    print(f"staged {name}: {total:,} bytes -> {dst}")

    cmd = [pio_bin(), "run", "-e", ENV, "-t", "uploadfs"]
    port = os.environ.get("KNOB_PORT")
    if port:
        cmd += ["--upload-port", port]
    print("+ " + " ".join(cmd))
    subprocess.run(cmd, cwd=PROJECT, check=True)
    print("\nflashed. uploadfs replaces the whole filesystem, so the pack is")
    print("now the only character installed and the firmware picks it up on")
    print("reboot. To switch back to ASCII: hold the screen -> settings ->")
    print("ascii pet.")


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    flash(Path(sys.argv[1]).resolve())
