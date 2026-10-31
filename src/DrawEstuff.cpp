#include "CellImpl.h"
#include "Config.h"
#include "Creature.h"
#include "DBCStores.h"
#include "GameObject.h"
#include "GameObjectAI.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "GridTerrainData.h"
#include "Log.h"
#include "LootMgr.h"
#include "Map.h"
#include "MapMgr.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "QuestDef.h"
#include "ScriptMgr.h"
#include <algorithm>
#include <locale>
#include <sstream>
#include <unordered_set>
#include <vector>

namespace
{
    bool sEnabled = true;
    bool sDebug = false;
    uint32 sMarkerEntry = 1990001;
    float sSpawnRange = 150.0f;
    bool sQuestTargets = true;
    uint32 sTargetEntry = 1990002;
    float sTargetRange = 60.0f;
    uint32 sMaxTargets = 5;

    // Addon messages arrive as "<prefix>\t<body>" (SendAddonMessage whispered to oneself).
    constexpr char const* ADDON_PREFIX = "AQDRAW\t";
    constexpr char const* MARKER_KEY = "DrawEstuffMarker";
    constexpr uint32 CHECK_INTERVAL = 1000;
    // Kept a little past the spawn range so a marker at the edge does not flicker.
    constexpr float DESPAWN_MARGIN = 50.0f;
    // A target that walked further than this from its marker gets a new one where it is now.
    constexpr float FOLLOW_DISTANCE = 3.0f;

#define DE_DEBUG(...) do { if (sDebug) LOG_INFO("module", "[DrawEstuff] " __VA_ARGS__); } while (0)

    // A small marker on a creature or gameobject one of the player's quests needs.
    struct TargetMark
    {
        ObjectGuid target;
        ObjectGuid go;
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
    };

    // What the player's addon asked for: the guide marker and the quest target markers.
    // Lives in Player::CustomData, so it goes away with the player.
    struct Marker : public DataMap::Base
    {
        bool active = false;
        uint32 mapId = 0;
        float x = 0.0f;
        float y = 0.0f;
        ObjectGuid goGuid;
        uint32 checkTimer = 0;

        bool targetsEnabled = false;
        std::vector<TargetMark> targets;

        bool Owns(ObjectGuid guid) const
        {
            if (active && goGuid == guid)
                return true;

            return std::any_of(targets.begin(), targets.end(), [guid](TargetMark const& mark) { return mark.go == guid; });
        }
    };

    // The zone named by the client map (zone-level AreaTable entry on a continent), or nullptr.
    AreaTableEntry const* FindMapZone(std::string const& name, LocaleConstant locale)
    {
        for (uint32 i = 0; i < sAreaTableStore.GetNumRows(); ++i)
        {
            AreaTableEntry const* area = sAreaTableStore.LookupEntry(i);
            if (!area || area->zone)
                continue;

            char const* localName = area->area_name[locale];
            char const* englishName = area->area_name[LOCALE_enUS];
            if (!(localName && name == localName) && !(englishName && name == englishName))
                continue;

            MapEntry const* map = sMapStore.LookupEntry(area->mapid);
            if (map && !map->Instanceable())
                return area;
        }

        return nullptr;
    }

    // "SET <x> <y> <zone name>" with x/y in client map percent (0-100), "CLEAR", or "TARGETS ON|OFF".
    void HandleCommand(Player* player, std::string const& body)
    {
        Marker* marker = player->CustomData.GetDefault<Marker>(MARKER_KEY);

        std::istringstream in(body);
        in.imbue(std::locale::classic());
        std::string command;
        in >> command;

        if (command == "TARGETS")
        {
            std::string state;
            in >> state;
            marker->targetsEnabled = state == "ON";
            marker->checkTimer = 0;
            DE_DEBUG("{} turned quest target markers {}", player->GetName(), marker->targetsEnabled ? "on" : "off");
            return;
        }

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
        if (!marker || !marker->Owns(me->GetGUID()))
            me->Delete();
    }

private:
    ObjectGuid _owner;
    uint32 _checkTimer = CHECK_INTERVAL;
};

namespace
{
    GameObject* SpawnMarker(Player* player, uint32 entry, float x, float y, float z, bool farVisible)
    {
        Map* map = player->GetMap();
        GameObject* go = new GameObject();
        if (!go->Create(map->GenerateLowGuid<HighGuid::GameObject>(), entry, map, player->GetPhaseMask(),
            x, y, z, 0.0f, G3D::Quat(), 100, GO_STATE_READY))
        {
            delete go;
            return nullptr;
        }

        // The AI comes from the template's ScriptName; without it everyone would see the marker.
        DrawEstuffMarkerAI* ai = dynamic_cast<DrawEstuffMarkerAI*>(go->AI());
        if (!ai)
        {
            LOG_ERROR("module", "[DrawEstuff] Gameobject {} needs ScriptName 'DrawEstuffMarkerAI'; run the module's SQL.", entry);
            delete go;
            return nullptr;
        }

        ai->SetOwner(player->GetGUID());
        // A new gameobject has a 300 s respawn delay; without clearing it, isSpawned() is false and it stays invisible.
        go->SetRespawnTime(0);
        go->SetSpawnedByDefault(false);
        if (farVisible)
            go->SetVisibilityDistanceOverride(VisibilityDistanceType::Large);
        map->AddToMap(go);
        return go;
    }

    void UpdateGuideMarker(Player* player, Marker* marker)
    {
        if (!marker->active)
            return;

        if (player->GetMapId() != marker->mapId)
        {
            marker->goGuid.Clear();
            return;
        }

        Map* map = player->GetMap();
        GameObject* go = marker->goGuid ? map->GetGameObject(marker->goGuid) : nullptr;
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
        if (go || distance > sSpawnRange)
            return;

        float z = std::max(map->GetHeight(player->GetPhaseMask(), marker->x, marker->y, MAX_HEIGHT), map->GetWaterLevel(marker->x, marker->y));
        if (z <= INVALID_HEIGHT)
            z = player->GetPositionZ();

        if (GameObject* spawned = SpawnMarker(player, sMarkerEntry, marker->x, marker->y, z, true))
        {
            marker->goGuid = spawned->GetGUID();
            DE_DEBUG("Spawned a marker for {} at ({:.1f}, {:.1f}, {:.1f})", player->GetName(), marker->x, marker->y, z);
        }
    }

    // Creature entries the player still has to kill (or cast a quest spell on) for a quest in the log.
    std::unordered_set<uint32> GetNeededCreatures(Player* player)
    {
        std::unordered_set<uint32> entries;
        QuestStatusMap& statuses = player->getQuestStatusMap();

        for (uint8 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
        {
            uint32 questId = player->GetQuestSlotQuestId(slot);
            Quest const* quest = questId ? sObjectMgr->GetQuestTemplate(questId) : nullptr;
            auto status = statuses.find(questId);
            if (!quest || status == statuses.end() || status->second.Status != QUEST_STATUS_INCOMPLETE)
                continue;

            for (uint8 i = 0; i < QUEST_OBJECTIVES_COUNT; ++i)
                if (quest->RequiredNpcOrGo[i] > 0 && status->second.CreatureOrGOCount[i] < quest->RequiredNpcOrGoCount[i])
                    entries.insert(uint32(quest->RequiredNpcOrGo[i]));
        }

        return entries;
    }

    // A creature to kill for a quest, or one that drops a quest item the player still needs.
    // Ones already fighting, or tapped by someone outside the player's group, are left out.
    bool IsQuestTarget(Player* player, Creature* creature, std::unordered_set<uint32> const& needed)
    {
        if (!creature->IsAlive() || creature->IsInCombat() || creature->GetOwnerGUID().IsPlayer() || !player->CanSeeOrDetect(creature))
            return false;

        if (creature->hasLootRecipient() && !creature->isTappedBy(player))
            return false;

        if (needed.count(creature->GetEntry()))
            return true;

        CreatureTemplate const* info = creature->GetCreatureTemplate();
        for (uint32 credit : info->KillCredit)
            if (credit && needed.count(credit))
                return true;

        return info->lootid && LootTemplates_Creature.HaveQuestLootForPlayer(info->lootid, player);
    }

    // A gameobject to use or loot for a quest: the same test that makes it sparkle in the client.
    // Quest givers are left out; they are the guide marker's job.
    bool IsQuestTarget(Player* player, GameObject* go)
    {
        if (go->GetEntry() == sMarkerEntry || go->GetEntry() == sTargetEntry)
            return false;

        if (!go->isSpawned() || go->getLootState() != GO_READY || go->GetGoType() == GAMEOBJECT_TYPE_QUESTGIVER
            || !player->CanSeeOrDetect(go))
            return false;

        return go->ActivateToQuest(player);
    }

    void UpdateTargetMarkers(Player* player, Marker* marker)
    {
        if (!marker->targetsEnabled && marker->targets.empty())
            return;

        Map* map = player->GetMap();
        std::vector<std::pair<float, WorldObject*>> found;

        if (sQuestTargets && marker->targetsEnabled && player->IsAlive() && !player->IsInFlight())
        {
            std::list<Creature*> creatures;
            std::list<GameObject*> objects;
            Acore::AllWorldObjectsInRange check(player, sTargetRange);
            Acore::CreatureListSearcher<Acore::AllWorldObjectsInRange> creatureSearcher(player, creatures, check);
            Cell::VisitObjects(player, creatureSearcher, sTargetRange);
            Acore::GameObjectListSearcher<Acore::AllWorldObjectsInRange> objectSearcher(player, objects, check);
            Cell::VisitObjects(player, objectSearcher, sTargetRange);

            std::unordered_set<uint32> needed = GetNeededCreatures(player);
            for (Creature* creature : creatures)
                if (IsQuestTarget(player, creature, needed))
                    found.emplace_back(player->GetExactDist(creature), creature);

            for (GameObject* go : objects)
                if (IsQuestTarget(player, go))
                    found.emplace_back(player->GetExactDist(go), go);

            std::sort(found.begin(), found.end(), [](auto const& a, auto const& b) { return a.first < b.first; });
            if (found.size() > sMaxTargets)
                found.resize(sMaxTargets);
        }

        std::vector<TargetMark> kept;
        for (auto const& candidate : found)
        {
            WorldObject* target = candidate.second;
            auto old = std::find_if(marker->targets.begin(), marker->targets.end(),
                [target](TargetMark const& mark) { return mark.target == target->GetGUID(); });

            // Keep the marker while the target stays put; one that walked off gets a new marker.
            if (old != marker->targets.end() && old->go && map->GetGameObject(old->go)
                && target->GetExactDist(old->x, old->y, old->z) <= FOLLOW_DISTANCE)
            {
                kept.push_back(*old);
                old->go.Clear();
                continue;
            }

            TargetMark mark;
            mark.target = target->GetGUID();
            mark.x = target->GetPositionX();
            mark.y = target->GetPositionY();
            mark.z = target->GetPositionZ();
            if (GameObject* spawned = SpawnMarker(player, sTargetEntry, mark.x, mark.y, mark.z, false))
            {
                mark.go = spawned->GetGUID();
                kept.push_back(mark);
            }
        }

        // Markers no longer wanted go now rather than on their own next check.
        for (TargetMark const& mark : marker->targets)
            if (mark.go)
                if (GameObject* go = map->GetGameObject(mark.go))
                    go->Delete();

        marker->targets = std::move(kept);
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
        sQuestTargets = sConfigMgr->GetOption<bool>("DrawEstuff.QuestTargets", true);
        sTargetEntry = sConfigMgr->GetOption<uint32>("DrawEstuff.TargetEntry", 1990002);
        sTargetRange = sConfigMgr->GetOption<float>("DrawEstuff.TargetRange", 60.0f);
        sMaxTargets = sConfigMgr->GetOption<uint32>("DrawEstuff.MaxTargets", 5);
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

    // Only players whose addon has sent a request have a Marker, so bots cost nothing here.
    void OnPlayerUpdate(Player* player, uint32 diff) override
    {
        Marker* marker = player->CustomData.Get<Marker>(MARKER_KEY);
        if (!marker || (!marker->active && !marker->targetsEnabled && marker->targets.empty()))
            return;

        if (marker->checkTimer > diff)
        {
            marker->checkTimer -= diff;
            return;
        }
        marker->checkTimer = CHECK_INTERVAL;

        if (!sEnabled || !player->IsInWorld())
        {
            marker->goGuid.Clear();
            marker->targets.clear();
            return;
        }

        UpdateGuideMarker(player, marker);
        UpdateTargetMarkers(player, marker);
    }
};

void AddDrawEstuffScripts()
{
    new DrawEstuffWorld();
    new DrawEstuffPlayer();
    RegisterGameObjectAI(DrawEstuffMarkerAI);
}
