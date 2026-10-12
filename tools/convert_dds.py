#!/usr/bin/env python3
"""Convert the shipped DDS textures to PNG.

Why: raylib loads DDS, but the engine's own world tileset loader
(Source/World/OzOzoneLoader.cpp LoadWorldTextures) only accepts ".png", and a
PNG keeps alpha as a real 8-bit channel that round-trips through the .oztex
packages. Converting every texture means one format end to end.

Usage:
    python tools/convert_dds.py --dry-run          # inventory + decode check, writes nothing
    python tools/convert_dds.py                    # convert in place (keeps the .dds)
    python tools/convert_dds.py --delete-source    # convert and remove the .dds

Reports, per file, the mode, size and whether the result carries an alpha
channel, then a summary. A file that Pillow cannot decode is reported as FAILED
and skipped - it is never silently dropped, because a missing texture in a
package resolves to a null Texture2D and renders as flat colour.
"""

import argparse
import sys
from pathlib import Path

try:
    from PIL import Image
except ImportError:
    sys.exit("Pillow is required: pip install Pillow")

REPO = Path(__file__).resolve().parent.parent
GAMEDATA = REPO / "GameData"


def collect(root: Path):
    # sorted so two runs report in the same order
    return sorted(p for p in root.rglob("*.dds") if p.is_file())


def convert_one(src: Path, delete_source: bool):
    """Return (status, detail). status is 'ok', 'skip' or 'fail'."""
    dst = src.with_suffix(".png")

    if dst.exists() and dst.stat().st_mtime >= src.stat().st_mtime:
        return "skip", f"png already newer ({dst.name})"

    try:
        with Image.open(src) as im:
            im.load()
            mode, size = im.mode, im.size
            # DDS may decode as RGB, RGBA, or (rarely) L/LA/P. Normalise to
            # something the engine can upload, preserving alpha when present.
            has_alpha = "A" in mode or "transparency" in im.info
            out = im.convert("RGBA" if has_alpha else "RGB")
            # A fully opaque RGBA is 33% larger than RGB for no gain; the engine
            # has no use for a dead alpha channel on a wall texture.
            if has_alpha and out.mode == "RGBA" and out.getchannel("A").getextrema() == (255, 255):
                out = out.convert("RGB")
                has_alpha = False
            out.save(dst, "PNG", optimize=True)
    except Exception as exc:  # noqa: BLE001 - report and continue, never abort the run
        return "fail", f"{type(exc).__name__}: {exc}"

    saved_mb = dst.stat().st_size / (1024 * 1024)
    src_kb = src.stat().st_size / 1024
    detail = f"{mode}->{out.mode} {size[0]}x{size[1]} alpha={'yes' if has_alpha else 'no'} " \
             f"{src_kb:.0f}KB->{saved_mb:.2f}MB"

    if delete_source:
        src.unlink()
    return "ok", detail


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dry-run", action="store_true",
                    help="decode and report, but write nothing")
    ap.add_argument("--delete-source", action="store_true",
                    help="remove each .dds after a successful conversion")
    args = ap.parse_args()

    files = collect(GAMEDATA)
    if not files:
        print("no .dds found under GameData/")
        return 0

    print(f"{len(files)} .dds under {GAMEDATA.relative_to(REPO)}")
    if args.dry_run:
        print("(dry run - nothing written)\n")

    fails, skips, oks = [], [], 0
    for src in files:
        if args.dry_run:
            # Decode only: confirm Pillow can read it before we commit to a
            # rewrite. Nothing is written.
            try:
                with Image.open(src) as im:
                    im.load()
                    status, detail = "ok", f"{im.mode} {im.size[0]}x{im.size[1]}"
            except Exception as exc:  # noqa: BLE001
                status, detail = "fail", f"{type(exc).__name__}: {exc}"
        else:
            status, detail = convert_one(src, args.delete_source)

        rel = src.relative_to(REPO)
        if status == "ok":
            oks += 1
            if args.dry_run:
                print(f"  ok    {rel}  {detail}")
        elif status == "skip":
            skips += 1
        else:
            fails.append(rel)
            print(f"  FAIL  {rel}  {detail}")

    print(f"\n{oks} ok, {skips} skipped, {len(fails)} failed")
    if fails:
        print("\nfailed files (never silently dropped - each would render flat):")
        for f in fails:
            print(f"  {f}")
        return 1
    if args.dry_run:
        print("\nre-run without --dry-run to write the PNGs")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
