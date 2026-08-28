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

## 2026-08-27 — half the library cycles go

Ten mutual pairs between the subsystem libraries this morning, five now, and the
measurement is a tool rather than an afternoon with `nm`:
`tools/library_cycle_check.py` reads the archives, builds the directed graph and
prints every pair weakest side first. The numbers in `TODO.md` are its output.

The five that went came in two shapes.

**Three singletons reached back up through.** `AGENTS.md` says services are
hand-wired downward as structs of pointers, and these were the exceptions.

* `Application::instance` from `src/game`. Only one call site looked like one -
  `warden_handler.cpp`, fixed by reading `owner_.services().expansionRegistry`,
  which was already there. The rest was `isActiveExpansion` in
  `game_utils.hpp`: an *inline* asking `Application::getInstance()`, so every
  translation unit in four libraries that asked "is this classic?" carried a
  reference to the composition root. It now asks
  `game::getActiveExpansionRegistry()`, set once by `Application` - the same
  shape as `pipeline::setActiveDBCLayout`, which was already doing this.
* `Input::getInstance().setBindingCommandHeld` from `src/addons`, one line in
  `LuaEngine::dispatchResolvedBinding`. Now a `BindingHeldSink` the composition
  root wires.
* `interfaceTakingTypedInput` from `src/rendering`. It moved into
  `wowee_takeover`, which is where "which of the two interfaces owns this"
  already lives, rather than dragging `keybinding_manager.cpp` there for one
  symbol.

**Two files in the wrong target.** `OpcodeTable` is the wire protocol: `src/network`
wants it to name a packet it has just read and `src/game` wants it to build one,
and it belongs to neither. `ZoneManager` reads AreaTable.dbc and the zone music
tables, depends on nothing in `src/game` at all, and is read by `src/audio` and
`src/rendering`. Each is now its own small target - `wowee_opcodes`,
`wowee_zones` - exactly as `wowee_takeover` was carved out. The files stay in
`src/game`; only which target compiles them changed, because moving them would
rename their namespace across thirty call sites for no gain.

`wowee_zones` is linked into the four libraries that read it rather than all ten:
it depends on `wowee_pipeline`, and giving it to `wowee_pipeline` too would
declare a cycle between the two for nothing.

What is left is not more of the same. `rendering -> core` is the camera
controller polling `core::Input`, which wants telling rather than asking;
`ui -> core` at 24 and `ui -> addons` at 9 have never been read through;
`network -> auth` is genuinely mutual crypto. `TODO.md` has the list.

## 2026-08-27 — upstream's arm64 crash fix, and the test that could not see it

Merged `eb0f0386b` from upstream: `pthread_jit_write_protect_np` traps a process
without `com.apple.security.cs.allow-jit` rather than failing, and 3.1.9 gated
the `JitWriteWindow` on arm64 macOS while gating the mapping it guards on macOS
*and* no Unicorn. Release builds have Unicorn, so no MAP_JIT page existed, the
window was pure liability, and the first Warden module the server sent took the
process out. Both gates read `WOWEE_MAP_JIT` now. Reported upstream as #131.

This machine's build has Unicorn (`/opt/homebrew/lib/libunicorn.dylib`), so the
copy installed earlier today carried that crash. Rebuilt and reinstalled.

The merge was clean; the only local conflict surface was `.github/workflows/build.yml`,
where our `-j` on the ctest steps and their entitlements argument sat in
different hunks.

**Upstream's regression test does not reach this build.** `HAVE_UNICORN` is an
INTERFACE compile definition on `wowee_common`, and `test_jit_write` links only
`catch2_main` - so it compiles as though there were no Unicorn while the client
it guards has one. Checked by running it: on this machine its new case takes the
`SUCCEED("the module image is mapped MAP_JIT here")` arm, and
`CHECK_FALSE(JitWriteWindow::required())` - the assertion that actually catches
the crash - never runs.

Adding `wowee_common` to that target is not the fix. The two mechanism cases in
the same file mmap MAP_JIT themselves and need the window compiled in; with the
client's definitions the window compiles away, the `memset` raises SIGBUS and
the binary dies at exit 138. Tried it, and that is what happens.

So `tests/test_jit_mapping_agreement.cpp` is a separate target that does link
`wowee_common` and maps nothing at all. It asks only whether the two gates agree
in the configuration the client actually ships. Canaried by putting the exact
3.1.9 gate back and rebuilding: it fails, while upstream's own file stays green.

186 CTest checks.

## 2026-08-27 — eighteen library cycles down to two

The morning took ten mutual pairs to five. The afternoon took five to two, and
eight of the ten subsystem libraries are now acyclic: `math`, `pipeline`,
`audio`, `network`, `auth`, `rendering`, `game` and `core`. Both survivors are
`src/ui`.

**`rendering → core` is gone, and it was the interesting one.** Six of its seven
symbols were `core::Input`, which is SDL key and mouse state for this frame and
depends on nothing of ours at all - a device abstraction that looked like a
dependency on the composition root purely because its file sat in `src/core`. It
is `wowee_input` now, which also took ten of the twenty-four symbols off
`ui → core`.

The seventh was `Application::instance`, and it was sixteen call sites across
five translation units, nearly all asking for an `AssetManager` or a
`GameHandler`. Both are now handed down through `Renderer` - `setAssetManager`,
`setGameHandler`, wired by `Application` beside the `setAudioCoordinator` call
that already did exactly this. `SpellVisualSystem` and `AnimationController` ask
their renderer; `CharacterPreview` takes the renderer it draws through as an
argument, which every caller already had on the next line to register with;
`EmoteRegistry` holds the asset manager it was given, because most of what reads
it are static lookups with no instance to carry one.

That last change turned up a dead branch. `Renderer::initialize` tried to enrich
the zone manager's music paths from DBC, and `Application` builds the renderer
*before* the asset manager exists - so the pointer it read was null every time
and the branch had never run. The enrich that matters is in `initializeRenderers`,
which is handed one. Removed rather than left as a branch nothing can take.

**`network → auth` is gone.** SHA-1/HMAC, RC4 and the vanilla header cipher are
`wowee_crypto` now. `src/auth` needs them for SRP and `src/network` needs them
for every world packet header; a cipher is not a login handshake and was not
`src/auth`'s to own. `big_num` stays where it is - it is SRP's arithmetic and
nothing outside the handshake asks for it.

**`rendering → game` is gone.** `wowee_opcodes` became `wowee_tables` and took
`expansion_profile.cpp` and `character.cpp` with it: three files with the same
three properties, which is what makes them one target rather than three. Each is
a table keyed by expansion or by race that is read rather than computed, each
depends on nothing of ours beyond the logger, and each is read by more than one
library. Renaming a target that was four hours old cost nothing and was better
than a fourth micro-target.

Every one of these left its file where it was. Moving `zone_manager.cpp` or
`opcode_table.cpp` out of `src/game` would rename its namespace across every
call site for no gain; only which target compiles them has changed.

What is left is neither shape. `ui → addons` is nine real service calls into the
addon system, and it is the first honest use for one of the unused interfaces in
`game_interfaces.hpp`. `ui → core` is fifteen, of which six are a table or
character composition that belongs elsewhere and nine want the UI handed a
services struct rather than reaching for the composition root. `TODO.md` has the
breakdown. Neither pays until it is finished - a pair closes only when its last
symbol goes.

## 2026-08-27 — the other half of the pet-state bug

The pet cluster that moved to `SpellHandler` on 2026-08-26 was the pet's action
bar, stance and spell list. Its stats were left behind, and they had the same
fault: `GameHandler` declared `petStats_`, `petResistances_`, `petAttackPower_`,
`petMinDamage_`, `petMaxDamage_`, `petExperience_` and `petNextLevelExp_`,
exposed an `xRef()` for each, read each in a getter - and mentioned none of them
in any of its own translation units. Nothing cleared them on a character switch,
so a hunter's pet attack power was still readable from a mage and the paperdoll's
pet tab would draw it.

They are fields of `SpellHandler::PetState` now, where `resetAllState` already
zeroes the whole struct, and `entity_controller` writes them through it. The
regression test that covers the first half covers these too.

Found by measuring, not by reading: for every `xRef()` accessor on `GameHandler`,
which files outside its own translation units use it. Only one accessor
(`rangedWeaponSwapCallbackRef`) has no external user at all, so deleting dead
surface is not the lever - 48 of them are used by `entity_controller.cpp` and
nothing else, and twenty-four of those have zero mentions in `GameHandler`'s own
`.cpp` files. `TODO.md` has the table and what is left in it.

Two clusters in that 48 were deliberately *not* moved. The ~20 player stat
members want their reset path checked first - that is how this bug was found.
The 12 spawn and death callbacks are fired only by `entity_controller` but set
from fifteen places across `src/core` and `src/ui`, so moving them means either
changing all fifteen or leaving forwarding setters behind, and forwarding is
what the earlier tranches went out of their way to avoid. `GameHandler` being
the registration point for those is arguably an interface rather than an
accident.

## 2026-08-27 — the same bug, a third time, and the thing that could not be tested

The pet's spells survived a character switch until 2026-08-26. The pet's stats
survived until this morning. This afternoon, asking the same question of every
member `entity_controller` owns turned up **eight more**, all of them the player's
own:

  `chosenTitleBit_`, `shapeshiftFormId_`, `playerHonorPoints_`,
  `playerArenaPoints_`, `playerRestedXp_`, `playerManaRegen_`,
  `playerManaRegenCasting_`, and the sticky-transport pair
  `playerTransportStickyGuid_` / `playerTransportStickyTimer_`.

The shape never changes, and it is worth stating once because it is what makes
these survive an obvious-looking reset: an update field is sent when the server
has a value for it, so **a character who has none of a thing is never told it is
zero**. A mage after a druid kept the druid's shapeshift form. A character with
no title wore the previous one's. The rested XP, honor and arena points on the
screen were somebody else's. The sticky transport is worse than stale - it
deliberately outlives the server's own mention of a boat by a few seconds, so a
leftover guid is a new character riding a transport they are nowhere near.

Two of the eleven that this reported are correct as they are and were left
alone: `characters` is the account's character list, not the character's, and
`updateFieldTable_` is the expansion's field layout.

**Why it kept happening:** the clearing was sixty lines inline in
`selectCharacter`, which ends by sending CMSG_PLAYER_LOGIN down a socket. Nothing
could call it without one, so nothing tested what it cleared, and what it cleared
had to be verified by reading a sixty-line block against a four-thousand-line
header. It is `GameHandler::resetStateForCharacterSwitch()` now, public for that
reason, and `tests/test_character_switch_reset.cpp` covers all three clusters -
the eight new fields, a sample of the ones that were already right so a tidy-up
cannot quietly drop them, and the pet, which lives in `SpellHandler` and is the
wiring the first two faults were on the wrong side of.

Canaried by deleting `shapeshiftFormId_ = 0;` and rebuilding: it reports.

## 2026-08-27 — five more, and a sweep so there is no sixth

Writing the fix above suggested its own check: the reset block tells you what it
clears and cannot tell you what it forgot, so ask the other way round. Every
GameHandler member that `EntityController` writes while unpacking a player's or a
pet's update fields, minus everything
`resetStateForCharacterSwitch` or `handleLoginVerifyWorld` names.

That found **five more**, none of them cleared anywhere:

  `helmVisible_`, `cloakVisible_` - the display switches. Both come home in
  PLAYER_FLAGS, and a character whose flags are all zero has no PLAYER_FLAGS to
  send: hiding a helm on one character hid it on the next one who had never
  touched the switch.
  `isResting_` - the same, for being in an inn.
  `corpseMapId_` - the one field of the corpse the block did not clear, while
  clearing the guid, the position and the valid flag. `canRetrieveCorpse`
  compares it against the map the player is on, so a stale one is a wrong answer
  about somebody else's corpse.
  `repopPending_` - a release-spirit request left in flight. CombatHandler's
  one-second dedupe reads it, so a stale true swallows the new character's first
  release.

`tools/character_switch_reset_check.py` is pinned at zero in `sweep_guard`.
`characters` and `updateFieldTable_` are exempt by name with the reason: the
first is the account's character list and the second the expansion's field-index
table, and neither is per-character.

The sweep's own first draft is the argument for the population line `sweep_guard`
insists on. It anchored the function-body regex on `GameHandler::` and read
`EntityController::applyUnitFieldsOnUpdate` with it, found nothing, and reported
a clean tree. The count of members examined - four, where it should have been
thirty-five - is what showed it. Both numbers are printed for that reason.

## 2026-08-27 — zero cycles

Eighteen mutual pairs between the subsystem libraries yesterday. Two this
afternoon. Zero now, and the symbol graph is a DAG.

The last two were both `src/ui`, and neither was a singleton or a misplaced
file - the two shapes that had accounted for everything up to here. They were a
**concrete type where a question would do**.

**`ui -> addons`** was nine calls into `AddonManager`, `LuaEngine` and two free
functions: run this Lua, raise this event, offer this slash command, reload,
list and toggle addons, open the quest log on a link, tell the CVar store a
setting moved. All real traffic, none of it a reach for the composition root -
so injection fixes nothing. What `src/ui` needs is a *narrower thing* than
AddonManager, which is what an interface is for. `ui::AddonBridge` is declared
in `src/ui`, because `src/ui` owns the requirement, and implemented in
`src/addons`, which already depended on `src/ui` for fifty other symbols.

**`ui -> core`** was fifteen, and it came apart in four pieces:

* `core::Window`'s three display setters. `window.cpp` is `wowee_window` now.
  It depends on `wowee_rendering` for the VkContext it creates, so it goes only
  to `core` and `ui` - and that is safe only because `src/rendering` turned out
  to reference no `Window` symbol at all, which was worth measuring before
  assuming.
* `AppearanceComposer` and `helmHidesHair`: `wowee_appearance`.
* `localizedKeyName`/`Label`: into `wowee_platform`, which is `wowee_input`
  renamed now that it holds `macos_platform.mm` beside the key state whose
  names it spells.
* `WorldLoader::mapDisplayName` and its sibling `mapIdToName`: two switch
  statements over map ids, now `game::mapDisplayName` and `game::mapWdtName` in
  `wowee_tables`, which is exactly the charter that target was written for.

That left six `Application` symbols and thirty-one `getInstance()` calls in
eleven files. Most of it was **finishing a design that was already there**:
`ui::UIServices` says in its own comment that it "replaces
Application::getInstance() calls throughout UI code", and it already carried the
window, the renderer, the asset manager, the game handler and the expansion
registry. The calls simply had not been converted.

Two things needed more than that. The expansion picker calls
`setAssetExpansionOverride` and `reloadExpansionData`, which no service owns; they
are handed down as the two calls they are. And the three `getRender*ForGuid`
queries - where is this guid, how big is it, where are its feet - became
`ui::RenderLocator`, the same shape as the addon bridge, implemented over
`EntitySpawner`.

Five free helpers in `src/ui` have no parameter to receive any of this: an icon
cache that needs the window to upload through, a scene pick that needs render
bounds. They read `ui::uiServices()`, a stored copy of the same struct. That is
a narrower global than the one it replaced and not the absence of one, and the
header says so: `Application::getInstance()` handed out the whole composition
root, this hands out the set `src/ui` was already given, and everything with a
`services_` member or a context struct uses that instead.

**What is left is not a cycle, it is a declaration.** `CMakeLists.txt` still
declares the ten subsystems as a complete graph, which is where the
duplicate-library warning comes from. Replacing it with the real edge list is
now possible; `TODO.md` says why it wants CI rather than this machine to confirm
it.

One thing found on the way and deliberately left: `ChatMarkupRenderer::render`
has no caller. Nothing constructs a `MarkupRenderContext` anywhere - FrameXML's
chat replaced it - so the quest-link handler that pulled `openInterfaceQuestLog`
into the symbol graph is dead code. It is converted along with the rest rather
than deleted, because deleting it is a separate question from this one.

## Five things from a play session

Reported from the chair, which is where the last three of these could only have
come from. Four are fixed, one turned out not to be a fault at all, and one is
still open with a diagnostic pointed at it.

**A bolt that lands beside the wolf.** The missile launched at the target's
last *server* position and flew to it in a straight line at a fixed duration.
Both halves are wrong for anything that is moving, and everything in combat is
moving: the server position is already behind where the target is being drawn,
and the target then walks for the whole second the bolt is in the air. The
missile now carries the target's render instance and re-reads its drawn
position every frame, turning as it goes - `advanceSpellMissile` is that step,
pure and tested, and `duration` demotes from the schedule to a cap so a missile
chasing something faster than itself still expires. `M2Renderer` gained
`setInstanceRotation` and `setInstancePose`; a homing missile has to turn, and
turning refiles the instance in the spatial grid exactly as moving does, so
they are one implementation rather than two.

**No way into chat by slash on a German keyboard.** `OPENCHATSLASH` is bound to
`SLASH`, which names the key right of `PERIOD`. On a German board that key
types `-` and the slash is on shift-7, so the binding could not fire and there
was no key at all that opened chat by slash. It is the *character* that opens
chat, not a key position, so `SDL_TEXTINPUT` carrying `/` - with no edit box
listening - now reports the command pressed. Every layout that can type a slash
reaches it, whatever it has to hold down. `Input::noteBindingCommandPressed` is
the press with no matching release that needs, so nothing is left held.

**A wolf running backwards.** A move arrives with one orientation for the whole
of it: a single bearing for a path that may curve through a dozen waypoints and
turn back on itself, and `updateMovement` never touched facing again. Down the
return leg of a patrol that draws a creature running backwards with the run
cycle playing forwards. `Entity` now turns along the way it actually travelled
this frame, smoothed so a waypoint corner reads as a turn. Three callers say
`holdFacing`: a player's relayed movement (their own client reported that
orientation, and strafing and backpedalling both move one way while facing
another), a passenger on a transport (world-space travel there is the boat's
motion, not theirs), and spline facing types 3 and 4, which name a facing to
hold.

**The Rockbiter icon beside the minimap was the question, not the bug.** That
is `TempEnchant1`, and it belongs exactly where it was - the tooltip in the
report is `TempEnchantButton_OnEnter` doing `SetInventoryItem`, so the screenshot
is the feature working. What was wrong is underneath it: `GetWeaponEnchantInfo`
answered expiration zero, on the reasoning that the frame reads it only to write
a countdown. Zero is not nil in Lua, so `TemporaryEnchantFrame_OnUpdate` wrote
"0" under the icon, then compared that zero against `BUFF_WARNING_TIME` and put
the button into the about-to-expire flash - a thirty-minute imbue pulsing for its
whole life. `SMSG_ITEM_ENCHANT_TIME_UPDATE` carries the real remaining time and
this client already files it per weapon slot, so it is answered now, and nil
where the server has not sent one. Nil is the answer FrameXML is written for:
no countdown, and no flash either.

**The objectives tracker's collapse button is still open.** Every part of it
that can be checked without a screen is correct, and `framexml_run` says so:
the button exists, is enabled, is mouse-enabled, has its `OnClick`, lays out at
16x16, answers `hitTest` at its own centre, and a synthetic press and release
through `dispatchMouse` collapses the tracker - `userCollapsed=true`,
`collapsed=true`, lines hidden. So the failure is upstream of all of it: in a
real session the click is not reaching the widget tree. Two diagnostics that
were already there could not say which: one reported a hit as a widget *id*,
which needs a debugger to turn back into a frame, and the other said a click
was "over this client's own window" without naming the window. Both now name
names, and the second also says which frame the interface would have been
offered - because the reply to that line is always "but there is nothing of
mine there", and the window that disagrees is the answer.

## The settings that reverted on every restart, and why the sweep went quiet

`framexml_settings_control_check` had been reporting six failures, and it was
right about all six: view distance, mouse speed, friendly nameplates, ground
clutter, invert mouse and autoloot were written to `settings.cfg` and never to
the CVar store, which is applied over that file at start-up - so every one of
them was undone by a CVar nobody had touched, on every start.

The cause is `0a7880aa1`, from the day before. `SettingsPanel::setSettingValue`
ended in an unconditional `addons::noteClientSettingChanged(...)`; the cycle
work turned it into `if (services_.addonBridge) services_.addonBridge->
noteSettingChanged(...)`. In the client that pointer is wired, so the client
kept working - and `framexml_run` builds a real settings panel and hands it
`UIServices{}`, every member null. The check written to watch exactly this
promise therefore stopped watching it and started failing, which is the good
outcome of the two available.

**The lesson is not "wire the harness".** It is that the ninth symbol did not
belong on that bridge. Eight of the nine really are questions for the addon
system - run this Lua, dispatch this slash command, list the addons. Telling
the CVar store that a setting moved is a write to a key-value map that lives in
`src/core`, and routing it through an interface implemented in `src/addons`
made a side effect conditional on a pointer that can be null. So
`ClientCVarBinding`, `kClientCVars`, `findClientCVar` and
`noteClientSettingChanged` moved to `core/cvar_store.hpp`, beside the store
they write, and the panel calls it outright again. `cvar_store.hpp` was
already the precedent: its own header says it lives in `wowee_base` because
`src/game` and `src/rendering` reading CVars was the whole of two back-edges.
No new library edge; `AddonBridge` is down to eight methods.

`g_applyingCVarToSetting` moved with it as `core::ApplyingCVarToSetting`, an
RAII guard rather than a bare flag set and cleared around a call.
`settingNumberText` and `settingIsOn` moved out of `ui/settings_schema.hpp`
into `core/setting_text.hpp`, because the store now has to spell a converted
value exactly as the panel would and a third copy is a third thing to disagree;
the schema header names them back into `wowee::ui` so no call site moved.

Two things about the checking are worth keeping. The sweep that caught this
needs the extracted interface, so it skips on every CI runner - the regression
would have reached five platforms unseen. `tests/test_cvar_setting_mirror.cpp`
is the same promise asked of the one function that keeps it, needs nothing but
a config root, and runs everywhere; canaried by making the mirror return early,
which fails three of its four cases. And `posix_only_check` caught the `setenv`
in its first draft, which is the sweep doing its job on the sweep's own fix.

Three sweeps read the rows out of the file as text rather than keeping a copy,
so all three were repointed. `cvar_slider_range_fit` reads both files now: the
table moved and the hand-written `key == "..."` branches beside it did not.

## Text measured against the text it holds now

Two reports, one cause. The world map's quest list drew titles on top of the
objective lines above them, and the objectives tracker's collapse button could
not be clicked - and a play session's log said why the second one outright,
which is what the diagnostics added the session before were for:

    press at (1301.38,581.041) hit WatchFrameCollapseExpandButton
    release on WatchFrameCollapseExpandButton - the frame is disabled

The click was never the problem. The button is `Disable()`d by
`WatchFrame_Update`, in the branch it takes when the objective handlers report
that they laid nothing out. That handler measures what it just drew:
`heightUsed = topEdge - lastLine:GetBottom()`. The world map does the same
thing one line up from its own bug:

    questFrame:SetHeight(max(questFrame.title:GetHeight() +
                             questFrame.objectives:GetHeight() + ...))

Both write text and ask how big it is in the same breath. A font string is
measured by the layout pass, once a frame, and the answer is cached against the
text it measured - so both were told what the *previous* text came to. Three
objectives asked and one line's height answered, so every quest block was built
to fit one line and the next was anchored over the top of it; and the tracker's
handler measured nothing, reported no pixels used, and disabled its own button.
That is also why the tracker sometimes worked: whether the answer was stale
depended on whether that string had changed since the last pass.

`sizeFontString` is the body of that pass lifted out so a script can ask for it,
and `measuredWidgetOf` and `textWidgetOf` - between them every rect and text
getter - now ask before answering. Two things make it work. It has to run
*before* `resolveWidget`, because the resolve places a widget from the sizes it
has. And a re-measure that changes the size clears `resolvedGen`, because the
resolve is skipped for anything already marked done this generation - without
that the string was measured correctly and the getter still read the rect built
from the old text. It also needs a guard the pass never did: a script can ask
at any time, including while FrameXML is still loading, and measuring without a
live ImGui context is a null dereference rather than an unanswered question.
The runner found that one, by segfaulting.

Three smaller things ride along. `applyResolution` keeps the window's centre
instead of its top-left: SDL holds the origin across a resize, and the saved
resolution is applied once the settings load, so a window created centred was
pushed right and down by half the difference at every start. Not in `setSize`,
which is the resize *event* - re-centring there would recentre the window while
it is being dragged.

And two diagnostics for reports that are not yet explained: the jump grunt
declining in silence now says whether the audio engine is up, how many clips
loaded and which voice profile was chosen; and target set/clear are a warning
pair, so "I killed it and cannot deselect it" can be read as either the clear
never running or something setting the target straight back.

## AGENTS.md learns how to read a play session

The log's path was already in `AGENTS.md`; nothing said how to use it, and this
session spent a morning on a question two lines of it answered. So the file now
carries a "Debugging from a play session" section: read the log before
theorising, a chain that checks out statically can still fail at runtime, and
the four properties that decide whether a diagnostic is worth writing - a
release build logs at WARNING and above so `LOG_INFO` never reaches an
installed client's log, the file is truncated per run, a line must name frames
rather than ids, and every branch should say which one ran so that silence is
itself a signal.

`framexml_run`'s flags are written down beside it, including the coordinate
conversion between `--hit`/`--mouse` (window pixels, top-left) and `--drawn`
(tree space, bottom-left, divided by the UI scale). Getting that wrong reports
"nothing" for a frame that is plainly there, which reads as a broken hit test;
it cost two rounds today.

Two rules the fork has now paid for are recorded with it: do not put a side
effect behind an optional injected pointer, and a font string's size is only as
fresh as its last measurement. Both are this week's bugs stated as the rule that
would have prevented them.

The upstream-refresh snippet drops from `--parallel $(sysctl -n hw.logicalcpu)`
to `-j4`, with a note to build the narrowest target and validate once at the
end. This is the machine the client is played on and a full `-j8` rebuild makes
it slow to use - which is a real cost of the validation loop, not a preference.

## Cooldowns were never drawn, because a cooldown frame is a container

Reported as "the cooldown of casts is not shown in the icon, it only shows in
seconds when hovering". Every link checked out: the client fires
ACTIONBAR_UPDATE_COOLDOWN, GetActionCooldown returns a start on GetTime's clock
and drawCooldown measures against appTimeSeconds which is the same clock,
`ActionButton1Cooldown:GetObjectType()` really is "Cooldown", and
CooldownFrame_SetTimer runs without error. The chain was intact and nothing was
drawn.

The draw order is what dropped it. A `Frame` with no backdrop, no status bar,
no edit box and no external texture is a container and is skipped - and
FrameXML declares every action button's cooldown as `<Cooldown
name="$parentCooldown">`, which is exactly that shape. It has something to
paint, its own sweep, and nothing in that test knew. So `drawCooldown` was
never reached for any cooldown in the game, and the only place a cooldown was
visible was the tooltip, which counts seconds down a different path - which is
why it read as a display quirk rather than a missing draw.

Exempt now, gated on a cooldown actually running, which is how the status bar
beside it is written: a bar is exempt when it has a fill, a cooldown when it
has a sweep. `tests/test_cooldown_draw_order.cpp` holds both halves - a running
cooldown is drawn, an ordinary frame is still a container - and was canaried by
removing the exemption. `framexml_run` keeps a second copy of the same rule for
its `--drawn:` explanation and was updated with it; the file's own comment
warns that the rule is written twice.

Found by reading the draw-order filter rather than by another play session,
after a note from the player that the log is on this machine and does not need
sending. That is now in `AGENTS.md`: read it, do not ask for it. The only thing
worth asking for is a session.

## The harness gets an action bar, and the cooldown fix gets a real check

Two additions to `framexml_run --player`, both data on the `GameHandler` it
already builds. Two action bar slots, one ready and one thirty seconds into a
minute, and every quest in the log watched.

The bar was the point. Without an action on it `ActionButton_Update` hides
every button, so nothing about the bar could be asked at all - and the cooldown
sweep that shipped broken for the life of the client was found by reading the
draw-order filter rather than by asking here. It is asked now: with the slots
filled, `ActionButton2Cooldown` reports DRAWN through the real
`ActionButton_UpdateCooldown` path, which is the fix checked end to end rather
than against a frame invented for the test.

Getting there turned up a third thing the harness needed. `CooldownFrame_SetTimer`
takes its start only when `start > 0`, and start is `GetTime() - elapsed`; the
runner asks 0.7 seconds after launch, so any cooldown already part-way through
had a negative start and was hidden. `advanceTestClock(600)` puts the clock ten
minutes in, where every such subtraction lands as it would in play. Worth
knowing generally: a harness that starts at t=0 is not a neutral starting
position, it is an unusual one.

A zone was tried too and taken out rather than shipped: setting
`worldStateZoneId_` does not make `GetRealZoneText()` answer, because the name
comes through AreaTable and the harness does not resolve it. What that exposed
is worth more than the fixture line - see `TODO.md` item 2. The tracker's
default filter drops every watched quest whose log header does not match the
zone text exactly, with no fallback, and a tracker that hides everything is
indistinguishable from a broken one.

## A sweep for the class, not the instance

`framexml_measure_after_settext.py`, the text twin of
`framexml_measure_after_move.py` and the same story told about strings: where
the interface writes text and measures it in the same breath. Twenty-one sites
across the interface, and two of them were today's bugs - the world map's quest
blocks and WatchFrame's quest handler.

It reports the sites as a population rather than as faults, because after the
fix they all answer correctly; what it fails on is the thing they depend on
going away. It reads `measuredWidgetOf` and `textWidgetOf` out of
`lua_widget_api.cpp` and checks that both still ask `sizeFontString` before
answering a size. Canaried by deleting one of those calls, which names the
accessor and says that all twenty-one sites are back to being answered for
their previous text. An empty population also fails: a matcher that has gone
blind reads exactly like a clean tree, which is the rule `sweep_guard` already
enforces on every sweep it runs.

This is the shape worth repeating. Both of today's measurement bugs were
instances of one pattern, and a sweep for the pattern would have found both
before either was reported - where a test for either would have found neither.

## The tracker, narrowed; and a corpse guid that outlived its corpse

The objectives tracker was chased with the fixture rather than with another
play session, and the fixture had to grow twice to get there. Its quest had no
objectives, and watchframe.lua treats a quest with none as *complete* -
`numObjectives == 0 and playerMoney >= requiredMoney` - which the default
filter then drops, so not one line was laid out and every question answered
"nothing was drawn" for a reason unrelated to what was being asked. With two
kill objectives on it and the zone filter satisfied, the handler reports
`heightUsed=44` and the collapse button is enabled.

So the layout is sound, the measurement fix holds, and the click was never the
problem. The fixture then grew to eight watched quests - what the report had -
to reach the handler's overflow break, and that is ruled out as well:
`heightUsed=260`, button enabled, title "Objectives (8)". The harness now
reproduces the reported screen and it works.

One anomaly from the reported log is recorded as a red herring so it is not
chased twice: `WatchFrame` measured 119.59 wide against the harness's 204.
`WatchFrame_SetWidth` only ever chooses 204 or 306, so that is the *collapsed*
width derived from the title - the symptom, not the cause.

What is left is written into `TODO.md` rather than guessed at here. Three times
today a confident static theory has been wrong, and each time the harness or
the log was what said so.

Two fragilities found on the way and recorded with it: a quest with no
objectives reads as complete, and the zone filter drops *every* watched quest
whose log header does not match `GetRealZoneText()` exactly, with no fallback.

Separately, the release-spirit report. `SMSG_DESTROY_OBJECT` never cleared the
tracked corpse, so `corpseGuid_` went on naming an object the server had
already taken away - after a reclaim, a decay, or a later death - and
`reclaimCorpse` would send `CMSG_RECLAIM_CORPSE` for it. Cleared now, and only
the guid: the *position* is what a ghost navigates by, it comes from
`MSG_CORPSE_QUERY` rather than from the object, and a corpse going out of sight
must not erase where it is. The guid returns with the create block when the
ghost walks back into range, so it is self-healing.

Whether that is the whole of "it jumps around" is not established, so all four
places that write the corpse position now name themselves in the log - death,
forced death update, corpse object, and corpse query. A stale one will say
which writer put it there.

## The harness finds one on its first outing

Extending `--player` to eight watched quests and an action bar made
`sweep_guard`'s dialog arm reach a path it never had, and it failed:

    QUEST_COMPLETE: attempt to call global 'debuginfo' (a nil value)

`debuginfo()` is the first line of `_ERRORMESSAGE`, which is FrameXML's own
script-error handler. Undefined, it meant every script error raised a *second*
error inside the handler meant to report the first - so whatever had actually
gone wrong, what surfaced was always "attempt to call global 'debuginfo'".
Every real fault in the interface has been reporting under that name. It is a
no-op now, which is what it is: the retail one writes to a debug channel and
returns nothing a script can see. With it defined the handler runs through to
loading Blizzard_DebugTools, which is where a real error would now be named.

That is the fixture paying for itself the first time it was asked, and on a
fault nobody had reported - which is the whole argument for reaching state
rather than writing more tests.

## 2026-08-28 — the objectives tracker's zone filter

Reported as "sometimes it is togglable, sometimes not, and it sometimes
correlates with going into a new area". The diagnostic shipped the day before
said it in one line, twice in one session:

```
objectives tracker collapsing: BY ITSELF - an update measured nothing, and
that also disables the expand button (userCollapsed=nil, lines=true)
```

`WatchFrame_Update` collapses the tracker and *disables the button that
expands it* when the objective handlers report that nothing was laid out. It
is FrameXML's own behaviour and it is correct: there is nothing to show. So
the fault was upstream, in what emptied the list.

`WatchFrame_DisplayTrackedQuests` drops a watched quest unless
`CURRENT_MAP_QUESTS` holds it. The real client fills that from the world map's
POI markers; this client cannot, because those markers are whatever the server
was last asked about rather than the zone the player is standing in - so a shim
in `addon_manager.cpp` rebuilt the table from the quest log, matching each
quest's **log header text** against `GetRealZoneText()`.

Those two strings disagree for every quest filed under a sub-area. The header
comes from the quest's `zoneOrSort`, an AreaTable id, and that is often the
sub-area - Camp Narache - while the player standing in it is in Mulgore. Each
such quest was dropped, and when that was all of them the tracker collapsed and
locked itself shut. Which quests those are changes as quests are accepted and
handed in, and the zone under the player changes as they walk: intermittent,
and correlated with walking into a new area.

Matched by id now. `game::questIsInZone` resolves both sides through
`ZoneManager::resolveAreaZoneId` before comparing, exposed to Lua as
`__WoweeCurrentZoneQuestIds`, and the shim is four lines of table copying. Two
things are deliberately kept rather than filtered: a quest with no area (a
QuestSort group, or one whose query response has not landed) and *every* quest
when the zone itself is unknown. Too many lines is a nuisance; a tracker that
cannot be opened is a dead frame.

Reproduced in `framexml_run --player` before the fix - `mapquests=0`,
`collapsed=true`, button disabled, title "Objectives (8)", the client's log
exactly - and green after it. Six cases in `tests/test_quest_zone.cpp`.

Two harness notes went into `AGENTS.md`. `argv[1]` is the asset path and
nothing else parses it, so `framexml_run --player ...` makes `--player` the
path: FrameXML never loads, every interface global answers from the missing-API
stub, and the run reads as a client with no tracker. And `VARIABLES_LOADED`
never fires there, so `WATCHFRAME_FILTER_TYPE` sits at watchframe.lua's own `0`
rather than the `3` the `trackerFilter` CVar supplies.

## 2026-08-28 — the jump grunt, and every other clip the character owns

The same session log answered the second open report in one line:

```
Jump sound: nothing played - the audio engine is up: yes, clips loaded: 0,
voice profile: never chosen
```

`ActivitySoundManager::setCharacterVoiceProfile` opened with
`if (!assetManager) return;`, dropping the request outright when the audio
manager had not been initialised yet. The player spawn asks for the profile and
`Renderer` initialises the audio managers during world load; which of those two
happens first depends on how the world loaded, and when it went the wrong way
the request was thrown away and never made again - so the profile stayed
"never chosen" with the engine running perfectly.

The mirror of it was there too. `initialize()` begins with `shutdown()`, which
empties every clip vector, and the character's clips are loaded only from the
spawn - so a re-initialise left them empty while `voiceProfileKey` still
matched, and the next `setCharacterVoiceProfile` returned early on that key
without reloading anything.

The profile is a *request* now - folder, base and gender, held separately from
the loaded clips. `setCharacterVoiceProfile` records it before it checks
whether it can act on it, `initialize()` reapplies whatever is on record once
the assets are up, and the key guard on the model-name overload also requires
the clips to still be there. The order of spawn and initialise stops mattering.

This is the jump grunt, the swim strokes, the hard landing, the attack grunts,
the wound and death vocals - every clip filed under the character's own voice.
No regression test: the whole path is `AssetManager` file reads, and a fake for
it would be testing the fake.

## 2026-08-28 — the slain target that would not deselect

Reported as "when I slayed an enemy it is still selected and shows in the top
left next to my avatar but I cannot deselect it, while entities I select and
deselect that are alive work without problem".

The target diagnostics shipped for it did not fire, and that was the answer.
The session log showed the target going from a guid to zero with **no** "Target
cleared" line between - so something was dropping the target by a path that
neither logged nor told anyone.

Three places assigned `setTargetGuidRaw(0)` directly: a looted corpse despawned
locally, a game object despawned locally, and every `SMSG_DESTROY_OBJECT` for
the selected unit. The interface redraws the target frame on
`PLAYER_TARGET_CHANGED` and on nothing else, so each of those left the frame
drawing a unit the client had already forgotten - and pressing Escape reached
`CombatHandler::clearTarget`, which saw a guid of zero and did nothing at all.
Two half-states that add up to a frame nothing can clear. Only dead things go
through those paths, which is why living targets were fine.

All three go through `clearTarget()` now, which zeroes the guid, fires the
event and logs the change.

The class rather than the instance: `tools/silent_target_drop_check.py` pins
`setTargetGuidRaw` to `combat_handler.cpp`, the one file that fires the event
alongside every call. Four call sites, all where they belong.

## 2026-08-28 — release spirit, and a corpse eight minutes stale

Reported as "the release spirit thing is a little buggy, it jumps around when
you have died multiple times in the same spot and you have to run to different
spots one after another". The four position diagnostics shipped the day before
put it on one screen:

```
08:26:52  Corpse position <- death (health=0):          (-728.86,-180.06,...)
08:26:54  Corpse position <- corpse object 0x...2a6:    (-728.82,-180.21,...)
08:27:06  Corpse position <- corpse object 0x...2a6:    (-728.82,-180.21,...)
08:27:07  Corpse position <- corpse object 0x...2a2:    (-715.63,-193.73,...)
08:27:23  Corpse position <- corpse object 0x...2a6:    (-728.82,-180.21,...)
```

`0x...2a2` is the corpse of a death at 08:20, six minutes and thirteen yards
earlier. A player has one live corpse, but the **bones** of earlier deaths stay
in the world and carry the same owner guid - so "is this corpse mine?" answered
yes for something that was no longer a destination, and each time the bones
drifted back into view the cached corpse position moved to them. Walk on, the
live corpse comes back into view, the position moves back. That is the jumping.

`CORPSE_FIELD_FLAGS` carries `CORPSE_FLAG_BONES` and settles it. The field
indices are named now in `include/game/corpse_fields.hpp` rather than being a
bare `6` with the arithmetic in a comment, and `corpseIsReclaimableBy` is the
whole rule: mine, and not bones. A missing flags field reads as zero, which is
the right way round - an initial update mask omits fields whose value is zero,
and a live corpse is exactly the one that may carry none, while bones always
carry a set bit. Six cases in `tests/test_corpse_fields.cpp`.

## 2026-08-28 — and the click that was never a click

The oldest tracker item, "the collapse button does not answer a click", was
closed by the same log without a line of code. A morning had gone into
narrowing it on 2026-08-27, ending with an ImGui window named as the prime
suspect and everything checkable headlessly ruled out.

The log shows presses landing squarely on `WatchFrameCollapseExpandButton`, and
one of them running its `OnClick`. The others were refused because the button
was **disabled** - by the zone filter fixed earlier today. The click was always
arriving. The first sentence of the hypothesis was wrong and every hour after
it was spent underneath that.

The rule it earns: when a chain of reasoning gets long without a reading in it,
the fault is usually above where the chain starts.

## 2026-08-28 — two diagnostics before the next session

Both are about the four fixes above being *verifiable* rather than inferred
from silence.

The tracker's collapse line now carries the filter's own numbers -
`watched=8, inZone=0, zone=, filter=3`. The one time it fired for real it said
that an update measured nothing and left the reason to a morning of narrowing;
a collapse with watches held and an empty zone table is the filter, and one
with a full table is not. Only on the collapse, so it costs nothing until
something is wrong.

And the zone-filter override no longer falls through its guard in silence. It
runs after FrameXML and any of its three names could be absent, in which case
FrameXML's POI-based original is restored - which answers nothing on this
client and is precisely the state that emptied the tracker and disabled its own
expand button. A quiet fallback to the bug is the worst of the three outcomes,
so it says so now.
