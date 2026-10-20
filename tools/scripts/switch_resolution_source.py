#!/usr/bin/env python3
"""Make a Switch-only build copy of the game's minimum-resolution check.

The mode-list builder rejects height<480 at EC879B, dropping 640x360 even
when the graphics adapter offers it. Lower that one check to360; do not edit
the executable or shared lifter output used by the Apple targets. Fail closed
if the instruction or generated expression changes.
"""
import argparse
from pathlib import Path
import re

MARKER = "/* 00EC879B: cmp ebx, 0x1E0 */"
CHECK = re.compile(re.escape(MARKER) + r"(\s*\{\s*const uint32_t a = c->ebx, b = )0x1E0u;")


def patch(source):
    sites = source.count(MARKER)
    if not sites or len(CHECK.findall(source)) != sites:
        raise ValueError("Switch resolution check changed; expected EC879B cmp ebx,480")
    return CHECK.sub(lambda m: MARKER + m[1] + "0x168u; /* Switch: allow 640x360 (0.5x). */", source)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    group = ap.add_mutually_exclusive_group(required=True)
    group.add_argument("--find", type=Path, metavar="GENERATED_DIR")
    group.add_argument("--patch", nargs=2, type=Path, metavar=("SOURCE", "OUTPUT"))
    args = ap.parse_args()
    if args.find:
        matches = []
        for name in (args.find / "sources.txt").read_text().splitlines():
            path = args.find / name.strip()
            if path.suffix == ".c" and MARKER in path.read_text():
                patch(path.read_text())  # validate before configuring the build
                matches.append(path.resolve())
        if len(matches) != 1:
            raise ValueError(f"expected one generated source with EC879B, found{len(matches)}")
        print(matches[0])
    else:
        source, output = args.patch
        result = patch(source.read_text())
        output.parent.mkdir(parents=True, exist_ok=True)
        if not output.exists() or output.read_text() != result:
            output.write_text(result)


if __name__ == "__main__":
    main()
