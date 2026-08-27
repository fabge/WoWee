#!/usr/bin/env python3
"""Player fields the client keeps from the character it was playing before.

    tools/character_switch_reset_check.py

WHY

This client has shipped the same bug three times.

    2026-08-26  the pet's action bar, stance and spell list. A hunter's pet was
                still in the mage's spellbook.
    2026-08-27  the pet's stats. A hunter's pet attack power was readable from
                a mage, and the paperdoll's pet tab drew it.
    2026-08-27  eight player fields - shapeshift form, chosen title, honor,
                arena points, rested XP, both mana regen rates, and the sticky
                transport. A mage after a druid was in bear form. A character
                with no title wore the previous one's.

The shape never changes, and it is what makes these survive a reset that looks
complete: an update field is sent when the server has a value for it, so **a
character who has none of a thing is never told it is zero**. Reading the reset
block tells you what it clears; it cannot tell you what it forgot.

WHAT IT DOES

Reads every GameHandler member that EntityController writes while applying a
unit's update fields, and reports each one that neither
GameHandler::resetStateForCharacterSwitch nor GameHandler::handleLoginVerifyWorld
clears. Both run on the way into the world, so either is a real clearing site.

WHAT IT CANNOT SEE

Which of them are per-character. A field written from a unit's update fields
usually is, but the account's character list is not, and neither is the
expansion's field-index table - both are exempt below, by name and with the
reason.

It also finds the writes by matching `owner_.xRef() = ` inside the two field
appliers named below. If either is split up or renamed the population line
drops, and sweep_guard's own blind-sweep check reports that - the number to
watch is the second one, not the first. That is not hypothetical: the first
draft of this file anchored on the wrong class name, found four writes instead
of forty, and said everything was fine.
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
HEADER = ROOT / "include/game/game_handler.hpp"
CALLBACKS = ROOT / "src/game/game_handler_callbacks.cpp"
ENTITY = ROOT / "src/game/entity_controller.cpp"

#: Written from a unit's fields and correctly kept across a character switch.
KEPT_ON_PURPOSE = {
    "characters": "the account's character list, not the character's",
    "updateFieldTable_": "the expansion's field-index table, not per-character",
}

#: Where a player's or a pet's own update fields are unpacked into GameHandler.
#: applyPlayerStatFields is the character sheet's numbers and the player-only
#: fields; applyUnitFieldsOnUpdate is every unit, and is where the pet's own
#: numbers are picked out by guid.
APPLYING_FUNCTIONS = ("applyPlayerStatFields", "applyUnitFieldsOnUpdate")

#: Both run on the way into the world, so a member cleared in either is cleared.
CLEARING_FUNCTIONS = ("resetStateForCharacterSwitch", "handleLoginVerifyWorld")


def function_body(source, cls, name):
    """The body of one out-of-line member definition, or an empty string.

    The signature may wrap across lines and the return type may itself be
    qualified, so this anchors on `<Class>::<name>(` rather than on the start of
    the line.
    """
    match = re.search(re.escape(cls) + r"::" + re.escape(name) +
                      r"\s*\([^)]*\)\s*(?:const\s*)?\{(.*?)\n\}\n", source, re.S)
    return match.group(1) if match else ""


def main():
    if not (HEADER.is_file() and CALLBACKS.is_file() and ENTITY.is_file()):
        print("0 player field(s) the character switch does not clear\n")
        print("  the files this reads are not where it expects them")
        return 0

    header = HEADER.read_text(errors="ignore")
    callbacks = CALLBACKS.read_text(errors="ignore")
    entity = ENTITY.read_text(errors="ignore")

    cleared = "\n".join(function_body(callbacks, "GameHandler", name)
                        for name in CLEARING_FUNCTIONS)
    applied = "\n".join(function_body(entity, "EntityController", name)
                        for name in APPLYING_FUNCTIONS)

    # accessor -> the member it hands out
    members = dict(re.findall(r"\b(\w+Ref)\s*\(\s*\)[^\{]*\{\s*return\s+([A-Za-z_]\w*)",
                              header))

    written, missing = set(), []
    for accessor, member in members.items():
        # An assignment through the accessor, with or without a subscript.
        if not re.search(r"owner_\." + accessor + r"\s*\(\s*\)\s*(\[[^\]]*\])?\s*=[^=]",
                         applied):
            continue
        written.add(member)
        if member in KEPT_ON_PURPOSE:
            continue
        if re.search(r"\b" + re.escape(member) + r"\b", cleared):
            continue
        missing.append(member)

    print(f"{len(missing)} player field(s) the character switch does not clear\n")
    for member in sorted(missing):
        print(f"  {member}")
        print("      written from a unit's update fields and named by neither "
              "resetStateForCharacterSwitch nor handleLoginVerifyWorld")
    if not missing:
        print("  (none)")

    print(f"\n{len(written)} member(s) written from update fields were checked, "
          f"{len(KEPT_ON_PURPOSE)} kept on purpose")
    return 0


if __name__ == "__main__":
    sys.exit(main())
