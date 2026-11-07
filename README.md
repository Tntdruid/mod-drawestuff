# mod-drawestuff

Author: Tntdruid

`mod-drawestuff` draws a quest guide marker in the world: a beam of light at the spot the [AzerothQuest](https://github.com/Tntdruid/AzerothQuest) arrow points to. Only the player who follows the guide sees their beam. Other players, and GMs, do not.

The 3.3.5a client cannot draw anything in the 3D world from an addon, so the addon tells the server where the arrow points and the server places the beam.

## How it works

1. AzerothQuest sends the arrow's target as an addon message whispered to the player themself, with the prefix `AQDRAW`. The module takes that message, and it is never delivered.
2. The module turns the zone and map percent into world coordinates, the same way `.go zonexy` does.
3. When the player comes within `DrawEstuff.SpawnRange` yards, the module spawns gameobject `1990001`, a silent glowing column, on the ground at that spot. The object's AI (`DrawEstuffMarkerAI`) lets only its owner see it.
4. A new target replaces the beam. The beam is also removed when the guide has no target, when the player moves more than 50 yards beyond the spawn range, leaves the map or logs out.

### Quest targets

When the addon asks for it (`TARGETS ON`), the module also puts a slim beam (gameobject `1990002`) on the nearest creatures and gameobjects within `DrawEstuff.TargetRange` that a quest in the player's log still needs, at most `DrawEstuff.MaxTargets` of them:

- creatures to kill or cast a quest spell on, including kill credit stand-ins,
- creatures that drop a quest item the player still needs,
- gameobjects to loot or use, by the same test that makes them sparkle in the client.

Creatures in combat, or tapped by someone outside the player's group, are skipped. A target that walks more than 3 yards gets a new beam where it is. Beams go when the target dies, is looted, or the objective is done. The list is checked once a second.

On a server without the module, the whisper is handed back to the player as an addon message that nothing shows, so AzerothQuest works the same as before.

### Addon messages

Each request is `SendAddonMessage("AQDRAW", body, "WHISPER", UnitName("player"))`:

| Body | Effect |
| --- | --- |
| `SET <x> <y> <zone name>` | Marks the spot. `x` and `y` are map percent (0-100) as on the world map, and the zone name is the one the world map shows, such as `SET 42.50 30.00 Ashenvale`. |
| `CLEAR` | Removes the marker. |
| `TARGETS ON` / `TARGETS OFF` | Turns the quest target beams on or off. The server forgets this at logout, so the addon sends it again each session. |

The zone name is matched against the zone names in AreaTable.dbc, in the player's client language or in English. Only continent zones work, not dungeons.

## Installation

Clone the module into the `modules` folder of your AzerothCore source, re-run CMake and rebuild:

```bash
cd azerothcore-wotlk/modules
git clone https://github.com/Tntdruid/mod-drawestuff.git
```

Apply `data/sql/db-world/drawestuff_marker.sql` to the world database, or let the database updater apply it. It adds gameobject templates 1990001 and 1990002. Without it the module logs an error and draws nothing.

Copy `conf/mod_drawestuff.conf.dist` to `mod_drawestuff.conf` in your config folder to change the settings.

## Configuration

```ini
DrawEstuff.Enable = 1          # 0 turns the markers off
DrawEstuff.MarkerEntry = 1990001
DrawEstuff.SpawnRange = 150    # yards from the target at which the beam appears
DrawEstuff.QuestTargets = 1    # 0 turns the quest target beams off for everyone
DrawEstuff.TargetEntry = 1990002
DrawEstuff.TargetRange = 60    # yards within which quest targets are marked
DrawEstuff.MaxTargets = 5      # nearest quest targets marked at once
DrawEstuff.Debug = 0           # 1 logs every request and spawn to the "module" log
```

The guide marker is the Night Elf flag capture glow of Warsong Gulch (display 7258), and the quest target markers the smaller Orc one (display 7086). Both are silent. The first version used the "Uther Shrine Lightbeam" (display 3272), which hums.

For another look, change `displayId` or `size` in the SQL. The client caches gameobject looks, so after a change delete the client's `Cache` folder before logging in, or the old look stays. Silent column-like displays from the 3.3.5 client:

| Display | Height at size 1 | Model |
| --- | --- | --- |
| 7258 | 14.6 | Warsong Gulch Night Elf flag capture glow |
| 7086 | 14.6 | Warsong Gulch Orc flag capture glow |
| 6813 | 14.2 | Stranglethorn light shaft |
| 7116 | 22.1 | Terokkar light ray |
| 6231 | 22.5 | Westfall lighthouse beam |
| 5456 | 36.8 | Volumetric light shaft |

## In the game

In AzerothQuest, `/aq beam` turns the guide beam on or off and `/aq targets` the quest target beams. Both are on by default.

When no beam shows, type `.drawestuff` in chat. It shows whether the module and its gameobject templates are there, what your addon has asked for, and the quest targets the server finds around you, so you can tell where it stops without the server log.

## Known limits

- The beam is placed on the ground (terrain) height. Inside buildings or caves it can sit on the ground outside or below them.
- Players see it from the normal "Large" visibility distance, not across the zone. The arrow still leads the way from far off.
