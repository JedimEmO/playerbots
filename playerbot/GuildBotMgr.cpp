#include "playerbot/playerbot.h"
#include "playerbot/GuildBotMgr.h"
#include "playerbot/PlayerbotAIConfig.h"
#include "playerbot/ServerFacade.h"
#include "playerbot/strategy/Engine.h"
#include "playerbot/strategy/actions/ChangeTalentsAction.h"
#include "playerbot/Talentspec.h"
#include "Accounts/AccountMgr.h"
#include "Chat/Chat.h"

#include <set>
#include <sstream>

namespace
{
    // How often the orders are read and acted on, and how often the state is written.
    constexpr uint32 ORDERS_MS = 5 * IN_MILLISECONDS;
    constexpr uint32 STATE_MS = 10 * IN_MILLISECONDS;
    // A character a player logged out of waits this long before the engine takes it back,
    // so a quick relog by the player does not race the engine's login.
    constexpr time_t RELOGIN_SECONDS = 30;
    // A login that has not completed is retried after this long.
    constexpr time_t LOGIN_RETRY_SECONDS = 60;
    // A levelling character that earns no experience for this long is reported as stalled.
    constexpr time_t STALL_SECONDS = 15 * MINUTE;

    std::string Trim(const std::string& s)
    {
        size_t b = s.find_first_not_of(' ');
        if (b == std::string::npos)
            return "";
        size_t e = s.find_last_not_of(' ');
        return s.substr(b, e - b + 1);
    }

    std::pair<std::string, std::string> SplitWord(const std::string& s)
    {
        std::string t = Trim(s);
        size_t sp = t.find(' ');
        if (sp == std::string::npos)
            return { t, "" };
        return { t.substr(0, sp), Trim(t.substr(sp + 1)) };
    }

    bool IsRealSession(Player* player)
    {
        return player->GetSession() && player->GetSession()->GetRemoteAddress() != "disconnected/bot";
    }

    uint32 FreeBagSlots(Player* bot)
    {
        uint32 free = 0;
        for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; slot++)
            if (!bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
                free++;

        for (uint8 bag = INVENTORY_SLOT_BAG_START; bag < INVENTORY_SLOT_BAG_END; ++bag)
        {
            const Bag* const pBag = (Bag*)bot->GetItemByPos(INVENTORY_SLOT_BAG_0, bag);
            if (pBag && pBag->GetProto()->Class == ITEM_CLASS_CONTAINER && pBag->GetProto()->SubClass == ITEM_SUBCLASS_CONTAINER)
                free += pBag->GetFreeSlots();
        }
        return free;
    }

    // The tree with the most talent points, by name, or empty below level 10.
    std::string TreeName(Player* bot)
    {
        TalentSpec spec(bot);
        if (!spec.GetTalentPoints())
            return "";
        return ChatHelper::formatClass(bot, spec.highestTree());
    }

    // The jobs the engine knows. Anything else fails with "unknown job".
    bool IsKnownJob(const std::string& job)
    {
        return job == "idle" || job == "level" || job == "party" || job == "farm" || job == "learn";
    }
}

bool GuildBotMgr::IsGuildBot(uint32 guid) const
{
    auto it = characters.find(guid);
    return it != characters.end() && it->second.enabled && !it->second.playerControlled;
}

bool GuildBotMgr::IsGuildBot(Player* player) const
{
    return player && IsGuildBot(player->GetGUIDLow());
}

bool GuildBotMgr::IsPlayerControlled(Player* player) const
{
    auto it = characters.find(player->GetGUIDLow());
    return it != characters.end() && (it->second.playerControlled || IsRealSession(player));
}

std::string GuildBotMgr::GetActiveJob(uint32 guid) const
{
    auto it = characters.find(guid);
    if (it == characters.end())
        return "idle";

    const Character& c = it->second;
    if (c.status != "working" || !IsKnownJob(c.job))
        return "idle";

    return c.job;
}

BotRoles GuildBotMgr::GetRole(uint32 guid) const
{
    auto it = characters.find(guid);
    return it == characters.end() ? BotRoles::BOT_ROLE_NONE : ChatHelper::parseRole(it->second.role);
}

bool GuildBotMgr::GetFarmOrder(uint32 guid, uint32& itemId, uint32& amount) const
{
    auto it = characters.find(guid);
    if (it == characters.end() || GetActiveJob(guid) != "farm")
        return false;

    itemId = uint32(atoi(ArgValue(it->second, "item").c_str()));
    amount = uint32(atoi(ArgValue(it->second, "amount").c_str()));
    return itemId && sObjectMgr.GetItemPrototype(itemId);
}

bool GuildBotMgr::IsJobItem(uint32 guid, uint32 itemId) const
{
    uint32 farmItem, amount;
    return GetFarmOrder(guid, farmItem, amount) && farmItem == itemId;
}

uint32 GuildBotMgr::ProfessionSkill(const std::string& name)
{
    static const std::map<std::string, uint32> skills = {
        { "alchemy", SKILL_ALCHEMY }, { "blacksmithing", SKILL_BLACKSMITHING }, { "enchanting", SKILL_ENCHANTING },
        { "engineering", SKILL_ENGINEERING }, { "herbalism", SKILL_HERBALISM }, { "leatherworking", SKILL_LEATHERWORKING },
        { "mining", SKILL_MINING }, { "skinning", SKILL_SKINNING }, { "tailoring", SKILL_TAILORING },
        { "cooking", SKILL_COOKING }, { "first_aid", SKILL_FIRST_AID }, { "fishing", SKILL_FISHING },
    };
    auto it = skills.find(name);
    return it == skills.end() ? 0 : it->second;
}

bool GuildBotMgr::MayLearnSkill(Player* bot, uint32 skill) const
{
    if (!IsGuildBot(bot) || bot->HasSkill(skill))
        return true;

    auto it = characters.find(bot->GetGUIDLow());
    return GetActiveJob(bot->GetGUIDLow()) == "learn" && ProfessionSkill(ArgValue(it->second, "profession")) == skill;
}

std::string GuildBotMgr::ArgValue(const Character& c, const std::string& key) const
{
    std::istringstream in(c.jobArgs);
    std::string word;
    while (in >> word)
    {
        size_t eq = word.find('=');
        if (eq != std::string::npos && word.substr(0, eq) == key)
            return word.substr(eq + 1);
    }
    return "";
}

void GuildBotMgr::AddNonCombatStrategies(Player* player, ai::Engine* engine) const
{
    // None of the random bots' habits, and nothing that needs a master to make sense.
    engine->removeStrategy("quest");
    engine->removeStrategy("duel");
    engine->removeStrategy("emote");
    engine->removeStrategy("follow");
    engine->removeStrategy("wander");
    engine->removeStrategy("rpg jump");
    engine->removeStrategy("follow jump");
    engine->addStrategy("roll");

    // In a group, following someone: the party comes first, and the job waits until the group is over.
    PlayerbotAI* ai = player->GetPlayerbotAI();
    Player* master = ai ? ai->GetMaster() : nullptr;
    if (player->GetGroup() && master && master != player)
    {
        engine->addStrategies("follow", "group", NULL);
        return;
    }

    const std::string job = GetActiveJob(player->GetGUIDLow());

    if (job == "learn")
    {
        // Walk to a trainer of the profession and learn it there; the town services as usual, no grinding.
        engine->addStrategies("collision", "travel", "rpg", "free", "guild level", NULL);
        engine->removeStrategy("rpg quest");
        engine->removeStrategy("rpg explore");
        engine->removeStrategy("rpg player");
        engine->removeStrategy("rpg bg");
        engine->removeStrategy("rpg guild");
        return;
    }

    if (job == "farm")
    {
        // Travel to where the item drops or grows, through the guild order travel target, and fight what is there.
        // It sells, repairs and trains like a levelling character, but does not travel to grind.
        engine->addStrategies("collision", "grind", "travel", "rpg", "free", "guild level", NULL);
        engine->removeStrategy("rpg quest");
        engine->removeStrategy("rpg explore");
        engine->removeStrategy("rpg player");
        engine->removeStrategy("rpg bg");
        engine->removeStrategy("rpg guild");
        return;
    }

    if (job == "level")
    {
        // Grind creatures of the character's level, travelling between grind spots,
        // and sell, repair, train and bank at the places travel picks.
        engine->addStrategies("collision", "grind", "travel", "rpg", "free", "guild level", NULL);
        // rpg adds all its parts; a levelling character only uses the town services.
        engine->removeStrategy("rpg quest");
        engine->removeStrategy("rpg explore");
        engine->removeStrategy("rpg player");
        engine->removeStrategy("rpg bg");
        engine->removeStrategy("rpg guild");
        return;
    }

    // idle: stand where it is.
    engine->addStrategy("stay");
}

void GuildBotMgr::Tick(uint32 elapsed)
{
    if (!sPlayerbotAIConfig.enabled)
        return;

    UpdateSessions(elapsed);

    sinceOrders += elapsed;
    sinceState += elapsed;

    if (sinceOrders >= ORDERS_MS)
    {
        sinceOrders = 0;
        if (!tablesReady)
            EnsureTables();
        LoadOrders();
        LoginLogout();
        ApplyOrders();
        CheckJobs();
    }

    if (sinceState >= STATE_MS)
    {
        sinceState = 0;
        WriteState();
    }
}

void GuildBotMgr::EnsureTables()
{
    CharacterDatabase.DirectExecute(
        "CREATE TABLE IF NOT EXISTS guildbot_character ("
        " guid INT UNSIGNED NOT NULL PRIMARY KEY,"
        " enabled TINYINT UNSIGNED NOT NULL DEFAULT 1,"
        " job VARCHAR(32) NOT NULL DEFAULT 'idle',"
        " job_args VARCHAR(255) NOT NULL DEFAULT '',"
        " job_seq INT UNSIGNED NOT NULL DEFAULT 1,"
        " updated_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP"
        ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4");
    CharacterDatabase.DirectExecute(
        "ALTER TABLE guildbot_character ADD COLUMN IF NOT EXISTS role VARCHAR(8) NOT NULL DEFAULT '' AFTER job_seq");

    CharacterDatabase.DirectExecute(
        "CREATE TABLE IF NOT EXISTS guildbot_state ("
        " guid INT UNSIGNED NOT NULL PRIMARY KEY,"
        " name VARCHAR(12) NOT NULL,"
        " controller VARCHAR(8) NOT NULL,"
        " level TINYINT UNSIGNED NOT NULL,"
        " xp INT UNSIGNED NOT NULL,"
        " xp_next INT UNSIGNED NOT NULL,"
        " money INT UNSIGNED NOT NULL,"
        " map SMALLINT UNSIGNED NOT NULL,"
        " zone INT UNSIGNED NOT NULL,"
        " x FLOAT NOT NULL, y FLOAT NOT NULL, z FLOAT NOT NULL,"
        " health_pct TINYINT UNSIGNED NOT NULL,"
        " dead TINYINT UNSIGNED NOT NULL,"
        " in_combat TINYINT UNSIGNED NOT NULL,"
        " free_bag_slots SMALLINT UNSIGNED NOT NULL,"
        " job VARCHAR(32) NOT NULL,"
        " job_seq INT UNSIGNED NOT NULL,"
        " job_status VARCHAR(16) NOT NULL,"
        " job_reason VARCHAR(255) NOT NULL,"
        " updated_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP"
        ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4");

    CharacterDatabase.DirectExecute(
        "ALTER TABLE guildbot_state"
        " ADD COLUMN IF NOT EXISTS role VARCHAR(8) NOT NULL DEFAULT '',"
        " ADD COLUMN IF NOT EXISTS talent_tree VARCHAR(32) NOT NULL DEFAULT '',"
        " ADD COLUMN IF NOT EXISTS talent_points TINYINT UNSIGNED NOT NULL DEFAULT 0,"
        " ADD COLUMN IF NOT EXISTS role_status VARCHAR(32) NOT NULL DEFAULT ''");

    CharacterDatabase.DirectExecute(
        "CREATE TABLE IF NOT EXISTS guildbot_event ("
        " id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT PRIMARY KEY,"
        " guid INT UNSIGNED NOT NULL,"
        " at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,"
        " kind VARCHAR(32) NOT NULL,"
        " detail VARCHAR(255) NOT NULL DEFAULT '',"
        " KEY guid (guid), KEY at (at)"
        ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4");

    tablesReady = true;
}

void GuildBotMgr::LoadOrders()
{
    auto result = CharacterDatabase.PQuery(
        "SELECT g.guid, c.account, c.name, g.enabled, g.job, g.job_args, g.job_seq, g.role "
        "FROM guildbot_character g JOIN characters c ON c.guid = g.guid");

    std::set<uint32> seen;
    if (result)
    {
        do
        {
            Field* f = result->Fetch();
            uint32 guid = f[0].GetUInt32();
            seen.insert(guid);

            Character& c = characters[guid];
            bool isNew = c.guid == 0;
            c.guid = guid;
            c.account = f[1].GetUInt32();
            c.name = f[2].GetCppString();
            c.enabled = f[3].GetUInt32() != 0;
            c.job = f[4].GetCppString();
            c.jobArgs = f[5].GetCppString();
            c.jobSeq = f[6].GetUInt32();
            std::string role = f[7].GetCppString();
            if (!isNew && role != c.role)
                c.roleChanged = true;
            c.role = role;
            if (isNew)
                c.offlineSince = 0;
        } while (result->NextRow());
    }

    for (auto it = characters.begin(); it != characters.end();)
    {
        if (seen.count(it->first))
        {
            ++it;
            continue;
        }

        if (GetPlayerBot(it->first))
            LogoutPlayerBot(it->first);
        it = characters.erase(it);
    }
}

void GuildBotMgr::LoginLogout()
{
    const time_t now = time(nullptr);

    for (auto& [guid, c] : characters)
    {
        Player* player = sObjectMgr.GetPlayer(ObjectGuid(HIGHGUID_PLAYER, guid), false);
        Player* held = GetPlayerBot(guid);

        if (held)
        {
            if (!c.enabled)
            {
                Event(guid, "logout", "switched off");
                LogoutPlayerBot(guid);
                c.offlineSince = now;
            }
            continue;
        }

        if (player)
        {
            // Online, and not ours: a player is on it, by a normal login or a takeover.
            if (IsRealSession(player) && !c.playerControlled)
            {
                c.playerControlled = true;
                Event(guid, "player", "a player logged in");
            }
            continue;
        }

        // Offline.
        if (c.playerControlled)
        {
            c.playerControlled = false;
            c.offlineSince = now;
        }

        if (!c.enabled)
            continue;

        if (c.offlineSince && now - c.offlineSince < RELOGIN_SECONDS)
            continue;

        if (c.loginRequested && now - c.loginRequested < LOGIN_RETRY_SECONDS)
            continue;

        c.loginRequested = now;
        // The bot's own account as the master account: the login check allows a bot on its master's account,
        // whether or not that account is online. No master is set; guild characters have none.
        AddPlayerBot(guid, c.account);
    }
}

void GuildBotMgr::OnBotLoginInternal(Player* const bot)
{
    auto it = characters.find(bot->GetGUIDLow());
    if (it == characters.end())
        return;

    Character& c = it->second;
    c.loginRequested = 0;
    c.lastLevel = bot->GetLevel();
    c.progressAt = 0;
    c.stalled = false;
    // OnBotLogin resets the strategies after this, from the current order.
    c.appliedSeq = c.jobSeq;
    if (!IsKnownJob(c.job))
    {
        c.status = "failed";
        c.reason = "unknown job " + c.job;
    }
    Event(c.guid, "login", c.job);
}

void GuildBotMgr::ApplyOrders()
{
    for (auto& [guid, c] : characters)
    {
        Player* bot = GetPlayerBot(guid);
        if (!bot || !bot->GetPlayerbotAI() || c.appliedSeq == c.jobSeq)
            continue;

        c.appliedSeq = c.jobSeq;
        c.reason.clear();
        c.progressAt = 0;
        c.stalled = false;
        uint32 farmItem = uint32(atoi(ArgValue(c, "item").c_str()));
        uint32 learnSkill = ProfessionSkill(ArgValue(c, "profession"));
        if (c.job == "farm" && (!farmItem || !sObjectMgr.GetItemPrototype(farmItem)))
        {
            c.status = "failed";
            c.reason = "farm needs item=<item id> of an item that exists";
            Event(guid, "job_failed", c.reason);
        }
        else if (c.job == "learn" && !learnSkill)
        {
            c.status = "failed";
            c.reason = "learn needs profession=<alchemy|blacksmithing|enchanting|engineering|herbalism|leatherworking|mining|skinning|tailoring|cooking|first_aid|fishing>";
            Event(guid, "job_failed", c.reason);
        }
        else if (c.job == "learn" && IsPrimaryProfessionSkill(learnSkill) && !bot->HasSkill(learnSkill) && !bot->GetFreePrimaryProfessionPoints())
        {
            c.status = "failed";
            c.reason = "already has two primary professions";
            Event(guid, "job_failed", c.reason);
        }
        else if (IsKnownJob(c.job))
        {
            c.status = "working";
            Event(guid, "job", c.job + (c.jobArgs.empty() ? "" : " " + c.jobArgs));
        }
        else
        {
            c.status = "failed";
            c.reason = "unknown job " + c.job;
            Event(guid, "job_failed", c.reason);
        }

        bot->GetPlayerbotAI()->ResetStrategies(false);
    }
}

void GuildBotMgr::CheckJobs()
{
    for (auto& [guid, c] : characters)
    {
        Player* bot = GetPlayerBot(guid);
        if (!bot)
            continue;

        if (c.lastLevel && bot->GetLevel() > c.lastLevel)
            Event(guid, "level", std::to_string(bot->GetLevel()));
        c.lastLevel = bot->GetLevel();

        ApplyRole(c, bot);

        bool dead = sServerFacade.UnitIsDead(bot);
        if (dead && !c.dead)
            Event(guid, "died", std::to_string(bot->GetZoneId()));
        c.dead = dead;

        const time_t now = time(nullptr);
        uint64 progress = uint64(bot->GetLevel()) * 10000000 + bot->GetUInt32Value(PLAYER_XP);
        if (progress != c.lastProgress || !c.progressAt)
        {
            c.lastProgress = progress;
            c.progressAt = now;
            if (c.stalled)
            {
                c.stalled = false;
                if (c.reason.rfind("no experience", 0) == 0)
                    c.reason.clear();
                Event(guid, "progress", "earning experience again");
            }
        }

        if (!c.partyLeader.empty() && (c.status != "working" || c.job != "party"))
            LeaveParty(c, bot);

        if (c.status != "working")
            continue;

        if (c.job == "party")
        {
            std::string why = KeepParty(c, bot);
            if (why != c.reason)
            {
                c.reason = why;
                if (why.empty())
                    Event(guid, "party", "joined " + c.partyLeader);
            }
            continue;
        }

        if (c.job == "learn")
        {
            uint32 skill = ProfessionSkill(ArgValue(c, "profession"));
            if (skill && bot->HasSkill(skill))
            {
                c.status = "done";
                c.reason = "knows " + ArgValue(c, "profession");
                Event(guid, "job_done", "learn " + c.reason);
                if (bot->GetPlayerbotAI())
                    bot->GetPlayerbotAI()->ResetStrategies(false);
            }
            continue;
        }

        if (c.job == "farm")
        {
            uint32 itemId, amount;
            if (GetFarmOrder(guid, itemId, amount) && bot->GetPlayerbotAI())
            {
                uint32 have = bot->GetPlayerbotAI()->GetInventoryItemsCountWithId(itemId);
                std::string what = std::to_string(have) + " of " + sObjectMgr.GetItemPrototype(itemId)->Name1;
                if (amount && have >= amount)
                {
                    c.status = "done";
                    c.reason = "has " + what;
                    Event(guid, "job_done", "farm " + c.reason);
                    bot->GetPlayerbotAI()->ResetStrategies(false);
                }
                else if (!FreeBagSlots(bot))
                {
                    c.status = "failed";
                    c.reason = "bags full, with " + what;
                    Event(guid, "job_failed", "farm " + c.reason);
                    bot->GetPlayerbotAI()->ResetStrategies(false);
                }
                else
                    c.reason = "has " + what;
            }
            continue;
        }

        if (c.job == "level" && !c.stalled && now - c.progressAt >= STALL_SECONDS)
        {
            c.stalled = true;
            c.reason = "no experience for " + std::to_string((now - c.progressAt) / MINUTE) + " minutes";
            Event(guid, "stalled", c.reason);
        }

        if (c.job == "level")
        {
            std::string until = ArgValue(c, "until");
            uint32 target = until.empty() ? 0 : uint32(atoi(until.c_str()));
            if (target && bot->GetLevel() >= target)
            {
                c.status = "done";
                c.reason = "reached level " + std::to_string(bot->GetLevel());
                Event(guid, "job_done", c.job + " " + c.reason);
                if (bot->GetPlayerbotAI())
                    bot->GetPlayerbotAI()->ResetStrategies(false);
            }
        }
    }
}

void GuildBotMgr::ApplyRole(Character& c, Player* bot)
{
    BotRoles role = ChatHelper::parseRole(c.role);
    std::set<int> trees = ChangeTalentsAction::RoleTrees(bot->getClass(), role);
    TalentSpec spec(bot);
    int spent = spec.GetTalentPoints();

    // Talents already spent in a tree that does not suit the role stay: a respec is the player's to pay for.
    std::string status = (spent > 0 && !trees.empty() && !trees.count(spec.highestTree())) ? "respec needed" : "";

    if (c.roleChanged)
    {
        c.roleChanged = false;
        if (spent == 0)
        {
            // Forget the talent path picked earlier, so the next pick follows the role.
            sRandomPlayerbotMgr.SetValue(bot->GetGUIDLow(), "specNo", 0);
            sRandomPlayerbotMgr.SetValue(bot->GetGUIDLow(), "specLink", 0, "");
        }
        Event(c.guid, "role", (c.role.empty() ? std::string("none") : c.role) + (status.empty() ? "" : " (" + status + ")"));

        if (spent == 0 && bot->CalculateTalentsPoints() > 0)
        {
            std::ostringstream out;
            ChangeTalentsAction::AutoSelectTalents(bot, &out, role);
            if (bot->GetPlayerbotAI())
                bot->GetPlayerbotAI()->ResetStrategies(false);
        }
    }

    if (status != c.roleStatus)
    {
        c.roleStatus = status;
        if (!status.empty())
            Event(c.guid, "respec_needed", c.role);
    }
}

std::string GuildBotMgr::KeepParty(Character& c, Player* bot)
{
    std::string leaderName = ArgValue(c, "leader");
    if (!normalizePlayerName(leaderName))
        return "no leader given";
    if (leaderName == c.name)
        return "cannot lead its own party";

    Player* leader = ObjectAccessor::FindPlayerByName(leaderName.c_str());
    if (!leader || !leader->IsInWorld())
        return "waiting for " + leaderName + " to be online";
    // Only the player's own characters: the same account, as the invite rule has it.
    if (leader->GetSession()->GetAccountId() != bot->GetSession()->GetAccountId())
        return leaderName + " is not on the same account";

    Group* group = bot->GetGroup();
    if (group && group->IsMember(leader->GetObjectGuid()))
    {
        c.partyLeader = leaderName;
        PlayerbotAI* ai = bot->GetPlayerbotAI();
        if (ai && ai->GetMaster() != leader && group->IsLeader(leader->GetObjectGuid()))
        {
            ai->SetMaster(leader);
            ai->ResetStrategies(false);
        }
        return "";
    }

    if (group)
        bot->RemoveFromGroup();

    Group* leaderGroup = leader->GetGroup();
    if (leaderGroup && !leaderGroup->IsLeader(leader->GetObjectGuid()))
        return leaderName + " is in a group it does not lead";
    if (leaderGroup && leaderGroup->IsFull())
        return leaderName + "'s group is full";

    // Invite and accept, the way the client does, so the core forms the group itself.
    if (!leaderGroup)
    {
        leaderGroup = leader->GetGroupInvite();
        if (!leaderGroup || !leaderGroup->IsLeader(leader->GetObjectGuid()))
        {
            leaderGroup = new Group();
            if (!leaderGroup->AddLeaderInvite(leader))
            {
                delete leaderGroup;
                return "could not form a group with " + leaderName;
            }
        }
    }
    if (!leaderGroup->AddInvite(bot))
        return "could not be invited to " + leaderName + "'s group";

    WorldPacket p;
    bot->GetSession()->HandleGroupAcceptOpcode(p);

    if (!bot->GetGroup() || !bot->GetGroup()->IsMember(leader->GetObjectGuid()))
        return "could not join " + leaderName + "'s group";

    c.partyLeader = leaderName;
    if (PlayerbotAI* ai = bot->GetPlayerbotAI())
    {
        ai->SetMaster(leader);
        ai->ResetStrategies(false);
    }
    return "";
}

void GuildBotMgr::LeaveParty(Character& c, Player* bot)
{
    Player* leader = ObjectAccessor::FindPlayerByName(c.partyLeader.c_str());
    Group* group = bot->GetGroup();
    if (group && (!leader || group->IsMember(leader->GetObjectGuid())))
        bot->RemoveFromGroup();

    Event(c.guid, "party_left", c.partyLeader);
    c.partyLeader.clear();
    if (PlayerbotAI* ai = bot->GetPlayerbotAI())
    {
        ai->SetMaster(nullptr);
        ai->ResetStrategies(false);
    }
}

void GuildBotMgr::WriteState()
{
    for (auto& [guid, c] : characters)
    {
        Player* player = sObjectMgr.GetPlayer(ObjectGuid(HIGHGUID_PLAYER, guid), false);
        WriteState(c, player);
    }
}

void GuildBotMgr::WriteState(const Character& c, Player* bot)
{
    std::string controller = !bot ? "offline" : (GetPlayerBot(c.guid) ? "bot" : "player");

    if (!bot)
    {
        CharacterDatabase.PExecute(
            "INSERT INTO guildbot_state (guid, name, controller, level, xp, xp_next, money, map, zone, x, y, z, "
            "health_pct, dead, in_combat, free_bag_slots, job, job_seq, job_status, job_reason) "
            "SELECT c.guid, c.name, 'offline', c.level, c.xp, 0, c.money, c.map, c.zone, c.position_x, c.position_y, c.position_z, "
            "0, 0, 0, 0, '%s', %u, '%s', '%s' FROM characters c WHERE c.guid = %u "
            "ON DUPLICATE KEY UPDATE controller = 'offline', level = VALUES(level), xp = VALUES(xp), money = VALUES(money), "
            "map = VALUES(map), zone = VALUES(zone), x = VALUES(x), y = VALUES(y), z = VALUES(z), in_combat = 0, "
            "job = VALUES(job), job_seq = VALUES(job_seq), job_status = VALUES(job_status), job_reason = VALUES(job_reason), role = '%s'",
            Escape(c.job).c_str(), c.jobSeq, Escape(c.status).c_str(), Escape(c.reason).c_str(), c.guid, Escape(c.role).c_str());
        return;
    }

    CharacterDatabase.PExecute(
        "REPLACE INTO guildbot_state (guid, name, controller, level, xp, xp_next, money, map, zone, x, y, z, "
        "health_pct, dead, in_combat, free_bag_slots, job, job_seq, job_status, job_reason, role, talent_tree, talent_points, role_status) "
        "VALUES (%u, '%s', '%s', %u, %u, %u, %u, %u, %u, %f, %f, %f, %u, %u, %u, %u, '%s', %u, '%s', '%s', '%s', '%s', %u, '%s')",
        c.guid, Escape(c.name).c_str(), controller.c_str(), bot->GetLevel(), bot->GetUInt32Value(PLAYER_XP),
        bot->GetUInt32Value(PLAYER_NEXT_LEVEL_XP), bot->GetMoney(), bot->GetMapId(), bot->GetZoneId(),
        bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(), uint32(bot->GetHealthPercent()),
        sServerFacade.UnitIsDead(bot) ? 1 : 0, sServerFacade.IsInCombat(bot) ? 1 : 0, FreeBagSlots(bot),
        Escape(c.job).c_str(), GetPlayerBot(c.guid) ? c.appliedSeq : c.jobSeq, Escape(c.status).c_str(), Escape(c.reason).c_str(),
        Escape(c.role).c_str(), Escape(TreeName(bot)).c_str(), uint32(TalentSpec(bot).GetTalentPoints()), Escape(c.roleStatus).c_str());
}

void GuildBotMgr::Event(uint32 guid, const std::string& kind, const std::string& detail)
{
    CharacterDatabase.PExecute("INSERT INTO guildbot_event (guid, kind, detail) VALUES (%u, '%s', '%s')",
        guid, Escape(kind).c_str(), Escape(detail.substr(0, 255)).c_str());
}

std::string GuildBotMgr::Escape(std::string text)
{
    CharacterDatabase.escape_string(text);
    return text;
}

void GuildBotMgr::OnPlayerTakeover(Player* player)
{
    auto it = characters.find(player->GetGUIDLow());
    if (it == characters.end() || !GetPlayerBot(player->GetGUIDLow()))
        return;

    // The core has moved the character onto the player's session. Let go of it without logging it out;
    // the bot session it had is left behind, as the core does for random bots.
    GetAllBots().erase(player->GetGUIDLow());
    it->second.playerControlled = true;
    Event(it->first, "player", "a player took over");
}

void GuildBotMgr::OnPlayerLogout(Player* player)
{
    // Characters following the one who left go back to their jobs.
    ForEachPlayerbot([&](Player* bot)
    {
        PlayerbotAI* ai = bot->GetPlayerbotAI();
        if (bot != player && ai && ai->GetMaster() == player)
        {
            ai->SetMaster(nullptr);
            ai->ResetStrategies(false);
        }
    });

    auto it = characters.find(player->GetGUIDLow());
    if (it == characters.end() || GetPlayerBot(player->GetGUIDLow()))
        return;

    if (it->second.playerControlled)
    {
        it->second.playerControlled = false;
        it->second.offlineSince = time(nullptr);
        Event(it->first, "player_left", "a player logged out");
    }
}

void GuildBotMgr::LogoutAll()
{
    LogoutAllBots();
}

GuildBotMgr::Character* GuildBotMgr::Find(const std::string& name)
{
    std::string wanted = name;
    normalizePlayerName(wanted);
    for (auto& [guid, c] : characters)
        if (c.name == wanted)
            return &c;
    return nullptr;
}

uint32 GuildBotMgr::GetOrCreateAccount(Player* master, std::string& error)
{
    if (!createAccount)
        error = "no account";
    return createAccount;
}

std::list<std::string> GuildBotMgr::HandleCommand(std::string args)
{
    auto [sub, rest] = SplitWord(args);

    if (!tablesReady)
        EnsureTables();

    if (sub == "add")
        return HandleAdd(rest);
    if (sub == "create")
        return HandleCreate(rest);
    if (sub == "list")
        return HandleList();
    if (sub == "state")
        return HandleState(rest);
    if (sub == "on" || sub == "off" || sub == "job" || sub == "remove" || sub == "role")
        return HandleOrder(sub, rest);

    return {
        "guildbot add <name> [job [args]]       make a character a guild character",
        "guildbot create <account> <name> <race> <class> [male|female]   create a level 1 character on an account",
        "guildbot job <name> <job> [args]       give an order: idle, level [until=<level>], party leader=<name>, farm item=<id> [amount=<n>], learn profession=<name>",
        "guildbot role <name> tank|healer|dps|none   the role its talents follow",
        "guildbot on|off <name>                 switch the engine's play on or off",
        "guildbot remove <name>                 stop treating it as a guild character",
        "guildbot list                          every guild character, one line each",
        "guildbot state <name>                  what it is doing, as the state table holds it",
    };
}

std::list<std::string> GuildBotMgr::HandleAdd(const std::string& args)
{
    auto [name, jobArgs] = SplitWord(args);
    auto [job, rest] = SplitWord(jobArgs);
    if (job.empty())
        job = "idle";

    std::string normalized = name;
    if (!normalizePlayerName(normalized))
        return { "usage: guildbot add <name> [job [args]]" };

    ObjectGuid guid = sObjectMgr.GetPlayerGuidByName(normalized);
    if (!guid)
        return { "no character named " + normalized };

    if (sRandomPlayerbotMgr.IsRandomBot(guid.GetCounter()))
        return { normalized + " is a random bot" };

    CharacterDatabase.DirectPExecute(
        "INSERT INTO guildbot_character (guid, enabled, job, job_args, job_seq) VALUES (%u, 1, '%s', '%s', 1) "
        "ON DUPLICATE KEY UPDATE enabled = 1, job = VALUES(job), job_args = VALUES(job_args), job_seq = job_seq + 1",
        guid.GetCounter(), Escape(job).c_str(), Escape(rest).c_str());
    sinceOrders = ORDERS_MS;
    return { normalized + " is a guild character, job " + job };
}

std::list<std::string> GuildBotMgr::HandleOrder(const std::string& sub, const std::string& args)
{
    auto [name, rest] = SplitWord(args);
    std::string normalized = name;
    if (!normalizePlayerName(normalized))
        return { "usage: guildbot " + sub + " <name> ..." };

    ObjectGuid guid = sObjectMgr.GetPlayerGuidByName(normalized);
    if (!guid || !IsGuildCharacter(guid.GetCounter()))
        return { normalized + " is not a guild character" };

    uint32 low = guid.GetCounter();
    if (sub == "on" || sub == "off")
        CharacterDatabase.DirectPExecute("UPDATE guildbot_character SET enabled = %u WHERE guid = %u", sub == "on" ? 1 : 0, low);
    else if (sub == "remove")
        CharacterDatabase.DirectPExecute("DELETE FROM guildbot_character WHERE guid = %u", low);
    else if (sub == "role")
    {
        std::string role = rest == "none" ? "" : rest;
        if (!role.empty() && ChatHelper::parseRole(role) == BotRoles::BOT_ROLE_NONE)
            return { "usage: guildbot role <name> tank|healer|dps|none" };
        CharacterDatabase.DirectPExecute("UPDATE guildbot_character SET role = '%s' WHERE guid = %u", Escape(role).c_str(), low);
    }
    else
    {
        auto [job, jobArgs] = SplitWord(rest);
        if (job.empty())
            return { "usage: guildbot job <name> <job> [args]" };
        CharacterDatabase.DirectPExecute(
            "UPDATE guildbot_character SET job = '%s', job_args = '%s', job_seq = job_seq + 1 WHERE guid = %u",
            Escape(job).c_str(), Escape(jobArgs).c_str(), low);
    }

    sinceOrders = ORDERS_MS;
    return { normalized + ": " + sub + (rest.empty() ? "" : " " + rest) };
}

std::list<std::string> GuildBotMgr::HandleList()
{
    std::list<std::string> out;
    for (auto& [guid, c] : characters)
    {
        Player* player = sObjectMgr.GetPlayer(ObjectGuid(HIGHGUID_PLAYER, guid), false);
        std::ostringstream line;
        line << c.name << " " << (!player ? "offline" : GetPlayerBot(guid) ? "bot" : "player")
             << (c.enabled ? "" : " (off)") << " L" << (player ? player->GetLevel() : 0)
             << (c.role.empty() ? "" : " role=" + c.role) << (c.roleStatus.empty() ? "" : " (" + c.roleStatus + ")")
             << " job=" << c.job << (c.jobArgs.empty() ? "" : " " + c.jobArgs)
             << " #" << c.jobSeq << " " << c.status << (c.reason.empty() ? "" : " (" + c.reason + ")");
        out.push_back(line.str());
    }
    if (out.empty())
        out.push_back("no guild characters");
    return out;
}

std::list<std::string> GuildBotMgr::HandleState(const std::string& args)
{
    Character* c = Find(Trim(args));
    if (!c)
        return { "not a guild character" };

    Player* bot = sObjectMgr.GetPlayer(ObjectGuid(HIGHGUID_PLAYER, c->guid), false);
    if (!bot)
        return { c->name + " offline, job " + c->job + " " + c->status };

    std::ostringstream out;
    out << "{\"name\":\"" << c->name << "\",\"controller\":\"" << (GetPlayerBot(c->guid) ? "bot" : "player")
        << "\",\"level\":" << bot->GetLevel() << ",\"xp\":" << bot->GetUInt32Value(PLAYER_XP)
        << ",\"xp_next\":" << bot->GetUInt32Value(PLAYER_NEXT_LEVEL_XP) << ",\"money\":" << bot->GetMoney()
        << ",\"map\":" << bot->GetMapId() << ",\"zone\":" << bot->GetZoneId()
        << ",\"health_pct\":" << uint32(bot->GetHealthPercent()) << ",\"dead\":" << (sServerFacade.UnitIsDead(bot) ? "true" : "false")
        << ",\"in_combat\":" << (sServerFacade.IsInCombat(bot) ? "true" : "false") << ",\"free_bag_slots\":" << FreeBagSlots(bot)
        << ",\"job\":\"" << c->job << "\",\"job_seq\":" << c->jobSeq << ",\"job_status\":\"" << c->status
        << "\",\"job_reason\":\"" << c->reason << "\",\"active_job\":\"" << GetActiveJob(c->guid) << "\"}";
    return { out.str() };
}

std::list<std::string> GuildBotMgr::HandleCreate(const std::string& args)
{
    std::istringstream in(args);
    std::string accountName, name, raceName, className, genderName;
    in >> accountName >> name >> raceName >> className >> genderName;
    if (className.empty())
        return { "usage: guildbot create <account> <name> <race> <class> [male|female]" };

    uint32 account = sAccountMgr.GetId(accountName);
    if (!account)
        return { "no account " + accountName };

    uint8 race = ChatHelper::parseRace(raceName);
    uint8 cls = ChatHelper::parseClass(className);
    if (!race || !cls)
        return { "unknown race or class" };

    std::string param = "name=" + name + " race=" + raceName + " class=" + className + " login=0";
    if (!genderName.empty())
        param += " gender=" + genderName;

    std::list<std::string> messages;
    ObjectGuid guid;
    createAccount = account;
    CreateBot(nullptr, param, messages, guid);
    createAccount = 0;

    if (guid)
    {
        // CreateBot leaves notes for the random bot manager's first login of a new bot (group, gear);
        // a guild character has neither.
        for (const char* note : { "create group", "create gear", "create levelup" })
            sRandomPlayerbotMgr.SetValue(guid.GetCounter(), note, 0);
    }
    return messages;
}
