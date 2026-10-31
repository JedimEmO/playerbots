#include "playerbot/playerbot.h"
#include "GuildRunValues.h"
#include "Groups/Group.h"

using namespace ai;

GuildRunView RunStateValue::Calculate()
{
    return sGuildRunStates.View(bot);
}

WorldPosition RunSpotValue::Calculate()
{
    // The same place a run move takes the bot to (GuildRunStates::PlaceOf), for the triggers to decide by.
    return GuildRunStates::PlaceOf(AI_VALUE(GuildRunView, "run state"), bot);
}
