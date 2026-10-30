#include "Config.h"
#include "DBCStores.h"
#include "GameObject.h"
#include "GameObjectAI.h"
#include "GridTerrainData.h"
#include "Log.h"
#include "Map.h"
#include "MapMgr.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "ScriptMgr.h"
#include <algorithm>
#include <locale>
#include <sstream>

namespace
{
    bool sEnabled = true;
    bool sDebug = false;
    uint32 sMarkerEntry = 1990001;
    float sSpawnRange = 150.0f;

    // Addon messages arrive as "<prefix>\t<body>" (SendAddonMessage whispered to oneself).
    constexpr char const* ADDON_PREFIX = "AQDRAW\t";
    constexpr char const* MARKER_KEY = "DrawEstuffMarker";
    constexpr uint32 CHECK_INTERVAL = 1000;
    // Kept a little past the spawn range so a marker at the edge does not flicker.
    constexpr float DESPAWN_MARGIN = 50.0f;

#define DE_DEBUG(...) do { if (sDebug) LOG_INFO("module", "[DrawEstuff] " __VA_ARGS__); } while (0)

    // The one guide marker a player asked for. Lives in Player::CustomData, so it goes away with the player.
    struct Marker : public DataMap::Base
    {
        bool active = false;
        uint32 mapId = 0;
        float x = 0.0f;
        float y = 0.0f;
        ObjectGuid goGuid;
        uint32 checkTimer = 0;
    };

    // The zone named by the client map (zone-level AreaTable entry on a continent), or nullptr.
    AreaTableEntry const* FindMapZone(std::string const& name, LocaleConstant locale)
    {
        for (uint32 i = 0; i < sAreaTableStore.GetNumRows(); ++i)
        {
            AreaTableEntry const* area = sAreaTableStore.LookupEntry(i);
            if (!area || area->zone)
                continue;

            if (name != area->area_name[locale] && name != area->area_name[LOCALE_enUS])
                continue;

            MapEntry const* map = sMapStore.LookupEntry(area->mapid);
            if (map && !map->Instanceable())
                return area;
        }

        return nullptr;
    }

    // "SET <x> <y> <zone name>" with x/y in client map percent (0-100), or "CLEAR".
    void HandleCommand(Player* player, std::string const& body)
    {
        Marker* marker = player->CustomData.GetDefault<Marker>(MARKER_KEY);

        std::istringstream in(body);
        in.imbue(std::locale::classic());
        std::string command;
        in >> command;

        if (command == "CLEAR")
        {
            marker->active = false;
            marker->goGuid.Clear();
            DE_DEBUG("{} cleared the marker", player->GetName());
            return;
        }

        if (command != "SET")
            return;

        float x = 0.0f, y = 0.0f;
        std::string zoneName;
        in >> x >> y;
        std::getline(in >> std::ws, zoneName);
        if (zoneName.empty())
            return;

        AreaTableEntry const* zone = FindMapZone(zoneName, player->GetSession()->GetSessionDbcLocale());
        if (!zone || x < 0.0f || x > 100.0f || y < 0.0f || y > 100.0f)
        {
            DE_DEBUG("{} sent an unknown spot: {} {:.1f} {:.1f}", player->GetName(), zoneName, x, y);
            return;
        }

        Zone2MapCoordinates(x, y, zone->ID);
        if (!MapMgr::IsValidMapCoord(zone->mapid, x, y))
            return;

        // A new spot replaces the old marker; its object removes itself on its next check.
        marker->active = true;
        marker->mapId = zone->mapid;
        marker->x = x;
        marker->y = y;
        marker->goGuid.Clear();
        marker->checkTimer = 0;
        DE_DEBUG("{} set a marker in {} at map {} ({:.1f}, {:.1f})", player->GetName(), zoneName, zone->mapid, x, y);
    }
}

// Shown only to the player who owns the marker; removes itself once that player has moved on.
class DrawEstuffMarkerAI : public GameObjectAI
{
public:
    explicit DrawEstuffMarkerAI(GameObject* go) : GameObjectAI(go) {}

    void SetOwner(ObjectGuid owner) { _owner = owner; }

    bool CanBeSeen(Player const* seer) override
    {
        return seer->GetGUID() == _owner;
    }

    void UpdateAI(uint32 diff) override
    {
        if (_checkTimer > diff)
        {
            _checkTimer -= diff;
            return;
        }
        _checkTimer = CHECK_INTERVAL;

        // Only finds the owner on this map, so a marker left behind on another map goes too.
        Player* owner = ObjectAccessor::GetPlayer(*me, _owner);
        Marker const* marker = owner ? owner->CustomData.Get<Marker>(MARKER_KEY) : nullptr;
        if (!marker || !marker->active || marker->goGuid != me->GetGUID())
            me->Delete();
    }

private:
    ObjectGuid _owner;
    uint32 _checkTimer = CHECK_INTERVAL;
};

namespace
{
    GameObject* SpawnMarker(Player* player, Marker const* marker)
    {
        Map* map = player->GetMap();
        float z = std::max(map->GetHeight(player->GetPhaseMask(), marker->x, marker->y, MAX_HEIGHT), map->GetWaterLevel(marker->x, marker->y));
        if (z <= INVALID_HEIGHT)
            z = player->GetPositionZ();

        GameObject* go = new GameObject();
        if (!go->Create(map->GenerateLowGuid<HighGuid::GameObject>(), sMarkerEntry, map, player->GetPhaseMask(),
            marker->x, marker->y, z, 0.0f, G3D::Quat(), 100, GO_STATE_READY))
        {
            delete go;
            return nullptr;
        }

        // The AI comes from the template's ScriptName; without it everyone would see the marker.
        DrawEstuffMarkerAI* ai = dynamic_cast<DrawEstuffMarkerAI*>(go->AI());
        if (!ai)
        {
            LOG_ERROR("module", "[DrawEstuff] Gameobject {} needs ScriptName 'DrawEstuffMarkerAI'; run the module's SQL.", sMarkerEntry);
            delete go;
            return nullptr;
        }

        ai->SetOwner(player->GetGUID());
        go->SetSpawnedByDefault(false);
        go->SetVisibilityDistanceOverride(VisibilityDistanceType::Large);
        map->AddToMap(go);
        return go;
    }
}

class DrawEstuffWorld : public WorldScript
{
public:
    DrawEstuffWorld() : WorldScript("DrawEstuffWorld", { WORLDHOOK_ON_AFTER_CONFIG_LOAD }) {}

    void OnAfterConfigLoad(bool /*reload*/) override
    {
        sEnabled = sConfigMgr->GetOption<bool>("DrawEstuff.Enable", true);
        sDebug = sConfigMgr->GetOption<bool>("DrawEstuff.Debug", false);
        sMarkerEntry = sConfigMgr->GetOption<uint32>("DrawEstuff.MarkerEntry", 1990001);
        sSpawnRange = sConfigMgr->GetOption<float>("DrawEstuff.SpawnRange", 150.0f);
    }
};

class DrawEstuffPlayer : public PlayerScript
{
public:
    // Only register the hooks we use; an empty list enables every PlayerScript hook.
    DrawEstuffPlayer() : PlayerScript("DrawEstuffPlayer", { PLAYERHOOK_CAN_PLAYER_USE_PRIVATE_CHAT, PLAYERHOOK_ON_UPDATE }) {}

    // The addon whispers its requests to the player themself; take those and do not deliver them.
    bool OnPlayerCanUseChat(Player* player, uint32 /*type*/, uint32 language, std::string& msg, Player* receiver) override
    {
        if (language != LANG_ADDON || player != receiver || msg.rfind(ADDON_PREFIX, 0) != 0)
            return true;

        if (sEnabled)
            HandleCommand(player, msg.substr(std::char_traits<char>::length(ADDON_PREFIX)));

        return false;
    }

    void OnPlayerUpdate(Player* player, uint32 diff) override
    {
        Marker* marker = player->CustomData.Get<Marker>(MARKER_KEY);
        if (!marker || !marker->active)
            return;

        if (marker->checkTimer > diff)
        {
            marker->checkTimer -= diff;
            return;
        }
        marker->checkTimer = CHECK_INTERVAL;

        if (!sEnabled || !player->IsInWorld() || player->GetMapId() != marker->mapId)
        {
            marker->goGuid.Clear();
            return;
        }

        GameObject* go = marker->goGuid ? player->GetMap()->GetGameObject(marker->goGuid) : nullptr;
        float distance = player->GetExactDist2d(marker->x, marker->y);

        if (distance > sSpawnRange + DESPAWN_MARGIN)
        {
            if (go)
                go->Delete();
            marker->goGuid.Clear();
            return;
        }

        // Follow the player into a quest phase so the marker stays in sight.
        if (go && go->GetPhaseMask() != player->GetPhaseMask())
            go->SetPhaseMask(player->GetPhaseMask(), true);

        // Spawned only near the player so its grid is loaded; spawned again if the grid unloaded it.
        if (!go && distance <= sSpawnRange)
        {
            if (GameObject* spawned = SpawnMarker(player, marker))
            {
                marker->goGuid = spawned->GetGUID();
                DE_DEBUG("Spawned a marker for {} at ({:.1f}, {:.1f}, {:.1f})", player->GetName(),
                    spawned->GetPositionX(), spawned->GetPositionY(), spawned->GetPositionZ());
            }
        }
    }
};

void AddDrawEstuffScripts()
{
    new DrawEstuffWorld();
    new DrawEstuffPlayer();
    RegisterGameObjectAI(DrawEstuffMarkerAI);
}
