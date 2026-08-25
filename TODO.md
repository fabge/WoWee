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

### A server-supplied character name reaches a filesystem path
`src/addons/addon_manager.cpp:470` ← set from `src/core/world_loader.cpp:1255`

The name from `SMSG_CHAR_ENUM` is concatenated straight into the SavedVariables
path with no validation, and there is no path-component validator anywhere in
the repo. `AGENTS.md` states this rule explicitly; it is unhonored. Reject
separators, `..`, and empty before `setCharacterName`. **~1 hour.**

### Logging out skips its own session teardown
`src/core/application.cpp:1883` vs `:2258`

All per-session cleanup — `PLAYER_LEAVING_WORLD`, `saveAllSavedVariables()`,
clearing `addonsLoaded_` — lives in the transition to character select. The
disconnect path transitions to the login screen instead, so SavedVariables are
never written and the next session runs on a Lua state built for the previous
character. Extract a `leaveWorldSession()` and call it from both. **~2 hours.**

### Three parser guards that exist and are ignored
`src/network/packet.cpp:120` → `packet_parsers_classic.cpp:999`, `:1033`,
`world_packets_social.cpp:116`

`readSizedString` returns a bool and restores the read position on failure; all
three call sites discard it, so on a truncated packet the following
`readUInt64()` consumes the length field as GUID bytes. Memory-safe, but it
reconstructs the fabricated-name symptom the guard was written to prevent. Mark
it `[[nodiscard]]` and the compiler finds them. **~1 hour.**

### WMO batch ranges reach the GPU unvalidated
`src/pipeline/wmo_loader.cpp:686` → `src/rendering/wmo_renderer.cpp:770`

Index ranges parsed from MOBA go to `vkCmdDrawIndexed` with no check against
the group's index count. `src/rendering/m2_renderer.cpp:1783` has exactly this
guard, added after a documented device loss — same input class, same
consequence, no guard. Mirror the M2 clamp. **~2 hours.**
*Reported, not verified.*

### The WMO group chunk loop can wrap and spin
`src/pipeline/wmo_loader.cpp:438`

`uint32_t chunkEnd = offset + chunkSize;` wraps, passes the `> size` check, and
`offset = chunkEnd` then moves backwards — an unbounded loop on a malformed
group file. The root chunk loop at `:93` was widened to 64-bit with a comment
saying why; the group loop 340 lines later was not. **~1 hour.**
*Reported, not verified.*

### The M2 embedded-skin path is missing the clamp its sibling has
`src/pipeline/m2_loader.cpp:1827` vs `:1955`

`loadSkin` clamps out-of-range batches; the vanilla/TBC embedded-skin path
builds batches identically with no clamp, and `character_renderer.cpp:3064`
draws them raw. Lift the existing clamp into a shared helper. **~1 hour.**
*Reported, not verified.*

### Signals bypass shutdown, and two paths write where they must not
`src/main.cpp:167`, `src/main.cpp:69`, `src/core/config_paths.cpp:97`

`SIGTERM`/`SIGINT` call `std::_Exit(1)`, so every Ctrl-C or OS quit drops
settings, SavedVariables and the WMO floor cache — set `shouldClose` instead,
keeping `_Exit` for a second signal. The crash log appends to a fixed
`/tmp/wowee_debug.log`. Portable config resolves to `<exeDir>/config`, which on
macOS is inside the signed seal — reuse `runningFromAppBundle()` from
`src/core/logger.cpp:60`. **~2 hours for all three.**

### `login.cfg` is rewritten in place
`src/ui/auth_screen.cpp:1084`

Permissions are set correctly before any hash byte is written, and the hash
appears in no log. But the rewrite is not atomic, so an interrupted save
truncates every stored server profile. Write a temp file, chmod it, rename.
**~1 hour.**

---

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

### A partially-loaded addon is marked loaded and left in limbo
`src/addons/addon_manager.cpp:1330`, `:1399`

It gets neither `ADDON_LOADED` nor `frameXmlNoteAddOnLoaded`, so its frames are
on screen, never initialised, never retried, and the takeover safety net still
reads it as not loaded. Fire both even on partial load. **~1 hour.**

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
