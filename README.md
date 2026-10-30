# mod-drawestuff

Author: Tntdruid

`mod-drawestuff` draws a quest guide marker in the world: a beam of light at the spot the [AzerothQuest](https://github.com/Tntdruid/AzerothQuest) arrow points to. Only the player who follows the guide sees their beam. Other players, and GMs, do not.

The 3.3.5a client cannot draw anything in the 3D world from an addon, so the addon tells the server where the arrow points and the server places the beam.

## How it works

1. AzerothQuest sends the arrow's target as an addon message whispered to the player themself, with the prefix `AQDRAW`. The module takes that message, and it is never delivered.
2. The module turns the zone and map percent into world coordinates, the same way `.go zonexy` does.
3. When the player comes within `DrawEstuff.SpawnRange` yards, the module spawns gameobject `1990001` on the ground at that spot. The object's AI (`DrawEstuffMarkerAI`) lets only its owner see it.
4. A new target replaces the beam. The beam is also removed when the guide has no target, when the player moves more than 50 yards beyond the spawn range, leaves the map or logs out.

On a server without the module, the whisper is handed back to the player as an addon message that nothing shows, so AzerothQuest works the same as before.

### Addon messages

Each request is `SendAddonMessage("AQDRAW", body, "WHISPER", UnitName("player"))`:

| Body | Effect |
| --- | --- |
| `SET <x> <y> <zone name>` | Marks the spot. `x` and `y` are map percent (0-100) as on the world map, and the zone name is the one the world map shows, such as `SET 42.50 30.00 Ashenvale`. |
| `CLEAR` | Removes the marker. |

The zone name is matched against the zone names in AreaTable.dbc, in the player's client language or in English. Only continent zones work, not dungeons.

## Installation

Clone the module into the `modules` folder of your AzerothCore source, re-run CMake and rebuild:

```bash
cd azerothcore-wotlk/modules
git clone https://github.com/Tntdruid/mod-drawestuff.git
```

Apply `data/sql/db-world/drawestuff_marker.sql` to the world database, or let the database updater apply it. It adds gameobject template 1990001. Without it the module logs an error and draws nothing.

Copy `conf/mod_drawestuff.conf.dist` to `mod_drawestuff.conf` in your config folder to change the settings.

## Configuration

```ini
DrawEstuff.Enable = 1          # 0 turns the markers off
DrawEstuff.MarkerEntry = 1990001
DrawEstuff.SpawnRange = 150    # yards from the target at which the beam appears
DrawEstuff.Debug = 0           # 1 logs every request and spawn to the "module" log
```

For another look, change `displayId` in the SQL. The default, 3272, is the light beam of "Uther Shrine Lightbeam". If the beam is hard to see, try another display there.

## In the game

In AzerothQuest, `/aq beam` turns the beam on or off. It is on by default.

## Known limits

- The beam is placed on the ground (terrain) height. Inside buildings or caves it can sit on the ground outside or below them.
- Players see it from the normal "Large" visibility distance, not across the zone. The arrow still leads the way from far off.
