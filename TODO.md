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

Three, from the play sessions on 2026-08-27 and 2026-08-28:

1. **`framexml_run` never creates `StaticPopup1` through `4`.** Found on
   2026-08-28 while checking the release-spirit key fix. `StaticPopupTemplate`
   becomes a table, the four concrete frames never appear, and staticpopup.lua's
   own functions - `StaticPopup_Visible`, `StaticPopup_DisplayedFrames` - land
   on the missing-API list, so the file's body evidently does not run in the
   harness even though the XML parses and emits. The client does create them:
   the session log of 2026-08-28 has presses landing on `StaticPopup1Button1`.
   So the harness cannot be used to check anything about a popup - the release
   dialog, the delete-item confirmation, the resurrect prompt - and the check
   for that fix had to be run against a frame built by hand. Worth finding
   before the next popup report, because the harness will answer "no popup"
   for all of them.

2. **`GetBottom`/`GetTop`/`GetLeft`/`GetRight` answer `0` where WoW answers
   `nil`.** Found on 2026-08-27 while chasing the tracker, not the cause of it,
   and real either way. WoW answers nil for a frame whose rect has not been
   calculated; this client answers zero. watchframe.lua:858 branches on exactly
   that - `if ( lastBottom and lastBottom < WatchFrame:GetBottom() )` is the
   tracker's overflow break, and a zero there reads as "below the bottom". It
   was not reproduced: a freshly created frame resolves on demand and answers a
   real number, so the zero needs a frame that cannot resolve - `resolveWidget`
   bails while `layingOut_` is set. Fixing it properly means answering nil only
   where `resolvedGen == 0`, which is WoW's own "not yet calculated" case;
   answering nil more widely would put a nil into the arithmetic these getters
   are read into all over the interface, which the comment above
   `lua_Region_GetLeft` already warns about.

3. **A quest with no objectives is treated as complete.**
   `numObjectives == 0 and playerMoney >= requiredMoney` in
   `WatchFrame_DisplayTrackedQuests`, which is FrameXML's own rule - but it
   means a quest whose objectives this client failed to load is filtered out of
   the tracker by default rather than showing as unfinished. Noticed while
   building the harness fixture on 2026-08-27; no report behind it yet.

The tracker's intermittent self-collapse, listed here on 2026-08-27 and again
on 2026-08-28 ("sometimes it is togglable, sometimes not, and it sometimes
correlates with going into a new area"), was fixed on 2026-08-28: the zone
filter matched quest-log header *text* against `GetRealZoneText()`, which
disagrees for every quest filed under a sub-area. See `log.md`.

"The collapse button does not answer a click", which stood here from
2026-08-27 with a whole morning of narrowing behind it and an ImGui window
named as the prime suspect, went with it - and was never a click problem at
all. The session log of 2026-08-28 shows presses landing squarely on
`WatchFrameCollapseExpandButton` and one of them running its `OnClick`; the
others were refused because the button was *disabled*, by the zone filter
above. The hypothesis was wrong in its first sentence and every hour after
that was spent below it. Worth remembering the next time a chain of reasoning
gets long without a reading in it.

The release-spirit report was fixed the same day and is in `log.md`: bones from
an earlier death carry the owner's guid, so each time they re-entered view they
overwrote the cached corpse position.

The pet-state item found on 2026-08-26 was fixed the same day with a
regression test. The eight that stood here on 2026-08-25 and the three that
outlived them - SavedVariables written beside the addon's own source, the crash
handler's fixed `/tmp` path, and quest objective lines that named no creature -
were all fixed on 2026-08-26 and are in `log.md`. The three were still listed
here after they were fixed; they were checked against the source on 2026-08-26
and removed.

## Tier 2 — structural

None of these is urgent. Each taxes every future change in its area.

### The world map's quest blobs are not drawn, so they cannot be hovered
`src/addons/lua_widget_api.cpp` — the no-op method table

Raised by an external review on 2026-08-28 as "blob hover tooltips cannot
appear", which is true and is not a defect. `WorldMapBlobFrame:UpdateMouseOverTooltip`
is a registered no-op returning nothing, so `worldmapframe.lua` hides
`WorldMapTooltip` — but `DrawQuestBlob` is a no-op in the same table, so there
are no blobs on screen to hover and hiding the tooltip is the consistent answer.
Inventing a hit test would put tooltips over blank map.

What is missing is the feature: FrameXML's blob layer draws a quest's objective
*areas* as filled regions, and this client replaces it with its own POI markers,
which are points. Closing the gap means drawing the blobs (the point lists are
already parsed — see `SMSG_QUEST_POI_QUERY_RESPONSE` in `quest_handler.cpp`) and
then hit-testing them in the blob frame's translated space, which
`WorldMapBlobFrame_CalculateHitTranslations` sets up. Only worth doing if the
POI markers turn out not to be enough in play.

### The taxi precache's tile derivation may be transposed too
`src/core/transport_callback_handler.cpp` — the tile lookup around the waypoint
loop

The same external review flagged this alongside `getTileBounds`, which *was*
transposed and was fixed on 2026-08-28 (see `log.md`). The reviewer withheld the
taxi one pending a direct test, and it was not established, so it was not changed
on suspicion. Settle it the same way the tile-bounds fix was settled: compare the
handler's derivation against `core::coords::canonicalToTile` for waypoints in
different tiles, including negative fractional coordinates. Neighbour padding
would mask a nearby case, so pick coordinates far enough apart that it cannot.

### The subsystem libraries are still declared as a cycle
`CMakeLists.txt` — the `WOWEE_SUBSYSTEM_LIBS` block

**18 mutual pairs on 2026-08-26. Zero now.** `tools/library_cycle_check.py`
measures it; run it against a build tree rather than reasoning about it.

The graph is a DAG, but the *declaration* is still a complete graph, because
nothing has replaced it yet. That is what remains, and it is a decision rather
than a tidy-up:

- The measurement is `nm` on macOS. A hand-written edge list that is right here
  can still be missing an edge that only GNU ld needs, and CI builds five
  platforms - so a wrong list is a broken build for somebody else, discovered
  late.
- The payoff is real: CMake stops repeating the connected component, which is
  the whole of the duplicate-library warning below, and a test can link a
  genuine subset instead of the world.
- The safe order is to write the edge list from the tool's output, keep the
  cycle declaration behind an option for one release, and let all five CI
  platforms build both.

**Half a day, and it wants CI to confirm it rather than this machine.**

Everything that got here came in three shapes, and they are worth keeping
written down because the next graph problem will be one of them:

- **A singleton reached back up through**, fixed by injection.
  `Application::instance` from `src/game`, `src/rendering` and `src/ui`;
  `Input::getInstance` from `src/addons`; `interfaceTakingTypedInput` from
  `src/rendering`.
- **A file in the wrong target.** `wowee_tables`, `wowee_zones`,
  `wowee_platform`, `wowee_crypto`, `wowee_window`, `wowee_appearance`. Each is
  a thing more than one library reads that belonged to none of them. No file
  moved directory; only which target compiles it changed.
- **A concrete type where a question would do.** `ui::AddonBridge` and
  `ui::RenderLocator`: nine calls into `AddonManager` and three into
  `EntitySpawner`, replaced by interfaces `src/ui` declares and the other side
  implements. This is what the unused interfaces in `game_interfaces.hpp` were
  reaching for and never found - the difference is that these were written from
  a measured list of what one library actually asks another, rather than from
  the shape of the class being hidden.

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
- `TerrainManager::collisionTiles_` is written and never read. Every worker that
  finds a WOC sidecar next to an ADT loads it, parses the triangles and stores
  them under the tile key; no code anywhere asks for the map. Either the
  collision consumer was never written or it moved, and until one exists this is
  per-tile I/O and memory for nothing. The insert is now mutex-guarded (it was
  several worker threads into an unguarded `unordered_map`, which is heap
  corruption on a rehash, not a stale read), so it is safe as well as useless.
  Decide which: wire it to the collision query or drop the load.
- `TerrainManager` has no test fixture. It needs an `AssetManager` and three
  renderers before `prepareTile` can be reached, so the map-generation fencing
  added on 2026-08-29 is covered by reading rather than by a test. A
  ThreadSanitizer build plus a map-transition harness is the instrument that
  would actually settle it.
