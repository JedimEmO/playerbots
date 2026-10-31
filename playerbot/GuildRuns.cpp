#include "playerbot/playerbot.h"
#include "playerbot/GuildRuns.h"
#include "playerbot/GuildBotMgr.h"
#include "playerbot/GuildDungeon.h"
#include "Groups/Group.h"
#include "World/World.h"
#include "Grids/GridNotifiers.h"
#include "Grids/GridNotifiersImpl.h"
#include "Grids/CellImpl.h"
#include "MotionGenerators/MoveMap.h"
#include "MotionGenerators/PathFinder.h"
#include "Movement/MoveSpline.h"
#include "playerbot/strategy/values/LastMovementValue.h"

#include <algorithm>
#include <sstream>

namespace
{
    // Counted once per this, however many guild characters in the group see it.
    constexpr uint32 OBSERVE_MS = 4 * IN_MILLISECONDS;
    // A gap longer than this between two checks is not time spent inside, such as across a restart.
    constexpr time_t MAX_STEP_SECONDS = 30;
    // Nobody inside for this long: the run is over.
    constexpr time_t IDLE_CLOSE_SECONDS = 30 * MINUTE;
    // Rows are rewritten at most this often, and at once when a boss dies or the run ends.
    constexpr time_t WRITE_SECONDS = 30;
    // Loot within this of a boss's death is counted as the boss's.
    constexpr time_t BOSS_LOOT_SECONDS = 2 * MINUTE;
    // A need or greed roll is matched to the item push that follows within this.
    constexpr uint32 ROLL_MATCH_MS = 20 * IN_MILLISECONDS;
    // The same push reaches every member of the group; one within this is the same item.
    constexpr uint32 PUSH_DEDUP_MS = 3 * IN_MILLISECONDS;
    // An item kept in the bags is watched this long for being equipped or sold, then left as kept.
    constexpr time_t FATE_WATCH_SECONDS = 3 * HOUR;
    constexpr uint32 FATE_CHECK_MS = MINUTE * IN_MILLISECONDS;
    // A fight is over once no member alive has been in combat for this long.
    constexpr uint32 FIGHT_QUIET_MS = 5 * IN_MILLISECONDS;
    // A fight that starts within this of the dungeon job's pull began with that pull.
    constexpr time_t FIGHT_PULL_SECONDS = 20;
    // At most this much time is counted between two samples, as across a stall of the world.
    constexpr float FIGHT_MAX_STEP = 5.0f;
    // Three or more of the party dead in one fight is a wipe, as the experiments count it.
    constexpr uint32 FIGHT_WIPE_DEATHS = 3;
    // A straight move is traced from this long; shorter ones are steps.
    constexpr float MOVE_TRACE_MIN_YARDS = 5.0f;
    // One off_mesh_move a character at most this often.
    constexpr uint32 MOVE_TRACE_GAP_MS = 10 * IN_MILLISECONDS;
    // The box the core's path finder looks for a poly in, at its widest: half sizes, in yards.
    constexpr float MOVE_TRACE_BOX = 10.0f;

    // Whether the navmesh of this map and instance has a poly the core would path a bot from, within MOVE_TRACE_BOX.
    bool OnNavMesh(dtNavMeshQuery const* query, const G3D::Vector3& point)
    {
        dtQueryFilter filter;
        filter.setIncludeFlags(NAV_GROUND | NAV_WATER);
        filter.setExcludeFlags(NAV_MAGMA_SLIME | NAV_GROUND_STEEP);
        // Detour's points are (y, z, x).
        const float at[3] = { point.y, point.z, point.x };
        const float box[3] = { MOVE_TRACE_BOX, MOVE_TRACE_BOX, MOVE_TRACE_BOX };
        dtPolyRef ref = 0;
        float closest[3] = { 0.0f, 0.0f, 0.0f };
        return dtStatusSucceed(query->findNearestPoly(at, box, &filter, &ref, closest)) && ref;
    }

    // A percentage, 255 for no mana.
    uint8 ManaPercent(Player* player)
    {
        if (player->GetPowerType() != POWER_MANA || !player->GetMaxPower(POWER_MANA))
            return 255;
        return uint8(player->GetPower(POWER_MANA) * 100 / player->GetMaxPower(POWER_MANA));
    }

    uint8 HealthPercent(Player* player)
    {
        return player->GetMaxHealth() ? uint8(player->GetHealth() * 100 / player->GetMaxHealth()) : 0;
    }

    std::string Escape(std::string text)
    {
        CharacterDatabase.escape_string(text);
        return text;
    }

    // Experience between two readings, across a level up.
    uint32 Gained(uint32 lastLevel, uint32 lastXp, uint32 level, uint32 xp)
    {
        if (!lastLevel)
            return 0;
        if (level == lastLevel)
            return xp > lastXp ? xp - lastXp : 0;
        if (level == lastLevel + 1)
            return sObjectMgr.GetXPForLevel(lastLevel) - std::min(lastXp, sObjectMgr.GetXPForLevel(lastLevel)) + xp;
        return 0;
    }

    std::string Outcome(bool lastBossDead, size_t killed)
    {
        return lastBossDead ? "cleared" : killed ? "partial" : "none";
    }
}

void GuildRuns::EnsureTables()
{
    std::lock_guard<std::mutex> guard(lock);
    if (tablesReady)
        return;

    CharacterDatabase.DirectExecute(
        "CREATE TABLE IF NOT EXISTS guildbot_run ("
        " instance INT UNSIGNED NOT NULL,"
        " opened TIMESTAMP NOT NULL DEFAULT '2000-01-01 00:00:00',"
        " map INT UNSIGNED NOT NULL,"
        " dungeon VARCHAR(64) NOT NULL,"
        " leader INT UNSIGNED NOT NULL,"
        " player_led TINYINT UNSIGNED NOT NULL DEFAULT 0,"
        " entered TIMESTAMP NULL DEFAULT NULL,"
        " closed TIMESTAMP NULL DEFAULT NULL,"
        " ended VARCHAR(16) NOT NULL DEFAULT 'open',"       // open, reset, left, lost
        " outcome VARCHAR(16) NOT NULL DEFAULT 'none',"     // cleared, partial, none
        " bosses_killed INT UNSIGNED NOT NULL DEFAULT 0,"
        " bosses INT UNSIGNED NOT NULL DEFAULT 0,"
        " wipes INT UNSIGNED NOT NULL DEFAULT 0,"
        " PRIMARY KEY (instance, opened), KEY opened (opened)"
        ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4");
    // What TraceMoves saw of the party's moves: new splines, straight ones, and those off the navmesh (from off it, or
    // straight with an end off it).
    CharacterDatabase.DirectExecute(
        "ALTER TABLE guildbot_run"
        " ADD COLUMN IF NOT EXISTS moves_traced INT UNSIGNED NOT NULL DEFAULT 0,"
        " ADD COLUMN IF NOT EXISTS moves_straight INT UNSIGNED NOT NULL DEFAULT 0,"
        " ADD COLUMN IF NOT EXISTS moves_off_mesh INT UNSIGNED NOT NULL DEFAULT 0,"
        " ADD COLUMN IF NOT EXISTS moves_single INT UNSIGNED NOT NULL DEFAULT 0,"         // one segment, any length
        " ADD COLUMN IF NOT EXISTS moves_single_yards INT UNSIGNED NOT NULL DEFAULT 0");  // the longest of them
    CharacterDatabase.DirectExecute(
        "CREATE TABLE IF NOT EXISTS guildbot_run_member ("
        " instance INT UNSIGNED NOT NULL,"
        " opened TIMESTAMP NOT NULL DEFAULT '2000-01-01 00:00:00',"
        " guid INT UNSIGNED NOT NULL,"
        " name VARCHAR(12) NOT NULL,"
        " player TINYINT UNSIGNED NOT NULL DEFAULT 0,"
        " level_in TINYINT UNSIGNED NOT NULL DEFAULT 0,"
        " level_out TINYINT UNSIGNED NOT NULL DEFAULT 0,"
        " experience INT UNSIGNED NOT NULL DEFAULT 0,"
        " money INT UNSIGNED NOT NULL DEFAULT 0,"
        " deaths INT UNSIGNED NOT NULL DEFAULT 0,"
        " seconds INT UNSIGNED NOT NULL DEFAULT 0,"
        " PRIMARY KEY (instance, opened, guid)"
        ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4");
    CharacterDatabase.DirectExecute(
        "CREATE TABLE IF NOT EXISTS guildbot_run_boss ("
        " instance INT UNSIGNED NOT NULL,"
        " opened TIMESTAMP NOT NULL DEFAULT '2000-01-01 00:00:00',"
        " entry INT NOT NULL,"
        " killed TIMESTAMP NULL DEFAULT NULL,"               // null: dead before the record saw the run
        " minutes INT UNSIGNED NOT NULL DEFAULT 0,"          // since the run entered, or the boss before
        " PRIMARY KEY (instance, opened, entry)"
        ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4");
    CharacterDatabase.DirectExecute(
        "CREATE TABLE IF NOT EXISTS guildbot_loot ("
        " id INT UNSIGNED NOT NULL AUTO_INCREMENT,"
        " instance INT UNSIGNED NOT NULL,"
        " opened TIMESTAMP NOT NULL DEFAULT '2000-01-01 00:00:00',"
        " at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,"
        " receiver INT UNSIGNED NOT NULL,"
        " item INT UNSIGNED NOT NULL,"
        " quality TINYINT UNSIGNED NOT NULL DEFAULT 0,"
        " count INT UNSIGNED NOT NULL DEFAULT 1,"
        " how VARCHAR(8) NOT NULL,"                           // need, greed, master, looted
        " source INT NOT NULL DEFAULT 0,"                     // the boss that died just before, or 0 for trash
        " item_guid INT UNSIGNED NOT NULL DEFAULT 0,"         // the item itself, 0 when it went onto a stack
        " fate VARCHAR(16) NOT NULL DEFAULT 'kept',"          // kept, equipped, gone
        " fate_at TIMESTAMP NULL DEFAULT NULL,"
        " PRIMARY KEY (id), KEY run (instance, opened), KEY item_guid (item_guid)"
        ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4");
    CharacterDatabase.DirectExecute(
        "CREATE TABLE IF NOT EXISTS guildbot_fight ("
        " instance INT UNSIGNED NOT NULL,"
        " opened TIMESTAMP NOT NULL DEFAULT '2000-01-01 00:00:00',"
        " seq INT UNSIGNED NOT NULL,"
        " started TIMESTAMP NOT NULL DEFAULT '2000-01-01 00:00:00',"
        " ended TIMESTAMP NULL DEFAULT NULL,"
        " leader INT UNSIGNED NOT NULL DEFAULT 0,"
        " x FLOAT NOT NULL DEFAULT 0, y FLOAT NOT NULL DEFAULT 0, z FLOAT NOT NULL DEFAULT 0,"   // the leader at the start
        " began VARCHAR(16) NOT NULL DEFAULT '',"             // pull, walked_in, attacked, member
        " leader_reason VARCHAR(255) NOT NULL DEFAULT '',"    // the job's reason at the start
        " pulled INT NOT NULL DEFAULT 0,"                     // the creature entry pulled, or 0
        " pack INT UNSIGNED NOT NULL DEFAULT 0,"              // hostiles within call-for-help reach of the first, out of combat
        " creatures INT UNSIGNED NOT NULL DEFAULT 0,"         // distinct hostiles in combat with the party
        " max_attackers INT UNSIGNED NOT NULL DEFAULT 0,"     // the most at any one sample
        " deaths INT UNSIGNED NOT NULL DEFAULT 0,"
        " wipe TINYINT UNSIGNED NOT NULL DEFAULT 0,"          // 3 or more members died
        " healer_mana TINYINT UNSIGNED NOT NULL DEFAULT 255," // the healer's mana % at the start, 255 with no healer
        " PRIMARY KEY (instance, opened, seq)"
        ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4");
    CharacterDatabase.DirectExecute(
        "CREATE TABLE IF NOT EXISTS guildbot_fight_creature ("
        " instance INT UNSIGNED NOT NULL,"
        " opened TIMESTAMP NOT NULL DEFAULT '2000-01-01 00:00:00',"
        " seq INT UNSIGNED NOT NULL,"
        " guid INT UNSIGNED NOT NULL,"                        // the creature's guid in the instance
        " spawn INT UNSIGNED NOT NULL DEFAULT 0,"             // its row in the world database, 0 for a summon
        " entry INT UNSIGNED NOT NULL DEFAULT 0,"
        " joined INT UNSIGNED NOT NULL DEFAULT 0,"            // seconds after the fight's start it was first seen
        " x FLOAT NOT NULL DEFAULT 0, y FLOAT NOT NULL DEFAULT 0, z FLOAT NOT NULL DEFAULT 0,"   // where, then
        " summoned TINYINT UNSIGNED NOT NULL DEFAULT 0,"
        " summoner INT UNSIGNED NOT NULL DEFAULT 0,"          // the summoning creature's entry, or 0
        " pulled TINYINT UNSIGNED NOT NULL DEFAULT 0,"        // the dungeon job's pulled creature
        " how VARCHAR(16) NOT NULL DEFAULT '',"               // pulled, attacked, was_attacked, both
        " PRIMARY KEY (instance, opened, seq, guid)"
        ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4");
    CharacterDatabase.DirectExecute(
        "CREATE TABLE IF NOT EXISTS guildbot_fight_member ("
        " instance INT UNSIGNED NOT NULL,"
        " opened TIMESTAMP NOT NULL DEFAULT '2000-01-01 00:00:00',"
        " seq INT UNSIGNED NOT NULL,"
        " guid INT UNSIGNED NOT NULL,"
        " role VARCHAR(8) NOT NULL DEFAULT '',"               // tank, healer, dps
        " deaths INT UNSIGNED NOT NULL DEFAULT 0,"
        " attacked_seconds INT UNSIGNED NOT NULL DEFAULT 0,"  // seconds anything attacked it
        " loose_seconds INT UNSIGNED NOT NULL DEFAULT 0,"     // for a non-tank, seconds it was attacked while the tank lived
        " max_attackers INT UNSIGNED NOT NULL DEFAULT 0,"
        " mana_start TINYINT UNSIGNED NOT NULL DEFAULT 255,"  // % values, 255 for no mana
        " mana_end TINYINT UNSIGNED NOT NULL DEFAULT 255,"
        " health_min TINYINT UNSIGNED NOT NULL DEFAULT 100,"
        " PRIMARY KEY (instance, opened, seq, guid)"
        ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4");

    tablesReady = true;
}

GuildRuns::Run* GuildRuns::OpenRun(Player* bot)
{
    GuildDungeon::Bosses bosses;
    if (!GuildDungeon::BossesOf(bot->GetMapId(), bot, bosses))
        return nullptr;

    Run& run = runs[bot->GetInstanceId()];
    run.instanceId = bot->GetInstanceId();
    run.mapId = bot->GetMapId();
    run.dungeon = bosses.name;
    run.bosses = bosses.entries;
    run.spawns = bosses.spawns;
    run.lastBoss = bosses.last;
    run.opened = time(nullptr);
    run.lastInside = run.opened;

    // The server gives a deleted instance's id to a new one, so an open run under this id of another map, or older
    // than RESUME_HOURS, is another instance's that was never closed: it ends as lost. On the shared server the lab's
    // restores delete its instances, and a Deadmines run left open by a play account two days before took in the
    // lab's Deadmines tank.
    constexpr uint32 RESUME_HOURS = 6;
    CharacterDatabase.PExecute(
        "UPDATE guildbot_run SET closed = NOW(), ended = 'lost' WHERE instance = %u AND ended = 'open' "
        "AND (map <> %u OR opened < NOW() - INTERVAL %u HOUR)", run.instanceId, run.mapId, RESUME_HOURS);

    // A run still open from before a restart goes on: its members' counts and its dead bosses come back.
    auto open = CharacterDatabase.PQuery(
        "SELECT UNIX_TIMESTAMP(opened), COALESCE(UNIX_TIMESTAMP(entered), 0), wipes, moves_traced, moves_straight, moves_off_mesh, "
        "moves_single, moves_single_yards "
        "FROM guildbot_run "
        "WHERE instance = %u AND ended = 'open' AND map = %u AND opened >= NOW() - INTERVAL %u HOUR "
        "ORDER BY opened DESC LIMIT 1", run.instanceId, run.mapId, RESUME_HOURS);
    if (open)
    {
        Field* f = open->Fetch();
        run.opened = time_t(f[0].GetUInt64());
        run.entered = time_t(f[1].GetUInt64());
        run.wipes = f[2].GetUInt32();
        run.movesTraced = f[3].GetUInt32();
        run.movesStraight = f[4].GetUInt32();
        run.movesOffMesh = f[5].GetUInt32();
        run.movesSingle = f[6].GetUInt32();
        run.movesSingleYards = f[7].GetUInt32();
        auto members = CharacterDatabase.PQuery(
            "SELECT guid, name, player, level_in, level_out, experience, money, deaths, seconds FROM guildbot_run_member "
            "WHERE instance = %u AND opened = FROM_UNIXTIME(%u)", run.instanceId, uint32(run.opened));
        if (members)
        {
            do
            {
                Field* m = members->Fetch();
                Member& member = run.members[m[0].GetUInt32()];
                member.name = m[1].GetCppString();
                member.player = m[2].GetUInt32() != 0;
                member.levelIn = m[3].GetUInt32();
                member.levelOut = m[4].GetUInt32();
                member.experience = m[5].GetUInt32();
                member.money = m[6].GetUInt32();
                member.deaths = m[7].GetUInt32();
                member.seconds = m[8].GetUInt32();
            } while (members->NextRow());
        }
        auto killed = CharacterDatabase.PQuery(
            "SELECT entry, COALESCE(UNIX_TIMESTAMP(killed), 0) FROM guildbot_run_boss WHERE instance = %u AND opened = FROM_UNIXTIME(%u)",
            run.instanceId, uint32(run.opened));
        if (killed)
        {
            do
            {
                Field* k = killed->Fetch();
                run.killed.insert(k[0].GetInt32());
                run.lastKill = std::max(run.lastKill, time_t(k[1].GetUInt64()));
            } while (killed->NextRow());
        }
        run.bossesSeen = true;
        // Fights go on numbering from the last one written.
        auto fights = CharacterDatabase.PQuery("SELECT COALESCE(MAX(seq), 0) FROM guildbot_fight WHERE instance = %u AND opened = FROM_UNIXTIME(%u)",
            run.instanceId, uint32(run.opened));
        if (fights)
            run.fightSeq = fights->Fetch()[0].GetUInt32();
    }
    run.dirty = true;
    return &run;
}

void GuildRuns::Observe(Player* bot)
{
    if (!bot || !bot->IsInWorld() || !bot->GetGroup())
        return;
    MapEntry const* map = sMapStore.LookupEntry(bot->GetMapId());
    if (!map || !map->IsNonRaidDungeon())
        return;

    std::lock_guard<std::mutex> guard(lock);
    if (!tablesReady)
        return;

    auto it = runs.find(bot->GetInstanceId());
    Run* run = it != runs.end() ? &it->second : OpenRun(bot);
    if (!run)
        return;

    const uint32 nowMs = WorldTimer::getMSTime();
    if (run->lastObservedMs && WorldTimer::getMSTimeDiff(run->lastObservedMs, nowMs) < OBSERVE_MS)
        return;
    run->lastObservedMs = nowMs;
    ObserveRun(*run, bot);
}

void GuildRuns::ObserveRun(Run& run, Player* bot)
{
    Group* group = bot->GetGroup();
    Map* map = bot->GetMap();
    const time_t now = time(nullptr);

    // The leader, and whether a player leads: the player's own character, or one the player has taken over.
    Player* leader = sObjectMgr.GetPlayer(group->GetLeaderGuid());
    const uint32 leaderGuid = group->GetLeaderGuid().GetCounter();
    const bool playerLed = leader && !sGuildBotMgr.IsGuildBot(leader);
    if (leaderGuid != run.leader || playerLed != run.playerLed)
    {
        run.leader = leaderGuid;
        run.playerLed = playerLed;
        run.dirty = true;
    }

    uint32 inside = 0, alive = 0, known = 0;
    for (auto& slot : group->GetMemberSlots())
    {
        Player* member = sObjectMgr.GetPlayer(slot.guid);
        if (!member || !member->IsInWorld())
            continue;
        const uint32 guid = slot.guid.GetCounter();
        const bool in = member->GetMapId() == run.mapId && member->GetInstanceId() == run.instanceId;
        auto found = run.members.find(guid);
        // Members count from when they are first inside; a known one is followed outside too, as a ghost or on the way back.
        if (!in && found == run.members.end())
            continue;

        Member& m = run.members[guid];
        known++;
        const uint32 level = member->GetLevel(), xp = member->GetUInt32Value(PLAYER_XP), money = member->GetMoney();
        if (!m.levelIn)
        {
            m.name = member->GetName();
            m.player = !sGuildBotMgr.IsGuildBot(member);
            m.levelIn = level;
        }
        else
        {
            m.experience += Gained(m.lastLevel, m.lastXp, level, xp);
            // Coin looted; what is spent, at a repairer, is not taken off.
            if (m.lastLevel && money > m.lastMoney)
                m.money += money - m.lastMoney;
            if (in && m.seenAt)
                m.seconds += uint32(std::min<time_t>(MAX_STEP_SECONDS, std::max<time_t>(0, now - m.seenAt)));
        }
        m.lastLevel = level;
        m.lastXp = xp;
        m.lastMoney = money;
        m.levelOut = level;
        m.seenAt = in ? now : 0;

        const bool dead = !member->IsAlive();
        if (dead && !m.dead)
            m.deaths++;
        m.dead = dead;
        if (!dead)
            alive++;
        if (in)
            inside++;
        m.dirty = true;
    }

    if (inside)
        run.lastInside = now;
    if (!run.entered && inside >= group->GetMembersCount())
    {
        run.entered = now;
        run.dirty = true;
    }

    // A wipe: every member the run knows dead at once. Counted once, until someone is alive again.
    if (known && !alive && !run.wiped)
    {
        run.wipes++;
        run.wiped = true;
        run.dirty = true;
    }
    else if (alive)
        run.wiped = false;

    // Bosses, as the dungeon job reads them; dead before the record first looked, they have no time.
    bool killedNow = false;
    for (size_t i = 0; i < run.bosses.size(); ++i)
    {
        const int32 entry = run.bosses[i];
        if (run.killed.count(entry) || !GuildDungeon::BossDead(map, run.spawns[i]))
            continue;
        run.killed.insert(entry);
        if (run.bossesSeen)
        {
            const time_t since = std::max(run.lastKill, run.entered ? run.entered : run.opened);
            CharacterDatabase.PExecute(
                "REPLACE INTO guildbot_run_boss (instance, opened, entry, killed, minutes) VALUES (%u, FROM_UNIXTIME(%u), %d, FROM_UNIXTIME(%u), %u)",
                run.instanceId, uint32(run.opened), entry, uint32(now), uint32((now - since) / MINUTE));
            run.lastKill = now;
        }
        else
            CharacterDatabase.PExecute(
                "REPLACE INTO guildbot_run_boss (instance, opened, entry, killed, minutes) VALUES (%u, FROM_UNIXTIME(%u), %d, NULL, 0)",
                run.instanceId, uint32(run.opened), entry);
        killedNow = true;
    }
    run.bossesSeen = true;

    if (killedNow || run.dirty)
        WriteRun(run);
}

void GuildRuns::WriteRun(Run& run)
{
    uint32 present = 0;
    for (size_t i = 0; i < run.bosses.size(); ++i)
        if (run.killed.count(run.bosses[i]))
            present++;

    std::string entered = run.entered ? "FROM_UNIXTIME(" + std::to_string(uint32(run.entered)) + ")" : "NULL";
    CharacterDatabase.PExecute(
        "INSERT INTO guildbot_run (instance, opened, map, dungeon, leader, player_led, entered, outcome, bosses_killed, bosses, wipes, "
        "moves_traced, moves_straight, moves_off_mesh, moves_single, moves_single_yards) "
        "VALUES (%u, FROM_UNIXTIME(%u), %u, '%s', %u, %u, %s, '%s', %u, %u, %u, %u, %u, %u, %u, %u) "
        "ON DUPLICATE KEY UPDATE leader = VALUES(leader), player_led = VALUES(player_led), entered = VALUES(entered), "
        "outcome = VALUES(outcome), bosses_killed = VALUES(bosses_killed), bosses = VALUES(bosses), wipes = VALUES(wipes), "
        "moves_traced = VALUES(moves_traced), moves_straight = VALUES(moves_straight), moves_off_mesh = VALUES(moves_off_mesh), "
        "moves_single = VALUES(moves_single), moves_single_yards = VALUES(moves_single_yards)",
        run.instanceId, uint32(run.opened), run.mapId, Escape(run.dungeon).c_str(), run.leader, run.playerLed ? 1 : 0,
        entered.c_str(), Outcome(run.killed.count(run.lastBoss) > 0, run.killed.size()).c_str(),
        uint32(run.killed.size()), uint32(run.bosses.size()), run.wipes, run.movesTraced, run.movesStraight, run.movesOffMesh,
        run.movesSingle, run.movesSingleYards);

    for (auto& [guid, m] : run.members)
    {
        if (!m.dirty)
            continue;
        CharacterDatabase.PExecute(
            "REPLACE INTO guildbot_run_member (instance, opened, guid, name, player, level_in, level_out, experience, money, deaths, seconds) "
            "VALUES (%u, FROM_UNIXTIME(%u), %u, '%s', %u, %u, %u, %u, %u, %u, %u)",
            run.instanceId, uint32(run.opened), guid, Escape(m.name).c_str(), m.player ? 1 : 0,
            m.levelIn, m.levelOut, m.experience, m.money, m.deaths, m.seconds);
        m.dirty = false;
    }
    run.dirty = false;
}

void GuildRuns::CloseRun(Run& run, const std::string& ended)
{
    if (run.fight.open)
        EndFight(run, time(nullptr));
    WriteRun(run);
    CharacterDatabase.PExecute(
        "UPDATE guildbot_run SET closed = NOW(), ended = '%s' WHERE instance = %u AND opened = FROM_UNIXTIME(%u)",
        ended.c_str(), run.instanceId, uint32(run.opened));
}

void GuildRuns::InstanceReset(uint32 instanceId)
{
    std::lock_guard<std::mutex> guard(lock);
    auto it = runs.find(instanceId);
    if (it == runs.end())
        return;
    CloseRun(it->second, "reset");
    runs.erase(it);
}

void GuildRuns::Sweep()
{
    std::lock_guard<std::mutex> guard(lock);
    if (!tablesReady)
        return;

    const time_t now = time(nullptr);
    for (auto it = runs.begin(); it != runs.end();)
    {
        Run& run = it->second;
        if (now - run.lastInside >= IDLE_CLOSE_SECONDS)
        {
            CloseRun(run, "left");
            it = runs.erase(it);
            continue;
        }
        // Counts written every so often while nothing else changes.
        bool dirty = run.dirty;
        for (auto& [guid, m] : run.members)
            dirty = dirty || m.dirty;
        if (dirty && now - run.lastWrite >= WRITE_SECONDS)
        {
            WriteRun(run);
            run.lastWrite = now;
        }
        ++it;
    }

    static uint32 lastFates = 0;
    const uint32 nowMs = WorldTimer::getMSTime();
    if (WorldTimer::getMSTimeDiff(lastFates, nowMs) >= FATE_CHECK_MS)
    {
        lastFates = nowMs;
        CheckFates();
    }
}

void GuildRuns::NotePull(Player* leader, Creature* creature)
{
    if (!leader || !creature)
        return;
    std::lock_guard<std::mutex> guard(pullLock);
    pulls[leader->GetGUIDLow()] = { creature->GetEntry(), creature->GetObjectGuid(), time(nullptr) };
}

void GuildRuns::SampleFights()
{
    std::lock_guard<std::mutex> guard(lock);
    if (!tablesReady)
        return;
    const uint32 nowMs = WorldTimer::getMSTime();
    const time_t now = time(nullptr);
    for (auto& [instanceId, run] : runs)
    {
        // The group, through any member the run knows that is in the world.
        Group* group = nullptr;
        if (Player* leader = sObjectMgr.GetPlayer(ObjectGuid(HIGHGUID_PLAYER, run.leader)))
            group = leader->GetGroup();
        for (auto it = run.members.begin(); !group && it != run.members.end(); ++it)
            if (Player* member = sObjectMgr.GetPlayer(ObjectGuid(HIGHGUID_PLAYER, it->first)))
                group = member->GetGroup();
        if (group)
        {
            TraceMoves(run, group, nowMs);
            SampleFight(run, group, nowMs, now);
        }
        else if (run.fight.open)
            EndFight(run, now);
    }
}

void GuildRuns::SampleFight(Run& run, Group* group, uint32 nowMs, time_t now)
{
    Fight& fight = run.fight;
    auto inside = [&](Player* member)
    {
        return member->IsInWorld() && member->GetMapId() == run.mapId && member->GetInstanceId() == run.instanceId;
    };

    // The party in the instance, and whether any of it alive is in combat.
    std::vector<Player*> party;
    bool fighting = false;
    for (auto& slot : group->GetMemberSlots())
        if (Player* member = sObjectMgr.GetPlayer(slot.guid))
        {
            // A member already in the fight stays in it, as a ghost outside too, so its death is counted.
            if (!inside(member) && !(fight.open && fight.members.count(member->GetGUIDLow())))
                continue;
            party.push_back(member);
            if (inside(member) && member->IsAlive() && member->IsInCombat())
                fighting = true;
        }

    if (!fight.open)
    {
        if (!fighting)
            return;
        fight = Fight();
        fight.open = true;
        fight.seq = ++run.fightSeq;
        fight.started = now;
        fight.startMs = nowMs;
        fight.lastMs = nowMs;
        fight.leader = group->GetLeaderGuid().GetCounter();

        Player* leader = sObjectMgr.GetPlayer(group->GetLeaderGuid());
        const bool leaderInside = leader && inside(leader);
        if (leaderInside)
        {
            fight.x = leader->GetPositionX();
            fight.y = leader->GetPositionY();
            fight.z = leader->GetPositionZ();
        }
        fight.leaderReason = sGuildBotMgr.GetReason(fight.leader).substr(0, 255);

        // How it began: the job's pull, the leader walking into it on its way, the party set on where it stood,
        // or a member before the leader. A pull begins one fight only.
        {
            std::lock_guard<std::mutex> pullGuard(pullLock);
            auto pull = pulls.find(fight.leader);
            if (pull != pulls.end())
            {
                if (now - pull->second.at < FIGHT_PULL_SECONDS)
                {
                    fight.pulled = pull->second.entry;
                    fight.pulledGuid = pull->second.guid;
                }
                pulls.erase(pull);
            }
        }
        const bool leaderFirst = leaderInside && leader->IsAlive() && leader->IsInCombat();
        const bool onTheWay = fight.leaderReason.rfind("on the way to", 0) == 0
            || (fight.leaderReason.empty() && leaderInside && leader->IsMoving());
        fight.began = fight.pulled ? "pull" : !leaderFirst ? "member" : onTheWay ? "walked_in" : "attacked";

        // The first creature: one attacking the first member in combat, the leader when it is, or that member's victim.
        Player* first = leaderFirst ? leader : nullptr;
        for (Player* member : party)
            if (!first && inside(member) && member->IsAlive() && member->IsInCombat())
                first = member;
        Unit* creature = nullptr;
        if (first)
        {
            for (Unit* attacker : first->getAttackers())
                if (attacker->GetTypeId() == TYPEID_UNIT && attacker->IsAlive())
                {
                    creature = attacker;
                    break;
                }
            if (!creature && first->GetVictim() && first->GetVictim()->GetTypeId() == TYPEID_UNIT)
                creature = first->GetVictim();
        }
        // Its pack: the creatures its call for help would bring, as the core reaches them, still out of combat.
        if (creature && creature->IsInWorld())
        {
            Creature* caller = static_cast<Creature*>(creature);
            float radius = sWorld.getConfig(CONFIG_FLOAT_CREATURE_FAMILY_ASSISTANCE_RADIUS);
            if (caller->GetCreatureInfo() && caller->GetCreatureInfo()->CallForHelp > 0)
                radius = caller->GetCreatureInfo()->CallForHelp;
            CreatureList near;
            MaNGOS::AnyAssistCreatureInRangeCheck check(caller, first, radius);
            MaNGOS::CreatureListSearcher<MaNGOS::AnyAssistCreatureInRangeCheck> searcher(near, check);
            Cell::VisitAllObjects(caller, searcher, radius);
            for (Creature* other : near)
                if (other != caller && other->IsAlive() && !other->IsInCombat())
                    fight.pack++;
        }
        for (Player* member : party)
            if (inside(member) && member->IsAlive() && PlayerbotAI::IsHeal(member))
            {
                fight.healerMana = ManaPercent(member);
                break;
            }
    }

    float step = fight.lastMs ? WorldTimer::getMSTimeDiff(fight.lastMs, nowMs) / 1000.0f : 1.0f;
    step = std::min(step, FIGHT_MAX_STEP);
    fight.lastMs = nowMs;

    Player* tank = nullptr;
    for (Player* member : party)
        if (!tank && PlayerbotAI::IsTank(member))
            tank = member;
    // A creature on a member other than the tank, while the tank lives inside, is one the tank does not hold.
    const bool tankUp = tank && tank->IsAlive() && inside(tank);

    std::set<Unit*> met;
    // The creatures of this sample, by what links them to the party: bit 1 it attacks a member, bit 2 a member attacks it.
    std::map<Unit*, uint8> links;
    bool alive = false;
    for (Player* member : party)
    {
        FightMember& m = fight.members[member->GetGUIDLow()];
        if (m.role.empty())
        {
            m.role = member == tank ? "tank" : PlayerbotAI::IsHeal(member) ? "healer" : "dps";
            m.manaStart = ManaPercent(member);
            // Dead before the fight began: not a death of this fight.
            m.dead = !member->IsAlive();
        }
        if (!member->IsAlive())
        {
            if (!m.dead)
            {
                m.deaths++;
                fight.deaths++;
                m.dead = true;
            }
            continue;
        }
        m.dead = false;
        if (!inside(member))
            continue;
        alive = true;
        m.healthMin = std::min(m.healthMin, HealthPercent(member));
        m.manaEnd = ManaPercent(member);

        // getAttackers holds the units whose victim is this member.
        uint32 attackers = 0;
        for (Unit* attacker : member->getAttackers())
        {
            if (!attacker->IsAlive())
                continue;
            attackers++;
            met.insert(attacker);
            links[attacker] |= 1;
        }
        if (Unit* victim = member->GetVictim())
            if (victim->GetTypeId() == TYPEID_UNIT && victim->IsAlive())
            {
                met.insert(victim);
                links[victim] |= 2;
            }
        if (attackers)
            m.attacked += step;
        if (attackers && tankUp && member != tank)
            m.loose += step;
        m.maxAttackers = std::max(m.maxAttackers, attackers);
    }
    fight.maxAttackers = std::max(fight.maxAttackers, uint32(met.size()));

    // Each creature once, as it was when first seen. One that joins and dies between two samples is not seen.
    for (auto& [unit, link] : links)
    {
        if (unit->GetTypeId() != TYPEID_UNIT || fight.creatures.count(unit->GetObjectGuid()))
            continue;
        Creature* creature = static_cast<Creature*>(unit);
        FightCreature& c = fight.creatures[creature->GetObjectGuid()];
        c.entry = creature->GetEntry();
        // A summon is a temporary summon, or a guardian pet a creature casts, as the Goblin Engineer's golem.
        // Its db guid is its own low guid, not a world database row.
        c.summoned = creature->IsTemporarySummon() || creature->IsPet();
        c.spawn = c.summoned ? 0 : creature->GetDbGuid();
        c.joined = WorldTimer::getMSTimeDiff(fight.startMs, nowMs) / IN_MILLISECONDS;
        c.x = creature->GetPositionX();
        c.y = creature->GetPositionY();
        c.z = creature->GetPositionZ();
        // A creature's guid carries its entry; a player's gives 0.
        if (creature->IsTemporarySummon())
            c.summoner = creature->GetSpawnerGuid().GetEntry();
        else if (creature->IsPet())
            c.summoner = creature->GetOwnerGuid().GetEntry();
        c.pulled = !fight.pulledGuid.IsEmpty() && creature->GetObjectGuid() == fight.pulledGuid;
        c.how = c.pulled ? "pulled" : link == 3 ? "both" : link == 1 ? "attacked" : "was_attacked";
    }

    // Over once nobody alive inside has been in combat for FIGHT_QUIET_MS, or nobody is alive inside.
    if (!alive)
    {
        EndFight(run, now);
        return;
    }
    if (fighting)
        fight.quietSinceMs = 0;
    else if (!fight.quietSinceMs)
        fight.quietSinceMs = nowMs;
    else if (WorldTimer::getMSTimeDiff(fight.quietSinceMs, nowMs) >= FIGHT_QUIET_MS)
        EndFight(run, now);
}

void GuildRuns::TraceMoves(Run& run, Group* group, uint32 nowMs)
{
    for (auto& slot : group->GetMemberSlots())
    {
        Player* bot = sObjectMgr.GetPlayer(slot.guid);
        // On a transport, a spline is in the transport's own coordinates.
        if (!bot || !bot->IsInWorld() || bot->IsBeingTeleported() || bot->GetTransport() || !bot->GetPlayerbotAI()
            || !sGuildBotMgr.IsGuildBot(bot))
            continue;
        const Movement::MoveSpline* spline = bot->movespline;
        if (!spline || !spline->Initialized() || spline->Finalized() || spline->isCyclic())
            continue;
        MoveTrace& trace = moveTraces[bot->GetGUIDLow()];
        if (trace.splineId == spline->GetId())
            continue;
        trace.splineId = spline->GetId();
        run.movesTraced++;

        // The same query the core's path finder uses for this bot.
        dtNavMeshQuery const* query = MMAP::MMapFactory::createOrGetMMapManager()->GetNavMeshQuery(bot->GetMapId(), bot->GetInstanceId());
        if (!query)
            continue;
        const G3D::Vector3 to = spline->FinalDestination();
        const G3D::Vector3 at(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ());
        // A move from off the navmesh, of any number of points: the core paths from where the bot stands even when
        // the nearest poly is several yards away.
        const bool startOff = !OnNavMesh(query, at);

        // A straight move: one segment, the core's shortcut when it finds no poly at an end or the bot stays in one
        // poly. A spline keeps a point before its path, so its first segment's index is not 0; by times instead, it
        // is on its last segment (the current destination is the final one) and that segment starts at time 0.
        // On it, the bot is the elapsed share of the way from the start, so the start is found back from where it stands.
        G3D::Vector3 from = at;
        bool fromOn = !startOff, toOn = true;
        const int32 raw = spline->GetRawPathIndex();
        const int32 passed = int32(spline->Duration()) - spline->ComputeTimeToIndex(raw + 1);
        if ((spline->CurrentDestination() - to).squaredLength() <= 0.01f && spline->ComputeTimeToIndex(raw) + passed == 0)
        {
            const float done = spline->Duration() ? float(std::max(passed, 0)) / spline->Duration() : 0.0f;
            if (done < 0.9f)
                from = (at - to * done) / (1.0f - done);
            const float yards = (to - from).length();
            run.movesSingle++;
            run.movesSingleYards = std::max(run.movesSingleYards, uint32(yards + 0.5f));
            if (yards >= MOVE_TRACE_MIN_YARDS)
            {
                run.movesStraight++;
                fromOn = OnNavMesh(query, from);
                toOn = OnNavMesh(query, to);
            }
        }
        if (!startOff && fromOn && toOn)
            continue;
        run.movesOffMesh++;
        if (trace.written && WorldTimer::getMSTimeDiff(trace.eventMs, nowMs) < MOVE_TRACE_GAP_MS)
            continue;
        const float yards = (to - from).length();

        // By playerbots' move when this move ends on a point of the path it last made: it moves to the last point
        // it keeps of that path, which can be short of the path's end. Else by a move of ours or the core's.
        PlayerbotAI* ai = bot->GetPlayerbotAI();
        ai::TravelPath& lastPath = ai->GetAiObjectContext()->GetValue<ai::LastMovement&>("last movement")->Get().lastPath;
        const std::vector<WorldPosition> points = lastPath.getPointPath();
        bool byPath = false;
        for (const WorldPosition& point : points)
            if (point.getMapId() == bot->GetMapId()
                && (G3D::Vector3(point.getX(), point.getY(), point.getZ()) - to).squaredLength() < MOVE_TRACE_MIN_YARDS * MOVE_TRACE_MIN_YARDS)
            {
                byPath = true;
                break;
            }
        std::ostringstream detail;
        detail.setf(std::ios::fixed);
        detail.precision(1);
        detail << "action=" << ai->GetLastExecutedActionName(BotState::BOT_STATE_NON_COMBAT)
               << " combat_action=" << ai->GetLastExecutedActionName(BotState::BOT_STATE_COMBAT)
               << " by=" << (byPath ? "path" : "direct") << " path=" << points.size()
               << " map=" << bot->GetMapId() << " from=" << from.x << "," << from.y << "," << from.z
               << " to=" << to.x << "," << to.y << "," << to.z << " yd=" << uint32(yards + 0.5f)
               << " from_mesh=" << (fromOn ? 1 : 0) << " to_mesh=" << (toOn ? 1 : 0)
               << " combat=" << (bot->IsInCombat() ? 1 : 0) << (startOff ? " start_off=1" : "") << (bot->IsAlive() ? "" : " ghost=1");
        sGuildBotMgr.RecordEvent(bot->GetGUIDLow(), "off_mesh_move", detail.str());
        trace.written = true;
        trace.eventMs = nowMs;
    }
}

void GuildRuns::EndFight(Run& run, time_t now)
{
    Fight& fight = run.fight;
    if (!fight.open)
        return;
    fight.open = false;
    CharacterDatabase.PExecute(
        "REPLACE INTO guildbot_fight (instance, opened, seq, started, ended, leader, x, y, z, began, leader_reason, pulled, pack, "
        "creatures, max_attackers, deaths, wipe, healer_mana) VALUES (%u, FROM_UNIXTIME(%u), %u, FROM_UNIXTIME(%u), FROM_UNIXTIME(%u), "
        "%u, %f, %f, %f, '%s', '%s', %u, %u, %u, %u, %u, %u, %u)",
        run.instanceId, uint32(run.opened), fight.seq, uint32(fight.started), uint32(now), fight.leader, fight.x, fight.y, fight.z,
        fight.began.c_str(), Escape(fight.leaderReason).c_str(), fight.pulled, fight.pack, uint32(fight.creatures.size()),
        fight.maxAttackers, fight.deaths, fight.deaths >= FIGHT_WIPE_DEATHS ? 1 : 0, uint32(fight.healerMana));
    for (auto& [guid, m] : fight.members)
        CharacterDatabase.PExecute(
            "REPLACE INTO guildbot_fight_member (instance, opened, seq, guid, role, deaths, attacked_seconds, loose_seconds, "
            "max_attackers, mana_start, mana_end, health_min) VALUES (%u, FROM_UNIXTIME(%u), %u, %u, '%s', %u, %u, %u, %u, %u, %u, %u)",
            run.instanceId, uint32(run.opened), fight.seq, guid, m.role.c_str(), m.deaths, uint32(m.attacked + 0.5f),
            uint32(m.loose + 0.5f), m.maxAttackers, uint32(m.manaStart), uint32(m.manaEnd), uint32(m.healthMin));
    // The creatures, a few rows to a statement.
    std::ostringstream rows;
    uint32 count = 0;
    auto flush = [&]()
    {
        if (!count)
            return;
        CharacterDatabase.Execute(("REPLACE INTO guildbot_fight_creature (instance, opened, seq, guid, spawn, entry, joined, x, y, z, "
            "summoned, summoner, pulled, how) VALUES " + rows.str()).c_str());
        rows.str("");
        count = 0;
    };
    for (auto& [guid, c] : fight.creatures)
    {
        char row[256];
        snprintf(row, sizeof(row), "%s(%u, FROM_UNIXTIME(%u), %u, %u, %u, %u, %u, %.2f, %.2f, %.2f, %u, %u, %u, '%s')",
            count ? ", " : "", run.instanceId, uint32(run.opened), fight.seq, guid.GetCounter(), c.spawn, c.entry, c.joined,
            c.x, c.y, c.z, c.summoned ? 1 : 0, c.summoner, c.pulled ? 1 : 0, c.how.c_str());
        rows << row;
        if (++count == 50)
            flush();
    }
    flush();
}

void GuildRuns::CheckFates()
{
    const time_t now = time(nullptr);
    for (auto it = loots.begin(); it != loots.end();)
    {
        Loot& loot = *it;
        Player* receiver = sObjectMgr.GetPlayer(ObjectGuid(HIGHGUID_PLAYER, loot.receiver), false);
        std::string fate = loot.fate;
        // The receiver offline: its items cannot be looked at, so the fate waits.
        if (receiver && receiver->IsInWorld())
        {
            Item* item = receiver->GetItemByGuid(loot.item);
            fate = !item ? "gone" : item->IsEquipped() ? "equipped" : "kept";
        }
        if (fate != loot.fate)
        {
            loot.fate = fate;
            CharacterDatabase.PExecute("UPDATE guildbot_loot SET fate = '%s', fate_at = NOW() WHERE item_guid = %u AND receiver = %u",
                fate.c_str(), loot.item.GetCounter(), loot.receiver);
        }
        if (fate != "kept" || now - loot.at > FATE_WATCH_SECONDS)
            it = loots.erase(it);
        else
            ++it;
    }
}

void GuildRuns::OnPacket(Player* bot, const WorldPacket& packet)
{
    if (packet.GetOpcode() != SMSG_LOOT_ROLL_WON && packet.GetOpcode() != SMSG_ITEM_PUSH_RESULT)
        return;

    std::lock_guard<std::mutex> guard(lock);
    if (!tablesReady || runs.empty())
        return;

    WorldPacket p(packet);
    p.rpos(0);
    const uint32 nowMs = WorldTimer::getMSTime();

    if (packet.GetOpcode() == SMSG_LOOT_ROLL_WON)
    {
        ObjectGuid lootGuid, winner;
        uint32 slot, entry, suffix, property;
        uint8 number, type;
        p >> lootGuid >> slot >> entry >> suffix >> property >> winner >> number >> type;
        rollsWon.erase(std::remove_if(rollsWon.begin(), rollsWon.end(),
            [&](const RollWon& r) { return WorldTimer::getMSTimeDiff(r.ms, nowMs) > ROLL_MATCH_MS; }), rollsWon.end());
        rollsWon.push_back({ winner.GetCounter(), entry, type, nowMs });
        return;
    }

    ObjectGuid receiverGuid;
    uint32 received, created, showInChat, slot, entry, suffix, count;
    int32 property;
    uint8 bag;
    p >> receiverGuid >> received >> created >> showInChat >> bag >> slot >> entry >> suffix >> property >> count;
    // Looted only: not bought, made or handed over by a quest.
    if (received || created)
        return;

    Player* receiver = sObjectMgr.GetPlayer(receiverGuid);
    if (!receiver)
        return;
    auto it = runs.find(receiver->GetInstanceId());
    if (it == runs.end() || receiver->GetMapId() != it->second.mapId)
        return;
    Run& run = it->second;

    // Every member of the group is told; count it once.
    std::ostringstream key;
    key << receiverGuid.GetCounter() << ":" << entry << ":" << uint32(bag) << ":" << slot << ":" << count;
    auto seen = recentPushes.find(key.str());
    if (seen != recentPushes.end() && WorldTimer::getMSTimeDiff(seen->second, nowMs) < PUSH_DEDUP_MS)
        return;
    recentPushes[key.str()] = nowMs;
    if (recentPushes.size() > 200)
        recentPushes.clear();

    std::string how = "looted";
    for (auto& roll : rollsWon)
        if (roll.winner == receiverGuid.GetCounter() && roll.entry == entry && WorldTimer::getMSTimeDiff(roll.ms, nowMs) <= ROLL_MATCH_MS)
            how = roll.type == 1 ? "need" : roll.type == 2 ? "greed" : "looted";
    if (how == "looted" && receiver->GetGroup() && receiver->GetGroup()->GetLootMethod() == MASTER_LOOT)
        how = "master";

    const time_t now = time(nullptr);
    int32 source = 0;
    if (run.lastKill && now - run.lastKill <= BOSS_LOOT_SECONDS)
        for (size_t i = run.bosses.size(); i-- > 0;)
            if (run.killed.count(run.bosses[i]))
            {
                source = run.bosses[i];
                break;
            }

    ObjectGuid itemGuid;
    if (slot != 0xFFFFFFFF)
        if (Item* item = receiver->GetItemByPos(bag, uint8(slot)))
            if (item->GetEntry() == entry)
                itemGuid = item->GetObjectGuid();

    ItemPrototype const* proto = sObjectMgr.GetItemPrototype(entry);
    CharacterDatabase.PExecute(
        "INSERT INTO guildbot_loot (instance, opened, receiver, item, quality, count, how, source, item_guid) "
        "VALUES (%u, FROM_UNIXTIME(%u), %u, %u, %u, %u, '%s', %d, %u)",
        run.instanceId, uint32(run.opened), receiverGuid.GetCounter(), entry, proto ? proto->Quality : 0, count,
        how.c_str(), source, itemGuid.GetCounter());

    if (itemGuid)
    {
        Loot loot;
        loot.instanceId = run.instanceId;
        loot.opened = run.opened;
        loot.at = now;
        loot.receiver = receiverGuid.GetCounter();
        loot.entry = entry;
        loot.count = count;
        loot.item = itemGuid;
        loots.push_back(loot);
    }
}

std::list<std::string> GuildRuns::HandleRuns(const std::string& args)
{
    uint32 limit = uint32(atoi(args.c_str()));
    if (!limit)
        limit = 5;

    std::list<std::string> out = { "dungeon runs, newest first" };
    auto runsResult = CharacterDatabase.PQuery(
        "SELECT r.instance, UNIX_TIMESTAMP(r.opened), DATE_FORMAT(r.opened, '%%m-%%d %%H:%%i'), r.dungeon, c.name, r.player_led, "
        "TIMESTAMPDIFF(MINUTE, r.opened, COALESCE(r.closed, NOW())), r.ended, r.outcome, r.bosses_killed, r.bosses, r.wipes "
        "FROM guildbot_run r LEFT JOIN characters c ON c.guid = r.leader ORDER BY r.opened DESC LIMIT %u", limit);
    if (!runsResult)
        return out;
    do
    {
        Field* f = runsResult->Fetch();
        const uint32 instance = f[0].GetUInt32(), opened = f[1].GetUInt32();
        std::ostringstream line;
        line << f[2].GetCppString() << "  " << f[3].GetCppString() << ", led by " << f[4].GetCppString() << (f[5].GetUInt32() ? " (player)" : "")
             << ", " << f[6].GetUInt32() << " min, " << f[7].GetCppString() << ", " << f[8].GetCppString()
             << ", " << f[9].GetUInt32() << " of " << f[10].GetUInt32() << " bosses, " << f[11].GetUInt32() << " wipes";
        out.push_back(line.str());

        auto members = CharacterDatabase.PQuery(
            "SELECT name, player, level_in, level_out, experience, money, deaths, seconds FROM guildbot_run_member "
            "WHERE instance = %u AND opened = FROM_UNIXTIME(%u) ORDER BY name", instance, opened);
        if (members)
        {
            do
            {
                Field* m = members->Fetch();
                std::ostringstream ml;
                ml << "    " << m[0].GetCppString() << (m[1].GetUInt32() ? " (player)" : "") << "  L" << m[2].GetUInt32() << "-" << m[3].GetUInt32()
                   << ", " << m[4].GetUInt32() << " xp, " << m[5].GetUInt32() / 100 << "s, " << m[6].GetUInt32() << " deaths, "
                   << m[7].GetUInt32() / MINUTE << " min inside";
                out.push_back(ml.str());
            } while (members->NextRow());
        }

        auto loot = CharacterDatabase.PQuery(
            "SELECT t.name, l.quality, c.name, l.how, l.fate, COALESCE(b.Name, '') FROM guildbot_loot l "
            "JOIN mangos.item_template t ON t.entry = l.item LEFT JOIN characters c ON c.guid = l.receiver "
            "LEFT JOIN mangos.creature_template b ON b.Entry = l.source "
            "WHERE l.instance = %u AND l.opened = FROM_UNIXTIME(%u) AND l.quality >= 2 ORDER BY l.at", instance, opened);
        if (loot)
        {
            do
            {
                Field* l = loot->Fetch();
                std::ostringstream ll;
                ll << "    loot " << l[0].GetCppString() << " (q" << l[1].GetUInt32() << ") to " << l[2].GetCppString() << " by " << l[3].GetCppString()
                   << ", " << l[4].GetCppString() << (l[5].GetCppString().empty() ? "" : ", from " + l[5].GetCppString());
                out.push_back(ll.str());
            } while (loot->NextRow());
        }
    } while (runsResult->NextRow());
    return out;
}
