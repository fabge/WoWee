# Work log

Append-only. Newest entries at the bottom, so a session that wants the recent
history reads the tail and a session that wants the whole story reads down.

What belongs here: what was changed and, more usefully, *why* — the reasoning
that is not recoverable from the diff. What does not: anything the commit
message already says in full, and anything still open, which belongs in
`TODO.md`.

One entry per session or per coherent piece of work. Keep them short.

---

## 2026-08-25 — the fork begins

Forked `Kelsidavis/WoWee` to `fabge/WoWee`, cloned to `~/code/WoWee`, and wrote
`AGENTS.md`: the fork tracks upstream closely, the locally installed
`/Applications/Wowee.app` is built from this fork rather than replaced by an
upstream release, and small bugs found while playing get fixed here.

Runtime layout settled at the same time, and it is the rule that matters most:
the app bundle is read-only at runtime. Game data lives in
`~/Library/Application Support/Wowee/Data` (~18 GB, never touched by an
upgrade), config in `~/.wowee`, logs in `~/Library/Logs/Wowee`.

## 2026-08-25 — first play-session fixes

Four things reported from actual play, all client implementation gaps rather
than server or Blizzard UI problems:

- Shift-right-click did not reverse auto-loot: the modifier state was never
  passed into the loot request at all.
- Loot rows showed a placeholder number instead of an icon: item metadata
  arrives asynchronously and nothing told FrameXML to redraw when it did, and
  the icon display ID already present in the loot packet was ignored.
- Loot slot numbering shifted when coins or items were removed.
- German QWERTZ keys were recorded as their US positions: SDL and ImGui both
  name the physical ANSI position, so the key printing Z reported Y.

Also fixed a binding contract: unbound keys returned `nil` where Blizzard's
binding UI expects `""`, which could abort a rebinding script.

Merged, installed, and confirmed in play.

## 2026-08-25 — gameplay contract hardening

A pass over what the first fixes exposed. Five commits, branch merged to
`master`:

- Shipped expansion definitions now win over the copy made when the data was
  extracted, which had gone stale — `CreatureModelScale` and `ModelScale` were
  missing, so creature scaling was silently skipped.
- Binding press and release are now a matched pair, tracked by physical key, so
  a modifier released before the key (or a rebind mid-hold) cannot release the
  wrong command.
- Quest POI button ranges kept dense — the `QuestPOI.lua` failure `AGENTS.md`
  had listed as under investigation.
- The client's native controls — movement, action bar, chat, targeting — now
  read the binding registry instead of hardcoded scancodes.
- Interface mouse-button bindings dispatch at all. The binding panel accepted
  them and the stock tables carried them; nothing ever delivered one.

## 2026-08-25 — one key-name vocabulary

The binding panel and the event pump each spelled keys their own way. A binding
is a string, so a key bound in the panel was looked up under a name the pump
never produced: the panel wrote ImGui's debug names (`LEFTARROW`,
`GRAVEACCENT`, `EQUAL`, `KEYPAD0`) — which ImGui's own source says are "not
meant to be saved persistently nor compared" — while the pump answered `LEFT`,
`` ` `` and nothing at all. Every arrow, keypad and punctuation key could be
bound and would never fire.

Both sides now read one table in `include/ui/key_names.hpp`, whose names are
Blizzard's own: the `KEY_*` entries in `GlobalStrings.lua` that
`GetBindingText(key, "KEY_")` looks up. Saved files carry the old spellings, so
the loader checks each name and falls back to the seeded default for one no key
answers to, saying so in the log.

Layout handling split into two questions that were previously tangled:
*identity* is the physical key's Blizzard name and never varies; *label* is
what the keyboard prints, which the platform is asked for. There is no
per-character mapping anywhere — the umlauts fall out of a loop over the same
table.

## 2026-08-25 — local release and validation scripts

The bundle recipe existed only in whatever session had last performed it. Now:
`tools/macos/configure_release.sh`, `make_app.sh`, `install_app.sh`, and
`tools/validate.sh`. `install_app.sh` refuses to replace a running client and
fails if the extracted data directory changes underneath it.

Decided at the same time: no backup of the previous app bundle. It holds no
state, upstream publishes releases, and any earlier build can be rebuilt from
this history.

## 2026-08-25 — three more from play

- Trackpad scrolling zoomed the camera instead of scrolling a panel. A scroll
  frame took the wheel only if something called `EnableMouseWheel` on it, and
  nothing does — `UIPanelScrollFrameTemplate` declares `OnMouseWheel` and stops
  there, and the only four call sites in the entire interface are two chat
  frames and one options panel. Being a scroll frame is what takes the wheel
  now; 81 named scroll frames answer to it.
- Binding `ä` displayed an apostrophe. The binding was always correct
  (`APOSTROPHE`); only the label was, because `GlobalStrings` is one locale's
  file and the extracted interface is enUS.
- Chat buttons jumped sides on the first drag. The underlying fault was wider:
  the widget tree is laid out from the render stage, which runs only while
  `IN_GAME`, and world entry loads the interface *before* returning to it — so
  every `GetLeft`/`GetRight`/`GetWidth` asked while the interface built itself
  answered zero. `FCF_UpdateButtonSide` read zero for both edges and chose
  right. The screen size is seeded before the interface is built now.

Added `IsMouseWheelEnabled`, a real WoW API that was missing, which is what
made the first of these checkable at all.

## 2026-08-25 — build and workflow

A no-op build cost 75 seconds: the opcode registry target rewrote its generated
headers on every build with identical content, and those headers are included
across the packet layer, so every build recompiled a third of the client. It
now costs 4.

Workflow simplified at the same time — work directly on `master` rather than
topic branches, `tools/validate.sh --quick` for iterating (~15s against ~4 min),
and the measured cost of each validation step written into `AGENTS.md` so the
expensive ones get run when they are worth it.

## 2026-08-25 — architecture review

Five parallel subsystem reviews. Verdict: better built than its size suggests.
The protocol layer is genuinely data-driven — there is no `enum Expansion`
anywhere, wire values come from per-expansion JSON, and adding an expansion is
a directory plus one factory line. The threading model is correct by
construction: the socket thread parses but never dispatches, which is why
`GameHandler` needs no mutexes. Do not refactor either.

The biggest finding was in the tooling. `sweep_guard` printed "every sweep at
or under its ceiling" while **77 of its 92 sweeps skipped themselves** — 57
wanted a `Data/interface` path nothing creates, 11 wanted a headless runner
they looked for only in `build/bin`. The FrameXML half of the safety net
`AGENTS.md` describes had not run locally or in CI for as long as those paths
were stale. `tools/link_local_data.sh` makes the links; 69 sweeps run now, in
the same three minutes, because the tool loop was parallelised. Parallelism
immediately exposed that several sweeps drive the client through `framexml_run`
and were reading each other's settings files, so each gets its own config root.

Three defects fixed, each verified independently first:

- Logging out freed the world while `WorldLoader` held a raw pointer taken once
  at construction, behind a null check that a dangling pointer walks through.
  The loader asks the application for the world now rather than remembering one.
- A DBC header declaring more fields than its record size can hold read past
  the record — the truncation check passes, because the file really does hold
  what it says.
- An addon event handler that unregistered itself made the next handler be
  stepped over. The frame dispatch sixty lines below iterates a copy and
  explains why; the fix had never been made in the path beside it.

Everything else went to `TODO.md`.

Open strategic question recorded there too: 23 local fixes, zero upstream PRs,
in the files upstream rewrites most.

## 2026-08-26 — upstream refresh

Merged `upstream/master` (10 commits) with no conflicts. Even in the five files
both sides had touched the hunks were nowhere near each other, which is luck
rather than design: upstream committed 365 changes to `src/addons/lua_engine.cpp`
in the preceding thirty days, so a clean merge is a snapshot and not a property.

One of the ten is worth naming, because it is the other half of a bug we fixed
the same day. Upstream's `37dcad079` normalizes the wheel delta to ±1, since
`hybridscrollframe.lua` tests `delta == 1` and a brisk trackpad flick reports 2
or 3; ours made scroll frames take the wheel at all. Both were needed and they
compose. The next collision may not be so kind, which is the concrete argument
for the upstream PRs in `TODO.md` — the key-name vocabulary especially, since
upstream has no equivalent and has not touched `camera_controller.cpp`,
`keybinding_manager.cpp` or `src/core/input.cpp` since our fork point.

Full `tools/validate.sh`: 180/180, both FrameXML arms zero failures.

## 2026-08-26 — a review's P1s, cleared

An external review of the merged tree. Nineteen findings; the seven marked P1
and five of the P2s were bounded enough to finish, and are below. The rest —
subsystem library targets, the `GameHandler` hub, the vestigial render graph,
`registerCoreAPI` — are the Tier 2 items already in `TODO.md`, restated by
someone who had not read it, which is some evidence they are real.

Two were credential leaks and neither was subtle:

- The 40-byte SRP session key was logged at INFO on every world login, and
  again at DEBUG inside the auth hash inputs. It is the secret the handshake
  proves knowledge of and the header cipher is keyed from, and `wowee.log` is
  the file a bug report attaches. `AGENTS.md` had the rule; the code predated
  anyone applying it.
- `login.cfg` was opened directly, so it was truncated before the first byte
  was written and an interrupted save lost every stored server profile. It is
  written to a temporary and renamed now. In the same function, a failure to
  set owner-only permissions logged a warning and then wrote the password
  hashes anyway; they are omitted instead.

The character name from `SMSG_CHAR_ENUM` went straight into a SavedVariables
filename. `AGENTS.md` names this rule specifically and `core::safeChildPath`
had been in the tree since `a96bd0c05` — it had simply never been applied here,
which is the more useful lesson than the fix.

Leaving the world had two roads and shared none of its teardown. Character
select ran it inline; `/logout` and a disconnect went to the login screen and
so never fired `PLAYER_LEAVING_WORLD`, never wrote SavedVariables, and left the
next login on a Lua state built for the previous character. Now
`leaveWorldSession()`, called from both.

Three malformed-asset guards were each a hardening pass that stopped one call
site short of its twin — the WMO group chunk loop against the root loop
widened above it, the WMO batch ranges against the identical M2 clamp added
after a real device loss, the M2 embedded-skin path against the external skin
path in the same file. All three were "reported, not verified" in `TODO.md` and
all three verified. The M2 clamp is one shared helper now rather than a
convention to remember.

Smaller, same day: `SIGINT`/`SIGTERM` shut down rather than `_Exit`, so a
Ctrl-C keeps its settings; portable config never resolves inside the signed app
bundle; `readSizedString` is `[[nodiscard]]` and its three deaf call sites now
return; and the load-on-demand addons, which were invisible to both persistence
paths, keep their enablement and their SavedVariables — and a failed one no
longer reports success to every caller after the first.

Full `tools/validate.sh`: 180/180, both FrameXML arms zero failures.

## 2026-08-26 — two quest bugs from a play session

Reported as "I cannot complete the quest even though I have the fur", with the
reward panel open and Complete Quest doing nothing.

The log said the button worked: `OnClick ran`, four times, no Lua error. The
handler is Blizzard's `QuestRewardCompleteButton_OnClick`, which tests
`itemChoice` itself and, when no reward has been picked, calls
`QuestChooseRewardError()` *instead of* `GetQuestReward`. Ours was a no-op stub
whose comment reasoned that the message belongs to the server — it does not,
the server is never told anything on that path. So the button genuinely did
nothing at all: no message, no sound, no request. The quest was one unmade
click away from turning in and the client had no way to say so. It raises
`UI_ERROR_MESSAGE` with `ERR_QUEST_MUST_CHOOSE` now.

The second one was in the same screenshot and is the more interesting bug: the
quest log read `Bundle of Furs: 0/1` on a quest it was simultaneously marking
`(Complete)`. `reconcileItemObjectivesFromInventory` is the only thing that
fills `itemCounts` from the bag, and it skipped quests already complete. The
server marks a collect quest complete from its update fields the moment the
item is looted, so the skip engaged before the count was ever taken, and every
collect quest spent its whole life displaying zero.

Neither has a regression test, and the reason is worth recording rather than
glossing: `QuestHandler` cannot be linked into a test without dragging in
`GameHandler` and 59 translation units. The five existing `test_quest_*` targets
all cover header-only pure functions. That is `TODO.md`'s library-targets item
producing a real cost on a real day.

Also noted for `TODO.md`, visible in the same screenshot: kill objectives render
as the literal "Creature slain" with no creature name.

## 2026-08-26 — working through TODO

Tier 1 is now empty.

**Crash log.** The backtrace was appended to a fixed `/tmp/wowee_debug.log` —
world-writable, shared by every user and every concurrent client, and nowhere
near the log a bug report actually arrives with. It sits beside `wowee.log`
now, at mode 0600, with the path resolved once at start-up into a fixed buffer
and written through `open`/`write`: the handler may not allocate, call `getenv`
or use `fprintf`, none of which the old one respected.

**SavedVariables** were written to the addon's own directory, which for a
bundled addon is inside the macOS code-signed seal and for an addon under
extracted game data is proprietary input. They live under the config root now,
and a file left behind by an older build is moved on the way to being read —
without that every player silently loses their addon settings exactly once.

**The interface layer reaches CTest.** `test_lua_unit_api` is the first test to
link a `lua_*_api.cpp` file. The estimate that preceded it was wrong in a way
worth keeping: every binding does guard its handler pointer, so nothing is
*called* — but the linker wants the symbols regardless, 41 of them, 32 being
`GameHandler` methods. Each is a stub in the test. They fail loudly rather than
silently, and one file needing 32 methods of `GameHandler` states the
god-object problem as a number instead of an opinion.

**A frame that fails to build no longer takes the rest of its file.** The
emitter produces one chunk per XML file and ran it whole, so an error building
the fourth frame lost the fifth through the fortieth — the half-built interface
`AGENTS.md` warns about, with nothing to say where it stopped. Each top-level
frame is wrapped in its own `pcall`, reported through `geterrorhandler()`. Safe
because every frame lives in `__w[n]`, a table reached as an upvalue from
inside the closure; there are no top-level locals to lose. The local-limit test
asserted a literal count of one `local` and failed on the new one — rewritten
to assert what it was protecting, that locals do not grow with widget count.

**The world header cipher** was chosen inside `WorldSocket::initEncryption`
from hard-coded build numbers, the one place the data-driven expansion model
leaked into `src/network/`. It is a profile field now, stated explicitly in all
four shipped profiles, with the old build boundaries as the fallback. Turtle is
the case that made a test necessary: realm build 7272, world build 5875, and it
is the world connection being encrypted.

Also from the play session: kill objectives name their creature instead of
reading "Creature slain".

## 2026-08-26 — a second review, and the risky one

An external review against a snapshot from earlier in the day. Four of its
findings were already fixed by the time it arrived — SavedVariables out of the
addon directories, the crash log off `/tmp`, per-frame `pcall` in the emitter,
and the cipher family on the expansion profile — which is a decent argument for
`log.md` existing at all.

Of the rest, the network findings were the serious ones and both were hangs
rather than crashes. `tryParsePackets` stops at a budget, and it was only
re-entered when the tick also read bytes: a burst that buffered more complete
packets than the budget allowed left the rest waiting for a byte that never
came, because the peer was waiting on a response to one of them. And the
`EWOULDBLOCK` retry in `send()` was unbounded while holding `ioMutex_`, so a
peer that stopped reading spun it forever inside the lock and took `disconnect`
and shutdown with it. A send that cannot finish now closes the connection: a
half-written packet has already desynced the stream.

Sixteen parse loops reserved on a server-supplied count before anything
constrained it. The loops always stopped when the bytes ran out — the count
never decided how much was *read*, only how much was *allocated*, which is the
kind of bug that survives review because the loop looks careful.
`Packet::boundedCount` is the shared answer.

The one worth flagging is the event pump. `WantCaptureMouse` is computed inside
`ImGui::NewFrame`, which ran after the pump, so every capture decision in the
pump described the previous frame — the structural cause of a bug class fixed
four times individually. The pump is two passes now: drain SDL and feed ImGui,
start the frame, then dispatch. It is committed alone, because it changes input
routing globally and no test here can see it. The four Escape probes stay until
a play session says it worked; they are the instrumentation for exactly this.

## 2026-08-26 — one library per subsystem

The build listed 419 sources by hand into a single `wowee` target, so a test
could not link a subsystem: it had to re-enumerate the translation units it
needed. That is why `tests/CMakeLists.txt` had grown to 2,145 lines, why
`src/audio/` and `lua_engine.cpp` had no test at all, and why yesterday's
`QuestHandler` fix shipped without a regression test.

There are now ten STATIC libraries, one per `src/` directory, plus a
`wowee_common` INTERFACE target holding the include paths, third-party links,
warning flags and precompiled header that used to hang off `wowee` directly.
Nothing about how the client compiles changed; what changed is that there is
somewhere to inherit it from. The client is those libraries plus `src/main.cpp`.

The libraries are declared as a cycle because that is what the symbol graph is:
every pair references both ways. CMake allows cycles among static libraries and
repeats the connected component, so this is stated rather than pretended away —
with the measurement in `TODO.md`, since the asymmetry (`addons`→`game` 502
symbols against 1 back) says which back-edges are accidental and worth deleting.

Three things fell out of it that were not the point:

`framexml_run` copied four properties off the `wowee` target one at a time, and
the comment there recorded that the fourth was once missed and broke the tool on
any Homebrew prefix. It links `wowee_core` now and there is nothing to keep in
sync. It also inherits `-Werror` for the first time, which found an unused local
in `tools/framexml_run.cpp`.

`macos_platform.mm` was attached to the executable, not to a library, so any
target linking a subsystem without also being the client failed on
`localizedKeyName`. It belongs to `wowee_core`, where its callers are.

147 of 419 objects contribute no symbol to the linked client and nothing
references them — 141 are the `wowee_*.cpp` open-format writers that belong to
`asset_extract`. Nothing is broken, because an archive member is pulled where it
is referenced and platform-gated code still gets its object on that platform.
Written up in `TODO.md` rather than acted on here.

`test_lua_unit_api` carried 41 stub definitions, 32 of them `GameHandler`
methods, because every binding guards its handler pointer so none is ever
called and the linker wants a definition anyway. It links `wowee_addons` now;
the stubs are gone and the assertions are unchanged.

`test_quest_item_reconcile` is new, and is the first CTest target to exercise a
`src/game/` handler rather than a header-only pure function. It covers
yesterday's fix: the server marks a collect quest complete from its update
fields the moment the item is looted, and the reconcile that fills `itemCounts`
from the bag used to skip complete quests — so the log drew "Bundle of Furs:
0/1" on a quest it was simultaneously marking (Complete). Verified by
reintroducing `if (quest.complete) continue;` and watching it fail.

`test_glm_link_check.py` flagged both new targets. Its premise — that a target
reaching glm must call `wowee_test_link_glm()` — now has a second and better
satisfier, since `wowee_common` carries glm to anything linking a subsystem
library, and the macOS build proves the transitive path works. The sweep was
taught the second form rather than the targets given a redundant call, and 12
of its 16 existing helper calls were confirmed still load-bearing first, so the
change adds a satisfier without blinding it. `sweep_guard`'s pinned pattern
followed the wording; the ceiling stays at 0.

182 tests, all passing, every sweep at its ceiling, both FrameXML arms clean.

## 2026-08-26 — registerCoreAPI, split

2,712 lines in one function, now a ten-line dispatcher over eight named
members: base globals, the widget metatable, the widget stub Lua, the frame
globals, addon compatibility, widget support Lua, UI compatibility, and the
addon utility library. Each carries a comment saying what it is for; the order
between them is load-bearing in two places and the call list is now where that
is said, rather than position in a wall of text.

Two things had to move to file scope to make the seam real: the `frameMethods`
table and the `applyFrameMethods` lambda beside it. They are used at two points
either side of a large Lua block - the C methods are put back over it because
that block gives unimplemented methods a no-op and would otherwise silently
replace a working binding - and that only worked while both halves lived in the
same function. The three `lua_EditBox_*` forward declarations moved with them,
and had to go *outside* the anonymous namespace: inside it they were three new
functions with internal linkage and no bodies, which the compiler said plainly.

Verified as pure movement rather than by the suite alone: the old function body
and the new ones were compared line-multiset against each other, and the only
differences are the lambda becoming a function taking `lua_State*`, the array
losing its `static`, and the new comments and signatures. No Lua source line and
no binding registration differs.

The two largest parts are still 811 and 941 lines, but both are single
`bootstrap()` calls holding Lua source. Splitting those further would be cutting
a string literal, not a function.

`lua_widget_api.cpp` is still the missing file, and TODO now says why it costs
more than this entry used to claim: 131 of the 207 widget bindings have internal
linkage and are interleaved with non-widget ones in 16 runs, so the definitions
have to move with the registration. Only five helpers are shared, which is the
part that is better than expected.

182 tests, all passing, every sweep at its ceiling, both FrameXML arms clean.

## 2026-08-26 — lua_widget_api.cpp

The widget surface is its own translation unit: 262 definitions and 4,586 lines
- every Region, Frame, Texture, FontString, EditBox, StatusBar, Cooldown,
Slider, ColorSelect, MessageFrame, ScrollFrame and Tooltip binding, plus the
registration that installs them. `lua_engine.cpp` goes from 10,933 lines to
5,067.

The seam is twelve names in `lua_widget_internal.hpp`. That number is the whole
finding: `widgetIdOf`, `widgetOf`, `engineFrom`, `callScriptOnTable`,
`pcallScript`, `recordScriptError`, `missingApiNames`, and the five globals
`registerBaseGlobals` still installs from the engine side. Everything else is
private to one file or the other.

Getting to twelve took measuring rather than guessing, and the first two
attempts were wrong in instructive ways. Cutting at the frame-metatable
boundary looked like a five-helper seam and was really forty-three, because
`installRegionMethods` binds the whole `lua_Region_*` family onto the texture
and fontstring metatables too - the Region methods are the shared base of every
widget type, and a cut there goes straight through them. The honest unit is all
of it.

The rest was the compiler as an oracle. Overloads collapsed by name produced
both redefinitions and missing overloads; forward declarations stayed behind
while their definitions moved; the registration blocks were not seeding the
dependency closure, so the screen and cursor globals were left orphaned. One
function, `lua_Cooldown_Clear`, had external linkage and no callers - putting
it inside the new file's anonymous namespace turned that into an unused-function
error, so it kept the linkage it had rather than being deleted inside a move.

Verified as movement, not rewrite: the old file and the two new ones compare
equal as line multisets apart from six lines - four `static` prefixes stripped
for external linkage, one forward declaration that moved, and one that went
stale.

Three sweeps then failed, and they were right to. `framexml_method_check` read
`lua_engine.cpp` as the single source of widget registrations and reported 217
widget methods as answered by nothing; `tooltip_setter_check` said 25 tooltip
setters raise; `framexml_presence_test_check` refused to report at all, because
its floor of 100 shared methods caught its own broken scan. All three were
scanning a file the code had left. `framexml_provides` - the one shared answer
to "does the client answer this" - now reads both, and the two direct readers
were given the same treatment plus an existence check, so a moved file fails
loudly instead of reporting a clean zero. Afterwards: 1,462 widget methods
answered and no gaps, 51 of 51 tooltip setters implemented.

182 tests, all passing, every sweep at its ceiling, both FrameXML arms clean.

## 2026-08-26 — SpellHandler owns its own state

TODO said to convert `SpellHandler` to one of the narrow interfaces in
`game_interfaces.hpp` as a proof. Measuring it first showed that was the wrong
move, which is the more useful result.

`SpellHandler` names **114** distinct members of its owner, and **51** of those
are `xRef()` accessors that return a mutable reference to `GameHandler`'s
private state - `spellNameCacheRef()`, `playerSkillsRef()`, `actionBarRef()`.
An interface over that decouples nothing: an interface whose methods hand out
`std::unordered_map<...>&` is `GameHandler` with a vtable in front of it. The
five interfaces stay unused, and they are not what unblocks this.

What the measurement did find: **29 of those 51 accessors are used by
`spell_handler.cpp` and nowhere else**, and **17 of the 29** had no other
mention anywhere in `GameHandler` beyond the accessor itself and a line
clearing them on character switch. That state was in the wrong class outright.

So the 17 moved into `SpellHandler`, and the ten spell-domain types that
describe them moved with them - the callback typedefs, `TotemSlot`,
`SpellModOp`, `SpellModKey`. `GameHandler` keeps a one-line alias for each,
because its own public setters still name them, and `PlayerSkill` moved to
`handler_types.hpp` because `window_manager.cpp` names it too. The two reset
sites moved as well: the character-switch clears are in `resetAllState()`, and
the three DBC loaded-flags are behind a new `resetDbcLoadFlags()`.

`GameHandler`'s public API is unchanged. Sixty call sites in `spell_handler.cpp`
now read a member instead of reaching back through its owner, and seventeen
`xRef()` accessors are gone: 478 member declarations to 461, 243 accessors to
226, and the header is 132 lines shorter.

That is a small fraction of a 5,030-line header, and saying so is the point -
the value here is the measurement, not the seventeen. TODO now carries the next
tranche with the numbers that decide it: `actionBar` has 13 non-reset uses in
`GameHandler`'s own code and is genuinely shared; fourteen more accessors are
shared with exactly one other handler, which is two handlers reaching through a
third object for state one of them should own.

182 tests, all passing, every sweep at its ceiling, both FrameXML arms clean.

## 2026-08-26 — the pet cluster leaves GameHandler

The second tranche, and the clearest case yet of state in the wrong class.
`petActionSlots_`, `petCommand_`, `petReact_`, `petSpellList_`,
`petAutocastSpells_` and `meleeSwingCallback_` had **no mention at all** in any
`game_handler*.cpp`. GameHandler declared them, exposed an `xRef()` for each,
and read them in a getter; the only code that did anything with them was
`SpellHandler` and `CombatHandler`, reaching through their owner to find each
other.

The pet five became one `SpellHandler::PetState`. Grouped rather than moved
loose, so the surface `CombatHandler` shares is a single name instead of five
accessors, and so the collaboration reads as one handler talking to another
rather than as both reaching through a third object. `meleeSwingCallback_` went
to `CombatHandler`, beside the swing timer that raises it, with a
`fireMeleeSwing()` that `SpellHandler`'s instant-attack paths call.

GameHandler's public API is again unchanged - `getPetSpells`, `getPetCommand`,
`getPetReact`, `getPetActionSlot`, `isPetSpellAutocast` and
`setMeleeSwingCallback` all forward. Across both tranches: 478 member
declarations to 454, 243 `xRef()` accessors to 220, the header 5,030 lines to
4,887, and `owner_.` in `spell_handler.cpp` 702 to 632.

One thing found and deliberately not fixed here: none of the pet state is
cleared on a character switch, and GameHandler did not clear it either. The
move preserved that rather than quietly changing it, and it is now the one
entry in Tier 1 - a hunter's pet spell list surviving into a different
character is almost certainly wrong, but it wants reproducing before a
behaviour change rides along inside a refactor.

182 tests, all passing, every sweep at its ceiling, both FrameXML arms clean.

## 2026-08-26 — wowee_base, and the cycle count

Eighteen cycles between the subsystem libraries, now thirteen, from two changes
that were both a file being in the wrong place rather than a call being wrong.

`src/auth`, `src/audio` and `src/pipeline` each reached into `core` for
`core::Logger` and for nothing else of consequence - three back-edges of three,
three and five symbols, of which the logger was all but two. A utility every
layer uses is not a dependency on the composition root; it is the bottom of the
graph. `wowee_base` is the logger, the memory monitor and the writable-path
rules, it depends on nothing of ours, and it has zero out-edges - which is the
property worth keeping, because anything added there becomes reachable from
every subsystem at once.

`stb_image`'s implementation was in `src/rendering/loading_screen.cpp`, which
was simply the first file that needed a PNG decoded. `src/pipeline/asset_manager`
calls `stbi_load` too, so the decoder's only definition sat in a library above
the one using it, and that was the entire `pipeline -> rendering` back-edge:
two symbols against fifty-nine the other way. It has its own translation unit in
`src/pipeline` now, where the format parsers are.

`pipeline` is acyclic outright as a result. The remaining thirteen are in
`TODO.md` ranked by their weakest side, which is the order to take them in.

Two were measured and deliberately left, because neither is the move it looks
like. `storedCVarValue` would kill two cycles by itself, but it checks an
in-memory CVar store before falling back to the file and that store lives with
the Lua CVar API - the file half belongs in `wowee_base`, the store half does
not, and the two answers have to be shown to agree before splitting them, since
`game` and `rendering` call it before the interface exists. `framexml_takeover`
would kill two more and moves easily - 727 lines whose only dependency is the
logger - but it is interface policy rather than a base utility, so where it goes
is a design decision and not a `git mv`.

Getting to zero is not the goal in itself: it is what lets the libraries be
declared with their real edges instead of as a complete graph, and that is what
lets a test link a genuine subset.

182 tests, all passing, every sweep at its ceiling, both FrameXML arms clean.

## 2026-08-26 — a working session, later half

Four things, and the ordering was deliberate: the correctness item first, then
the build graph, then the docs that describe both.

**The pet bug.** `SMSG_PET_SPELLS` is the only thing that has ever populated or
cleared the pet's spell list, autocast set and action bar, and it arrives when a
pet is summoned or dismissed - so a character that has never had a pet is never
sent it, and a hunter's pet survived into the next character logged in.
`GameHandler` zeroed `petGuid_` on the switch, which is what kept the stale pet
off the screen; every other reader still answered from it. Fixed by clearing
`pet_` alongside, and covered by a test that fails without it. It was testable
at all only because the pet state had moved into `SpellHandler` that morning -
which is the argument for the whole decomposition, made concrete.

**Cycles: 18 in the morning, 10 now**, with four libraries acyclic outright.
Every fix was a file in the wrong library rather than a call in the wrong place:
`framexml_takeover` is interface policy consulted by five of the ten libraries
and got its own target; the CVar store moved to `wowee_base` because `src/game`
and `src/rendering` read it during play and were reaching into `src/addons` for
it; the app clock followed the logger. The store moved rather than just the
reader - `storedCVarValue` checks the in-memory map before the file and is
called from the threat update, so leaving the map in `addons` would have traded
a cycle for a file read per call.

What is left is three singletons and a module that is too big to move for one
symbol, and `TODO.md` says which is which. `game -> core` is now literally
`Application::instance`: the game layer reaching into the composition root
through a global, which `AGENTS.md` says is exactly what this codebase does not
do. That is a design change, not a relocation, and it is written down as one.

**`tempEnchantTimers_`** moved to `SpellHandler` - written there, read once by
`InventoryHandler` through the forwarding getter. Across the day `GameHandler`
went from 478 member declarations to 453, 243 `xRef()` accessors to 219, and
5,030 header lines to 4,883.

**The docs.** `AGENTS.md` described a build that no longer exists, and it is the
file every session reads first. It now has the target layout, the rule that a
test links a subsystem library rather than re-enumerating translation units, and
why the libraries are declared as a cycle. The `sweep_guard` comment in
`tests/CMakeLists.txt` claimed three seconds for what takes three minutes and is
95% of the suite; corrected, with the two ways to avoid paying it.

183 tests, all passing, every sweep at its ceiling, both FrameXML arms clean.

## 2026-08-26 — upstream merge, and the cost of the split

Four upstream commits, one conflict, and it was the one predicted at the start
of the day: `src/addons/lua_engine.cpp`. Upstream edited two things in it; we
had moved 4,586 lines of it into `lua_widget_api.cpp` that morning, so git saw
the whole widget block as ours-deleted / theirs-modified.

Resolved by taking our side of the block - the code lives in the new file - and
then landing upstream's change where it now is. Only one of their two hunks
needed relocating: the random-suffix stat naming (`itemStatName` ->
`itemModStatName`, which answers nothing for the five primary stats every animal
suffix rolls) is inside the tooltip builder that moved, while the cursor
world-drop fix stayed in `lua_engine.cpp` and merged on its own.

Before committing the resolution, upstream's version of the conflicted block was
compared line-multiset against our `lua_widget_api.cpp`: twenty lines differed,
of which twelve were their intended change and the rest were section separators
and a doc comment that did not travel. That check is the reason to trust the
resolution - "took ours" on a 2,673-line conflict is otherwise indistinguishable
from dropping an upstream fix on the floor.

This is the convergent-evolution cost the fork was told to expect, priced: one
conflict, one relocated hunk, one verification. It will recur every time
upstream touches the widget half of `lua_engine.cpp`, and it is the argument for
the Tier 3 pull requests rather than against the split.

184 tests, all passing, every sweep at its ceiling, both FrameXML arms clean.

## 2026-08-26 — from play: quest text, and three things behind it

Four reports from a play session, and two of them had the same cause.

**`$n` and `$c` in the quest log.** `GetQuestText` and `GetObjectiveText` - the
panel that offers a quest - have resolved WoW's in-text tokens against the
player since the tokens were implemented. `GetQuestLogQuestText` pushed the raw
strings, so the same quest read "are noble traits, $n" in the log and correctly
in the panel that had just offered it. Both go through `pushQuestText` now.

**"Bundle of Furs: 0/1" on a complete quest, again.** Not a regression: the
installed binary was built at 08:46 and the fix landed at 09:34. The same
screenshot showed `$N` unresolved, which was fixed an hour before it was taken.
Worth writing down as a diagnostic habit - two symptoms that were both fixed
already is a strong signal about the binary rather than the code.

**The bundled addon has never shipped.** `WoweeAllBags` - every bag in one
window, which is exactly what was asked for - lives in `addons/` and
`make_app.sh` has never copied it into the bundle. The dev build gets it from
the `copy_bundled_addons` CMake target, which copies next to the executable, so
the gap was invisible to anyone running out of the build tree. `/allbags` and
`/bags` answered nothing in every installed build since it was written.

**Spell missiles do not travel.** Confirmed rather than fixed:
`playPhysicalProjectile` exists for arrows and bullets, but `SpellVisual.dbc`'s
`MissileModel` is read only as a *fallback path* for the cast and impact models,
never launched from caster to target. So a Lightning Bolt is an impact effect
with nothing in between, which is what was reported. Written into `TODO.md` with
the machinery that already exists to do it.

## 2026-08-26 — the render graph, first increment

`Renderer` rebuilt its subsystems' pipelines from a hand-written enumeration
sixty lines long. A Vulkan pipeline bakes in its render pass and sample count,
so the failure a missing line produces is a pipeline bound to a destroyed pass:
nothing warns, and the driver loses the device a fraction of a second after an
anti-aliasing change. That does not look like a missing line in a list.

`PipelineRegistry` replaces the enumeration with a registration, in insertion
order because two entries depend on earlier ones - the water renderer rebuilds
its single-sampled pass afterwards, the swim effects resync their target pass
first, and both register the whole of what they need so the ordering stays
visible. The registry is refilled on every rebuild rather than once at startup,
because these are created lazily and one that did not exist when the registry
was first filled would never be in it, which is the same silent gap.

The part that matters is `render_pipeline_registry_check.py`: every type under
`include/rendering/` declaring `recreatePipelines()` must be registered, or the
sweep fails. Verified by removing `weather` from the registry and watching it
report. Twenty headers, twenty-one entries, ceiling zero. Comments are stripped
before matching, because `pipeline_registry.hpp` explains the whole mechanism in
its own doc comment and was the first false positive it produced.

This is the first increment, not the item. `RenderGraph` still registers five
passes while the real frame is sequenced imperatively, and the 35 subsystems are
still hand-wired members. What is done is that one of the three lists cannot
silently go stale any more.

184 tests, all passing, every sweep at its ceiling, both FrameXML arms clean.

## 2026-08-27 — spell missiles travel

A Shaman's Lightning Bolt was a glowing ball at the target with nothing between
it and the caster. Reported from play, and the machinery to fix it was already
in the tree.

`SpellVisual.dbc` has a `MissileModel` column - `Spells\LightningBolt_Missile`
for visual 173, `Spells\Frostbolt` for 13 - and this client read it only as a
*fallback path*, used when a spell had neither a CastKit nor an ImpactKit. So
it was drawn standing still, at the caster or at the target, and nothing was
ever launched. For Lightning Bolt it was not drawn at all: it has both kits, so
the fallback never fired and the impact ball was the whole of the effect.

Three things were missing and all three are in:

* `MissileModel` is loaded into its own map rather than as a stand-in for the
  other two. It is no longer a cast fallback - it is not art the caster wears.
* The travel speed comes from `Spell.dbc`'s `Speed`, in yards per second:
  Lightning Bolt 20, Fireball 24, Frostbolt 28, and 0 for everything instant,
  which is how a spell with a missile is told from one without. The column is
  not named in any of the four `dbc_layouts.json` files, so all four now name
  it, and the reader falls back to `RangeIndex + 1` - which is where it sits in
  every expansion this client reads - for a layout copy that predates the name.
* On SMSG_SPELL_GO the missile is launched from the caster's right-hand
  attachment to each hit target, and the impact visual fires when it *arrives*
  rather than the instant the cast completes. A missile that launches suppresses
  the immediate impact; one that does not (no missile model, no speed, target in
  the caster's own position) leaves the old behaviour exactly as it was.

Frostbolt, Fireball and every wand shot are the same gap and the same fix.

`spellMissileDuration` and `spellMissileRotation` are pure, so
`tests/test_spell_missile.cpp` covers the arithmetic without a device: distance
over speed, the clamp at both ends, the zero-speed refusal, and the four
headings. The rest needs a play session to judge.

Along the way the three copies of "read this M2, parse it, upload it, remember
that it failed" became one `acquireEffectModel`. The precast, cast and impact
paths had a copy each, differing only in log wording.

## 2026-08-27 — the client stops compiling the asset pipeline's output side

Found by the library split of 2026-08-26: 147 of the client's 419 objects
contributed no symbol to the linked binary. 142 of them were the
`src/pipeline/wowee_*.cpp` open-format writers - the emitters that turn
extracted Blizzard data into this project's own formats. The client only ever
*reads* what they produce.

They are now `wowee_openformat`, `EXCLUDE_FROM_ALL`, and `wowee_editor` links it
instead of listing all 142 sources a second time. Four stay in `wowee_pipeline`
because the client does read through them - `wowee_model`, `wowee_building`,
`wowee_collision`, `wowee_terrain_loader` - and those are also the ones
`asset_extract` names.

Building the editor for the first time in a while turned up a dead private field
in `tools/editor/editor_ui.hpp` that `-Werror` rejects. Deleted. The editor is an
on-demand target, so nothing local had built it since the field was orphaned.

## 2026-08-27 — five renderer subsystems nobody released, and five that were never anything

`Renderer::shutdown()` exists because `~Renderer` is too late: `Application` says
so where it calls it - a sub-renderer has to free its VMA allocations before
`VkContext::shutdown()` reaches `vmaDestroyAllocator`. The list is hand-written
and was thirty-five entries long. Ten of them were wrong.

`MountDust`, `ChargeEffect`, `QuestMarkerRenderer` and `LevelUpEffect` each free
real Vulkan objects and did it from their destructors alone, so they came down
*after* shutdown had finished rather than inside it. `LightingManager` was never
released at all. It held only because `Application` resets the renderer on the
very next line - one statement away from a use-after-free of the allocator, with
nothing anywhere saying so.

The other five were the opposite fault. `skybox`, `celestial`, `starField`,
`clouds` and `lensFlare` were declared as owning pointers, assigned `nullptr` at
initialize and assigned `nullptr` again at shutdown, and never held anything.
The real objects belong to `SkySystem` and the accessors already went through it.
Deleted.

`renderer_shutdown_check.py` pins this at zero: every `std::unique_ptr` member of
`Renderer` must be `.reset()` in `shutdown()`. Canaried by removing
`zoneManager.reset()` and watching it report.

This is deliberately a sweep over the enumeration and *not* a registry like
`PipelineRegistry`, which is what `TODO.md` proposed. The pipeline rebuild list
had no order to keep and no way to be read from outside, so a registry was the
only way to check it. Teardown is the other way round: the order is load-bearing
and documented in place - SpellVisualSystem before M2Renderer, AnimationController
before the renderers it references - and `x.reset()` is already machine-readable.
A registry would have moved that ordering somewhere less visible and checked
nothing this does not.

## 2026-08-27 — a half-loaded addon gets its initialisation event

The other half of the load-on-demand fix of 2026-08-26. A partially-loaded addon
was tracked as failed and reported as failed, but got neither `ADDON_LOADED` nor
`frameXmlNoteAddOnLoaded` - so its frames were on screen, never initialised, and
the takeover safety net still read it as not loaded and kept drawing this
client's own copy over the top.

Fires both now. Withholding `ADDON_LOADED` reads well - an addon that half-ran is
not ready - but it does not undo the frames the addon already put on screen; it
guarantees they are never wired to anything. `Blizzard_TalentUI` builds its tabs
there and `Blizzard_TimeManager` reads its saved alarm there. It is also what the
real client does: a Lua error in one of an addon's files is reported and the
addon carries on loading. The caller still learns the truth - `loadAddon` returns
false and `loadAddOnByName` turns that into `CORRUPT`.

## 2026-08-27 — tools/ has an index that cannot go stale

126 Python sweeps, eight subdirectories, and the only index of any of it was the
`CHECKS` list inside `sweep_guard.py`, which names about thirty. Someone asking
"is there already a sweep for this?" had `ls` and the file names.

`tools/README.md` now has one, and the table is generated from each tool's own
docstring first line, so a tool whose purpose changes updates its own entry.
`tools_readme_check.py` is pinned at zero in `sweep_guard`: a tool added without
an entry, or an entry whose tool has moved on, fails the build.
`--write` regenerates the block; everything outside the two markers is prose.

A hand-written index would have been worse than none - it would answer correctly
until the next tool was added, and a reader who trusts an index that has quietly
dropped four sweeps writes the fifth copy of one of them.

Also: the three CI `ctest` steps that ran serially now pass `-j`. The suite is a
hundred and eighty small binaries and one three-minute sweep; serially it was the
longest step in two of the jobs. And `docs/plan-new-mmo.md`'s line counts, which
were 2025's, were re-measured - `src/ui/` is 34k and FrameXML-driven, not "~42k,
ImGui-based".
