
#include "playerbot/playerbot.h"
#include "FocusTargetStrategy.h"
#include "playerbot/strategy/values/RtiTargetValue.h"

using namespace ai;

void AvoidCrossStrategy::AppendTargetExclusions(GuidSet& exclusions, TargetValueExclusionType type)
{
    if (type == TargetValueExclusionType::TankTarget)
        return;

    Group* group = ai->GetBot()->GetGroup();
    if (!group)
        return;

    ObjectGuid marked = group->GetTargetIcon(RtiTargetValue::GetRtiIndex("cross"));
    if (marked)
        exclusions.insert(marked);
}