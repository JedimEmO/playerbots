#pragma once

#include "Common.h"

class Player;

// Helpers the encounter strategies share, under the names AzerothCore's raid code uses,
// so ported strategies call them unchanged.
namespace EncounterHelpers
{
    // The group's main tank while it is alive; null when it is dead or there is none.
    Player* GetGroupMainTank(Player* bot);
    // The living assist tank of this index, 0 for the first, counting only the living;
    // null when there is none.
    Player* GetGroupAssistTank(Player* bot, uint8 index);
}
