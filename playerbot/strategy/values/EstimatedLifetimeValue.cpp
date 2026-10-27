#include "playerbot/playerbot.h"
#include "EstimatedLifetimeValue.h"
#include "playerbot/PlayerbotAIConfig.h"

using namespace ai;

float EstimatedLifetimeValue::Calculate()
{
    Unit* target = ai->GetUnit(AI_VALUE(ObjectGuid, qualifier));
    if (!target || !target->IsAlive())
        return 0.0f;

    float dps = AI_VALUE(float, "estimated group dps");
    // Damage spread over three or more attackers reaches this one more slowly.
    if (AI_VALUE(uint8, "attackers count") >= 3)
        dps *= 0.75f;

    return target->GetHealth() / std::max(dps, 1.0f);
}

float EstimatedGroupDpsValue::Calculate()
{
    std::vector<Player*> players = { bot };
    if (Group* group = bot->GetGroup())
    {
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player* member = ref->getSource();
            if (!member || member == bot || !member->GetPlayerbotAI() || !member->IsInWorld() || !member->IsAlive())
                continue;

            if (member->GetMapId() != bot->GetMapId() || member->GetDistance(bot) > sPlayerbotAIConfig.sightDistance)
                continue;

            players.push_back(member);
        }
    }

    float total = 0.0f;
    for (Player* player : players)
    {
        float role = PlayerbotAI::IsTank(player) ? 0.3f : PlayerbotAI::IsHeal(player) ? 0.1f : 1.0f;

        // AzerothCore weighs its own gear score against a level's rare gear; on cmangos the gear score is the
        // average item level, and a level's rare items are about five levels above it.
        float gear = ai->GetEquipGearScore(player, false, false) / float(player->GetLevel() + 5);
        gear = std::min(std::max(gear, 0.75f), 4.0f);

        total += GetBasicDps(player->GetLevel()) * role * gear;
    }

    // Buffs from a bigger group.
    if (players.size() >= 25)
        total *= 1.2f;
    else if (players.size() >= 10)
        total *= 1.1f;
    else if (players.size() >= 5)
        total *= 1.05f;

    return total;
}

float EstimatedGroupDpsValue::GetBasicDps(uint32 level)
{
    if (level <= 15)
        return 5 + level * 1;
    if (level <= 25)
        return 20 + (level - 15) * 2;
    if (level <= 45)
        return 40 + (level - 25) * 3;
    if (level <= 55)
        return 100 + (level - 45) * 20;
    if (level <= 60)
        return 300 + (level - 55) * 50;
    if (level <= 70)
        return 550 + (level - 60) * 65;
    return 1200 + (level - 70) * 200;
}
