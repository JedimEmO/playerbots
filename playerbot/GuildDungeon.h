#pragma once

#include "Common.h"

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
namespace ai { class TravelDestination; class PlaceTravelDestination; }

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
    int32 TravelEntry(Player* bot) const;
    // The way out, for the leader inside once the last boss is dead; null otherwise.
    ai::TravelDestination* ExitDestination(Player* bot) const;
    // The next boss itself, for its party's leader at the boss's place: alive, loaded, and this near.
    // Upstream's grind skips a boss out of sight, such as VanCleef on the deck above, so the leader takes it as its target.
    Creature* NextBoss(Player* bot, float within) const;

    // The entry guard, for every area trigger a bot takes. Two members entering an unbound instance at once
    // can crash the server on the group bind, so a guild character in a group enters a dungeon only first,
    // as the group's leader, or after the leader is inside and the group is bound, and one at a time.
    static bool MayEnter(Player* bot, uint32 triggerId);
    // Where a guild character should go when its group's leader is on the other side of a dungeon's entrance,
    // inside while it is outside or outside while it is inside: the leader's place. Its follow target is out of
    // reach on another map, so follow moves there instead, and the route the travel map gives runs through the
    // entrance or the way out. False when it is not in that position.
    static bool LeaderAcrossEntrance(Player* bot, WorldLocation& leaderPlace);

    // For the run record: the dungeon on this map, its bosses in order with their spawns, and the last boss.
    struct Bosses { std::string name; std::vector<int32> entries; std::vector<uint32> spawns; int32 last = 0; };
    static bool BossesOf(uint32 mapId, Player* member, Bosses& out);
    // The boss's spawn in this instance is dead: killed, with its respawn time saved, or a corpse now.
    static bool BossDead(Map* map, uint32 spawn);
    // The boss's spawn exists in this instance: false for a pooled rare the pool did not choose.
    static bool BossSpawned(Map* map, uint32 spawn);

private:
    GuildDungeon() = default;

    enum class Phase { Check, Go, Hold, Bosses, Leave };

    struct Dungeon
    {
        std::string name;
        uint32 mapId = 0;
        AreaTrigger const* entrance = nullptr;
        std::vector<int32> bosses;     // boss entries, nearest the entrance first
        std::vector<uint32> spawns;    // each boss's spawn on the map, by database guid; 0 if none is known
        int32 lastBoss = 0;            // the boss whose death ends the run
        uint32 bossLevel = 0;          // the level of the last boss, which the party must be near
        ai::PlaceTravelDestination* exit = nullptr;  // outside, where the way out leads; kept in exits
    };

    struct Run
    {
        Dungeon dungeon;
        Phase phase = Phase::Check;
        bool resolved = false;
        bool entered = false;          // the whole party has been inside
        int32 nextBoss = 0;            // the boss the party is on its way to
        uint32 nextSpawn = 0;          // its spawn
        std::set<int32> killed;        // bosses reported dead
        bool bossesSeen = false;       // the bosses have been looked at once in this run
        time_t leaveStarted = 0;       // when the last boss died and the party set out for the way out
        time_t leaderOutAt = 0;        // when the leader came out, with others still inside
        time_t lastHearth = 0;         // when a member stranded inside was last sent home by hearthstone
        std::string cleared;           // what the run did, for the job's reason once it is done
    };

    static bool Resolve(const std::string& name, Player* leader, Dungeon& dungeon, std::string& error);
    static uint32 FindMap(const std::string& name);
    // Why the party cannot set out yet, or empty. Sets failed when waiting would not help.
    static std::string CheckParty(Player* leader, const Dungeon& dungeon, bool& failed);
    // The bosses step, inside with the whole party.
    void TickBosses(Player* leader, Run& run, Update& update);
    // The finish step: out, then the reset.
    void TickLeave(Player* leader, Run& run, Update& update);

    mutable std::mutex lock;
    std::map<uint32, Run> runs;                 // by leader guid
    static std::mutex exitLock;
    static std::map<uint32, std::unique_ptr<ai::PlaceTravelDestination>> exits;  // by dungeon map, never freed
    static std::mutex entryLock;
    static std::map<uint32, uint32> lastEntry;  // by group id: when a member last took the entrance, in ms
};

#define sGuildDungeon GuildDungeon::instance()
