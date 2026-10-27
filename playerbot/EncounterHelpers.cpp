#include "playerbot/playerbot.h"
#include "playerbot/EncounterHelpers.h"

namespace EncounterHelpers
{
    Player* GetGroupMainTank(Player* bot)
    {
        Group* group = bot->GetGroup();
        if (!group)
            return nullptr;

        Player* mainTank = sObjectMgr.GetPlayer(PlayerbotAI::GetMainTankGuid(group));
        return mainTank && mainTank->IsAlive() ? mainTank : nullptr;
    }

    Player* GetGroupAssistTank(Player* bot, uint8 index)
    {
        Group* group = bot->GetGroup();
        if (!group)
            return nullptr;

        for (Player* tank : PlayerbotAI::GetGroupTanks(group))
        {
            if (PlayerbotAI::IsAssistTankOfIndex(tank, index, true))
                return tank;
        }

        return nullptr;
    }
}
