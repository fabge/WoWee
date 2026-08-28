# Shaman development notes — Tohaze

Tauren shaman on ChromieCraft (WotLK 3.3.5a, max level 80). Elemental (caster) spec. Started these notes at level 11.

## The two systems

**Trainer spells** and **talents** are separate and independent.

- The trainer sells spells. Every shaman of a given level can buy the same list — there is no "elemental trainer". Buying Lightning Bolt does not make you a caster, it just puts the button in the spellbook.
- Talents specialize you. First point at level 10, one per level after.

Visit the trainer every 2 levels. New *ranks* of existing spells are the single biggest power jump while levelling — bigger than most gear upgrades.

## Windows and keys

| Window | Key | What it is |
|---|---|---|
| Spellbook | `P` | What you have bought. Tabs on the right are General / Shaman / Professions. |
| **Talents** | **`N`** | The three trees. This is where points are spent. |
| Character | `C` | Gear and stats. |

The spellbook is *not* the talent window — easy to confuse because both are book-shaped micro-buttons at the bottom right.

## Talent trees

- **Elemental Combat** — caster. Lightning Bolt, shocks, totem damage. **This is our tree.**
- **Enhancement** — melee, two-weapon, Windfury.
- **Restoration** — healing.

Do not dip into other trees while levelling. Resets are available at the trainer for an escalating gold cost, so nothing is permanent, but it is wasted gold.

### Point order

ChromieCraft regenerates mana fast, so mana efficiency is not the bottleneck it is on a blizzlike server. Damage first:

1. **Concussion** 5/5 — +5% damage to Lightning Bolt, Chain Lightning and shocks.
2. **Convection** 5/5 — cheaper shocks and lightning. Still worth taking, just second.
3. **Call of Flame** — buffs Searing Totem, a large share of levelling damage.
4. **Elemental Focus**, then **Elemental Fury** as they unlock.

Milestones further down the tree:

- **Elemental Mastery** at 31 points.
- **Lava Burst** at 41 points — the signature Elemental nuke in Wrath.

## Action bar (level 11 onward)

| Button | Notes |
|---|---|
| Lightning Bolt | Main nuke and opener. |
| Flame Shock | DoT, apply right after the pull. |
| Earth Shock | Instant burst / finisher. Expensive, do not spam. |
| Searing Totem | Drop every fight. Free sustained damage. |
| Lightning Shield | Keep up permanently, refresh out of combat. |
| Flametongue Weapon | Caster imbue — grants spell power. **Not** Rockbiter (that is the melee/threat imbue). |
| Healing Wave | Emergency heal and between-pull topping off. |
| Stoneclaw / Stoneskin Totem | Situational, keep reachable. |
| Ghost Wolf | Unlocks at 16. Travel speed, big quality-of-life. |

## Pull rotation

1. Buff: Lightning Shield + Flametongue Weapon.
2. Open with Lightning Bolt.
3. Drop Searing Totem as the mob closes.
4. Flame Shock.
5. Lightning Bolt until dead.

With ChromieCraft's regen this can be run as near-pure casting. Melee weaving (autoattacking while Searing Totem works) remains a fallback if mana ever does get tight — Lightning Shield damages attackers, so standing in melee is not wasted.

## Standing reminders

- Buy **mail armour training at level 40**. Leather until then, do not worry about it.
- Keep the weapon reasonably current while still melee weaving at low levels.
- Stock water from any innkeeper.
- Astral Recall at 30 — a second hearthstone on its own cooldown.

## Level checkpoints to revisit

- **16** — Ghost Wolf.
- **20** — Water Shield, Frost Shock, Healing Stream Totem.
- **26** — Chain Lightning, Mana Spring Totem.
- **40** — mail armour, and the tree is deep enough for Elemental Mastery territory.

## Weapon imbues

Only **one imbue per weapon**, and a shaman only gets two at once by dual-wielding, which needs the Dual Wield talent deep in Enhancement. So with one weapon (or weapon + shield) there is exactly one active imbue, and a new one overwrites the old.

- **Flametongue Weapon** — fire damage on hit plus **spell power**. This is ours. Keep it up permanently, re-apply after death or when the 30 min expires.
- **Rockbiter Weapon** — attack power and threat. Enhancement/tanking. Dead weight for Elemental.

## Totem tools (class quests)

The four elemental totem *items* are permanent tools. They live in the bags, are never consumed, are not equipped, and must be present to cast totems of that school. **Never sell or delete them.**

| Item | From | Gates |
|---|---|---|
| Earth Totem | Call of Earth (~lvl 4) | Stoneclaw, Stoneskin, Earthbind, Strength of Earth |
| Fire Totem | **Call of Fire (lvl 10)** | Searing Totem, Fire Nova, Magma, Totem of Wrath |
| Water Totem | Call of Water (lvl 20) | Healing Stream, Mana Spring, Fire Resistance |
| Air Totem | Call of Air (lvl 30) | Windfury, Grace of Air, Wrath of Air, Grounding |

Do each class quest as soon as it is available — they gate real damage, not flavour. Call of Fire in particular is what unlocks Searing Totem.

## Bag triage

- Quest-item totems (e.g. Winterhoof Cleansing Totem, Mulgore water wells) are one-shot quest items. Once the quest is turned in they are vendor trash.
- **Smartstone** is a ChromieCraft server addition, not a Blizzard item. Right-click to see its menu; keep it.

## Smartstone — what it actually is

Real ChromieCraft feature, not a vestigial item. Module: https://github.com/chromiecraft/mod-chromiecraft-smartstone

It repurposes item **32547** ("Tier 5 Mage Test Gear"), which is why the tooltip is bare and reads `Use: Dummy` — the item template was reused and all behaviour lives in a server-side script. Using it should open a **gossip menu** (same window as talking to an NPC) offering:

- Companions and pets — free ones tied to PvE achievements (Serpentshrine Waterspawn, Hyjal Wisp), others subscriber-only
- Costumes and cosmetic auras
- Character services — name/race/faction change, Portable Barbershop, via tokens or subscription
- Announcement toggles — BG/arena queue, PvP notifications (needs `EnablePlayerSettings = 1` server-side)

Re-obtainable at any time with the `.smartstone` chat command, so **deleting it is reversible** — it is worth keeping only when the services are actually usable. At low level with no unlocks it is one bag slot for nothing.

Before deleting, run `.smartstone` while still holding it to confirm the command works on this account. If it responds, delete freely. If it does nothing, keep the stone.

### Nothing happens on use — two candidates

1. **Empty menu (most likely, benign).** Almost every entry is gated behind achievements, tokens or a subscription. At low level with no unlocks there may be zero valid entries, and an empty gossip menu can result in no packet at all: cast animation, no window.
2. **Gossip rendering in this fork.** The whole feature is a gossip packet. If the client does not display gossip triggered by *item* use, this is exactly the symptom.

**Test to distinguish:** talk to an NPC with a real gossip menu — a flight master, or an innkeeper's "Make this inn your home". If NPC gossip renders correctly, client gossip handling is fine and the Smartstone is simply empty for this character; nothing to fix. If NPC gossip is also blank or missing options, that is a genuine client bug worth an entry in `TODO.md`.

## Progression checkpoint (level 11, Thunder Bluff)

Turn-in order for the backlog of completed quests:

1. **Call of Fire** — first. Rewards the Fire Totem, which gates Searing Totem.
2. **Winterhoof Cleansing** — return to Mull Thunderhorn, Bloodhoof Village. Also clears the quest totem from the bag.
3. The Venture Co., The Hunter's Way, Rites of the Earthmother.
4. Preparation for Ceremony — Thunder Bluff.

Use **Show Map** in the quest log to locate any turn-in.

Next zone: Mulgore is done. Head south from Bloodhoof Village to **The Crossroads, The Barrens** — the standard Horde 10–20 zone, which covers the run up to Call of Water at 20.

## Action bar layout

Principle: everything pressed *during* a fight sits on `1`–`6` where the fingers already rest. Everything else goes on keys there is time to reach for.

Main bar — combat:

| Key | Spell |
|---|---|
| `1` | Lightning Bolt |
| `2` | Flame Shock |
| `3` | Earth Shock |
| `4` | Searing Totem |
| `5` | Healing Wave |
| `6` | Stoneclaw Totem |

Secondary — reachable, not urgent:

| Key | Spell |
|---|---|
| `7` | Earthbind Totem |
| `8` | Lightning Shield |
| `9` | Flametongue Weapon |
| `0` | Ghost Wolf (16) |
| `-` | Healthstone / potions / food |
| `=` | Hearthstone |

## Shock cooldown — the key mechanic

**All shocks share one 6 second cooldown.** Flame Shock, Earth Shock and later Frost Shock are not independent buttons; using one locks out the others. Every fight is a choice of which shock owns the current window, never a sequence of both. (Reverberation in the Elemental tree shortens this cooldown.)

## Combat patterns

Pre-pull, out of combat and free: Lightning Shield up, Flametongue Weapon on, **drop Searing Totem where you are standing and then pull**. ~20 yard range, 60 second duration — the fight must happen next to the totem.

**Normal trash** (dead in under ~10s):
1. Lightning Bolt as the opener while it closes
2. **Earth Shock** — instant, front-loaded
3. Lightning Bolt to finish, autoattacking in melee between casts

No Flame Shock here; a 12 second DoT on an 8 second mob is wasted mana.

**Tough mob / elite / higher level:**
1. **Flame Shock** first — the DoT gets full value over a long fight
2. Lightning Bolt
3. Earth Shock when the shock cooldown returns
4. Healing Wave below ~40%

**Multiple mobs:**
1. **Earthbind Totem** immediately — slows everything nearby
2. Kill one at a time, fully. Never split damage.
3. Back up while they are slowed for free casts
4. **Stoneclaw Totem** when in trouble — briefly pulls mobs off, buying room to heal or run

**Casters and ranged mobs** will not walk to you. Break line of sight behind terrain to force them to close, then fight them next to Searing Totem where they cannot cast.

**Escape:** Stoneclaw Totem, then Ghost Wolf out. Do not try to out-heal a losing fight at low level — Healing Wave has a cast time and dying mid-cast is the usual outcome.

**Between fights:** drink to full before anything dangerous, re-apply Lightning Shield if it dropped. Even with fast regen, opening a fight at half mana is how a bad add kills you.
