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

Empty. The pet-state item found on 2026-08-26 was fixed the same day with a
regression test. The eight that stood here on 2026-08-25 and the three that
outlived them - SavedVariables written beside the addon's own source, the crash
handler's fixed `/tmp` path, and quest objective lines that named no creature -
were all fixed on 2026-08-26 and are in `log.md`. The three were still listed
here after they were fixed; they were checked against the source on 2026-08-26
and removed.

## Tier 2 — structural

None of these is urgent. Each taxes every future change in its area.

### Breaking the cycles between the subsystem libraries
`CMakeLists.txt` — the `WOWEE_SUBSYSTEM_LIBS` block

**18 cycles on 2026-08-26, 10 now**, and four libraries are acyclic outright:
`wowee_base`, `wowee_takeover`, `wowee_math`, `wowee_pipeline`. Every fix so
far was a file in the wrong library, not a call in the wrong place - the
logger, the memory monitor, the writable-path rules, the app clock, the CVar
store, `stb_image`'s implementation, and the takeover policy.

The point of reaching zero is that the libraries can then be declared with
their real edges instead of as a complete graph, and a test can link a genuine
subset. What is left, by weakest side:

| cycle | weak side | what it is |
| --- | --- | --- |
| `game` → `core` | **1** | `core::Application::instance` |
| `rendering` → `ui` | **1** | `ui::interfaceTakingTypedInput` |
| `addons` → `core` | 2 | `core::Input::getInstance`, `setBindingCommandHeld` |
| `network` → `game` | 3 | `OpcodeTable`, `getActiveOpcodeTable` |
| `audio` → `game` | 4 | `ZoneManager` |
| `rendering` → `core` | 7 | |
| `network` → `auth` | 8 | genuinely mutual |
| `rendering` → `game` | 8 | |
| `ui` → `addons` | 9 | |
| `core` → `ui` | 24 | |

The two single-symbol ones are the interesting pair, and neither is a move:

- **`Application::instance`** is `game` reaching into the composition root
  through a global. The fix is injection, not relocation - `AGENTS.md` says
  services are hand-wired downward as structs of pointers rather than reached
  back up through singletons, and this is the exception to that rule.
- **`interfaceTakingTypedInput`** lives in `src/ui/keybinding_manager.cpp`,
  which is far too big to move for one symbol. Either the query belongs in
  `wowee_takeover` beside the rest of the ownership policy, or the camera
  controller should be told rather than asking.

`Input::getInstance` is the same singleton shape as `Application::instance`.
**~half a day for the three singletons, and it is a design change each time.**

### The static-library cycle prints a linker warning on every link
`CMakeLists.txt` — the `WOWEE_SUBSYSTEM_LIBS` block

Apple's ld says `ignoring duplicate libraries: libwowee_addons.a, ...` on every
target that links a subsystem, because CMake repeats the connected component of
a static-library cycle on the link line. Cosmetic, Apple-only, and not an error
- but it is noise in every build log, and this repository does not leave
warnings standing.

Linking the client against `wowee_core` alone silenced it once; adding
`wowee_base` and `wowee_takeover` to each subsystem's PUBLIC link brought it
back, and the test targets that link `wowee_game` or `wowee_addons` show it too.

The real fix is the cycles above: with a DAG, CMake does not repeat anything.
`-Wl,-no_warn_duplicate_libraries` is not accepted by the toolchain here, and
`$<LINK_GROUP:RESCAN,...>` needs `cmake_minimum_required` raised from 3.15 to
3.24, which is a compatibility decision for five CI platforms rather than a
tidy-up. **Do not paper over it; it goes away when the cycles do.**

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

A second tranche went on 2026-08-26: the pet cluster - `petActionSlots_`,
`petCommand_`, `petReact_`, `petSpellList_`, `petAutocastSpells_` - plus
`meleeSwingCallback_`. All six had *no* mention in GameHandler's own `.cpp` at
all; it declared them, exposed an `xRef()`, and read them in a getter. The pet
five became one `SpellHandler::PetState`, and the callback went to
`CombatHandler` beside the swing timer that raises it.

What is left, hardest last:

- **`actionBar`** — 13 non-reset uses in `GameHandler`'s own packet and callback
  code. Genuinely shared, and the wrong thing to move blindly.
- **`hasHomeBind_` / `homeBindMapId_`** (4 uses each), **`earnedAchievements_`**
  and **`pendingGameObjectInteractGuid_`** (3 each) — each needs its
  `GameHandler` uses read before deciding.
- **8 accessors still shared with exactly one other handler**: the skill-line
  cluster with `InventoryHandler` (`skillLineNames_`, `skillLineCategories_`,
  `spellToSkillLine_`, `tempEnchantTimers_`), the stable pair with
  `QuestHandler` (`stableMasterGuid_`, `stableWindowOpen_`), plus
  `lastInteractedGoGuid_` and `achievementNameCache_`. Unlike the pet cluster
  these all have real `GameHandler` uses too, so each is a decision rather than
  a move.

Only once a handler owns its state does an interface over it mean anything.
**~1 day per tranche.**

### The render graph exists but is vestigial
`src/rendering/renderer.cpp`, `src/rendering/render_graph.cpp`

Two of the three lists are dealt with as of 2026-08-27.

Pipeline rebuilds go through `PipelineRegistry`, and
`render_pipeline_registry_check.py` fails the build if a type declaring
`recreatePipelines()` is never registered. That was the failure worth killing
first - a missing entry left a pipeline bound to a destroyed render pass and
cost a lost device, with no warning anywhere.

`shutdown()` is **not** a registry and should not become one. It was checked
instead: `renderer_shutdown_check.py` requires every `std::unique_ptr` member of
`Renderer` to be `.reset()` there. Ten were wrong when the sweep was written -
five never released, five that had never held anything - and all ten are fixed.
The reasoning is in the sweep's own docstring and in `log.md`: teardown order is
load-bearing and documented in place, and `x.reset()` is already
machine-readable, so a registry would have hidden the ordering and checked
nothing the sweep does not.

What is left is the one piece that was always the multi-day one:

- `Renderer` still holds 30 hand-wired subsystem members, referenced through the
  frame rather than through any interface (`m2Renderer` alone appears 66 times),
  so a registry cannot touch most of it.
- `RenderGraph` registers 5 passes while the real frame is sequenced
  imperatively. Until the graph is authoritative it is a second description of
  the frame that nothing checks against the first.

Making the graph authoritative is the work. **Multi-day.**

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
- `docs/threading.md` and `docs/plan-modernization.md` may still cite line
  numbers that have moved; `plan-new-mmo.md`'s were re-measured on 2026-08-27.
