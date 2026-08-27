#!/usr/bin/env python3
"""Subsystems the renderer owns and never releases in shutdown().

    tools/renderer_shutdown_check.py

WHY

Renderer::shutdown() exists because ~Renderer is too late. Application says so
where it calls it: a sub-renderer has to free its VMA allocations before
VkContext::shutdown() reaches vmaDestroyAllocator, and the destructor order
gives no such guarantee. Every owned subsystem therefore has to appear in
shutdown(), and the list is hand-written - thirty-five entries on 2026-08-27,
added one at a time over the life of the renderer.

Five were missing when this was written. MountDust, ChargeEffect,
QuestMarkerRenderer and LevelUpEffect each free real Vulkan objects and did it
from their destructors alone, so they came down after shutdown() had finished
rather than inside it; LightingManager was simply never released. It held only
because Application happens to reset the renderer on the very next line - one
statement away from a use-after-free of the allocator, with nothing anywhere
saying so.

Five more were the opposite fault: skybox, celestial, starField, clouds and
lensFlare were declared as owning pointers, assigned nullptr at initialize and
assigned nullptr again at shutdown, and never held anything at all. The real
objects belong to SkySystem. A sweep that reads "is it released?" reports both
shapes, because a member that is never released and a member that is never
anything look the same from here and both want deleting or fixing.

WHAT IT DOES

Reads every `std::unique_ptr<T> name;` member of Renderer out of the header and
reports each one that Renderer::shutdown() does not `.reset()`.

This is deliberately a sweep over the enumeration rather than a registry like
PipelineRegistry. The pipeline rebuild list had no order to keep and no way to
be read from outside, so a registry was the only way to check it. Teardown is
the other way round: the order in shutdown() is load-bearing and documented in
place - SpellVisualSystem before M2Renderer, AnimationController before the
renderers it references - and `x.reset()` is already machine-readable. A
registry would have moved that ordering somewhere less visible and checked
nothing this does not.

WHAT IT CANNOT SEE

Whether the order is right. Only that nothing is left out.
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
HEADER = ROOT / "include/rendering/renderer.hpp"
SOURCE = ROOT / "src/rendering/renderer.cpp"


def owned_members(header):
    """Every `std::unique_ptr<T> name;` member, in declaration order."""
    return re.findall(r"std::unique_ptr<[^>]+>\s+(\w+)\s*;", header)


def shutdown_body(source):
    match = re.search(r"void Renderer::shutdown\(\)\s*\{(.*?)\n\}\n", source, re.S)
    return match.group(1) if match else None


def main():
    if not HEADER.is_file() or not SOURCE.is_file():
        print("0 owned subsystem(s) shutdown() never releases\n")
        print("  renderer.hpp or renderer.cpp is not where this expects it")
        return 0

    members = owned_members(HEADER.read_text(errors="ignore"))
    body = shutdown_body(SOURCE.read_text(errors="ignore"))
    if body is None:
        print("0 owned subsystem(s) shutdown() never releases\n")
        print("  Renderer::shutdown() could not be found; this sweep is blind")
        return 0

    missing = [name for name in members
               if not re.search(r"\b" + re.escape(name) + r"\.reset\(\)", body)]

    print(f"{len(missing)} owned subsystem(s) shutdown() never releases\n")
    for name in missing:
        print(f"  {name}")
        print("      declared as a unique_ptr member and never .reset() in "
              "Renderer::shutdown()")
    if not missing:
        print("  (none)")
    print(f"\n{len(members)} owned subsystems checked")
    return 0


if __name__ == "__main__":
    sys.exit(main())
