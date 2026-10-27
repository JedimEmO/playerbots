#pragma once
#include "playerbot/strategy/Value.h"

namespace ai
{
    // How many seconds a unit has left under the group's damage: estimated lifetime::current target.
    // The qualifier names a unit value. Zero for no unit or a dead one.
    class EstimatedLifetimeValue : public FloatCalculatedValue, public Qualified
    {
    public:
        EstimatedLifetimeValue(PlayerbotAI* ai, std::string name = "estimated lifetime") : FloatCalculatedValue(ai, name), Qualified() {}
        float Calculate() override;
    };

    // The damage per second the bot's group is expected to deal to one target, as AzerothCore estimates it:
    // a base for the level, scaled by role and by the average item level against the level's,
    // over the bot and the bots of its group near it. A player is left out, since a player may not be fighting.
    class EstimatedGroupDpsValue : public FloatCalculatedValue
    {
    public:
        EstimatedGroupDpsValue(PlayerbotAI* ai, std::string name = "estimated group dps") : FloatCalculatedValue(ai, name, 20) {}
        float Calculate() override;

        static float GetBasicDps(uint32 level);
    };
}
