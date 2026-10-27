#pragma once
#include "playerbot/strategy/Value.h"

namespace ai
{
    // The group's main tank, alive or dead, as PlayerbotAI::GetMainTankGuid answers it;
    // without a group, the bot itself when it is a tank.
    class MainTankValue : public UnitCalculatedValue
    {
    public:
        MainTankValue(PlayerbotAI* ai, std::string name = "main tank") : UnitCalculatedValue(ai, name) {}
        ObjectGuid Calculate() override;
    };

    // The group's assist tank of an index, dead or alive: assist tank::0 is the first after the main tank.
    // The index stays when a tank dies, as PlayerbotAI::IsAssistTankOfIndex answers without indexLivingOnly.
    class AssistTankValue : public UnitCalculatedValue, public Qualified
    {
    public:
        AssistTankValue(PlayerbotAI* ai, std::string name = "assist tank") : UnitCalculatedValue(ai, name), Qualified() {}
        ObjectGuid Calculate() override;
    };
}
