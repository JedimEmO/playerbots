#pragma once

#include "Common.h"
#include "Entities/ObjectGuid.h"

#include <list>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

class Group;
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
//   guildbot_fight       per fight in a run: how it began, what the party met, deaths, the healer's mana at the start
//   guildbot_fight_member per fight and member: its role, deaths, seconds attacked and attacked off the tank, mana, health
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
    // About once a second, on the world thread: the fight log of each open run's party,
    // and an off_mesh_move event for each of its moves from off the navmesh, or straight with an end off it.
    void SampleFights();
    // The dungeon job's pull step pulled this creature for the leader's party; the next fight began with it.
    void NotePull(Player* leader, Creature* creature);

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

    // A member's part in one fight. Percentages are 255 for no mana.
    struct FightMember
    {
        std::string role;              // tank, healer or dps
        uint32 deaths = 0;
        float attacked = 0.0f;         // seconds anything attacked it
        float loose = 0.0f;            // for a non-tank, seconds it was attacked while the tank lived
        uint32 maxAttackers = 0;
        uint8 manaStart = 255, manaEnd = 255, healthMin = 100;
        bool dead = false;
    };

    // A creature's part in one fight, from the sample it was first seen in.
    struct FightCreature
    {
        uint32 entry = 0;
        uint32 spawn = 0;              // its row in the world database's creature table, 0 for a summon
        uint32 joined = 0;             // seconds after the fight's start
        float x = 0.0f, y = 0.0f, z = 0.0f;
        bool summoned = false;
        uint32 summoner = 0;           // the entry of the creature that summoned it, when the core keeps it
        bool pulled = false;           // the dungeon job's pulled creature
        std::string how;               // pulled, attacked (on a member), was_attacked (a member on it), or both
        uint32 nearMember = 0;         // the living member inside nearest it at its first sight, and how far, in 3D
        float nearDistance = 0.0f;
        int32 fled = -1;               // seconds after the fight's start it was first seen fleeing, -1 for never
        float lastX = 0.0f, lastY = 0.0f, lastZ = 0.0f; // where it was at the last sample it was in the world
    };

    // The fight a run's party is in, from the first member in combat inside until all alive have been out of it
    // FIGHT_QUIET_MS, or all are dead.
    struct Fight
    {
        bool open = false;
        uint32 seq = 0;
        time_t started = 0;
        uint32 startMs = 0;
        uint32 lastMs = 0;             // the sample before
        uint32 quietSinceMs = 0;       // since when no member alive has been in combat, or 0
        uint32 leader = 0;
        float x = 0.0f, y = 0.0f, z = 0.0f;
        std::string began, leaderReason;
        uint32 pulled = 0, pack = 0, maxAttackers = 0, deaths = 0;
        uint8 healerMana = 255;
        ObjectGuid pulledGuid;
        std::map<ObjectGuid, FightCreature> creatures;
        std::map<uint32, FightMember> members;
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
        Fight fight;
        uint32 fightSeq = 0;           // the last fight's number in this run
        // The party's moves looked at by TraceMoves: new splines, straight ones of MOVE_TRACE_MIN_YARDS or more,
        // and moves off the navmesh: those starting where the bot stands off it, and straight ones with an end off it.
        uint32 movesTraced = 0, movesStraight = 0, movesOffMesh = 0;
        // Of the new splines, those of one segment at any length, and the longest of them in yards: a check that the
        // straight test sees moves at all.
        uint32 movesSingle = 0, movesSingleYards = 0;
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
    void SampleFight(Run& run, Group* group, uint32 nowMs, time_t now);
    void EndFight(Run& run, time_t now);
    void TraceMoves(Run& run, Group* group, uint32 nowMs);

    std::mutex lock;
    bool tablesReady = false;
    std::map<uint32, Run> runs;                       // by instance id
    std::vector<Loot> loots;                          // items whose fate is still to be seen
    std::vector<RollWon> rollsWon;                    // recent, to tell need and greed from loot taken
    std::map<std::string, uint32> recentPushes;       // receiver, entry, slot: when, to count a broadcast once
    // Pulls the dungeon job made, by leader guid: the creature, its entry and when. Its own lock: the job notes them
    // under its own.
    struct NotedPull { uint32 entry = 0; ObjectGuid guid; time_t at = 0; };
    std::mutex pullLock;
    std::map<uint32, NotedPull> pulls;
    // For each guild character in a run's party: the last move looked at, and when it last made an off_mesh_move.
    struct MoveTrace { uint32 splineId = 0; uint32 eventMs = 0; bool written = false; };
    std::map<uint32, MoveTrace> moveTraces;           // by guid
};

#define sGuildRuns GuildRuns::instance()
