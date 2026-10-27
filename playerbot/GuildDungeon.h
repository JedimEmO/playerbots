#pragma once

#include "Common.h"

#include <map>
#include <mutex>
#include <set>
#include <string>
#include <utility>
#include <vector>

class Map;
class Player;
struct AreaTrigger;

// The dungeon job: a party of guild characters runs a five-player dungeon. The leader, normally the tank,
// has the job; the others are on the party job with it as their leader. See docs/dungeons.md.
//
// Built so far: Resolve (the name to a map, its entrance and its bosses), Check (a full party that fits
// the dungeon and may enter it), Go (a forced travel target at the first boss, whose route passes the
// entrance), Enter (the leader goes in first, the others one at a time, and it holds until all are in)
// and Bosses (a forced travel target at the first boss still alive; done when the last boss dies).
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

    // The entry guard, for every area trigger a bot takes. Two members entering an unbound instance at once
    // can crash the server on the group bind, so a guild character in a group enters a dungeon only first,
    // as the group's leader, or after the leader is inside and the group is bound, and one at a time.
    static bool MayEnter(Player* bot, uint32 triggerId);
    // Where a guild character outside should go when its group's leader is inside a dungeon: the leader's place.
    // Its follow target is out of reach on another map, so follow moves there instead, and the route the travel
    // map gives runs through the entrance trigger. False when it is not in that position.
    static bool LeaderInDungeon(Player* bot, WorldLocation& leaderPlace);

private:
    GuildDungeon() = default;

    enum class Phase { Check, Go, Hold, Bosses };

    struct Dungeon
    {
        std::string name;
        uint32 mapId = 0;
        AreaTrigger const* entrance = nullptr;
        std::vector<int32> bosses;     // boss entries, nearest the entrance first
        std::vector<uint32> spawns;    // each boss's spawn on the map, by database guid; 0 if none is known
        int32 lastBoss = 0;            // the boss whose death ends the run
        uint32 bossLevel = 0;          // the level of the last boss, which the party must be near
    };

    struct Run
    {
        Dungeon dungeon;
        Phase phase = Phase::Check;
        bool resolved = false;
        bool entered = false;          // the whole party has been inside
        int32 nextBoss = 0;            // the boss the party is on its way to
        std::set<int32> killed;        // bosses reported dead
        bool bossesSeen = false;       // the bosses have been looked at once in this run
    };

    static bool Resolve(const std::string& name, Player* leader, Dungeon& dungeon, std::string& error);
    static uint32 FindMap(const std::string& name);
    // Why the party cannot set out yet, or empty. Sets failed when waiting would not help.
    static std::string CheckParty(Player* leader, const Dungeon& dungeon, bool& failed);
    // The boss's spawn in this instance is dead: killed, with its respawn time saved, or a corpse now.
    static bool BossDead(Map* map, uint32 spawn);
    // The boss's spawn exists in this instance: false for a pooled rare the pool did not choose.
    static bool BossSpawned(Map* map, uint32 spawn);
    // The bosses step, inside with the whole party.
    void TickBosses(Player* leader, Run& run, Update& update);

    mutable std::mutex lock;
    std::map<uint32, Run> runs;                 // by leader guid
    static std::mutex entryLock;
    static std::map<uint32, uint32> lastEntry;  // by group id: when a member last took the entrance, in ms
};

#define sGuildDungeon GuildDungeon::instance()
