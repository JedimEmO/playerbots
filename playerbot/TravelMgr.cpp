#include "playerbot/TravelMgr.h"
#include <sstream>
#include <ctime>
#include <cstdio>
#include <numeric>
#include <iomanip>

#include "playerbot/strategy/values/SharedValueContext.h"
#include "playerbot/strategy/values/TravelValues.h"
#include "MotionGenerators/PathFinder.h"
#include "TravelNode.h"
#include "PlayerbotAI.h"
#include "BotTests.h"
#include "Globals/ObjectAccessor.h"

using namespace ai;
#include "Tools/Formulas.h"

using namespace MaNGOS;

PlayerTravelInfo::PlayerTravelInfo(Player* player)
{
    PlayerbotAI* ai = player->GetPlayerbotAI();
    AiObjectContext* context = ai->GetAiObjectContext();

    position = player;

    team = player->GetTeam();
    level = player->GetLevel();
    currentSkill[SKILL_MINING] = player->GetSkillValue(SKILL_MINING);
    currentSkill[SKILL_HERBALISM] = player->GetSkillValue(SKILL_HERBALISM);
    currentSkill[SKILL_FISHING] = player->GetSkillValue(SKILL_FISHING);
    currentSkill[SKILL_SKINNING] = player->GetSkillValue(SKILL_SKINNING);

    skillMax[SKILL_MINING] = player->GetSkillMax(SKILL_MINING);
    skillMax[SKILL_HERBALISM] = player->GetSkillMax(SKILL_HERBALISM);
    skillMax[SKILL_FISHING] = player->GetSkillMax(SKILL_FISHING);
    skillMax[SKILL_SKINNING] = player->GetSkillMax(SKILL_SKINNING);

    money = player->GetMoney();

    autonomous = sRandomPlayerbotMgr.IsAutonomousBot(player);

    if (player->GetGroup())
        groupSize = player->GetGroup()->GetMembersCount();

    focusList = AI_VALUE(focusQuestTravelList, "focus travel target");

    for (auto& [valueName, value] : boolValues)
        value = AI_VALUE(bool, valueName);

    for (auto& [valueName, value] : uint8Values)
        value = AI_VALUE(uint8, valueName);

    for (auto& [valueName, value] : uint32Values)
        value = AI_VALUE(uint32, valueName);
}

bool PlayerTravelInfo::IsGrayQuest(const Quest* quest) const
{
    int32 lowLevelDiff = sWorld.getConfig(CONFIG_INT32_QUEST_LOW_LEVEL_HIDE_DIFF);
    if (!quest || lowLevelDiff < 0)
        return false;

    int32 questLevel = quest->GetQuestLevel() > 0 ? quest->GetQuestLevel() : (int32)level;
    return (int32)level > questLevel + lowLevelDiff;
}

std::string EntryTravelDestination::GetShortName() const
{
    switch (purpose)
    {
    case TravelDestinationPurpose::QuestGiver:
        return "questgiver";
    case TravelDestinationPurpose::QuestObjective1:
    case TravelDestinationPurpose::QuestObjective2:
    case TravelDestinationPurpose::QuestObjective3:
    case TravelDestinationPurpose::QuestObjective4:
        return "questobjective";
    case TravelDestinationPurpose::QuestTaker:
        return "questtaker";
    case TravelDestinationPurpose::Vendor:
        return "vendor";
    case TravelDestinationPurpose::AH:
        return "ah";
    case TravelDestinationPurpose::Repair:
        return "repair";
    case TravelDestinationPurpose::Mail:
        return "mail";
    case TravelDestinationPurpose::Bank:
        return "bank";
    case TravelDestinationPurpose::Trainer:
        return "trainer";
    case TravelDestinationPurpose::Explore:
        return "explore";
    case TravelDestinationPurpose::GenericRpg:
        return "rpg";
    case TravelDestinationPurpose::Grind:
        return "grind";
    case TravelDestinationPurpose::Boss:
        return  "boss";
    case TravelDestinationPurpose::GatherFishing:
    case TravelDestinationPurpose::GatherHerbalism:
    case TravelDestinationPurpose::GatherMining:
    case TravelDestinationPurpose::GatherSkinning:
        return "gather";
    default:
        return "unknown";
    }
    return "none";
};

std::string QuestTravelDestination::GetTitle() const {
    return ChatHelper::formatQuest(GetQuestTemplate());
}

bool QuestRelationTravelDestination::IsPossible(const PlayerTravelInfo& info) const
{
    if (!info.GetBoolValue2("has strategy", "rpg quest"))
        return false;

    bool forceThisQuest = info.HasFocusQuest();

    if (forceThisQuest && !info.IsFocusQuest(GetQuestId()))
        return false;

    const Quest* quest = GetQuestTemplate();

    if (GetRelation() == 0)
    {
        if (!forceThisQuest && (int32)quest->GetQuestLevel() >= (int32)info.GetLevel() + (int32)5)
            return false;

        // wow-playground: a player's character levels; a gray quest gives it next to no experience, whatever its
        // reward. Random bots still take them, for the reward or the money.
        if (!forceThisQuest && info.IsAutonomous() && info.IsGrayQuest(quest))
            return false;

        if ((int32)info.GetLevel() < quest->GetMinLevel() || (int32)info.GetLevel() > quest->GetMaxLevel())
            return false;

        if (OnMap(info.GetPosition())) //CanTakeQuest will check required conditions which will fail on a different map.
            if (quest->GetRequiredCondition())          //So we skip this quest for now.
                return false;

        if (!info.HasFocusQuest())
        {
            if (info.GetBoolValue("can fight equal"))
            {
                if (info.GetUint8Value("free quest log slots") < 5)
                    return false;
            }
            else
            {
                if (info.GetUint8Value("free quest log slots") < 10)
                    return false;
            }

            //Do not try to pick up dungeon/elite quests in instances without a group.
            if ((quest->GetType() == QUEST_TYPE_ELITE || quest->GetType() == QUEST_TYPE_DUNGEON) && !info.GetBoolValue("can fight boss"))
                return false;
        }
    }
    else
    {
        //Do not try to hand-in dungeon/elite quests in instances without a group.
        if ((quest->GetType() == QUEST_TYPE_ELITE || quest->GetType() == QUEST_TYPE_DUNGEON) && !info.GetBoolValue("can fight boss"))
        {
            if (IsOverWorld(info.GetPosition()))
                return false;
        }
    }

    return true;
}

bool QuestRelationTravelDestination::IsActive(Player* bot, const PlayerTravelInfo& info) const {
    PlayerbotAI* ai = bot->GetPlayerbotAI();
    AiObjectContext* context = ai->GetAiObjectContext();

    if(!IsPossible(info))
        return false;

    bool forceThisQuest = info.HasFocusQuest(); //Checked in IsPossible if it's 'this' quest.

    if (GetRelation() == 0)
    {
        if (!bot->GetMap()->IsContinent() && (GetClosestPoint(bot)->getMapId() != bot->GetMapId())) //This gives issues for bot->CanTakeQuest so stop here.
            return false;

        if (forceThisQuest)
        {
            if (!AI_VALUE2(bool, "group or", "following party,can accept quest npc::" + std::to_string(GetEntry()))) //Noone has yellow exclamation mark.
                if (!AI_VALUE2(bool, "group or", "following party,can accept quest low level npc::" + std::to_string(GetEntry()))) //Noone can do this quest.
                    return false;
        }
        else
        {
            if (info.GetBoolValue("can fight equal"))
            {
                if (!AI_VALUE2(bool, "group or", "following party,can accept quest npc::" + std::to_string(GetEntry()))) //Noone has yellow exclamation mark.
                    if (!AI_VALUE2(bool, "group or", "following party,can accept quest low level npc::" + std::to_string(GetEntry()) + ",need quest reward::" + std::to_string(GetQuestId()))) //Noone can do this quest for a usefull reward.
                        return false;
            }
            else
            {
                if (!AI_VALUE2(bool, "group or", "following party,can accept quest low level npc::" + std::to_string(GetEntry()))) //Noone can pick up this quest for money.
                    return false;
            }
        }
    }
    else
    {
        if (!AI_VALUE2(bool, "group or", "following party,can turn in quest npc::" + std::to_string(GetEntry())))
            return false;
    }

    if (GetEntry() > 0)
    {
        return !GuidPosition(HIGHGUID_UNIT, GetEntry()).IsHostileTo(bot);
    }

    return true;
}

std::string QuestRelationTravelDestination::GetTitle() const {
    std::ostringstream out;

    out << "talk to ";

    if (GetRelation() == 0)
        out << "questgiver ";
    else
        out << "questtaker ";

    out << ChatHelper::formatWorldEntry(GetEntry());
    return out.str();
}

bool QuestObjectiveTravelDestination::IsPossible(const PlayerTravelInfo& info) const
{
    if (!info.GetBoolValue2("has strategy", "rpg quest"))
        return false;

    bool forceThisQuest = info.HasFocusQuest();

    if (forceThisQuest && !info.IsFocusQuest(GetQuestId()))
        return false;

    // wow-playground: a player's character does not chase the objectives of a quest gone gray in its log; it gives
    // next to no experience. A finished one is still handed in, and the quest log cleaning drops the rest.
    if (!forceThisQuest && info.IsAutonomous() && info.IsGrayQuest(GetQuestTemplate()))
        return false;

    bool skipShouldGrindCheck = false;

    //Check mob level
    if (GetEntry() > 0)
    {
#ifdef MANGOSBOT_TWO
        switch (GetQuestId())
        {
            case 12779: //An End To All Things
                skipShouldGrindCheck = true;
        }
#endif
    }

    if (!forceThisQuest)
    {
        if ((int32)GetQuestTemplate()->GetQuestLevel() > (int32)info.GetLevel() + (int32)1)
            return false;

        if (!skipShouldGrindCheck && GetQuestTemplate()->GetQuestLevel() + 5 > (int)info.GetLevel() && !info.GetBoolValue("can fight equal"))
            return false;
    }

    if (info.IsInRaid() != (GetQuestTemplate()->GetType() == QUEST_TYPE_RAID))
        return false;


    bool skipKillableCheck = false;

    //Check mob level
    if (GetEntry() > 0)
    {
#ifdef MANGOSBOT_TWO        
        switch (GetQuestId()) {
        case 12680: //Grand Theft Palomino
        case 12687: //Into the Realm of Shadows
        case 12698: //The Gift That Keeps On Giving
        case 12779: //An End To All Things
        case 12801: //The Light of Dawn
        case 12701: //Massacre At Light's Point
            skipKillableCheck = true;
        }
#endif

        CreatureInfo const* cInfo = GetCreatureInfo();

        if (cInfo->NpcFlags & UNIT_NPC_FLAG_VENDOR && GetQuestTemplate()->ReqItemId[GetObjective()])
        {
            ItemPrototype const* proto = sObjectMgr.GetItemPrototype(GetQuestTemplate()->ReqItemId[GetObjective()]);
            if (GetQuestTemplate()->ReqItemCount[GetObjective()] * proto->BuyPrice > info.GetMoney()) //Need more money.
                return false;

            skipKillableCheck = true;
        }

        if (!skipKillableCheck && !forceThisQuest)
        {
            if (cInfo && (int)cInfo->MaxLevel - (int)info.GetLevel() > 4)
                return false;

            //Do not try to hand-in dungeon/elite quests in instances without a group.
            if (cInfo->Rank > CREATURE_ELITE_NORMAL)
            {
                if (!IsOverWorld(info.GetPosition()) && info.GetPosition().getMapId() != 609 && !info.GetBoolValue("can fight boss"))
                    return false;
                else if (!info.GetBoolValue("can fight elite"))
                    return false;
            }
        }
    }

    if (!forceThisQuest)
    {
        if (!skipKillableCheck && GetQuestTemplate()->GetType() == QUEST_TYPE_ELITE && !info.GetBoolValue("can fight elite"))
            return false;

        //Do not try to do dungeon/elite quests in instances without a group.
        if ((GetQuestTemplate()->GetType() == QUEST_TYPE_ELITE || GetQuestTemplate()->GetType() == QUEST_TYPE_DUNGEON || GetQuestTemplate()->GetType() == QUEST_TYPE_RAID) && !info.GetBoolValue("can fight boss"))
        {
            if (!IsOverWorld(info.GetPosition()))
                return false;
        }

        //Do not try to do pvp quests in bg's (no way to travel there). 
        if (GetQuestTemplate()->GetType() == QUEST_TYPE_PVP)
        {
            if (!IsOverWorld(info.GetPosition()))
                return false;
        }
    }

    return true;
}

bool QuestObjectiveTravelDestination::IsActive(Player* bot, const PlayerTravelInfo& info) const {
    PlayerbotAI* ai = bot->GetPlayerbotAI();
    AiObjectContext* context = ai->GetAiObjectContext();

    if (!IsPossible(info))
        return false;

    bool forceThisQuest = info.HasFocusQuest();

    bool skipKillableCheck = false;

    if (GetEntry() > 0)
    {
#ifdef MANGOSBOT_TWO
        switch (GetQuestId())
        {
            case 12848: //The Endless Hunger
                skipKillableCheck = true;
                break;
            case 12680: //Grand Theft Palomino
                switch (GetEntry())
                {
                    case 28605: //Havenshire horse
                    case 28606:
                    case 28607: return !AI_VALUE2(bool, "trigger active", "in vehicle") && ai->CanSpellClick(bot, GetEntry()); break;
                    case 28653: //Salanar the Horseman
                        return true;
                        break;
                }
                break;
            case 12687: //Into the Realm of Shadows
                switch (GetEntry())
                {
                    case 28768: //Dark Rider of Acherus
                    case 28909:
                    case 28782: //Acherus Deathcharger
                        return !AI_VALUE2(bool, "trigger active", "in vehicle");
                    case 29501: //Scourge Gryphon (return to hand in)
                        return AI_VALUE2(bool, "trigger active", "in vehicle");
                }
                break;
            case 12698: //The Gift That Keeps On Giving
                switch (GetEntry())
                {
                    case 28819: //Scarlet Miner
                        return !bot->FindGuardianWithEntry(28845);
                    case 28658: //Gothic the Harvester
                        return bot->FindGuardianWithEntry(28845);
                }
                break;
            case 12701: //Massacre At Light's Point
                switch (GetEntry())
                {
                    case 28833: //Scarlet Cannon
                        return DistanceTo(bot) < 30.0f;
                }
        }
#endif

        CreatureInfo const* cInfo = GetCreatureInfo();

        if (!skipKillableCheck && cInfo->NpcFlags & UNIT_NPC_FLAG_VENDOR && GetQuestTemplate()->ReqItemId[GetObjective()] &&
            !GuidPosition(HIGHGUID_UNIT, GetEntry()).IsHostileTo(bot))
        {
            skipKillableCheck = true;
        }
    }

    if (!skipKillableCheck)
        skipKillableCheck = ai->CanSpellClick(bot, GetEntry());

    std::vector<std::string> qualifier = { std::to_string(GetQuestTemplate()->GetQuestId()), std::to_string(GetObjective()) };

    if (!AI_VALUE2(bool, "group or", "following party,need quest objective::" + Qualified::MultiQualify(qualifier, ","))) //Noone needs the quest objective.
        return false;

    WorldPosition botPos(bot);

    if (!skipKillableCheck && GetEntry() > 0 && !IsOut(botPos))
    {
        TravelTarget* target = AI_VALUE(TravelTarget*, "travel target");

        //Only look for the target if it is unique or if we are currently working on it.
        if (IsUnique() || (target->GetStatus() == TravelStatus::TRAVEL_STATUS_WORK && target->GetEntry() == GetEntry()))
        {
            std::list<ObjectGuid> targets = AI_VALUE(std::list<ObjectGuid>, "possible targets");

            for (auto& target : targets)
                if (target.GetEntry() == GetEntry() && target.IsCreature() && ai->GetCreature(target) && ai->GetCreature(target)->IsAlive())
                    return true;

            return false;
        }
    }

    return true;
}

std::string QuestObjectiveTravelDestination::GetTitle() const {
    std::ostringstream out;

    if (GetQuestTemplate()->ReqItemCount[GetObjective()] > 0)
        out << "loot " << ChatHelper::formatItem(sObjectMgr.GetItemPrototype(GetQuestTemplate()->ReqItemId[GetObjective()]), 0, 0) << " from";
    else if (GetEntry() > 0)
        out << "kill";
    else
        out << "use";

    out << " " << ChatHelper::formatWorldEntry(GetEntry());

    out << " (objective " << (GetObjective() + 1) << ")";

    return out.str();
}

uint8 QuestObjectiveTravelDestination::GetObjective() const
{
    switch (GetPurpose())
    {
    case TravelDestinationPurpose::QuestObjective1:
        return 0;
    case TravelDestinationPurpose::QuestObjective2:
        return 1;
    case TravelDestinationPurpose::QuestObjective3:
        return 2;
    case TravelDestinationPurpose::QuestObjective4:
        return 3;
    default:
        return 0;
    }
    return 0;
}

bool RpgTravelDestination::IsPossible(const PlayerTravelInfo& info) const
{   
    //Horde pvp baracks
    if (ClosestMapId(info.GetPosition()) == 450 && info.GetTeam() == ALLIANCE)
        return false;

    //Alliance pvp baracks
    if (ClosestMapId(info.GetPosition()) == 449 && info.GetTeam() == HORDE)
        return false;

    return true;
}

bool RpgTravelDestination::IsActive(Player* bot, const PlayerTravelInfo& info) const
{
    PlayerbotAI* ai = bot->GetPlayerbotAI();
    AiObjectContext* context = ai->GetAiObjectContext();

    if (!IsPossible(info))
        return false;   

    //Once the target rpged with it is added to the ignore list. We can now move on.
    std::set<ObjectGuid>& ignoreList = AI_VALUE(std::set<ObjectGuid>&,"ignore rpg target");

    for (auto& i : ignoreList)
    {
        if (i.GetEntry() == GetEntry())
        {
            return false;
        }
    }    

    return !GuidPosition(HIGHGUID_UNIT, GetEntry()).IsHostileTo(bot);
}

std::string RpgTravelDestination::GetTitle() const 
{
    std::ostringstream out;

    switch (GetPurpose())
    {    
    case TravelDestinationPurpose::Vendor:
        out << "sell items to";
        break;
    case TravelDestinationPurpose::AH:
        out << "put items on auction at";
        break;
    case TravelDestinationPurpose::Repair:
        out << "repair at";
        break;
    case TravelDestinationPurpose::Mail:
        out << "receive mail from";
        break;
    case TravelDestinationPurpose::Trainer:
        out << "train a skill at";
        break;
    case TravelDestinationPurpose::GenericRpg: 
        out << "find"; //Named travel purpose.
        break;
    default:
        out << "";
        break;
    }

    /*
    out << " " << GetShortName();

    if (GetEntry() > 0)
        out << " npc ";
    else
        out << " object ";

    */

    out << " ";

    out << ChatHelper::formatWorldEntry(GetEntry());

    return out.str();
}

AreaTableEntry const* ZoneTravelDestination::GetArea() const
{
    for (uint32 areaid = 0; areaid <= sAreaStore.GetNumRows(); ++areaid)
    {
        AreaTableEntry const* areaEntry = sAreaStore.LookupEntry(areaid);
        if (areaEntry && areaEntry->ID == GetEntry())
        {
            return areaEntry;
        }
    }

    return nullptr;
}

bool ExploreTravelDestination::IsPossible(const PlayerTravelInfo& info) const
{
    AreaTableEntry const* area = GetArea();

    if (GetLevel() && (uint32)GetLevel() > info.GetLevel() && info.GetLevel() < DEFAULT_MAX_LEVEL)
        return false;

    return true;
}

bool ExploreTravelDestination::IsActive(Player* bot, const PlayerTravelInfo& info) const
{
    if (!IsPossible(info))
        return false;

    AreaTableEntry const* area = GetArea();

    if (area->exploreFlag == 0xffff)
        return false;
    int offset = area->exploreFlag / 32;

    uint32 val = (uint32)(1 << (area->exploreFlag % 32));
    uint32 currFields = bot->GetUInt32Value(PLAYER_EXPLORED_ZONES_1 + offset);

    return !(currFields & val);    
}

bool GrindTravelDestination::IsPossible(const PlayerTravelInfo& info) const
{
    if (info.GetBoolValue("should sell") && (info.GetBoolValue("can sell") || info.GetBoolValue("can ah sell")))
        return false;

    CreatureInfo const* cInfo = GetCreatureInfo();

#ifdef MANGOSBOT_TWO
    if (cInfo->Rank == CREATURE_ELITE_NORMAL && cInfo->MinLootGold == 0 && info.GetPosition().getMapId() == 609)
        return true;
#endif

    // wow-playground: the window moved to GetLevelWindow, which the grind spots share.
    int32 minLevel, maxLevel;
    GetLevelWindow(info, minLevel, maxLevel);

    if ((int32)cInfo->MaxLevel > maxLevel) //@lvl5 max = 3, @lvl60 max = 57
        return false;

    if ((int32)cInfo->MaxLevel < minLevel) //@lvl5 min = 3, @lvl60 max = 50
        return false;

    if (cInfo->MinLootGold == 0 && !info.IsAutonomous())
        return false;

    // wow-playground: a player's character leaves named rares and rare elites alone, Sethir the Ancient and Ursal the
    // Mauler on Teldrassil among them: one spawn, a long respawn, and often more than a solo character can take.
    if (info.IsAutonomous() && (cInfo->Rank == CREATURE_ELITE_RARE || cInfo->Rank == CREATURE_ELITE_RAREELITE))
        return false;

    if (cInfo->Rank > CREATURE_ELITE_NORMAL && !info.GetBoolValue("can fight elite"))
        return false;

    return true;
}

void GrindTravelDestination::GetLevelWindow(const PlayerTravelInfo& info, int32& minLevel, int32& maxLevel)
{
    int32 botLevel = info.GetLevel();

    uint8 botPowerLevel = info.GetUint8Value("durability");
    float levelMod = botPowerLevel / 500.0f; //(0-0.2f)
    float levelBoost = botPowerLevel / 50.0f; //(0-2.0f)

    maxLevel = std::max(botLevel * (0.5f + levelMod), botLevel - 5.0f + levelBoost);
    minLevel = std::max(botLevel * (0.4f + levelMod), botLevel - 12.0f + levelBoost);

    // wow-playground: a player's character grinds for experience: from three levels below its own, up to its own
    // level at full durability (two below with worn gear). A creature's experience falls off fast below the player's
    // level, by a level's share of the zero difference (8 at levels 12-15): three below gives 62%, one just above gray
    // a level 14 character under a percent of a level. Random bots grind three to six levels down for gold. Creatures
    // need not drop gold; their loot sells.
    if (info.IsAutonomous())
    {
        maxLevel = botLevel - 2 + int32(levelBoost);
        minLevel = std::max(int32(MaNGOS::XP::GetGrayLevel(botLevel)) + 1, botLevel - 3);
    }
}

GrindSpotTravelDestination::GrindSpotTravelDestination(uint32 id, Team team, const std::vector<AsyncGuidPosition>& spawns, bool hasElite)
    : TravelDestination(), id(id), team(team), hasElite(hasElite), spawns(spawns)
{
    SetExpireDelay((MAX_SECONDS + MINUTE) * IN_MILLISECONDS); // IsActive ends the work first, at MAX_SECONDS
    SetCooldownShort(); // on to the next spot, or a vendor, right away

    float meanX = 0, meanY = 0;
    std::map<uint32, uint32> counts;
    for (auto& spawn : spawns)
    {
        meanX += spawn.getX() / spawns.size();
        meanY += spawn.getY() / spawns.size();
        counts[spawn.GetEntry()]++;

        CreatureInfo const* cInfo = ObjectMgr::GetCreatureTemplate(spawn.GetEntry());
        if (!minLevel || cInfo->MinLevel < minLevel)
            minLevel = cInfo->MinLevel;
        if (cInfo->MaxLevel > maxLevel)
            maxLevel = cInfo->MaxLevel;
    }

    entries.assign(counts.begin(), counts.end());
    std::stable_sort(entries.begin(), entries.end(), [](auto& a, auto& b) { return a.second > b.second; });

    // The mean itself can be in the air above a slope or inside a rock; the spawn nearest to it stands on the ground.
    WorldPosition mean(spawns.front().getMapId(), meanX, meanY, spawns.front().getZ());
    point = *std::min_element(this->spawns.begin(), this->spawns.end(), [&mean](auto& a, auto& b) { return a.sqDistance2d(mean) < b.sqDistance2d(mean); });

    for (auto& spawn : spawns)
        radius = std::max(radius, spawn.distance(point));
    radius = std::max(radius, 20.0f);

    AddPoint(&point);
}

std::string GrindSpotTravelDestination::GetTitle() const
{
    std::ostringstream out;
    out << "grind spot [";
    for (uint32 i = 0; i < entries.size() && i < 3; i++)
    {
        CreatureInfo const* cInfo = ObjectMgr::GetCreatureTemplate(entries[i].first);
        out << (i ? ", " : "") << (cInfo ? cInfo->Name : "?");
    }
    if (entries.size() > 3)
        out << ", +" << (entries.size() - 3);
    out << "] L" << minLevel;
    if (maxLevel != minLevel)
        out << "-" << maxLevel;
    out << " (" << spawns.size() << " spawns";
    if (hasElite)
        out << ", elites near";
    out << ")";
    return out.str();
}

bool GrindSpotTravelDestination::IsPossible(const PlayerTravelInfo& info) const
{
    if (info.GetTeam() != team)
        return false;

    // As for the single entry grind destinations: the bot sells first. A spot also waits for a repair.
    if (info.GetBoolValue("should sell") && (info.GetBoolValue("can sell") || info.GetBoolValue("can ah sell")))
        return false;

    if (info.GetBoolValue("should repair") && info.GetBoolValue("can repair"))
        return false;

    if (hasElite && !info.GetBoolValue("can fight elite"))
        return false;

    int32 windowMin, windowMax;
    GrindTravelDestination::GetLevelWindow(info, windowMin, windowMax);

    // wow-playground: a spot's best creatures are within a level of the window's top, not only some creature inside the
    // window: overlap alone sent a level 12 druid to level 8-10 spots at the window's bottom, worth little experience.
    // Where no spot fits, the bot falls back to the single entry destinations of the whole window.
    if ((int32)maxLevel < windowMax - 1)
        return false;

    // Nor above it: a spot's toughest creature is at most a level over the window's top. The cells mix whatever stands
    // there, and the bot fights all of it; a level 16-29 spot at the Tower of Althalaxx had a level 23 warrior pull
    // level 29 Voidcallers and flee from them for half an hour.
    if ((int32)maxLevel > windowMax + 1)
        return false;

    return (int32)maxLevel >= windowMin && (int32)minLevel <= windowMax;
}

bool GrindSpotTravelDestination::IsHostileTo(Player* bot) const
{
    // The spot was built for the team by a representative race; this is the bot's own faction. Not friendly is
    // enough, as the build takes neutral creatures (LoadGrindSpots).
    return !GuidPosition(bot).IsFriendlyTo(GuidPosition(HIGHGUID_UNIT, entries.front().first), bot->GetInstanceId());
}

bool GrindSpotTravelDestination::IsBusy(Player* bot)
{
    if (bot->IsInCombat() || !bot->getAttackers().empty())
        return true;

    if (!bot->IsAlive())
        return false;

    // Eating, drinking, or a corpse to loot or skin.
    if (bot->IsSitState())
        return true;

    AiObjectContext* context = bot->GetPlayerbotAI()->GetAiObjectContext();
    return AI_VALUE(bool, "has available loot");
}

bool GrindSpotTravelDestination::IsActive(Player* bot, const PlayerTravelInfo& info) const
{
    PlayerbotAI* ai = bot->GetPlayerbotAI();
    if (!ai)
        return false;

    AiObjectContext* context = ai->GetAiObjectContext();

    if (!IsPossible(info) || !IsHostileTo(bot))
        return false;

    time_t now = time(0);
    time_t left = AI_VALUE2(time_t, "manual time", "grind spot left " + std::to_string(id));
    if (left && left + LEFT_SECONDS > now)
        return false;

    // wow-playground: a spot beyond MAX_DISTANCE, found when none nearer fits, needs a route the travel nodes know.
    if (!IsRoutable(bot))
        return false;

    TravelTarget* target = AI_VALUE(TravelTarget*, "travel target");

    // wow-playground: random bots crowd the starter zones, and a spot of three to five spawns feeds one character. A
    // spot with other players in it is not picked; the partitions then offer the next nearest, a little farther.
    if (target->GetDestination() != this)
    {
        if (CountOtherPlayers(bot))
            return false;

        // wow-playground: a spot whose creatures are all dead, as far as the bot's map has them loaded, waits.
        Map* map = bot->GetMapId() == point.getMapId() ? bot->GetMap() : nullptr;
        if (map && map->IsLoaded(point.getX(), point.getY()) && !CountAlive(bot))
            return false;

        return true;
    }

    if (target->GetStatus() != TravelStatus::TRAVEL_STATUS_WORK)
        return true;

    // Working the spot. It stays active while the bot keeps fighting, looting and resting.
    if (IsBusy(bot))
        SET_AI_VALUE2(time_t, "manual time", "grind spot busy", now);

    uint32 worked = target->GetExpiredTime() / IN_MILLISECONDS;
    time_t busy = std::max(AI_VALUE2(time_t, "manual time", "grind spot busy"), now - (time_t)worked);
    uint32 idle = now - busy;

    // wow-playground: another player taking the spot's creatures: half of them or more dead while someone else is
    // there, for CROWDED_SECONDS on end. The bot moves on instead of walking between empty spawn points.
    uint32 crowdedFor = 0;
    if (CountAlive(bot) * 2 <= spawns.size() && CountOtherPlayers(bot))
    {
        time_t crowded = AI_VALUE2(time_t, "manual time", "grind spot crowded");
        if (!crowded || crowded < now - (time_t)worked)
            SET_AI_VALUE2(time_t, "manual time", "grind spot crowded", crowded = now);
        crowdedFor = now - crowded;
    }
    else
        SET_AI_VALUE2(time_t, "manual time", "grind spot crowded", 0);

    std::string reason;
    if (idle >= IDLE_SECONDS)
        reason = "nothing to fight for " + std::to_string(idle) + "s";
    else if (worked >= MAX_SECONDS)
        reason = "worked it for " + std::to_string(worked / MINUTE) + " minutes";
    else if (crowdedFor >= CROWDED_SECONDS)
        reason = "others take its creatures, " + std::to_string(crowdedFor) + "s";

    if (reason.empty())
        return true;

    Leave(bot, reason);
    return false;
}

void GrindSpotTravelDestination::Leave(Player* bot, const std::string& reason) const
{
    PlayerbotAI* ai = bot->GetPlayerbotAI();
    AiObjectContext* context = ai->GetAiObjectContext();
    SET_AI_VALUE2(time_t, "manual time", "grind spot left " + std::to_string(id), time(0));
    ai->TellDebug(ai->GetMaster(), "Leaving " + GetTitle() + ": " + reason + ".", "debug travel");
}

uint32 GrindSpotTravelDestination::CountOtherPlayers(Player* bot) const
{
    if (bot->GetMapId() != point.getMapId() || !bot->GetMap())
        return 0;

    // The players of the bot's own map, a short list next to the map's creatures, walked on the bot's map thread.
    float range = radius + CROWD_MARGIN;
    uint32 count = 0;
    Group* group = bot->GetGroup();
    for (auto& ref : bot->GetMap()->GetPlayers())
    {
        Player* player = ref.getSource();
        if (!player || player == bot || !player->IsInWorld() || !player->IsAlive() || player->IsTaxiFlying())
            continue;

        if (group && group->IsMember(player->GetObjectGuid()))
            continue;

        // One far above the spot's creatures gets nothing from them and does not stay.
        if (player->GetLevel() > maxLevel + 10)
            continue;

        if (point.sqDistance2d(WorldPosition(player)) <= range * range)
            count++;
    }

    return count;
}

uint32 GrindSpotTravelDestination::CountAlive(Player* bot) const
{
    uint32 alive = 0;
    for (auto& spawn : spawns)
    {
        Creature* creature = spawn.GetCreature(bot->GetInstanceId());
        if (creature && creature->IsInWorld() && creature->IsAlive())
            alive++;
    }
    return alive;
}

bool GrindSpotTravelDestination::IsRoutable(Player* bot) const
{
    WorldPosition botPos(bot);
    if (botPos.getMapId() == point.getMapId() && botPos.distance(point) <= MAX_DISTANCE)
        return true;

    // The nearest node to each end; the travel nodes link walks, flights, boats, zeppelins and portals.
    std::vector<TravelNode*> fromNodes = sTravelNodeMap.getNodes(botPos, 500.0f);
    std::vector<TravelNode*> toNodes = sTravelNodeMap.getNodes(point, 500.0f);
    if (fromNodes.empty() || toNodes.empty())
        return false;

    return fromNodes.front()->hasRouteTo(toNodes.front());
}

bool GrindSpotTravelDestination::IsIn(const WorldPosition& pos, float radius) const
{
    // Inside the spot is close enough to start working it; the creatures are around.
    return point.getMapId() == pos.getMapId() && point.distance(pos) <= (radius ? radius : this->radius);
}

const GuidPosition* GrindSpotTravelDestination::GetRoamPoint(Player* bot) const
{
    WorldPosition botPos(bot);
    std::vector<const GuidPosition*> alive, others;
    for (auto& spawn : spawns)
    {
        if (spawn.sqDistance2d(botPos) < 15.0f * 15.0f)
            continue;

        Creature* creature = spawn.GetCreature(bot->GetInstanceId());
        if (creature && creature->IsInWorld() && creature->IsAlive())
            alive.push_back(&spawn);
        else
            others.push_back(&spawn);
    }

    std::vector<const GuidPosition*>& points = alive.empty() ? others : alive;
    if (points.empty())
        return nullptr;

    return points[urand(0, points.size() - 1)];
}

std::string GrindSpotTravelDestination::GetWorkStatus(Player* bot) const
{
    AiObjectContext* context = bot->GetPlayerbotAI()->GetAiObjectContext();
    TravelTarget* target = AI_VALUE(TravelTarget*, "travel target");

    std::ostringstream out;
    out << "grind spot " << id << ": " << CountAlive(bot) << "/" << spawns.size() << " alive, radius " << uint32(radius) << "y";
    out << ", " << CountOtherPlayers(bot) << " other players";

    if (target->GetDestination() == this && target->GetStatus() == TravelStatus::TRAVEL_STATUS_WORK)
    {
        time_t now = time(0);
        uint32 worked = target->GetExpiredTime() / IN_MILLISECONDS;
        time_t busy = std::max(AI_VALUE2(time_t, "manual time", "grind spot busy"), now - (time_t)worked);
        out << " | working " << worked / MINUTE << "m" << worked % MINUTE << "s of " << MAX_SECONDS / MINUTE << "m";
        out << " | no fight for " << uint32(now - busy) << "s of " << IDLE_SECONDS << "s";
        if (time_t crowded = AI_VALUE2(time_t, "manual time", "grind spot crowded"))
            out << " | crowded for " << uint32(now - crowded) << "s of " << CROWDED_SECONDS << "s";
    }

    return out.str();
}

bool GrindTravelDestination::IsActive(Player* bot, const PlayerTravelInfo& info) const
{
    PlayerbotAI* ai = bot->GetPlayerbotAI();
    AiObjectContext* context = ai->GetAiObjectContext();

    if (!IsPossible(info))
        return false;

    return GuidPosition(bot).IsHostileTo(GuidPosition(HIGHGUID_UNIT, GetEntry()), bot->GetInstanceId());
}

std::string GrindTravelDestination::GetTitle() const
{
    std::ostringstream out;

    out << "get xp or gold from killing ";

    out << ChatHelper::formatWorldEntry(GetEntry());

    return out.str();
}

bool BossTravelDestination::IsPossible(const PlayerTravelInfo& info) const
{
    if (!info.GetBoolValue("can fight boss"))
        return false;

    CreatureInfo const* cInfo = ObjectMgr::GetCreatureTemplate(GetEntry());

    if ((int32)cInfo->MaxLevel > info.GetLevel() + 3)
        return false;
    
    const MapEntry* mapEntry = ClosetMapEntry(info.GetPosition());
    
    if (info.IsInGroup())
    {
        if (info.IsInRaid())
        {
#ifndef MANGOSBOT_TWO
            if (mapEntry && mapEntry->IsNonRaidDungeon())
#else
            if (mapEntry && mapEntry->IsNonRaidDungeon())
#endif
                return false;
        }
        else if (mapEntry && mapEntry->IsRaid())
            return false;
    }

    //Ragefire casm
    if (ClosestMapId(info.GetPosition()) == 389 && info.GetTeam() == ALLIANCE)
        return false;

    //Stockades
    if (ClosestMapId(info.GetPosition()) == 34 && info.GetTeam() == HORDE)
        return false;

    //Do not move to overworld bosses/uniques that are far away.
    if (mapEntry && mapEntry->IsContinent() && DistanceTo(info.GetPosition()) > 2000.0f)
        return false;

    return true;
}

bool BossTravelDestination::IsActive(Player* bot, const PlayerTravelInfo& info) const
{
    PlayerbotAI* ai = bot->GetPlayerbotAI();
    AiObjectContext* context = ai->GetAiObjectContext();

    if (!IsPossible(info))
        return false;

    if (!GuidPosition(bot).IsHostileTo(GuidPosition(HIGHGUID_UNIT, GetEntry()), bot->GetInstanceId()))
        return false;

    WorldPosition botPos(bot);

    if (!IsOut(botPos))
    {
        std::list<ObjectGuid> targets = AI_VALUE(std::list<ObjectGuid>, "possible targets");

        for (auto& target : targets)
            if (target.GetEntry() == GetEntry() && target.IsCreature() && ai->GetCreature(target) && ai->GetCreature(target)->IsAlive())
                return true;

        return false;
    }

    if (!AI_VALUE2(bool, "has upgrade",  GetEntry()))
        return false;

    return true;
}

std::string BossTravelDestination::GetTitle() const
{
    std::ostringstream out;

    out << "get loot from ";

    out << ChatHelper::formatWorldEntry(GetEntry());

    return out.str();
}

bool GatherTravelDestination::IsPossible(const PlayerTravelInfo& info) const
{
    uint32 skillId = SKILL_NONE;
    uint32 reqSkillValue = 0;

    if (GetPurpose() == TravelDestinationPurpose::GatherFishing)
    {
        skillId = SKILL_FISHING;

        int32 reqSkillValue = sObjectMgr.GetFishingBaseSkillLevel(GetEntry());

        if (!reqSkillValue) //no fishable zone or area should be 0
            return false;

        if (reqSkillValue > 5)
            reqSkillValue -= 5; //Give chance to levelup.
    }
    else if (GetEntry() > 0)
    {
        CreatureInfo const* cInfo = GetCreatureInfo();

        if (!cInfo)
            return false;

        skillId = cInfo->GetRequiredLootSkill();
        uint32 targetLevel = cInfo->MaxLevel;
        reqSkillValue = targetLevel < 10 ? 1 : targetLevel < 20 ? (targetLevel - 10) * 10 : targetLevel * 5;
    }
    else
    {
        GameObjectInfo const* goInfo = GetGoInfo();

        if (!goInfo)
            return false;

        uint32 lockId = goInfo->GetLockId();
        LockEntry const* lockInfo = sLockStore.LookupEntry(lockId);
        if (!lockInfo)
            return false;

        for (int i = 0; i < 8; ++i)
        {
            if (lockInfo->Type[i] == LOCK_KEY_SKILL)
                if (SkillByLockType(LockType(lockInfo->Index[i])) > 0)
                {
                    skillId = SkillByLockType(LockType(lockInfo->Index[i]));
                    reqSkillValue = std::max((uint32)1, lockInfo->Skill[i]);
                    break;
                }
        }
    }

    if (!info.GetCurrentSkill((SkillType)skillId))
        return false;

    uint32 skillValue = uint32(info.GetCurrentSkill((SkillType)skillId));
    if (reqSkillValue > skillValue)
        return false;

    // wow-playground: a farming bot wants the materials, not the skill, so a node it cannot learn from still counts.
    // A bot on a farm order (FarmOrders) wants its one resource, so even a gray one counts.
    bool farmOrder = info.GetBoolValue("has farm order");
    if (info.GetSkillMax((SkillType)skillId) <= skillValue && !info.GetBoolValue("has strategy::farm") && !farmOrder) //Not able to increase skill.
        return false;

    if (GetPurpose() != TravelDestinationPurpose::GatherFishing && reqSkillValue + 100 < skillValue && !farmOrder) //Gray level = no skillup
        return false;

    return true;
}

bool GatherTravelDestination::IsActive(Player* bot, const PlayerTravelInfo& info) const
{
    PlayerbotAI* ai = bot->GetPlayerbotAI();
    AiObjectContext* context = ai->GetAiObjectContext();

    if (!IsPossible(info))
        return false;   


    if (GetPurpose() != TravelDestinationPurpose::GatherFishing)
    {
        if (!IsOut(bot))
        {
            if (this->GetEntry() > 0)
            {
                std::list<ObjectGuid> targets = AI_VALUE(std::list<ObjectGuid>, "possible targets");

                for (auto& target : targets)
                    if (target.GetEntry() == GetEntry() && target.IsCreature() && ai->GetCreature(target) && ai->GetCreature(target)->IsAlive())
                        return true;
            }
            else
            {
                std::list<ObjectGuid> targets = AI_VALUE(std::list<ObjectGuid>, "nearest game objects no los");

                for (auto& target : targets)
                    if (target.GetEntry() == GetEntry())
                        return true;
            }

            return false;
        }
    }

    return true;
}

std::string GatherTravelDestination::GetTitle() const {
    std::ostringstream out;

    if (GetPurpose() == TravelDestinationPurpose::GatherFishing)
    {
        out << "fish";
    }
    else
    {   
        switch (GetPurpose())
        {
            case TravelDestinationPurpose::GatherSkinning:
                out << "skin ";
                break;
            case TravelDestinationPurpose::GatherMining:
                out << "mine ";
                break;
            case TravelDestinationPurpose::GatherHerbalism:
                out << "gather from ";
                break;
        }

        out << ChatHelper::formatWorldEntry(GetEntry());
    }

    return out.str();
}

TravelTarget::TravelTarget(PlayerbotAI* ai) : AiObject(ai)
{
    sTravelMgr.SetNullTravelTarget(this);
    SetStatus(TravelStatus::TRAVEL_STATUS_EXPIRED);
}

void TravelTarget::SetTarget(TravelDestination* tDestination1, WorldPosition* wPosition1) {
    if (dynamic_cast<TemporaryTravelDestination*>(tDestination) && tDestination1 != tDestination)
        delete tDestination;

    wPosition = wPosition1;
    tDestination = tDestination1;

    SetStatus(TravelStatus::TRAVEL_STATUS_TRAVEL);
}

void TravelTarget::CopyTarget(TravelTarget* const target) {
    SetTarget(target->tDestination, target->wPosition);
    groupMember = target->groupMember;
    forced = target->forced;
    relevance = target->relevance;
    extendRetryCount = target->extendRetryCount;
}

void TravelTarget::SetStatus(TravelStatus status) {
    m_status = status;
    startTime = WorldTimer::getMSTime();

    switch (m_status) {
    case TravelStatus::TRAVEL_STATUS_NONE:
    case TravelStatus::TRAVEL_STATUS_PREPARE:
    case TravelStatus::TRAVEL_STATUS_EXPIRED:
        statusTime = 1;
        break;
    case TravelStatus::TRAVEL_STATUS_READY:
        statusTime = HOUR *  1000;
        break;
    case TravelStatus::TRAVEL_STATUS_TRAVEL:
        statusTime = GetMaxTravelTime() * 2 + sPlayerbotAIConfig.maxWaitForMove;
        break;
    case TravelStatus::TRAVEL_STATUS_WORK:
        statusTime = tDestination->GetExpireDelay();
        break;
    case TravelStatus::TRAVEL_STATUS_COOLDOWN:
        statusTime = tDestination->GetCooldownDelay();
    default: break;
    }
}

bool TravelTarget::IsDestinationActive()
{
    Player* player = bot;
    if (groupMember)
    {
        Player* member = groupMember.GetPlayer();
        if (!ai->IsSafe(member)) //unknown
            return true;
        else
            player = member;
    }

    if (!player->GetPlayerbotAI()) //No ai so clear target.
        return false;

    return tDestination->IsActive(player, PlayerTravelInfo(player));
}

bool TravelTarget::IsConditionsActive(bool clear)
{
    Player* player = bot;
    if (groupMember)
    {
        Player* member = groupMember.GetPlayer();
        if (!ai->IsSafe(member)) //unknown
            return true;
        else
            player = member;
    }

    if (!player || !player->GetPlayerbotAI()) //No ai so clear target.
        return false;
        
    AiObjectContext* playerContext = player->GetPlayerbotAI()->GetAiObjectContext();

    if (!playerContext)
        return false;

    if (clear)
        for (auto& condition : travelConditions)
            playerContext->ClearValues(condition);

    for (auto& condition : travelConditions)
    {
        auto* value = playerContext->GetValue<bool>(condition);
        if (!value || !value->Get())
            return false;
    }

    return true;
}

void TravelTarget::CheckStatus()
{
    if (!IsActive())
        return;

    if (groupMember)
    {
        Player* member = groupMember.GetPlayer();
        if (!member || (ai->IsSafe(member) && member->GetGroup() != bot->GetGroup()))
        {
            ai->TellDebug(ai->GetMaster(), "Travel target expired because it was from a player that is no longer in the group.", "debug travel");
            SetStatus(TravelStatus::TRAVEL_STATUS_EXPIRED);
            return;
        }
    }

    if (!ai->HasStrategy("travel", BotState::BOT_STATE_NON_COMBAT) && !ai->HasStrategy("travel once", BotState::BOT_STATE_NON_COMBAT))
    {
        ai->TellDebug(ai->GetMaster(), "The target is clearing because it was a travel once destination.", "debug travel");
        sTravelMgr.SetNullTravelTarget(this);
        return;
    }

    if (statusTime != 0 && GetTimeLeft() <= 0 && !IsForced())
    {
        ai->TellDebug(ai->GetMaster(), "Travel target expired because the status time was exceeded.", "debug travel");
        SetStatus(TravelStatus::TRAVEL_STATUS_EXPIRED);
        ai->GetAiObjectContext()->ClearValues("no active travel destinations");
        return;
    }

    if (GetStatus() == TravelStatus::TRAVEL_STATUS_TRAVEL)
    {
        bool HasArrived = tDestination->IsIn(bot);

        if (HasArrived)
        {
            if (ai->HasStrategy("travel once", BotState::BOT_STATE_NON_COMBAT))
            {
                ai->TellDebug(ai->GetMaster(), "The target is clearing because it was a travel once destination.", "debug travel");
                ai->ChangeStrategy("nc -travel once", BotState::BOT_STATE_NON_COMBAT);
                sTravelMgr.SetNullTravelTarget(this);
                return;
            }

            ai->TellDebug(ai->GetMaster(), "The target is starting to work because the destination has been reached.", "debug travel");
            SetStatus(TravelStatus::TRAVEL_STATUS_WORK);
            return;
        }
        else if(IsForced()) return; //While traveling do not go into cooldown
    }

    if (GetStatus() != TravelStatus::TRAVEL_STATUS_COOLDOWN)
    {
        bool destinationInactive = !IsDestinationActive() && !IsForced();
        bool conditionsInactive = !destinationInactive && !IsConditionsActive(); // Only check conditions if destination is still active

        if (destinationInactive || conditionsInactive)
        {
            ai->TellDebug(ai->GetMaster(), "The target is cooling down because the destination was no longer active or the conditions are no longer true.", "debug travel");
            forced = false;
            SetStatus(TravelStatus::TRAVEL_STATUS_COOLDOWN);
            return;
        }
    }
}

bool TravelTarget::IsActive() {
    if (m_status == TravelStatus::TRAVEL_STATUS_NONE || m_status == TravelStatus::TRAVEL_STATUS_EXPIRED || m_status == TravelStatus::TRAVEL_STATUS_PREPARE)
        return false;

    return true;
};

TravelState TravelTarget::GetTravelState() {
    if (!tDestination || typeid(*tDestination) == typeid(NullTravelDestination))
        return TravelState::TRAVEL_STATE_IDLE;

    if (typeid(*tDestination) == typeid(QuestRelationTravelDestination))
    {
        if (((QuestRelationTravelDestination*)tDestination)->GetRelation() == 0)
        {
            if (GetStatus() == TravelStatus::TRAVEL_STATUS_TRAVEL || GetStatus() == TravelStatus::TRAVEL_STATUS_PREPARE)
                return TravelState::TRAVEL_STATE_TRAVEL_PICK_UP_QUEST;
            if (GetStatus() == TravelStatus::TRAVEL_STATUS_WORK)
                return TravelState::TRAVEL_STATE_WORK_PICK_UP_QUEST;
        }
        else
        {
            if (GetStatus() == TravelStatus::TRAVEL_STATUS_TRAVEL || GetStatus() == TravelStatus::TRAVEL_STATUS_PREPARE)
                return TravelState::TRAVEL_STATE_TRAVEL_HAND_IN_QUEST;
            if (GetStatus() == TravelStatus::TRAVEL_STATUS_WORK)
                return TravelState::TRAVEL_STATE_WORK_HAND_IN_QUEST;
        }
    }
    else if (typeid(*tDestination) == typeid(QuestObjectiveTravelDestination))
    {
        if (GetStatus() == TravelStatus::TRAVEL_STATUS_TRAVEL || GetStatus() == TravelStatus::TRAVEL_STATUS_PREPARE)
            return TravelState::TRAVEL_STATE_TRAVEL_DO_QUEST;
        if (GetStatus() == TravelStatus::TRAVEL_STATUS_WORK)
            return TravelState::TRAVEL_STATE_WORK_DO_QUEST;
    }
    else if (typeid(*tDestination) == typeid(RpgTravelDestination))
    {
        return TravelState::TRAVEL_STATE_TRAVEL_RPG;
    }
    else if (typeid(*tDestination) == typeid(ExploreTravelDestination))
    {
        return TravelState::TRAVEL_STATE_TRAVEL_EXPLORE;
    }

    return TravelState::TRAVEL_STATE_IDLE;
}

void TravelMgr::Clear()
{
#ifndef MANGOSBOT_ZERO
    sObjectAccessor.ExecuteOnAllPlayers([this](Player* plr) { TravelMgr::SetNullTravelTarget(plr); });
#else
    HashMapHolder<Player>::ReadGuard g(HashMapHolder<Player>::GetLock());
    HashMapHolder<Player>::MapType& m = sObjectAccessor.GetPlayers();
    for (HashMapHolder<Player>::MapType::iterator itr = m.begin(); itr != m.end(); ++itr)
        TravelMgr::SetNullTravelTarget(itr->second);
#endif
    for (auto& [purpose, entries] : destinationMap)
        for (auto& [id, dests] : entries)
            for (auto& dest : dests)
                delete dest;
    destinationMap.clear();
    pointsMap.clear();
    fishPoints.clear();

    // wow-playground: see GrindSpotTravelDestination.
    for (auto& spot : grindSpots)
        delete spot;
    grindSpots.clear();
}

int32 TravelMgr::GetAreaLevel(uint32 area_id)
{
    auto lev = areaLevels.find(area_id);

    if (lev != areaLevels.end())
        return lev->second;

    AreaTableEntry const* area = GetAreaEntryByAreaID(area_id);

    if (!area)
    {
        areaLevels[area_id] = -2;
        return -2;
    }

    //Get exploration level
    if (area->area_level) 
    {
        areaLevels[area_id] = area->area_level;
        return area->area_level;
    }


    int32 level = 0;
    uint32 cnt = 0;

    //Get sub-area's
    for (uint32 i = 0; i <= sAreaStore.GetNumRows(); i++)
    {
        AreaTableEntry const* subArea = GetAreaEntryByAreaID(i);

        if (!subArea || subArea->zone != area->ID)
            continue;

        int32 subLevel = GetAreaLevel(subArea->ID);

        if (!subLevel)
            continue;

        level += subLevel;

        cnt++;
    }

    if (cnt)
    {
        areaLevels[area_id] = std::max(uint32(1), level / cnt);
        return areaLevels[area_id];
    }

    //Get units avarage
    FactionTemplateEntry const* humanFaction = sFactionTemplateStore.LookupEntry(1);
    FactionTemplateEntry const* orcFaction = sFactionTemplateStore.LookupEntry(2);

    for (auto& creaturePair : WorldPosition().getCreaturesNear())
    {
        if (WorldPosition(creaturePair).GetArea() != area)
            continue;

        CreatureData const cData = creaturePair->second;
        CreatureInfo const* cInfo = ObjectMgr::GetCreatureTemplate(cData.id);

        if (!cInfo)
            continue;

        FactionTemplateEntry const* factionEntry = sFactionTemplateStore.LookupEntry(cInfo->Faction);
        ReputationRank reactionHum = PlayerbotAI::GetFactionReaction(humanFaction, factionEntry);
        ReputationRank reactionOrc = PlayerbotAI::GetFactionReaction(orcFaction, factionEntry);

        if (reactionHum > REP_NEUTRAL || reactionOrc > REP_NEUTRAL)
            continue;

        level += cInfo->MaxLevel;
        cnt++;
    }

    if (cnt)
    {
        areaLevels[area_id] = std::max(uint32(1),level / cnt);
        return areaLevels[area_id];
    }

    //Use parent zone value.
    if (area->zone)
    {
        areaLevels[area_id] = 0; //Set a temporary value so it wont be counted.
        level = GetAreaLevel(area->zone);
        areaLevels[area_id] = level;        
        return areaLevels[area_id];
    }

    areaLevels[area_id] = -1;

    return areaLevels[area_id];
}

void TravelMgr::LoadAreaLevels()
{
    if (!areaLevels.empty())
        return;

    WorldDatabase.PExecute("CREATE TABLE IF NOT EXISTS `ai_playerbot_zone_level` (`id` bigint(20) NOT NULL ,`level` bigint(20) NOT NULL,PRIMARY KEY(`id`))");

    std::string query = "SELECT id, level FROM ai_playerbot_zone_level";

    {
        auto result = WorldDatabase.PQuery("%s", query.c_str());

        std::vector<uint32> loadedAreas;

        if (result)
        {
            BarGoLink bar(result->GetRowCount());            

            do
            {
                Field* fields = result->Fetch();
                bar.step();

                areaLevels[fields[0].GetUInt32()] = fields[1].GetInt32();

                loadedAreas.push_back(fields[0].GetUInt32());
            } while (result->NextRow());

            sLog.outString(">> Loaded " SIZEFMTD " area levels.", areaLevels.size());
        }

        BarGoLink bar(sAreaStore.GetNumRows());
        WorldDatabase.BeginTransaction();
        for (uint32 i = 0; i < sAreaStore.GetNumRows(); ++i)    // areaflag numbered from 0
        {
            bar.step();
            if (AreaTableEntry const* area = sAreaStore.LookupEntry(i))
            {
                if (std::find(loadedAreas.begin(), loadedAreas.end(), area->ID) == loadedAreas.end())
                {
                    int32 level = sTravelMgr.GetAreaLevel(area->ID);

                    WorldDatabase.PExecute("INSERT INTO `ai_playerbot_zone_level` (`id`, `level`) VALUES ('%d', '%d')", area->ID, level);
                }
            }
        }
        WorldDatabase.CommitTransaction();
        if(areaLevels.size() > loadedAreas.size())
            sLog.outString(">> Generated " SIZEFMTD " areas.", areaLevels.size()- loadedAreas.size());
    }
}

void TravelMgr::SetMobAvoidArea()
{
    sLog.outString("-Apply mob avoidance maps");

    std::vector<std::future<void>> calculations;

    BarGoLink bar(sMapStore.GetNumRows());

    for (uint32 i = 0; i < sMapStore.GetNumRows(); ++i)
    {
        if (!sMapStore.LookupEntry(i))
            continue;
        
        uint32 mapId = sMapStore.LookupEntry(i)->MapID;
        calculations.push_back(std::async([this, mapId] { SetMobAvoidAreaMap(mapId); }));
        bar.step();
    }

    BarGoLink bar2(calculations.size());
    for (uint32 i = 0; i < calculations.size(); i++)
    {
        calculations[i].wait();
        bar2.step();
    }

    sLog.outString(">> Modified navmap areas for %d maps.", sMapStore.GetNumRows());
}

void TravelMgr::SetMobAvoidAreaMap(uint32 mapId) 
{
    PathFinder path(mapId, 0);
    FactionTemplateEntry const* humanFaction = sFactionTemplateStore.LookupEntry(1);
    FactionTemplateEntry const* orcFaction = sFactionTemplateStore.LookupEntry(2);

    std::vector<CreatureDataPair const*> creatures = WorldPosition(mapId, 1,1).getCreaturesNear();

    for (auto& creaturePair : creatures)
    {
        CreatureData const cData = creaturePair->second;
        CreatureInfo const* cInfo = ObjectMgr::GetCreatureTemplate(cData.id);

        if (!cInfo)
            continue;

        WorldPosition point = WorldPosition(cData.mapid, cData.posX, cData.posY, cData.posZ, cData.orientation);

        if (cInfo->NpcFlags > 0)
            continue;

        FactionTemplateEntry const* factionEntry = sFactionTemplateStore.LookupEntry(cInfo->Faction);
        ReputationRank reactionHum = PlayerbotAI::GetFactionReaction(humanFaction, factionEntry);
        ReputationRank reactionOrc = PlayerbotAI::GetFactionReaction(orcFaction, factionEntry);

        if (reactionHum >= REP_NEUTRAL || reactionOrc >= REP_NEUTRAL)
            continue;

        if (!point.getTerrain())
            continue;

        if (!point.loadMapAndVMap(0))
            continue;

        path.setArea(point.getMapId(), point.getX(), point.getY(), point.getZ(), 12, 50.0f);
        path.setArea(point.getMapId(), point.getX(), point.getY(), point.getZ(), 13, 20.0f);
    }
}

void TravelMgr::LoadQuestTravelTable()
{
    if (!sTravelMgr.destinationMap.empty())
        return;

    // Clearing store (for reloading case)
    Clear();

    sLog.outString("Loading trainable spells.");
    GAI_VALUE(trainableSpellMap*, "trainable spell map");
    GAI_VALUE(std::vector<MountValue>, "full mount list");
    
    sLog.outString("Loading object locations.");

    EntryGuidps guidpMap = GAI_VALUE(EntryGuidps, "entry guidps");

    sLog.outString("Finding possible travel destinations.");

    EntryQuestRelationMap eMap = GAI_VALUE(EntryQuestRelationMap, "entry quest relation");

    sLog.outString("Creating travel destinations.");

    BarGoLink bar(eMap.size());

    for (auto& [entry, relation] : eMap)
    {


        bar.step();
        for (auto& [questId, flag] : relation)
        {
            if (guidpMap.find(entry) == guidpMap.end())
            {
                sLog.outDebug("Entry %d for quest %d has no valid location.", entry, questId);
                continue;
            }

            std::vector<QuestTravelDestination*> locs;

            for (uint32 purposeFlagNr = 0; purposeFlagNr < 6; purposeFlagNr++)
            {
                TravelDestinationPurpose purposeFlag = (TravelDestinationPurpose)(1 << purposeFlagNr);
                if (flag & (uint32)purposeFlag)
                {
                    QuestTravelDestination* loc = nullptr;

                    if (purposeFlag == TravelDestinationPurpose::QuestGiver || purposeFlag == TravelDestinationPurpose::QuestTaker)
                        loc = AddDestination<QuestRelationTravelDestination>(entry, purposeFlag, questId);
                    else
                        loc = AddDestination<QuestObjectiveTravelDestination>(entry, purposeFlag, questId);

                    locs.push_back(loc);
                }
            }

            if (!locs.empty())
            {
                for (auto& guidP : guidpMap.at(entry))
                {
                    pointsMap.insert(std::make_pair(guidP.GetRawValue(), guidP));

                    for (auto tLoc : locs)
                    {
                        tLoc->AddPoint(&pointsMap.at(guidP.GetRawValue()));
                    }
                }
            }
        }
    }       

    sLog.outString("Loading all travel locations.");

    // wow-playground: a vendor or other business inside a hostile camp is no place to sell or train. The Defias
    // Profiteer, friendly to both teams, stands among the Defias of Moonbrook; a level 19 druid walked to him to sell
    // and fled from his neighbours for a quarter of an hour. Creatures hostile to both teams, by map and cell.
    std::map<std::tuple<uint32, int32, int32>, std::vector<WorldPosition>> campCells;
    {
        FactionTemplateEntry const* teamTemplates[2] = { sFactionTemplateStore.LookupEntry(1), sFactionTemplateStore.LookupEntry(2) }; // Human, Orc
        for (auto& [entry, guidps] : guidpMap)
        {
            if (entry <= 0)
                continue;

            CreatureInfo const* cInfo = ObjectMgr::GetCreatureTemplate(entry);
            if (!cInfo || cInfo->NpcFlags || cInfo->CreatureType == CREATURE_TYPE_CRITTER || cInfo->CreatureType == CREATURE_TYPE_TOTEM)
                continue;

            if (cInfo->ExtraFlags & (CREATURE_EXTRA_FLAG_INVISIBLE | CREATURE_EXTRA_FLAG_CIVILIAN))
                continue;

            for (auto& guidP : guidps)
            {
                if (!guidP.isOverworld() || guidP.GetGameEventId())
                    continue;

                CreatureData const* data = guidP.GetCreatureData();
                if (!data)
                    continue;

                CreatureSpawnTemplate const* spawnTemplate = data->spawnTemplate;
                uint32 faction = spawnTemplate && spawnTemplate->faction ? spawnTemplate->faction : cInfo->Faction;
                FactionTemplateEntry const* creatureTemplate = sFactionTemplateStore.LookupEntry(faction);
                if (!creatureTemplate)
                    continue;

                bool hostileToBoth = true;
                for (uint32 t = 0; t < 2; t++)
                    if (!teamTemplates[t] || PlayerbotAI::GetFactionReaction(teamTemplates[t], creatureTemplate) >= REP_NEUTRAL)
                        hostileToBoth = false;

                if (hostileToBoth)
                    campCells[std::make_tuple(guidP.getMapId(), (int32)floor(guidP.getX() / CAMP_RANGE), (int32)floor(guidP.getY() / CAMP_RANGE))].push_back(guidP);
            }
        }
    }

    auto inHostileCamp = [&campCells](const WorldPosition& point)
    {
        int32 cx = (int32)floor(point.getX() / CAMP_RANGE);
        int32 cy = (int32)floor(point.getY() / CAMP_RANGE);
        uint32 count = 0;
        for (int32 x = cx - 1; x <= cx + 1; x++)
            for (int32 y = cy - 1; y <= cy + 1; y++)
            {
                auto it = campCells.find(std::make_tuple(point.getMapId(), x, y));
                if (it == campCells.end())
                    continue;

                for (auto& creature : it->second)
                    if (creature.distance(point) <= CAMP_RANGE)
                        count++;
            }
        return count >= CAMP_MIN_CREATURES;
    };

    auto isBusiness = [](TravelDestinationPurpose purpose)
    {
        return purpose == TravelDestinationPurpose::Vendor || purpose == TravelDestinationPurpose::Repair || purpose == TravelDestinationPurpose::Mail ||
            purpose == TravelDestinationPurpose::Bank || purpose == TravelDestinationPurpose::Trainer || purpose == TravelDestinationPurpose::AH;
    };

    uint32 campSpawns = 0;
    std::set<int32> campEntries;

    for (auto& [entry, purpose] : GAI_VALUE(EntryTravelPurposeMap, "entry travel purpose"))
    {
        std::vector<TravelDestination*> dests;

        if (guidpMap.find(entry) == guidpMap.end())
            continue;

        static uint32 maxPurposeFlag = std::countr_zero((uint32)TravelDestinationPurpose::MaxFlag); 

        for (uint32 purposeFlagNr = 0; purposeFlagNr < maxPurposeFlag; purposeFlagNr++)
        {
            TravelDestinationPurpose purposeFlag = (TravelDestinationPurpose)(1 << purposeFlagNr);
            if (purpose & (uint32)purposeFlag)
            {
                switch (purposeFlag) {
                case TravelDestinationPurpose::GenericRpg:
                case TravelDestinationPurpose::Trainer:
                case TravelDestinationPurpose::Repair:
                case TravelDestinationPurpose::Vendor:
                case TravelDestinationPurpose::AH:
                case TravelDestinationPurpose::Mail:
                case TravelDestinationPurpose::Bank:
                    dests.push_back(AddDestination<RpgTravelDestination>(entry, purposeFlag));
                    break;
                case TravelDestinationPurpose::GatherSkinning:
                case TravelDestinationPurpose::GatherMining:
                case TravelDestinationPurpose::GatherHerbalism:
                    dests.push_back(AddDestination<GatherTravelDestination>(entry, purposeFlag));
                    break;
                case TravelDestinationPurpose::Grind:
                    dests.push_back(AddDestination<GrindTravelDestination>(entry, purposeFlag));
                    break;
                case TravelDestinationPurpose::Boss:
                    dests.push_back(AddDestination<BossTravelDestination>(entry, purposeFlag));
                    break;
                default:
                    break;
                };
            }
        }

        if (dests.empty())
            continue;

        for (auto& guidP : guidpMap.at(entry))
        {
            pointsMap.insert(std::make_pair(guidP.GetRawValue(), guidP));

            bool inCamp = guidP.isOverworld() && inHostileCamp(guidP);

            for (auto tLoc : dests)
            {
                if (inCamp && isBusiness(tLoc->GetPurpose()))
                {
                    campSpawns++;
                    campEntries.insert(entry);
                    continue;
                }

                tLoc->AddPoint(&pointsMap.at(guidP.GetRawValue()));
            }
        }       
    }     

    std::ostringstream campOut;
    for (int32 entry : campEntries)
        if (CreatureInfo const* cInfo = entry > 0 ? ObjectMgr::GetCreatureTemplate(entry) : nullptr)
            campOut << " " << cInfo->Name << ",";
    sLog.outString("Left %u business points of %u entries out, inside hostile camps:%s", campSpawns, (uint32)campEntries.size(), campOut.str().c_str());

    // wow-playground: see GrindSpotTravelDestination.
    sLog.outString("Loading grind spots.");
    LoadGrindSpots(guidpMap);

    sLog.outString("Loading Explore locations.");

    //Explore points
    for (auto& creatureDataPair : WorldPosition().getCreaturesNear())
    {
        ExploreTravelDestination* loc;

        AsyncGuidPosition point(creatureDataPair);

        if (!point.isValid())
            continue;

        AreaTableEntry const* area = point.GetArea();
       
        if (!area)
            continue;

        if (!area->exploreFlag)
            continue;

        point.FetchArea();

        pointsMap.insert_or_assign(point.GetRawValue(), point);

        loc = AddDestination<ExploreTravelDestination>(area->ID, TravelDestinationPurpose::Explore);
        loc->AddPoint(&pointsMap.at(point.GetRawValue()));
    }

    GetPopulatedGrids();

    //Analyse log files
    if (sPlayerbotAIConfig.hasLog("log_analysis.csv"))
    {
        sLog.outString("Running analysis.");
        LogAnalysis::RunAnalysis();
    }

    sLog.outString("Clearing log files.");

     //Clear these logs files
    sPlayerbotAIConfig.openLog("zones.csv", "w");
    sPlayerbotAIConfig.openLog("creatures.csv", "w");
    sPlayerbotAIConfig.openLog("gos.csv", "w");
    sPlayerbotAIConfig.openLog("map_offsets.csv", "w");
    sPlayerbotAIConfig.openLog("bot_movement.csv", "w");
    sPlayerbotAIConfig.openLog("bot_pathfinding.csv", "w");
    sPlayerbotAIConfig.openLog("pathfind_attempt.csv", "w");
    sPlayerbotAIConfig.openLog("pathfind_attempt_point.csv", "w");
    sPlayerbotAIConfig.openLog("pathfind_result.csv", "w");
    sPlayerbotAIConfig.openLog("load_map_grid.csv", "w");
    sPlayerbotAIConfig.openLog("strategy.csv", "w");

    sPlayerbotAIConfig.openLog("unload_grid.csv", "w");
    sPlayerbotAIConfig.openLog("unload_obj.csv", "w");
    sPlayerbotAIConfig.openLog("bot_events.csv", "w");
    sPlayerbotAIConfig.openLog("bot_heartbeat.csv", "w");
    sPlayerbotAIConfig.openLog("bot_reactions.csv", "w");
    sPlayerbotAIConfig.openLog("travel_map.csv", "w");
    sPlayerbotAIConfig.openLog("quest_map.csv", "w");
    sPlayerbotAIConfig.openLog("activity_pid.csv", "w");
    sPlayerbotAIConfig.openLog("deaths.csv", "w");
    sPlayerbotAIConfig.openLog("player_paths.csv", "w");
    sPlayerbotAIConfig.openLog("travel_destinations.csv", "w");
    sPlayerbotAIConfig.openLog("deadzone.csv", "w"); 
    std::string logsDir = sConfig.GetStringDefault("LogsDir");
    if (!logsDir.empty() && logsDir.back() != '/' && logsDir.back() != '\\')
        logsDir.append("/");
    time_t t = time(nullptr);
    tm* aTm = localtime(&t);
    char stamp[24];
    std::strftime(stamp, sizeof(stamp), "%Y%m%d_%H%M%S", aTm);
    std::rename((logsDir + "bot_test_results.log").c_str(),
                (logsDir + "bot_test_results_" + stamp + ".log").c_str());
    sPlayerbotAIConfig.openLog("bot_test_results.log", "w", true);
    

    if (sPlayerbotAIConfig.hasLog("activity_pid.csv"))
    {
        std::ostringstream out;
        out << "Timestamp,";

        out << "sWorld.GetCurrentDiff(),";
        out << "sWorld.GetAverageDiff(),";
        out << "sWorld.GetMaxDiff(),";
        out << "virtualMemUsedByMe" << ",";
        out << "activityPercentage,";
        out << "activityPercentageMod,";
        out << "activeBots,";
        out << "playerBots.size(),";
        out << "totalLevel,";
        out << "avarageLevel1-9,";
        out << "avarageLevel10-19,";
        out << "avarageLevel20-29,";
        out << "avarageLevel30-39,";
        out << "avarageLevel40-49,";
        out << "avarageLevel50-59,";
#ifdef MANGOSBOT_ZERO
        out << "avarageLevel60,";
#else
        out << "avarageLevel60-69,";
#ifdef MANGOSBOT_ONE
        out << "avarageLevel70,";
#else
        out << "avarageLevel70-79,";
        out << "avarageLevel80,";
#endif
#endif

        out << "totalGold,";
        out << "avarageGold,";
        out << "totalavarageGearScore,";
        out << "avarageGearScore";

        sPlayerbotAIConfig.log("activity_pid.csv", out.str().c_str());
    }

    sLog.outString("Loading travel nodes.");

    sTravelNodeMap.loadNodeStore();

    sTravelNodeMap.generateAll();

    sTravelNodeMap.printMap();
    sTravelNodeMap.printNodeStore();
    sTravelNodeMap.saveNodeStore();

    LoadFishLocations();

    if (sPlayerbotAIConfig.hasLog("map_offsets.csv"))
    {
        std::vector<uint32> mapIds;
        auto addMapId = [&mapIds](uint32 mapId)
        {
            for (uint32 seen : mapIds)
                if (seen == mapId)
                    return;
            mapIds.push_back(mapId);
        };

        addMapId(0);
        addMapId(1);
        addMapId(530);
        addMapId(571);

        for (auto& node : sTravelNodeMap.getNodes())
            if (!node->getPosition()->isOverworld())
                addMapId(node->getMapId());

        std::ostringstream out;
        out << "mapId,offsetX,offsetY\n";
        out << std::fixed << std::setprecision(4);
        for (uint32 mapId : mapIds)
        {
            WorldPosition offset = sTravelNodeMap.getMapOffset(mapId);
            out << mapId << "," << offset.getX() << "," << offset.getY() << "\n";
        }

        sPlayerbotAIConfig.log("map_offsets.csv", out.str().c_str());
    }

    //Creature/gos/zone export.
    if (sPlayerbotAIConfig.hasLog("creatures.csv"))
    {
        sLog.outString("Create creature overlay exports.");

        for (auto& creaturePair : WorldPosition().getCreaturesNear())
        {
            CreatureData const cData = creaturePair->second;
            CreatureInfo const* cInfo = ObjectMgr::GetCreatureTemplate(cData.id);

            if (!cInfo)
                continue;

            WorldPosition point = WorldPosition(cData.mapid, cData.posX, cData.posY, cData.posZ, cData.orientation);

            std::string name = cInfo->Name;
            name.erase(remove(name.begin(), name.end(), ','), name.end());
            name.erase(remove(name.begin(), name.end(), '\"'), name.end());

            std::ostringstream out;
            out << name << ",";
            point.printWKT(out);
            out << cInfo->MaxLevel << ",";
            out << cInfo->Rank << ",";
            out << cInfo->Faction << ",";
            out << cInfo->NpcFlags << ",";
            out << point.getAreaName() << ",";
            out << std::fixed;

            sPlayerbotAIConfig.log("creatures.csv", out.str().c_str());
        }
    }

    if (sPlayerbotAIConfig.hasLog("vmangoslines.csv"))
    {

        uint32 mapId = 0;
        std::vector<WorldPosition> pos;

            static float const topNorthSouthLimit[] = {
                2032.048340f, -6927.750000f,
                1634.863403f, -6157.505371f,
                1109.519775f, -5181.036133f,
                1315.204712f, -4096.020508f,
                1073.089233f, -3372.571533f,
                 825.833191f, -3125.778809f,
                 657.343994f, -2314.813232f,
                 424.736145f, -1888.283691f,
                 744.395813f, -1647.935425f,
                1424.160645f,  -654.948181f,
                1447.065308f,  -169.751358f,
                1208.715454f,   189.748703f,
                1596.240356f,   998.616699f,
                1577.923706f,  1293.419922f,
                1458.520264f,  1727.373291f,
                1591.916138f,  3728.139404f
            };

            pos.clear();

# define my_sizeof(type) ((char *)(&type+1)-(char*)(&type))

            int size = my_sizeof(topNorthSouthLimit) / my_sizeof(topNorthSouthLimit[0]);

            for (int32 i = 0; i < size-1; i=i+2)
            {
                if (topNorthSouthLimit[i] == 0)
                    break;
                pos.push_back(WorldPosition(mapId, topNorthSouthLimit[i], topNorthSouthLimit[i + 1], 0));
            }

            std::ostringstream out;
            out << "topNorthSouthLimit" << ",";
            WorldPosition().printWKT(pos,out,1);
            out << std::fixed;

            sPlayerbotAIConfig.log("vmangoslines.csv", out.str().c_str());

            static float const ironforgeAreaSouthLimit[] = {
                -7491.33f,  3093.74f,
                -7472.04f,  -391.88f,
                -6366.68f,  -730.10f,
                -6063.96f, -1411.76f,
                -6087.62f, -2190.21f,
                -6349.54f, -2533.66f,
                -6308.63f, -3049.32f,
                -6107.82f, -3345.30f,
                -6008.49f, -3590.52f,
                -5989.37f, -4312.29f,
                -5806.26f, -5864.11f
            };

            pos.clear();

            size = my_sizeof(ironforgeAreaSouthLimit) / my_sizeof(ironforgeAreaSouthLimit[0]);

            for (int32 i = 0; i < size - 1; i = i + 2)
            {
                if (ironforgeAreaSouthLimit[i] == 0)
                    break;
                pos.push_back(WorldPosition(mapId, ironforgeAreaSouthLimit[i], ironforgeAreaSouthLimit[i + 1], 0));
            }

            out.str("");
            out.clear();
            
            out << "ironforgeAreaSouthLimit" << ",";
            WorldPosition().printWKT(pos, out, 1);
            out << std::fixed;

            sPlayerbotAIConfig.log("vmangoslines.csv", out.str().c_str());

            static float const stormwindAreaNorthLimit[] = {
                 -8004.25f,  3714.11f,
                 -8075.00f, -179.00f,
                 -8638.00f, 169.00f,
                 -9044.00f, 35.00f,
                 -9068.00f, -125.00f,
                 -9094.00f, -147.00f,
                 -9206.00f, -290.00f,
                 -9097.00f, -510.00f,
                 -8739.00f, -501.00f,
                 -8725.50f, -1618.45f,
                 -9810.40f, -1698.41f,
                -10049.60f, -1740.40f,
                -10670.61f, -1692.51f,
                -10908.48f, -1563.87f,
                -13006.40f, -1622.80f,
                -12863.23f, -4798.42f
            };

            pos.clear();

            size = my_sizeof(stormwindAreaNorthLimit) / my_sizeof(stormwindAreaNorthLimit[0]);

            for (int32 i = 0; i < size - 1; i = i + 2)
            {
                if (stormwindAreaNorthLimit[i] == 0)
                    break;
                pos.push_back(WorldPosition(mapId, stormwindAreaNorthLimit[i], stormwindAreaNorthLimit[i + 1], 0));
            }

            out.str("");
            out.clear();

            out << "stormwindAreaNorthLimit" << ",";
            WorldPosition().printWKT(pos, out, 1);
            out << std::fixed;

            sPlayerbotAIConfig.log("vmangoslines.csv", out.str().c_str());

            static float const stormwindAreaSouthLimit[] = {
                 -8725.337891f,  3535.624023f,
                 -9525.699219f,   910.132568f,
                 -9796.953125f,   839.069580f,
                 -9946.341797f,   743.102844f,
                -10287.361328f,   760.076477f,
                -10083.828125f,   380.389893f,
                -10148.072266f,    80.056450f,
                -10014.583984f,  -161.638519f,
                 -9978.146484f,  -361.638031f,
                 -9877.489258f,  -563.304871f,
                 -9980.967773f, -1128.510498f,
                 -9991.717773f, -1428.793213f,
                 -9887.579102f, -1618.514038f,
                -10169.600586f, -1801.582031f,
                 -9966.274414f, -2227.197754f,
                 -9861.309570f, -2989.841064f,
                 -9944.026367f, -3205.886963f,
                 -9610.209961f, -3648.369385f,
                 -7949.329590f, -4081.389404f,
                 -7910.859375f, -5855.578125f
            };

            pos.clear();

            size = my_sizeof(stormwindAreaSouthLimit) / my_sizeof(stormwindAreaSouthLimit[0]);

            for (int32 i = 0; i < size - 1; i = i + 2)
            {
                if (stormwindAreaSouthLimit[i] == 0)
                    break;
                pos.push_back(WorldPosition(mapId, stormwindAreaSouthLimit[i], stormwindAreaSouthLimit[i + 1], 0));
            }

            out.str("");
            out.clear();

            out << "stormwindAreaSouthLimit" << ",";
            WorldPosition().printWKT(pos, out, 1);
            out << std::fixed;

            sPlayerbotAIConfig.log("vmangoslines.csv", out.str().c_str());
            
            mapId = 1;

            static float const teldrassilSouthLimit[] = {
            7916.0f,   3475.0f,
            7916.0f,   1000.0f,
            8283.0f,   -501.0f,
            8804.0f,   -1098.0f
            };

            pos.clear();

            size = my_sizeof(teldrassilSouthLimit) / my_sizeof(teldrassilSouthLimit[0]);

            for (int32 i = 0; i < size - 1; i = i + 2)
            {
                if (teldrassilSouthLimit[i] == 0)
                    break;
                pos.push_back(WorldPosition(mapId, teldrassilSouthLimit[i], teldrassilSouthLimit[i + 1], 0));
            }

            out.str("");
            out.clear();

            out << "teldrassilSouthLimit" << ",";
            WorldPosition().printWKT(pos, out, 1);
            out << std::fixed;

            sPlayerbotAIConfig.log("vmangoslines.csv", out.str().c_str());
       
            static float const northMiddleLimit[] = {
                  -2280.00f,  4054.00f,
                  -2401.00f,  2365.00f,
                  -2432.00f,  1338.00f,
                  -2286.00f,   769.00f,
                  -2137.00f,   662.00f,
                  -2044.54f,   489.86f,
                  -1808.52f,   436.39f,
                  -1754.85f,   504.55f,
                  -1094.55f,   651.75f,
                   -747.46f,   647.73f,
                   -685.55f,   408.43f,
                   -311.38f,   114.43f,
                   -358.40f,  -587.42f,
                   -377.92f,  -748.70f,
                   -512.57f,  -919.49f,
                   -280.65f, -1008.87f,
                    -81.29f,  -930.89f,
                    284.31f, -1105.39f,
                    568.86f,  -892.28f,
                   1211.09f, -1135.55f,
                    879.60f, -2110.18f,
                    788.96f, -2276.02f,
                    899.68f, -2625.56f,
                   1281.54f, -2689.42f,
                   1521.82f, -3047.85f,
                   1424.22f, -3365.69f,
                   1694.11f, -3615.20f,
                   2373.78f, -4019.96f,
                   2388.13f, -5124.35f,
                   2193.79f, -5484.38f,
                   1703.57f, -5510.53f,
                   1497.59f, -6376.56f,
                   1368.00f, -8530.00f
            };

            pos.clear();

            size = my_sizeof(northMiddleLimit) / my_sizeof(northMiddleLimit[0]);

            for (int32 i = 0; i < size - 1; i = i + 2)
            {
                if (northMiddleLimit[i] == 0)
                    break;
                pos.push_back(WorldPosition(mapId, northMiddleLimit[i], northMiddleLimit[i + 1], 0));
            }

            out.str("");
            out.clear();

            out << "northMiddleLimit" << ",";
            WorldPosition().printWKT(pos, out, 1);
            out << std::fixed;

            sPlayerbotAIConfig.log("vmangoslines.csv", out.str().c_str());

            static float const durotarSouthLimit[] = {
                    2755.00f, -3766.00f,
                    2225.00f, -3596.00f,
                    1762.00f, -3746.00f,
                    1564.00f, -3943.00f,
                    1184.00f, -3915.00f,
                     737.00f, -3782.00f,
                     -75.00f, -3742.00f,
                    -263.00f, -3836.00f,
                    -173.00f, -4064.00f,
                     -81.00f, -4091.00f,
                     -49.00f, -4089.00f,
                     -16.00f, -4187.00f,
                      -5.00f, -4192.00f,
                     -14.00f, -4551.00f,
                    -397.00f, -4601.00f,
                    -522.00f, -4583.00f,
                    -668.00f, -4539.00f,
                    -790.00f, -4502.00f,
                   -1176.00f, -4213.00f,
                   -1387.00f, -4674.00f,
                   -2243.00f, -6046.00f
            };

            pos.clear();

            size = my_sizeof(durotarSouthLimit) / my_sizeof(durotarSouthLimit[0]);

            for (int32 i = 0; i < size - 1; i = i + 2)
            {
                if (durotarSouthLimit[i] == 0)
                    break;
                pos.push_back(WorldPosition(mapId, durotarSouthLimit[i], durotarSouthLimit[i + 1], 0));
            }

            out.str("");
            out.clear();

            out << "durotarSouthLimit" << ",";
            WorldPosition().printWKT(pos, out, 1);
            out << std::fixed;

            sPlayerbotAIConfig.log("vmangoslines.csv", out.str().c_str());

            static float const valleyoftrialsSouthLimit[] = {
                    -324.00f, -3869.00f,
                    -774.00f, -3992.00f,
                    -965.00f, -4290.00f,
                    -932.00f, -4349.00f,
                    -828.00f, -4414.00f,
                    -661.00f, -4541.00f,
                    -521.00f, -4582.00f
            };

            pos.clear();

            size = my_sizeof(valleyoftrialsSouthLimit) / my_sizeof(valleyoftrialsSouthLimit[0]);

            for (int32 i = 0; i < size - 1; i = i + 2)
            {
                if (valleyoftrialsSouthLimit[i] == 0)
                    break;
                pos.push_back(WorldPosition(mapId, valleyoftrialsSouthLimit[i], valleyoftrialsSouthLimit[i + 1], 0));
            }

            out.str("");
            out.clear();

            out << "valleyoftrialsSouthLimit" << ",";
            WorldPosition().printWKT(pos, out, 1);
            out << std::fixed;

            sPlayerbotAIConfig.log("vmangoslines.csv", out.str().c_str());

            static float const middleToSouthLimit[] = {
                        -2402.01f,      4255.70f,
                    -2475.933105f,  3199.568359f, // Desolace
                    -2344.124023f,  1756.164307f,
                    -2826.438965f,   403.824738f, // Mulgore
                    -3472.819580f,   182.522476f, // Feralas
                    -4365.006836f, -1602.575439f, // the Barrens
                    -4515.219727f, -1681.356079f,
                    -4543.093750f, -1882.869385f, // Thousand Needles
                        -4824.16f,     -2310.11f,
                    -5102.913574f, -2647.062744f,
                    -5248.286621f, -3034.536377f,
                    -5246.920898f, -3339.139893f,
                    -5459.449707f, -4920.155273f, // Tanaris
                        -5437.00f,     -5863.00f
            };

            pos.clear();

            size = my_sizeof(middleToSouthLimit) / my_sizeof(middleToSouthLimit[0]);

            for (int32 i = 0; i < size - 1; i = i + 2)
            {
                if (middleToSouthLimit[i] == 0)
                    break;
                pos.push_back(WorldPosition(mapId, middleToSouthLimit[i], middleToSouthLimit[i + 1], 0));
            }

            out.str("");
            out.clear();

            out << "middleToSouthLimit" << ",";
            WorldPosition().printWKT(pos, out, 1);
            out << std::fixed;

            sPlayerbotAIConfig.log("vmangoslines.csv", out.str().c_str());

            static float const orgrimmarSouthLimit[] = {
                    2132.5076f, -3912.2478f,
                    1944.4298f, -3855.2583f,
                    1735.6906f, -3834.2417f,
                    1654.3671f, -3380.9902f,
                    1593.9861f, -3975.5413f,
                    1439.2548f, -4249.6923f,
                    1436.3106f, -4007.8950f,
                    1393.3199f, -4196.0625f,
                    1445.2428f, -4373.9052f,
                    1407.2349f, -4429.4145f,
                    1464.7142f, -4545.2875f,
                    1584.1331f, -4596.8764f,
                    1716.8065f, -4601.1323f,
                    1875.8312f, -4788.7187f,
                    1979.7647f, -4883.4585f,
                    2219.1562f, -4854.3330f
            };

            pos.clear();

            size = my_sizeof(orgrimmarSouthLimit) / my_sizeof(orgrimmarSouthLimit[0]);

            for (int32 i = 0; i < size - 1; i = i + 2)
            {
                if (orgrimmarSouthLimit[i] == 0)
                    break;
                pos.push_back(WorldPosition(mapId, orgrimmarSouthLimit[i], orgrimmarSouthLimit[i + 1], 0));
            }

            out.str("");
            out.clear();

            out << "orgrimmarSouthLimit" << ",";
            WorldPosition().printWKT(pos, out, 1);
            out << std::fixed;

            sPlayerbotAIConfig.log("vmangoslines.csv", out.str().c_str());

            static float const feralasThousandNeedlesSouthLimit[] = {
                    -6495.4995f, -4711.981f,
                    -6674.9995f, -4515.0019f,
                    -6769.5717f, -4122.4272f,
                    -6838.2651f, -3874.2792f,
                    -6851.1314f, -3659.1179f,
                    -6624.6845f, -3063.3843f,
                    -6416.9067f, -2570.1301f,
                    -5959.8466f, -2287.2634f,
                    -5947.9135f, -1866.5028f,
                    -5947.9135f,  -820.4881f,
                    -5876.7114f,    -3.5138f,
                    -5876.7114f,   917.6407f,
                    -6099.3603f,  1153.2884f,
                    -6021.8989f,  1638.1809f,
                    -6091.6176f,  2335.8892f,
                    -6744.9946f,  2393.4855f,
                    -6973.8608f,  3077.0281f,
                    -7068.7241f,  4376.2304f,
                    -7142.1211f,  4808.4331f
            };


            pos.clear();

            size = my_sizeof(feralasThousandNeedlesSouthLimit) / my_sizeof(feralasThousandNeedlesSouthLimit[0]);

            for (int32 i = 0; i < size - 1; i = i + 2)
            {
                if (feralasThousandNeedlesSouthLimit[i] == 0)
                    break;
                pos.push_back(WorldPosition(mapId, feralasThousandNeedlesSouthLimit[i], feralasThousandNeedlesSouthLimit[i + 1], 0));
            }

            out.str("");
            out.clear();

            out << "feralasThousandNeedlesSouthLimit" << ",";
            WorldPosition().printWKT(pos, out, 1);
            out << std::fixed;

            sPlayerbotAIConfig.log("vmangoslines.csv", out.str().c_str());

            mapId = 530;

            static float const ShattrathAreaSouthLimit[] = {
            -2493.8823f,  5761.6894f,
            -2593.7438f,  4768.7978f,
            -1831.5280f,  3383.5705f
            };

            pos.clear();

            size = my_sizeof(ShattrathAreaSouthLimit) / my_sizeof(ShattrathAreaSouthLimit[0]);

            for (int32 i = 0; i < size - 1; i = i + 2)
            {
                if (ShattrathAreaSouthLimit[i] == 0)
                    break;
                pos.push_back(WorldPosition(mapId, ShattrathAreaSouthLimit[i], ShattrathAreaSouthLimit[i + 1], 0));
            }

            out.str("");
            out.clear();

            out << "ShattrathAreaSouthLimit" << ",";
            WorldPosition().printWKT(pos, out, 1);
            out << std::fixed;

            sPlayerbotAIConfig.log("vmangoslines.csv", out.str().c_str());

            static float const HellfireZangarSouthLimit[] = {

            -531.47265f,  8697.5830f,
            -514.56945f,  7291.2763f,
            -404.92804f,  6976.7958f,
            -593.56475f,  6646.0634f,
            -856.75695f,  6318.5507f,
            -1166.2729f,  5799.7817f,
            -1007.9321f,  4761.1352f,
            -1831.5280f,  3383.5705f,
            -2135.1586f,  2335.4426f,
            -2179.3974f,  896.0285f,
            };

            pos.clear();

            size = my_sizeof(HellfireZangarSouthLimit) / my_sizeof(HellfireZangarSouthLimit[0]);

            for (int32 i = 0; i < size - 1; i = i + 2)
            {
                if (HellfireZangarSouthLimit[i] == 0)
                    break;
                pos.push_back(WorldPosition(mapId, HellfireZangarSouthLimit[i], HellfireZangarSouthLimit[i + 1], 0));
            }

            out.str("");
            out.clear();

            out << "HellfireZangarSouthLimit" << ",";
            WorldPosition().printWKT(pos, out, 1);
            out << std::fixed;

            sPlayerbotAIConfig.log("vmangoslines.csv", out.str().c_str());

            static float const BladeEdgeNetherSouthLimit[] = {
            2074.6831f,  8216.6113f,
            1248.3884f,  7472.7592f,
            1118.4877f,  6972.6821f,
            1212.2004f,  6106.2861f,
            1175.4729f,  5633.375f,
            1543.8314f,  3961.8886f,
            };

            pos.clear();

            size = my_sizeof(BladeEdgeNetherSouthLimit) / my_sizeof(BladeEdgeNetherSouthLimit[0]);

            for (int32 i = 0; i < size - 1; i = i + 2)
            {
                if (BladeEdgeNetherSouthLimit[i] == 0)
                    break;
                pos.push_back(WorldPosition(mapId, BladeEdgeNetherSouthLimit[i], BladeEdgeNetherSouthLimit[i + 1], 0));
            }

            out.str("");
            out.clear();

            out << "BladeEdgeNetherSouthLimit" << ",";
            WorldPosition().printWKT(pos, out, 1);
            out << std::fixed;

            sPlayerbotAIConfig.log("vmangoslines.csv", out.str().c_str());
    }

    if (sPlayerbotAIConfig.hasLog("gos.csv"))
    {
        sLog.outString("Create go overlay exports.");
        for (auto& gameObjectPair : WorldPosition().getGameObjectsNear())
        {
            GameObjectData const gData = gameObjectPair->second;
            auto data = sGOStorage.LookupEntry<GameObjectInfo>(gData.id);

            if (!data)
                continue;

            WorldPosition point = WorldPosition(gData.mapid, gData.posX, gData.posY, gData.posZ, gData.orientation);

            std::string name = data->name;
            name.erase(remove(name.begin(), name.end(), ','), name.end());
            name.erase(remove(name.begin(), name.end(), '\"'), name.end());

            std::ostringstream out;
            out << name << ",";
            point.printWKT(out);
            out << data->type << ",";
            out << point.getAreaName() << ",";
            out << std::fixed;

            sPlayerbotAIConfig.log("gos.csv", out.str().c_str());
        }
    }

    if (sPlayerbotAIConfig.hasLog("zones.csv"))
    {
        sLog.outString("Create zone overlay exports.");

        std::unordered_map<std::string, std::vector<WorldPosition>> zoneLocs;

        std::vector<WorldPosition> Locs = {};
        
        for (auto& cdp : WorldPosition().getCreaturesNear())
        {
            WorldPosition point(cdp);
            std::string name = std::to_string(point.getMapId()) + point.getAreaName();

            if (zoneLocs.find(name) == zoneLocs.end())
                zoneLocs.insert_or_assign(name, Locs);

            zoneLocs.at(name).push_back(point);            
        }        

        for (auto& loc : zoneLocs)
        {
            if (loc.second.empty())
                continue;

            if (!sTravelNodeMap.getMapOffset(loc.second.front().getMapId()) && loc.second.front().getMapId() != 0)
                continue;

            std::vector<WorldPosition> points = loc.second;;
           
            std::ostringstream out; 

            WorldPosition pos = WorldPosition(points, WP_MEAN_CENTROID);

            out << "\"center\"" << ",";
            out << points.begin()->getMapId() << ",";
            out << points.begin()->getAreaName() << ",";
            out << points.begin()->getAreaName(true, true) << ",";

            pos.printWKT(out);

            if(points.begin()->GetArea())
                out << std::to_string(points.begin()->getAreaLevel());
            else
                out << std::to_string(-1);

            out << "\n";
            
            out << "\"area\"" << ",";
            out << points.begin()->getMapId() << ",";
            out << points.begin()->getAreaName() << ",";
            out << points.begin()->getAreaName(true, true) << ",";

            WorldPosition().printWKT(points, out, 0);

            if (points.begin()->GetArea())
                out << std::to_string(points.begin()->getAreaLevel());
            else
                out << std::to_string(-1);

            sPlayerbotAIConfig.log("zones.csv", out.str().c_str());
        }
    }    

    if (sPlayerbotAIConfig.hasLog("telecache.csv"))
    {
        sLog.outString("Create telecache overlay exports.");

        sRandomPlayerbotMgr.PrintTeleportCache();
    }

    if (sPlayerbotAIConfig.hasLog("travel_destinations.csv"))
    {
        sLog.outString("Create travel destinations export.");

        for (auto& [purpose, entryDestinations] : destinationMap)
        {
            for (auto& [entry, destinations] : entryDestinations)
            {
                for (auto& destination : destinations)
                {
                    std::ostringstream out;

                    out << TravelDestinationPurposeName.at(purpose) << ",";
                    out << entry << ",";
                    out << destination->GetShortName() << ",";
                    out << "\"" << destination->GetTitle() << "\",";
                    out << destination->GetSize() << ",";
                    out << "\"MultiPoint(";
                    destination->printWKT(out, false);
                    out.seekp(-1, out.cur);
                    out << ")\"";
                    sPlayerbotAIConfig.log("travel_destinations.csv", out.str().c_str());
                }
            }
        }
    }

#ifndef MANGOSBOT_TWO    
    sTerrainMgr.Update(60 * 60 * 24);
#else
    for (uint32 i = 0; i < sMapStore.GetNumRows(); ++i)
    {
        if (!sMapStore.LookupEntry(i))
            continue;

        uint32 mapId = sMapStore.LookupEntry(i)->MapID;

        if (WorldPosition(mapId, 0, 0).getMap(0))
            continue;

        WorldPosition::unloadMapAndVMaps(mapId);
    }
#endif     
}

// wow-playground: see GrindSpotTravelDestination. Creature spawns are bucketed into cells of CELL_SIZE yards, height
// included so a cave is apart from the hill above it, for each team the creatures are hostile to. A cell with
// MIN_SPAWNS non-elite spawns becomes a spot; elites in the cell only mark it. A spawn's own faction, unit flags and npc
// flags (its spawn template) count over its creature's, since a captive or an escort is often the hostile creature's
// entry with a friendly faction or a flag that keeps it from being attacked.
void TravelMgr::LoadGrindSpots(const EntryGuidps& guidpMap)
{
    // A race of each team stands for it; IsActive checks the bot's own faction.
    FactionTemplateEntry const* teamTemplates[2] = { sFactionTemplateStore.LookupEntry(1), sFactionTemplateStore.LookupEntry(2) }; // Human, Orc
    const Team teams[2] = { ALLIANCE, HORDE };

    struct Cell
    {
        std::vector<AsyncGuidPosition> spawns;
        bool hasElite = false;
    };
    std::map<std::tuple<uint32, uint32, int32, int32, int32>, Cell> cells;
    // Creatures of each team's factions, by team index, map and cell of TOWN_DISTANCE.
    std::map<std::tuple<uint32, uint32, int32, int32>, std::vector<WorldPosition>> towns;

    uint32 spawnCount = 0;

    for (auto& [entry, guidps] : guidpMap)
    {
        if (entry <= 0)
            continue;

        CreatureInfo const* cInfo = ObjectMgr::GetCreatureTemplate(entry);
        if (!cInfo)
            continue;

        for (auto guidP : guidps)
        {
            if (!guidP.isOverworld())
                continue;

            CreatureData const* data = guidP.GetCreatureData();
            if (!data)
                continue;

            CreatureSpawnTemplate const* spawnTemplate = data->spawnTemplate;
            uint32 faction = spawnTemplate && spawnTemplate->faction ? spawnTemplate->faction : cInfo->Faction;
            uint32 unitFlags = spawnTemplate && spawnTemplate->unitFlags != -1 ? uint32(spawnTemplate->unitFlags) : cInfo->UnitFlags;
            uint32 npcFlags = spawnTemplate && spawnTemplate->npcFlags != -1 ? uint32(spawnTemplate->npcFlags) : cInfo->NpcFlags;

            FactionTemplateEntry const* creatureTemplate = sFactionTemplateStore.LookupEntry(faction);
            if (!creatureTemplate)
                continue;

            // A faction of a player team: guards, villagers, the other team's people. They are no grind, but they
            // mark the team's towns and camps, where neither team grinds.
            if (creatureTemplate->factionGroupMask & (FACTION_GROUP_MASK_ALLIANCE | FACTION_GROUP_MASK_HORDE))
            {
                for (uint32 t = 0; t < 2; t++)
                    if (creatureTemplate->factionGroupMask & (t ? FACTION_GROUP_MASK_HORDE : FACTION_GROUP_MASK_ALLIANCE))
                        towns[std::make_tuple(t, guidP.getMapId(), (int32)floor(guidP.getX() / GrindSpotTravelDestination::TOWN_DISTANCE),
                            (int32)floor(guidP.getY() / GrindSpotTravelDestination::TOWN_DISTANCE))].push_back(guidP);
                continue;
            }

            if (cInfo->CreatureType == CREATURE_TYPE_CRITTER || cInfo->CreatureType == CREATURE_TYPE_TOTEM || cInfo->CreatureType == CREATURE_TYPE_NOT_SPECIFIED)
                continue;

            if (npcFlags)
                continue;

            if (cInfo->ExtraFlags & (CREATURE_EXTRA_FLAG_INVISIBLE | CREATURE_EXTRA_FLAG_CIVILIAN | CREATURE_EXTRA_FLAG_GUARD))
                continue;

            if (unitFlags & (UNIT_FLAG_SPAWNING | UNIT_FLAG_IMMUNE_TO_PLAYER | UNIT_FLAG_UNTARGETABLE | UNIT_FLAG_UNINTERACTIBLE))
                continue;

            if (cInfo->StaticFlags & (uint32(CreatureStaticFlags::NO_XP) | uint32(CreatureStaticFlags::UNKILLABLE) | uint32(CreatureStaticFlags::IMMUNE_TO_PC) | uint32(CreatureStaticFlags::UNINTERACTIBLE)))
                continue;

            if (cInfo->ExperienceMultiplier <= 0.0f)
                continue;

            // Rares and rare elites never count: one spawn, a long respawn, often more than a solo character can take.
            // An elite, rare elites included, only marks the cell.
            if (cInfo->Rank == CREATURE_ELITE_RARE)
                continue;
            bool elite = cInfo->Rank != CREATURE_ELITE_NORMAL;

            // wow-playground: neutral creatures count too, the yellow ones a character attacks first: the night elves'
            // start in Shadowglen has only neutral creatures below level 3 (faction 189), so a level 2 priest found no
            // spot there and sailed for Elwynn's kobolds. A faction with a reputation is left alone when neutral.
#if defined(MANGOSBOT_ONE)
            const FactionEntry* factionEntry = sFactionStore.LookupEntry<FactionEntry>(creatureTemplate->faction);
#else
            const FactionEntry* factionEntry = sFactionStore.LookupEntry(creatureTemplate->faction);
#endif
            bool hasReputation = factionEntry && factionEntry->HasReputation();

            bool hostile[2];
            for (uint32 t = 0; t < 2; t++)
            {
                ReputationRank reaction = teamTemplates[t] ? PlayerbotAI::GetFactionReaction(teamTemplates[t], creatureTemplate) : REP_FRIENDLY;
                hostile[t] = reaction < REP_NEUTRAL || (reaction == REP_NEUTRAL && !hasReputation);
            }

            if (!hostile[0] && !hostile[1])
                continue;

            if ((int32)data->spawntimesecsmin < 0)
                continue;

            // Event spawns come and go.
            if (guidP.GetGameEventId())
                continue;

            AreaTableEntry const* area = guidP.GetArea();
            if (!area)
                continue;

            AreaTableEntry const* zone = area->zone ? GetAreaEntryByAreaID(area->zone) : area;
            const uint32 cityFlags = AREA_FLAG_CAPITAL | AREA_FLAG_CITY | AREA_FLAG_SLAVE_CAPITAL | AREA_FLAG_SLAVE_CAPITAL2;
            if ((area->flags & cityFlags) || (zone && (zone->flags & cityFlags)))
                continue;

            spawnCount++;

            for (uint32 t = 0; t < 2; t++)
            {
                if (!hostile[t])
                    continue;

                Cell& cell = cells[std::make_tuple((uint32)teams[t], guidP.getMapId(),
                    (int32)floor(guidP.getX() / GrindSpotTravelDestination::CELL_SIZE),
                    (int32)floor(guidP.getY() / GrindSpotTravelDestination::CELL_SIZE),
                    (int32)floor(guidP.getZ() / GrindSpotTravelDestination::CELL_SIZE))];

                if (elite)
                    cell.hasElite = true;
                else
                    cell.spawns.push_back(guidP);
            }
        }
    }

    // A spot near a team's town or camp has its guards and people around: the other team's attack the bot, and its
    // own take its kills, draw it into town on errands and stand among prisoners and quest creatures.
    auto nearTown = [&towns](uint32 townTeam, const WorldPosition& point, float range)
    {
        int32 cx = (int32)floor(point.getX() / GrindSpotTravelDestination::TOWN_DISTANCE);
        int32 cy = (int32)floor(point.getY() / GrindSpotTravelDestination::TOWN_DISTANCE);
        for (int32 x = cx - 1; x <= cx + 1; x++)
            for (int32 y = cy - 1; y <= cy + 1; y++)
            {
                auto it = towns.find(std::make_tuple(townTeam, point.getMapId(), x, y));
                if (it == towns.end())
                    continue;

                for (auto& npc : it->second)
                    if (npc.distance(point) <= range)
                        return true;
            }
        return false;
    };

    uint32 teamCount[2] = { 0, 0 }, nearEnemyTowns = 0, nearOwnTowns = 0;
    for (auto& [key, cell] : cells)
    {
        if (cell.spawns.size() < GrindSpotTravelDestination::MIN_SPAWNS)
            continue;

        Team team = Team(std::get<0>(key));
        uint32 t = team == ALLIANCE ? 0 : 1;
        GrindSpotTravelDestination* spot = new GrindSpotTravelDestination(grindSpots.size(), team, cell.spawns, cell.hasElite);

        if (nearTown(t ? 0 : 1, spot->GetPoint(), GrindSpotTravelDestination::TOWN_DISTANCE))
        {
            delete spot;
            nearEnemyTowns++;
            continue;
        }

        if (nearTown(t, spot->GetPoint(), GrindSpotTravelDestination::OWN_TOWN_DISTANCE))
        {
            delete spot;
            nearOwnTowns++;
            continue;
        }

        grindSpots.push_back(spot);
        teamCount[t]++;
    }

    sLog.outString("Loaded %u grind spots, %u for the alliance and %u for the horde, from %u creature spawns; %u left out near the other team's towns, %u near their own.",
        (uint32)grindSpots.size(), teamCount[0], teamCount[1], spawnCount, nearEnemyTowns, nearOwnTowns);
}

void TravelMgr::GetPopulatedGrids()
{
    sLog.outString("Finding populated grids.");

    BarGoLink bar(sMapStore.GetNumRows());      

    for (uint32 i = 0; i < sMapStore.GetNumRows(); ++i)
    {
        bar.step();

        if (!sMapStore.LookupEntry(i))
            continue;

        uint32 mapId = sMapStore.LookupEntry(i)->MapID;

        GetPopulatedGrids(mapId);
    }
}

void TravelMgr::GetPopulatedGrids(uint32 mapId)
{
    EntryGuidps guidpMap = GAI_VALUE(EntryGuidps, "entry guidps");

    for (auto& [entry, guidPs] : guidpMap)
    {
        for (auto& guidP : guidPs)
        {
            if (guidP.getMapId() == mapId)
            {
                populatedGrids[guidP.getMapId()][guidP.getGridPair().x_coord][guidP.getGridPair().y_coord] = true;
            }
        }
    }
}

uint32 TravelMgr::GetFishZone(const AsyncGuidPosition& pos) const
{
    uint32 zone, subzone;
    int32 areaFlag = pos.getAreaFlag();

    TerrainManager::GetZoneAndAreaIdByAreaFlag(zone, subzone, areaFlag, pos.getMapId());

    if (sObjectMgr.GetFishingBaseSkillLevel(subzone))
        return subzone;

    if (sObjectMgr.GetFishingBaseSkillLevel(zone))
        return zone;

    return 0;
}

void TravelMgr::LoadFishLocations()
{
    sLog.outString("Loading Fish locations.");

    fishPoints.clear();
    fishMap.clear();
    destinationMap[TravelDestinationPurpose::GatherFishing].clear();

    auto result = WorldDatabase.Query("SELECT `name`, `map_id`, `position_x`, `position_y`, `position_z`, `orientation`, `description` FROM `ai_playerbot_named_location` WHERE `name` LIKE 'FISH_LOCATION%'");

    if (!result)
    {
        sTravelNodeMap.setHasToGen();
        GetFishLocations();
        sTravelNodeMap.setHasToGen(false);
        SaveFishLocations();
        return;
    }

    uint32 count = 0;
    do
    {
        ++count;

        Field* fields = result->Fetch();

        std::string name = fields[0].GetCppString();
        uint32 mapId = fields[1].GetUInt32();
        float positionX = fields[2].GetFloat();
        float positionY = fields[3].GetFloat();
        float positionZ = fields[4].GetFloat();
        float orientation = fields[5].GetFloat();
        std::string description = fields[6].GetCppString();

        AsyncGuidPosition* point = &fishPoints.emplace_back(GuidPosition(0, WorldPosition(mapId, positionX, positionY, positionZ, orientation)));

        int32 areaFlag = stoi(description);
        point->setAreaFlag(areaFlag);
        uint32 zone = GetFishZone(*point);

        if (!zone)
            continue;

        TravelDestination* dest = AddDestination<GatherTravelDestination>(zone, TravelDestinationPurpose::GatherFishing);                

        dest->AddPoint(point);
        fishMap.AddPoint(point);

    } while (result->NextRow());

    sLog.outString(">> Loaded %u fish locations", count);
    sLog.outString();
}

void TravelMgr::GetFishLocations()
{
    sLog.outString("Generating Fish locations.");

    BarGoLink bar(1000);

    std::vector<std::future<void>> calculations;

    for (int32 mapId = 1000; mapId >= 0; mapId--)
    {       
        bool hashFishing = false;
        for (uint32 i = 0; i < sAreaStore.GetNumRows(); ++i)    // areaflag numbered from 0
        {
            AreaTableEntry const* area = sAreaStore.LookupEntry(i);

            if (!area)
                continue;

            if (area->mapid != mapId)
                continue;

            if (!sObjectMgr.GetFishingBaseSkillLevel(area->ID))
                continue;

            hashFishing = true;
            break;
        }

        if (!hashFishing)
            continue;

        calculations.push_back(std::async([this, mapId] { GetFishLocations(mapId); }));

        bar.step();
    }

    BarGoLink bar1(calculations.size());   

    for (uint32 i = 0; i < calculations.size(); i++)
    {
        calculations[i].wait();
        bar1.step();
    }
}

void TravelMgr::GetFishLocations(uint32 mapId)
{
    WorldPosition ironForge(0, -4832.27, -1069.64, 502.268);
    TravelNode* ironForgeNode = sTravelNodeMap.getNode(ironForge);
    WorldPosition orgrimmar(1, 1845.49, -4395.95, 5.19264);

    PathFinder path(mapId,0);

    const int8 subCellPerGrid = 64;
    const float sizeOfSubCell = SIZE_OF_GRIDS / subCellPerGrid;

    std::unordered_map<uint32, std::vector<AsyncGuidPosition>> fishSpots;

    for (uint8 gridx = 0; gridx < 64; ++gridx)
    {
        for (uint8 gridy = 0; gridy < 64; ++gridy)
        {
            if (!populatedGrids[mapId][gridx][gridy])
                continue;

            GridPair grid(gridx, gridy);

            WorldPosition gridPos(mapId, grid);

            for (uint8 cellx = 0; cellx < subCellPerGrid; cellx++)
            {
                for (uint8 celly = 0; celly < subCellPerGrid; celly++)
                {
                    //Get spot in water.
                    WorldPosition waterSpot = gridPos + WorldPosition(mapId, sizeOfSubCell * cellx, sizeOfSubCell * celly);

                    waterSpot.setZ(waterSpot.getHeight(true));

                    if (!waterSpot.isInWater())
                        continue;

                    //Get nearby location on ground.
                    WorldPosition fishSpot;
                    float botAngle = urand(0, 360);
                    bool inWater = true;
                    for (float angle = 0.0f; angle < 360.0f; angle += 45.0f)
                    {
                        fishSpot = waterSpot + WorldPosition(mapId, cos((angle + botAngle) / 180.0f * M_PI_F) * 10, sin((angle + botAngle) / 180.0f * M_PI_F) * 10);

                        fishSpot.setZ(fishSpot.getHeight(false));

                        if (!fishSpot.getTerrain())
                            continue;

                        if (fabs(fishSpot.getZ() - waterSpot.getZ()) > 10.0f)
                            continue;

                        uint32 area = path.getArea(mapId, fishSpot.getX(), fishSpot.getY(), fishSpot.getZ());

                        if (!area || area == NAV_AREA_GROUND_STEEP)
                        {
                        //    sLog.outError("no area %d %f %f %f", mapId, fishSpot.getX(), fishSpot.getY(), fishSpot.getZ());
                            continue;
                        }
                   
                        inWater = fishSpot.isInWater();

                        if (!inWater)
                            break;
                    }

                    if (inWater)
                        continue;

                    AsyncGuidPosition fishPos(GuidPosition(0, fishSpot));
                    fishPos.setO(fishPos.getAngleTo(waterSpot));

                    fishPos.FetchArea();
                    uint32 zone = GetFishZone(fishPos);

                    if (!zone)
                        continue;

                    std::vector<WorldPosition> startPath;
                    std::vector<WorldPosition> endPath;

                    if (fishPos.getMapId() == 0 && sTravelNodeMap.getRoute(ironForge, fishPos, startPath, endPath, nullptr).isEmpty())
                        continue;

                    if (fishPos.getMapId() == 1 && sTravelNodeMap.getRoute(orgrimmar, fishPos, startPath, endPath, nullptr).isEmpty())
                        continue;

                    if (fishPos.getMapId() > 1)
                    {
                        bool noPath = true;
                        for (auto& node : sTravelNodeMap.getNodes(fishPos))
                        {
                            if (!node->hasRouteTo(ironForgeNode))
                                continue;

                            if (!sTravelNodeMap.getFullPath(*node->getPosition(), fishPos).empty())
                            {
                                noPath = false;
                                break;
                            }
                        }

                        if (noPath)
                            continue;
                    }

                    fishSpots[zone].push_back(fishPos);                    
                }
            }
        }
    }

    getDestinationMutex.lock();
    for (auto& [zone, spots] : fishSpots)
    {
        TravelDestination* dest = AddDestination<GatherTravelDestination>(zone, TravelDestinationPurpose::GatherFishing);

        for (auto& spot : spots)
        {
            WorldPosition* point = &fishPoints.emplace_back(spot);
            dest->AddPoint(point);
            fishMap.AddPoint(point);
        }
    }
    getDestinationMutex.unlock();
}

void TravelMgr::SaveFishLocations()
{
    sLog.outString("Saving Fish locations.");

    //Save fishpoints sorted by mapId, areaFlag, X, Y.
    fishPoints.sort([](AsyncGuidPosition i, AsyncGuidPosition j) {
        if (i.getMapId() != j.getMapId()) return i.getMapId() > j.getMapId();
        if (i.getAreaFlag() != j.getAreaFlag()) return i.getAreaFlag() > j.getAreaFlag();
        if (i.getX() != j.getX()) return i.getX() > j.getX();
        return i.getY() > j.getY(); });

    WorldDatabase.BeginTransaction();

    BarGoLink bar(fishPoints.size());

    uint32 count = 0;    

    for (auto& fishPoint : fishPoints)
    {
        std::string name = "FISH_LOCATION_" + std::to_string(fishPoint.getMapId()) + "_" + std::to_string(fishPoint.getAreaFlag()) + "_" + std::to_string(count);
        std::string description = std::to_string(fishPoint.getAreaFlag());
        name.erase(remove(name.begin(), name.end(), '\''), name.end());

        WorldDatabase.PExecute("INSERT INTO `ai_playerbot_named_location` (`name`, `map_id`, `position_x`, `position_y`, `position_z`, `orientation`, `description`) VALUES ('%s', '%d', '%f', '%f', '%f', '%f', '%s')"
            , name.c_str(), fishPoint.getMapId(), fishPoint.getX(), fishPoint.getY(), fishPoint.getZ(), fishPoint.getO(), description.c_str());

        bar.step();
        count++;
    }

    WorldDatabase.CommitTransaction();
}

WorldPosition* TravelMgr::GetFishSpot(WorldPosition start, bool onlyNearestGrid)
{
    //0% Different map, 25% different grid, 25% different cell, 100% different point (inside cell)
    std::list<uint8> chances = { 0, 25, 25, 100 };

    if (onlyNearestGrid)
        chances = { 0, 0, 0, 100 };

    return fishMap.GetNextPoint(start, chances);
}

DestinationList TravelMgr::GetDestinations(const PlayerTravelInfo& info, uint32 purposeFlag, const std::vector<int32>& entries, bool onlyPossible, float maxDistance) const
{
    WorldPosition center = info.GetPosition();
    DestinationList retDests;

    // wow-playground: a levelling character grinds at grind spots, the nearest first by the partitions, within
    // GrindSpotTravelDestination::MAX_DISTANCE. A character that outgrew the zone finds none there; then any spot on
    // its map counts, and only then the other maps. GrindSpotTravelDestination::IsActive keeps those it has no route to out. The
    // single entry destinations are left out, unless no spot is possible anywhere.
    bool useGrindSpots = purposeFlag != (uint32)TravelDestinationPurpose::None && (purposeFlag & (uint32)TravelDestinationPurpose::Grind) &&
        entries.empty() && info.GetBoolValue2("has strategy", "levelling");

    // wow-playground: a player's character below LOW_LEVEL grinds within MAX_DISTANCE, at spots and at single entry
    // destinations both. Its start zone and the next have spots for it; a level 2 dwarf walked from Coldridge Valley
    // through the Burning Steppes for Elwynn's kobolds, and a level 3 gnome to Northshire.
    bool lowLevel = info.IsAutonomous() && info.GetLevel() < GrindSpotTravelDestination::LOW_LEVEL;
    float grindDistance = std::min(maxDistance > 0 ? maxDistance : FLT_MAX, GrindSpotTravelDestination::MAX_DISTANCE);

    if (useGrindSpots)
    {
        // Near, then anywhere on this map, then other maps: the partitions skip to a farther range now and then, which
        // must not take the bot across the sea while its own continent has a spot.
        const std::vector<std::pair<float, bool>> passes = { { std::min(maxDistance > 0 ? maxDistance : FLT_MAX, GrindSpotTravelDestination::MAX_DISTANCE), true },
            { FLT_MAX, true }, { FLT_MAX, false } };
        for (auto& [spotDistance, sameMap] : passes)
        {
            if (lowLevel && spotDistance > grindDistance)
                break;

            for (auto& spot : grindSpots)
            {
                if (sameMap && spot->GetPoint().getMapId() != center.getMapId())
                    continue;

                if (onlyPossible && !spot->IsPossible(info))
                    continue;

                float distance = spot->DistanceTo(center);
                if (distance > spotDistance || distance == FLT_MAX)
                    continue;

                retDests.push_back(spot);
            }

            if (!retDests.empty())
                break;
        }

        useGrindSpots = !retDests.empty();
    }

    for (auto& [purpose, entryDests] : destinationMap)
    {

        if (purposeFlag != (uint32)TravelDestinationPurpose::None && !((uint32)purpose & (uint32)purposeFlag))
            continue;

        if (useGrindSpots && purpose == TravelDestinationPurpose::Grind)
            continue;

        float destDistance = lowLevel && purpose == TravelDestinationPurpose::Grind ? grindDistance : maxDistance;

        for (auto& [destEntry, dests] : entryDests)
        {
            if (entries.size() && std::find(entries.begin(), entries.end(), destEntry) == entries.end())
                continue;

            for (auto& dest : dests)
            {
                if (onlyPossible && !dest->IsPossible(info))
                    continue;

                if (destDistance > 0 && dest->DistanceTo(center) > destDistance)
                    continue;

                if (dest->DistanceTo(center) == FLT_MAX) //Do not return destinations on maps you can't path to.
                    continue;
                
                retDests.push_back(dest);
            }
        }
    }

    return retDests;
}

void TravelMgr::GetPartitionsLock(bool getLock)
{
    std::unique_lock<std::mutex> lock(sTravelMgr.getDestinationMutex);
    if (getLock)
    {
        sTravelMgr.getDestinationVar.wait(lock, [&] { return sTravelMgr.availableDestinationWorkers; });
        sTravelMgr.availableDestinationWorkers--;

        return;
    }
    
    sTravelMgr.availableDestinationWorkers++;
    sTravelMgr.getDestinationVar.notify_one();
}

bool TravelMgr::IsLocationLevelValid(const WorldPosition& position, const PlayerTravelInfo& info)
{
    bool canFightElite = info.GetBoolValue("can fight elite");
    uint32 botLevel = info.GetLevel();

    if (position.getMapId() == 530 && info.GetLevel() < 58) //Outland
        return false;

    if (position.getMapId() == 571 && info.GetLevel() < 68) //Northrend
        return false;

    if (info.GetBoolValue("can fight boss"))
        botLevel += 5;
    else if (canFightElite)
        botLevel += 2;
    else if (!info.GetBoolValue("can fight equal"))
    {
        botLevel -= (2 + info.GetUint32Value("death count"));
    }

    if (botLevel < 6)
        botLevel = 6;

    uint32 areaLevel = position.getAreaLevel();

    if (!position.isOverworld() && !canFightElite)
        areaLevel += 10;

    if (!areaLevel || botLevel < areaLevel) //Skip points that are in a area that is too high level.
        return false;

    return true;
}

PartitionedTravelList TravelMgr::GetPartitions(const WorldPosition& center, const std::vector<uint32>& distancePartitions, const PlayerTravelInfo& info, uint32 purposeFlag, const std::vector<int32>& entries, bool onlyPossible, float maxDistance) const
{
    sTravelMgr.GetPartitionsLock();

    PartitionedTravelList pointMap;
    DestinationList destinations = GetDestinations(info, purposeFlag, entries, onlyPossible, maxDistance);



    unsigned seed = std::chrono::system_clock::now().time_since_epoch().count();
    std::shuffle(destinations.begin(), destinations.end(), std::default_random_engine(seed));

    for (auto& dest : destinations)
    {
        TravelPoint point(dest, sTravelMgr.nullWorldPosition, 0.0f);

        std::pair<uint32, std::vector<WorldPosition*>> pointRange = dest->GetClosestPartition(center, distancePartitions);

        if (!pointRange.first)
            continue;

        MANGOS_ASSERT(pointRange.second.size());
        std::vector<WorldPosition*> points = pointRange.second;
        std::shuffle(points.begin(), points.end(), std::default_random_engine(seed));

        // wow-playground: a grind spot's own creature levels decide (GrindSpotTravelDestination::IsPossible); the
        // area level, an average of the zone, kept a character that outgrew its zone out of the next one.
        bool isGrindSpot = dynamic_cast<GrindSpotTravelDestination*>(dest);

        // wow-playground: a player's character sells, repairs, trains and gets mail where it is, even in a zone above
        // its level: a level 22 mage in Dustwallow Marsh found Theramore's vendors too high and sailed to Loch Modan.
        TravelDestinationPurpose destPurpose = dest->GetPurpose();
        bool isBusiness = info.IsAutonomous() && (destPurpose == TravelDestinationPurpose::Vendor || destPurpose == TravelDestinationPurpose::Repair ||
            destPurpose == TravelDestinationPurpose::Mail || destPurpose == TravelDestinationPurpose::Bank || destPurpose == TravelDestinationPurpose::Trainer);

        // wow-playground: a player's character gathers on the way, not on a journey: a level 11 gnome mage with 7
        // herbalism set off from Dun Morogh for the Gloom Weed of Tirisfal Glades, a 36 minute walk. A farm order
        // goes as far as its resource.
        bool nearOnly = info.IsAutonomous() && !info.GetBoolValue("has farm order") && (destPurpose == TravelDestinationPurpose::GatherHerbalism ||
            destPurpose == TravelDestinationPurpose::GatherMining || destPurpose == TravelDestinationPurpose::GatherSkinning || destPurpose == TravelDestinationPurpose::GatherFishing);

        for (auto& position : points)
        {
            bool nearBusiness = isBusiness && position->getMapId() == center.getMapId() && position->sqDistance2d(center) < 1000.0f * 1000.0f;

            if (nearOnly && (position->getMapId() != center.getMapId() || position->sqDistance2d(center) > 1000.0f * 1000.0f))
                continue;

            if (!isGrindSpot && !nearBusiness && !IsLocationLevelValid(*position, info))
                continue;

            float distance = position->distance(center);

            // wow-playground: GetDestinations already limited the grind spots.
            if (distance > maxDistance && !isGrindSpot)
                continue;
            
            point = TravelPoint(dest, position, distance);
        }

        if (std::get<2>(point) > 0)
            pointMap[pointRange.first].push_back(point);
    }

    sTravelMgr.GetPartitionsLock(false);

    return pointMap;
}

void TravelMgr::ShuffleTravelPoints(std::vector<TravelPoint>&points)
{
    unsigned seed = std::chrono::system_clock::now().time_since_epoch().count();
    std::shuffle(points.begin(), points.end(), std::default_random_engine(seed));
    /*
    std::vector<uint32> weights;
    std::transform(points.begin(), points.end(), std::back_inserter(weights), [](TravelPoint point) { return 200000 / (1 + std::get<2>(point)); });

    //If any weight is 0 add 1 to all weights.
    for (auto& w : weights)
    {
        if (w > 0)
            continue;

        std::for_each(weights.begin(), weights.end(), [](uint32& d) { d += 1; });
        break;
    }

    std::mt19937 gen(time(0));

    WeightedShuffle(points.begin(), points.end(), weights.begin(), weights.end(), gen);
    */
}

void TravelMgr::SetNullTravelTarget(TravelTarget* target) const
{
    if (target)
    {
        target->SetTarget(nullTravelDestination, nullWorldPosition);
        target->SetStatus(TravelStatus::TRAVEL_STATUS_NONE);
    }
}

void TravelMgr::SetNullTravelTarget(Player* player) const
{
    if (!player)
        return;

    if (!player->GetPlayerbotAI())
        return;

    TravelTarget* target = player->GetPlayerbotAI()->GetAiObjectContext()->GetValue<TravelTarget*>("travel target")->Get();

    SetNullTravelTarget(target);
}

void TravelMgr::AddMapTransfer(WorldPosition start, WorldPosition end, float portalDistance, bool makeShortcuts)
{
    uint32 sMap = start.getMapId();
    uint32 eMap = end.getMapId();

    if (sMap == eMap)
        return;
    
    //Calculate shortcuts.
    if(makeShortcuts)
        for (auto& mapTransfers : mapTransfersMap)
        {
            uint32 sMapt = mapTransfers.first.first;
            uint32 eMapt = mapTransfers.first.second;

            for (auto& mapTransfer : mapTransfers.second)
            {
                if (eMapt == sMap && sMapt != eMap) // [S1 >MT> E1 -> S2] >THIS> E2
                {
                    float newDistToEnd = MapTransDistance(mapTransfer.GetPointFrom(), start) + portalDistance;
                    if (MapTransDistance(mapTransfer.GetPointFrom(), end) > newDistToEnd)
                        AddMapTransfer(mapTransfer.GetPointFrom(), end, newDistToEnd, false);
                }

                if (sMapt == eMap && eMapt != sMap) // S1 >THIS> [E1 -> S2 >MT> E2]
                {
                    float newDistToEnd = portalDistance + MapTransDistance(end, mapTransfer.GetPointTo());
                    if (MapTransDistance(start, mapTransfer.GetPointTo()) > newDistToEnd)
                        AddMapTransfer(start, mapTransfer.GetPointTo(), newDistToEnd, false);
                }
            }
        }

    //Add actual transfer.
    auto mapTransfers = mapTransfersMap.find(std::make_pair(start.getMapId(), end.getMapId()));
    
    if (mapTransfers == mapTransfersMap.end())
        mapTransfersMap.insert({ { sMap, eMap }, {MapTransfer(start, end, portalDistance)} });
    else
        mapTransfers->second.push_back(MapTransfer(start, end, portalDistance));        
};

void TravelMgr::LoadMapTransfers()
{
    for (auto& startNode : sTravelNodeMap.getNodes())
    {
        for (auto& [endNode, path] : *startNode->getLinks())
        {
            // wow-playground: a boat or zeppelin path has a length of 0.1 yards and its ride time as extra cost, so a
            // crossing cost nothing and the other continent was next door. The ride now costs its time at running
            // speed, and a transport adds the average wait at the dock, a boat every five minutes or so.
            float cost = path->getDistance();
            if (path->getPathType() == TravelNodePathType::transport)
                cost += path->getExtraCost() * 7.0f + 1000.0f;
            AddMapTransfer(*startNode->getPosition(), *endNode->getPosition(), cost);
        }
    }
}

std::vector<std::pair<WorldPosition, float>> TravelMgr::sqMapTransDistances(const WorldPosition& start, uint32 endMapId) const
{
    uint32 startMapId = start.getMapId();
    if (startMapId == endMapId)
        return { { start, 0.0f } };

    auto mapTransfers = mapTransfersMap.find({ startMapId, endMapId });   

    if (mapTransfers == mapTransfersMap.end())
        return {};

    std::vector<std::pair<WorldPosition, float>> retPortals;

    for (auto& mapTrans : mapTransfers->second)
    {
        WorldPosition portalOnEndMap = mapTrans.GetPointTo();

        float sqDist = mapTrans.sqDist(start, portalOnEndMap);

        retPortals.push_back(std::make_pair(portalOnEndMap, sqDist));
    }

    return retPortals;
}

float TravelMgr::MapTransDistance(const WorldPosition& start, const WorldPosition& end, bool toMap) const
{
    //These are the portals on the end map that start can reach.
    std::vector<std::pair<WorldPosition, float>> portals = sqMapTransDistances(start, end.getMapId());

    if (portals.empty())
        return FLT_MAX;

    float minsqDist = FLT_MAX;

    for (auto& [portal, sqDistanceFromStartToPortal] : portals)
    {
        // wow-playground: the square of the whole way, see MapTransfer::sqDist.
        float dist = sqrt(sqDistanceFromStartToPortal) + sqrt(portal.sqDistance2d(end));
        float sqDist = dist * dist;
        if (sqDist < minsqDist)
            minsqDist = sqDist;
    }

    return sqrt(minsqDist);
}

std::string TravelMgr::GetCacheCensus() const
{
    uint32 destinations = 0;
    uint32 destinationKeys = 0;
    for (auto& purpose : destinationMap)
        for (auto& entry : purpose.second)
        {
            ++destinationKeys;
            destinations += entry.second.size();
        }

    uint32 populated = 0;
    for (uint32 mapId = 0; mapId < 1024; ++mapId)
        for (uint32 gx = 0; gx < 64; ++gx)
            for (uint32 gy = 0; gy < 64; ++gy)
                if (populatedGrids[mapId][gx][gy])
                    ++populated;

    std::ostringstream out;
    out << "mem travel: destinations=" << destinations << " destKeys=" << destinationKeys
        << " points=" << pointsMap.size()
        << " fish=" << fishPoints.size()
        << " areaLevels=" << areaLevels.size()
        << " transfers=" << mapTransfersMap.size()
        << " badMmap=" << badMmap.size()
        << " populatedGrids=" << populated;
    return out.str();
}