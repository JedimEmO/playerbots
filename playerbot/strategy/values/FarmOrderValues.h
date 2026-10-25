#pragma once
#include "playerbot/strategy/Value.h"
#include "playerbot/TravelMgr.h"

namespace ai
{
    // wow-playground: a farm order keeps an autonomous bot gathering one resource, a herb, an ore or a skin, until it
    // has gathered a count of it. The console sets it with .rndbot farm, and RandomPlayerbotMgr keeps it. It is a named
    // travel purpose beside the guild order, "should travel named::farm order", asked just above it with the same
    // forced trip; the destinations are the nodes and creatures that yield the item.

    // One place the item comes from: a creature to skin, or minus the entry of a node to gather, as the travel
    // destinations count entries, with the skill it asks for.
    struct FarmSource
    {
        int32 entry;
        SkillType skill;
        uint32 reqSkill;
    };

    class FarmOrders
    {
    public:
        // Most of one item a farm order may ask for.
        static constexpr uint32 MAX_COUNT = 1000;

        // Every herb and mining node whose loot holds the item, and every creature whose skinning loot does.
        static std::vector<FarmSource> Sources(uint32 itemId);
        static bool IsSource(uint32 itemId, int32 entry);
        // The item a farm order names: an item id, or a name, ignoring case. An exact name comes first, else the one
        // item whose name holds the text. Only items some source yields count. 0 with error set when none fits.
        static uint32 FindItem(const std::string& text, std::string& error);
        // How many of the item are in the bot's bags that are its own, not what its player left on it.
        static uint32 Held(Player* bot, uint32 itemId);
        // Gathered so far: what went to the player by .rndbot mailgoods since the order was set, and what the bot holds.
        static uint32 Gathered(Player* bot, uint32 itemId, uint32 mailed);
        static std::string SkillName(SkillType skill);

        // Herb nodes are pooled, so most spawn points of a gather destination are empty at any time. A bot working a
        // farm order's gather trip walks from spawn point to spawn point, nearest first, skipping the ones it saw in the
        // last VISIT_SECONDS; with none left within ROAM_DISTANCE the trip ends, and the next trip goes to the nearest
        // point it has not seen.
        static constexpr time_t VISIT_SECONDS = 5 * MINUTE;
        static constexpr float ROAM_DISTANCE = 400.0f;
        // A point this close to the bot counts as seen: a node there would be in sight.
        static constexpr float SEEN_DISTANCE = 30.0f;

        // The gather destination of the farm order trip the bot works, or none.
        static GatherTravelDestination* WorkedGather(PlayerbotAI* ai);
        // The next spawn point to walk to, after marking those in sight as seen; nullptr when none is left in range.
        static const WorldPosition* RoamPoint(Player* bot, const TravelDestination* destination);
        static void MarkVisited(Player* bot, const WorldPosition* point);
        // The points the bot saw in the last VISIT_SECONDS.
        static std::set<const WorldPosition*> Visited(Player* bot);
    };

    // The bot has a farm order it has not finished: an autonomous bot, an order, and its count not yet gathered.
    class HasFarmOrderValue : public BoolCalculatedValue
    {
    public:
        HasFarmOrderValue(PlayerbotAI* ai) : BoolCalculatedValue(ai, "has farm order", 10) {}

        bool Calculate() override;
    };
}
