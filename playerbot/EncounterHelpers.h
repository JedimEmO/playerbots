#pragma once

#include "Common.h"
#include "playerbot/WorldPosition.h"

#include <initializer_list>
#include <string>
#include <vector>

class Player;
class PlayerbotAI;
class Unit;

// Helpers the encounter strategies share, under the names AzerothCore's raid code uses,
// so ported strategies call them unchanged.
namespace EncounterHelpers
{
    // Rough proxies for how far a boss fight is: at 95% health the raid is in place and the tank has threat,
    // so it is time for cooldowns; at 10% the boss is almost dead, so adds can wait.
    constexpr float BOSS_ENGAGED_HEALTH_PCT = 95.0f;
    constexpr float BOSS_BURN_HEALTH_PCT = 10.0f;

    // True while the bot is on this map and the instance script says an encounter is in progress.
    // A script may set that at another moment than the pull, so it is a hint, not the pull itself.
    bool IsEncounterInProgress(Player* bot, uint32 mapId);

    // Puts a raid target icon on the target; false when it already wears it, or there is no group.
    // The icon indexes are RtiTargetValue's: 0 star, 1 circle, 2 diamond, 3 triangle, 4 moon, 5 square, 6 cross, 7 skull.
    bool MarkTargetWithIcon(Player* bot, Unit* target, uint8 iconId);
    bool MarkTargetWithSkull(Player* bot, Unit* target);
    bool MarkTargetWithSquare(Player* bot, Unit* target);
    bool MarkTargetWithStar(Player* bot, Unit* target);
    bool MarkTargetWithCircle(Player* bot, Unit* target);
    bool MarkTargetWithDiamond(Player* bot, Unit* target);
    bool MarkTargetWithTriangle(Player* bot, Unit* target);
    bool MarkTargetWithCross(Player* bot, Unit* target);
    bool MarkTargetWithMoon(Player* bot, Unit* target);
    // Takes an icon off whatever wears it, so bots do not charge a mark left from before.
    bool ClearTargetIcon(Player* bot, uint8 iconId);
    // Takes an icon off only when it is on a creature of these entries, one a strategy marked,
    // and leaves a player's marks alone.
    bool ClearStrategyTargetIcon(Player* bot, uint8 iconId, std::initializer_list<uint32> entries);
    // Points the bot's own rti value at an icon by name, such as skull.
    void SetRtiTarget(PlayerbotAI* ai, const std::string& rtiName);

    // True for the one bot that runs a shared mechanic for its group on this map, such as marking:
    // the first living bot in member order.
    bool IsMechanicTrackerBot(Player* bot, uint32 mapId);

    // The group's main tank while it is alive; null when it is dead or there is none.
    Player* GetGroupMainTank(Player* bot);
    // The living assist tank of this index, 0 for the first, counting only the living;
    // null when there is none.
    Player* GetGroupAssistTank(Player* bot, uint8 index);

    // The nearest living group member other than the bot within the radius, by distance between centres.
    Player* GetNearestPlayerInRadius(Player* bot, float radius);
    // Where the dynamic objects of a spell lie within the radius, such as fire patches on the ground.
    // AzerothCore returns Position; WorldPosition is the playerbot type, and its distances are not squared.
    std::vector<WorldPosition> GetDynamicObjectPositions(Player* bot, float searchRadius, uint32 spellId);
}
