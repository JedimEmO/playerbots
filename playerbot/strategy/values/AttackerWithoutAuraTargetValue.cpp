
#include "playerbot/playerbot.h"
#include "AttackerWithoutAuraTargetValue.h"
#include "TargetValue.h"
#include "playerbot/PlayerbotAIConfig.h"

using namespace ai;

ObjectGuid AttackerWithoutAuraTargetValue::Calculate()
{
    std::list<ObjectGuid> attackers = ai->GetAiObjectContext()->GetValue<std::list<ObjectGuid>>("possible attack targets")->Get();
    PlayerbotAI* ai = bot->GetPlayerbotAI();
    Unit* target = ai->GetUnit(ai->GetAiObjectContext()->GetValue<ObjectGuid>("current target")->Get());
    GuidSet exclusions = GatherStrategyTargetExclusions(ai, TargetValueExclusionType::DebuffTarget);
    for (std::list<ObjectGuid>::iterator i = attackers.begin(); i != attackers.end(); ++i)
    {
        Unit* unit = ai->GetUnit(*i);
        if (!unit || unit == target || exclusions.count(*i))
            continue;

        if (bot->GetDistance(unit) > ai->GetRange("spell"))
            continue;

        if (!ai->HasAura(qualifier, unit))
            return unit->GetObjectGuid();
    }

    return ObjectGuid();
}
