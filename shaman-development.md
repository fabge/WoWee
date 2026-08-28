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

## Smartstone — resolved

Tooltip reads `Use: Dummy`. A dummy spell has no client-side effect; the behaviour would live entirely in a server-side script. The hand-raise animation is the generic cast with nothing behind it, so the ChromieCraft module is either not enabled or not applicable at this level. Unique, BoP, no vendor value. Safe to delete; nothing depends on it.

If it later turns out other players get a working gossip/menu from it, that would be a client packet-handling bug in this fork rather than a game question — worth a look then, not before.

## Progression checkpoint (level 11, Thunder Bluff)

Turn-in order for the backlog of completed quests:

1. **Call of Fire** — first. Rewards the Fire Totem, which gates Searing Totem.
2. **Winterhoof Cleansing** — return to Mull Thunderhorn, Bloodhoof Village. Also clears the quest totem from the bag.
3. The Venture Co., The Hunter's Way, Rites of the Earthmother.
4. Preparation for Ceremony — Thunder Bluff.

Use **Show Map** in the quest log to locate any turn-in.

Next zone: Mulgore is done. Head south from Bloodhoof Village to **The Crossroads, The Barrens** — the standard Horde 10–20 zone, which covers the run up to Call of Water at 20.
