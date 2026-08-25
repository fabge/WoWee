# Work log

Append-only. Newest entries at the bottom, so a session that wants the recent
history reads the tail and a session that wants the whole story reads down.

What belongs here: what was changed and, more usefully, *why* — the reasoning
that is not recoverable from the diff. What does not: anything the commit
message already says in full, and anything still open, which belongs in
`TODO.md`.

One entry per session or per coherent piece of work. Keep them short.

---

## 2026-08-25 — the fork begins

Forked `Kelsidavis/WoWee` to `fabge/WoWee`, cloned to `~/code/WoWee`, and wrote
`AGENTS.md`: the fork tracks upstream closely, the locally installed
`/Applications/Wowee.app` is built from this fork rather than replaced by an
upstream release, and small bugs found while playing get fixed here.

Runtime layout settled at the same time, and it is the rule that matters most:
the app bundle is read-only at runtime. Game data lives in
`~/Library/Application Support/Wowee/Data` (~18 GB, never touched by an
upgrade), config in `~/.wowee`, logs in `~/Library/Logs/Wowee`.

## 2026-08-25 — first play-session fixes

Four things reported from actual play, all client implementation gaps rather
than server or Blizzard UI problems:

- Shift-right-click did not reverse auto-loot: the modifier state was never
  passed into the loot request at all.
- Loot rows showed a placeholder number instead of an icon: item metadata
  arrives asynchronously and nothing told FrameXML to redraw when it did, and
  the icon display ID already present in the loot packet was ignored.
- Loot slot numbering shifted when coins or items were removed.
- German QWERTZ keys were recorded as their US positions: SDL and ImGui both
  name the physical ANSI position, so the key printing Z reported Y.

Also fixed a binding contract: unbound keys returned `nil` where Blizzard's
binding UI expects `""`, which could abort a rebinding script.

Merged, installed, and confirmed in play.

## 2026-08-25 — gameplay contract hardening

A pass over what the first fixes exposed. Five commits, branch merged to
`master`:

- Shipped expansion definitions now win over the copy made when the data was
  extracted, which had gone stale — `CreatureModelScale` and `ModelScale` were
  missing, so creature scaling was silently skipped.
- Binding press and release are now a matched pair, tracked by physical key, so
  a modifier released before the key (or a rebind mid-hold) cannot release the
  wrong command.
- Quest POI button ranges kept dense — the `QuestPOI.lua` failure `AGENTS.md`
  had listed as under investigation.
- The client's native controls — movement, action bar, chat, targeting — now
  read the binding registry instead of hardcoded scancodes.
- Interface mouse-button bindings dispatch at all. The binding panel accepted
  them and the stock tables carried them; nothing ever delivered one.

## 2026-08-25 — one key-name vocabulary

The binding panel and the event pump each spelled keys their own way. A binding
is a string, so a key bound in the panel was looked up under a name the pump
never produced: the panel wrote ImGui's debug names (`LEFTARROW`,
`GRAVEACCENT`, `EQUAL`, `KEYPAD0`) — which ImGui's own source says are "not
meant to be saved persistently nor compared" — while the pump answered `LEFT`,
`` ` `` and nothing at all. Every arrow, keypad and punctuation key could be
bound and would never fire.

Both sides now read one table in `include/ui/key_names.hpp`, whose names are
Blizzard's own: the `KEY_*` entries in `GlobalStrings.lua` that
`GetBindingText(key, "KEY_")` looks up. Saved files carry the old spellings, so
the loader checks each name and falls back to the seeded default for one no key
answers to, saying so in the log.

Layout handling split into two questions that were previously tangled:
*identity* is the physical key's Blizzard name and never varies; *label* is
what the keyboard prints, which the platform is asked for. There is no
per-character mapping anywhere — the umlauts fall out of a loop over the same
table.

## 2026-08-25 — local release and validation scripts

The bundle recipe existed only in whatever session had last performed it. Now:
`tools/macos/configure_release.sh`, `make_app.sh`, `install_app.sh`, and
`tools/validate.sh`. `install_app.sh` refuses to replace a running client and
fails if the extracted data directory changes underneath it.

Decided at the same time: no backup of the previous app bundle. It holds no
state, upstream publishes releases, and any earlier build can be rebuilt from
this history.

## 2026-08-25 — three more from play

- Trackpad scrolling zoomed the camera instead of scrolling a panel. A scroll
  frame took the wheel only if something called `EnableMouseWheel` on it, and
  nothing does — `UIPanelScrollFrameTemplate` declares `OnMouseWheel` and stops
  there, and the only four call sites in the entire interface are two chat
  frames and one options panel. Being a scroll frame is what takes the wheel
  now; 81 named scroll frames answer to it.
- Binding `ä` displayed an apostrophe. The binding was always correct
  (`APOSTROPHE`); only the label was, because `GlobalStrings` is one locale's
  file and the extracted interface is enUS.
- Chat buttons jumped sides on the first drag. The underlying fault was wider:
  the widget tree is laid out from the render stage, which runs only while
  `IN_GAME`, and world entry loads the interface *before* returning to it — so
  every `GetLeft`/`GetRight`/`GetWidth` asked while the interface built itself
  answered zero. `FCF_UpdateButtonSide` read zero for both edges and chose
  right. The screen size is seeded before the interface is built now.

Added `IsMouseWheelEnabled`, a real WoW API that was missing, which is what
made the first of these checkable at all.

## 2026-08-25 — build and workflow

A no-op build cost 75 seconds: the opcode registry target rewrote its generated
headers on every build with identical content, and those headers are included
across the packet layer, so every build recompiled a third of the client. It
now costs 4.

Workflow simplified at the same time — work directly on `master` rather than
topic branches, `tools/validate.sh --quick` for iterating (~15s against ~4 min),
and the measured cost of each validation step written into `AGENTS.md` so the
expensive ones get run when they are worth it.

## 2026-08-25 — architecture review

Five parallel subsystem reviews. Verdict: better built than its size suggests.
The protocol layer is genuinely data-driven — there is no `enum Expansion`
anywhere, wire values come from per-expansion JSON, and adding an expansion is
a directory plus one factory line. The threading model is correct by
construction: the socket thread parses but never dispatches, which is why
`GameHandler` needs no mutexes. Do not refactor either.

The biggest finding was in the tooling. `sweep_guard` printed "every sweep at
or under its ceiling" while **77 of its 92 sweeps skipped themselves** — 57
wanted a `Data/interface` path nothing creates, 11 wanted a headless runner
they looked for only in `build/bin`. The FrameXML half of the safety net
`AGENTS.md` describes had not run locally or in CI for as long as those paths
were stale. `tools/link_local_data.sh` makes the links; 69 sweeps run now, in
the same three minutes, because the tool loop was parallelised. Parallelism
immediately exposed that several sweeps drive the client through `framexml_run`
and were reading each other's settings files, so each gets its own config root.

Three defects fixed, each verified independently first:

- Logging out freed the world while `WorldLoader` held a raw pointer taken once
  at construction, behind a null check that a dangling pointer walks through.
  The loader asks the application for the world now rather than remembering one.
- A DBC header declaring more fields than its record size can hold read past
  the record — the truncation check passes, because the file really does hold
  what it says.
- An addon event handler that unregistered itself made the next handler be
  stepped over. The frame dispatch sixty lines below iterates a copy and
  explains why; the fix had never been made in the path beside it.

Everything else went to `TODO.md`.

Open strategic question recorded there too: 23 local fixes, zero upstream PRs,
in the files upstream rewrites most.
