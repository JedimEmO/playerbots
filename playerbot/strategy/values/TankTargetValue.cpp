
#include "playerbot/playerbot.h"
#include "TankTargetValue.h"
#include "PossibleAttackTargetsValue.h"

using namespace ai;

// AzerothCore's tank target. Mobs no tank holds come first, the nearest of them; then the held ones
// in melee range, then the rest, each by least threat on the bot. A mob attacking another tank counts
// as held, except to the assigned main tank, which counts only itself and so takes back what it should hold.
// With more than one tank in the group, the assigned main tank keeps its current target.
class FindTankTargetSmartStrategy : public FindNonCcTargetStrategy
{
public:
    FindTankTargetSmartStrategy(PlayerbotAI* ai) : FindNonCcTargetStrategy(ai)
    {
        Player* bot = ai->GetBot();
        explicitMainTank = PlayerbotAI::IsExplicitMainTank(bot);
        if (explicitMainTank && PlayerbotAI::GetGroupTankNum(bot) > 1)
            currentTarget = ai->GetUnit(ai->GetAiObjectContext()->GetValue<ObjectGuid>("current target")->Get());
    }

public:
    void CheckAttacker(Unit* creature, ThreatManager* threatManager) override
    {
        Player* bot = ai->GetBot();
        AiObjectContext* context = ai->GetAiObjectContext();

        if (!creature->IsAlive() || IsCcTarget(creature))
            return;

        if (!PossibleAttackTargetsValue::IsValid(creature, bot))
        {
            std::list<ObjectGuid> attackers = AI_VALUE(std::list<ObjectGuid>, "possible attack targets");
            if (std::find(attackers.begin(), attackers.end(), creature->GetObjectGuid()) == attackers.end())
                return;
        }

        if (!result || IsBetter(creature, result))
            result = creature;
    }

private:
    bool IsBetter(Unit* candidate, Unit* best)
    {
        if (currentTarget)
        {
            if (best == currentTarget)
                return false;
            if (candidate == currentTarget)
                return true;
        }

        int candidateLevel = Level(candidate);
        int bestLevel = Level(best);
        if (candidateLevel != bestLevel)
            return candidateLevel > bestLevel;

        Player* bot = ai->GetBot();
        if (candidateLevel == 2)
            return bot->GetDistance(candidate) < bot->GetDistance(best);

        return Threat(candidate) < Threat(best);
    }

    // 2 when no tank holds the unit, 1 when one does and it is in melee range, 0 otherwise.
    int Level(Unit* unit)
    {
        Player* bot = ai->GetBot();
        if (!IsHeld(unit))
            return 2;

        return bot->CanReachWithMeleeAttack(unit) ? 1 : 0;
    }

    bool IsHeld(Unit* unit)
    {
        Unit* victim = unit->GetVictim();
        if (!victim)
            return false;

        if (victim == ai->GetBot())
            return true;

        return !explicitMainTank && victim->IsPlayer() && PlayerbotAI::IsTank(static_cast<Player*>(victim));
    }

    float Threat(Unit* unit)
    {
        return sServerFacade.GetThreatManager(unit).getThreat(ai->GetBot());
    }

    bool explicitMainTank = false;
    Unit* currentTarget = nullptr;
};

ObjectGuid TankTargetValue::Calculate()
{
    GuidSet exclusions = GatherStrategyTargetExclusions(ai, TargetValueExclusionType::TankTarget);

    // The marked target, unless another bot tank following the same mark already holds it.
    ObjectGuid rti = RtiTargetValue::Calculate();
    Unit* rtiTarget = exclusions.count(rti) ? nullptr : ai->GetUnit(rti);
    if (rtiTarget)
    {
        Unit* victim = rtiTarget->GetVictim();
        Player* player = victim && victim != bot && victim->IsPlayer() ? static_cast<Player*>(victim) : nullptr;
        bool heldBySameMark = player && player->GetPlayerbotAI() && PlayerbotAI::IsTank(player) &&
            PAI_VALUE(std::string, "rti") == AI_VALUE(std::string, "rti");
        if (!heldBySameMark)
            return rti;
    }

    FindTankTargetSmartStrategy strategy(ai);
    Unit* target = FindTarget(&strategy, exclusions);
    return target ? target->GetObjectGuid() : ObjectGuid();
}
