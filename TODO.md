# Open work

Findings from the architecture review of 2026-08-25, plus anything else worth
picking up. Newest findings are added at the top of their tier; finished items
move to `log.md` rather than being deleted here.

Each entry names the file and line where the problem is, what actually goes
wrong, and roughly what it costs to fix. Where a finding is marked **reported,
not verified**, a reviewer found it by reading and nobody has reproduced it —
confirm it before changing anything.

Read `AGENTS.md` first. It has the fork's purpose, the runtime layout, and the
validation policy, including which checks are cheap and which are not.

---

## Tier 1 — correctness

Bounded, each with a stated failure mode. A regression test is expected with
each fix; every one of these is testable headlessly.

Empty. The eight items that stood here on 2026-08-25 and the three that
outlived them - SavedVariables written beside the addon's own source, the crash
handler's fixed `/tmp` path, and quest objective lines that named no creature -
were all fixed on 2026-08-26 and are in `log.md`. The three were still listed
here after they were fixed; they were checked against the source on 2026-08-26
and removed.

## Tier 2 — structural

None of these is urgent. Each taxes every future change in its area.

### Breaking the cycles between the subsystem libraries
`CMakeLists.txt` — the `WOWEE_SUBSYSTEM_LIBS` block

The library targets exist as of 2026-08-26 and the two items that stood here
before them — "no library targets, so subsystems cannot be tested" and
"deleting the link stubs in `test_lua_unit_api.cpp`" — are done and in
`log.md`. What is left is the reason they have to be declared as a cycle.

Every subsystem pair references both ways, so all ten are linked to all ten and
CMake repeats the connected component on the link line. That is correct and it
builds, but it means linking one subsystem offers the linker all of them, and a
new call from any subsystem into any other is invisible rather than a build
error. Measured 2026-08-26, and the asymmetry says where to start:

| edge | forward | back |
| --- | --- | --- |
| `addons` → `game` | 502 | 1 |
| `ui` → `game` | 211 | 3 |
| `rendering` → `pipeline` | 59 | 2 |
| `core` → `ui` | 67 | 30 |
| `core` ↔ `rendering` | 165 | 17 |

The three back-edges of 1, 2 and 3 symbols are almost certainly accidental and
worth deleting on their own merits; that alone turns three cycles into
dependencies and lets those libraries be declared honestly. `core ↔ rendering`
is real and is the same god-object problem as `GameHandler` below.

Do not replace the cycle with a hand-written edge list until the back-edges are
gone: the measurement above is macOS-only, a missing edge is a link failure on
GNU ld, and CI builds five platforms. **~half a day for the small back-edges.**

### 147 translation units the client links for nothing
`CMakeLists.txt` — `WOWEE_SRC_PIPELINE`

Found by the library split on 2026-08-26: with the sources in archives rather
than compiled straight into the executable, 147 of 419 objects contribute no
symbol to the linked client, and nothing anywhere in the client references them.
141 are the `src/pipeline/wowee_*.cpp` open-format writers, which belong to the
`asset_extract` tool; the other six are `vk_buffer.cpp`, `poi_marker_layer.cpp`,
`animation_manager.cpp`, `touch_controls.cpp` and the two `chat_markup_*.cpp`,
whose headers are included but whose out-of-line definitions nothing calls.

Nothing is broken — an archive member is pulled where it is referenced, so the
Android build still gets `touch_controls.o` where `application.cpp` references
it, and the client is now smaller by not linking the rest. The open question is
whether the pipeline writers should move to a target of their own so
`asset_extract` names them and the client does not compile them at all.
**~2 hours, and it needs a look at what `asset_extract` actually wants.**

### A partially-loaded addon still gets no initialisation event
`src/addons/addon_manager.cpp:1330`, `:1399`

Half of this was fixed on 2026-08-26: a failed load-on-demand addon is tracked
in `lodFailed_` and no longer reports success to every caller after the first.
What remains is the addon itself — it gets neither `ADDON_LOADED` nor
`frameXmlNoteAddOnLoaded`, so its frames are on screen, never initialised, and
the takeover safety net still reads it as not loaded.

Firing both on a partial load is one line and probably wrong: `ADDON_LOADED`
tells FrameXML the addon is ready, and for a half-run addon it is not. Decide
whether the honest answer is to fire them anyway, or to tear the partial frames
back down. **~2 hours, and it needs the decision first.**

### `GameHandler` is a god object, and the interfaces are not the fix
`include/game/game_handler.hpp`, `include/game/game_interfaces.hpp`

Still 461 members and 226 `xRef()` accessors after the first pass of
2026-08-26. What that pass established is that the plan recorded here was
wrong, so it is worth writing down before the next one:

`SpellHandler` names **114** distinct members of its owner, and **51** of those
are `xRef()` accessors returning a mutable reference to `GameHandler`'s private
state. Putting an interface in front of that decouples nothing - an interface
whose methods hand out `std::unordered_map<...>&` is `GameHandler` with a
vtable. The five interfaces in `game_interfaces.hpp` are still unused, and they
are not what unblocks this.

What worked instead: of those 51 accessors, **29 were used by
`spell_handler.cpp` and nowhere else**, and 17 of those had no other mention in
`GameHandler` at all beyond the accessor and a line clearing them on character
switch. Those 17 moved, and with them the ten spell-domain types that described
them. No forwarding was left behind.

The next tranche is the same measurement, one step harder:

- **`actionBar`** — 13 non-reset uses in `GameHandler`'s own packet and callback
  code. Genuinely shared, and the wrong thing to move blindly.
- **`hasHomeBind_` / `homeBindMapId_`** (4 each), **`earnedAchievements_`** and
  **`pendingGameObjectInteractGuid_`** (3 each) — each needs its `GameHandler`
  uses read before deciding.
- **14 accessors shared with exactly one other handler** — the pet cluster with
  `CombatHandler`, the skill-line cluster with `InventoryHandler`, the stable
  cluster with `QuestHandler`. Each is a pair of handlers reaching through a
  third object for state one of them should own.

Only once a handler owns its state does an interface over it mean anything.
**~1 day per tranche.**

### The render graph exists but is vestigial
`src/rendering/renderer.cpp:900`, `:3866`

`Renderer` holds 35 hand-wired subsystems and 35 headers declare
`recreatePipelines()` with no common interface, so an MSAA change is a literal
60-line enumeration. `RenderGraph` was built to fix this and registers 5 passes
while the real frame is sequenced imperatively. A new render pass means editing
three lists and forgetting one is silent. **Multi-day.**

### Remove the four Escape probes
`src/core/application.cpp:1548`, `:1579`, `:1616`, `:1632`

They log at WARNING on every Escape press and exist to say which path an Escape
took. They were kept deliberately through the two-pass event pump change of
2026-08-26, which is the fix they were instrumenting: they are the evidence if
that change misbehaves in play. Delete them once a play session has confirmed
it — and not before.

## Tier 3 — upstream

`AGENTS.md` says fixes should be contributable and PRs kept focused. As of
2026-08-25 this fork has 23 local fixes and has opened **zero** pull requests,
while upstream merges outside contributions readily (8 PRs from 6 contributors
in six weeks) and rewrites `src/addons/lua_engine.cpp` — where our patches live
— several hundred times a quarter. Every week they stay local raises the merge
cost.

Suggested order, each its own PR:

1. **Writable-path and credential fixes** — `ad5d005ac`, `7f8602bb1`,
   `80e793c89`, `a96bd0c05`, `86fd81f00`. Self-contained, security-shaped,
   easy to review.
2. **FrameXML contract fixes** — `df4f78823` (unbound keys return `""`),
   `1338a4951` (quest POI ranges), `839f22175` (one key-name vocabulary).
3. **The binding registry** — `86942725e`, `a59277807`, `e8d22c30c`. This one
   supersedes upstream's own PR #124, which hardcoded arrow keys into
   `camera_controller.cpp`; ours routes movement through the binding registry
   instead, so arrows work because `MOVEFORWARD`'s secondary binding is `UP`
   and they are rebindable. It needs the conversation more than the others.
4. **Loot fixes** — `4b793e12e`, `171b09126`.

---

## Smaller things

- `sweep_guard` skips 13 sweeps here: 5 want TBC/Turtle overlay DBCs, 7 want a
  local server checkout (`WOWEE_SERVER_SRC`), 1 wants assets. Pointing
  `WOWEE_SERVER_SRC` at an AzerothCore clone would light up seven
  packet-agreement sweeps that have never run on this machine.
- `docs/threading.md:16`, `docs/plan-modernization.md` and
  `docs/plan-new-mmo.md:19` cite line numbers and sizes that have moved.
  `plan-new-mmo.md` describes `src/ui/` as "~42k, ImGui-based"; it is 31k and
  FrameXML-driven. Stale plans are worse than none.
- `tests/CMakeLists.txt:418` claims the sweeps take "under three seconds". They
  take three minutes.
- No ctest invocation passes `-j`. (The Windows jobs now run their tests: the
  earlier note here said they configured without `-DWOWEE_BUILD_TESTS=ON`,
  which was wrong — the option defaults to ON, so they were building every test
  target and simply never running one.)
- `tools/` has 141 files and no README; the only index is the `CHECKS` list
  inside `sweep_guard.py`.
