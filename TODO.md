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

**18 cycles on 2026-08-26, 10 on 2026-08-27 morning, 2 now.** Measure them with
`tools/library_cycle_check.py <build-dir>` rather than by hand.

Eight of the ten libraries are acyclic outright: `math`, `pipeline`, `audio`,
`network`, `auth`, `rendering`, `game` and `core` are each in at most one pair,
and both remaining pairs are `src/ui`.

| cycle | weak side | what it is |
| --- | --- | --- |
| `ui` → `addons` | **9** | `AddonManager` and `LuaEngine` services |
| `ui` → `core` | 15 | `Application` (5), `Window` (3), `AppearanceComposer` + `helmHidesHair` (3), `localizedKeyName`/`Label` (2), `WorldLoader::mapDisplayName` (1) |

Everything removed so far came in two shapes, and both are exhausted for the
libraries that are now clean:

- **A singleton reached back up through**, fixed by injection.
  `Application::instance` from `src/game` and `src/rendering`,
  `Input::getInstance` from `src/addons`, `interfaceTakingTypedInput` from
  `src/rendering`. Sixteen call sites in `src/rendering` alone were asking the
  composition root for an `AssetManager` or a `GameHandler`; both are now
  handed down through `Renderer`, the way `setAudioCoordinator` already was.
- **A file in the wrong target.** `wowee_tables` (opcode table, expansion
  profiles, race models), `wowee_zones`, `wowee_input`, `wowee_crypto`. Each
  is a thing more than one library reads that belonged to none of them, and
  each depends on nothing of ours beyond the logger.

What is left is neither, and it is why `src/ui` is last:

- **`ui` → `addons`** is nine real service calls - `AddonManager::reload`,
  `setAddonEnabled`, `fireEvent`, `runScript`, `LuaEngine::dispatchSlashCommand`,
  `openInterfaceQuestLog`, `noteClientSettingChanged`, `TocFile::getTitle`.
  The addon control panel and the chat command dispatcher genuinely drive the
  addon system. An interface over what `src/ui` needs from it would work, and
  it is the first honest use for one in this codebase.
- **`ui` → `core`** has three cheap thirds and one expensive one.
  `localizedKeyName`/`Label` belong beside the key state in `wowee_input`;
  `AppearanceComposer` and `helmHidesHair` are character composition and not
  composition-root work; `WorldLoader::mapDisplayName` is a table. Doing all
  three leaves the five `Application` calls -
  `reloadExpansionData`, `setAssetExpansionOverride`, and the three
  `getRender*ForGuid` - plus `Window`'s three display setters, and those want
  the UI to be handed a services struct rather than reach for one. **That is
  the multi-day piece, and it does not pay until it is finished: a pair only
  closes when its last symbol goes.**

The point of reaching zero is that the libraries can then be declared with
their real edges instead of as a complete graph, and a test can link a genuine
subset.

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

**212 `xRef()` accessors** and ~4,880 header lines after three tranches. The
plan originally recorded here was wrong and is worth keeping written down: an
interface whose methods hand out `std::unordered_map<...>&` is `GameHandler`
with a vtable, and the five interfaces in `game_interfaces.hpp` are still
unused.

What works instead is measuring which accessor is used by exactly one other
file, and moving the state there. Measured 2026-08-27:

| sole external user | accessors |
| --- | --- |
| `entity_controller.cpp` | 48 |
| `movement_handler.cpp` | 22 |
| `social_handler.cpp` | 18 |
| `inventory_handler.cpp` | 14 |
| `spell_handler.cpp` | 12 |
| `combat_handler.cpp` | 12 |
| `chat_handler.cpp` | 6 |
| `quest_handler.cpp` | 5 |

Only **one** accessor is used by no other file at all
(`rangedWeaponSwapCallbackRef`), so deleting dead surface is not the lever -
relocation is.

Of `entity_controller.cpp`'s 48, twenty-four have **zero** mentions anywhere in
`GameHandler`'s own translation units: it declares them, exposes an `xRef()`,
reads them in a getter, and never touches them again. That is the exact shape
of the pet cluster, and it is where the next tranche should start.

The seven pet-stat members went that way on 2026-08-27, and finding them found
a bug: nothing cleared them on a character switch, so a hunter's pet attack
power was still readable from a mage. They are in `SpellHandler::PetState` now,
where `resetAllState` already reaches, with a regression test.

Two clusters are left in that 48, and they are not the same:

- **~20 player stat members** - `playerCritPct_`, `playerDodgePct_`,
  `playerMeleeAP_`, `playerXp_`, `playerCombatRatings_` and the rest of the
  character sheet's numbers. Written only by `entity_controller`, read only
  through `GameHandler` getters. Their reset path **was** checked, on
  2026-08-27, and eight of them were not being cleared at all - see `log.md`.
  That is fixed and tested; the relocation itself is still open.
- **12 callbacks** - `creatureSpawnCallback_`, `npcDeathCallback_`,
  `playerSpawnCallback_` and so on. Fired only by `entity_controller`, but
  **set from 15 places across `src/core` and `src/ui`**. Moving them means
  either adding `getEntityController()` and changing all 15, or leaving
  forwarding setters behind - and forwarding is what the earlier tranches went
  out of their way to avoid. `GameHandler` being the registration point for
  these is arguably an interface rather than an accident. Decide before moving.

Also still open: **`actionBar`** (13 non-reset uses in `GameHandler`'s own
packet and callback code - genuinely shared), and `hasHomeBind_`,
`homeBindMapId_`, `earnedAchievements_`, `pendingGameObjectInteractGuid_`,
`stableMasterGuid_`, `stableWindowOpen_`, `achievementNameCache_` and the
skill-line cluster, all of which measure as used only inside `GameHandler`'s own
files plus at most one handler. **~1 day per tranche.**

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

The one with a live cause behind it, added 2026-08-27:

0. **`test_jit_write` cannot see the build it guards.** Upstream's `eb0f0386b`
   fixed a crash that took out every arm64 Mac at login and covered it with a
   case in `tests/test_jit_write.cpp`. That target links only `catch2_main`,
   and `HAVE_UNICORN` is an INTERFACE definition on `wowee_common` - so the
   test compiles as though the build had no Unicorn while every release build
   has one, takes the `SUCCEED` arm, and never runs the assertion. Our
   `tests/test_jit_mapping_agreement.cpp` is the same check in a target that
   does link `wowee_common`, canaried against the 3.1.9 gate. Small, isolated,
   and it makes a fix they have just shipped actually testable. Deferred with
   the rest while PRs are on hold.

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
