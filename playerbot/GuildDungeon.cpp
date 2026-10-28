#include "playerbot/playerbot.h"
#include "playerbot/GuildDungeon.h"
#include "playerbot/GuildBotMgr.h"
#include "playerbot/GuildRuns.h"
#include "playerbot/TravelMgr.h"
#include "playerbot/TravelNode.h"
#include "playerbot/ServerFacade.h"
#include "Pools/PoolManager.h"
#include "Maps/InstanceData.h"
#include "AI/ScriptDevAI/ScriptDevAIMgr.h"
#include "AI/ScriptDevAI/include/sc_gossip.h"
#include "Grids/GridNotifiers.h"
#include "Grids/GridNotifiersImpl.h"
#include "Grids/CellImpl.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>

using namespace ai;

std::mutex GuildDungeon::entryLock;
std::map<uint32, uint32> GuildDungeon::lastEntry;
std::mutex GuildDungeon::exitLock;
std::map<uint32, std::unique_ptr<PlaceTravelDestination>> GuildDungeon::exits;
std::map<uint32, std::unique_ptr<PlaceTravelDestination>> GuildDungeon::insides;
std::map<uint32, std::unique_ptr<PlaceTravelDestination>> GuildDungeon::stones;
std::map<uint32, std::unique_ptr<PlaceTravelDestination>> GuildDungeon::usePlaces;
std::map<uint32, std::unique_ptr<PlaceTravelDestination>> GuildDungeon::bossPlaces;
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
    // A member inside that has stood still this long since the leader came out is stranded, somewhere no route leads
    // out of, and goes home by hearthstone, as a player would; tried again this often while it is on cooldown.
    constexpr time_t STRANDED_SECONDS = 3 * MINUTE;
    // A member within this of where it stood that long is standing still; one that moves farther is on its way.
    constexpr float STRANDED_STILL = 10.0f;
    constexpr time_t HEARTH_RETRY_SECONDS = MINUTE;
    // At the stone: the leader uses it within this, and a member this near it follows the leader in by it.
    // The member's reach is wide: some stones stand on a ledge above the road, out of reach of the paths
    // below, where a member that fought its way off the ledge cannot climb back.
    constexpr float AT_STONE = 8.0f;
    constexpr float NEAR_STONE = 120.0f;
    // A follower farther than this from its leader on the same map travels to it, by the travel map's routes.
    constexpr float FOLLOW_BY_TRAVEL = 150.0f;
    // Or this far above or below it, outside: follow walks under the leader but takes no lift up to it.
    constexpr float FOLLOW_BY_TRAVEL_HEIGHT = 20.0f;
    // Leaving, a leader farther than this from where the entrance put the party goes there first, then out.
    constexpr float EXIT_NEAR = 30.0f;
    // A boss the travel map has no route to sorts after every boss it has a route to.
    constexpr float NO_ROUTE = 1000000.0f;
    // A boss the leader comes no nearer to by this much in this long, alive and out of combat, is out of reach
    // on this map: the party passes it over, or fails the run at the last boss.
    constexpr float HEADWAY = 10.0f;
    constexpr time_t NO_HEADWAY_SECONDS = 5 * MINUTE;
    // A boss the leader has given up on this many times, untouched at full health, is out of reach: passed over.
    constexpr uint32 GIVE_UPS_TO_PASS = 2;
    // Before each boss the leader waits this long for a member farther off than NEAR_LEADER, then goes on without it.
    constexpr time_t PARTY_WAIT_SECONDS = 3 * MINUTE;

    // Why the stone last turned each member away, so the event log has each new reason once.
    std::mutex refusalLock;
    std::map<uint32, std::string> stoneRefusals;
    // A member by the stone that has not gone in by it after this walks in by the entrance instead.
    constexpr time_t STONE_WAIT_SECONDS = MINUTE;
    std::map<uint32, time_t> stoneSince;           // by member guid: since when it has been by the stone, outside

    // A member dead in a dungeon waits this long to be resurrected by a party member this near, then releases.
    constexpr time_t RESURRECT_WAIT_SECONDS = 2 * MINUTE;
    constexpr float RESURRECT_NEAR = 60.0f;
    std::mutex deathLock;
    std::map<uint32, time_t> deadSince;            // by guid: when it was first seen dead, until it lives again
    // A ghost not back in its dungeon this long after dying takes the graveyard's way instead.
    constexpr time_t CORPSE_RUN_SECONDS = 15 * MINUTE;

    // The last boss, where the boss destinations alone would not say which it is.
    const std::map<uint32, int32> LAST_BOSS = {
        { 389, 11519 },  // Ragefire Chasm: Bazzalan
        { 36, 639 },     // The Deadmines: Edwin VanCleef
        { 43, 5775 },    // Wailing Caverns: Verdan the Everliving; the Naralex escort, and Mutanus with it, come before the run ends
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

    // Elites placed more than once that are bosses all the same, which the boss travel destinations leave out,
    // since those take an elite with one spawn. The run kills every one of them the instance has.
    const std::map<uint32, std::vector<int32>> MORE_BOSSES = {
        { 34, { 1696, 1663 } },  // The Stockade: Targorr the Dread, four of him; Dextren Ward, three
    };

    // A dungeon with wings, each its own entrance on one map, is run a wing at a time, by the wing's name.
    // The table names the wing's entrance and last boss. Its bosses are those the row lists, or with no list,
    // those nearest the wing's arrival.
    struct Wing { const char* name; const char* title; uint32 trigger; int32 lastBoss; std::vector<int32> bosses; };
    const Wing WINGS[] = {
        { "scarlet_monastery_graveyard", "Scarlet Monastery Graveyard", 45, 4543, {} },   // Bloodmage Thalnos
        { "scarlet_monastery_library", "Scarlet Monastery Library", 614, 6487, {} },      // Arcanist Doan
        { "scarlet_monastery_armory", "Scarlet Monastery Armory", 612, 3975, {} },        // Herod
        { "scarlet_monastery_cathedral", "Scarlet Monastery Cathedral", 610, 3977, {} },  // High Inquisitor Whitemane
        // Maraudon: Vyletongue and Celebras stand nearer the orange arrival than the purple one, so these rows list their bosses.
        { "maraudon_orange", "Maraudon Orange", 3133, 12258, { 13282, 12258 } },          // Noxxion, Razorlash
        { "maraudon_purple", "Maraudon Purple", 3134, 12225, { 12236, 12237, 12225 } },   // Lord Vyletongue, Meshlok the Harvester, Celebras the Cursed
    };

    // Friendly creatures a party has to speak to on its way, as a player would, with the gossip action to pick,
    // and the door the creature then opens, or 0. Each is a step among the bosses, in route order;
    // the party takes the one friendly to it.
    // An escort walks off after speaking, and the party stays with it, fighting what attacks it, until it is gone;
    // the bosses named after it must be dead before it is spoken to; the wait is how long the party gives it.
    struct Talk { uint32 map; int32 entry; uint32 action; uint32 door; bool escort = false; time_t wait = 0; std::array<int32, 4> after = {}; };
    const Talk TALKS[] = {
        // Shadowfang Keep: the prisoner, freed, walks to the Courtyard Door and unlocks it. Horde and Alliance have one each.
        { 33, 3849, GOSSIP_ACTION_INFO_DEF + 1, 18895 },   // Deathstalker Adamant
        { 33, 3850, GOSSIP_ACTION_INFO_DEF + 1, 18895 },   // Sorcerer Ashcrombe
        // Wailing Caverns: with the four Fanglords dead, the Disciple of Naralex walks to Naralex, stopping for waves,
        // and wakes him; Mutanus the Devourer comes at the end. His gossip starts it at any time, so the Fanglords come first.
        { 43, 3678, GOSSIP_ACTION_INFO_DEF + 1, 0, true, 20 * MINUTE, { 3671, 3669, 3670, 3673 } },
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
    // An object used several times is used each time it can be, out of combat, and the step is done
    // when the creature its last use brings is dead. One with no boss is due when the leader comes near it.
    // A row can name an encounter of the instance script whose state the `used` event reports, before and after the use.
    // The creatures each use but the last brings, which must all die before it can be used again, can be named too:
    // the party hunts them between uses, since some never come to the object.
    struct UseObject { uint32 map; uint32 entry; uint32 users; int32 before; bool wakes; uint32 times = 1; uint32 brings = 0; uint32 report = 0;
                       std::array<uint32, 2> waves = {}; };
    const UseObject USES[] = {
        // Blackfathom Deeps: the four Fires of Aku'mai, lit once Kelris is dead, open the way to Aku'mai.
        { 48, 21118, 1, 4829, false },
        { 48, 21119, 1, 4829, false },
        { 48, 21120, 1, 4829, false },
        { 48, 21121, 1, 4829, false },
        // Uldaman: the Altar of Archaedas is a ritual for three. Its spell wakes him, and he attacks the first user.
        // The event reports TYPE_ARCHAEDAS (2): 0 not started, 1 in progress, 2 failed, 3 done, 4 waking.
        { 70, 133234, 3, 2748, true, 1, 0, 2 },
        // Razorfen Downs: the Gong brings a wave of Tomb Fiends, then one of Tomb Reavers, then Tuten'kash.
        // It can be rung again once the wave is dead. It comes before Mordresh Fire Eye, since the route between the bosses
        // does not come within 60 yards of it.
        // The waves are Tomb Fiends and Tomb Reavers, from two corridors 46 and 82 yards off.
        { 129, 148917, 1, 7357, false, 3, 7355, 0, { 7349, 7351 } },
    };
    // The leader uses an object this near it, and members this near the leader come to it to join.
    constexpr float USE_DISTANCE = 5.0f;
    constexpr float USE_JOIN_DISTANCE = 20.0f;
    // A member joining stands this near the object, inside the distance the core takes a use from.
    constexpr float USE_STAND = 2.0f;
    // An object that has not woken its boss yet is used again after this.
    constexpr time_t USE_RETRY_SECONDS = 15;
    // An object with no boss of its own is due when the leader comes this near it.
    constexpr float USE_NEAR = 60.0f;
    // The party looks this far from an object for the wave its last use brought.
    constexpr float WAVE_RANGE = 120.0f;

    // Creatures with no spawn of their own, brought by an event, that a party kills before a boss, as players would:
    // the creature, the boss, and the door their deaths open. The step is done when none is alive near the leader,
    // or when the door is open.
    struct KillFirst { uint32 map; uint32 entry; int32 before; uint32 door; };
    const KillFirst KILLS[] = {
        // Shadowfang Keep: Fenrus's death brings Arugal's four voidwalkers, and the Sorcerer's Gate opens when all four die.
        { 33, 4627, 3927, 18972 },
        // Zul'Farrak: Witch Doctor Zum'rah turns hostile when a player comes near and raises Zul'Farrak Zombies from the
        // forty Shallow Graves around him, one every 20 seconds, until he dies. He spawns friendly, so he is no boss of
        // the run; with no boss of its own the row has him killed whenever he is near and hostile.
        { 209, 7271, 0, 0 },
        // Zul'Farrak: Nekrum Gutchewer and Shadowpriest Sezz'ziz come down the pyramid last; their deaths open the End Door.
        { 209, 7796, 7267, 146084 },
        { 209, 7275, 7267, 146084 },
    };
    // The leader looks this far for them.
    constexpr float KILL_RANGE = 60.0f;

    // Items a party uses on an object before a boss, as players would: the item, the object, the boss, the door
    // the event it starts opens, the creature the party stays by while the event runs, and how long it waits for the door.
    // The member who carries the item comes to the object and uses it; the step is done when the door is open.
    struct ItemOnObject { uint32 map; uint32 item; uint32 object; int32 before; uint32 door; uint32 guard; time_t wait; };
    const ItemOnObject ITEM_USES[] = {
        // Zul'Farrak: the Executioner's Key, from the Sandfury Executioner by the cages, opens Sergeant Bly's cage.
        // Trolls then climb the pyramid at Bly's crew, Nekrum and Sezz'ziz last, and their deaths open the End Door to Ukorz.
        { 209, 8444, 141074, 7267, 146084, 7604, 15 * MINUTE },
    };
    // An item use that did not start is tried again after this.
    constexpr time_t ITEM_RETRY_SECONDS = 15;

    // The leader of an event stays this near the creature it is by, and moves back within it when it is farther.
    constexpr float HOLD_NEAR = 12.0f;
    constexpr float HOLD_STAND = 5.0f;
    // A step's object or creature this near, the leader walks to by itself. Its travel can stop short, 42 yards from the
    // Disciple of Naralex, and once arrived the travel target goes on counting as working there while fights take the
    // leader off, 200 yards from the gong in Razorfen Downs.
    constexpr float APPROACH_RANGE = 60.0f;
    // A boss that fell asleep as the next boss, such as Mograine lying as dead until Whitemane raises him, keeps the leader
    // by it this long, fighting what comes, before the next boss after it comes first.
    constexpr time_t SLEEPER_WAIT_SECONDS = 3 * MINUTE;

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

    // The walk from one place to another over the travel map's links, between the nodes nearest each,
    // or a negative number when the travel map has no route between them.
    float RouteLength(const WorldPosition& from, const WorldPosition& to)
    {
        std::shared_lock<std::shared_timed_mutex> guard(sTravelNodeMap.m_nMapMtx);
        std::vector<TravelNode*> starts = sTravelNodeMap.getNodes(from, 50.0f);
        std::vector<TravelNode*> goals = sTravelNodeMap.getNodes(to, 50.0f);
        if (starts.empty() || goals.empty())
            return -1.0f;
        TravelNode* start = starts.front();
        TravelNode* goal = goals.front();
        float length = from.distance(*start->getPosition()) + goal->getPosition()->distance(to);
        if (start == goal)
            return length;
        // With no unit, the search adds no hearthstone or teleport spell to the route.
        TravelNodeRoute route = sTravelNodeMap.getRoute(start, goal, nullptr);
        std::vector<TravelNode*>& nodes = route.getNodes();
        if (nodes.size() < 2)
            return -1.0f;
        for (size_t i = 0; i + 1 < nodes.size(); ++i)
        {
            if (!nodes[i]->hasLinkTo(nodes[i + 1]))
                return -1.0f;
            length += nodes[i]->linkDistanceTo(nodes[i + 1]);
        }
        return length;
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
    // A dungeon the wing table lists is run a wing at a time. Another with a second entrance, such as Gnomeregan's
    // train depot, is one dungeon with a back door, and all its bosses belong to the run.
    bool wings = false;
    for (const Wing& w : WINGS)
        if (AreaTrigger const* wingEntrance = sObjectMgr.GetAreaTrigger(w.trigger))
            if (wingEntrance->target_mapId == dungeon.mapId)
                wings = true;

    // The bosses are the boss travel destinations on the dungeon's map: its elites with one spawn,
    // in route order from the arrival over the travel map. A boss the travel map has no route to comes after
    // those it has, nearest first, and the last boss goes last below.
    const AreaTrigger* at = dungeon.entrance;
    WorldPosition arrival(dungeon.mapId, at->target_X, at->target_Y, at->target_Z, at->target_Orientation);
    std::vector<std::pair<float, int32>> bosses;
    for (TravelDestination* dest : sTravelMgr.GetDestinations(PlayerTravelInfo(leader), (uint32)TravelDestinationPurpose::Boss, {}, false, 0))
    {
        if (dest->GetEntry() <= 0)
            continue;
        // Not friends: an escort or a prisoner, such as the Disciple of Naralex or Deathstalker Adamant, is an elite
        // with one spawn too, but friendly, and the party would stand by it waiting for it to die. A neutral boss,
        // such as Kresh, attacks nobody but is a boss all the same.
        CreatureInfo const* bossInfo = ObjectMgr::GetCreatureTemplate(dest->GetEntry());
        FactionTemplateEntry const* bossFaction = bossInfo ? sFactionTemplateStore.LookupEntry(bossInfo->Faction) : nullptr;
        FactionTemplateEntry const* ownFaction = leader->GetFactionTemplateEntry();
        const bool friendly = bossFaction && ownFaction && bossFaction->IsFriendlyTo(*ownFaction) && !bossFaction->IsHostileToPlayers();
        const Talk* talk = nullptr;
        for (const Talk& t : TALKS)
            if (t.map == dungeon.mapId && t.entry == dest->GetEntry())
                talk = &t;
        // A talk step only for the party it is friendly to; any other friendly creature is no step at all.
        if (talk ? !friendly : friendly)
            continue;
        // Nor is a neutral creature that offers quests or gossip a boss, such as Willix the Importer, whom a party
        // escorts and cannot fight: the leader would stand by him until the headway rule passed him over.
        const bool hostile = bossFaction && ownFaction && (bossFaction->IsHostileTo(*ownFaction) || bossFaction->IsHostileToPlayers());
        if (!talk && !hostile && bossInfo && (bossInfo->NpcFlags & (UNIT_NPC_FLAG_GOSSIP | UNIT_NPC_FLAG_QUESTGIVER)))
            continue;
        if (talk)
        {
            dungeon.talks[talk->entry] = talk->action;
            if (talk->escort)
                dungeon.talkEscorts.insert(talk->entry);
            if (talk->wait)
                dungeon.talkWaits[talk->entry] = talk->wait;
            for (int32 boss : talk->after)
                if (boss)
                    dungeon.talkAfter[talk->entry].push_back(boss);
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
        // In a dungeon with wings, only the bosses of this run's wing: those its row lists,
        // or else those nearest its arrival of all the entrances'.
        if (wing && !wing->bosses.empty())
        {
            if (std::find(wing->bosses.begin(), wing->bosses.end(), dest->GetEntry()) == wing->bosses.end())
                continue;
        }
        else if (wings && EntranceNear(dungeon.mapId, point->getX(), point->getY(), point->getZ()) != dungeon.entrance)
            continue;
        const float route = RouteLength(arrival, *point);
        bosses.push_back({ route >= 0.0f ? route : NO_ROUTE + point->distance(arrival), dest->GetEntry() });
    }
    // The bosses the table adds, ordered from the spawn of theirs nearest the arrival.
    if (auto more = MORE_BOSSES.find(dungeon.mapId); more != MORE_BOSSES.end())
        for (int32 entry : more->second)
        {
            if (std::any_of(bosses.begin(), bosses.end(), [&](auto const& boss) { return boss.second == entry; }))
                continue;
            if (wing && !wing->bosses.empty() && std::find(wing->bosses.begin(), wing->bosses.end(), entry) == wing->bosses.end())
                continue;
            WorldPosition nearest;
            float best = -1.0f;
            auto findNearest = [&](auto const& pair)
            {
                if (pair.second.id != uint32(entry) || pair.second.mapid != dungeon.mapId)
                    return false;
                WorldPosition place(pair.second.mapid, pair.second.posX, pair.second.posY, pair.second.posZ);
                if (best < 0.0f || place.distance(arrival) < best)
                {
                    best = place.distance(arrival);
                    nearest = place;
                }
                return false;
            };
            sObjectMgr.DoCreatureData(findNearest);
            if (best < 0.0f)
                continue;
            if (wings && !(wing && !wing->bosses.empty())
                && EntranceNear(dungeon.mapId, nearest.getX(), nearest.getY(), nearest.getZ()) != dungeon.entrance)
                continue;
            const float route = RouteLength(arrival, nearest);
            bosses.push_back({ route >= 0.0f ? route : NO_ROUTE + best, entry });
        }

    std::sort(bosses.begin(), bosses.end());
    std::set<int32> routed;  // the bosses the travel map has a route to
    for (auto& [distance, entry] : bosses)
    {
        // Its spawns on the map tell whether it is dead in an instance: all of them, since a pooled rare such as
        // Bruegal Ironknuckle stands at whichever the pool chose. A boss with no spawn of its own, one a script
        // summons, cannot be told apart and is left out. So is a spawn that picks its creature from several at each
        // start (creature_spawn_entry): a creature it picked for only one spawn looks like an elite with one spawn,
        // such as a Defias Convict in the Stockade, and it is trash.
        std::vector<uint32> spawns;
        auto findSpawns = [&](auto const& pair)
        {
            if (pair.second.id == uint32(entry) && pair.second.mapid == dungeon.mapId
                && !sObjectMgr.GetAllRandomCreatureEntries(pair.first))
                spawns.push_back(pair.first);
            return false;
        };
        sObjectMgr.DoCreatureData(findSpawns);
        if (spawns.empty())
            continue;
        dungeon.bosses.push_back(entry);
        dungeon.spawns.push_back(spawns);
        if (distance < NO_ROUTE)
            routed.insert(entry);
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
        // Inside, where the entrance puts a party, beside the way out.
        auto& in = insides[dungeon.entrance->entry];
        if (!in)
            in = std::make_unique<PlaceTravelDestination>(arrival, "the way out of " + dungeon.name);
        dungeon.inside = in.get();
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
        step.times = std::max(use.times, 1u);
        step.brings = use.brings;
        step.report = use.report;
        step.waves = use.waves;
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

    // Items to use on objects, where the map has the object's spawn, with the door it opens.
    for (const ItemOnObject& use : ITEM_USES)
    {
        if (use.map != dungeon.mapId)
            continue;
        Dungeon::ItemUse step;
        step.item = use.item;
        step.before = use.before;
        step.guard = use.guard;
        step.wait = use.wait;
        WorldPosition place;
        auto findSpawn = [&](auto const& pair)
        {
            if (pair.second.mapid != dungeon.mapId)
                return false;
            if (pair.second.id == use.door)
                step.doorSpawn = pair.first;
            if (pair.second.id == use.object && !step.spawn)
            {
                step.spawn = pair.first;
                place = WorldPosition(pair.second.mapid, pair.second.posX, pair.second.posY, pair.second.posZ, pair.second.orientation);
            }
            return false;
        };
        sObjectMgr.DoGOData(findSpawn);
        if (!step.spawn)
            continue;
        std::lock_guard<std::mutex> guard(exitLock);
        auto& destination = usePlaces[step.spawn];
        if (!destination)
        {
            GameObjectInfo const* info = ObjectMgr::GetGameObjectInfo(use.object);
            destination = std::make_unique<PlaceTravelDestination>(place, std::string("the ") + (info ? info->name : "object") + " in " + dungeon.name);
        }
        step.place = destination.get();
        dungeon.itemUses.push_back(step);
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
    // Without a table entry, the boss with the longest route from the entrance ends the run,
    // and a boss the travel map has no route to only when it has a route to none.
    if (!dungeon.lastBoss)
    {
        dungeon.lastBoss = dungeon.bosses.back();
        for (auto it = dungeon.bosses.rbegin(); it != dungeon.bosses.rend(); ++it)
            if (routed.count(*it))
            {
                dungeon.lastBoss = *it;
                break;
            }
    }

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
                const std::vector<uint32>& spawns = run.dungeon.spawns[i];
                if (run.dungeon.bosses[i] != run.dungeon.lastBoss || std::none_of(spawns.begin(), spawns.end(),
                    [&](uint32 spawn) { return bind->state->GetCreatureRespawnTime(spawn) > time(nullptr); }))
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
                // The clocks start afresh, as after a wipe: time outside is not time without headway.
                run.headwayBoss = 0;
                run.waitBoss = 0;
                run.waiting = false;
                update.strategiesChanged = true;
            }
            TickBosses(leader, run, update);
            if (update.status != "working")
                runs.erase(leader->GetGUIDLow());
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

bool GuildDungeon::BossSpawned(Map* map, const std::vector<uint32>& spawns)
{
    return std::any_of(spawns.begin(), spawns.end(), [&](uint32 spawn) { return BossSpawned(map, spawn); });
}

bool GuildDungeon::BossDead(Map* map, const std::vector<uint32>& spawns)
{
    bool any = false;
    for (uint32 spawn : spawns)
    {
        if (!BossSpawned(map, spawn))
            continue;
        if (!BossDead(map, spawn))
            return false;
        any = true;
    }
    return any;
}

uint32 GuildDungeon::LiveSpawn(Map* map, const std::vector<uint32>& spawns, const WorldPosition& near)
{
    uint32 live = 0;
    float best = 0.0f;
    for (uint32 spawn : spawns)
    {
        if (!BossSpawned(map, spawn) || BossDead(map, spawn))
            continue;
        WorldPosition place;
        if (Creature* creature = map->GetCreature(spawn))
            place = WorldPosition(creature);
        else if (CreatureData const* data = sObjectMgr.GetCreatureData(spawn))
            place = WorldPosition(data->mapid, data->posX, data->posY, data->posZ);
        else
            continue;
        const float distance = place.distance(near);
        if (!live || distance < best)
        {
            live = spawn;
            best = distance;
        }
    }
    return live;
}

void GuildDungeon::TickBosses(Player* leader, Run& run, Update& update)
{
    const Dungeon& dungeon = run.dungeon;
    Map* map = leader->GetMap();
    // The leader waits for a member far behind only on a tick that decides so below.
    const bool wasWaiting = run.waiting;
    run.waiting = false;

    int32 next = 0;
    uint32 present = 0;
    int32 asleep = 0;          // the first boss alive but unattackable, passed over for the next
    uint32 asleepSpawn = 0;
    run.holding = false;
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
            // Gone: dead, or despawned at the end of its walk. One not loaded, far off, is gone only if its spawn says so.
            Creature* creature = map->GetCreature(dungeon.spawns[i].front());
            if (creature ? !creature->IsAlive() : BossDead(map, dungeon.spawns[i].front()))
            {
                run.talked.insert(entry);
                continue;
            }
            if (next)
                continue;
            // Not yet: the bosses it comes after are not all done. One is done when it is dead, passed over as out of
            // reach, or not in this run at all, such as Lady Anacondra, whom this world has no spawn of.
            auto done = [&](int32 boss)
            {
                return run.killed.count(boss) || run.passed.count(boss)
                    || std::find(dungeon.bosses.begin(), dungeon.bosses.end(), boss) == dungeon.bosses.end();
            };
            if (auto after = dungeon.talkAfter.find(entry); after != dungeon.talkAfter.end()
                && !std::all_of(after->second.begin(), after->second.end(), done))
                continue;
            next = entry;
            run.nextSpawn = dungeon.spawns[i].front();
            // Not loaded: the leader travels to it, and its grid loads on the way.
            if (!creature)
                continue;
            const bool escort = dungeon.talkEscorts.count(entry);
            auto waitFor = dungeon.talkWaits.find(entry);
            const time_t wait = waitFor != dungeon.talkWaits.end() ? waitFor->second : TALK_WAIT_SECONDS;

            const time_t now = time(nullptr);
            auto spoken = run.talkedAt.find(entry);
            const bool offers = creature->HasFlag(UNIT_NPC_FLAGS, UNIT_NPC_FLAG_GOSSIP);
            if ((hasDoor || escort) && !offers)
            {
                // Its escort is under way, after the leader spoke or before a restart: the party waits here
                // while it walks to the door, or walks with it.
                if (spoken == run.talkedAt.end())
                    spoken = run.talkedAt.emplace(entry, now).first;
                if (now - spoken->second >= wait)
                {
                    run.talked.insert(entry);
                    update.events.push_back({ "gave_up", BossName(entry) + (hasDoor ? " did not open the " + doorName : " did not finish its walk") });
                    next = 0;
                    run.nextSpawn = 0;
                    continue;
                }
                if (escort)
                {
                    HoldNear(leader, creature);
                    run.holding = true;
                    talkWait = BossName(entry) + " to finish its walk";
                }
                else
                    talkWait = BossName(entry) + " to open the " + doorName;
                continue;
            }
            // Near but not near enough to speak: the leader walks the rest of the way itself.
            if (offers && leader->GetDistance(creature) > TALK_DISTANCE && leader->GetDistance(creature) <= APPROACH_RANGE)
            {
                HoldNear(leader, creature, TALK_DISTANCE - 1.0f, TALK_DISTANCE / 2.0f);
                run.holding = true;
            }
            if (leader->GetDistance(creature) <= TALK_DISTANCE && !leader->IsInCombat() && offers
                && (spoken == run.talkedAt.end() || now - spoken->second >= TALK_RETRY_SECONDS))
            {
                // As a player picks the option in its gossip: the script does the rest, such as an escort to a door.
                sScriptDevAIMgr.OnGossipSelect(leader, creature, GOSSIP_SENDER_MAIN, talk->second, nullptr);
                run.talkedAt[entry] = now;
                update.events.push_back({ "talked", BossName(entry) });
                if (!hasDoor && !escort)
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
        // A boss passed over as out of reach is alive, but not the next boss.
        if (next || run.passed.count(entry))
            continue;
        // Of a boss with several spawns, the one the leader is on its way to while it lives, else the living one nearest.
        // A copy the leader has given up on, out of its reach such as in a cell, is left while another copy lives;
        // with none left but those, the boss is taken as it is, and two give-ups pass it over.
        const std::vector<uint32>& allSpawns = dungeon.spawns[i];
        std::vector<uint32> reachable;
        if (allSpawns.size() > 1)
            for (uint32 copySpawn : allSpawns)
            {
                Creature* copy = map->GetCreature(copySpawn);
                if (!copy || !sGuildBotMgr.GiveUps(leader, copy->GetObjectGuid()))
                    reachable.push_back(copySpawn);
            }
        const std::vector<uint32>& spawns = !reachable.empty() && LiveSpawn(map, reachable, WorldPosition(leader)) ? reachable : allSpawns;
        const bool onWay = run.nextBoss == entry && std::count(spawns.begin(), spawns.end(), run.nextSpawn)
            && BossSpawned(map, run.nextSpawn) && !BossDead(map, run.nextSpawn);
        const uint32 spawn = onWay ? run.nextSpawn : LiveSpawn(map, spawns, WorldPosition(leader));
        if (!spawn)
            continue;
        // A boss alive but unattackable that no object of ours wakes, such as Mograine lying as dead until Whitemane
        // raises him: the next boss after it comes first, and it is the next boss again once it can be attacked.
        if (Creature* boss = map->GetCreature(spawn))
            if (boss->IsAlive() && boss->HasFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_UNINTERACTIBLE)
                && std::none_of(dungeon.uses.begin(), dungeon.uses.end(), [&](const Dungeon::Use& use) { return use.wakes && use.before == entry; }))
            {
                if (!asleep)
                {
                    asleep = entry;
                    asleepSpawn = spawn;
                }
                // It fell asleep as the next boss: an event wakes it, which comes to it.
                if (run.nextBoss == entry && run.sleeper != entry)
                {
                    run.sleeper = entry;
                    run.sleeperSpawn = spawn;
                    run.sleeperSince = time(nullptr);
                }
                continue;
            }
        next = entry;
        run.nextSpawn = spawn;
    }
    // Only sleeping bosses left: the first of them is still the next boss, and the run is not cleared.
    if (!next && asleep)
    {
        next = asleep;
        run.nextSpawn = asleepSpawn;
    }
    // A boss that fell asleep as the next boss: the leader stays by it for a while, fighting what comes, as the event
    // that wakes it comes to it. Whitemane runs to Mograine and raises him there.
    run.sleeperWaiting = false;
    if (run.sleeper)
    {
        Creature* sleeper = map->GetCreature(run.sleeperSpawn);
        if (!sleeper || !sleeper->IsAlive() || !sleeper->HasFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_UNINTERACTIBLE))
            run.sleeper = 0;
        else if (time(nullptr) - run.sleeperSince < SLEEPER_WAIT_SECONDS)
        {
            HoldNear(leader, sleeper);
            run.holding = true;
            run.sleeperWaiting = true;
            talkWait = BossName(run.sleeper) + " to wake";
        }
    }
    // A boss with several spawns is travelled to at the one chosen above: its boss destination, where it has one,
    // would pick any of them, such as a spot the pool left empty.
    run.nextPlace = nullptr;
    for (size_t i = 0; i < dungeon.bosses.size() && next && !dungeon.talks.count(next); ++i)
        if (dungeon.bosses[i] == next && dungeon.spawns[i].size() > 1 && run.nextSpawn)
            if (CreatureData const* data = sObjectMgr.GetCreatureData(run.nextSpawn))
            {
                std::lock_guard<std::mutex> guard(exitLock);
                auto& place = bossPlaces[run.nextSpawn];
                if (!place)
                    place = std::make_unique<PlaceTravelDestination>(WorldPosition(data->mapid, data->posX, data->posY, data->posZ),
                        BossName(next) + " in " + dungeon.name);
                run.nextPlace = place.get();
            }

    run.bossesSeen = true;
    std::string count = std::to_string(run.killed.size()) + " of " + std::to_string(present) + " bosses down";
    if (!run.passed.empty())
        count += ", " + std::to_string(run.passed.size()) + " passed over";
    // A creature to speak to still ahead, such as an escort, keeps the run going past the last boss.
    if ((run.killed.count(dungeon.lastBoss) && !dungeon.talks.count(next)) || !next)
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
        // A row with no boss of its own is due whenever its creature is near and hostile.
        if (kill.before && kill.before != next)
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
            if (unit->IsAlive() && unit->IsInWorld() && (kill.before || leader->CanAttack(unit))
                && (!nearest || leader->GetDistance(unit) < leader->GetDistance(nearest)))
                nearest = unit;
        if (!nearest)
            continue;
        run.killGuid = nearest->GetObjectGuid();
        killing = BossName(kill.entry);
        break;
    }

    // A member far behind, alive and on this map, such as one caught in a fight it cannot finish: before each boss
    // the leader waits for it where it stands for up to PARTY_WAIT_SECONDS, then goes on without it.
    // Creatures to kill first come before the wait.
    const time_t now = time(nullptr);
    if (run.waitBoss != next)
    {
        run.waitBoss = next;
        run.waitSince = 0;
        run.waitedOut = false;
    }
    std::string behind;
    if (Group* group = leader->GetGroup())
        for (auto& slot : group->GetMemberSlots())
            if (Player* member = sObjectMgr.GetPlayer(slot.guid))
                if (member != leader && member->IsInWorld() && member->IsAlive() && member->GetMapId() == leader->GetMapId()
                    && member->GetDistance(leader) > NEAR_LEADER)
                {
                    behind = slot.name;
                    break;
                }
    if (killing.empty() && !behind.empty() && !run.waitedOut)
    {
        if (!run.waitSince)
            run.waitSince = now;
        if (now - run.waitSince < PARTY_WAIT_SECONDS)
        {
            run.waiting = true;
            if (!wasWaiting)
            {
                update.strategiesChanged = true;
                if (PlayerbotAI* ai = leader->GetPlayerbotAI())
                    sTravelMgr.SetNullTravelTarget(ai->GetAiObjectContext()->GetValue<TravelTarget*>("travel target")->Get());
            }
            run.headwaySince = now;
            update.reason = "waiting for " + behind + " to catch up before " + BossName(next) + " in " + dungeon.name + ", " + count;
            return;
        }
        run.waitedOut = true;
        update.events.push_back({ "member_behind", behind + ", going on to " + BossName(next) + " without it" });
    }
    if (wasWaiting)
        update.strategiesChanged = true;

    // An object to use before the next boss, such as an altar that wakes it: the leader goes to it first,
    // and it and as many members as the object needs use it, as players would.
    // Time at an object is not time without headway to the boss, so the use step's returns restart that clock.
    run.useSpawn = 0;

    // An item to use on an object before the next boss, such as a key on a cage: the member who carries it comes
    // to the object and uses it, as a player would, and the party then stays by the event's creature until the door opens.
    for (const Dungeon::ItemUse& use : dungeon.itemUses)
    {
        if (use.before != next || run.used.count(use.spawn))
            continue;
        GameObject* door = use.doorSpawn ? map->GetGameObject(use.doorSpawn) : nullptr;
        if (door && door->GetGoState() != GO_STATE_READY)
        {
            run.used.insert(use.spawn);
            if (run.bossesSeen)
                update.events.push_back({ "door_open", std::string(door->GetGOInfo()->name) });
            continue;
        }
        GameObject* object = map->GetGameObject(use.spawn);
        ItemPrototype const* proto = sObjectMgr.GetItemPrototype(use.item);
        if (!object || !proto)
            continue;
        const std::string itemName = proto->Name1;
        const std::string objectName = object->GetGOInfo()->name;

        // Used: the party stays by the event's creature, or the object, while the event runs.
        if (auto usedAt = run.itemUsedAt.find(use.spawn); usedAt != run.itemUsedAt.end())
        {
            if (now - usedAt->second >= use.wait)
            {
                run.used.insert(use.spawn);
                update.events.push_back({ "gave_up", "the " + std::string(door ? door->GetGOInfo()->name : "door") + " did not open" });
                continue;
            }
            WorldObject* by = object;
            if (use.guard)
            {
                std::list<Unit*> found;
                MaNGOS::AllCreaturesOfEntryInRangeCheck check(object, use.guard, 2 * KILL_RANGE);
                MaNGOS::UnitListSearcher<MaNGOS::AllCreaturesOfEntryInRangeCheck> searcher(found, check);
                Cell::VisitAllObjects(object, searcher, 2 * KILL_RANGE);
                for (Unit* unit : found)
                    if (unit->IsAlive() && unit->IsInWorld())
                        by = unit;
            }
            HoldNear(leader, by);
            run.holding = true;
            run.headwaySince = now;
            update.reason = (killing.empty() ? "holding by " + std::string(by->GetName()) : "killing " + killing)
                + " while the " + objectName + "'s event runs in " + dungeon.name + ", " + count;
            return;
        }

        // Who carries it: nobody yet, and the step waits, say for the creature that drops it.
        Player* carrier = nullptr;
        if (Group* group = leader->GetGroup())
        {
            for (auto& slot : group->GetMemberSlots())
                if (Player* member = sObjectMgr.GetPlayer(slot.guid))
                    if (member->IsInWorld() && member->IsAlive() && member->GetMapId() == leader->GetMapId() && member->HasItemCount(use.item, 1))
                        carrier = member;
        }
        else if (leader->HasItemCount(use.item, 1))
            carrier = leader;
        if (!carrier)
            continue;

        run.useSpawn = use.spawn;
        if (leader->GetDistance(object) > USE_DISTANCE)
        {
            Approach(leader, object, use.place, USE_DISTANCE - 1.0f, USE_STAND, run);
            run.headwaySince = now;
            update.reason = "on the way to use the " + itemName + " on the " + objectName + " in " + dungeon.name + ", " + count;
            return;
        }
        // A player carries it: it is theirs to use.
        run.headwaySince = now;
        if (!sGuildBotMgr.IsGuildBot(carrier))
        {
            update.reason = "at the " + objectName + " in " + dungeon.name + ", waiting for " + carrier->GetName() + " to use the " + itemName;
            return;
        }
        // The carrier comes to it (UseSpot), and uses it out of combat, from within reach.
        if (!object->IsWithinDistInMap(carrier, object->GetInteractionDistance()) || carrier->IsInCombat() || carrier->IsNonMeleeSpellCasted(false)
            || now - run.itemTriedAt[use.spawn] < ITEM_RETRY_SECONDS)
        {
            update.reason = "at the " + objectName + " in " + dungeon.name + ", " + carrier->GetName() + " to use the " + itemName;
            return;
        }
        run.itemTriedAt[use.spawn] = now;
        Item* item = carrier->GetItemByEntry(use.item);
        if (!item || carrier->CanUseItem(item) != EQUIP_ERR_OK)
        {
            update.reason = "at the " + objectName + " in " + dungeon.name + ", " + carrier->GetName() + " cannot use the " + itemName;
            return;
        }
        // As the client's use of an item on an object does: its spell, cast at the object.
        SpellCastTargets targets;
        targets.setGOTarget(object);
        carrier->CastItemUseSpell(item, targets, 0);
        // Started when the carrier is casting it, or the object has opened already.
        if (carrier->IsNonMeleeSpellCasted(false) || object->GetGoState() != GO_STATE_READY)
        {
            run.itemUsedAt[use.spawn] = now;
            update.events.push_back({ "used", itemName + " on the " + objectName });
        }
        update.reason = "at the " + objectName + " in " + dungeon.name + ", " + carrier->GetName() + " uses the " + itemName;
        return;
    }

    for (const Dungeon::Use& use : dungeon.uses)
    {
        if (use.before && use.before != next)
            continue;
        if (use.times > 1 || use.brings)
        {
            if (run.used.count(use.spawn))
                continue;
            GameObject* object = map->GetGameObject(use.spawn);
            const uint32 times = run.useCount[use.spawn];
            // One with no boss of its own: due once the leader comes near it, and until it is done.
            if (!use.before && !times && (!object || leader->GetDistance(object) > USE_NEAR))
                continue;
            if (!object)
            {
                // Not loaded: nothing to do with it, unless it is under way, when the leader stays.
                if (!times)
                {
                    run.used.insert(use.spawn);
                    continue;
                }
                run.headwaySince = now;
                update.reason = "waiting at an object in " + dungeon.name + ", " + count;
                return;
            }
            const std::string name = object->GetGOInfo()->name;
            if (times >= use.times)
            {
                // Used up: done once what it brings is dead; until then the leader takes it as its target.
                Unit* brought = nullptr;
                if (use.brings)
                {
                    std::list<Unit*> found;
                    MaNGOS::AllCreaturesOfEntryInRangeCheck check(object, use.brings, KILL_RANGE);
                    MaNGOS::UnitListSearcher<MaNGOS::AllCreaturesOfEntryInRangeCheck> searcher(found, check);
                    Cell::VisitAllObjects(object, searcher, KILL_RANGE);
                    for (Unit* unit : found)
                        if (unit->IsAlive() && unit->IsInWorld())
                            brought = unit;
                }
                if (!brought)
                {
                    run.used.insert(use.spawn);
                    update.events.push_back({ "used", name + ", done" });
                    continue;
                }
                run.killGuid = brought->GetObjectGuid();
                run.headwaySince = now;
                update.reason = "killing " + BossName(use.brings) + " at the " + name + " in " + dungeon.name + ", " + count;
                return;
            }
            run.useSpawn = use.spawn;
            // Used, and shut until its wave is dead: the party hunts the wave, where it is.
            if (times && object->HasFlag(GAMEOBJECT_FLAGS, GO_FLAG_NO_INTERACT) && use.waves[0])
            {
                Unit* nearest = nullptr;
                for (uint32 wave : use.waves)
                {
                    if (!wave)
                        continue;
                    std::list<Unit*> found;
                    MaNGOS::AllCreaturesOfEntryInRangeCheck check(object, wave, WAVE_RANGE);
                    MaNGOS::UnitListSearcher<MaNGOS::AllCreaturesOfEntryInRangeCheck> searcher(found, check);
                    Cell::VisitAllObjects(object, searcher, WAVE_RANGE);
                    for (Unit* unit : found)
                        if (unit->IsAlive() && unit->IsInWorld() && (!nearest || leader->GetDistance(unit) < leader->GetDistance(nearest)))
                            nearest = unit;
                }
                if (nearest)
                {
                    run.killGuid = nearest->GetObjectGuid();
                    HoldNear(leader, nearest, KILL_RANGE / 2.0f, HOLD_STAND);
                    run.holding = true;
                    run.headwaySince = now;
                    update.reason = "killing the " + name + "'s wave, " + std::string(nearest->GetName()) + ", in " + dungeon.name + ", used "
                        + std::to_string(times) + " of " + std::to_string(use.times) + " times";
                    return;
                }
            }
            if (leader->GetDistance(object) <= USE_DISTANCE)
            {
                // At it, it stays by it between uses, out of a fight, instead of fighting its way off.
                HoldNear(leader, object, USE_DISTANCE - 1.0f, USE_STAND);
                run.holding = true;
                // Each time it can be used, out of combat: its last use brought a wave, and the wave is dead.
                const bool usable = object->IsSpawned() && !object->HasFlag(GAMEOBJECT_FLAGS, GO_FLAG_NO_INTERACT)
                    && !object->HasFlag(GAMEOBJECT_FLAGS, GO_FLAG_IN_USE);
                if (usable && !leader->IsInCombat() && object->CanUseNow(leader))
                {
                    WorldPacket packet(CMSG_GAMEOBJ_USE);
                    packet << object->GetObjectGuid();
                    leader->GetSession()->HandleGameObjectUseOpcode(packet);
                    // The script shuts it until the wave it brought is dead; that is a use.
                    if (object->HasFlag(GAMEOBJECT_FLAGS, GO_FLAG_NO_INTERACT) || !object->IsSpawned())
                    {
                        run.useCount[use.spawn] = times + 1;
                        update.events.push_back({ "used", name + ", " + std::to_string(times + 1) + " of " + std::to_string(use.times) });
                    }
                }
                run.headwaySince = now;
                update.reason = "at the " + name + " in " + dungeon.name + ", used " + std::to_string(run.useCount[use.spawn]) + " of "
                    + std::to_string(use.times) + " times";
                return;
            }
            Approach(leader, object, use.place, USE_DISTANCE - 1.0f, USE_STAND, run);
            run.headwaySince = now;
            update.reason = "on the way to use the " + name + " in " + dungeon.name + ", " + count;
            return;
        }
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
            // At it, it stays by it between uses, out of a fight, instead of fighting its way off.
            HoldNear(leader, object, USE_DISTANCE - 1.0f, USE_STAND);
            run.holding = true;
            // Everyone has used it and the boss still sleeps: again, after a while.
            std::set<uint32>& usedBy = run.usedBy[use.spawn];
            if (usedBy.size() >= use.users)
            {
                if (now - run.usedAt[use.spawn] < USE_RETRY_SECONDS)
                {
                    run.headwaySince = now;
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
            // The core's count of the object's users after each use, for the event: a use it took raises it,
            // and a ritual that went off clears it.
            std::string counts;
            InstanceData* script = map->GetInstanceData();
            const std::string stateBefore = script && use.report ? std::to_string(script->GetData(use.report)) : "";
            auto useBy = [&](Player* user)
            {
                WorldPacket packet(CMSG_GAMEOBJ_USE);
                packet << object->GetObjectGuid();
                user->GetSession()->HandleGameObjectUseOpcode(packet);
                usedBy.insert(user->GetGUIDLow());
                counts += (counts.empty() ? "" : " ") + std::to_string(object->GetUniqueUseCount());
            };
            // The leader first: a ritual's first user casts its spell, and a boss it wakes attacks that user.
            if (!usedBy.count(leader->GetGUIDLow()))
            {
                if (!canUse(leader))
                {
                    run.headwaySince = now;
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
                run.headwaySince = now;
                update.reason = "at the " + name + " in " + dungeon.name + ", " + std::to_string(usedBy.size()) + " of "
                    + std::to_string(use.users) + " have used it";
                return;
            }
            run.usedAt[use.spawn] = now;
            std::string detail = use.users > 1 && !counts.empty() ? name + ", users counted " + counts : name;
            if (script && use.report)
                detail += ", encounter " + std::to_string(use.report) + " state " + stateBefore + " to " + std::to_string(script->GetData(use.report));
            update.events.push_back({ "used", detail });
            if (!use.wakes)
            {
                run.used.insert(use.spawn);
                continue;
            }
            run.headwaySince = now;
            update.reason = "used the " + name + " in " + dungeon.name + ", waiting for " + BossName(next) + " to wake";
            return;
        }
        Approach(leader, object, use.place, USE_DISTANCE - 1.0f, USE_STAND, run);
        run.headwaySince = now;
        update.reason = "on the way to use the " + name + " in " + dungeon.name + ", " + count;
        return;
    }

    // No headway: the leader comes no nearer to the next boss by HEADWAY in NO_HEADWAY_SECONDS, alive and out of
    // combat, so it is out of reach on this map, as Gelihast was from the water below his ledge. The party passes it
    // over; the last boss is not passed over, and the run fails. A talk step, creatures to kill first, a prisoner
    // on its way to a door and a fight all restart the clock.
    WorldPosition bossPlace;
    if (Creature* boss = map->GetCreature(run.nextSpawn))
        bossPlace = WorldPosition(boss);
    else if (CreatureData const* data = sObjectMgr.GetCreatureData(run.nextSpawn))
        bossPlace = WorldPosition(dungeon.mapId, data->posX, data->posY, data->posZ);
    if (run.headwayBoss != next)
    {
        run.headwayBoss = next;
        run.headwayBest = -1.0f;
        run.headwaySince = now;
    }
    // The AI's combat state says the leader is fighting: the player's flag stays set while any creature has it in its
    // threat list, and the AI's state is never empty of something to attack for long (GuildBotMgr::CheckEmptyFight).
    PlayerbotAI* leaderAi = leader->GetPlayerbotAI();
    const bool fighting = leaderAi ? leaderAi->IsStateActive(BotState::BOT_STATE_COMBAT) : leader->IsInCombat();
    const bool idle = dungeon.talks.count(next) || !killing.empty() || !talkWait.empty() || fighting
        || !leader->IsAlive() || !bossPlace;
    const float bossDistance = bossPlace ? WorldPosition(leader).distance(bossPlace) : 0.0f;
    const bool nearer = bossPlace && (run.headwayBest < 0.0f || bossDistance < run.headwayBest - HEADWAY);
    if (nearer)
        run.headwayBest = bossDistance;
    // Given up on twice: the leader fought the boss and left it untouched at full health for 45 seconds two times,
    // out of reach of its blows and spells, such as Lady Sarevess in her pool. Those fights hold the clock above, and
    // the leader would take the boss again at once, so the party passes it over. Once could be a hiccup on the way in.
    Creature* bossCreature = map->GetCreature(run.nextSpawn);
    const bool givenUp = !dungeon.talks.count(next) && bossCreature && bossCreature->IsAlive()
        && sGuildBotMgr.GiveUps(leader, bossCreature->GetObjectGuid()) >= GIVE_UPS_TO_PASS;
    if ((idle || nearer) && !givenUp)
        run.headwaySince = now;
    else if (givenUp || now - run.headwaySince >= NO_HEADWAY_SECONDS)
    {
        const std::string why = givenUp ? "gave up on " + BossName(next) + " twice, untouched at full health"
            : "no nearer to " + BossName(next) + " than " + std::to_string(uint32(run.headwayBest)) + " yards in "
            + std::to_string(NO_HEADWAY_SECONDS / MINUTE) + " minutes";
        if (next == dungeon.lastBoss)
        {
            update.status = "failed";
            update.reason = why + " in " + dungeon.name + ", the last boss; " + count;
            return;
        }
        run.passed.insert(next);
        run.headwayBoss = 0;
        update.events.push_back({ "boss_passed", why });
        update.reason = "passed over " + BossName(next) + " in " + dungeon.name + ", " + count;
        return;
    }

    // Any travel target but the next boss goes, at a boss that died or one that replaced it, such as a grind spot:
    // upstream asks for no travel target while one is active, so the dungeon's request could not take it back.
    if (PlayerbotAI* ai = leader->GetPlayerbotAI())
    {
        TravelTarget* target = ai->GetAiObjectContext()->GetValue<TravelTarget*>("travel target")->Get();
        if (target && target->IsActive() && (run.nextPlace ? target->GetDestination() != run.nextPlace : target->GetEntry() != next))
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
    // A member inside with the leader out goes home by hearthstone once it has stood still for STRANDED_SECONDS:
    // left behind where no route leads out, such as caught on the Deadmines ship. One still on its way keeps going,
    // such as a member waiting for the Vator in Gnomeregan, whom the hearthstone took as it boarded.
    const bool mayHearth = leaderOut && now - run.lastHearth >= HEARTH_RETRY_SECONDS;
    bool hearthed = false;
    if (!leaderOut)
        run.stillInside.clear();

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
            PlayerbotAI* memberAi = member->GetPlayerbotAI();
            if (leaderOut && memberAi && sGuildBotMgr.IsGuildBot(member) && member->IsAlive() && !member->IsBeingTeleported())
            {
                Run::Still& still = run.stillInside[member->GetGUIDLow()];
                if (!still.since || member->GetDistance(still.x, still.y, still.z) > STRANDED_STILL)
                {
                    still.x = member->GetPositionX();
                    still.y = member->GetPositionY();
                    still.z = member->GetPositionZ();
                    still.since = now;
                }
                else if (mayHearth && now - still.since >= STRANDED_SECONDS)
                {
                    bool home = memberAi->DoSpecificAction("hearthstone", Event(), true);
                    update.events.push_back({ "recovery", std::string(member->GetName()) + (home ? " hearthstone out of " : " hearthstone not ready in ") + dungeon.name });
                    still.since = now;
                    hearthed = true;
                }
            }
        }
        if (hearthed)
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
                // Far inside, the place by the way out; near it, the way out itself, as ExitDestination gives them.
                TravelDestination* wanted = dungeon.inside && dungeon.inside->DistanceTo(WorldPosition(leader)) > EXIT_NEAR
                    ? static_cast<TravelDestination*>(dungeon.inside) : static_cast<TravelDestination*>(dungeon.exit);
                if (target && target->IsActive() && target->GetDestination() != wanted)
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
    // The way out is behind it. Its travel target there stays active and forced otherwise, and no new travel is asked
    // for while one is active, so the next run's way to the dungeon never starts: Ragefire Chasm's leader spent the
    // half hour after its second run on errands around Orgrimmar with "out of Ragefire Chasm" still its target.
    if (PlayerbotAI* ai = leader->GetPlayerbotAI())
        sTravelMgr.SetNullTravelTarget(ai->GetAiObjectContext()->GetValue<TravelTarget*>("travel target")->Get());
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
    // Not while it stays by an event's creature, such as an escort: it moves with that instead.
    return run.phase == Phase::Bosses && run.nextBoss && !run.waiting && !run.holding && bot->GetMapId() == run.dungeon.mapId;
}

bool GuildDungeon::IsLeaving(Player* leader) const
{
    std::lock_guard<std::mutex> guard(lock);
    auto it = runs.find(leader->GetGUIDLow());
    return it != runs.end() && it->second.phase == Phase::Leave;
}

bool GuildDungeon::IsHolding(Player* bot) const
{
    std::lock_guard<std::mutex> guard(lock);
    auto it = runs.find(bot->GetGUIDLow());
    // Holding: by the entrance while the party comes in, or before a boss while a member far behind catches up.
    return it != runs.end() && (it->second.phase == Phase::Hold || (it->second.phase == Phase::Bosses && it->second.waiting));
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
    AreaTableEntry const* area = GetAreaEntryByAreaID(areaId);
    if (!area)
        return 0;
    if (MapEntry const* map = sMapStore.LookupEntry(area->mapid))
        if (map->IsDungeon())
            return map->MapID;
    // Some stones name the outdoor area in front of the dungeon, which has the dungeon's name but lies on the continent:
    // Gnomeregan, Zul'Farrak and Uldaman. The dungeon map of the same name is theirs.
    const int locale = sWorld.GetDefaultDbcLocale();
    const char* areaName = area->area_name[locale];
    if (!areaName || !*areaName)
        return 0;
    for (uint32 i = 0; i < sMapStore.GetNumRows(); ++i)
        if (MapEntry const* map = sMapStore.LookupEntry(i))
            if (map->IsDungeon() && map->name[locale] && strcmp(map->name[locale], areaName) == 0)
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
    if (!MayEnterMap(player, mapId, &why))
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
            {
                // By the stone, which takes it in (FollowByStone). One the stone has not taken in after a minute
                // walks in by the entrance instead, whose trigger has the same guard.
                std::lock_guard<std::mutex> guard(refusalLock);
                auto since = stoneSince.emplace(bot->GetGUIDLow(), time(nullptr)).first;
                if (time(nullptr) - since->second < STONE_WAIT_SECONDS)
                    return false;
                return LeaderAcrossEntrance(bot, place);
            }
            place = WorldLocation(stone.getMapId(), stone.getX(), stone.getY(), stone.getZ(), stone.getO());
            return true;
        }
        return LeaderAcrossEntrance(bot, place);
    }

    // The leader at an object that needs more than one user, such as an altar: to it, near enough to use it.
    if (sGuildDungeon.UseSpot(leader, bot, place))
        return true;

    {
        std::lock_guard<std::mutex> guard(refusalLock);
        stoneSince.erase(bot->GetGUIDLow());
    }

    // Upstream's follow gives up this far off; a follower that wandered or was taken away travels back.
    // So does one far below or above its leader outside, such as under it at the lift by Gnomeregan, where follow
    // stops beneath the leader: the travel map's route takes the lift.
    MapEntry const* here = sMapStore.LookupEntry(bot->GetMapId());
    // Inside a dungeon only while the leader leaves it, as from Gnomeregan by the Vator: during the run, follow stays as it is.
    const bool leaving = here && here->IsDungeon() && sGuildDungeon.IsLeaving(leader);
    const bool apart = here && (!here->IsDungeon() || leaving) && std::fabs(bot->GetPositionZ() - leader->GetPositionZ()) > FOLLOW_BY_TRAVEL_HEIGHT;
    if (bot->GetDistance(leader) > FOLLOW_BY_TRAVEL || apart)
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

TravelDestination* GuildDungeon::BossDestination(Player* bot) const
{
    std::lock_guard<std::mutex> guard(lock);
    auto it = runs.find(bot->GetGUIDLow());
    if (it == runs.end() || it->second.phase != Phase::Bosses || it->second.useSpawn || bot->GetMapId() != it->second.dungeon.mapId)
        return nullptr;
    return it->second.nextPlace;
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
    for (const Dungeon::ItemUse& use : it->second.dungeon.itemUses)
        if (use.spawn == it->second.useSpawn)
            return use.place;
    return nullptr;
}

void GuildDungeon::HoldNear(Player* leader, WorldObject* by)
{
    HoldNear(leader, by, HOLD_NEAR, HOLD_STAND);
}

void GuildDungeon::Approach(Player* leader, WorldObject* to, TravelDestination* place, float near, float stand, Run& run)
{
    if (leader->GetDistance(to) <= APPROACH_RANGE)
    {
        HoldNear(leader, to, near, stand);
        run.holding = true;
        return;
    }
    // Farther off it travels. A travel target to the same place that counts as working there, or waits out its cooldown,
    // while the leader is far from it starts again; one elsewhere goes.
    if (PlayerbotAI* ai = leader->GetPlayerbotAI())
    {
        TravelTarget* target = ai->GetAiObjectContext()->GetValue<TravelTarget*>("travel target")->Get();
        if (!target)
            return;
        const TravelStatus status = target->GetStatus();
        if (target->GetDestination() != place ? target->IsActive()
            : status == TravelStatus::TRAVEL_STATUS_WORK || status == TravelStatus::TRAVEL_STATUS_COOLDOWN)
            target->SetStatus(TravelStatus::TRAVEL_STATUS_EXPIRED);
    }
}

void GuildDungeon::HoldNear(Player* leader, WorldObject* by, float near, float stand)
{
    // No travel target while it stays: the dungeon's travel would take it on to the next boss.
    if (PlayerbotAI* ai = leader->GetPlayerbotAI())
    {
        TravelTarget* target = ai->GetAiObjectContext()->GetValue<TravelTarget*>("travel target")->Get();
        if (target && target->IsActive())
            target->SetStatus(TravelStatus::TRAVEL_STATUS_EXPIRED);
    }
    // In a fight it fights where the fight is; out of one, it goes back by the creature.
    if (leader->IsInCombat() || leader->GetDistance(by) <= near || leader->IsMoving())
        return;
    float x, y, z;
    by->GetNearPoint(leader, x, y, z, leader->GetObjectBoundingRadius(), stand, by->GetAngle(leader));
    leader->GetMotionMaster()->MovePoint(0, x, y, z, FORCED_MOVEMENT_RUN);
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
    // An item to use on the object: the member who carries it comes to it.
    for (const Dungeon::ItemUse& use : it->second.dungeon.itemUses)
    {
        if (use.spawn != it->second.useSpawn || it->second.itemUsedAt.count(use.spawn) || !member->HasItemCount(use.item, 1))
            continue;
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
    // Far inside, the place by the way out comes first. The travel map may have no route from the last boss to the way
    // out, as from Verdan in Wailing Caverns, and the way out leads to another map, which no walk by the movement maps
    // reaches. The place inside is on the dungeon's own map, so a walk does, and from it the route out is a short one.
    const Dungeon& dungeon = it->second.dungeon;
    if (dungeon.inside && dungeon.inside->DistanceTo(WorldPosition(bot)) > EXIT_NEAR)
        return dungeon.inside;
    return dungeon.exit;
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
    // Nor while it waits by a boss that fell asleep, which an event wakes.
    if (it->second.useSpawn || it->second.dungeon.talks.count(it->second.nextBoss) || it->second.sleeperWaiting)
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

bool GuildDungeon::MayEnterMap(Player* bot, uint32 mapId, std::string* why)
{
    Group* group = bot->GetGroup();
    if (!group)
        return true;

    if (sGuildBotMgr.IsGuildBot(bot) && !group->IsLeader(bot->GetObjectGuid()))
    {
        // The leader first, a player or a guild character: inside, in the world, and the group bound.
        Player* leader = sObjectMgr.GetPlayer(group->GetLeaderGuid());
        if (!leader || !leader->IsInWorld() || leader->IsBeingTeleportedFar() || leader->GetMapId() != mapId)
        {
            if (why)
                *why = "The leader is not inside yet.";
            return false;
        }
        if (!group->GetBoundInstance(mapId))
        {
            if (why)
                *why = "The group is not bound to the leader's instance.";
            return false;
        }
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

bool GuildDungeon::LetsFinish(uint32 spellId)
{
    // Casts an event needs to land, which the party leaves to finish.
    // Scarlet Monastery: Whitemane cannot die until her Scarlet Resurrection has raised Mograine.
    static const std::set<uint32> spells = { 9232 };
    return spells.count(spellId);
}

bool GuildDungeon::IsStepItem(uint32 mapId, uint32 itemId)
{
    for (const ItemOnObject& use : ITEM_USES)
        if (use.map == mapId && use.item == itemId)
            return true;
    return false;
}

bool GuildDungeon::KeepsToCorpse(Player* bot)
{
    if (!sGuildBotMgr.IsGuildBot(bot) || !bot->GetGroup() || bot->IsAlive())
        return false;
    Corpse* corpse = bot->GetCorpse();
    if (!corpse)
        return false;
    MapEntry const* map = sMapStore.LookupEntry(corpse->GetMapId());
    if (!map || !map->IsNonRaidDungeon())
        return false;
    // A ghost that has not got back in after a long while cannot reach the entrance: the graveyard's way, as before.
    std::lock_guard<std::mutex> guard(deathLock);
    auto since = deadSince.emplace(bot->GetGUIDLow(), time(nullptr)).first;
    return time(nullptr) - since->second < CORPSE_RUN_SECONDS;
}

bool GuildDungeon::CorpseGraveyard(Player* ghost, WorldLocation& place)
{
    if (!sGuildBotMgr.IsGuildBot(ghost) || !ghost->IsInWorld() || !ghost->HasFlag(PLAYER_FLAGS, PLAYER_FLAGS_GHOST))
        return false;
    Corpse* corpse = ghost->GetCorpse();
    MapEntry const* map = corpse ? sMapStore.LookupEntry(corpse->GetMapId()) : nullptr;
    if (!map || !map->IsNonRaidDungeon())
        return false;
    AreaTrigger const* at = EntranceNear(corpse->GetMapId(), corpse->GetPositionX(), corpse->GetPositionY(), corpse->GetPositionZ());
    AreaTriggerEntry const* entry = at ? sAreaTriggerStore.LookupEntry(at->entry) : nullptr;
    if (!entry || entry->mapid != ghost->GetMapId())
        return false;
    WorldSafeLocsEntry const* grave = ghost->GetMap()->GetGraveyardManager().GetClosestGraveYard(
        entry->x, entry->y, entry->z, entry->mapid, ghost->GetTeam());
    if (!grave || grave->map_id != ghost->GetMapId())
        return false;
    place = WorldLocation(grave->map_id, grave->x, grave->y, grave->z, grave->o);
    return true;
}

bool GuildDungeon::CorpseEntrance(Player* ghost, WorldLocation& place, uint32& triggerId, float& reach)
{
    if (!ghost->IsInWorld() || !ghost->HasFlag(PLAYER_FLAGS, PLAYER_FLAGS_GHOST) || !KeepsToCorpse(ghost))
        return false;
    Corpse* corpse = ghost->GetCorpse();
    if (corpse->GetMapId() == ghost->GetMapId())
        return false;
    // The entrance of the wing the corpse lies in.
    AreaTrigger const* at = EntranceNear(corpse->GetMapId(), corpse->GetPositionX(), corpse->GetPositionY(), corpse->GetPositionZ());
    AreaTriggerEntry const* entry = at ? sAreaTriggerStore.LookupEntry(at->entry) : nullptr;
    if (!entry || entry->mapid != ghost->GetMapId())
        return false;
    triggerId = entry->id;
    place = WorldLocation(entry->mapid, entry->x, entry->y, entry->z, 0.0f);
    reach = std::max({ entry->radius, entry->box_x / 2.0f, entry->box_y / 2.0f, 2.0f });
    return true;
}

bool GuildDungeon::WaitsForResurrect(Player* dead)
{
    if (!sGuildBotMgr.IsGuildBot(dead) || dead->IsAlive() || dead->GetCorpse() || !dead->IsInWorld())
        return false;
    Group* group = dead->GetGroup();
    MapEntry const* map = sMapStore.LookupEntry(dead->GetMapId());
    if (!group || !map || !map->IsNonRaidDungeon())
        return false;

    const time_t now = time(nullptr);
    time_t since;
    {
        std::lock_guard<std::mutex> guard(deathLock);
        since = deadSince.emplace(dead->GetGUIDLow(), now).first->second;
    }
    if (now - since >= RESURRECT_WAIT_SECONDS)
        return false;

    // A priest, paladin or shaman of the party, alive and near: it resurrects the dead once the fight is over.
    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->getSource();
        if (!member || member == dead || !member->IsInWorld() || !member->IsAlive() || member->GetMapId() != dead->GetMapId()
            || member->GetDistance(dead) > RESURRECT_NEAR)
            continue;
        const uint8 cls = member->getClass();
        if (cls == CLASS_PRIEST || cls == CLASS_PALADIN || cls == CLASS_SHAMAN)
            return true;
    }
    return false;
}

void GuildDungeon::NoteAlive(Player* bot)
{
    if (!bot->IsAlive())
        return;
    std::lock_guard<std::mutex> guard(deathLock);
    deadSince.erase(bot->GetGUIDLow());
}
