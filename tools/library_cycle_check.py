#!/usr/bin/env python3
"""Mutual dependencies between the subsystem libraries, and how one-sided each is.

    tools/library_cycle_check.py [build-dir]

WHY

Since 2026-08-26 the client is one STATIC library per src/ directory, and the
graph between them is cyclic in every direction. CMake permits that and repeats
the connected component on the link line, so the cycles are declared rather than
pretended away - but every one of them is a pair of libraries that cannot be
linked, tested or reasoned about apart, and Apple's ld prints a duplicate-library
warning on every target that touches one.

The numbers in TODO.md that rank the cycles by their weakest side were measured
by hand. This is that measurement, so the next person does not have to
reconstruct it, and so a claim that an edge is gone can be checked rather than
believed.

WHAT IT DOES

Reads the defined and undefined symbols of each libwowee_*.a with nm, builds the
directed graph of who needs whom, and prints every mutual pair weakest side
first. The weak side is the interesting number: an edge of one or two symbols is
usually one file in the wrong library or one singleton reached for, and those are
the ones worth taking next.

Needs archives, so it needs a configured build tree. Without one it says so and
exits clean - this is a measurement, not a gate, and it is not pinned in
sweep_guard: nm's output differs between platforms and CI builds five.

WHAT IT CANNOT SEE

Weak symbols, inline functions and anything the compiler emitted into both
sides. It counts what the linker would have to resolve across the boundary, not
what the source says.
"""
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

#: The libraries the client is made of. wowee_base, wowee_takeover and
#: wowee_openformat are deliberately not subsystems and are excluded: base is
#: the bottom of the graph by construction.
SUBSYSTEMS = ["math", "core", "network", "auth", "audio", "pipeline",
              "rendering", "game", "ui", "addons"]


def find_archives(build_dir):
    """Every libwowee_<sub>.a under the build tree, by subsystem name."""
    found = {}
    for sub in SUBSYSTEMS:
        matches = sorted(build_dir.rglob(f"libwowee_{sub}.a"))
        if matches:
            found[sub] = matches[0]
    return found


def symbols(archive):
    """(defined, undefined) symbol names in one archive."""
    try:
        out = subprocess.run(["nm", "-g", str(archive)],
                             capture_output=True, text=True, timeout=300).stdout
    except (OSError, subprocess.TimeoutExpired):
        return set(), set()
    defined, undefined = set(), set()
    for line in out.splitlines():
        match = re.match(r"^(?:[0-9a-fA-F]+)?\s*([A-Za-z])\s+(\S+)$", line.strip())
        if not match:
            continue
        kind, name = match.groups()
        if kind == "U":
            undefined.add(name)
        elif kind.isupper():
            defined.add(name)
    return defined, undefined


def main():
    build_dir = Path(sys.argv[1]) if len(sys.argv) > 1 else None
    if build_dir is None:
        for candidate in ("build-review", "build-release-arm64", "build"):
            if (ROOT / candidate).is_dir():
                build_dir = ROOT / candidate
                break
    if build_dir is None or not build_dir.is_dir():
        print("0 mutual pair(s) between the subsystem libraries\n")
        print("  no build tree to measure; pass one as an argument")
        return 0

    archives = find_archives(build_dir)
    if len(archives) < 2:
        print("0 mutual pair(s) between the subsystem libraries\n")
        print(f"  {build_dir} holds no subsystem archives; build first")
        return 0

    table = {sub: symbols(path) for sub, path in archives.items()}
    # needs[a][b] = how many symbols a leaves for b to define.
    needs = {}
    for a, (_, undef) in table.items():
        needs[a] = {}
        for b, (defined, _) in table.items():
            if a == b:
                continue
            needs[a][b] = len(undef & defined)

    pairs = []
    names = sorted(archives)
    for i, a in enumerate(names):
        for b in names[i + 1:]:
            forward, back = needs[a][b], needs[b][a]
            if forward and back:
                pairs.append((min(forward, back), a, forward, b, back))
    pairs.sort()

    print(f"{len(pairs)} mutual pair(s) between the subsystem libraries\n")
    for weak, a, forward, b, back in pairs:
        heavy = f"{a} -> {b}" if forward >= back else f"{b} -> {a}"
        light = f"{b} -> {a}" if forward >= back else f"{a} -> {b}"
        print(f"  weak side {weak:>4}   {light}   (against {max(forward, back)} "
              f"for {heavy})")
    if not pairs:
        print("  (none)")
    print(f"\n{len(archives)} subsystem archives measured in {build_dir}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
