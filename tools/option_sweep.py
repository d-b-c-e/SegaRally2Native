"""Renders F9 snapshots once with the default settings and once with each
visual option turned off, to see which options change the picture in Sega
Rally 2. The pictures go to out/sweep/<snap>_<variant>.bmp.

Usage (from the folder of SegaRally2.exe):
    python tools/option_sweep.py out/snap/snap_N.bin [more snapshots...]
"""
import os
import shutil
import subprocess
import sys

VARIANTS = [
    ("base", []), ("base2", []),
    ("nosun", ["--no-sun"]), ("noshadows", ["--no-shadows"]),
    ("rtoff", ["--rt", "off"]), ("rtshadows", ["--rt", "shadows"]),
    ("nohdr", ["--no-hdr"]), ("noao", ["--no-ao"]),
    ("nobloom", ["--no-bloom"]),
    ("nosoft", ["--no-soft"]), ("nosharpen", ["--no-sharpen"]), ("nohaze", ["--no-haze"]),
    ("classichud", ["--classic-hud"]),
]


def main():
    os.chdir(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
    snaps = sys.argv[1:]
    if not snaps:
        print(__doc__)
        return 1
    os.makedirs("out/sweep", exist_ok=True)
    for snap in snaps:
        name = os.path.splitext(os.path.basename(snap))[0]
        for var, args in VARIANTS:
            if os.path.exists("out/capture.bmp"):
                os.remove("out/capture.bmp")
            subprocess.run(["SegaRally2.exe", "--replay", snap, "--shot", "1"] + args,
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            if os.path.exists("out/capture.bmp"):
                shutil.move("out/capture.bmp", f"out/sweep/{name}_{var}.bmp")
                print(name, var)
            else:
                print(name, var, ": no picture")
    print("Done: out/sweep")
    return 0


if __name__ == "__main__":
    sys.exit(main())
