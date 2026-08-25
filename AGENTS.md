# Fork purpose

This repository is Fabian's maintained WoWee fork. The fork exists to track upstream closely while providing a dependable client for the WoW setup on this machine. Small bugs discovered during ordinary play should be diagnosed, fixed here, tested, and kept as focused commits. The locally installed client should be built from this fork rather than replaced blindly by an upstream release.

## Repository and remotes

- Local checkout: `~/code/WoWee`
- `origin`: `https://github.com/fabge/WoWee.git` — Fabian's fork and the writable remote
- `upstream`: `https://github.com/Kelsidavis/WoWee.git` — canonical project
- `master` is the fork's integration branch. It should contain current upstream plus our validated local fixes.
- Develop each logical fix on a short-lived topic branch, keep commits focused, then merge it into the fork's `master` after validation.
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
4. Build a macOS arm64 app bundle from the fork.
5. Preserve the previous working app until the new build has launched, logged in, entered the world, and rendered the FrameXML interface successfully.
6. Verify the bundle architecture and signature state and inspect the new runtime log.

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

Minimum validation for every code change:

```bash
cmake --build build-review --parallel "$(sysctl -n hw.logicalcpu)"
ctest --test-dir build-review --output-on-failure -j "$(sysctl -n hw.logicalcpu)"
git diff --check
```

The current suite has 176 CTest checks. Two FrameXML compilation checks skip in a public checkout because the repository cannot contain Blizzard interface data. On this machine, run them manually against the extracted WotLK interface when changing XML emission, templates, widgets, or Lua APIs:

```bash
build-review/bin/framexml_compile_check "$HOME/Library/Application Support/Wowee/Data/expansions/wotlk/interface/framexml"
build-review/bin/framexml_compile_check "$HOME/Library/Application Support/Wowee/Data/expansions/wotlk/interface/addons"
```

Both commands must report zero failed compilations, zero unparsed XML files, and zero unbuilt elements. Gameplay, rendering, movement, packet, or FrameXML lifecycle changes also require a manual smoke test against the configured ChromieCraft WotLK server.

Do not dismiss a failing custom sweep as noise. The repository's tools encode many previously observed silent failures. Determine whether a failure is a real regression, missing proprietary test data, a missing optional server checkout, or a harness path assumption before changing a pinned ceiling.

# Fix and release policy

- Prefer small, durable fixes that can plausibly be contributed upstream.
- Include the observed failure mode in comments or tests when the contract is otherwise non-obvious.
- Add a regression test for every pure function, parser edge case, persistence rule, or platform path rule that can be tested headlessly.
- Do not patch symptoms in extracted game files, the installed app bundle, or generated build output.
- Keep local-only diagnostics behind explicit flags and write their output outside the app bundle.
- Record fixes as conventional commits and push them to `origin` promptly.
- Upstream pull requests should remain focused. Split unrelated runtime-path, security, UI-contract, and gameplay fixes into separate PRs even if they coexist on the fork's integration branch.

Known runtime issue still under investigation: `QuestPOI.lua` can attempt to hide a missing POI button. Do not add a nil guard to Blizzard's Lua or a speculative C++ workaround. Reproduce and identify why the recorded maximum button index can refer to a missing global frame, then fix that lifecycle mismatch with a regression test.
