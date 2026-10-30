#pragma once

#include "Common.h"

#include <array>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <utility>
#include <vector>

class Creature;
class Map;
class Player;
struct AreaTrigger;
namespace ai { class TravelDestination; class PlaceTravelDestination; class WorldPosition; }

// The dungeon job: a party of guild characters runs a five-player dungeon. The leader, normally the tank,
// has the job; the others are on the party job with it as their leader. See docs/dungeons.md.
//
// Built so far: Resolve (the name to a map, its entrance and its bosses), Check (a full party that fits
// the dungeon and may enter it), Go (a forced travel target at the first boss, whose route passes the
// entrance), Enter (the leader goes in first, the others one at a time, and it holds until all are in)
// Bosses (a forced travel target at the first boss still alive, until the last boss dies) and Finish (out by the
// way out, the others following, and the instance reset once nobody is inside; then done).
class GuildDungeon
{
public:
    static GuildDungeon& instance()
    {
        static GuildDungeon instance;
        return instance;
    }

    struct Update
    {
        std::string status = "working";  // working, done or failed
        std::string reason;
        std::vector<std::pair<std::string, std::string>> events;  // kind, detail
        bool strategiesChanged = false;   // the leader's non-combat strategies must be rebuilt
    };

    // Empty when the job can run a dungeon by this name, else why not.
    static std::string Validate(const std::string& name);

    // Every few seconds, from GuildBotMgr, for a leader on the dungeon job.
    Update Tick(Player* leader, const std::string& name);
    // The job ended or changed: forget the run.
    void Stop(uint32 leaderGuid);

    // For the leader's strategies and travel: travelling to the dungeon now, holding inside it,
    // and the boss whose destination the travel target takes.
    bool ShouldTravel(Player* bot) const;
    bool IsHolding(Player* bot) const;
    // The leader picks no rpg target: inside its dungeon, or near its stone on the way in.
    bool NoRpg(Player* bot) const;
    // The leader's run is in its leave phase, on the way out after the last boss.
    bool IsLeaving(Player* leader) const;
    // The leader, or a member of its group, is inside its dungeon in the bosses step: its routes stay on the map.
    bool KeepsToMap(Player* bot) const;
    // The leader, or a member of its group, is out of its dungeon to repair, at the one repairer the run chose.
    bool OnRepairTrip(Player* bot) const;
    // The keys to the locked doors of the dungeon a job names, for the bench to hand out.
    static std::vector<uint32> DungeonKeys(const std::string& name);
    int32 TravelEntry(Player* bot) const;
    // The way out, for the leader inside once the last boss is dead; null otherwise.
    ai::TravelDestination* ExitDestination(Player* bot) const;
    // The next boss itself, for its party's leader at the boss's place: alive, loaded, and this near.
    // Upstream's grind skips a boss out of sight, such as VanCleef on the deck above, so the leader takes it as its target.
    Creature* NextBoss(Player* bot, float within) const;
    // A creature its party's leader kills before the next boss, such as one an event brought: alive, loaded, and this near.
    // The leader takes it as its target even while travelling.
    Creature* KillTarget(Player* bot, float within) const;

    // The entry guard, for every area trigger a bot takes. Two members entering an unbound instance at once
    // can crash the server on the group bind, so a guild character in a group enters a dungeon only first,
    // as the group's leader, or after the leader is inside and the group is bound, and one at a time.
    static bool MayEnter(Player* bot, uint32 triggerId);

    // The meeting stone enhancement: a stone used with no party member selected takes the user into its dungeon,
    // after the entrance's own checks (level, key, quest, the hourly limit) and one member of a group at a time.
    // Selecting a member keeps the stone's summoning. The core calls UseStone from the stone's use.
    static void UseStone(Player* player, uint32 areaId);
    // Through this entrance, or the map's default when none is given.
    static bool EnterByStone(Player* player, uint32 mapId, std::string& why, AreaTrigger const* entrance = nullptr);
    // The entrances into a dungeon map from outside it: one, or one per wing, as Scarlet Monastery has four.
    static std::vector<AreaTrigger const*> Entrances(uint32 mapId);
    // The entrance whose arrival is nearest a place on the dungeon map: the wing it is in.
    static AreaTrigger const* EntranceNear(uint32 mapId, float x, float y, float z);
    // The dungeon a stone's area names, or 0.
    static uint32 StoneMap(uint32 areaId);
    // Where the dungeon's stone stands; false when it has none.
    static bool StonePlace(uint32 mapId, ai::WorldPosition& place);
    // A guild character in a group, at the stone of the dungeon its leader is in: it goes in after the leader.
    static void FollowByStone(Player* member);
    // The stone, for the leader on its way to the dungeon; null when the dungeon has none.
    ai::TravelDestination* StoneDestination(Player* bot) const;
    // An object the leader has to go to and use before the next boss, such as an altar; null when there is none.
    ai::TravelDestination* UseDestination(Player* bot) const;
    // The living spawn of a next boss with several spawns, which its boss destination would not pick out; null otherwise.
    ai::TravelDestination* BossDestination(Player* bot) const;
    // The repairer the party goes to on a repair trip, for the leader; null otherwise.
    ai::TravelDestination* RepairDestination(Player* bot) const;
    // For a member whose leader is at an object that needs more than one user: a place by it, near enough to use it.
    bool UseSpot(Player* leader, Player* member, WorldLocation& place) const;
    // For a member whose leader gathers for or pulls a creature ahead, and which stands nearer that creature than the
    // leader: a place behind the leader on its way, where it waits.
    bool PullHoldSpot(Player* leader, Player* member, WorldLocation& place) const;
    // The member's party holds at a corner just after a pull, and the member stands by it: it moves to no creature yet.
    bool HoldsAtCorner(Player* member) const;
    // The leader of an event stays by a creature or object of it, such as an escort: no travel, and back by it out of a fight.
    static void HoldNear(Player* leader, WorldObject* by);
    // The same, staying this near and coming back to this near.
    static void HoldNear(Player* leader, WorldObject* by, float near, float stand);
    // Where a guild character should go when its group's leader is on the other side of a dungeon's entrance,
    // inside while it is outside or outside while it is inside: the leader's place. Its follow target is out of
    // reach on another map, so follow moves there instead, and the route the travel map gives runs through the
    // entrance or the way out. False when it is not in that position.
    static bool LeaderAcrossEntrance(Player* bot, WorldLocation& leaderPlace);
    // Where a guild character in a group travels instead of following, when following cannot work:
    // its leader across a dungeon's entrance (the stone, when the dungeon has one), or far off on the same map.
    static bool FollowByTravel(Player* bot, WorldLocation& place);
    // Adds the links the travel map lacks inside dungeons, from points walked by hand, once it is loaded.
    static void AddHandLinks();
    // The one-at-a-time entry guard, for the trigger and the stone alike; guild characters also go after their leader.
    // Why not, when it is a reason that lasts: the leader not inside yet, or the group not bound.
    static bool MayEnterMap(Player* player, uint32 mapId, std::string* why = nullptr);

    // A dead guild character in a group whose corpse lies in a dungeon goes back to it, as a player would:
    // it releases, walks to the dungeon's entrance as a ghost and takes it, which revives it inside by the core's rule.
    // It never takes a spirit healer, a repop, or a graveyard elsewhere.
    static bool KeepsToCorpse(Player* bot);
    // An item a step of a dungeon on this map needs, such as Zul'Farrak's Executioner's Key: a guild character
    // in a party there needs it in a roll, keeps it, and takes it for itself as master looter.
    static bool IsStepItem(uint32 mapId, uint32 itemId);
    // A spell a creature casts that an event needs to land, such as Whitemane's resurrection of Mograine:
    // bots do not interrupt it.
    static bool LetsFinish(uint32 spellId);
    // For such a ghost outside: the entrance trigger it walks to, its place and how near counts as in it.
    // False when it is not outside, or the trigger is not on its map.
    static bool CorpseEntrance(Player* ghost, WorldLocation& place, uint32& triggerId, float& reach);
    // The ghost is near another way into the dungeon its corpse lies in than the trigger it goes for.
    static bool NearOtherEntrance(Player* ghost, uint32 triggerId, uint32 dungeonMap);
    // For a guild ghost whose corpse lies in a dungeon: the graveyard of its faction nearest that dungeon's entrance,
    // where it takes the spirit healer once the corpse run has taken too long. False for any other ghost.
    static bool CorpseGraveyard(Player* ghost, WorldLocation& place);
    // A guild character dead in a dungeon does not release for a while when a party member who can resurrect it
    // is alive and near; it waits to be resurrected, as a player would.
    static bool WaitsForResurrect(Player* dead);
    // Called for every guild character on each check: a living one forgets when it died.
    static void NoteAlive(Player* bot);

    // For the run record: the dungeon on this map, its bosses in order with their spawns, and the last boss.
    struct Bosses { std::string name; std::vector<int32> entries; std::vector<std::vector<uint32>> spawns; int32 last = 0; };
    static bool BossesOf(uint32 mapId, Player* member, Bosses& out);
    // The boss's spawn in this instance is dead: killed, with its respawn time saved, or a corpse now.
    static bool BossDead(Map* map, uint32 spawn);
    // The boss's spawn exists in this instance: false for a pooled rare the pool did not choose.
    static bool BossSpawned(Map* map, uint32 spawn);
    // A boss with several spawns, a pooled rare or one placed more than once, exists in this instance while any of
    // them does, and is dead when every one this instance has is dead.
    static bool BossSpawned(Map* map, const std::vector<uint32>& spawns);
    static bool BossDead(Map* map, const std::vector<uint32>& spawns);
    // Its living spawn in this instance nearest a place, or 0.
    static uint32 LiveSpawn(Map* map, const std::vector<uint32>& spawns, const ai::WorldPosition& near);

private:
    GuildDungeon() = default;

    enum class Phase { Check, Go, Hold, Bosses, Leave, Repair };

    struct Dungeon
    {
        std::string name;
        uint32 mapId = 0;
        AreaTrigger const* entrance = nullptr;
        std::vector<int32> bosses;     // boss entries, in route order from the entrance, the last boss last
        // Each boss's spawns on the map, by database guid: one, or several for a pooled rare or a boss placed more than once.
        std::vector<std::vector<uint32>> spawns;
        std::map<int32, uint32> talks; // steps that are friendly creatures to speak to, by entry: the gossip action to pick
        std::map<int32, uint32> talkDoors; // the spawn of the door each of them opens, by entry
        std::set<int32> talkEscorts;   // those that walk an escort after speaking: the party stays with them until they are gone
        std::map<int32, time_t> talkWaits; // how long the party waits for each after speaking, by entry, when not the default
        std::map<int32, std::vector<int32>> talkAfter; // bosses that must be dead before each is spoken to, by entry
        int32 lastBoss = 0;            // the boss whose death ends the run
        uint32 bossLevel = 0;          // the level of the last boss, which the party must be near
        ai::PlaceTravelDestination* exit = nullptr;  // outside, where the way out leads; kept in exits
        ai::PlaceTravelDestination* inside = nullptr; // inside, where the entrance puts a party, by the way out; kept in insides
        ai::PlaceTravelDestination* stone = nullptr; // the dungeon's meeting stone, if it has one; kept in stones
        // Objects to use before a boss, as a player would: the object's spawn, how many members use it, the boss,
        // and whether it wakes the boss, so that the step is due for as long as the boss sleeps.
        // An object used several times, such as a gong, is used each time it can be, and is done when what it brings is dead.
        struct Use { uint32 entry = 0; uint32 spawn = 0; uint32 users = 1; int32 before = 0; bool wakes = false;
                     uint32 times = 1; uint32 brings = 0; uint32 report = 0; std::array<uint32, 2> waves = {};
                     ai::PlaceTravelDestination* place = nullptr; };
        std::vector<Use> uses;
        // Creatures to kill before a boss: their entry, the boss, and the spawn of the door their deaths open, or 0.
        // Only those within nearBoss of the boss's spawn, when it is set.
        struct Kill { uint32 entry = 0; int32 before = 0; uint32 doorSpawn = 0; float nearBoss = 0.0f; };
        std::vector<Kill> kills;
        // Places the leader goes by once a boss is dead, before anything else: the boss, and the place.
        struct Via { int32 after = 0; ai::PlaceTravelDestination* place = nullptr; };
        std::vector<Via> vias;
        // Items to use on an object before a boss, as a player would: the item, the object's spawn, the boss,
        // the door the event it starts opens, the creature the party stays by meanwhile, and how long it waits.
        struct ItemUse { uint32 item = 0; uint32 spawn = 0; int32 before = 0; uint32 doorSpawn = 0; uint32 guard = 0; time_t wait = 0;
                         ai::PlaceTravelDestination* place = nullptr; };
        std::vector<ItemUse> itemUses;
    };

    struct Run
    {
        Dungeon dungeon;
        Phase phase = Phase::Check;
        bool resolved = false;
        bool entered = false;          // the whole party has been inside
        int32 nextBoss = 0;            // the boss the party is on its way to
        uint32 nextSpawn = 0;          // its spawn
        ai::PlaceTravelDestination* nextPlace = nullptr; // where that spawn stands, when the boss has several; kept in bossPlaces
        std::set<int32> killed;        // bosses reported dead
        std::set<int32> viasDone;      // the bosses whose via place the leader has come by, or given up on
        ai::PlaceTravelDestination* viaPlace = nullptr; // the via place it travels to now, or null
        time_t viaSince = 0;           // since when it has travelled to it
        std::set<int32> talked;        // talk steps done
        std::map<int32, time_t> talkedAt; // when the leader last spoke to each, by entry
        std::set<uint32> used;         // use steps done, by the object's spawn
        uint32 useSpawn = 0;           // the use step the leader is on its way to or at, or 0
        std::map<uint32, time_t> usedAt; // when everyone a use step needs last used it, by the object's spawn
        std::map<uint32, std::set<uint32>> usedBy; // who has used it this time, by the object's spawn and the member's guid
        std::map<uint32, uint32> useCount; // how many times an object used several times has been used, by its spawn
        std::map<uint32, time_t> itemUsedAt; // when an item use step's item was used, by the object's spawn
        std::map<uint32, time_t> itemTriedAt; // when it was last tried, by the object's spawn
        bool holding = false;          // the leader stays by a creature of an event now, and travels nowhere
        int32 sleeper = 0;             // a boss that fell asleep as the next boss, such as Mograine lying as dead, or 0
        uint32 sleeperSpawn = 0;       // its spawn
        time_t sleeperSince = 0;       // when it fell asleep
        bool sleeperWaiting = false;   // the leader waits by it now
        ObjectGuid killGuid;           // the creature the leader kills before the next boss, or empty
        std::set<int32> passed;        // bosses passed over as out of reach on this map
        int32 headwayBoss = 0;         // the boss the leader's headway is measured to
        float headwayBest = -1.0f;     // the nearest the leader has come to it, in yards, or negative before the first look
        time_t headwaySince = 0;       // when the leader last came nearer, or last had a reason to stand still
        time_t nodeWalkAt = 0;         // when the leader last set out for the nearest travel node, its moves failing
        bool nodeWalking = false;      // it is stepping there now, and its travel waits
        int32 nodeWalkBoss = 0;        // the next boss the walks below were for
        uint32 nodeWalks = 0;          // how many it has started for that boss
        float nodeX = 0.0f, nodeY = 0.0f, nodeZ = 0.0f; // the node it steps toward
        float nodeBest = 0.0f;         // the nearest it has come to the node on this walk
        time_t nodeBestAt = 0;         // when it last came nearer by NODE_WALK_GAIN
        int32 waitBoss = 0;            // the boss the leader last waited before for a member far behind
        time_t waitSince = 0;          // when that wait started, or 0
        bool waitedOut = false;        // the wait before that boss is over, and the leader went on without the member
        bool waiting = false;          // the leader waits for a member far behind now, and travels nowhere
        time_t regroupSince = 0;       // since when the leader has waited for a member dead or outside, or 0
        bool regroupedOut = false;     // that wait is over, and the leader went on without the member
        bool repairTrip = false;       // the party is out of the dungeon, or on its way out, to repair
        time_t repairSince = 0;        // when it first came out to repair on this trip, or 0
        ai::PlaceTravelDestination* repairPlace = nullptr; // the repairer it goes to; kept in repairPlaces
        int32 repairEntry = 0;         // that repairer's creature entry
        bool repairArrived = false;    // the leader has come to it, and stays there
        bool repairGaveUp = false;     // a repair trip of this run gave up, and no other starts
        bool insideWalking = false;    // this tick, the leader walks straight to the place by the way in, and travels nowhere
        bool insideWalkedLast = false; // it did on the tick before
        time_t insideWalkSince = 0;    // when that walk started, or 0
        float insideWalkBest = 0.0f;   // the nearest it has come to the place on that walk
        time_t insideWalkBestAt = 0;   // when it last came nearer by INSIDE_WALK_GAIN
        time_t insideWalkFailedAt = 0; // when a walk last stopped without getting there
        ObjectGuid pullGuid;           // the creature ahead the leader stopped to pull, or empty
        time_t pullSeenAt = 0;         // when it stopped for it
        time_t pullAt = 0;             // when the tank pulled it, or 0 while the party gathers
        std::map<ObjectGuid, uint32> pullTries; // pulls that brought nothing, by creature
        std::set<ObjectGuid> pullIgnored; // creatures the leader no longer stops for: given up on, or not pullable
        std::vector<std::array<float, 3>> trail; // the leader's last places inside, one every CORNER_STEP, newest last
        bool cornerGather = false;     // the party gathers at cornerSpot for the creature being pulled, a caster near it
        std::array<float, 3> cornerSpot = {}; // that corner: a place on the leader's way the creature cannot see
        time_t cornerUntil = 0;        // after the pull, the members hold at the corner until then
        float stillX = 0.0f, stillY = 0.0f, stillZ = 0.0f; // where the leader has stood, out of a fight, since stillSince
        time_t stillSince = 0;         // since when it has stood within LEADER_STILL_NEAR of that place, or 0
        time_t travelRestartAt = 0;    // when its travel was last started again for standing still on it
        bool bossesSeen = false;       // the bosses have been looked at once in this run
        time_t leaveStarted = 0;       // when the last boss died and the party set out for the way out
        time_t leaderOutAt = 0;        // when the leader came out, with others still inside
        time_t lastHearth = 0;         // when a member stranded inside was last sent home by hearthstone
        // With the leader out, where each member inside last stood still, and since when, by guid.
        struct Still { float x = 0.0f, y = 0.0f, z = 0.0f; time_t since = 0; };
        std::map<uint32, Still> stillInside;
        bool insideReached = false;    // the leader has come by the way out inside: from now on it goes out
        bool leaveResting = false;     // the leave waits for the party to finish its fight and rest first
        time_t restSince = 0;          // since when the party has rested between fights inside, or 0
        std::string cleared;           // what the run did, for the job's reason once it is done
    };

    static bool Resolve(const std::string& name, Player* leader, Dungeon& dungeon, std::string& error);
    static uint32 FindMap(const std::string& name);
    // Why the party cannot set out yet, or empty. Sets failed when waiting would not help, and farOnly when the reason
    // is a member that is not near the leader.
    static std::string CheckParty(Player* leader, const Dungeon& dungeon, bool& failed, bool* farOnly = nullptr);
    // The bosses step, inside with the whole party.
    // The leader opens a locked door of the key table by it, with the key in its bags.
    static void OpenKeyDoors(Player* leader, Update& update);
    void TickBosses(Player* leader, Run& run, Update& update);
    // The finish step: out, then the reset.
    void TickLeave(Player* leader, Run& run, Update& update);
    void TickRepair(Player* leader, Run& run, Update& update);
    // Walks the leader straight to the place by the way in when a short path there exists, rather than by the travel
    // map's route: true while it walks, and the travel waits.
    bool WalkToInside(Player* leader, Run& run, float near, Update& update);
    // Keeps where the leader stands still, and starts its travel again when the travel is on its way and the leader is not.
    void RestartStalledTravel(Player* leader, Run& run, Update& update);
    // An equipped item of the member's at no durability, by name, or empty.
    static std::string BrokenGear(Player* member);
    // To a step's object or creature: by itself within APPROACH_RANGE, which holds it there, and by travel farther off.
    static void Approach(Player* leader, WorldObject* to, ai::TravelDestination* place, float near, float stand, Run& run);

    mutable std::mutex lock;
    std::map<uint32, Run> runs;                 // by leader guid
    static std::mutex exitLock;
    static std::map<uint32, std::unique_ptr<ai::PlaceTravelDestination>> exits;  // by dungeon map, never freed
    static std::map<uint32, std::unique_ptr<ai::PlaceTravelDestination>> insides; // by entrance trigger, never freed
    static std::map<uint32, std::unique_ptr<ai::PlaceTravelDestination>> stones; // by dungeon map, never freed
    static std::map<uint32, std::unique_ptr<ai::PlaceTravelDestination>> usePlaces; // by object spawn, never freed
    static std::map<int32, std::unique_ptr<ai::PlaceTravelDestination>> viaPlaces; // by the boss before, never freed
    static std::vector<std::unique_ptr<ai::PlaceTravelDestination>> repairPlaces; // repairers runs went to, never freed
    static std::map<uint32, std::unique_ptr<ai::PlaceTravelDestination>> bossPlaces; // by creature spawn, never freed

    static std::mutex entryLock;
    static std::map<uint32, uint32> lastEntry;  // by group id: when a member last took the entrance, in ms
};

#define sGuildDungeon GuildDungeon::instance()
