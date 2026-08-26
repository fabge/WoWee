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

Eight items that stood here on 2026-08-25 were fixed on 2026-08-26 and moved to
`log.md`. What is left is what was not bounded enough to finish in that pass.

### SavedVariables are written beside the addon's own source
`src/addons/addon_manager.cpp:464`

`getSavedVariablesPath` returns `addon.basePath + "/" + name + ".lua.saved"`, so
mutable per-session state is written into the addon directory. For a bundled
addon on macOS that directory is `Wowee.app/Contents/Resources/addons`, inside
the code-signed seal, which `AGENTS.md` forbids writing to; for an addon living
under extracted game data it writes into proprietary input. It works today only
because those writes happen to be permitted.

The fix is not just a path change: existing `.lua.saved` files have to be found
in the old location and moved, or every player silently loses their addon
settings once. Route through `core::getConfigRoot()` with a per-addon
subdirectory, migrating on first read. **~3 hours, migration included.**

### The crash handler writes to a fixed `/tmp` path
`src/main.cpp:69`

`/tmp/wowee_debug.log` is shared by every user on the machine and by every
concurrent client. It is opened with `fopen` from inside a signal handler,
which is already not async-signal-safe, so this wants resolving to a per-user
path captured once at startup rather than being built in the handler.
**~1 hour.**

### Quest objective lines name no creature
`src/addons/lua_quest_api.cpp:982`

`GetQuestLogLeaderBoard` builds kill objectives as the literal `"Creature
slain: 0/10"` — a stock client says `"Bristleback Quilboar slain: 0/10"`. The
objective is unreadable when a quest has three of them, which is exactly when
it matters. The name needs a creature-name cache keyed on the objective's
`npcOrGoId`; the item half of the same function already does the equivalent
lookup through `getItemInfo`. **~3 hours, most of it the cache.**

## Tier 2 — structural

None of these is urgent. Each taxes every future change in its area.

### Input capture decisions are one frame stale
`src/core/application.cpp:1294`, `:1359`, `:1618` vs `src/ui/ui_manager.cpp:367`

The event pump reads `WantCaptureMouse` / `WantCaptureKeyboard` before
`ImGui::NewFrame()` runs, so every claim decision describes the previous frame.
This is the structural cause of the "a key reaches two handlers, or none" class
— four separate instances of it were fixed individually on 2026-08-25. Move
`NewFrame()` ahead of the pump, then delete the four `LOG_WARNING("Escape: …")`
probes, which are that bug being instrumented rather than removed.
**Half a day, and it retires the class.**

### No library targets, so subsystems cannot be tested
`CMakeLists.txt:585`, `:1214`

419 sources are listed by hand into one `wowee` target. A test cannot link a
subsystem; it must re-enumerate the `.cpp` files it needs, which is why
`tests/CMakeLists.txt` is 2,125 lines for 82 executables, and why `src/audio/`
(5.3k lines) and `src/addons/lua_engine.cpp` (10,783 lines) have no test at
all. This is the single biggest drag on adding tests anywhere. **Multi-day.**

Concretely, on 2026-08-26: the fix to
`QuestHandler::reconcileItemObjectivesFromInventory` shipped without a
regression test because covering it means linking `quest_handler.cpp`,
`game_handler.cpp` and the 59 translation units behind them. The five existing
`test_quest_*` targets all test header-only pure functions, which is the shape
of test this build graph permits and the reason the handler logic has none.

### One test seam for the interface layer
`src/addons/lua_unit_api.cpp`

The cheapest first step against the above, and worth doing before it: a
`test_lua_unit_api` target linking that one file plus lua51, calling
`registerUnitLuaAPI(L)` against a null `GameHandler` — every binding already
guards its handler pointer. That opens the highest-churn subsystem in the
codebase to CTest for the first time. **~3 hours.**

### A Lua error mid-file destroys every frame declared after it
`src/ui/framexml_emitter.cpp:1428`, `src/addons/addon_manager.cpp:1273`

The emitter produces one chunk per XML file and runs it whole. This is the
principal source of the half-built interface `AGENTS.md` warns about. Wrapping
each top-level frame in a `pcall` (the `__w` temporaries table is an upvalue and
survives) turns "the rest of the file is gone" into one named failure.
**~2 hours.**

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

### `GameHandler` is a real god object, and the fix is already there
`include/game/game_handler.hpp`, `include/game/game_interfaces.hpp`

961 method declarations, 328 members, five interfaces implemented at once, 59
translation units rebuilding on any edit. The decomposition into Spell /
Inventory / Social handlers is nominal: each holds a concrete `GameHandler&`
back-reference, so the dependency graph is bidirectional. The narrow interfaces
that would cut it exist and are unused. Convert `SpellHandler` first as a proof.
**~1 day for the first one.**

### `registerCoreAPI()` is 2,712 lines in one function
`src/addons/lua_engine.cpp:5222`–`7934`

The `lua_*_api.cpp` split took the game-data domains and stopped; the widget /
region / frame method surface — the largest and most contract-sensitive part —
never moved. Split by table, mechanically, no behaviour change. A
`lua_widget_api.cpp` is the missing file. **~half a day.**

### The render graph exists but is vestigial
`src/rendering/renderer.cpp:900`, `:3866`

`Renderer` holds 35 hand-wired subsystems and 35 headers declare
`recreatePipelines()` with no common interface, so an MSAA change is a literal
60-line enumeration. `RenderGraph` was built to fix this and registers 5 passes
while the real frame is sequenced imperatively. A new render pass means editing
three lists and forgetting one is silent. **Multi-day.**

### The crypto era is chosen in the network layer
`src/network/world_socket.cpp:824`

Hard-coded build numbers rather than a field on the expansion profile — the one
place the otherwise clean data-driven expansion model leaks. Adding an
expansion should not mean editing `src/network/`. **~3 hours.**

---

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
- CI runs less than local: both Windows jobs configure without
  `-DWOWEE_BUILD_TESTS=ON`, so no tests run on Windows at all, and no ctest
  invocation passes `-j`.
- `tools/` has 141 files and no README; the only index is the `CHECKS` list
  inside `sweep_guard.py`.
- `src/core/application.cpp:1956` logs `[FISH-AIM]` at warning level on every
  fishing cast in release builds.
- Two independent frame pacers exist (`application.cpp:1273` and `:1722`); a
  user frame cap above 60 is silently overridden by the second while vsync is
  on.
