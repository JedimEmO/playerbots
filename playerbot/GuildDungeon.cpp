#include "playerbot/playerbot.h"
#include "playerbot/GuildDungeon.h"
#include "playerbot/GuildBotMgr.h"
#include "playerbot/GuildRuns.h"
#include "playerbot/TravelMgr.h"
#include "playerbot/ServerFacade.h"
#include "Pools/PoolManager.h"
#include "AI/ScriptDevAI/ScriptDevAIMgr.h"
#include "AI/ScriptDevAI/include/sc_gossip.h"
#include "Grids/GridNotifiers.h"
#include "Grids/GridNotifiersImpl.h"
#include "Grids/CellImpl.h"

#include <algorithm>
#include <cctype>

using namespace ai;

std::mutex GuildDungeon::entryLock;
std::map<uint32, uint32> GuildDungeon::lastEntry;
std::mutex GuildDungeon::exitLock;
std::map<uint32, std::unique_ptr<PlaceTravelDestination>> GuildDungeon::exits;
std::map<uint32, std::unique_ptr<PlaceTravelDestination>> GuildDungeon::stones;
std::map<uint32, std::unique_ptr<PlaceTravelDestination>> GuildDungeon::usePlaces;
namespace
{
    // Where each dungeon's meeting stone stands, by dungeon map; built once from the spawns.
    std::map<uint32, WorldPosition> stonePlaces;
    bool stonesIndexed = false;
}

namespace
{
    // A party of five runs a five-player dungeon.
    constexpr uint32 PARTY_SIZE = 5;
    // Members this far from the leader are not yet with it.
    constexpr float NEAR_LEADER = 100.0f;
    // A boss this many levels above a member is still one the party can fight, as the boss travel purpose has it.
    constexpr uint32 BOSS_LEVELS_ABOVE = 3;
    // Members take the entrance at least this far apart, so the core binds the group to the instance once.
    constexpr uint32 ENTRY_GAP_MS = 3 * IN_MILLISECONDS;
    // Out by the way out within this, or the job fails.
    constexpr time_t LEAVE_SECONDS = 15 * MINUTE;
    // A member still inside this long after the leader came out is stranded, somewhere no route leads out of,
    // and goes home by hearthstone, as a player would; tried again this often while it is on cooldown.
    constexpr time_t STRANDED_SECONDS = 3 * MINUTE;
    constexpr time_t HEARTH_RETRY_SECONDS = MINUTE;
    // At the stone: the leader uses it within this, and a member this near it follows the leader in by it.
    // The member's reach is wide: some stones stand on a ledge above the road, out of reach of the paths
    // below, where a member that fought its way off the ledge cannot climb back.
    constexpr float AT_STONE = 8.0f;
    constexpr float NEAR_STONE = 120.0f;
    // A follower farther than this from its leader on the same map travels to it, by the travel map's routes.
    constexpr float FOLLOW_BY_TRAVEL = 150.0f;

    // Why the stone last turned each member away, so the event log has each new reason once.
    std::mutex refusalLock;
    std::map<uint32, std::string> stoneRefusals;

    // The last boss, where the boss destinations alone would not say which it is.
    const std::map<uint32, int32> LAST_BOSS = {
        { 389, 11519 },  // Ragefire Chasm: Bazzalan
        { 36, 639 },     // The Deadmines: Edwin VanCleef
        { 43, 5775 },    // Wailing Caverns: Verdan the Everliving; Mutanus comes only with the Naralex escort
        { 33, 4275 },    // Shadowfang Keep: Archmage Arugal
        { 48, 4829 },    // Blackfathom Deeps: Aku'mai
        { 34, 1716 },    // The Stockade: Bazil Thredd
        { 47, 4421 },    // Razorfen Kraul: Charlga Razorflank
        { 90, 7800 },    // Gnomeregan: Mekgineer Thermaplugg
        { 129, 7358 },   // Razorfen Downs: Amnennar the Coldbringer
        { 189, 3977 },   // Scarlet Monastery: High Inquisitor Whitemane, in the Cathedral
        { 70, 2748 },    // Uldaman: Archaedas
        { 209, 7267 },   // Zul'Farrak: Chief Ukorz Sandscalp
        { 349, 12201 },  // Maraudon: Princess Theradras
    };

    // A dungeon with wings, each its own entrance on one map, is run a wing at a time, by the wing's name.
    // Its bosses are those nearest the wing's arrival; the table names the wing's entrance and last boss.
    struct Wing { const char* name; const char* title; uint32 trigger; int32 lastBoss; };
    const Wing WINGS[] = {
        { "scarlet_monastery_graveyard", "Scarlet Monastery Graveyard", 45, 4543 },   // Bloodmage Thalnos
        { "scarlet_monastery_library", "Scarlet Monastery Library", 614, 6487 },      // Arcanist Doan
        { "scarlet_monastery_armory", "Scarlet Monastery Armory", 612, 3975 },        // Herod
        { "scarlet_monastery_cathedral", "Scarlet Monastery Cathedral", 610, 3977 },  // High Inquisitor Whitemane
    };

    // Friendly creatures a party has to speak to on its way, as a player would, with the gossip action to pick,
    // and the door the creature then opens, or 0. Each is a step among the bosses, in route order;
    // the party takes the one friendly to it.
    struct Talk { uint32 map; int32 entry; uint32 action; uint32 door; };
    const Talk TALKS[] = {
        // Shadowfang Keep: the prisoner, freed, walks to the Courtyard Door and unlocks it. Horde and Alliance have one each.
        { 33, 3849, GOSSIP_ACTION_INFO_DEF + 1, 18895 },   // Deathstalker Adamant
        { 33, 3850, GOSSIP_ACTION_INFO_DEF + 1, 18895 },   // Sorcerer Ashcrombe
    };
    // The leader speaks from this near, out of combat. A step with no door is done then;
    // one with a door is done when the door is open, and the party waits by the creature while it walks there.
    constexpr float TALK_DISTANCE = 6.0f;
    // A creature that offers its gossip again with the door still shut is spoken to again after this.
    constexpr time_t TALK_RETRY_SECONDS = 15;
    // A creature that walks off and leaves the door shut this long is given up on.
    constexpr time_t TALK_WAIT_SECONDS = 3 * MINUTE;

    // Objects a party uses before a boss, as players would: how many members use it, and the boss it is for.
    // An object that wakes its boss is used for as long as the boss sleeps, unattackable and out of combat,
    // so it is used again after a wipe puts the boss back to sleep; any other is used once.
    struct UseObject { uint32 map; uint32 entry; uint32 users; int32 before; bool wakes; };
    const UseObject USES[] = {
        // Blackfathom Deeps: the four Fires of Aku'mai, lit once Kelris is dead, open the way to Aku'mai.
        { 48, 21118, 1, 4829, false },
        { 48, 21119, 1, 4829, false },
        { 48, 21120, 1, 4829, false },
        { 48, 21121, 1, 4829, false },
        // Uldaman: the Altar of Archaedas is a ritual for three. Its spell wakes him, and he attacks the first user.
        { 70, 133234, 3, 2748, true },
    };
    // The leader uses an object this near it, and members this near the leader come to it to join.
    constexpr float USE_DISTANCE = 5.0f;
    constexpr float USE_JOIN_DISTANCE = 20.0f;
    // A member joining stands this near the object, inside the distance the core takes a use from.
    constexpr float USE_STAND = 2.0f;
    // An object that has not woken its boss yet is used again after this.
    constexpr time_t USE_RETRY_SECONDS = 15;

    // Creatures with no spawn of their own, brought by an event, that a party kills before a boss, as players would:
    // the creature, the boss, and the door their deaths open. The step is done when none is alive near the leader,
    // or when the door is open.
    struct KillFirst { uint32 map; uint32 entry; int32 before; uint32 door; };
    const KillFirst KILLS[] = {
        // Shadowfang Keep: Fenrus's death brings Arugal's four voidwalkers, and the Sorcerer's Gate opens when all four die.
        { 33, 4627, 3927, 18972 },
    };
    // The leader looks this far for them.
    constexpr float KILL_RANGE = 60.0f;

    // Letters only, in lower case, without a leading "the": "The Deadmines" and "deadmines" are one name.
    std::string Normalize(const std::string& name)
    {
        std::string out;
        for (char ch : name)
            if (std::isalpha((unsigned char)ch))
                out += char(std::tolower((unsigned char)ch));
        if (out.rfind("the", 0) == 0 && out.size() > 3)
            out = out.substr(3);
        return out;
    }

    std::string BossName(int32 entry)
    {
        CreatureInfo const* info = entry > 0 ? ObjectMgr::GetCreatureTemplate(entry) : nullptr;
        return info ? std::string(info->Name) : std::to_string(entry);
    }

    std::string MemberName(Group const* group, ObjectGuid guid)
    {
        for (auto& slot : group->GetMemberSlots())
            if (slot.guid == guid)
                return slot.name;
        return "a member";
    }
}

namespace
{
    const Wing* FindWing(const std::string& name)
    {
        const std::string wanted = Normalize(name);
        for (const Wing& wing : WINGS)
            if (Normalize(wing.name) == wanted)
                return &wing;
        return nullptr;
    }
}

std::vector<AreaTrigger const*> GuildDungeon::Entrances(uint32 mapId)
{
    std::vector<AreaTrigger const*> entrances;
    for (uint32 i = 0; i < sAreaTriggerStore.GetNumRows(); ++i)
    {
        AreaTriggerEntry const* entry = sAreaTriggerStore.LookupEntry(i);
        if (!entry || entry->mapid == mapId)
            continue;
        AreaTrigger const* at = sObjectMgr.GetAreaTrigger(entry->id);
        if (at && at->target_mapId == mapId)
            entrances.push_back(at);
    }
    return entrances;
}

AreaTrigger const* GuildDungeon::EntranceNear(uint32 mapId, float x, float y, float z)
{
    AreaTrigger const* nearest = nullptr;
    float best = 0.0f;
    for (AreaTrigger const* at : Entrances(mapId))
    {
        const float dx = at->target_X - x, dy = at->target_Y - y, dz = at->target_Z - z;
        const float distance = dx * dx + dy * dy + dz * dz;
        if (!nearest || distance < best)
        {
            nearest = at;
            best = distance;
        }
    }
    return nearest ? nearest : sObjectMgr.GetMapEntranceTrigger(mapId);
}

uint32 GuildDungeon::FindMap(const std::string& name)
{
    if (const Wing* wing = FindWing(name))
        if (AreaTrigger const* at = sObjectMgr.GetAreaTrigger(wing->trigger))
            return at->target_mapId;

    const std::string wanted = Normalize(name);
    if (wanted.empty())
        return 0;

    for (uint32 i = 0; i < sMapStore.GetNumRows(); ++i)
    {
        MapEntry const* map = sMapStore.LookupEntry(i);
        if (map && map->IsNonRaidDungeon() && Normalize(map->name[0]) == wanted)
            return map->MapID;
    }
    return 0;
}

std::string GuildDungeon::Validate(const std::string& name)
{
    if (name.empty())
        return "dungeon needs name=<dungeon>, such as name=deadmines";

    uint32 mapId = FindMap(name);
    if (!mapId)
        return "no five-player dungeon called " + name;

    if (!sObjectMgr.GetMapEntranceTrigger(mapId))
        return "no entrance known for " + name;

    return "";
}

bool GuildDungeon::Resolve(const std::string& name, Player* leader, Dungeon& dungeon, std::string& error)
{
    error = Validate(name);
    if (!error.empty())
        return false;

    dungeon.mapId = FindMap(name);
    const Wing* wing = FindWing(name);
    dungeon.name = wing ? wing->title : sMapStore.LookupEntry(dungeon.mapId)->name[0];
    dungeon.entrance = wing ? sObjectMgr.GetAreaTrigger(wing->trigger) : sObjectMgr.GetMapEntranceTrigger(dungeon.mapId);
    const bool wings = Entrances(dungeon.mapId).size() > 1;

    // The bosses are the boss travel destinations on the dungeon's map: its elites with one spawn.
    // Nearest the arrival point first; the boss step will order them by route.
    const AreaTrigger* at = dungeon.entrance;
    WorldPosition arrival(dungeon.mapId, at->target_X, at->target_Y, at->target_Z, at->target_Orientation);
    std::vector<std::pair<float, int32>> bosses;
    for (TravelDestination* dest : sTravelMgr.GetDestinations(PlayerTravelInfo(leader), (uint32)TravelDestinationPurpose::Boss, {}, false, 0))
    {
        if (dest->GetEntry() <= 0)
            continue;
        // Only enemies: an escort or a prisoner, such as the Disciple of Naralex or Deathstalker Adamant, is an elite
        // with one spawn too, but friendly, and the party would stand by it waiting for it to die.
        CreatureInfo const* bossInfo = ObjectMgr::GetCreatureTemplate(dest->GetEntry());
        FactionTemplateEntry const* bossFaction = bossInfo ? sFactionTemplateStore.LookupEntry(bossInfo->Faction) : nullptr;
        FactionTemplateEntry const* ownFaction = leader->GetFactionTemplateEntry();
        const bool hostile = bossFaction && ownFaction && (bossFaction->IsHostileTo(*ownFaction) || bossFaction->IsHostileToPlayers());
        const Talk* talk = nullptr;
        for (const Talk& t : TALKS)
            if (t.map == dungeon.mapId && t.entry == dest->GetEntry())
                talk = &t;
        // A talk step only for the party it is friendly to; any other friendly creature is no step at all.
        if (talk ? hostile : !hostile)
            continue;
        if (talk)
        {
            dungeon.talks[talk->entry] = talk->action;
            if (talk->door)
            {
                auto findDoor = [&](auto const& pair)
                {
                    if (pair.second.id != talk->door || pair.second.mapid != dungeon.mapId)
                        return false;
                    dungeon.talkDoors[talk->entry] = pair.first;
                    return true;
                };
                sObjectMgr.DoGOData(findDoor);
            }
        }
        // The destination's point nearest the arrival: on the dungeon's map if it has one there.
        std::list<uint8> nearest = { 0, 0, 0 };
        WorldPosition* point = dest->GetNextPoint(arrival, nearest);
        if (!point || point->getMapId() != dungeon.mapId)
            continue;
        // In a dungeon with wings, only the bosses of this run's wing: nearest its arrival of all the entrances'.
        if (wings && EntranceNear(dungeon.mapId, point->getX(), point->getY(), point->getZ()) != dungeon.entrance)
            continue;
        bosses.push_back({ point->distance(arrival), dest->GetEntry() });
    }
    std::sort(bosses.begin(), bosses.end());
    for (auto& [distance, entry] : bosses)
    {
        // Its spawn on the map tells whether it is dead in an instance; a boss with no spawn of its own,
        // one a script summons, cannot be told apart and is left out.
        uint32 spawn = 0;
        auto findSpawn = [&](auto const& pair)
        {
            if (pair.second.id != uint32(entry) || pair.second.mapid != dungeon.mapId)
                return false;
            spawn = pair.first;
            return true;
        };
        sObjectMgr.DoCreatureData(findSpawn);
        if (!spawn)
            continue;
        dungeon.bosses.push_back(entry);
        dungeon.spawns.push_back(spawn);
    }

    if (dungeon.bosses.empty())
    {
        error = "no bosses known in " + dungeon.name;
        return false;
    }

    // The way out: where the trigger that leads out of the dungeon puts a player, outside by the entrance.
    AreaTrigger const* back = sObjectMgr.GetGoBackTrigger(dungeon.mapId);
    if (!back)
    {
        error = "no way out known for " + dungeon.name;
        return false;
    }
    {
        std::lock_guard<std::mutex> guard(exitLock);
        auto& exit = exits[dungeon.mapId];
        if (!exit)
            exit = std::make_unique<PlaceTravelDestination>(
                WorldPosition(back->target_mapId, back->target_X, back->target_Y, back->target_Z, back->target_Orientation),
                "out of " + dungeon.name);
        dungeon.exit = exit.get();
    }

    // Objects to use on the way, where the map has their spawns.
    for (const UseObject& use : USES)
    {
        if (use.map != dungeon.mapId)
            continue;
        Dungeon::Use step;
        step.entry = use.entry;
        step.users = use.users;
        step.before = use.before;
        step.wakes = use.wakes;
        WorldPosition place;
        auto findSpawn = [&](auto const& pair)
        {
            if (pair.second.id != use.entry || pair.second.mapid != dungeon.mapId)
                return false;
            step.spawn = pair.first;
            place = WorldPosition(pair.second.mapid, pair.second.posX, pair.second.posY, pair.second.posZ, pair.second.orientation);
            return true;
        };
        sObjectMgr.DoGOData(findSpawn);
        if (!step.spawn)
            continue;
        std::lock_guard<std::mutex> guard(exitLock);
        auto& destination = usePlaces[step.spawn];
        if (!destination)
        {
            GameObjectInfo const* info = ObjectMgr::GetGameObjectInfo(use.entry);
            destination = std::make_unique<PlaceTravelDestination>(place, std::string("the ") + (info ? info->name : "object") + " in " + dungeon.name);
        }
        step.place = destination.get();
        dungeon.uses.push_back(step);
    }

    // Creatures to kill on the way, with the spawn of the door they open, where the map has it.
    for (const KillFirst& kill : KILLS)
    {
        if (kill.map != dungeon.mapId)
            continue;
        Dungeon::Kill step;
        step.entry = kill.entry;
        step.before = kill.before;
        if (kill.door)
        {
            auto findDoor = [&](auto const& pair)
            {
                if (pair.second.id != kill.door || pair.second.mapid != dungeon.mapId)
                    return false;
                step.doorSpawn = pair.first;
                return true;
            };
            sObjectMgr.DoGOData(findDoor);
        }
        dungeon.kills.push_back(step);
    }

    // The meeting stone; StonePlace takes the same lock, so this comes after it is let go.
    WorldPosition stonePlace;
    if (StonePlace(dungeon.mapId, stonePlace))
    {
        std::lock_guard<std::mutex> guard(exitLock);
        auto& stone = stones[dungeon.mapId];
        if (!stone)
            stone = std::make_unique<PlaceTravelDestination>(stonePlace, "the meeting stone of " + dungeon.name);
        dungeon.stone = stone.get();
    }

    // The party must be near the last boss's level: the one the table names, or else the highest.
    auto last = LAST_BOSS.find(dungeon.mapId);
    const int32 wingLast = wing ? wing->lastBoss : 0;
    uint32 highest = 0;
    for (int32 entry : dungeon.bosses)
    {
        CreatureInfo const* info = ObjectMgr::GetCreatureTemplate(entry);
        if (!info)
            continue;
        highest = std::max(highest, info->MaxLevel);
        if (wingLast ? entry == wingLast : last != LAST_BOSS.end() && entry == last->second)
        {
            dungeon.bossLevel = info->MaxLevel;
            dungeon.lastBoss = entry;
        }
    }
    if (!dungeon.bossLevel)
        dungeon.bossLevel = highest;
    // Without a table entry, the boss farthest from the entrance ends the run.
    if (!dungeon.lastBoss)
        dungeon.lastBoss = dungeon.bosses.back();

    // The last boss goes last, or the bosses farther from the entrance than it, such as Cookie, are never visited.
    for (size_t i = 0; i + 1 < dungeon.bosses.size(); ++i)
    {
        if (dungeon.bosses[i] != dungeon.lastBoss)
            continue;
        std::rotate(dungeon.bosses.begin() + i, dungeon.bosses.begin() + i + 1, dungeon.bosses.end());
        std::rotate(dungeon.spawns.begin() + i, dungeon.spawns.begin() + i + 1, dungeon.spawns.end());
        break;
    }

    return true;
}

std::string GuildDungeon::CheckParty(Player* leader, const Dungeon& dungeon, bool& failed)
{
    failed = false;

    Group* group = leader->GetGroup();
    if (!group)
        return "waiting for a party";
    if (!group->IsLeader(leader->GetObjectGuid()))
        return "waiting to lead its group";
    if (group->IsRaidGroup())
    {
        failed = true;
        return "in a raid, not a party";
    }
    if (group->GetMembersCount() < PARTY_SIZE)
        return "waiting for a party of " + std::to_string(PARTY_SIZE) + ", has " + std::to_string(group->GetMembersCount());

    for (auto& slot : group->GetMemberSlots())
    {
        Player* member = sObjectMgr.GetPlayer(slot.guid);
        if (!member || !member->IsInWorld())
            return "waiting for " + slot.name + " to be online";
    }

    const uint32 minLevel = dungeon.bossLevel > BOSS_LEVELS_ABOVE ? dungeon.bossLevel - BOSS_LEVELS_ABOVE : 0;
    for (auto& slot : group->GetMemberSlots())
    {
        Player* member = sObjectMgr.GetPlayer(slot.guid);
        const std::string& who = slot.name;

        if (member->GetLevel() < minLevel)
        {
            failed = true;
            return who + " is level " + std::to_string(member->GetLevel()) + "; " + dungeon.name + " needs " + std::to_string(minLevel);
        }

        // The core's own check at the entrance: level, key, quest, and the new instances an account may enter in an hour.
        uint32 misc = 0;
        switch (member->GetAreaTriggerLockStatus(dungeon.entrance, misc))
        {
        case AREA_LOCKSTATUS_OK:
            break;
        case AREA_LOCKSTATUS_ZONE_IN_COMBAT:
            return dungeon.name + " has a fight in progress";
        case AREA_LOCKSTATUS_TOO_LOW_LEVEL:
            failed = true;
            return who + " is below level " + std::to_string(misc) + " for " + dungeon.name;
        case AREA_LOCKSTATUS_MISSING_ITEM:
        {
            ItemPrototype const* item = sObjectMgr.GetItemPrototype(misc);
            failed = true;
            return who + " lacks " + (item ? std::string(item->Name1) : "item " + std::to_string(misc)) + " for " + dungeon.name;
        }
        case AREA_LOCKSTATUS_QUEST_NOT_COMPLETED:
            failed = true;
            return who + " has not done quest " + std::to_string(misc) + " for " + dungeon.name;
        case AREA_LOCKSTATUS_TOO_MANY_INSTANCE:
            failed = true;
            return who + "'s account has entered its limit of new instances this hour";
        default:
            failed = true;
            return who + " may not enter " + dungeon.name;
        }

        if (sServerFacade.UnitIsDead(member))
            return "waiting for " + who + " to be alive";

        if (member != leader && (member->GetMapId() != leader->GetMapId() || member->GetDistance(leader) > NEAR_LEADER))
            return "waiting for " + who + " to come near";
    }

    return "";
}

namespace
{
    // Upstream marks a travel purpose that once found no destination as having none, until something else clears it,
    // and then never asks again; a run that wants to travel and has no target asks afresh.
    void AllowTravelRequest(Player* leader)
    {
        PlayerbotAI* ai = leader->GetPlayerbotAI();
        if (!ai)
            return;
        AiObjectContext* context = ai->GetAiObjectContext();
        if (!context->GetValue<TravelTarget*>("travel target")->Get()->IsActive())
            context->GetValue<bool>("no active travel destinations", "dungeon")->Set(false);
    }
}

GuildDungeon::Update GuildDungeon::Tick(Player* leader, const std::string& name)
{
    Update update;
    std::lock_guard<std::mutex> guard(lock);
    AllowTravelRequest(leader);

    Run& run = runs[leader->GetGUIDLow()];
    if (!run.resolved)
    {
        std::string error;
        if (!Resolve(name, leader, run.dungeon, error))
        {
            runs.erase(leader->GetGUIDLow());
            update.status = "failed";
            update.reason = error;
            return update;
        }
        run.resolved = true;

        // A run cleared before a restart: the group is still bound to the instance, whose saved state has the last
        // boss dead. It has to finish, getting everyone out and resetting, before a new run can go in.
        Group* bound = leader->GetGroup();
        InstanceGroupBind* bind = bound ? bound->GetBoundInstance(run.dungeon.mapId) : nullptr;
        if (bind && bind->state)
        {
            for (size_t i = 0; i < run.dungeon.bosses.size(); ++i)
            {
                if (run.dungeon.bosses[i] != run.dungeon.lastBoss || bind->state->GetCreatureRespawnTime(run.dungeon.spawns[i]) <= time(nullptr))
                    continue;
                run.phase = Phase::Leave;
                run.leaveStarted = time(nullptr);
                run.cleared = run.dungeon.name + " cleared to " + BossName(run.dungeon.lastBoss) + " before a restart";
                update.strategiesChanged = true;
            }
        }
    }

    if (run.phase == Phase::Leave)
    {
        TickLeave(leader, run, update);
        if (update.status != "working")
            runs.erase(leader->GetGUIDLow());
        return update;
    }

    const Dungeon& dungeon = run.dungeon;
    Group* group = leader->GetGroup();

    if (leader->GetMapId() == dungeon.mapId)
    {
        uint32 members = group ? group->GetMembersCount() : 1, inside = 0;
        if (group)
        {
            for (auto& slot : group->GetMemberSlots())
            {
                Player* member = sObjectMgr.GetPlayer(slot.guid);
                if (member && member->IsInWorld() && !member->IsBeingTeleportedFar() && member->GetMapId() == dungeon.mapId)
                    inside++;
            }
        }
        else
            inside = 1;
        const bool all = inside == members && members >= PARTY_SIZE;

        if (!run.entered && all)
        {
            run.entered = true;
            update.events.push_back({ "dungeon_entered", dungeon.name });
        }

        // The whole party is in: on to the bosses.
        if (run.entered && all)
        {
            if (run.phase != Phase::Bosses)
            {
                run.phase = Phase::Bosses;
                run.nextBoss = 0;
                update.strategiesChanged = true;
            }
            TickBosses(leader, run, update);
            return update;
        }

        // Not all of it: the leader holds where it is while the others come in, one at a time.
        if (run.phase != Phase::Hold)
        {
            run.phase = Phase::Hold;
            update.strategiesChanged = true;
            if (PlayerbotAI* ai = leader->GetPlayerbotAI())
                sTravelMgr.SetNullTravelTarget(ai->GetAiObjectContext()->GetValue<TravelTarget*>("travel target")->Get());
        }
        update.reason = "entering " + dungeon.name + ", " + std::to_string(inside) + " of " + std::to_string(members) + " inside";
        return update;
    }

    // Outside: back from inside, say after a release, the leader checks the party again and travels.
    if (run.phase == Phase::Hold)
    {
        run.phase = Phase::Check;
        update.strategiesChanged = true;
    }

    bool failed = false;
    std::string why = CheckParty(leader, dungeon, failed);
    if (failed)
    {
        runs.erase(leader->GetGUIDLow());
        update.status = "failed";
        update.reason = why;
        return update;
    }
    // Back outside after the party has been in, after a death or a wrong turn, it goes back in to them:
    // they are inside, and will not come near out here.
    if (run.entered && !why.empty())
    {
        why.clear();
        if (run.phase != Phase::Go)
        {
            run.phase = Phase::Go;
            update.strategiesChanged = true;
        }
    }

    if (run.phase == Phase::Check && why.empty())
        run.phase = Phase::Go;

    // With a meeting stone, the leader goes to it and in by it, once the party is with it.
    if (run.phase == Phase::Go && dungeon.stone && why.empty() && dungeon.stone->DistanceTo(WorldPosition(leader)) <= AT_STONE)
    {
        std::string refused;
        if (!EnterByStone(leader, dungeon.mapId, refused, dungeon.entrance) && !refused.empty())
            why = refused;
    }

    // Once on the way, a member falling behind does not stop the leader: it waits at the entrance
    // until the party is near, as upstream's travel does for dungeon destinations.
    if (run.phase == Phase::Go)
        update.reason = (run.entered ? "going back into " : "travelling to ") + dungeon.name + (why.empty() ? "" : "; " + why);
    else
        update.reason = why;
    return update;
}

bool GuildDungeon::BossesOf(uint32 mapId, Player* member, Bosses& out)
{
    MapEntry const* map = sMapStore.LookupEntry(mapId);
    if (!map || !map->IsNonRaidDungeon())
        return false;
    Dungeon dungeon;
    std::string error;
    if (!Resolve(map->name[0], member, dungeon, error))
        return false;
    out.name = dungeon.name;
    out.entries = dungeon.bosses;
    out.spawns = dungeon.spawns;
    out.last = dungeon.lastBoss;
    return true;
}

bool GuildDungeon::BossDead(Map* map, uint32 spawn)
{
    if (!map || !spawn)
        return false;
    // Loaded: the creature itself. Its grid unloaded: the respawn time the core saved at its death.
    if (Creature* creature = map->GetCreature(spawn))
        return !creature->IsAlive();
    MapPersistentState* state = map->GetPersistentState();
    return state && state->GetCreatureRespawnTime(spawn) > time(nullptr);
}

bool GuildDungeon::BossSpawned(Map* map, uint32 spawn)
{
    if (!map || !spawn || !sPoolMgr.IsPartOfAPool<Creature>(spawn))
        return true;
    MapPersistentState* state = map->GetPersistentState();
    return !state || state->IsSpawnedPoolObject<Creature>(spawn);
}

void GuildDungeon::TickBosses(Player* leader, Run& run, Update& update)
{
    const Dungeon& dungeon = run.dungeon;
    Map* map = leader->GetMap();

    int32 next = 0;
    uint32 present = 0;
    std::string talkWait;  // what the party waits for after speaking, such as a prisoner on its way to a door
    for (size_t i = 0; i < dungeon.bosses.size(); ++i)
    {
        int32 entry = dungeon.bosses[i];
        // A creature to speak to: a step until it is done, and not a boss to count.
        if (auto talk = dungeon.talks.find(entry); talk != dungeon.talks.end())
        {
            if (run.talked.count(entry))
                continue;
            // The door it opens is open: done, whoever opened it.
            auto doorSpawn = dungeon.talkDoors.find(entry);
            const bool hasDoor = doorSpawn != dungeon.talkDoors.end();
            GameObject* door = hasDoor ? map->GetGameObject(doorSpawn->second) : nullptr;
            const std::string doorName = door ? std::string(door->GetGOInfo()->name) : "door";
            if (door && door->GetGoState() != GO_STATE_READY)
            {
                run.talked.insert(entry);
                // A door open before this run started, such as after a restart, is not news.
                if (run.bossesSeen)
                    update.events.push_back({ "door_open", doorName });
                continue;
            }
            Creature* creature = map->GetCreature(dungeon.spawns[i]);
            if (!creature || !creature->IsAlive())
            {
                run.talked.insert(entry);
                continue;
            }
            if (next)
                continue;
            next = entry;
            run.nextSpawn = dungeon.spawns[i];

            const time_t now = time(nullptr);
            auto spoken = run.talkedAt.find(entry);
            const bool offers = creature->HasFlag(UNIT_NPC_FLAGS, UNIT_NPC_FLAG_GOSSIP);
            if (hasDoor && !offers)
            {
                // Its escort is under way, after the leader spoke or before a restart: the party waits here
                // while it walks to the door.
                if (spoken == run.talkedAt.end())
                    spoken = run.talkedAt.emplace(entry, now).first;
                if (now - spoken->second >= TALK_WAIT_SECONDS)
                {
                    run.talked.insert(entry);
                    update.events.push_back({ "gave_up", BossName(entry) + " did not open the " + doorName });
                    next = 0;
                    run.nextSpawn = 0;
                    continue;
                }
                talkWait = BossName(entry) + " to open the " + doorName;
                continue;
            }
            if (leader->GetDistance(creature) <= TALK_DISTANCE && !leader->IsInCombat() && offers
                && (spoken == run.talkedAt.end() || now - spoken->second >= TALK_RETRY_SECONDS))
            {
                // As a player picks the option in its gossip: the script does the rest, such as an escort to a door.
                sScriptDevAIMgr.OnGossipSelect(leader, creature, GOSSIP_SENDER_MAIN, talk->second, nullptr);
                run.talkedAt[entry] = now;
                update.events.push_back({ "talked", BossName(entry) });
                if (!hasDoor)
                {
                    run.talked.insert(entry);
                    next = 0;
                    run.nextSpawn = 0;
                }
            }
            continue;
        }
        if (BossDead(map, dungeon.spawns[i]))
        {
            present++;
            // Bosses dead before this run started, such as before a restart, are not news.
            if (run.killed.insert(entry).second && run.bossesSeen)
                update.events.push_back({ "boss_killed", BossName(entry) });
            continue;
        }
        // A rare in a spawn pool, such as Miner Johnson, that the pool did not spawn in this instance.
        if (!BossSpawned(map, dungeon.spawns[i]))
            continue;
        present++;
        if (!next)
        {
            next = entry;
            run.nextSpawn = dungeon.spawns[i];
        }
    }

    run.bossesSeen = true;
    const std::string count = std::to_string(run.killed.size()) + " of " + std::to_string(present) + " bosses down";
    if (run.killed.count(dungeon.lastBoss) || !next)
    {
        // Cleared: out by the way out, and the reset.
        run.cleared = dungeon.name + " cleared to " + BossName(dungeon.lastBoss) + ", " + count;
        update.events.push_back({ "dungeon_done", dungeon.name + ", " + count });
        run.phase = Phase::Leave;
        run.leaveStarted = time(nullptr);
        run.nextBoss = 0;
        run.nextSpawn = 0;
        update.strategiesChanged = true;
        TickLeave(leader, run, update);
        return;
    }

    // Creatures an event brought that stand between the party and the next boss: the leader takes the nearest
    // as its target, on its way or not. Done once none is alive near the leader, or the door they open is open.
    run.nextBoss = next;
    run.killGuid = ObjectGuid();
    std::string killing;
    for (const Dungeon::Kill& kill : dungeon.kills)
    {
        if (kill.before != next)
            continue;
        if (kill.doorSpawn)
            if (GameObject* door = map->GetGameObject(kill.doorSpawn))
                if (door->GetGoState() != GO_STATE_READY)
                    continue;
        std::list<Unit*> found;
        MaNGOS::AllCreaturesOfEntryInRangeCheck check(leader, kill.entry, KILL_RANGE);
        MaNGOS::UnitListSearcher<MaNGOS::AllCreaturesOfEntryInRangeCheck> searcher(found, check);
        Cell::VisitAllObjects(leader, searcher, KILL_RANGE);
        Unit* nearest = nullptr;
        for (Unit* unit : found)
            if (unit->IsAlive() && unit->IsInWorld() && (!nearest || leader->GetDistance(unit) < leader->GetDistance(nearest)))
                nearest = unit;
        if (!nearest)
            continue;
        run.killGuid = nearest->GetObjectGuid();
        killing = BossName(kill.entry);
        break;
    }

    // An object to use before the next boss, such as an altar that wakes it: the leader goes to it first,
    // and it and as many members as the object needs use it, as players would.
    run.useSpawn = 0;
    const time_t now = time(nullptr);
    for (const Dungeon::Use& use : dungeon.uses)
    {
        if (use.before != next)
            continue;
        if (use.wakes)
        {
            // Due while the boss sleeps, unattackable and out of combat; done while it is awake.
            Creature* boss = map->GetCreature(run.nextSpawn);
            if (!boss || !boss->IsAlive() || boss->IsInCombat() || !boss->HasFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_UNINTERACTIBLE))
                continue;
        }
        else if (run.used.count(use.spawn))
            continue;
        GameObject* object = map->GetGameObject(use.spawn);
        if (!object)
        {
            run.used.insert(use.spawn);
            continue;
        }
        const std::string name = object->GetGOInfo()->name;
        run.useSpawn = use.spawn;
        if (leader->GetDistance(object) <= USE_DISTANCE)
        {
            // Everyone has used it and the boss still sleeps: again, after a while.
            std::set<uint32>& usedBy = run.usedBy[use.spawn];
            if (usedBy.size() >= use.users)
            {
                if (now - run.usedAt[use.spawn] < USE_RETRY_SECONDS)
                {
                    update.reason = "used the " + name + " in " + dungeon.name + ", waiting for " + BossName(next) + " to wake";
                    return;
                }
                usedBy.clear();
            }

            // The core takes a use only from a user within the object's reach who can use it now.
            // The core keeps each user of a ritual until it has enough, so members use it as they come.
            auto canUse = [&](Player* user)
            {
                return user->IsInWorld() && user->IsAlive() && user->GetMapId() == leader->GetMapId()
                    && object->IsWithinDistInMap(user, object->GetInteractionDistance()) && object->CanUseNow(user);
            };
            auto useBy = [&](Player* user)
            {
                WorldPacket packet(CMSG_GAMEOBJ_USE);
                packet << object->GetObjectGuid();
                user->GetSession()->HandleGameObjectUseOpcode(packet);
                usedBy.insert(user->GetGUIDLow());
            };
            // The leader first: a ritual's first user casts its spell, and a boss it wakes attacks that user.
            if (!usedBy.count(leader->GetGUIDLow()))
            {
                if (!canUse(leader))
                {
                    update.reason = "at the " + name + " in " + dungeon.name + ", waiting to use it";
                    return;
                }
                useBy(leader);
            }
            if (Group* group = leader->GetGroup())
                for (auto& slot : group->GetMemberSlots())
                    if (Player* member = sObjectMgr.GetPlayer(slot.guid))
                        if (usedBy.size() < use.users && !usedBy.count(member->GetGUIDLow()) && canUse(member))
                            useBy(member);
            if (usedBy.size() < use.users)
            {
                update.reason = "at the " + name + " in " + dungeon.name + ", " + std::to_string(usedBy.size()) + " of "
                    + std::to_string(use.users) + " have used it";
                return;
            }
            run.usedAt[use.spawn] = now;
            update.events.push_back({ "used", name });
            if (!use.wakes)
            {
                run.used.insert(use.spawn);
                continue;
            }
            update.reason = "used the " + name + " in " + dungeon.name + ", waiting for " + BossName(next) + " to wake";
            return;
        }
        if (PlayerbotAI* ai = leader->GetPlayerbotAI())
        {
            TravelTarget* target = ai->GetAiObjectContext()->GetValue<TravelTarget*>("travel target")->Get();
            if (target && target->IsActive() && target->GetDestination() != use.place)
                target->SetStatus(TravelStatus::TRAVEL_STATUS_EXPIRED);
        }
        update.reason = "on the way to use the " + name + " in " + dungeon.name + ", " + count;
        return;
    }

    // Any travel target but the next boss goes, at a boss that died or one that replaced it, such as a grind spot:
    // upstream asks for no travel target while one is active, so the dungeon's request could not take it back.
    if (PlayerbotAI* ai = leader->GetPlayerbotAI())
    {
        TravelTarget* target = ai->GetAiObjectContext()->GetValue<TravelTarget*>("travel target")->Get();
        if (target && target->IsActive() && target->GetEntry() != next)
            target->SetStatus(TravelStatus::TRAVEL_STATUS_EXPIRED);
    }
    if (!talkWait.empty())
        update.reason = "waiting for " + talkWait + " in " + dungeon.name + ", " + count;
    else if (!killing.empty())
        update.reason = "killing " + killing + " before " + BossName(next) + " in " + dungeon.name + ", " + count;
    else
        update.reason = (dungeon.talks.count(next) ? "on the way to speak to " : "on the way to ") + BossName(next) + " in " + dungeon.name + ", " + count;
}

void GuildDungeon::TickLeave(Player* leader, Run& run, Update& update)
{
    const Dungeon& dungeon = run.dungeon;
    Group* group = leader->GetGroup();

    const time_t now = time(nullptr);
    const bool leaderOut = leader->GetMapId() != dungeon.mapId;
    if (!leaderOut)
        run.leaderOutAt = 0;
    else if (!run.leaderOutAt)
        run.leaderOutAt = now;
    const bool stranded = leaderOut && now - run.leaderOutAt >= STRANDED_SECONDS && now - run.lastHearth >= HEARTH_RETRY_SECONDS;

    uint32 inside = 0, guildInside = 0;
    std::string playerInside;
    if (group)
    {
        for (auto& slot : group->GetMemberSlots())
        {
            Player* member = sObjectMgr.GetPlayer(slot.guid);
            if (!member || !(member->GetMapId() == dungeon.mapId || (member->IsBeingTeleportedFar() && member->GetTeleportDest().mapid == dungeon.mapId)))
                continue;
            inside++;
            if (sGuildBotMgr.IsGuildBot(member))
                guildInside++;
            else
                playerInside = member->GetName();
            // Left behind where no route leads out, such as caught on the Deadmines ship: home by hearthstone.
            PlayerbotAI* memberAi = member->GetPlayerbotAI();
            if (stranded && memberAi && sGuildBotMgr.IsGuildBot(member) && member->IsAlive() && !member->IsBeingTeleported())
            {
                bool home = memberAi->DoSpecificAction("hearthstone", Event(), true);
                update.events.push_back({ "recovery", std::string(member->GetName()) + (home ? " hearthstone out of " : " hearthstone not ready in ") + dungeon.name });
            }
        }
        if (stranded)
            run.lastHearth = now;
    }
    else if (leader->GetMapId() == dungeon.mapId)
        inside = 1;

    if (inside)
    {
        // A player still inside takes the time they want: the leader waits outside, and the reset follows.
        if (!guildInside && !playerInside.empty())
        {
            update.reason = "waiting for " + playerInside + " to come out of " + dungeon.name + "; " + run.cleared;
            return;
        }
        if (now - run.leaveStarted > LEAVE_SECONDS)
        {
            update.status = "failed";
            update.reason = run.cleared + "; " + std::to_string(inside) + " still inside after " + std::to_string(LEAVE_SECONDS / MINUTE) + " minutes";
            return;
        }
        // The leader inside goes for the way out; any other travel target goes, as at a dead boss.
        if (leader->GetMapId() == dungeon.mapId)
            if (PlayerbotAI* ai = leader->GetPlayerbotAI())
            {
                TravelTarget* target = ai->GetAiObjectContext()->GetValue<TravelTarget*>("travel target")->Get();
                if (target && target->IsActive() && target->GetDestination() != dungeon.exit)
                    target->SetStatus(TravelStatus::TRAVEL_STATUS_EXPIRED);
            }
        update.reason = "leaving " + dungeon.name + ", " + std::to_string(inside) + " inside; " + run.cleared;
        return;
    }

    // Everyone is out: the leader resets the instance, as a player's client asks the server to.
    bool reset = true;
    if (InstanceGroupBind* bind = group ? group->GetBoundInstance(dungeon.mapId) : nullptr)
    {
        const uint32 instanceId = bind->state ? bind->state->GetInstanceId() : 0;
        WorldPacket packet(CMSG_RESET_INSTANCES);
        leader->GetSession()->HandleResetInstancesOpcode(packet);
        reset = !group->GetBoundInstance(dungeon.mapId);
        if (reset && instanceId)
            sGuildRuns.InstanceReset(instanceId);
    }
    update.status = "done";
    update.reason = run.cleared + (reset ? ", and reset" : ", but the reset failed");
    if (reset)
        update.events.push_back({ "dungeon_reset", dungeon.name });
}

bool GuildDungeon::LeaderAcrossEntrance(Player* bot, WorldLocation& leaderPlace)
{
    Group* group = bot->GetGroup();
    if (!group || !sGuildBotMgr.IsGuildBot(bot) || !bot->IsAlive())
        return false;

    Player* leader = sObjectMgr.GetPlayer(group->GetLeaderGuid());
    if (!leader || leader == bot || !leader->IsInWorld() || leader->IsBeingTeleported() || leader->GetMapId() == bot->GetMapId())
        return false;
    MapEntry const* leaderMap = sMapStore.LookupEntry(leader->GetMapId());
    MapEntry const* botMap = sMapStore.LookupEntry(bot->GetMapId());
    if (!(leaderMap && leaderMap->IsDungeon()) && !(botMap && botMap->IsDungeon()))
        return false;

    // At the stone of the leader's dungeon, it waits for its turn to go in by the stone instead of walking off to the entrance.
    WorldPosition stone;
    if (leaderMap && leaderMap->IsDungeon() && StonePlace(leader->GetMapId(), stone) && stone.getMapId() == bot->GetMapId()
        && stone.distance(WorldPosition(bot)) <= NEAR_STONE)
        return false;

    leader->GetPosition(leaderPlace);
    return true;
}

void GuildDungeon::Stop(uint32 leaderGuid)
{
    std::lock_guard<std::mutex> guard(lock);
    runs.erase(leaderGuid);
}

bool GuildDungeon::ShouldTravel(Player* bot) const
{
    std::lock_guard<std::mutex> guard(lock);
    auto it = runs.find(bot->GetGUIDLow());
    if (it == runs.end())
        return false;
    const Run& run = it->second;
    if (run.phase == Phase::Go)
        return bot->GetMapId() != run.dungeon.mapId;
    if (run.phase == Phase::Leave)
        return bot->GetMapId() == run.dungeon.mapId;
    return run.phase == Phase::Bosses && run.nextBoss && bot->GetMapId() == run.dungeon.mapId;
}

bool GuildDungeon::IsHolding(Player* bot) const
{
    std::lock_guard<std::mutex> guard(lock);
    auto it = runs.find(bot->GetGUIDLow());
    return it != runs.end() && it->second.phase == Phase::Hold;
}

int32 GuildDungeon::TravelEntry(Player* bot) const
{
    std::lock_guard<std::mutex> guard(lock);
    auto it = runs.find(bot->GetGUIDLow());
    if (it == runs.end() || it->second.dungeon.bosses.empty())
        return 0;
    if (it->second.phase == Phase::Bosses)
        return it->second.nextBoss;
    return it->second.dungeon.bosses.front();
}

TravelDestination* GuildDungeon::StoneDestination(Player* bot) const
{
    std::lock_guard<std::mutex> guard(lock);
    auto it = runs.find(bot->GetGUIDLow());
    if (it == runs.end() || it->second.phase != Phase::Go || bot->GetMapId() == it->second.dungeon.mapId)
        return nullptr;
    return it->second.dungeon.stone;
}

uint32 GuildDungeon::StoneMap(uint32 areaId)
{
    if (!areaId)
        return 0;
    for (uint32 i = 0; i < sMapStore.GetNumRows(); ++i)
        if (MapEntry const* map = sMapStore.LookupEntry(i))
            if (map->linked_zone == areaId && map->IsDungeon())
                return map->MapID;
    if (AreaTableEntry const* area = GetAreaEntryByAreaID(areaId))
        if (MapEntry const* map = sMapStore.LookupEntry(area->mapid))
            if (map->IsDungeon())
                return map->MapID;
    return 0;
}

bool GuildDungeon::StonePlace(uint32 mapId, WorldPosition& place)
{
    std::lock_guard<std::mutex> guard(exitLock);
    if (!stonesIndexed)
    {
        stonesIndexed = true;
        auto index = [&](auto const& pair)
        {
            GameObjectData const& data = pair.second;
            GameObjectInfo const* info = ObjectMgr::GetGameObjectInfo(data.id);
            if (!info || info->type != GAMEOBJECT_TYPE_MEETINGSTONE)
                return false;
            if (uint32 dungeon = StoneMap(info->meetingstone.areaID))
                stonePlaces[dungeon] = WorldPosition(data.mapid, data.posX, data.posY, data.posZ, data.orientation);
            return false;
        };
        sObjectMgr.DoGOData(index);
    }
    auto it = stonePlaces.find(mapId);
    if (it == stonePlaces.end())
        return false;
    place = it->second;
    return true;
}

void GuildDungeon::UseStone(Player* player, uint32 areaId)
{
    std::string why;
    const uint32 mapId = StoneMap(areaId);
    if (!mapId)
        why = "This stone leads to no dungeon.";
    else if (!EnterByStone(player, mapId, why) && why.empty())
        why = "Someone else is entering; try again in a moment.";
    if (!why.empty() && !player->GetPlayerbotAI())
        ChatHandler(player).SendSysMessage(why.c_str());
}

bool GuildDungeon::EnterByStone(Player* player, uint32 mapId, std::string& why, AreaTrigger const* entrance)
{
    why.clear();
    if (!player->IsAlive() || player->IsInCombat() || player->IsBeingTeleported() || player->GetMapId() == mapId)
        return false;
    AreaTrigger const* at = entrance ? entrance : sObjectMgr.GetMapEntranceTrigger(mapId);
    MapEntry const* map = sMapStore.LookupEntry(mapId);
    if (!at || !map)
    {
        why = "No entrance is known for this dungeon.";
        return false;
    }

    // The entrance's own checks, as walking in would make them.
    uint32 misc = 0;
    switch (player->GetAreaTriggerLockStatus(at, misc))
    {
    case AREA_LOCKSTATUS_OK:
        break;
    case AREA_LOCKSTATUS_TOO_LOW_LEVEL:
        why = "You must be level " + std::to_string(misc) + " to enter " + std::string(map->name[0]) + ".";
        return false;
    case AREA_LOCKSTATUS_MISSING_ITEM:
    {
        ItemPrototype const* item = sObjectMgr.GetItemPrototype(misc);
        why = "You need " + (item ? std::string(item->Name1) : "a key") + " to enter " + std::string(map->name[0]) + ".";
        return false;
    }
    case AREA_LOCKSTATUS_QUEST_NOT_COMPLETED:
        why = "You have not done the quest " + std::string(map->name[0]) + " needs.";
        return false;
    case AREA_LOCKSTATUS_TOO_MANY_INSTANCE:
        why = "You have entered too many instances recently.";
        return false;
    case AREA_LOCKSTATUS_ZONE_IN_COMBAT:
        why = std::string(map->name[0]) + " has a fight in progress.";
        return false;
    default:
        why = "You may not enter " + std::string(map->name[0]) + ".";
        return false;
    }

    // One at a time, and guild characters after their leader: two entering an unbound instance at once can crash the server.
    if (!MayEnterMap(player, mapId))
        return false;

    player->TeleportTo(at->target_mapId, at->target_X, at->target_Y, at->target_Z, at->target_Orientation);
    return true;
}

bool GuildDungeon::FollowByTravel(Player* bot, WorldLocation& place)
{
    Group* group = bot->GetGroup();
    if (!group || !sGuildBotMgr.IsGuildBot(bot) || !bot->IsAlive() || !bot->IsInWorld())
        return false;
    Player* leader = sObjectMgr.GetPlayer(group->GetLeaderGuid());
    if (!leader || leader == bot || !leader->IsInWorld() || leader->IsBeingTeleported())
        return false;

    if (leader->GetMapId() != bot->GetMapId())
    {
        // The leader inside a dungeon with a stone on this side: to the stone, which takes the follower in.
        WorldPosition stone;
        MapEntry const* map = sMapStore.LookupEntry(leader->GetMapId());
        if (map && map->IsNonRaidDungeon() && StonePlace(leader->GetMapId(), stone) && stone.getMapId() == bot->GetMapId())
        {
            if (stone.distance(WorldPosition(bot)) <= NEAR_STONE)
                return false;
            place = WorldLocation(stone.getMapId(), stone.getX(), stone.getY(), stone.getZ(), stone.getO());
            return true;
        }
        return LeaderAcrossEntrance(bot, place);
    }

    // The leader at an object that needs more than one user, such as an altar: to it, near enough to use it.
    if (sGuildDungeon.UseSpot(leader, bot, place))
        return true;

    // Upstream's follow gives up this far off; a follower that wandered or was taken away travels back.
    if (bot->GetDistance(leader) > FOLLOW_BY_TRAVEL)
    {
        leader->GetPosition(place);
        return true;
    }
    return false;
}

void GuildDungeon::FollowByStone(Player* member)
{
    Group* group = member->GetGroup();
    if (!group || !sGuildBotMgr.IsGuildBot(member) || !member->IsInWorld() || member->IsBeingTeleported() || !member->IsAlive()
        || member->IsInCombat() || group->IsLeader(member->GetObjectGuid()))
        return;
    Player* leader = sObjectMgr.GetPlayer(group->GetLeaderGuid());
    if (!leader || !leader->IsInWorld() || leader->IsBeingTeleported() || leader->GetMapId() == member->GetMapId())
        return;
    MapEntry const* map = sMapStore.LookupEntry(leader->GetMapId());
    if (!map || !map->IsNonRaidDungeon())
        return;
    WorldPosition stone;
    if (!StonePlace(leader->GetMapId(), stone) || stone.getMapId() != member->GetMapId() || stone.distance(WorldPosition(member)) > NEAR_STONE)
        return;
    // In by the entrance of the wing the leader is in.
    std::string why;
    EnterByStone(member, leader->GetMapId(), why,
        EntranceNear(leader->GetMapId(), leader->GetPositionX(), leader->GetPositionY(), leader->GetPositionZ()));
    std::lock_guard<std::mutex> guard(refusalLock);
    std::string& last = stoneRefusals[member->GetGUIDLow()];
    if (!why.empty() && why != last)
        sGuildBotMgr.RecordEvent(member->GetGUIDLow(), "refused", why);
    last = why;
}

TravelDestination* GuildDungeon::UseDestination(Player* bot) const
{
    std::lock_guard<std::mutex> guard(lock);
    auto it = runs.find(bot->GetGUIDLow());
    if (it == runs.end() || it->second.phase != Phase::Bosses || !it->second.useSpawn || bot->GetMapId() != it->second.dungeon.mapId)
        return nullptr;
    for (const Dungeon::Use& use : it->second.dungeon.uses)
        if (use.spawn == it->second.useSpawn)
            return use.place;
    return nullptr;
}

bool GuildDungeon::UseSpot(Player* leader, Player* member, WorldLocation& place) const
{
    std::lock_guard<std::mutex> guard(lock);
    auto it = runs.find(leader->GetGUIDLow());
    if (it == runs.end() || it->second.phase != Phase::Bosses || !it->second.useSpawn || member->GetMapId() != it->second.dungeon.mapId
        || leader->GetMapId() != member->GetMapId())
        return false;
    for (const Dungeon::Use& use : it->second.dungeon.uses)
    {
        if (use.spawn != it->second.useSpawn || use.users < 2)
            continue;
        // A member who has used it this time stays where it is.
        auto usedBy = it->second.usedBy.find(use.spawn);
        if (usedBy != it->second.usedBy.end() && usedBy->second.count(member->GetGUIDLow()))
            return false;
        GameObject* object = member->GetMap()->GetGameObject(use.spawn);
        if (!object || leader->GetDistance(object) > USE_DISTANCE || member->GetDistance(leader) > USE_JOIN_DISTANCE
            || member->GetDistance(object) <= USE_STAND)
            return false;
        float x, y, z;
        object->GetNearPoint(member, x, y, z, member->GetObjectBoundingRadius(), USE_STAND, object->GetAngle(member));
        place = WorldLocation(object->GetMapId(), x, y, z, 0.0f);
        return true;
    }
    return false;
}

TravelDestination* GuildDungeon::ExitDestination(Player* bot) const
{
    std::lock_guard<std::mutex> guard(lock);
    auto it = runs.find(bot->GetGUIDLow());
    if (it == runs.end() || it->second.phase != Phase::Leave || bot->GetMapId() != it->second.dungeon.mapId)
        return nullptr;
    return it->second.dungeon.exit;
}

Creature* GuildDungeon::NextBoss(Player* bot, float within) const
{
    std::lock_guard<std::mutex> guard(lock);
    auto it = runs.find(bot->GetGUIDLow());
    if (it == runs.end() || it->second.phase != Phase::Bosses || !it->second.nextSpawn || bot->GetMapId() != it->second.dungeon.mapId
        || !bot->IsInWorld())
        return nullptr;
    // An object to use first, such as the altar that wakes the boss: the leader stays at it.
    // A creature to speak to is no target either.
    if (it->second.useSpawn || it->second.dungeon.talks.count(it->second.nextBoss))
        return nullptr;
    Creature* boss = bot->GetMap()->GetCreature(it->second.nextSpawn);
    if (!boss || !boss->IsAlive() || !boss->IsInWorld() || bot->GetDistance(boss) > within)
        return nullptr;
    return boss;
}

Creature* GuildDungeon::KillTarget(Player* bot, float within) const
{
    std::lock_guard<std::mutex> guard(lock);
    auto it = runs.find(bot->GetGUIDLow());
    if (it == runs.end() || it->second.phase != Phase::Bosses || it->second.killGuid.IsEmpty() || bot->GetMapId() != it->second.dungeon.mapId
        || !bot->IsInWorld())
        return nullptr;
    Creature* creature = bot->GetMap()->GetCreature(it->second.killGuid);
    if (!creature || !creature->IsAlive() || !creature->IsInWorld() || bot->GetDistance(creature) > within)
        return nullptr;
    return creature;
}

bool GuildDungeon::MayEnter(Player* bot, uint32 triggerId)
{
    AreaTrigger const* at = sObjectMgr.GetAreaTrigger(triggerId);
    if (!at || bot->GetMapId() == at->target_mapId)
        return true;

    MapEntry const* map = sMapStore.LookupEntry(at->target_mapId);
    if (!map || !map->IsDungeon())
        return true;

    // Only guild characters, which go in with their leader; random bots keep upstream's behaviour.
    if (!bot->GetGroup() || !sGuildBotMgr.IsGuildBot(bot))
        return true;
    return MayEnterMap(bot, at->target_mapId);
}

bool GuildDungeon::MayEnterMap(Player* bot, uint32 mapId)
{
    Group* group = bot->GetGroup();
    if (!group)
        return true;

    if (sGuildBotMgr.IsGuildBot(bot) && !group->IsLeader(bot->GetObjectGuid()))
    {
        // The leader first, a player or a guild character: inside, in the world, and the group bound.
        Player* leader = sObjectMgr.GetPlayer(group->GetLeaderGuid());
        if (!leader || !leader->IsInWorld() || leader->IsBeingTeleportedFar() || leader->GetMapId() != mapId)
            return false;
        if (!group->GetBoundInstance(mapId))
            return false;
    }

    // Then one at a time: nobody else on the way in, and a gap since the last one went.
    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->getSource();
        if (member && member != bot && member->IsBeingTeleportedFar() && member->GetTeleportDest().mapid == mapId)
            return false;
    }

    std::lock_guard<std::mutex> guard(entryLock);
    const uint32 now = WorldTimer::getMSTime();
    auto it = lastEntry.find(group->GetId());
    if (it != lastEntry.end() && WorldTimer::getMSTimeDiff(it->second, now) < ENTRY_GAP_MS)
        return false;
    lastEntry[group->GetId()] = now;
    return true;
}
