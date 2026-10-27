#include "playerbot/playerbot.h"
#include "playerbot/GuildRuns.h"
#include "playerbot/GuildBotMgr.h"
#include "playerbot/GuildDungeon.h"

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
        " ended VARCHAR(16) NOT NULL DEFAULT 'open',"       // open, reset, left
        " outcome VARCHAR(16) NOT NULL DEFAULT 'none',"     // cleared, partial, none
        " bosses_killed INT UNSIGNED NOT NULL DEFAULT 0,"
        " bosses INT UNSIGNED NOT NULL DEFAULT 0,"
        " wipes INT UNSIGNED NOT NULL DEFAULT 0,"
        " PRIMARY KEY (instance, opened), KEY opened (opened)"
        ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4");
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

    // A run still open from before a restart goes on: its members' counts and its dead bosses come back.
    auto open = CharacterDatabase.PQuery(
        "SELECT UNIX_TIMESTAMP(opened), COALESCE(UNIX_TIMESTAMP(entered), 0), wipes FROM guildbot_run "
        "WHERE instance = %u AND ended = 'open' ORDER BY opened DESC LIMIT 1", run.instanceId);
    if (open)
    {
        Field* f = open->Fetch();
        run.opened = time_t(f[0].GetUInt64());
        run.entered = time_t(f[1].GetUInt64());
        run.wipes = f[2].GetUInt32();
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
        "INSERT INTO guildbot_run (instance, opened, map, dungeon, leader, player_led, entered, outcome, bosses_killed, bosses, wipes) "
        "VALUES (%u, FROM_UNIXTIME(%u), %u, '%s', %u, %u, %s, '%s', %u, %u, %u) "
        "ON DUPLICATE KEY UPDATE leader = VALUES(leader), player_led = VALUES(player_led), entered = VALUES(entered), "
        "outcome = VALUES(outcome), bosses_killed = VALUES(bosses_killed), bosses = VALUES(bosses), wipes = VALUES(wipes)",
        run.instanceId, uint32(run.opened), run.mapId, Escape(run.dungeon).c_str(), run.leader, run.playerLed ? 1 : 0,
        entered.c_str(), Outcome(run.killed.count(run.lastBoss) > 0, run.killed.size()).c_str(),
        uint32(run.killed.size()), uint32(run.bosses.size()), run.wipes);

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
