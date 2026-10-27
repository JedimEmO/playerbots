
#include "playerbot/playerbot.h"
#include "MoltenCoreDungeonActions.h"
#include "playerbot/EncounterHelpers.h"
#include "playerbot/strategy/values/RtiTargetValue.h"

using namespace ai;
using namespace ai::MoltenCore;

namespace
{
    // AzerothCore's points: tank camps 56 yards apart, so the Core Ragers stand out of Golemagg's Trust,
    // and not on the steep walls they would be pulled onto otherwise.
    constexpr float GOLEMAGG_TANK_X = 795.7308f, GOLEMAGG_TANK_Y = -994.8848f;
    constexpr float CORE_RAGER_TANK_X = 846.6453f, CORE_RAGER_TANK_Y = -1019.0639f;
    constexpr float HEALER_X = 821.2f, HEALER_Y = -1007.0f, HEALER_Z = -203.0f;
    constexpr float HEALER_POSITION_TOLERANCE = 8.0f;

    constexpr float GOLEMAGGS_TRUST_DISTANCE = 30.0f;
    constexpr float CORE_RAGER_STEP_DISTANCE = 5.0f;

    Unit* FindGolemagg(PlayerbotAI* ai)
    {
        return ai->GetUnit(ai->GetAiObjectContext()->GetValue<ObjectGuid>("find target", "golemagg the incinerator")->Get());
    }

    float Distance2d(Unit* unit, float x, float y)
    {
        return unit->GetDistance2d(x, y, DIST_CALC_NONE);
    }
}

bool McGolemaggMarkBossAction::isUseful()
{
    Unit* boss = FindGolemagg(ai);
    Group* group = bot->GetGroup();
    return boss && group && group->GetTargetIcon(RtiTargetValue::GetRtiIndex("skull")) != boss->GetObjectGuid();
}

bool McGolemaggMarkBossAction::Execute(Event& event)
{
    return EncounterHelpers::MarkTargetWithSkull(bot, FindGolemagg(ai));
}

Unit* McGolemaggTankAction::Golemagg()
{
    return FindGolemagg(ai);
}

bool McGolemaggTankAction::Taunt()
{
    Event event;
    switch (bot->getClass())
    {
        case CLASS_WARRIOR:
            return ai->DoSpecificAction("taunt", event, true) || ai->DoSpecificAction("mocking blow", event, true);
        case CLASS_DRUID:
            return ai->DoSpecificAction("growl", event, true);
        default:
            return false;
    }
}

bool McGolemaggTankAction::MoveUnitToPosition(Unit* target, float x, float y, float maxDistance, float stepDistance)
{
    if (bot->GetVictim() != target)
        return AttackUnit(target);

    if (target->GetVictim() != bot)
        return Taunt();

    float distance = Distance2d(bot, x, y);
    if (distance <= maxDistance || distance <= 0.0f)
        return false;

    float stepX = bot->GetPositionX() + (x - bot->GetPositionX()) / distance * stepDistance;
    float stepY = bot->GetPositionY() + (y - bot->GetPositionY()) / distance * stepDistance;
    float stepZ = bot->GetPositionZ();
    bot->UpdateGroundPositionZ(stepX, stepY, stepZ);
    return MoveTo(bot->GetMapId(), stepX, stepY, stepZ);
}

bool McGolemaggTankAction::FindCoreRagers(Unit*& first, Unit*& second)
{
    first = second = nullptr;
    for (const ObjectGuid& guid : AI_VALUE(std::list<ObjectGuid>, "possible targets no los"))
    {
        Unit* unit = ai->GetUnit(guid);
        if (!unit || !unit->IsAlive() || unit->GetEntry() != NPC_CORE_RAGER)
            continue;

        if (!first)
            first = unit;
        else
        {
            second = unit;
            break;
        }
    }

    return first && second;
}

bool McGolemaggMainTankAttackGolemaggAction::Execute(Event& event)
{
    Unit* boss = Golemagg();
    Unit* first;
    Unit* second;
    if (!boss || !FindCoreRagers(first, second))
        return false;

    // He only needs moving while a Core Rager still has Golemagg's Trust.
    if (first->HasAura(SPELL_GOLEMAGGS_TRUST) || second->HasAura(SPELL_GOLEMAGGS_TRUST))
        return MoveUnitToPosition(boss, GOLEMAGG_TANK_X, GOLEMAGG_TANK_Y, boss->GetCombatReach());

    return false;
}

bool McGolemaggAssistTankAttackCoreRagerAction::Execute(Event& event)
{
    Unit* boss = Golemagg();
    if (!boss)
        return false;

    bool firstAssist = PlayerbotAI::IsAssistTankOfIndex(bot, 0, true);
    bool secondAssist = PlayerbotAI::IsAssistTankOfIndex(bot, 1, true);
    if (!firstAssist && !secondAssist)
        return AttackUnit(boss);

    Unit* first;
    Unit* second;
    if (!FindCoreRagers(first, second))
        return false;

    Unit* mine = firstAssist ? first : second;
    Unit* other = firstAssist ? second : first;

    // Get my Core Rager attacking me.
    if (mine->GetVictim() != bot)
    {
        if (bot->GetVictim() != mine)
            return AttackUnit(mine);
        return Taunt();
    }

    // The other Core Rager is loose, or on someone who is not one of the two assist tanks: take it too.
    Unit* otherVictim = other->GetVictim();
    if (otherVictim && otherVictim->IsPlayer())
    {
        Player* otherPlayer = static_cast<Player*>(otherVictim);
        if (!PlayerbotAI::IsAssistTankOfIndex(otherPlayer, 0, true) && !PlayerbotAI::IsAssistTankOfIndex(otherPlayer, 1, true))
        {
            if (bot->GetVictim() != other)
                return AttackUnit(other);
            return Taunt();
        }
    }

    if (bot->GetVictim() != mine)
        return AttackUnit(mine);

    // Out of Golemagg's Trust: to the Core Rager point when Golemagg is far enough from it, else straight away from him.
    if (mine->HasAura(SPELL_GOLEMAGGS_TRUST) || (otherVictim == bot && other->HasAura(SPELL_GOLEMAGGS_TRUST)))
    {
        if (Distance2d(boss, CORE_RAGER_TANK_X, CORE_RAGER_TANK_Y) > GOLEMAGGS_TRUST_DISTANCE &&
            Distance2d(bot, CORE_RAGER_TANK_X, CORE_RAGER_TANK_Y) > CORE_RAGER_STEP_DISTANCE)
            return MoveUnitToPosition(mine, CORE_RAGER_TANK_X, CORE_RAGER_TANK_Y, CORE_RAGER_STEP_DISTANCE);

        return Move(boss->GetAngle(bot), CORE_RAGER_STEP_DISTANCE);
    }

    return false;
}

bool McGolemaggHealerPositionAction::Execute(Event& event)
{
    if (Distance2d(bot, HEALER_X, HEALER_Y) <= HEALER_POSITION_TOLERANCE)
        return false;

    return MoveTo(bot->GetMapId(), HEALER_X, HEALER_Y, HEALER_Z);
}

bool McGolemaggBackOffAction::Execute(Event& event)
{
    Unit* boss = FindGolemagg(ai);
    if (!boss)
        return false;

    bot->CastStop();

    float distance = MAGMA_SPLASH_BACK_OFF_DISTANCE - Distance2d(bot, boss->GetPositionX(), boss->GetPositionY());
    if (distance <= 0.0f)
        return true;

    return Move(boss->GetAngle(bot), distance);
}