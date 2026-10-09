#!/usr/bin/env python3
"""Copy every built env (now one: `matrix`, for every board) into a folder ESP Web Tools can flash from: the
Spectre Matrix app's install page (install.html) puts the firmware on a new,
blank driver board over USB.

    pio run                      # build first
    scripts/export_web.py <dest> # e.g. a local folder, then rsync it to the
                                 # server next to the app: <app>/firmware/

Writes <dest>/<env>/{bootloader,partitions,boot_app0,firmware}.bin +
manifest.json per env, and <dest>/index.json (the envs, for the page).
"""
import json
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BUILD = ROOT / ".pio" / "build"
BOOT_APP0 = Path.home() / ".platformio/packages/framework-arduinoespressif32/tools/partitions/boot_app0.bin"

# ESP32 flash layout: same offsets in every partition table of this project
# (otadata at 0xe000, app0 at 0x10000)
PARTS = [
    ("bootloader.bin", 0x1000),
    ("partitions.bin", 0x8000),
    ("boot_app0.bin", 0xE000),
    ("firmware.bin", 0x10000),
]

# what each env is, for people choosing on the install page
DESCRIPTIONS = {
    "matrix": "Every board: choose its panels afterwards in the app (Settings → Display)",
}


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    dest = Path(sys.argv[1]).expanduser()
    version = subprocess.run(["git", "describe", "--tags", "--always", "--dirty"], cwd=ROOT,
                             capture_output=True, text=True).stdout.strip() or "unknown"

    envs = sorted(p.parent.name for p in BUILD.glob("*/firmware.bin") if (p.parent / "bootloader.bin").exists())
    if not envs:
        sys.exit("No builds found, run `pio run` first")

    index = []
    for env in envs:
        out = dest / env
        out.mkdir(parents=True, exist_ok=True)
        for name, _ in PARTS:
            src = BOOT_APP0 if name == "boot_app0.bin" else BUILD / env / name
            shutil.copyfile(src, out / name)
        manifest = {
            "name": f"Spectre Matrix {env}",
            "version": version,
            "new_install_prompt_erase": True,
            "builds": [{
                "chipFamily": "ESP32",
                "parts": [{"path": name, "offset": offset} for name, offset in PARTS],
            }],
        }
        (out / "manifest.json").write_text(json.dumps(manifest, indent=4) + "\n")
        size = (BUILD / env / "firmware.bin").stat().st_size
        index.append({"env": env, "description": DESCRIPTIONS.get(env, env), "size": size})
        print(f"{env:16} {size:>8} bytes")

    (dest / "index.json").write_text(json.dumps({"version": version, "envs": index}, indent=2) + "\n")
    print(f"{len(envs)} envs, version {version} -> {dest}")


if __name__ == "__main__":
    main()
