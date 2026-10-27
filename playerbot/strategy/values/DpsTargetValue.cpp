
#include "playerbot/playerbot.h"
#include "DpsTargetValue.h"
#include "LeastHpTargetValue.h"

using namespace ai;


ObjectGuid DpsTargetValue::Calculate()
{
    GuidSet exclusions = GatherStrategyTargetExclusions(ai, TargetValueExclusionType::DpsTarget);
    ObjectGuid rti = RtiTargetValue::Calculate();
    if (rti && !exclusions.count(rti)) return rti;

    FindLeastHpTargetStrategy strategy(ai);
    Unit* target = TargetValue::FindTarget(&strategy, exclusions);
    return target ? target->GetObjectGuid() : ObjectGuid();
}

class FindMaxHpTargetStrategy : public FindTargetStrategy
{
public:
    FindMaxHpTargetStrategy(PlayerbotAI* ai) : FindTargetStrategy(ai)
    {
        maxHealth = 0;
    }

public:
    virtual void CheckAttacker(Unit* attacker, ThreatManager* threatManager) override
    {
        if (!result || result->GetHealth() < attacker->GetHealth())
            result = attacker;
    }

protected:
    float maxHealth;
};

ObjectGuid DpsAoeTargetValue::Calculate()
{
    GuidSet exclusions = GatherStrategyTargetExclusions(ai, TargetValueExclusionType::DpsTarget);
    ObjectGuid rti = RtiTargetValue::Calculate();
    if (rti && !exclusions.count(rti)) return rti;

    FindMaxHpTargetStrategy strategy(ai);
    Unit* target = TargetValue::FindTarget(&strategy, exclusions);
    return target ? target->GetObjectGuid() : ObjectGuid();
}
