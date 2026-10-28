#include "playerbot/playerbot.h"
#include "playerbot/GuildBotMgr.h"
#include "playerbot/GuildBench.h"
#include "playerbot/GuildDungeon.h"
#include "playerbot/GuildJobs.h"
#include "playerbot/GuildRuns.h"
#include "playerbot/PlayerbotAIConfig.h"
#include "playerbot/ServerFacade.h"
#include "playerbot/strategy/Engine.h"
#include "playerbot/strategy/actions/ChangeTalentsAction.h"
#include "playerbot/Talentspec.h"
#include "playerbot/TravelMgr.h"
#include "playerbot/TravelNode.h"
#include "playerbot/strategy/values/LastMovementValue.h"
#include "playerbot/strategy/values/PossibleAttackTargetsValue.h"
#include "Accounts/AccountMgr.h"
#include "AI/BaseAI/CreatureAI.h"
#include "Entities/Pet.h"
#include "Maps/GridMap.h"
#include "Chat/Chat.h"

#include <algorithm>
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
    // A party member outside, this far from its dungeon leader and no more than a few yards from where it stood this
    // long ago, is left behind where its own moves cannot get it out, such as halfway up the lift by Gnomeregan.
    constexpr float LEFT_BEHIND_DISTANCE = 100.0f;
    constexpr float LEFT_BEHIND_STILL = 5.0f;
    // A member outside while its leader is inside counts as stuck when it stays within this of one place: at the foot
    // of Gnomeregan's lifts it wandered some 25 yards to and fro.
    constexpr float LEFT_OUTSIDE_STILL = 30.0f;
    constexpr time_t LEFT_BEHIND_SECONDS = 5 * MINUTE;
    // A character that gained experience this recently is earning it, for the hourly measure.
    constexpr time_t EARNING_WINDOW_SECONDS = 5 * MINUTE;
    // The measure's counts reach the table this often, and at every change of hour or job.
    constexpr time_t METRIC_WRITE_SECONDS = 5 * MINUTE;
    // A gap longer than this between checks, such as a restart, is not counted as time played.
    constexpr time_t MEASURE_GAP_SECONDS = 30;
    // A group member farther than this from every other is on its own.
    constexpr float ALONE_DISTANCE = 60.0f;
    // A second stall this soon after a reset sends the character home instead.
    constexpr time_t RECOVERY_WINDOW_SECONDS = 30 * MINUTE;
    // A character this far below the ground at its x and y, with no floor under its feet, has fallen through the world.
    constexpr float UNDER_MAP_DEPTH = 20.0f;
    constexpr float UNDER_MAP_FLOOR_SEARCH = 10.0f;
    // A grind spot's rate counts once levelling characters have worked it this long, all together.
    constexpr uint32 SPOT_RATE_MIN_WORK = 10 * MINUTE;
    // A levelling character that has worked its spot this long, earning there less than this share of the best rate
    // known for spots worked by characters within this many levels of it, leaves the spot for LEAVE_DESTINATION_SECONDS.
    constexpr uint32 STAY_JUDGE_SECONDS = 10 * MINUTE;
    constexpr float STAY_LEAVE_SHARE = 0.4f;
    constexpr uint32 STAY_BAND_LEVELS = 3;
    // A target the character has not hurt in this long is out of its reach; it is passed over for a while.
    constexpr time_t UNREACHED_TARGET_SECONDS = 45;
    constexpr time_t IGNORE_TARGET_SECONDS = 10 * MINUTE;
    // Loot it has been after this long without taking it is out of reach, on a deck above or across water.
    constexpr time_t UNREACHED_LOOT_SECONDS = 45;
    // This many give-ups this close together mean the spot itself is out of reach: leave it for a while.
    constexpr size_t GIVE_UPS_TO_LEAVE = 3;
    constexpr time_t GIVE_UP_WINDOW_SECONDS = 10 * MINUTE;
    constexpr time_t LEAVE_DESTINATION_SECONDS = 30 * MINUTE;
    // A grind spot whose moves fail twice this close together is out of reach; failures at this many spots within
    // the window mean the character is stuck where it stands instead.
    constexpr time_t UNREACHABLE_AGAIN_SECONDS = HOUR;
    constexpr time_t UNREACHABLE_WINDOW_SECONDS = 10 * MINUTE;
    constexpr size_t UNREACHABLE_SPOTS_STUCK = 3;
    // A stuck character walks to the nearest travel node within this range.
    constexpr float STUCK_NODE_RANGE = 300.0f;
    // A route to a grind spot that ends farther than this from it does not get there.
    constexpr float NO_ROUTE_SHORT_YARDS = 200.0f;
    // In the AI's combat state this long with nothing to attack and nothing under crowd control, the fight is empty.
    constexpr time_t EMPTY_FIGHT_SECONDS = 20;
    // A dungeon leader outside that leaves this many such fights this close together goes straight to its stone,
    // for at most this long. Pyrewood's night worgen held the Shadowfang Keep party on the road below the stone.
    constexpr size_t EMPTY_FIGHTS_TO_APPROACH = 3;
    constexpr time_t EMPTY_FIGHT_WINDOW_SECONDS = 10 * MINUTE;
    constexpr time_t APPROACH_SECONDS = 10 * MINUTE;
    // A character fully under water swims up for air after this long standing still out of a fight, or after this
    // long in any case. Breath lasts 60 seconds, then drowning takes a tenth of its health every 2 seconds.
    // A dungeon leader whose travel counts as travelling while it moves less than this in this long starts its travel again.
    constexpr float STILL_TRAVEL_MOVED = 3.0f;
    constexpr time_t STILL_TRAVEL_SECONDS = 30;
    constexpr time_t UNDERWATER_IDLE_SECONDS = 20;
    constexpr time_t UNDERWATER_SECONDS = 40;
    // A ghost under the world is taken out at most this often.
    constexpr time_t GHOST_UNDER_MAP_SECONDS = MINUTE;

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
        // The tree's plain name, such as "combat": formatClass gives "combat (0/1/0)|r rogue" with chat colours.
        std::string formatted = ChatHelper::formatClass(bot, spec.highestTree());
        return formatted.substr(0, formatted.find(" ("));
    }

    // The jobs the engine knows. Anything else fails with "unknown job".
    bool IsKnownJob(const std::string& job)
    {
        return job == "idle" || job == "level" || job == "party" || job == "farm" || job == "learn" || job == "dungeon" || GuildJobs::IsJob(job);
    }

    // True for a character on a continent well below the terrain at its x and y with no floor near its feet,
    // as after a fall through the world. A cave, a cellar or a tunnel has a floor under the character, so it does not count.
    bool UnderTheMap(Player* bot)
    {
        Map* map = bot->GetMap();
        if (!map || !map->IsContinent() || bot->IsTaxiFlying() || bot->GetTransport())
            return false;
        TerrainInfo const* terrain = map->GetTerrain();
        if (!terrain)
            return false;
        const float x = bot->GetPositionX(), y = bot->GetPositionY(), z = bot->GetPositionZ();
        const float surface = terrain->GetHeightStatic(x, y, MAX_HEIGHT, false);
        if (surface <= INVALID_HEIGHT || z > surface - UNDER_MAP_DEPTH)
            return false;
        const float floor = terrain->GetHeightStatic(x, y, z, true, UNDER_MAP_FLOOR_SEARCH);
        return floor <= INVALID_HEIGHT || floor > z + 5.0f || floor < z - UNDER_MAP_FLOOR_SEARCH;
    }

    // Out from under the world to the nearest graveyard of its faction on its map, as a player's stuck request does.
    bool SendToGraveyard(Player* bot)
    {
        WorldSafeLocsEntry const* grave = bot->GetMap()->GetGraveyardManager().GetClosestGraveYard(
            bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(), bot->GetMapId(), bot->GetTeam());
        if (!grave || grave->map_id != bot->GetMapId())
            return false;
        bot->CombatStop();
        return bot->TeleportTo(grave->map_id, grave->x, grave->y, grave->z, grave->o);
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

bool GuildBotMgr::StaysWithParty(Player* bot) const
{
    if (!bot || !bot->GetGroup() || !IsGuildBot(bot))
        return false;
    const std::string job = GetActiveJob(bot->GetGUIDLow());
    return job == "dungeon" || job == "party";
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

bool GuildBotMgr::IsAlone(Player* bot) const
{
    if (!bot->GetGroup())
        return true;
    auto it = characters.find(bot->GetGUIDLow());
    return it != characters.end() && it->second.alone;
}

BotRoles GuildBotMgr::GetRole(uint32 guid) const
{
    auto it = characters.find(guid);
    return it == characters.end() ? BotRoles::BOT_ROLE_NONE : ChatHelper::parseRole(it->second.role);
}

bool GuildBotMgr::IsIgnoredTarget(Player* bot, ObjectGuid target) const
{
    auto it = ignoredTargets.find(bot->GetGUIDLow());
    if (it == ignoredTargets.end())
        return false;
    auto t = it->second.find(target);
    return t != it->second.end() && t->second > time(nullptr);
}

uint32 GuildBotMgr::GiveUps(Player* bot, ObjectGuid target) const
{
    auto it = giveUpCounts.find(bot->GetGUIDLow());
    if (it == giveUpCounts.end())
        return 0;
    auto t = it->second.find(target);
    return t == it->second.end() ? 0 : t->second;
}

bool GuildBotMgr::SpotRate(int32 entry, float& experiencePerSecond) const
{
    std::lock_guard<std::mutex> guard(spotYieldLock);
    auto it = spotYields.find(entry);
    if (it == spotYields.end() || it->second.work < SPOT_RATE_MIN_WORK)
        return false;
    experiencePerSecond = float(it->second.experience) / float(it->second.work);
    return true;
}

bool GuildBotMgr::BestSpotRate(uint32 level, int32 except, float& experiencePerSecond) const
{
    std::lock_guard<std::mutex> guard(spotYieldLock);
    bool found = false;
    for (auto& [entry, yield] : spotYields)
    {
        if (entry == except || yield.work < SPOT_RATE_MIN_WORK || yield.level + STAY_BAND_LEVELS < level || yield.level > level + STAY_BAND_LEVELS)
            continue;
        const float rate = float(yield.experience) / float(yield.work);
        if (!found || rate > experiencePerSecond)
            experiencePerSecond = rate;
        found = true;
    }
    return found;
}

uint32 GuildBotMgr::SpotCrowd(int32 entry, uint32 exceptGuid) const
{
    std::lock_guard<std::mutex> guard(spotYieldLock);
    uint32 crowd = 0;
    for (auto& [guid, spot] : spotWorkers)
        if (spot == entry && guid != exceptGuid)
            crowd++;
    return crowd;
}

void GuildBotMgr::AddSpotYield(int32 entry, uint32 work, uint32 experience, uint32 level)
{
    if (!entry || !work)
        return;
    std::lock_guard<std::mutex> guard(spotYieldLock);
    SpotYield& yield = spotYields[entry];
    yield.work += work;
    yield.experience += experience;
    yield.level = std::max(yield.level, level);
}

void GuildBotMgr::LoadSpotYields()
{
    auto result = CharacterDatabase.Query(
        "SELECT s.entry, SUM(s.work), SUM(s.work_experience), MAX(s.level) FROM guildbot_spot s "
        "JOIN guildbot_metric m ON m.hour = s.hour AND m.guid = s.guid AND m.job = 'level' "
        // Rows from before work_experience existed have it 0 with experience earned; they would read as worthless spots.
        "WHERE s.hour >= NOW() - INTERVAL 48 HOUR AND (s.work_experience > 0 OR s.experience = 0) GROUP BY s.entry");
    if (!result)
        return;
    do
    {
        Field* f = result->Fetch();
        AddSpotYield(f[0].GetInt32(), f[1].GetUInt32(), f[2].GetUInt32(), f[3].GetUInt32());
    } while (result->NextRow());
}

std::set<int32> GuildBotMgr::LeftDestinations(Player* bot) const
{
    std::set<int32> left;
    auto it = leftDestinations.find(bot->GetGUIDLow());
    if (it == leftDestinations.end())
        return left;
    const time_t now = time(nullptr);
    for (auto& [entry, until] : it->second)
        if (until > now)
            left.insert(entry);
    return left;
}

void GuildBotMgr::CheckLoot(Character& c, Player* bot)
{
    PlayerbotAI* ai = bot->GetPlayerbotAI();
    if (!ai)
        return;
    AiObjectContext* context = ai->GetAiObjectContext();
    LootObject loot = context->GetValue<LootObject>("loot target")->Get();
    if (loot.IsEmpty() || bot->IsInCombat())
    {
        lootChases.erase(c.guid);
        return;
    }

    const time_t now = time(nullptr);
    LootChase& chase = lootChases[c.guid];
    if (chase.target != loot.guid)
    {
        chase.target = loot.guid;
        chase.since = now;
        return;
    }
    if (now - chase.since < UNREACHED_LOOT_SECONDS)
        return;

    // Upstream's move to loot has no give-up: it keeps a character walking at loot it cannot reach,
    // and a leader waits for a member that is moving. Drop it, and keep it off the stack for a while.
    context->GetValue<LootObjectStack*>("available loot")->Get()->Remove(loot.guid);
    context->GetValue<LootObject>("loot target")->Set(LootObject());
    bot->GetMotionMaster()->Clear();
    bot->StopMoving();
    ignoredTargets[c.guid][loot.guid] = now + IGNORE_TARGET_SECONDS;
    // The detail names what the loot was on: a corpse's creature, or the object.
    std::string what;
    if (WorldObject* object = loot.GetWorldObject(bot))
        what = object->GetName();
    if (what.empty() && loot.guid.IsCreature())
        if (CreatureInfo const* info = ObjectMgr::GetCreatureTemplate(loot.guid.GetEntry()))
            what = info->Name;
    if (what.empty() && loot.guid.IsGameObject())
        if (GameObjectInfo const* info = ObjectMgr::GetGameObjectInfo(loot.guid.GetEntry()))
            what = info->name;
    Event(c.guid, "gave_up", "loot" + (what.empty() ? std::string() : " " + what));
    lootChases.erase(c.guid);
}

void GuildBotMgr::CheckChase(Character& c, Player* bot)
{
    PlayerbotAI* ai = bot->GetPlayerbotAI();
    Unit* victim = bot->GetVictim();
    // Its own pet is never a target to give up on: the character stops attacking it.
    if (victim && (victim->GetOwnerGuid() == bot->GetObjectGuid() || victim->GetObjectGuid() == bot->GetPetGuid()))
    {
        bot->AttackStop();
        chases.erase(c.guid);
        return;
    }
    if (!ai || !victim || !victim->IsAlive() || victim->GetObjectGuid().IsPlayer())
    {
        chases.erase(c.guid);
        return;
    }

    const time_t now = time(nullptr);
    Chase& chase = chases[c.guid];
    if (chase.target != victim->GetObjectGuid() || victim->GetHealth() < chase.health)
    {
        // A new target, or one it is hurting: the clock starts again.
        chase.target = victim->GetObjectGuid();
        chase.health = victim->GetHealth();
        chase.since = now;
        return;
    }

    // Hurt but no lower all this time, and fighting no one: a creature its blows no longer touch. A Reanimated Corpse
    // in Scholomance turns invincible at 1 health and lies down until it heals; the tank swung at one for half an
    // hour while the run stood still.
    // One the core keeps from dying counts so whatever it fights: such a corpse holding the mage as its victim
    // was kept by the rule above, and the run stood still on Lord Alexei Barov.
    const bool unkillable = victim->GetTypeId() == TYPEID_UNIT
        && static_cast<Creature*>(victim)->GetSettings().HasFlag(CreatureStaticFlags::UNKILLABLE);
    const bool untouchable = victim->GetHealth() < victim->GetMaxHealth() && (!victim->GetVictim() || unkillable);
    if (now - chase.since < UNREACHED_TARGET_SECONDS || (victim->GetHealth() < victim->GetMaxHealth() && !untouchable))
        return;

    // Untouched at full health all this time: a creature it cannot get to, in a mine shaft or on a ledge.
    // Drop it as upstream's reach action does after failed moves, which a chase that starts but leads nowhere never counts.
    AiObjectContext* context = ai->GetAiObjectContext();
    context->GetValue<ObjectGuid>("old target")->Set(victim->GetObjectGuid());
    context->GetValue<ObjectGuid>("current target")->Set(ObjectGuid());
    context->GetValue<ObjectGuid>("attack target")->Set(ObjectGuid());
    bot->SetSelectionGuid(ObjectGuid());
    bot->AttackStop();

    auto& ignored = ignoredTargets[c.guid];
    for (auto it = ignored.begin(); it != ignored.end();)
        it = it->second <= now ? ignored.erase(it) : std::next(it);
    ignored[victim->GetObjectGuid()] = now + IGNORE_TARGET_SECONDS;
    // The count outlives the ignore, so a boss given up on twice in a run is known as out of reach. Creatures of a
    // new instance have new guids, so the count is dropped once it holds many.
    auto& counts = giveUpCounts[c.guid];
    if (counts.size() > 200)
        counts.clear();
    ++counts[victim->GetObjectGuid()];

    Event(c.guid, "gave_up", victim->GetName());
    chases.erase(c.guid);

    // Several give-ups in a row: the spot it travelled to is out of reach, in a cave or behind a wall.
    // Leave it, and let the next search pass over it, or the search finds it again.
    auto& recent = giveUps[c.guid];
    recent.erase(std::remove_if(recent.begin(), recent.end(), [&](time_t t) { return now - t > GIVE_UP_WINDOW_SECONDS; }), recent.end());
    recent.push_back(now);
    if (recent.size() < GIVE_UPS_TO_LEAVE)
        return;
    recent.clear();
    // A dungeon leader's travel target inside is its next boss, which the dungeon job hands back at once:
    // leaving it costs a new route search and nothing else.
    if (GetActiveJob(c.guid) == "dungeon" && bot->GetMap()->IsDungeon())
        return;

    ai::TravelTarget* travel = context->GetValue<ai::TravelTarget*>("travel target")->Get();
    int32 entry = travel ? travel->GetEntry() : 0;
    if (!entry)
        return;
    leftDestinations[c.guid][entry] = now + LEAVE_DESTINATION_SECONDS;
    travel->SetStatus(ai::TravelStatus::TRAVEL_STATUS_EXPIRED);
    // The detail is the plain name of what the spot was for: a creature, or a game object for a negative entry.
    std::string spot = std::to_string(entry);
    if (CreatureInfo const* info = entry > 0 ? ObjectMgr::GetCreatureTemplate(entry) : nullptr)
        spot = info->Name;
    else if (GameObjectInfo const* info = entry < 0 ? ObjectMgr::GetGameObjectInfo(-entry) : nullptr)
        spot = info->name;
    Event(c.guid, "left_spot", spot);
}

void GuildBotMgr::CheckEmptyFight(Character& c, Player* bot)
{
    PlayerbotAI* ai = bot->GetPlayerbotAI();
    if (!ai || !bot->IsAlive() || !ai->IsStateActive(BotState::BOT_STATE_COMBAT) || bot->GetVictim()
        || bot->IsNonMeleeSpellCasted(false))
    {
        c.emptyFightSince = 0;
        return;
    }

    AiObjectContext* context = ai->GetAiObjectContext();
    const std::list<ObjectGuid> attackers = context->GetValue<std::list<ObjectGuid>>("attackers")->Get();
    bool paused = !context->GetValue<std::list<ObjectGuid>>("possible attack targets")->Get().empty();
    // A creature under crowd control is a fight paused, not an empty one. So is a living attacker within sight:
    // in a boss pull the possible targets can be empty for a while, out of sight round a corner or before the tank
    // has a target, and leaving then drops the tank's threat on the boss. Only attackers all dead or far off,
    // as a creature fallen under the floor, make an empty fight.
    for (ObjectGuid guid : attackers)
        if (Unit* unit = ai->GetUnit(guid))
        {
            if (PossibleAttackTargetsValue::HasBreakableCC(unit, bot) || PossibleAttackTargetsValue::HasUnBreakableCC(unit, bot))
                paused = true;
            // A boss pull is a group's: a character on its own is held by a creature it cannot attack, not paused.
            // Hthirhunt at the Dabyrie farm had four Cresting Exiles, eight levels up and out in the water, on its
            // attackers at full health for most of an hour, and could neither rest nor move on.
            if (unit->IsAlive() && bot->IsWithinDistInMap(unit, sPlayerbotAIConfig.sightDistance) && bot->GetGroup())
                paused = true;
        }
    const time_t now = time(nullptr);
    if (paused)
    {
        c.emptyFightSince = 0;
        return;
    }
    if (!c.emptyFightSince)
    {
        c.emptyFightSince = now;
        return;
    }
    if (now - c.emptyFightSince < EMPTY_FIGHT_SECONDS)
        return;
    c.emptyFightSince = 0;

    LeaveEmptyFight(c, bot, attackers);

    // A dungeon leader held outside, fight after fight: it goes straight to its stone.
    auto& exits = c.emptyFightExits;
    exits.erase(std::remove_if(exits.begin(), exits.end(), [&](time_t t) { return now - t > EMPTY_FIGHT_WINDOW_SECONDS; }), exits.end());
    exits.push_back(now);
    if (exits.size() >= EMPTY_FIGHTS_TO_APPROACH && !c.approachUntil)
        if (ai::TravelDestination* stone = sGuildDungeon.StoneDestination(bot))
        {
            exits.clear();
            c.approachUntil = now + APPROACH_SECONDS;
            Event(c.guid, "recovery", "straight to " + stone->GetTitle());
        }
}

void GuildBotMgr::LeaveEmptyFight(Character& c, Player* bot, const std::list<ObjectGuid>& attackers, bool record)
{
    PlayerbotAI* ai = bot->GetPlayerbotAI();
    AiObjectContext* context = ai->GetAiObjectContext();
    const time_t now = time(nullptr);

    // What holds it is given up on, so its attackers leave it out, and it leaves the fight where it stands.
    // Its threat goes too, as a player's does by vanishing: while it is flagged in combat it cannot eat or drink,
    // and a leader waiting on its mana would stand there.
    std::string holder;
    auto& ignored = ignoredTargets[c.guid];
    for (ObjectGuid guid : attackers)
    {
        ignored[guid] = now + IGNORE_TARGET_SECONDS;
        if (holder.empty())
            if (Unit* unit = ai->GetUnit(guid))
                holder = unit->GetName();
    }
    for (const char* name : { "old target", "current target", "attack target", "pull target" })
        context->GetValue<ObjectGuid>(name)->Set(ObjectGuid());
    bot->SetSelectionGuid(ObjectGuid());
    bot->AttackStop();
    if (bot->IsInCombat())
    {
        bot->getHostileRefManager().deleteReferences();
        if (Pet* pet = bot->GetPet())
            pet->getHostileRefManager().deleteReferences();
        bot->CombatStopWithPets(true);
    }
    context->GetValue<std::list<ObjectGuid>>("attackers")->Reset();
    context->GetValue<std::list<ObjectGuid>>("attackers", 1)->Reset();
    context->GetValue<bool>("has attackers")->Reset();
    ai->OnCombatEnded();
    if (record)
        Event(c.guid, "recovery", "left a fight with nothing to attack" + (holder.empty() ? std::string() : ": " + holder));
}

void GuildBotMgr::CheckApproach(Character& c, Player* bot)
{
    if (!c.approachUntil)
        return;
    PlayerbotAI* ai = bot->GetPlayerbotAI();
    // The stone is there while it is the leader outside its dungeon, on the way in.
    ai::TravelDestination* stone = ai && bot->IsAlive() ? sGuildDungeon.StoneDestination(bot) : nullptr;
    const time_t now = time(nullptr);
    if (!stone || now >= c.approachUntil)
    {
        c.approachUntil = 0;
        return;
    }

    AiObjectContext* context = ai->GetAiObjectContext();
    // Something it can fight comes first.
    if (bot->GetVictim() || !context->GetValue<std::list<ObjectGuid>>("possible attack targets")->Get().empty())
        return;
    // A fight with nothing to attack does not hold it now: it leaves at once, so its travel goes on.
    if (ai->IsStateActive(BotState::BOT_STATE_COMBAT) || bot->IsInCombat())
        LeaveEmptyFight(c, bot, context->GetValue<std::list<ObjectGuid>>("attackers")->Get(), false);

    ai::TravelTarget* travel = context->GetValue<ai::TravelTarget*>("travel target")->Get();
    if (travel->GetDestination() == stone && travel->IsForced() && travel->GetStatus() <= ai::TravelStatus::TRAVEL_STATUS_TRAVEL)
        return;
    std::list<uint8> nearest = { 0 };
    if (WorldPosition* point = stone->GetNextPoint(WorldPosition(bot), nearest, true))
    {
        travel->SetTarget(stone, point);
        travel->SetForced(true);
        travel->SetStatus(ai::TravelStatus::TRAVEL_STATUS_READY);
        travel->SetExpireIn(uint32(c.approachUntil - now) * IN_MILLISECONDS);
    }
}

void GuildBotMgr::CheckUnreachable(Character& c, Player* bot)
{
    if (c.job != "level" || c.status != "working" || !bot->IsAlive())
        return;
    PlayerbotAI* ai = bot->GetPlayerbotAI();
    if (!ai)
        return;
    ai::TravelTarget* travel = ai->GetAiObjectContext()->GetValue<ai::TravelTarget*>("travel target")->Get();
    if (!travel)
        return;

    // The travel target keeps its failed-move count from target to target, as upstream has it and the dungeon
    // leaders need. For a levelling character the count starts again with each grind spot: carried over, a few failures
    // put every later spot in cooldown at its first failed move, and the Gadgetzan trio set spot after spot aside in
    // experiment 18 while walking to Gadgetzan's own node.
    const bool grinding = dynamic_cast<ai::GrindTravelDestination*>(travel->GetDestination()) != nullptr;
    if (grinding && travel->GetStatus() == ai::TravelStatus::TRAVEL_STATUS_TRAVEL && travel->GetEntry() != c.retrySpot)
    {
        travel->SetRetry(true, 0);
        c.retrySpot = travel->GetEntry();
    }

    // Cooling down after its moves failed: no route there, a lift it does not take or a shore it cannot climb.
    // Without this the same spot wins again at the next choice, as Hfortmage's Lost One Fisherman did for a quarter hour.
    if (travel->GetStatus() != ai::TravelStatus::TRAVEL_STATUS_COOLDOWN)
    {
        c.unreachableHandled = 0;
        return;
    }
    if (!travel->IsMaxRetry(true) || !grinding)
        return;
    const int32 entry = travel->GetEntry();
    // Each cooldown counts once, not on every check while it lasts.
    if (!entry || entry == c.unreachableHandled)
        return;
    c.unreachableHandled = entry;
    // Counted: the next spot starts from no failed moves, whatever this one comes to.
    travel->SetRetry(true, 0);
    c.retrySpot = 0;
    const time_t now = time(nullptr);
    auto& left = leftDestinations[c.guid];
    auto it = left.find(entry);
    if (it != left.end() && it->second > now)
        return;
    std::string spot = std::to_string(entry);
    if (CreatureInfo const* info = ObjectMgr::GetCreatureTemplate(entry))
        spot = info->Name;

    // No route the travel map knows for this character: the spot is out of its reach as things stand, and its failed
    // moves are no sign of being stuck. Dustwallow from Gadgetzan without the Great Lift is such a spot; Afortmage
    // picked one after another all hour in experiment 16. Set aside at once, without strikes.
    // A route that stops short of the spot is none: Afortmage's from Gadgetzan to Dustwallow ran 10 points and
    // ended 4476 yards from it.
    WorldPosition* target = travel->GetPosition();
    TravelPath route = target ? sTravelNodeMap.getFullPath(WorldPosition(bot), *target, bot) : TravelPath();
    if (target && (route.empty() || route.getBack().distance(*target) > NO_ROUTE_SHORT_YARDS))
    {
        left[entry] = now + LEAVE_DESTINATION_SECONDS;
        travel->SetStatus(ai::TravelStatus::TRAVEL_STATUS_EXPIRED);
        Event(c.guid, "left_spot", spot + ": no route there");
        return;
    }

    // Moves along known routes to several spots failing close together say the character is stuck where it stands:
    // Afortdru in Feralas set 27 spots aside in six minutes. It forgets its path and walks to the nearest travel node,
    // where the routes start, before it chooses again.
    auto& times = c.unreachableTimes;
    times.erase(std::remove_if(times.begin(), times.end(), [&](time_t t) { return now - t > UNREACHABLE_WINDOW_SECONDS; }), times.end());
    times.push_back(now);
    if (times.size() >= UNREACHABLE_SPOTS_STUCK)
    {
        if (now - c.stuckNotedAt > UNREACHABLE_WINDOW_SECONDS)
        {
            c.stuckNotedAt = now;
            times.clear();
            ai->GetAiObjectContext()->GetValue<ai::LastMovement&>("last movement")->Get().clear();
            ai->StopMoving();
            bot->GetMotionMaster()->Clear();
            std::string where = "no way from here";
            // Never a move for a character on its way to another map: the path build asserts without one.
            TravelNode* node = bot->IsInWorld() && !bot->IsBeingTeleported() ? sTravelNodeMap.getNode(WorldPosition(bot), nullptr, STUCK_NODE_RANGE) : nullptr;
            if (node && node->getPosition()->getMapId() == bot->GetMapId())
            {
                WorldPosition* at = node->getPosition();
                bot->GetMotionMaster()->MovePoint(0, at->getX(), at->getY(), at->getZ(), FORCED_MOVEMENT_RUN);
                where += ", to " + node->getName();
            }
            travel->SetStatus(ai::TravelStatus::TRAVEL_STATUS_EXPIRED);
            Event(c.guid, "recovery", where);
        }
        return;
    }

    // A spot is out of reach on its second failure within the hour; one failure can be a bad path once.
    auto failed = c.unreachableAt.find(entry);
    const bool again = failed != c.unreachableAt.end() && now - failed->second <= UNREACHABLE_AGAIN_SECONDS;
    c.unreachableAt[entry] = now;
    if (!again)
        return;

    left[entry] = now + LEAVE_DESTINATION_SECONDS;
    travel->SetStatus(ai::TravelStatus::TRAVEL_STATUS_EXPIRED);
    Event(c.guid, "left_spot", spot + ": no way there");
}

void GuildBotMgr::CheckStay(Character& c, Player* bot)
{
    if (c.job != "level" || c.status != "working" || !bot->IsAlive() || !c.stay.entry || c.stay.work < STAY_JUDGE_SECONDS)
        return;
    PlayerbotAI* ai = bot->GetPlayerbotAI();
    if (!ai)
        return;
    ai::TravelTarget* travel = ai->GetAiObjectContext()->GetValue<ai::TravelTarget*>("travel target")->Get();
    if (!travel || travel->IsForced() || travel->GetEntry() != c.stay.entry
        || !dynamic_cast<ai::GrindTravelDestination*>(travel->GetDestination()))
        return;

    // What this spot gives this character, against the best spot near its level that others have worked.
    float best;
    if (!BestSpotRate(bot->GetLevel(), c.stay.entry, best) || best <= 0.0f)
        return;
    const float rate = float(c.stay.experience) / float(c.stay.work);
    if (rate >= best * STAY_LEAVE_SHARE)
        return;

    const time_t now = time(nullptr);
    const int32 entry = c.stay.entry;
    leftDestinations[c.guid][entry] = now + LEAVE_DESTINATION_SECONDS;
    travel->SetStatus(ai::TravelStatus::TRAVEL_STATUS_EXPIRED);
    c.stay = Character::Stay();
    std::string spot = std::to_string(entry);
    if (CreatureInfo const* info = entry > 0 ? ObjectMgr::GetCreatureTemplate(entry) : nullptr)
        spot = info->Name;
    Event(c.guid, "left_spot", spot + ": " + std::to_string(uint32(rate * HOUR)) + " experience an hour there, the best near its level "
        + std::to_string(uint32(best * HOUR)));
}

void GuildBotMgr::CheckGhostUnderMap(Character& c, Player* bot)
{
    if (!bot->HasFlag(PLAYER_FLAGS, PLAYER_FLAGS_GHOST))
        return;
    const time_t now = time(nullptr);
    if (now - c.underMapAt < GHOST_UNDER_MAP_SECONDS || !UnderTheMap(bot))
        return;
    c.underMapAt = now;
    WorldLocation grave;
    bool out;
    if (GuildDungeon::CorpseGraveyard(bot, grave))
    {
        bot->GetMotionMaster()->Clear();
        out = bot->TeleportTo(grave.mapid, grave.coord_x, grave.coord_y, grave.coord_z, grave.orientation);
    }
    else
        out = SendToGraveyard(bot);
    if (out)
        Event(c.guid, "recovery", "under the map");
}

void GuildBotMgr::CheckStillTravel(Character& c, Player* bot)
{
    PlayerbotAI* ai = bot->GetPlayerbotAI();
    ai::TravelTarget* travel = ai ? ai->GetAiObjectContext()->GetValue<ai::TravelTarget*>("travel target")->Get() : nullptr;
    const bool still = travel && GetActiveJob(c.guid) == "dungeon" && bot->IsAlive() && bot->IsInWorld()
        && !bot->IsBeingTeleported() && !bot->IsInCombat() && !bot->IsMoving()
        && travel->GetStatus() == ai::TravelStatus::TRAVEL_STATUS_TRAVEL
        && ai->GetAiObjectContext()->GetValue<ai::LastMovement&>("last movement")->Get().lastPath.empty();
    const time_t now = time(nullptr);
    if (!still || (c.stillTravelSince && bot->GetDistance(c.stillTravelX, c.stillTravelY, bot->GetPositionZ()) > STILL_TRAVEL_MOVED))
    {
        c.stillTravelSince = still ? now : 0;
        c.stillTravelX = bot->GetPositionX();
        c.stillTravelY = bot->GetPositionY();
        return;
    }
    if (!c.stillTravelSince)
    {
        c.stillTravelSince = now;
        c.stillTravelX = bot->GetPositionX();
        c.stillTravelY = bot->GetPositionY();
        return;
    }
    if (now - c.stillTravelSince < STILL_TRAVEL_SECONDS)
        return;
    // Its moves report success and nothing moves it: the leave in Stratholme and Blackfathom Deeps, and the way to
    // Ambassador Flamelash in Blackrock Depths, stood so until the run failed. A fresh travel builds a fresh path.
    c.stillTravelSince = 0;
    travel->SetStatus(ai::TravelStatus::TRAVEL_STATUS_EXPIRED);
    ai->GetAiObjectContext()->GetValue<ai::LastMovement&>("last movement")->Get().clear();
    ai->StopMoving();
    Event(c.guid, "recovery", "travel stood still");
}

void GuildBotMgr::CheckBreath(Character& c, Player* bot)
{
    if (!bot->IsAlive() || !bot->IsInWorld() || bot->IsBeingTeleported() || !bot->IsUnderwater()
        || bot->HasAuraType(SPELL_AURA_WATER_BREATHING))
    {
        c.underwaterSince = 0;
        c.surfacing = false;
        return;
    }
    const time_t now = time(nullptr);
    if (!c.underwaterSince)
    {
        c.underwaterSince = now;
        return;
    }
    // Upstream's movement knows no breath. In Blackfathom Deeps a leader whose travel stopped 15 yards down, by the
    // way to Old Serra'kis, stood there out of combat and ate as it drowned.
    const time_t under = now - c.underwaterSince;
    const bool idle = !bot->IsMoving() && !bot->IsInCombat();
    if (under < UNDERWATER_SECONDS && (!idle || under < UNDERWATER_IDLE_SECONDS))
        return;
    const float surface = bot->GetTerrain()->GetWaterLevel(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ());
    // No water level here, as with no liquid data: nothing to swim up to.
    if (surface <= bot->GetPositionZ())
        return;
    if (bot->getStandState() != UNIT_STAND_STATE_STAND)
        bot->SetStandState(UNIT_STAND_STATE_STAND);
    bot->GetMotionMaster()->Clear();
    bot->GetMotionMaster()->MovePoint(0, bot->GetPositionX(), bot->GetPositionY(), surface, FORCED_MOVEMENT_RUN);
    if (!c.surfacing)
        Event(c.guid, "recovery", "up for air");
    c.surfacing = true;
}

void GuildBotMgr::CheckLeftBehind(Character& c, Player* bot)
{
    Group* group = bot->GetGroup();
    Player* leader = group ? sObjectMgr.GetPlayer(group->GetLeaderGuid()) : nullptr;
    MapEntry const* map = sMapStore.LookupEntry(bot->GetMapId());
    // Left outside: the leader is inside its dungeon and the member in the open world. In Gnomeregan a member that came
    // out with its leader by a wrong turn could not take the lifts back up to the stone, and the leader held inside
    // by the Train Depot entrance waiting for it.
    MapEntry const* leaderMap = leader && leader->IsInWorld() ? sMapStore.LookupEntry(leader->GetMapId()) : nullptr;
    const bool outside = leaderMap && leaderMap->IsDungeon() && leader->GetMapId() != bot->GetMapId();
    const bool behind = c.job == "party" && leader && leader != bot && leader->IsInWorld() && IsGuildBot(leader)
        && GetActiveJob(leader->GetGUIDLow()) == "dungeon" && bot->IsAlive() && !bot->IsInCombat() && map && !map->IsDungeon()
        && (outside || (leader->GetMapId() == bot->GetMapId() && bot->GetDistance(leader) > LEFT_BEHIND_DISTANCE));
    const time_t now = time(nullptr);
    if (!behind)
    {
        c.stillSince = 0;
        c.behindReset = false;
        return;
    }
    if (!c.stillSince || bot->GetDistance(c.stillX, c.stillY, c.stillZ) > (outside ? LEFT_OUTSIDE_STILL : LEFT_BEHIND_STILL))
    {
        c.stillX = bot->GetPositionX();
        c.stillY = bot->GetPositionY();
        c.stillZ = bot->GetPositionZ();
        c.stillSince = now;
        return;
    }
    if (now - c.stillSince < LEFT_BEHIND_SECONDS)
        return;
    c.stillSince = now;

    // As the stall recovery does for a levelling character: under the map, the nearest graveyard; else a reset first,
    // and if that does not get it moving, home by hearthstone, which for a party is by its dungeon's stone.
    PlayerbotAI* ai = bot->GetPlayerbotAI();
    if (!ai)
        return;
    if (UnderTheMap(bot) && SendToGraveyard(bot))
    {
        ai->Reset(true);
        Event(c.guid, "recovery", "left behind, under the map");
    }
    // Left outside, the hearthstone comes first: it takes the member to its dungeon's stone, which takes it in.
    else if (outside && ai->DoSpecificAction("hearthstone", ai::Event(), true))
    {
        c.behindReset = false;
        Event(c.guid, "recovery", "left outside, hearthstone");
    }
    else if (!c.behindReset)
    {
        ai->Reset(true);
        c.behindReset = true;
        Event(c.guid, "recovery", "left behind, reset");
    }
    else
    {
        const bool home = ai->DoSpecificAction("hearthstone", ai::Event(), true);
        c.behindReset = false;
        Event(c.guid, "recovery", home ? "left behind, hearthstone" : "left behind, hearthstone not ready");
    }
}

uint8 GuildBotMgr::GetTankOrder(uint32 guid) const
{
    auto it = tankOrders.find(guid);
    return it == tankOrders.end() ? 0 : it->second;
}

std::string GuildBotMgr::GetDungeonOrder(uint32 guid) const
{
    auto it = characters.find(guid);
    if (it == characters.end())
        return "";
    if (it->second.job == "dungeon")
        return ArgValue(it->second, "name");
    if (it->second.job != "party")
        return "";
    std::string leaderName = ArgValue(it->second, "leader");
    if (!normalizePlayerName(leaderName))
        return "";
    for (const auto& [leaderGuid, leader] : characters)
        if (leader.name == leaderName && leader.job == "dungeon")
            return ArgValue(leader, "name");
    return "";
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
    if (GetFarmOrder(guid, farmItem, amount) && farmItem == itemId)
        return true;
    // What a kit or deposit order names, while it runs.
    auto it = characters.find(guid);
    return it != characters.end() && GuildJobs::KeepsForJob(GetActiveJob(guid), it->second.jobArgs, itemId);
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

    if (job == "dungeon")
    {
        // Inside, the leader holds by the entrance while the party follows it in.
        if (sGuildDungeon.IsHolding(player))
        {
            engine->addStrategy("stay");
            return;
        }
        // On the way, a forced travel target takes it to the dungeon once it has sold and repaired; no grinding.
        // Inside, the travel target is the next boss, and at the boss it fights: grind, which starts no fight
        // on the way to a destination that is not a grind spot, only there.
        engine->addStrategies("collision", "travel", "rpg", "free", "guild level", NULL);
        if (MapEntry const* map = sMapStore.LookupEntry(player->GetMapId()); map && map->IsDungeon())
            engine->addStrategy("grind");
        engine->removeStrategy("rpg quest");
        engine->removeStrategy("rpg explore");
        engine->removeStrategy("rpg player");
        engine->removeStrategy("rpg bg");
        engine->removeStrategy("rpg guild");
        return;
    }

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

    if (auto it = characters.find(player->GetGUIDLow()); it != characters.end() && GuildJobs::Travels(job, it->second.jobArgs))
    {
        // Pay at a counter, kit and deposit: a forced travel target, the travel named "guild job", takes it to the
        // Guild Master or the emissary its order names, the vaultkeeper, or the vendor kit buys from. No rpg and
        // no free: it makes no trips to sell or repair on the way, which could spend the gold it carries.
        engine->addStrategies("collision", "travel", "guild level", NULL);
        return;
    }

    // idle, and GuildJobs' pay without at, reset and craft: stand where it is.
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
        LoadAssignments();
        LoginLogout();
        ApplyOrders();
        CheckJobs();
        sGuildRuns.Sweep();
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
        "ALTER TABLE guildbot_state ADD COLUMN IF NOT EXISTS tank_order TINYINT UNSIGNED NOT NULL DEFAULT 0");

    // Who tanks, in order, for the group a character is in; keyed by character, so the player's own counts too.
    CharacterDatabase.DirectExecute(
        "CREATE TABLE IF NOT EXISTS guildbot_assignment ("
        " guid INT UNSIGNED NOT NULL PRIMARY KEY,"
        " tank_order TINYINT UNSIGNED NOT NULL DEFAULT 0,"
        " updated_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP"
        ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4");

    CharacterDatabase.DirectExecute(
        "CREATE TABLE IF NOT EXISTS guildbot_event ("
        " id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT PRIMARY KEY,"
        " guid INT UNSIGNED NOT NULL,"
        " at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,"
        " kind VARCHAR(32) NOT NULL,"
        " detail VARCHAR(255) NOT NULL DEFAULT '',"
        " KEY guid (guid), KEY at (at)"
        ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4");

    // The hourly measure of how much of its time a character spends earning experience.
    CharacterDatabase.DirectExecute(
        "CREATE TABLE IF NOT EXISTS guildbot_metric ("
        " hour TIMESTAMP NOT NULL DEFAULT '2000-01-01 00:00:00',"
        " guid INT UNSIGNED NOT NULL,"
        " job VARCHAR(16) NOT NULL,"
        " seconds INT UNSIGNED NOT NULL DEFAULT 0,"
        " earning INT UNSIGNED NOT NULL DEFAULT 0,"
        " experience INT UNSIGNED NOT NULL DEFAULT 0,"
        " deaths INT UNSIGNED NOT NULL DEFAULT 0,"
        " PRIMARY KEY (hour, guid, job)"
        ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4");
    for (const char* column : ACTIVITY_COLUMNS)
        CharacterDatabase.DirectPExecute("ALTER TABLE guildbot_metric ADD COLUMN IF NOT EXISTS %s INT UNSIGNED NOT NULL DEFAULT 0", column);

    // The measure by grind spot: per hour, character and spot, the seconds on the way and there, and what it gave.
    CharacterDatabase.DirectExecute(
        "CREATE TABLE IF NOT EXISTS guildbot_spot ("
        " hour TIMESTAMP NOT NULL DEFAULT '2000-01-01 00:00:00',"
        " guid INT UNSIGNED NOT NULL,"
        " entry INT NOT NULL,"
        " level TINYINT UNSIGNED NOT NULL DEFAULT 0,"
        " visits INT UNSIGNED NOT NULL DEFAULT 0,"
        " travel INT UNSIGNED NOT NULL DEFAULT 0,"
        " work INT UNSIGNED NOT NULL DEFAULT 0,"
        " experience INT UNSIGNED NOT NULL DEFAULT 0,"
        " money INT UNSIGNED NOT NULL DEFAULT 0,"
        " deaths INT UNSIGNED NOT NULL DEFAULT 0,"
        " PRIMARY KEY (hour, guid, entry), KEY entry (entry)"
        ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4");
    // The experience earned at the spot itself, which its rate is made of; experience also counts the fights on the way.
    CharacterDatabase.DirectExecute(
        "ALTER TABLE guildbot_spot ADD COLUMN IF NOT EXISTS work_experience INT UNSIGNED NOT NULL DEFAULT 0 AFTER experience");

    sGuildRuns.EnsureTables();
    LoadSpotYields();
    tablesReady = true;
}

GuildBotMgr::Activity GuildBotMgr::ActivityOf(Player* bot, int32& spot)
{
    spot = 0;
    PlayerbotAI* ai = bot->GetPlayerbotAI();
    ai::TravelTarget* target = ai ? ai->GetAiObjectContext()->GetValue<ai::TravelTarget*>("travel target")->Get() : nullptr;
    ai::TravelDestination* destination = target ? target->GetDestination() : nullptr;
    if (destination && uint32(destination->GetPurpose()) & uint32(ai::TravelDestinationPurpose::Grind) && destination->GetEntry() > 0)
        spot = destination->GetEntry();

    if (!bot->IsAlive())
        return ACTIVITY_DEAD;
    if (bot->IsInCombat())
        return ACTIVITY_FIGHT;
    if (bot->getStandState() == UNIT_STAND_STATE_SIT)
        return ACTIVITY_REST;

    if (!destination)
        return ACTIVITY_OTHER;

    const uint32 purpose = uint32(destination->GetPurpose());
    const uint32 errands = uint32(ai::TravelDestinationPurpose::GenericRpg) | uint32(ai::TravelDestinationPurpose::Trainer)
        | uint32(ai::TravelDestinationPurpose::Repair) | uint32(ai::TravelDestinationPurpose::Vendor)
        | uint32(ai::TravelDestinationPurpose::AH) | uint32(ai::TravelDestinationPurpose::Mail)
        | uint32(ai::TravelDestinationPurpose::Bank);
    const ai::TravelStatus status = target->GetStatus();
    const bool going = status == ai::TravelStatus::TRAVEL_STATUS_TRAVEL || status == ai::TravelStatus::TRAVEL_STATUS_READY;
    if (purpose & errands && (going || status == ai::TravelStatus::TRAVEL_STATUS_WORK))
        return ACTIVITY_ERRAND;
    if (purpose & uint32(ai::TravelDestinationPurpose::Grind))
    {
        if (going)
            return ACTIVITY_TRAVEL;
        if (status == ai::TravelStatus::TRAVEL_STATUS_WORK)
            return ACTIVITY_SEEK;
    }
    return ACTIVITY_OTHER;
}

void GuildBotMgr::Measure(Character& c, Player* bot, bool died)
{
    const time_t now = time(nullptr);
    const uint32 level = bot->GetLevel(), xp = bot->GetUInt32Value(PLAYER_XP);

    // Experience gained since the last check, across a level up.
    uint32 gained = 0;
    if (c.xpLevel && level == c.xpLevel && xp > c.xpValue)
        gained = xp - c.xpValue;
    else if (c.xpLevel && level == c.xpLevel + 1)
        gained = sObjectMgr.GetXPForLevel(c.xpLevel) - std::min(c.xpValue, sObjectMgr.GetXPForLevel(c.xpLevel)) + xp;
    if (gained || (c.xpLevel && level > c.xpLevel))
        c.xpAt = now;
    c.xpLevel = level;
    c.xpValue = xp;

    const std::string job = c.status == "working" ? c.job : "idle";
    const time_t hour = now - now % HOUR;
    if (c.metricHour && (hour != c.metricHour || job != c.metricJob))
        WriteMetric(c);
    if (!c.metricHour)
    {
        c.metricHour = hour;
        c.metricJob = job;
    }

    const time_t step = c.measuredAt ? now - c.measuredAt : 0;
    c.measuredAt = now;
    if (step > 0 && step <= MEASURE_GAP_SECONDS)
    {
        c.seconds += uint32(step);
        if (c.xpAt && now - c.xpAt < EARNING_WINDOW_SECONDS)
            c.earning += uint32(step);
        int32 spot = 0;
        const Activity activity = ActivityOf(bot, spot);
        c.activity[activity] += uint32(step);
        if (spot && (activity == ACTIVITY_TRAVEL || activity == ACTIVITY_SEEK || activity == ACTIVITY_FIGHT || activity == ACTIVITY_REST || activity == ACTIVITY_DEAD))
        {
            Character::SpotCount& count = c.spots[spot];
            if (spot != c.lastSpot)
                count.visits++;
            if (activity == ACTIVITY_TRAVEL)
                count.travel += uint32(step);
            else
                count.work += uint32(step);
            count.experience += gained;
            if (activity != ACTIVITY_TRAVEL)
                count.workExperience += gained;
            // Coin looted there; selling happens on errands, which are not at a spot.
            if (bot->GetMoney() > c.lastMoney && activity != ACTIVITY_TRAVEL)
                count.money += bot->GetMoney() - c.lastMoney;
            if (died)
                count.deaths++;
            // The stay: a new one when it works a different spot; a gap with no spot, between renewals, continues it.
            if (spot != c.stay.entry)
            {
                c.stay = Character::Stay{ spot, 0, 0 };
                std::lock_guard<std::mutex> guard(spotYieldLock);
                if (c.job == "level")
                    spotWorkers[c.guid] = spot;
                else
                    spotWorkers.erase(c.guid);
            }
            if (activity != ACTIVITY_TRAVEL)
            {
                c.stay.work += uint32(step);
                c.stay.experience += gained;
            }
        }
        c.lastSpot = spot;
    }
    c.lastMoney = bot->GetMoney();
    c.experience += gained;
    if (died)
        c.deaths++;

    if (c.seconds >= METRIC_WRITE_SECONDS)
        WriteMetric(c);
}

void GuildBotMgr::WriteMetric(Character& c)
{
    if (c.metricHour && (c.seconds || c.experience || c.deaths))
    {
        std::ostringstream columns, values, updates;
        for (uint32 i = 0; i < ACTIVITIES; ++i)
        {
            columns << ", " << ACTIVITY_COLUMNS[i];
            values << ", " << c.activity[i];
            updates << ", " << ACTIVITY_COLUMNS[i] << " = " << ACTIVITY_COLUMNS[i] << " + VALUES(" << ACTIVITY_COLUMNS[i] << ")";
        }
        CharacterDatabase.PExecute(
            "INSERT INTO guildbot_metric (hour, guid, job, seconds, earning, experience, deaths%s) "
            "VALUES (FROM_UNIXTIME(%u), %u, '%s', %u, %u, %u, %u%s) "
            "ON DUPLICATE KEY UPDATE seconds = seconds + VALUES(seconds), earning = earning + VALUES(earning), "
            "experience = experience + VALUES(experience), deaths = deaths + VALUES(deaths)%s",
            columns.str().c_str(), uint32(c.metricHour), c.guid, Escape(c.metricJob).c_str(),
            c.seconds, c.earning, c.experience, c.deaths, values.str().c_str(), updates.str().c_str());
    }
    if (c.metricHour)
        for (auto& [entry, count] : c.spots)
        {
            if (c.metricJob == "level")
                AddSpotYield(entry, count.work, count.workExperience, c.xpLevel);
            CharacterDatabase.PExecute(
                "INSERT INTO guildbot_spot (hour, guid, entry, level, visits, travel, work, experience, work_experience, money, deaths) "
                "VALUES (FROM_UNIXTIME(%u), %u, %d, %u, %u, %u, %u, %u, %u, %u, %u) "
                "ON DUPLICATE KEY UPDATE level = GREATEST(level, VALUES(level)), visits = visits + VALUES(visits), "
                "travel = travel + VALUES(travel), work = work + VALUES(work), experience = experience + VALUES(experience), "
                "work_experience = work_experience + VALUES(work_experience), money = money + VALUES(money), deaths = deaths + VALUES(deaths)",
                uint32(c.metricHour), c.guid, entry, c.xpLevel, count.visits, count.travel, count.work, count.experience, count.workExperience,
                count.money, count.deaths);
        }
    c.spots.clear();
    c.seconds = c.earning = c.experience = c.deaths = 0;
    std::fill(std::begin(c.activity), std::end(c.activity), 0);
    c.metricHour = 0;
}

std::list<std::string> GuildBotMgr::HandleMetrics(const std::string& args)
{
    uint32 hours = uint32(atoi(Trim(args).c_str()));
    if (!hours)
        hours = 12;

    std::list<std::string> out = { "levelling characters, by hour: earning experience / time played, experience, deaths, characters" };
    std::ostringstream sums;
    for (const char* column : ACTIVITY_COLUMNS)
        sums << ", SUM(" << column << ")";
    auto byHour = CharacterDatabase.PQuery(
        "SELECT DATE_FORMAT(hour, '%%Y-%%m-%%d %%H:00'), SUM(earning), SUM(seconds), SUM(experience), SUM(deaths), COUNT(DISTINCT guid)%s "
        "FROM guildbot_metric WHERE job = 'level' AND hour >= NOW() - INTERVAL %u HOUR GROUP BY hour ORDER BY hour", sums.str().c_str(), hours);
    if (byHour)
    {
        do
        {
            Field* f = byHour->Fetch();
            uint32 earning = f[1].GetUInt32(), seconds = f[2].GetUInt32();
            std::ostringstream line;
            line << f[0].GetCppString() << "  " << (seconds ? earning * 100 / seconds : 0) << "% of " << seconds / MINUTE << " min, "
                 << f[3].GetUInt32() << " xp, " << f[4].GetUInt32() << " deaths, " << f[5].GetUInt32() << " characters";
            out.push_back(line.str());
            // The split by activity; hours counted before it existed show none.
            uint32 split = 0;
            for (uint32 i = 0; i < ACTIVITIES; ++i)
                split += f[6 + i].GetUInt32();
            if (split)
            {
                std::ostringstream activities;
                activities << "    of the split " << split / MINUTE << " min:";
                for (uint32 i = 0; i < ACTIVITIES; ++i)
                    activities << " " << ACTIVITY_COLUMNS[i] << " " << f[6 + i].GetUInt32() * 100 / split << "%";
                out.push_back(activities.str());
            }
        } while (byHour->NextRow());
    }

    out.push_back("by character, the same hours:");
    auto byCharacter = CharacterDatabase.PQuery(
        "SELECT c.name, SUM(m.earning), SUM(m.seconds), SUM(m.experience), SUM(m.deaths) "
        "FROM guildbot_metric m JOIN characters c ON c.guid = m.guid "
        "WHERE m.job = 'level' AND m.hour >= NOW() - INTERVAL %u HOUR GROUP BY m.guid ORDER BY SUM(m.earning) / SUM(m.seconds)", hours);
    if (byCharacter)
    {
        do
        {
            Field* f = byCharacter->Fetch();
            uint32 earning = f[1].GetUInt32(), seconds = f[2].GetUInt32();
            std::ostringstream line;
            line << "  " << f[0].GetCppString() << "  " << (seconds ? earning * 100 / seconds : 0) << "% of " << seconds / MINUTE << " min, "
                 << f[3].GetUInt32() << " xp, " << f[4].GetUInt32() << " deaths";
            out.push_back(line.str());
        } while (byCharacter->NextRow());
    }
    return out;
}

std::list<std::string> GuildBotMgr::HandleSpots(const std::string& args)
{
    uint32 hours = uint32(atoi(Trim(args).c_str()));
    if (!hours)
        hours = 12;

    std::list<std::string> out = { "grind spots, by experience per hour there: levels, visits, minutes walking per visit, minutes there, xp, gold, deaths" };
    auto result = CharacterDatabase.PQuery(
        "SELECT t.Name, MIN(s.level), MAX(s.level), SUM(s.visits), SUM(s.travel), SUM(s.work), SUM(s.experience), SUM(s.money), SUM(s.deaths) "
        "FROM guildbot_spot s LEFT JOIN mangos.creature_template t ON t.Entry = s.entry "
        "WHERE s.hour >= NOW() - INTERVAL %u HOUR GROUP BY s.entry HAVING SUM(s.work) >= 60 "
        "ORDER BY SUM(s.experience) / SUM(s.work) DESC", hours);
    if (!result)
        return out;
    do
    {
        Field* f = result->Fetch();
        uint32 visits = f[3].GetUInt32(), travel = f[4].GetUInt32(), work = f[5].GetUInt32(), xp = f[6].GetUInt32();
        std::ostringstream line;
        line << "  " << (f[0].IsNULL() ? std::string("?") : f[0].GetCppString()) << "  L" << f[1].GetUInt32() << "-" << f[2].GetUInt32()
             << ", " << (work ? uint64(xp) * HOUR / work : 0) << " xp/h, " << visits << " visits, "
             << (visits ? travel / visits / MINUTE : 0) << " min walk/visit, " << work / MINUTE << " min there, "
             << xp << " xp, " << f[7].GetUInt32() / 10000 << "g, " << f[8].GetUInt32() << " deaths";
        out.push_back(line.str());
    } while (result->NextRow());
    return out;
}

void GuildBotMgr::LoadOrders()
{
    auto result = CharacterDatabase.PQuery(
        "SELECT g.guid, c.account, c.name, g.enabled, g.job, g.job_args, g.job_seq, g.role, "
        "s.job_seq, s.job_status, s.job_reason "
        "FROM guildbot_character g JOIN characters c ON c.guid = g.guid LEFT JOIN guildbot_state s ON s.guid = g.guid");

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
            {
                c.offlineSince = 0;
                // An order already done or failed before a restart stays so, or the job would run again.
                std::string status = f[9].GetCppString();
                if (!f[8].IsNULL() && f[8].GetUInt32() == c.jobSeq && (status == "done" || status == "failed"))
                {
                    c.status = status;
                    c.reason = f[10].GetCppString();
                }
            }
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
        c.runsDone = 0;
        sGuildDungeon.Stop(guid);
        c.progressAt = 0;
        c.stalled = false;
        uint32 farmItem = uint32(atoi(ArgValue(c, "item").c_str()));
        uint32 learnSkill = ProfessionSkill(ArgValue(c, "profession"));
        std::string dungeonError = c.job == "dungeon" ? GuildDungeon::Validate(ArgValue(c, "name")) : "";
        if (!dungeonError.empty())
        {
            c.status = "failed";
            c.reason = dungeonError;
            Event(guid, "job_failed", c.reason);
        }
        else if (c.job == "farm" && (!farmItem || !sObjectMgr.GetItemPrototype(farmItem)))
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

// The zone, or 0 for a character between maps, which has no terrain to ask.
uint32 GuildBotMgr::ZoneOf(Player* bot)
{
    return bot->IsInWorld() && !bot->IsBeingTeleportedFar() ? bot->GetZoneId() : 0;
}

void GuildBotMgr::CheckJobs()
{
    for (auto& [guid, c] : characters)
    {
        Player* bot = GetPlayerBot(guid);
        // Between maps, after a teleport to another map, a character has no map, and what asks for one asserts.
        if (!bot || !bot->IsInWorld() || bot->IsBeingTeleportedFar())
            continue;

        if (c.lastLevel && bot->GetLevel() > c.lastLevel)
            Event(guid, "level", std::to_string(bot->GetLevel()));
        c.lastLevel = bot->GetLevel();

        ApplyRole(c, bot);
        CheckChase(c, bot);
        CheckLeftBehind(c, bot);
        CheckEmptyFight(c, bot);
        CheckApproach(c, bot);
        CheckGhostUnderMap(c, bot);
        CheckBreath(c, bot);
        CheckStillTravel(c, bot);
        CheckStay(c, bot);
        CheckUnreachable(c, bot);
        CheckLoot(c, bot);

        bool dead = sServerFacade.UnitIsDead(bot);
        if (dead && !c.dead)
            Event(guid, "died", std::to_string(bot->GetZoneId()));
        Measure(c, bot, dead && !c.dead);
        c.dead = dead;
        sGuildRuns.Observe(bot);
        GuildDungeon::NoteAlive(bot);
        GuildDungeon::FollowByStone(bot);

        // Cut off from its group, say outside a dungeon its party is in: it plays as a solo character until it is back.
        bool alone = false;
        if (Group* group = bot->GetGroup(); group && !dead)
        {
            alone = true;
            for (GroupReference* ref = group->GetFirstMember(); ref && alone; ref = ref->next())
            {
                Player* member = ref->getSource();
                if (member && member != bot && member->IsInWorld() && member->IsAlive() && member->GetMapId() == bot->GetMapId() &&
                    member->GetDistance(bot) <= ALONE_DISTANCE)
                    alone = false;
            }
        }
        if (alone != c.alone)
        {
            c.alone = alone;
            if (bot->GetPlayerbotAI())
                bot->GetPlayerbotAI()->ResetStrategies(false);
        }

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
            bool wasJoined = !c.partyLeader.empty();
            c.reason = KeepParty(c, bot);
            if (!wasJoined && !c.partyLeader.empty())
                Event(guid, "party", "joined " + c.partyLeader);
            continue;
        }

        if (c.job == "dungeon")
        {
            // A dungeon leader leads: a master taken from an invite it accepted, such as the player's
            // before promoting it, would have it follow instead of running the job.
            PlayerbotAI* leaderAi = bot->GetPlayerbotAI();
            Group* ownGroup = bot->GetGroup();
            if (leaderAi && leaderAi->GetMaster() && leaderAi->GetMaster() != bot && ownGroup && ownGroup->IsLeader(bot->GetObjectGuid()))
            {
                leaderAi->SetMaster(nullptr);
                leaderAi->ResetStrategies(false);
            }
            std::string waiting = InviteCompanion(c, bot);

            GuildDungeon::Update update = sGuildDungeon.Tick(bot, ArgValue(c, "name"));
            if (!waiting.empty() && update.status == "working" && update.reason.rfind("waiting", 0) == 0)
                update.reason = waiting;
            for (auto& [kind, detail] : update.events)
                Event(guid, kind, detail);
            c.reason = update.reason;
            if (update.status == "failed")
            {
                c.status = "failed";
                Event(guid, "job_failed", "dungeon " + c.reason);
            }
            else if (update.status == "done")
            {
                // With `runs`, the next run starts from the stone at the next check; 0 runs has no end.
                ++c.runsDone;
                const std::string runsArg = ArgValue(c, "runs");
                const uint32 runs = runsArg.empty() ? 1 : uint32(atoi(runsArg.c_str()));
                if (runs == 0 || c.runsDone < runs)
                {
                    c.reason = "run " + std::to_string(c.runsDone) + (runs ? " of " + std::to_string(runs) : "") + " done: " + c.reason;
                    Event(guid, "dungeon_again", c.reason);
                }
                else
                {
                    c.status = "done";
                    Event(guid, "job_done", "dungeon " + c.reason);
                }
            }
            if ((update.strategiesChanged || update.status != "working") && bot->GetPlayerbotAI())
                bot->GetPlayerbotAI()->ResetStrategies(false);
            continue;
        }

        if (GuildJobs::IsJob(c.job))
        {
            GuildJobs::Update update = sGuildJobs.Tick(bot, c.job, c.jobArgs, c.jobSeq);
            for (auto& [kind, detail] : update.events)
                Event(guid, kind, detail);
            c.reason = update.reason;
            if (update.status != "working")
            {
                c.status = update.status;
                Event(guid, c.status == "done" ? "job_done" : "job_failed", c.job + " " + c.reason);
                if (bot->GetPlayerbotAI())
                    bot->GetPlayerbotAI()->ResetStrategies(false);
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

        if (c.job == "level" && now - c.progressAt >= STALL_SECONDS)
        {
            c.reason = "no experience for " + std::to_string((now - c.progressAt) / MINUTE) + " minutes";
            if (!c.stalled)
            {
                c.stalled = true;
                Event(guid, "stalled", c.reason);
            }

            // Get it going again, once every stall period while it earns nothing: first drop what it is doing,
            // which is usually a target it cannot reach; if that does not help, go home by hearthstone.
            PlayerbotAI* ai = bot->GetPlayerbotAI();
            if (ai && now - c.recoveredAt >= STALL_SECONDS)
            {
                // Fallen through the world: no reset or travel gets it out, and the hearthstone may be an hour away.
                if (bot->IsAlive() && UnderTheMap(bot) && SendToGraveyard(bot))
                {
                    ai->Reset(true);
                    Event(guid, "recovery", "under the map");
                }
                else if (now - c.recoveredAt > RECOVERY_WINDOW_SECONDS)
                {
                    bot->CombatStop();
                    ai->Reset(true);
                    Event(guid, "recovery", "reset");
                }
                else
                {
                    bool home = ai->DoSpecificAction("hearthstone", ai::Event(), true);
                    Event(guid, "recovery", home ? "hearthstone" : "hearthstone not ready");
                }
                c.recoveredAt = now;
            }
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

std::string GuildBotMgr::InviteCompanion(Character& c, Player* bot)
{
    std::string name = ArgValue(c, "with");
    if (name.empty() || !normalizePlayerName(name) || name == c.name)
        return "";

    Player* companion = ObjectAccessor::FindPlayerByName(name.c_str());
    if (!companion || !companion->IsInWorld())
        return "waiting for " + name + " to be online";
    // The player's own characters only, as the party job has it.
    if (companion->GetSession()->GetAccountId() != bot->GetSession()->GetAccountId())
        return name + " is not on the same account";

    Group* group = bot->GetGroup();
    if (group && group->IsMember(companion->GetObjectGuid()))
        return "";
    if (group && !group->IsLeader(bot->GetObjectGuid()))
        return "";
    if (companion->GetGroup())
        return "waiting for " + name + " to leave their group";
    if (group && group->IsFull())
        return "no room in the group for " + name;

    // Once a minute, as a player would ask again; the client shows the invite, and the player accepts it.
    const time_t now = time(nullptr);
    if (!companion->GetGroupInvite() && now - c.invitedAt >= MINUTE)
    {
        c.invitedAt = now;
        WorldPacket invite(CMSG_GROUP_INVITE);
        invite << name;
        bot->GetSession()->HandleGroupInviteOpcode(invite);
    }
    return "waiting for " + name + " to accept the invite";
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
    // A full party becomes a raid, so a guild can gather forty; a full raid has no room.
    if (leaderGroup && leaderGroup->IsFull() && !leaderGroup->IsRaidGroup())
        leaderGroup->ConvertToRaid();
    if (leaderGroup && leaderGroup->IsFull())
        return leaderName + "'s raid is full";

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
            "job = VALUES(job), job_seq = VALUES(job_seq), job_status = VALUES(job_status), job_reason = VALUES(job_reason), role = '%s', tank_order = %u",
            Escape(c.job).c_str(), c.jobSeq, Escape(c.status).c_str(), Escape(c.reason).c_str(), c.guid, Escape(c.role).c_str(), uint32(GetTankOrder(c.guid)));
        return;
    }

    CharacterDatabase.PExecute(
        "REPLACE INTO guildbot_state (guid, name, controller, level, xp, xp_next, money, map, zone, x, y, z, "
        "health_pct, dead, in_combat, free_bag_slots, job, job_seq, job_status, job_reason, role, talent_tree, talent_points, role_status, tank_order) "
        "VALUES (%u, '%s', '%s', %u, %u, %u, %u, %u, %u, %f, %f, %f, %u, %u, %u, %u, '%s', %u, '%s', '%s', '%s', '%s', %u, '%s', %u)",
        c.guid, Escape(c.name).c_str(), controller.c_str(), bot->GetLevel(), bot->GetUInt32Value(PLAYER_XP),
        bot->GetUInt32Value(PLAYER_NEXT_LEVEL_XP), bot->GetMoney(), bot->GetMapId(), ZoneOf(bot),
        bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(), uint32(bot->GetHealthPercent()),
        sServerFacade.UnitIsDead(bot) ? 1 : 0, sServerFacade.IsInCombat(bot) ? 1 : 0, FreeBagSlots(bot),
        Escape(c.job).c_str(), GetPlayerBot(c.guid) ? c.appliedSeq : c.jobSeq, Escape(c.status).c_str(), Escape(c.reason).c_str(),
        Escape(c.role).c_str(), Escape(TreeName(bot)).c_str(), uint32(TalentSpec(bot).GetTalentPoints()), Escape(c.roleStatus).c_str(),
        uint32(GetTankOrder(c.guid)));
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
    if (sub == "debug")
        return HandleDebug(rest);
    if (sub == "metrics")
        return HandleMetrics(rest);
    if (sub == "spots")
        return HandleSpots(rest);
    if (sub == "runs")
        return sGuildRuns.HandleRuns(rest);
    if (sub == "tank")
        return HandleTank(rest);
    if (sub == "bench")
        return GuildBench::HandleCommand(rest);
    if (sub == "on" || sub == "off" || sub == "job" || sub == "remove" || sub == "role")
        return HandleOrder(sub, rest);

    return {
        "guildbot add <name> [job [args]]       make a character a guild character",
        "guildbot create <account> <name> <race> <class> [male|female]   create a level 1 character on an account",
        "guildbot job <name> <job> [args]       give an order: idle, level [until=<level>], party leader=<name>, farm item=<id> [amount=<n>], learn profession=<name>, dungeon name=<dungeon> [with=<name>]",
        "guildbot role <name> tank|healer|dps|none   the role its talents follow",
        "guildbot tank <name> <order>|none      tank order in its group: 1 main tank, 2 the first assist tank, ...",
        "guildbot on|off <name>                 switch the engine's play on or off",
        "guildbot remove <name>                 stop treating it as a guild character",
        "guildbot list                          every guild character, one line each",
        "guildbot bench ...                     GM test benches only: level, gear, consumables (guildbot bench help)",
        "guildbot state <name>                  what it is doing, as the state table holds it",
        "guildbot debug <name>                  the engine's view: strategies, last action, target, power, movement",
        "guildbot metrics [hours]               how much of their time levelling characters earn experience, by hour",
        "guildbot spots [hours]                 grind spots by experience per hour there, with the walk to them",
        "guildbot runs [count]                  the latest dungeon runs: members, bosses, wipes and loot",
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

void GuildBotMgr::LoadAssignments()
{
    std::map<uint32, uint8> loaded;
    auto result = CharacterDatabase.PQuery("SELECT guid, tank_order FROM guildbot_assignment WHERE tank_order > 0");
    if (result)
    {
        do
        {
            Field* f = result->Fetch();
            loaded[f[0].GetUInt32()] = uint8(f[1].GetUInt32());
        } while (result->NextRow());
    }

    auto describe = [](uint8 order) {
        return order == 0 ? std::string("none") : order == 1 ? std::string("main") : "assist " + std::to_string(order - 2);
    };

    if (!assignmentsLoaded)
    {
        assignmentsLoaded = true;
        tankOrders = std::move(loaded);
        return;
    }

    // An event for every change, the player's own characters included.
    for (auto& [guid, order] : loaded)
        if (GetTankOrder(guid) != order)
            Event(guid, "tank", describe(order));
    for (auto& [guid, order] : tankOrders)
        if (!loaded.count(guid))
            Event(guid, "tank", describe(0));

    tankOrders = std::move(loaded);
}

std::list<std::string> GuildBotMgr::HandleTank(const std::string& args)
{
    auto [name, orderText] = SplitWord(args);
    std::string normalized = name;
    if (!normalizePlayerName(normalized) || orderText.empty())
        return { "usage: guildbot tank <name> <order>|none" };

    ObjectGuid guid = sObjectMgr.GetPlayerGuidByName(normalized);
    if (!guid)
        return { "no character named " + normalized };

    uint32 order = orderText == "none" ? 0 : uint32(atoi(orderText.c_str()));
    if (orderText != "none" && (order < 1 || order > 40))
        return { "the order is 1 for the main tank, 2 for the first assist tank, and so on, or none" };

    if (order)
        CharacterDatabase.DirectPExecute("REPLACE INTO guildbot_assignment (guid, tank_order) VALUES (%u, %u)", guid.GetCounter(), order);
    else
        CharacterDatabase.DirectPExecute("DELETE FROM guildbot_assignment WHERE guid = %u", guid.GetCounter());

    sinceOrders = ORDERS_MS;
    return { normalized + ": tank " + orderText };
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
             << (GetTankOrder(guid) ? " tank=" + std::to_string(GetTankOrder(guid)) : "")
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
        << ",\"map\":" << bot->GetMapId() << ",\"zone\":" << ZoneOf(bot)
        << ",\"health_pct\":" << uint32(bot->GetHealthPercent()) << ",\"dead\":" << (sServerFacade.UnitIsDead(bot) ? "true" : "false")
        << ",\"in_combat\":" << (sServerFacade.IsInCombat(bot) ? "true" : "false") << ",\"free_bag_slots\":" << FreeBagSlots(bot)
        << ",\"job\":\"" << c->job << "\",\"job_seq\":" << c->jobSeq << ",\"job_status\":\"" << c->status
        << "\",\"job_reason\":\"" << c->reason << "\",\"active_job\":\"" << GetActiveJob(c->guid) << "\"}";
    return { out.str() };
}

std::list<std::string> GuildBotMgr::HandleDebug(const std::string& args)
{
    Character* c = Find(Trim(args));
    if (!c)
        return { "not a guild character" };

    Player* bot = sObjectMgr.GetPlayer(ObjectGuid(HIGHGUID_PLAYER, c->guid), false);
    PlayerbotAI* ai = GetPlayerBot(c->guid) ? bot->GetPlayerbotAI() : nullptr;
    if (!bot || !ai)
        return { c->name + " is not played by the engine now" };

    auto join = [](const std::list<std::string_view>& names) {
        std::string text;
        for (auto& name : names)
            text += (text.empty() ? "" : ",") + std::string(name);
        return text;
    };

    std::list<std::string> out;
    out.push_back("combat: " + join(ai->GetStrategies(BotState::BOT_STATE_COMBAT)));
    out.push_back("non combat: " + join(ai->GetStrategies(BotState::BOT_STATE_NON_COMBAT)));
    out.push_back("last action: combat " + ai->GetLastExecutedActionName(BotState::BOT_STATE_COMBAT)
        + ", non combat " + ai->GetLastExecutedActionName(BotState::BOT_STATE_NON_COMBAT));
    out.push_back("last decision: " + ai->GetLastActionDecision(BotState::BOT_STATE_COMBAT));

    std::ostringstream life;
    life << "map " << bot->GetMapId() << (bot->IsBeingTeleportedFar() ? ", teleporting to map " + std::to_string(bot->GetTeleportDest().mapid) : "")
         << ", " << (bot->IsAlive() ? "alive" : bot->HasFlag(PLAYER_FLAGS, PLAYER_FLAGS_GHOST) ? "ghost" : "dead")
         << ", in combat " << (bot->IsInCombat() ? "yes" : "no");
    if (Corpse* corpse = bot->GetCorpse())
        life << ", corpse on map " << corpse->GetMapId();
    out.push_back(life.str());

    std::ostringstream power;
    power << "power " << uint32(bot->GetPowerType()) << " at " << bot->GetPower(bot->GetPowerType()) << "/" << bot->GetMaxPower(bot->GetPowerType())
          << ", form " << uint32(bot->GetShapeshiftForm()) << ", moving " << (bot->IsMoving() ? "yes" : "no")
          << ", movement " << uint32(bot->GetMotionMaster()->GetCurrentMovementGeneratorType())
          << ", casting " << (bot->IsNonMeleeSpellCasted(false) ? "yes" : "no");
    out.push_back(power.str());

    if (Unit* victim = bot->GetVictim())
    {
        std::ostringstream target;
        target << "victim " << victim->GetName() << " health " << victim->GetHealth() << "/" << victim->GetMaxHealth()
               << " distance " << uint32(bot->GetDistance(victim)) << " height " << int32(victim->GetPositionZ() - bot->GetPositionZ())
               << " sight " << (bot->IsWithinLOSInMap(victim) ? "yes" : "no") << " attacking us " << (victim->GetVictim() == bot ? "yes" : "no")
               << ", its victim " << (victim->GetVictim() ? victim->GetVictim()->GetName() : "none")
               << ", in combat " << (victim->IsInCombat() ? "yes" : "no") << ", evading " << (victim->GetCombatManager().IsInEvadeMode() ? "yes" : "no")
               << ", melee reach " << (bot->CanReachWithMeleeAttack(victim) ? "yes" : "no") << ", we fight " << (bot->IsInCombat() ? "yes" : "no");
        out.push_back(target.str());
        // What a creature victim is doing, for a boss that neither fights back nor dies: its movement type,
        // what holds it, whether its script has taken it over, and whether it can die.
        if (victim->IsCreature())
        {
            Creature* creature = static_cast<Creature*>(victim);
            std::ostringstream state;
            state << "victim state: movement " << uint32(creature->GetMotionMaster()->GetCurrentMovementGeneratorType())
                  << ", moving " << (creature->IsStopped() ? "no" : "yes")
                  << ", held " << (creature->hasUnitState(UNIT_STAT_NOT_MOVE) ? "yes" : "no")
                  << ", casting " << (creature->IsNonMeleeSpellCasted(false) ? "yes" : "no")
                  << ", unkillable " << (creature->GetSettings().HasFlag(CreatureStaticFlags::UNKILLABLE) ? "yes" : "no");
            if (UnitAI* unitAi = creature->AI())
                state << ", script status " << (unitAi->GetCombatScriptStatus() ? "on" : "off")
                      << ", react " << uint32(unitAi->GetReactState());
            out.push_back(state.str());
        }
    }
    else
        out.push_back("no victim");

    std::ostringstream travel;
    ai::TravelTarget* target = ai->GetAiObjectContext()->GetValue<ai::TravelTarget*>("travel target")->Get();
    if (target && target->GetDestination() && target->GetPosition())
        travel << "travel " << target->GetDestination()->GetTitle() << " (" << target->GetDestination()->GetShortName() << ")"
               << " status " << uint32(target->GetStatus()) << " map " << target->GetPosition()->getMapId()
               << " distance " << uint32(target->Distance(bot)) << " forced " << (target->IsForced() ? "yes" : "no");
    else
        travel << "no travel target";
    out.push_back(travel.str());

    // The moves behind the travel: failed moves to the present target, and the last path, where it starts and ends,
    // to tell a path that leads off from where the character stands from one it has left.
    std::ostringstream move;
    move << "move retries " << (target ? target->GetRetryCount(true) : 0) << ", retries " << (target ? target->GetRetryCount(false) : 0);
    ai::TravelPath& lastPath = ai->GetAiObjectContext()->GetValue<ai::LastMovement&>("last movement")->Get().lastPath;
    if (lastPath.empty())
        move << ", no last path";
    else
    {
        move << ", last path " << lastPath.getPath().size() << " points, starts " << uint32(lastPath.getFront().distance(WorldPosition(bot)))
             << " yd off, ends on map " << lastPath.getBack().getMapId();
        if (target && target->GetPosition())
            move << " " << uint32(lastPath.getBack().distance(*target->GetPosition())) << " yd from the target";
    }
    // A chase is what pulls it off its path: a victim, or a friend it reaches for with a spell.
    if (bot->GetMotionMaster()->GetCurrentMovementGeneratorType() == CHASE_MOTION_TYPE)
        if (Unit* chased = sServerFacade.GetChaseTarget(bot))
            move << ", chasing " << chased->GetName() << " at " << uint32(bot->GetDistance(chased)) << " yd"
                 << (chased->IsPlayer() ? "" : ", no player");
    out.push_back(move.str());

    std::ostringstream attackers;
    attackers << "attackers:";
    for (Unit* attacker : bot->getAttackers())
        attackers << " " << attacker->GetName() << " (" << uint32(attacker->GetHealthPercent()) << "%)";
    out.push_back(attackers.str());
    return out;
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
