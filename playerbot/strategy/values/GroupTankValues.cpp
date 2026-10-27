#include "playerbot/playerbot.h"
#include "GroupTankValues.h"

using namespace ai;

ObjectGuid MainTankValue::Calculate()
{
    Group* group = bot->GetGroup();
    if (!group)
        return PlayerbotAI::IsTank(bot) ? bot->GetObjectGuid() : ObjectGuid();

    return PlayerbotAI::GetMainTankGuid(group);
}

ObjectGuid AssistTankValue::Calculate()
{
    Group* group = bot->GetGroup();
    if (!group)
        return ObjectGuid();

    uint8 index = uint8(atoi(qualifier.c_str()));
    for (Player* tank : PlayerbotAI::GetGroupTanks(group))
    {
        if (PlayerbotAI::IsAssistTankOfIndex(tank, index))
            return tank->GetObjectGuid();
    }

    return ObjectGuid();
}
