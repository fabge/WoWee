#!/usr/bin/env python3
"""Pipeline owners the renderer would never rebuild.

    tools/render_pipeline_registry_check.py

WHY

A Vulkan graphics pipeline bakes in its render pass and sample count, so every
subsystem holding one must rebuild when MSAA changes or the swapchain is
recreated. Renderer did that from a hand-written enumeration sixty lines long.

The failure that produces is silent and badly disguised. A subsystem added to
the renderer and left out of the list keeps a pipeline bound to a destroyed
render pass; nothing warns, and it draws with a stale pass until the driver
loses the device. The symptom is a crash a fraction of a second after an
anti-aliasing change, which looks nothing like a missing line in a list.

WHAT IT DOES

Every type under include/rendering/ that declares recreatePipelines() must be
named in Renderer::registerPipelineOwners(). Matching is by the member the
registration calls it on, not by class name, because several subsystems are
owned by another subsystem rather than by Renderer.

WHAT IT CANNOT SEE

Whether the order is right - only that nothing is missing. Order is why the
registry preserves insertion order and why two entries register more than a
bare recreatePipelines() call.
"""
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
RENDERER = ROOT / "src/rendering/renderer.cpp"
HEADERS = ROOT / "include/rendering"


def without_comments(text):
    """Comments dropped, so a header that only *mentions* the name is not read
    as declaring it - pipeline_registry.hpp explains the whole mechanism in its
    own doc comment and was the first false positive this produced."""
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def declaring_types():
    """Types that declare recreatePipelines(), by header."""
    out = {}
    for path in sorted(HEADERS.rglob("*.hpp")):
        text = without_comments(path.read_text(errors="ignore"))
        if not re.search(r"\brecreatePipelines\s*\(", text):
            continue
        names = re.findall(r"^\s*class\s+(\w+)", text, re.M)
        out[path.relative_to(ROOT).as_posix()] = names
    return out


def main() -> int:
    if not RENDERER.exists() or not HEADERS.is_dir():
        print("renderer.cpp or include/rendering is not where this expects it; "
              "the zero below would mean the scan broke.")
        return 1
    src = RENDERER.read_text(errors="ignore")

    m = re.search(r"void Renderer::registerPipelineOwners\(\)\s*\{(.*?)\n\}", src, re.S)
    if not m:
        print("registerPipelineOwners() not found - the registry moved or was "
              "renamed, and this check cannot answer.")
        return 1
    body = m.group(1)
    registered = set(re.findall(r'add\(\s*"(\w+)"', body))
    registered |= set(re.findall(r'pipelineRegistry_\.add\(\s*"(\w+)"', body))

    decls = declaring_types()
    # A header counts as covered when any registered name mentions its class,
    # case-insensitively and ignoring a trailing underscore on the member.
    missing = []
    for header, classes in decls.items():
        stem = pathlib.Path(header).stem.replace("_", "")
        hit = any(stem in r.lower().replace("_", "") or
                  r.lower().replace("_", "") in stem or
                  any(c.lower() in r.lower() or r.lower() in c.lower() for c in classes)
                  for r in registered)
        if not hit:
            missing.append((header, classes))

    print(f"{len(decls)} header(s) declare recreatePipelines(), "
          f"{len(registered)} entries registered\n")
    print(f"{len(missing)} declare it and are never registered - these keep a "
          f"pipeline bound to a destroyed render pass:")
    for header, classes in missing:
        print(f"  {header:58} {', '.join(classes) or '(no class found)'}")
    if not missing:
        print("  (none)")
    return 1 if missing else 0


if __name__ == "__main__":
    sys.exit(main())
