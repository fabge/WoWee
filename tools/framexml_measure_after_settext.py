#!/usr/bin/env python3
"""Where the interface writes a string and measures it in the same breath.

The text twin of framexml_measure_after_move.py, and the same story. A font
string's size used to be written only by the once-a-frame layout pass, which
caches against the text it measured -- so anything that set text and asked how
big it was before the next frame was answered for the *previous* text. One line
where there were now three.

Two of the sites below were live bugs on 2026-08-27:
WorldMapQuestFrame_UpdateQuests sized every quest block from the objectives
string it had just filled, so each block was built to fit one line and the next
was drawn over the top of it; and WatchFrame's quest handler measured the lines
it had just written, reported that it had used no pixels, and took the branch
that collapses the tracker and disables its own expand button.

The fix measures on demand: ui::sizeFontString is asked by the widget
accessors behind every rect and text getter. So these all answer now, and the
sweep stays for the reason its sibling does -- it names what depends on that.
If measuring is ever made lazy again, this is the list that breaks, and none of
it breaks loudly: a wrong height is a frame drawn in the wrong place, not an
error.

Reports the sites as a population rather than as faults, and fails only when
the thing they depend on is not wired -- an empty population means the matcher
has gone blind, which reads exactly like a clean tree.
"""
import re, sys, pathlib, collections

ROOT = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else "Data/interface")
REPO = pathlib.Path(__file__).resolve().parent.parent

SETTEXT = re.compile(r'\b([A-Za-z_][\w.\[\]"\']*)\s*:\s*Set(?:Formatted)?Text\s*\(')
MEASURE = re.compile(r'\b([A-Za-z_][\w.\[\]"\']*)\s*:\s*Get(?:String)?Height\s*\(')
WINDOW = 12   # lines; setting and measuring happen close together


def scan(path):
    hits = []
    lines = path.read_text(errors="ignore").splitlines()
    written = {}
    for n, line in enumerate(lines):
        if line.lstrip().startswith("--"):
            continue
        for m in SETTEXT.finditer(line):
            written[m.group(1)] = n
        for m in MEASURE.finditer(line):
            name = m.group(1)
            src = written.get(name)
            if src is not None and 0 <= n - src <= WINDOW:
                hits.append((n + 1, name, lines[n].strip()[:70]))
    return hits


def measuresOnDemand():
    """Both widget accessors ask for a fresh measurement before answering.

    Read out of the source rather than restated, so this cannot quietly agree
    with a version of the file that no longer does it.
    """
    api = REPO / "src/addons/lua_widget_api.cpp"
    if not api.is_file():
        return None
    text = api.read_text(errors="ignore")
    wired = {}
    for fn in ("measuredWidgetOf", "textWidgetOf"):
        at = text.find(f"Widget* {fn}(lua_State* L, int index) {{")
        if at == -1:
            wired[fn] = False
            continue
        body = text[at:text.find("\n}\n", at)]
        wired[fn] = "sizeFontString" in body
    return wired


total = collections.Counter()
rows = []
if ROOT.is_dir():
    for p in sorted(ROOT.rglob("*.lua")):
        for line, name, text in scan(p):
            total[p.name] += 1
            rows.append((p.name, line, name, text))

if not ROOT.is_dir():
    print("the extracted interface is missing - nothing was scanned.")
    raise SystemExit(0)

print(f"{len(rows)} places measure a string they just wrote, in {len(total)} files\n")
for f, c in total.most_common(12):
    print(f"  {c:>3}  {f}")
print()
for f, line, name, text in rows[:10]:
    print(f"  {f}:{line}  {name}")
print()

wired = measuresOnDemand()
if wired is None:
    print("lua_widget_api.cpp could not be read - nothing was checked.")
    raise SystemExit(1)

unprotected = [fn for fn, ok in wired.items() if not ok]
if not rows:
    print("no site matched, so the matcher has gone blind or the interface moved "
          "- an empty population here reads exactly like a clean tree.")
    raise SystemExit(1)
if unprotected:
    print(f"{len(unprotected)} accessor(s) answer a size without measuring first:")
    for fn in unprotected:
        print(f"  {fn} does not call sizeFontString")
    print(f"\nevery one of the {len(rows)} sites above is answered for its previous text.")
    raise SystemExit(1)

print("0 accessor(s) answer a size without measuring first: every site above is "
      "measured against the text it holds now.")
