#pragma once
#include "NonCombatStrategy.h"

namespace ai
{
    // The parts of upkeep a levelling guild character needs on its own: getting unstuck, and opening
    // the containers it loots. Not the rest of "maintenance", which destroys, disenchants and enchants
    // items by itself; a guild character's items are its player's.
    class GuildLevelStrategy : public NonCombatStrategy
    {
    public:
        GuildLevelStrategy(PlayerbotAI* ai) : NonCombatStrategy(ai) {}
        std::string getName() override { return "guild level"; }
        int GetType() override { return STRATEGY_TYPE_NONCOMBAT; }

    private:
        void InitNonCombatTriggers(std::list<TriggerNode*>& triggers) override;
    };
}
