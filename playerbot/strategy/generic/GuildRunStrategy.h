#pragma once
#include "playerbot/strategy/Strategy.h"

namespace ai
{
    // A guild run's leader inside the dungeon: from stop to stop of the run state's route, it walks to the pull spot,
    // waits there for the party and its mana, pulls the pack and holds the fight at the spot. Its own fighting is the
    // tank strategies'; the pull is the `pull` strategy's. See GuildRunStates.
    class RunLeadStrategy : public Strategy
    {
    public:
        RunLeadStrategy(PlayerbotAI* ai) : Strategy(ai) {}
        std::string getName() override { return "run lead"; }

#ifdef GenerateBotHelp
        virtual std::string GetHelpName() { return "run lead"; } //Must equal iternal name
        virtual std::string GetHelpDescription() { return "This strategy leads a guild party's run from pull spot to pull spot."; }
        virtual std::vector<std::string> GetRelatedStrategies() { return { "run party", "pull" }; }
#endif
    private:
        void InitNonCombatTriggers(std::list<TriggerNode*>& triggers) override;
        void InitCombatTriggers(std::list<TriggerNode*>& triggers) override;
    };

    // A guild run's member inside the dungeon: with the leader on the way, at its stand point at the pull spot,
    // and within reach of it in the fight. Its own fighting is its class's and `dps assist`'s.
    class RunPartyStrategy : public Strategy
    {
    public:
        RunPartyStrategy(PlayerbotAI* ai) : Strategy(ai) {}
        std::string getName() override { return "run party"; }

#ifdef GenerateBotHelp
        virtual std::string GetHelpName() { return "run party"; } //Must equal iternal name
        virtual std::string GetHelpDescription() { return "This strategy keeps a guild party member at its place in the run."; }
        virtual std::vector<std::string> GetRelatedStrategies() { return { "run lead" }; }
#endif
    private:
        void InitNonCombatTriggers(std::list<TriggerNode*>& triggers) override;
        void InitCombatTriggers(std::list<TriggerNode*>& triggers) override;
    };
}
