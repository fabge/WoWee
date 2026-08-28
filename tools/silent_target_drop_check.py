#!/usr/bin/env python3
"""Target-guid writes that do not tell the interface.

    tools/silent_target_drop_check.py

WHY

Reported as "I slayed an enemy and it is still selected next to my avatar and
I cannot deselect it, while living targets deselect fine".

The interface redraws the target frame on PLAYER_TARGET_CHANGED and on nothing
else. Three places dropped the player's target by assigning the guid directly -
a looted corpse despawned locally, a game object despawned locally, and every
SMSG_DESTROY_OBJECT for the selected unit - and none of them fired the event.
So the frame went on drawing a unit the client had already forgotten, and
Escape reached CombatHandler::clearTarget, which saw a guid of zero and did
nothing at all. Two half-states that add up to a frame nothing can clear.

Nothing in the log said so either: the diagnostic pair in setTarget and
clearTarget only covers the paths that go through them, so the session log
showed a target changing from one guid to zero with no "Target cleared"
between - which is what pointed at this in the end.

WHAT IT DOES

setTargetGuidRaw is the raw store behind the target guid. Only
CombatHandler::setTarget and CombatHandler::clearTarget may call it; they are
the two that fire the event and log the change. Everything else goes through
clearTarget.

WHAT IT CANNOT SEE

A path that drops the target some other way - assigning the member from inside
GameHandler, or a future setter with a different name. It also says nothing
about the *focus* guid, which has the same shape and no reported fault yet.
"""
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
TREE = [ROOT / "src", ROOT / "include"]

RAW_SETTER = "setTargetGuidRaw"

# The file allowed to call it, because it is the one that fires
# PLAYER_TARGET_CHANGED alongside every call.
OWNER = "src/game/combat_handler.cpp"
# And the header that defines the setter itself.
DEFINITION = "include/game/game_handler.hpp"


def main():
    if not (ROOT / OWNER).is_file():
        print(f"{OWNER} is gone, so the rule below is meaningless")
        return 1

    calls = 0
    offenders = []
    for base in TREE:
        if not base.is_dir():
            continue
        for path in sorted(base.rglob("*")):
            if path.suffix not in (".cpp", ".hpp"):
                continue
            rel = str(path.relative_to(ROOT))
            try:
                text = path.read_text(errors="ignore")
            except OSError:
                continue
            for i, raw in enumerate(text.split("\n")):
                stripped = raw.strip()
                if stripped.startswith("//") or stripped.startswith("*"):
                    continue
                if not re.search(r"(?<![\w:])%s\s*\(" % RAW_SETTER, raw):
                    continue
                calls += 1
                if rel in (OWNER, DEFINITION):
                    continue
                offenders.append((rel, i + 1))

    print(f"{calls} call site(s) of {RAW_SETTER}\n")
    print(f"{len(offenders)} outside {OWNER}, so the interface is never told:")
    for rel, line in offenders:
        print(f"  {rel}:{line}  (use CombatHandler::clearTarget)")
    if not offenders:
        print("  (none)")
    return 1 if offenders else 0


if __name__ == "__main__":
    sys.exit(main())
