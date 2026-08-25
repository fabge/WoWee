# Fork purpose

This repository is Fabian's maintained WoWee fork. The fork exists to track upstream closely while providing a dependable client for the WoW setup on this machine. Small bugs discovered during ordinary play should be diagnosed, fixed here, tested, and kept as focused commits. The locally installed client should be built from this fork rather than replaced blindly by an upstream release.

## Repository and remotes

- Local checkout: `~/code/WoWee`
- `origin`: `https://github.com/fabge/WoWee.git` — Fabian's fork and the writable remote
- `upstream`: `https://github.com/Kelsidavis/WoWee.git` — canonical project
- `master` is the fork's integration branch. It should contain current upstream plus our validated local fixes.
- Work directly on `master`. This is a personal fork with one author: a topic branch and a merge commit per fix buy nothing here and cost a branch to name, switch to, merge and push. Keep each commit focused and push it when it is validated.
- Do not force-push `master`. Prefer merging current `upstream/master` into it so our local patch history remains stable and conflicts are explicit.

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

Use a separate ignored build directory such as `build-review` for local work. The macOS development setup uses Homebrew dependencies and CMake as documented in `BUILD_INSTRUCTIONS.md`.

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
| `tools/validate.sh --quick` (179 tests, no `sweep_guard`) | ~15s |
| `sweep_guard` alone | ~3 min |
| `tools/validate.sh` (everything) | ~4 min |
| Release configure and build from scratch | ~5 min |
| `make_app.sh` plus `install_app.sh` | ~2 min |

`sweep_guard` is 95% of the test time on its own. So:

- While iterating, build the target you changed and run the test that covers it, or `tools/validate.sh --quick`. Seconds, and it catches nearly everything.
- Run the full `tools/validate.sh` once, when the change is finished and about to be pushed. Not after every edit.
- The release build, the app bundle and the install are for when the change should reach the installed client — when it is asked for, or when a play session needs it. They are not part of validating a change.
- Do not re-run an expensive step to confirm something an earlier run in the same session already showed.

The current suite has 180 CTest checks. Two FrameXML compilation checks skip in a public checkout because the repository cannot contain Blizzard interface data; `tools/validate.sh` reports the FrameXML arms as skipped there rather than failing. The FrameXML checks must report zero failed compilations, zero unparsed XML files, and zero unbuilt elements. Gameplay, rendering, movement, packet, or FrameXML lifecycle changes also require a manual smoke test against the configured ChromieCraft WotLK server.

Do not dismiss a failing custom sweep as noise. The repository's tools encode many previously observed silent failures. Determine whether a failure is a real regression, missing proprietary test data, a missing optional server checkout, or a harness path assumption before changing a pinned ceiling.

# Fix and release policy

- Prefer small, durable fixes that can plausibly be contributed upstream.
- Include the observed failure mode in comments or tests when the contract is otherwise non-obvious.
- Add a regression test for every pure function, parser edge case, persistence rule, or platform path rule that can be tested headlessly.
- Do not patch symptoms in extracted game files, the installed app bundle, or generated build output.
- Keep local-only diagnostics behind explicit flags and write their output outside the app bundle.
- Record fixes as conventional commits on `master` and push them to `origin` promptly.
- Upstream pull requests should remain focused. Split unrelated runtime-path, security, UI-contract, and gameplay fixes into separate PRs even if they coexist on the fork's integration branch.

Known runtime issue still under investigation: `QuestPOI.lua` can attempt to hide a missing POI button. Do not add a nil guard to Blizzard's Lua or a speculative C++ workaround. Reproduce and identify why the recorded maximum button index can refer to a missing global frame, then fix that lifecycle mismatch with a regression test.
