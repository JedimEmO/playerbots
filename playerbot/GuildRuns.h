#pragma once

#include "Common.h"
#include "Entities/ObjectGuid.h"

#include <list>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

class Player;
class WorldPacket;

// The record of dungeon runs, for the player's agent to learn from: which dungeon, who, how long, what each
// member gained, which bosses died when, and what dropped, who got it and what became of it.
// It observes; the dungeon job controls. A run is recorded whoever leads it: a guild character on the job,
// or the player with alts following, as long as a guild character is in the group to see it.
// See docs/dungeons.md, "Runs as data".
//
// Tables (characters database):
//   guildbot_run         per run: dungeon, instance, leader, whether a player led, opened, entered, closed, outcome
//   guildbot_run_member  per run and member: levels, experience, coin, deaths, seconds inside
//   guildbot_run_boss    per run and boss: when it died, minutes since the run entered or the boss before
//   guildbot_loot        per item looted in a run: who, how (need, greed, master, looted), its source and its fate
class GuildRuns
{
public:
    static GuildRuns& instance()
    {
        static GuildRuns instance;
        return instance;
    }

    void EnsureTables();
    // Every check, for each guild character the engine plays: opens, counts and closes its group's run.
    void Observe(Player* bot);
    // After a round of checks: closes runs nobody has been inside for a while, and looks at the fate of items.
    void Sweep();
    // The dungeon job reset this instance.
    void InstanceReset(uint32 instanceId);
    // A packet a guild character received: the item pushes and roll results its group is told of.
    void OnPacket(Player* bot, const WorldPacket& packet);

    std::list<std::string> HandleRuns(const std::string& args);

private:
    GuildRuns() = default;

    struct Member
    {
        std::string name;
        bool player = false;           // a player's own character, not one the engine plays
        uint32 levelIn = 0, levelOut = 0;
        uint32 lastLevel = 0, lastXp = 0, lastMoney = 0;
        uint32 experience = 0, money = 0, deaths = 0, seconds = 0;
        bool dead = false;
        time_t seenAt = 0;             // the last check it was counted inside
        bool dirty = false;
    };

    struct Run
    {
        uint32 instanceId = 0;
        uint32 mapId = 0;
        time_t opened = 0;
        time_t entered = 0;            // when the whole group was first inside
        time_t lastInside = 0;         // when a member was last seen inside
        std::string dungeon;
        uint32 leader = 0;
        bool playerLed = false;
        std::vector<int32> bosses;
        std::vector<std::vector<uint32>> spawns; // each boss's spawns, as the dungeon job keeps them
        int32 lastBoss = 0;
        std::set<int32> killed;
        bool bossesSeen = false;
        time_t lastKill = 0;
        uint32 wipes = 0;
        bool wiped = false;
        uint32 lastObservedMs = 0;
        time_t lastWrite = 0;
        std::map<uint32, Member> members;
        bool dirty = false;
    };

    struct Loot
    {
        uint32 id = 0;                 // the row's id, once written
        uint32 instanceId = 0;
        time_t opened = 0;
        time_t at = 0;
        uint32 receiver = 0;
        uint32 entry = 0;
        uint32 count = 0;
        ObjectGuid item;               // the item itself, when it went into a slot of its own
        std::string fate = "kept";
    };

    struct RollWon { uint32 winner = 0; uint32 entry = 0; uint8 type = 0; uint32 ms = 0; };

    Run* OpenRun(Player* bot);
    void ObserveRun(Run& run, Player* bot);
    void WriteRun(Run& run);
    void CloseRun(Run& run, const std::string& outcome);
    void CheckFates();

    std::mutex lock;
    bool tablesReady = false;
    std::map<uint32, Run> runs;                       // by instance id
    std::vector<Loot> loots;                          // items whose fate is still to be seen
    std::vector<RollWon> rollsWon;                    // recent, to tell need and greed from loot taken
    std::map<std::string, uint32> recentPushes;       // receiver, entry, slot: when, to count a broadcast once
};

#define sGuildRuns GuildRuns::instance()
