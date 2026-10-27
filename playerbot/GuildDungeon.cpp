#include "playerbot/playerbot.h"
#include "playerbot/GuildDungeon.h"
#include "playerbot/GuildBotMgr.h"
#include "playerbot/GuildRuns.h"
#include "playerbot/TravelMgr.h"
#include "playerbot/ServerFacade.h"
#include "Pools/PoolManager.h"

#include <algorithm>
#include <cctype>

using namespace ai;

std::mutex GuildDungeon::entryLock;
std::map<uint32, uint32> GuildDungeon::lastEntry;
std::mutex GuildDungeon::exitLock;
std::map<uint32, std::unique_ptr<PlaceTravelDestination>> GuildDungeon::exits;
std::map<uint32, std::unique_ptr<PlaceTravelDestination>> GuildDungeon::stones;
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
    constexpr float AT_STONE = 8.0f;
    constexpr float NEAR_STONE = 40.0f;

    // The last boss, where the boss destinations alone would not say which it is.
    const std::map<uint32, int32> LAST_BOSS = {
        { 36, 639 },   // The Deadmines: Edwin VanCleef
    };

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

uint32 GuildDungeon::FindMap(const std::string& name)
{
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
    dungeon.name = sMapStore.LookupEntry(dungeon.mapId)->name[0];
    dungeon.entrance = sObjectMgr.GetMapEntranceTrigger(dungeon.mapId);

    // The bosses are the boss travel destinations on the dungeon's map: its elites with one spawn.
    // Nearest the arrival point first; the boss step will order them by route.
    const AreaTrigger* at = dungeon.entrance;
    WorldPosition arrival(dungeon.mapId, at->target_X, at->target_Y, at->target_Z, at->target_Orientation);
    std::vector<std::pair<float, int32>> bosses;
    for (TravelDestination* dest : sTravelMgr.GetDestinations(PlayerTravelInfo(leader), (uint32)TravelDestinationPurpose::Boss, {}, false, 0))
    {
        if (dest->GetEntry() <= 0)
            continue;
        // The destination's point nearest the arrival: on the dungeon's map if it has one there.
        std::list<uint8> nearest = { 0, 0, 0 };
        WorldPosition* point = dest->GetNextPoint(arrival, nearest);
        if (!point || point->getMapId() != dungeon.mapId)
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

        WorldPosition stonePlace;
        if (StonePlace(dungeon.mapId, stonePlace))
        {
            auto& stone = stones[dungeon.mapId];
            if (!stone)
                stone = std::make_unique<PlaceTravelDestination>(stonePlace, "the meeting stone of " + dungeon.name);
            dungeon.stone = stone.get();
        }
    }

    // The party must be near the last boss's level: the one the table names, or else the highest.
    auto last = LAST_BOSS.find(dungeon.mapId);
    uint32 highest = 0;
    for (int32 entry : dungeon.bosses)
    {
        CreatureInfo const* info = ObjectMgr::GetCreatureTemplate(entry);
        if (!info)
            continue;
        highest = std::max(highest, info->MaxLevel);
        if (last != LAST_BOSS.end() && entry == last->second)
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

        // The core's own check at the entrance: level, key, quest, and the five new instances an hour.
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
            return who + "'s account entered five instances in the last hour";
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

    if (run.phase == Phase::Check && why.empty())
        run.phase = Phase::Go;

    // With a meeting stone, the leader goes to it and in by it, once the party is with it.
    if (run.phase == Phase::Go && dungeon.stone && why.empty() && dungeon.stone->DistanceTo(WorldPosition(leader)) <= AT_STONE)
    {
        std::string refused;
        if (!EnterByStone(leader, dungeon.mapId, refused) && !refused.empty())
            why = refused;
    }

    // Once on the way, a member falling behind does not stop the leader: it waits at the entrance
    // until the party is near, as upstream's travel does for dungeon destinations.
    if (run.phase == Phase::Go)
        update.reason = "travelling to " + dungeon.name + (why.empty() ? "" : "; " + why);
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
    for (size_t i = 0; i < dungeon.bosses.size(); ++i)
    {
        int32 entry = dungeon.bosses[i];
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

    // Any travel target but the next boss goes, at a boss that died or one that replaced it, such as a grind spot:
    // upstream asks for no travel target while one is active, so the dungeon's request could not take it back.
    run.nextBoss = next;
    if (PlayerbotAI* ai = leader->GetPlayerbotAI())
    {
        TravelTarget* target = ai->GetAiObjectContext()->GetValue<TravelTarget*>("travel target")->Get();
        if (target && target->IsActive() && target->GetEntry() != next)
            target->SetStatus(TravelStatus::TRAVEL_STATUS_EXPIRED);
    }
    update.reason = "on the way to " + BossName(next) + " in " + dungeon.name + ", " + count;
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

bool GuildDungeon::EnterByStone(Player* player, uint32 mapId, std::string& why)
{
    why.clear();
    if (!player->IsAlive() || player->IsInCombat() || player->IsBeingTeleported() || player->GetMapId() == mapId)
        return false;
    AreaTrigger const* at = sObjectMgr.GetMapEntranceTrigger(mapId);
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

void GuildDungeon::FollowByStone(Player* member)
{
    Group* group = member->GetGroup();
    if (!group || !sGuildBotMgr.IsGuildBot(member) || !member->IsAlive() || member->IsInCombat() || group->IsLeader(member->GetObjectGuid()))
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
    std::string why;
    EnterByStone(member, leader->GetMapId(), why);
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
    if (it == runs.end() || it->second.phase != Phase::Bosses || !it->second.nextSpawn || bot->GetMapId() != it->second.dungeon.mapId)
        return nullptr;
    Creature* boss = bot->GetMap()->GetCreature(it->second.nextSpawn);
    if (!boss || !boss->IsAlive() || !boss->IsInWorld() || bot->GetDistance(boss) > within)
        return nullptr;
    return boss;
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
