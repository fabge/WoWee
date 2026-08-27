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
cmake --build build-review --parallel "$(sysctl -n hw.logicalcpu)"
ctest --test-dir build-review --output-on-failure -j "$(sysctl -n hw.logicalcpu)"
git push origin master
```

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
- `wowee_openformat` — the `src/pipeline/wowee_*.cpp` writers that turn extracted Blizzard data into this project's own formats. `EXCLUDE_FROM_ALL`, and only `wowee_editor` links it: the client reads what they produce and never calls them, and compiling them into it was 142 objects that contributed no symbol.

**A test should link a subsystem library, not re-enumerate translation units.** `target_link_libraries(test_x PRIVATE wowee_game)` and let the linker work out what it needs; the older targets in `tests/CMakeLists.txt` name `.cpp` files by hand because there was nothing to link, and they go stale. `wowee_common` carries glm, so a target that links a subsystem needs no `wowee_test_link_glm()` call.

The subsystem libraries are declared as a cycle, because the symbol graph is still one — 10 mutual pairs as of 2026-08-26, down from 18. CMake allows cycles among static libraries and repeats the connected component. `TODO.md` ranks what is left by its weakest side. Do not replace the cycle with a hand-written edge list until they are gone: the measurement is macOS-only, a missing edge is a link failure on GNU ld, and CI builds five platforms.

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
| `tools/validate.sh --quick` (184 tests, no `sweep_guard`) | ~15s |
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

The current suite has 185 CTest checks. Two FrameXML compilation checks skip in a public checkout because the repository cannot contain Blizzard interface data; `tools/validate.sh` reports the FrameXML arms as skipped there rather than failing. The FrameXML checks must report zero failed compilations, zero unparsed XML files, and zero unbuilt elements. Gameplay, rendering, movement, packet, or FrameXML lifecycle changes also require a manual smoke test against the configured ChromieCraft WotLK server.

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
