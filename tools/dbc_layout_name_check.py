#!/usr/bin/env python3
"""DBC column names the code asks for and no layout declares.

    tools/dbc_layout_name_check.py

WHY

Every DBC read goes through a named lookup into
Data/expansions/<x>/dbc_layouts.json - `spellL->field("RangeIndex")`,
`(*atL)["ParentAreaNum"]`. A name the layout does not declare comes back as
0xFFFFFFFF, and every caller in this client treats that as "this build has no
such column" and skips the read. So the feature behind it is switched off in
silence: no error, no warning, and a plausible zero everywhere it is read.

That is how auto self-cast was off for the life of the client. The code asked
for "EffectImplicitTargetA" while every other effect column in the same
function is numbered - "EffectApplyAuraName0" - and no layout has ever declared
either spelling. `getSpellImplicitTargetA` answered 0 for every spell, the
friendly-target test above it never fired, and healing yourself with a mob
selected sent the heal at the mob and got "Invalid target" back. The predicate
it feeds carries a comment saying its four values were "measured over the
shipped Spell.dbc" - measured offline, by hand, and then wired to a column that
was never read.

The sibling sweeps cover the other two failures: dbc_layout_check.py finds an
index past the end of the file, dbc_column_agreement_check.py finds an index
that is in range and holds the wrong field. This one finds the name that
resolves to nothing at all.

WHAT IT DOES

Collects every literal passed to `->field("...")` and every `[...]` subscript of
a layout pointer in src/ and tools/, and reports the ones no dbc_layouts.json
declares under any table.

WHAT IT CANNOT SEE

Which table a name was meant for - a name is accepted if any layout declares
it, so asking the Spell layout for a column that only AreaTable has still
passes. And a name built at run time rather than written as a literal.
"""
import json
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
TREE = [ROOT / "src", ROOT / "tools"]
LAYOUTS = sorted((ROOT / "Data" / "expansions").glob("*/dbc_layouts.json"))

# spellL->field("X")  /  (*atL)["X"]  /  layout->field("X")
CALL = re.compile(r'->\s*field\s*\(\s*"([A-Za-z0-9_]+)"\s*\)')
SUBSCRIPT = re.compile(r'\(\s*\*\s*[A-Za-z_][A-Za-z0-9_]*L?\s*\)\s*\[\s*"([A-Za-z0-9_]+)"\s*\]')


def main():
    if not LAYOUTS:
        print("No dbc_layouts.json under Data/expansions, so the rule below "
              "is meaningless")
        return 1

    declared = set()
    for path in LAYOUTS:
        try:
            data = json.loads(path.read_text())
        except (OSError, ValueError) as exc:
            print(f"{path} could not be read: {exc}")
            return 1
        for table in data.values():
            if isinstance(table, dict):
                declared.update(table.keys())

    asked = {}
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
                for pattern in (CALL, SUBSCRIPT):
                    for name in pattern.findall(raw):
                        asked.setdefault(name, (rel, i + 1))

    missing = sorted((n, w) for n, w in asked.items() if n not in declared)

    print(f"{len(asked)} DBC column name(s) asked for across "
          f"{len(LAYOUTS)} layout file(s), {len(declared)} declared\n")
    print(f"{len(missing)} that no layout declares, so the read is skipped:")
    for name, (rel, line) in missing:
        print(f"  {rel}:{line}  \"{name}\"")
    if not missing:
        print("  (none)")
    return 1 if missing else 0


if __name__ == "__main__":
    sys.exit(main())
