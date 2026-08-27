# Start here

- `AGENTS.md` — this file: what the fork is for, how the client is put together, where things live at runtime, and how to validate a change.
- `TODO.md` — what is known to be wrong and not yet fixed, with the file and line for each and a rough cost. Pick work from here.
- `log.md` — an append-only record of what was changed and why, newest at the bottom. Read the tail to find out what the last few sessions did. Add an entry when you finish a piece of work.

# Fork purpose

This repository is Fabian's maintained WoWee fork. The fork exists to track upstream closely while providing a dependable client for the WoW setup on this machine. Small bugs discovered during ordinary play should be diagnosed, fixed here, tested, and kept as focused commits. The locally installed client should be built from this fork rather than replaced blindly by an upstream release.

## Repository and remotes

- Local checkout: `~/code/WoWee`
- `origin`: `https://github.com/fabge/WoWee.git` — Fabian's fork and the writable remote
- `upstream`: `https://github.com/Kelsidavis/WoWee.git` — canonical project
- `master` is the fork's integration branch. It should contain current upstream plus our validated local fixes.
- Work directly on `master`. This is a personal fork with one author: a topic branch and a merge commit per fix buy nothing here and cost a branch to name, switch to, merge and push. Keep each commit focused and push it when it is validated.
- Do not force-push `master`. Prefer merging current `upstream/master` into it so our local patch history remains stable and conflicts are explicit.
- Commit with `git commit --no-verify`. The pre-commit hooks come from a global config rather than from this repository, and the Python formatter among them rewrites whole upstream files: a two-line change to `tools/sweep_guard.py` came back as a two-thousand-line reformat, which is exactly the divergence from upstream this fork is trying not to accumulate. What is given up is a private-key scan and a large-file check — `.gitignore` already covers `login.cfg` and the asset archives, so keep it that way and never `git add -f` either.

A routine upstream refresh is:

```bash
git fetch upstream --prune
git switch master
git merge upstream/master
cmake --build build-review -j4
ctest --test-dir build-review --output-on-failure -j4
git push origin master
```

`-j4` rather than every logical core, deliberately. This is the machine the
client is played on, and a full `-j8` rebuild makes it noticeably slow to use.
Build the narrowest target that answers the question - `cmake --build
build-review --target test_x` - rather than the whole tree, batch edits and
validate once at the end, and only run the release build and installer when
there is something to actually play. Say when a check is being skipped and why;
skipping quietly is the thing to avoid, not skipping.

Resolve conflicts by preserving the current upstream architecture and reapplying only behavior our fork still needs. If upstream independently fixes one of our patches, drop the redundant local change rather than preserving it for historical reasons.

# Local WoWee installation

The intended steady state is that `/Applications/Wowee.app` is built and installed from this fork's `master`. Treat the installed app as a deployment artifact, not as source and not as a place to patch files manually.

Before replacing the installed app:

1. Confirm WoWee is not running.
2. Fetch and integrate upstream.
3. Build and run the full test suite.
4. Build a macOS app bundle from the fork.
5. Replace the installed app in place. Do not keep a backup copy of the previous bundle: the app carries no state worth recovering, an upstream release can be downloaded at any time, and any earlier build can be rebuilt from this fork's history. Delete any `Wowee.app.backup-*` left behind by an older upgrade.
6. Verify the bundle architecture and signature state and inspect the new runtime log.

Steps 3 to 6 are scripted, and the scripts are the record of how it is done:

```bash
tools/validate.sh
tools/macos/configure_release.sh          # release tree, Homebrew prefixes and all
tools/macos/make_app.sh                   # dist/Wowee.app, ad-hoc signed and verified
tools/macos/install_app.sh                # /Applications/Wowee.app, data checked untouched
```

`install_app.sh` refuses to replace a running client and fails if the extracted data directory changes underneath it.

Never delete or re-extract game assets during an application upgrade unless extraction itself is the task. The approximately 18 GB of extracted data lives outside the app at `~/Library/Application Support/Wowee/Data` and must remain untouched. User configuration lives in `~/.wowee`.

A locally built app may be ad-hoc signed rather than Developer ID notarized. Do not claim that a local build is notarized. Do not copy files into a signed app after signing it, and do not allow normal runtime writes inside `Wowee.app`.

# Local runtime layout

Our fork deliberately keeps mutable state outside the macOS application bundle:

- Game data: `~/Library/Application Support/Wowee/Data`
- Configuration and per-character state: `~/.wowee`
- Main log: `~/Library/Logs/Wowee/wowee.log`
- Runtime caches: `~/Library/Caches/Wowee`

The app changes its working directory to `Contents/Resources` because assets are resolved relatively. That directory is inside the code-signed seal and must be treated as read-only even when Unix permissions technically permit writing. New logs, caches, diagnostics, screenshots, generated state, and ImGui state must use an explicit per-user path.

Do not restore the old `Data/fonts` symlink or disable `blizzard_tokenui`; both were workarounds for WoWee 2.x and are obsolete with the FrameXML-based 3.x client.

# Debugging from a play session

`~/Library/Logs/Wowee/wowee.log` is the fastest route to a cause for anything
reported from the chair, and it is readable directly - there is nothing to set
up. Read it before theorising. On 2026-08-27 a morning went into narrowing "the
objectives tracker's collapse button does not answer a click" through static
analysis and the headless runner, all of which said the button was fine; the
answer was two lines of the log the player had already produced:

    WidgetInput: press at (1301.38,581.041) hit WatchFrameCollapseExpandButton
    WidgetInput: release on WatchFrameCollapseExpandButton - the frame is disabled

The click was never the problem. **A chain that checks out statically can still
fail at runtime, and the log is how you find out which link.**

**Read it yourself. Do not ask the player to send it, or to paste anything out
of it** - it is a file on this machine at a known path, and asking for it turns
a two-second read into a round trip. The only thing worth asking for is a
*session*: "play, do the thing that goes wrong, then quit" - and after that the
log is there to be opened. `~/.wowee/missing_api.txt` sits beside it and records
what the interface asked for that this client does not answer.

Four things to know about it:

- **A release build logs at WARNING and above** (`kDefaultMinLevelValue`, on
  `NDEBUG`), so the installed client's log carries no `LOG_INFO` at all - and
  the installed client is what a bug report comes from. A diagnostic meant to
  be read by a person has to be `LOG_WARNING`. Several useful lines were
  invisible for exactly this reason: the activity sound manager reports its
  chosen voice profile and clip counts, which is the whole answer to "the jump
  grunt is gone", and it was an info line nobody could see. `WOWEE_LOG_LEVEL=info`
  raises it for one run when more is genuinely wanted; do not rely on that for
  a line a player is expected to produce.
- **Truncated on every start** (`std::ios::trunc`), so a log is one session. Ask
  for it before the next launch.
- **Name names, not ids.** A line that says `hit=37` needs a debugger to turn
  back into a frame and is worthless in a report. The widget input lines print
  frame names for this reason.
- **Emit one line per outcome, so silence is itself a signal.** The Escape
  chain in `Application` is the model: every branch says which one ran, so no
  line at all means the key never arrived, which is a different fault in a
  different place. A diagnostic that goes quiet in the state it exists to
  report is worse than none.

The headless counterpart is `framexml_run`, which loads the real FrameXML with
the real emitter and no window:

```bash
cmake -S . -B build-review -DWOWEE_BUILD_FRAMEXML_RUN=ON
cmake --build build-review --target framexml_run -j4
./build-review/bin/framexml_run Data '--lua:__WoweeWarn("x=" .. tostring(x))'
```

`--lua:` runs a chunk (use `__WoweeWarn`, not `print`), `--drawn:NAME` reports
whether a frame is drawn and its resolved rect, `--hit:X,Y` runs the real hit
test, `--mouse:X,Y,L` presses and releases, and `--fire:` raises an event.

`--hit` and `--mouse` take **window pixels from the top-left**, while `--drawn`
reports the widget tree's own space: origin bottom-left, y upward, and divided
by the UI scale. Converting a rect from one to the other is
`winX = treeX * scale` and `winY = 1080 - treeY * scale`, where the runner's
height is 1080 and the scale is `1080 / UIParent:GetHeight()` - typically
1.40625, not 1. Getting this wrong reports "nothing" for a frame that is
plainly there and reads as a broken hit test; it wasted two rounds on
2026-08-27. It answers "is this wired up at all" in seconds. It does not
answer "why does this not happen in the client" - it has no game handler, and
`GameScreen` is handed an empty `UIServices`, so anything reached through an
injected pointer is inert there. Which is the next entry.

## Where bugs actually come from

Counted on 2026-08-27, over nine bugs fixed in one day: **seven came from
Fabian playing**, one came from a sweep - which was catching a regression
introduced the day before - and **none came from the 182 tests**.

That is not a complaint about the net. 182 tests and 129 sweeps are very good
at stopping a thing from breaking twice, which is the job they have. But they
find almost nothing new, so "add more tests" is not the answer to "find bugs";
they are different jobs. The lever is to make each play session yield more, not
to replace the session.

Two things follow. Sweep for the *class* rather than the instance - the stale
measurement fixed on 2026-08-27 was a pattern (`SetText(...)` then `GetHeight()`
on the same object in one function) and a text-level sweep for it would have
found both instances before either was reported. And keep the headless runner
able to reach real state, because most of what goes wrong is a wrong value
rather than a wrong picture: that day's bugs were a boolean (a button left
disabled), a widget missing from a list (every cooldown), a number (a text
height), a float (a creature's facing) and a string that never reached a map
(six settings). None of those needed eyes. They needed the client driven far
enough to reach the state.

**The harness is not a second client.** `framexml_run` is honest because it
links the same libraries the client does, so an answer that differs from the
client's is a bug in the client. Whatever state it is given must stay *data* -
a struct of starting values - and never become a second implementation of game
logic. Writing packet handling or spell rules into the harness is the signal to
stop: at that point it drifts, and it starts producing bugs of its own. There
is deliberately no server simulator and no scripted play-through here.

## Two rules this fork paid for

- **Do not put a side effect behind an optional injected pointer.** A pointer
  that can be null is a side effect that can silently not happen.
  `SettingsPanel::setSettingValue` ended in an unconditional call telling the
  CVar store a setting had moved; routing it through `services_.addonBridge`
  meant it stopped happening in the harness that exists to watch it, and six
  settings reverted on every restart for a day. Cycle-breaking finds the layer
  a symbol belongs in - it does not license moving one behind an interface
  because an interface happens to be nearby.
- **A font string's size is only as fresh as its last measurement.** The layout
  pass measures once a frame and caches against the text it measured, so
  anything that writes text and asks how big it is in the same breath gets the
  previous answer. `ui::sizeFontString` exists to be asked, and the widget
  accessors behind every rect and text getter now ask - but code that reaches a
  `Widget` directly still has to. Two FrameXML layouts do exactly this
  (`WorldMapQuestFrame_UpdateQuests`, `WatchFrame`'s quest handler) and both
  were wrong because of it.

# How the client is put together

Reviewed subsystem by subsystem on 2026-08-25; this is the map, and the parts marked settled should not be redesigned without a reason that names them.

- `src/network` + `src/auth` — framing, SRP, the world socket. Only the socket thread touches the socket: it parses but never dispatches, queueing packets that the main thread drains. That is why nothing in the game layer needs a mutex. **Settled.**
- `src/game` — packet handlers and game state, dispatched through a table rather than a switch, so adding a packet is one registration and one handler. Expansion differences are data: there is no `enum Expansion` anywhere, wire values come from per-expansion JSON with `_extends`/`_remove` inheritance, and handlers switch on expansion-agnostic logical opcodes. Adding an expansion is a directory plus one factory line. **Settled — do not refactor.** `include/game/game_handler.hpp` is nonetheless a god object; see `TODO.md`.
- `src/pipeline` — format parsers (M2, WMO, ADT, BLP, DBC) and the asset manager, with an LRU cache and a worker pool that applies real backpressure. The parsers read untrusted extracted data and have been hardened once already; keep the bounds checks and add a regression test when touching one. Lowest defect density in the repo by an order of magnitude.
- `src/rendering` — Vulkan. Device, swapchain and the frame ring live in `VkContext`; resource lifetime uses two deliberately distinct deferred-destroy paths. `Renderer` then hand-wires 35 subsystems, which is the part that taxes new work.
- `src/addons` + `src/ui` — the interface. Blizzard's real FrameXML is emitted from XML into Lua and run against a C++ widget tree; the Blizzard Lua API surface is implemented in C++. `framexml_takeover` decides, per UI element, whether this client or FrameXML owns it. Highest churn and highest defect density in the codebase — this is where the architectural attention belongs.
- `src/core` — the composition root. Services are hand-wired downward as structs of pointers rather than reached back up through singletons, and the platform seams are real (seven `#ifdef`s in 5,229 lines, all Android lifecycle). Input dispatch has a documented priority chain: focused edit box, then a listening frame, then a key binding, then the per-frame poll.

# Relevant architecture and findings

- WoWee 3.x loads Blizzard's FrameXML as the primary interface. Fix compatibility in the C++ Lua/widget implementation, not by modifying extracted Blizzard Lua or XML files.
- Extracted Blizzard interface data is proprietary local test input. Never commit it, copy it into fixtures, or include it in patches.
- Named values received from a server are untrusted. Never concatenate server-provided character, realm, addon, or asset names into writable filesystem paths without validating that they are a single safe path component.
- `login.cfg` may contain an SRP `H(username:password)` value. This hash is sufficient to authenticate and must be treated as a credential, including owner-only POSIX permissions and exclusion from logs and commits.
- Blizzard Lua API return contracts are exact. `nil`, `false`, `0`, and `""` are not interchangeable. Inspect the calling FrameXML before implementing or changing a binding.
- A Lua error during frame construction can leave a partially built visible interface. Fix the first contract violation rather than hiding the resulting panel.
- Android needs explicit writable config and cache roots; desktop fallback assumptions such as `HOME` or a writable current directory do not apply there.
- The renderer and protocol layers process data from servers and extracted assets. Preserve bounds checks, avoid unchecked size arithmetic, and add a regression test for malformed or truncated input when touching parsers.

# Validation

Once per checkout, link the extracted data and the built helper tools into the
paths the sweeps read:

```bash
tools/link_local_data.sh
```

Without those links 77 of `sweep_guard`'s 92 sweeps skip themselves and the
suite reports a clean run it did not earn — which is how the FrameXML half of
the safety net went missing for as long as it did. `tools/validate.sh` makes
them itself; a bare `ctest` does not.

Use a separate ignored build directory such as `build-review` for local work. The macOS development setup uses Homebrew dependencies and CMake as documented in `BUILD_INSTRUCTIONS.md`.

## How the build is put together

Since 2026-08-26 the client is not one target. It is `src/main.cpp` linked against one STATIC library per `src/` directory — `wowee_core`, `wowee_game`, `wowee_ui`, `wowee_addons`, `wowee_rendering`, `wowee_pipeline`, `wowee_audio`, `wowee_network`, `wowee_auth`, `wowee_math` — plus three targets that are not subsystems:

- `wowee_common` — an INTERFACE target holding the include paths, third-party links, warning flags and precompiled header. Everything that compiles client code inherits it, so there is one answer to "how is this built" rather than one per target.
- `wowee_base` — the logger, the app clock, the memory monitor, the writable-path rules and the CVar store. **It depends on nothing of ours, and that is the property to protect**: anything added there becomes reachable from every subsystem at once.
- `wowee_takeover` — the policy deciding, per UI element, whether this client or FrameXML owns it. Five of the ten libraries consult it.
- `wowee_tables` — `src/game/opcode_table.cpp`, `expansion_profile.cpp`, `character.cpp`. Static tables keyed by expansion or by race, read rather than computed, depending on nothing of ours but the logger, and read by more than one library. None of them belongs to the library it sat in.
- `wowee_window` — `src/core/window.cpp`. The SDL window and the Vulkan surface on it. Depends on `wowee_rendering`, so it is linked only into `core` and `ui`; `src/rendering` references no `Window` symbol at all, which is what makes that safe.
- `wowee_appearance` — `src/core/appearance_composer.cpp`, `helm_visual.cpp`. What a character looks like, from their inventory and CharSections. Read by core, rendering and ui, and none of it is composition-root work.
- `wowee_crypto` — `src/auth/crypto.cpp`, `rc4.cpp`, `vanilla_crypt.cpp`. SHA-1/HMAC, RC4 and the vanilla header cipher. `src/auth` needs them for SRP, `src/network` for every world packet header; a cipher is not a login handshake. `big_num` stays in `src/auth` — it is SRP's arithmetic.
- `wowee_platform` — `src/core/input.cpp` and, on macOS, `macos_platform.mm`. SDL key and mouse state for this frame, and the platform's own bits beside it. Depends on nothing of ours; in `wowee_core` it made a device abstraction look like a dependency on the composition root.
- `wowee_zones` — `src/game/zone_manager.cpp`. AreaTable.dbc and the zone music tables. Depends on `wowee_pipeline` and on nothing in `src/game`, and is read by audio, rendering, game and ui, so it is linked into those four rather than all ten.
- `wowee_openformat` — the `src/pipeline/wowee_*.cpp` writers that turn extracted Blizzard data into this project's own formats. `EXCLUDE_FROM_ALL`, and only `wowee_editor` links it: the client reads what they produce and never calls them, and compiling them into it was 142 objects that contributed no symbol.

**A test should link a subsystem library, not re-enumerate translation units.** `target_link_libraries(test_x PRIVATE wowee_game)` and let the linker work out what it needs; the older targets in `tests/CMakeLists.txt` name `.cpp` files by hand because there was nothing to link, and they go stale. `wowee_common` carries glm, so a target that links a subsystem needs no `wowee_test_link_glm()` call.

The subsystem libraries are still *declared* as a cycle, but the symbol graph is not one any more — **0 mutual pairs as of 2026-08-27, down from 18**. Measure with `tools/library_cycle_check.py <build-dir>` rather than by hand. Replacing the declaration with a hand-written edge list is now possible and is the next step, but it is a real decision rather than a tidy-up: the measurement is macOS-only, a missing edge is a link failure on GNU ld, and CI builds five platforms. Until then CMake repeats the connected component, which is where the duplicate-library warning comes from.

Minimum validation before pushing a change:

```bash
tools/validate.sh
```

That builds, runs CTest, checks whitespace, and — on this machine, where the extracted WotLK interface exists — runs both `framexml_compile_check` trees and the frame-emitted check against it, reporting one summary.

## What each step costs, and when to pay it

Measured on this machine, 2026-08-25:

| Step | Time |
| --- | --- |
| Incremental build after an edit | 4s to ~1 min, depending on the file |
| `tools/validate.sh --quick` (186 tests, no `sweep_guard`) | ~15s |
| `sweep_guard` alone (92 sweeps, run in parallel) | ~3 min |
| `tools/validate.sh` (everything) | ~4 min |
| Release configure and build from scratch | ~5 min |
| `make_app.sh` plus `install_app.sh` | ~2 min |

`sweep_guard` is 95% of the test time on its own. So:

- While iterating, build the target you changed and run the test that covers it, or `tools/validate.sh --quick`. Seconds, and it catches nearly everything.
- While iterating on a sweep, run that one sweep — `python3 tools/<name>.py` takes a second or two. `sweep_guard` runs all 92 and takes three minutes; running it repeatedly to check one of them is most of how an afternoon disappears.
- Run the full `tools/validate.sh` once, when the change is finished and about to be pushed. Not after every edit.
- The release build, the app bundle and the install are for when the change should reach the installed client — when it is asked for, or when a play session needs it. They are not part of validating a change.
- Do not re-run an expensive step to confirm something an earlier run in the same session already showed.

The current suite has 187 CTest checks. Two FrameXML compilation checks skip in a public checkout because the repository cannot contain Blizzard interface data; `tools/validate.sh` reports the FrameXML arms as skipped there rather than failing. The FrameXML checks must report zero failed compilations, zero unparsed XML files, and zero unbuilt elements. Gameplay, rendering, movement, packet, or FrameXML lifecycle changes also require a manual smoke test against the configured ChromieCraft WotLK server.

Do not dismiss a failing custom sweep as noise. The repository's tools encode many previously observed silent failures. Determine whether a failure is a real regression, missing proprietary test data, a missing optional server checkout, or a harness path assumption before changing a pinned ceiling.

# Fix and release policy

- Prefer small, durable fixes that can plausibly be contributed upstream.
- Include the observed failure mode in comments or tests when the contract is otherwise non-obvious.
- Add a regression test for every pure function, parser edge case, persistence rule, or platform path rule that can be tested headlessly.
- Do not patch symptoms in extracted game files, the installed app bundle, or generated build output.
- Keep local-only diagnostics behind explicit flags and write their output outside the app bundle.
- Record fixes as conventional commits on `master` and push them to `origin` promptly, and add a short entry to `log.md` when a piece of work is done.
- When a review or a play session turns up something that is not being fixed now, write it into `TODO.md` with its file and line rather than leaving it in a session transcript nobody will read again.
- Upstream pull requests should remain focused. Split unrelated runtime-path, security, UI-contract, and gameplay fixes into separate PRs even if they coexist on the fork's integration branch.

Known runtime issue still under investigation: `QuestPOI.lua` can attempt to hide a missing POI button. Do not add a nil guard to Blizzard's Lua or a speculative C++ workaround. Reproduce and identify why the recorded maximum button index can refer to a missing global frame, then fix that lifecycle mismatch with a regression test.
