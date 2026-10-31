#pragma once
#include "playerbot/strategy/Multiplier.h"
#include "playerbot/strategy/Strategy.h"

namespace ai
{
    class ConserveManaMultiplier : public Multiplier
    {
    public:
        ConserveManaMultiplier(PlayerbotAI* ai) : Multiplier(ai, "conserve mana") {}

    public:
        virtual float GetValue(Action* action) override;
    };

    class SaveManaMultiplier : public Multiplier
    {
    public:
        SaveManaMultiplier(PlayerbotAI* ai) : Multiplier(ai, "save mana") {}

    public:
        virtual float GetValue(Action* action) override;
    };

    class ConserveManaStrategy : public Strategy
    {
    public:
        ConserveManaStrategy(PlayerbotAI* ai) : Strategy(ai) {}
        std::string getName() override { return "conserve mana"; }

#ifdef GenerateBotHelp
        virtual std::string GetHelpName() { return "conserve mana"; } //Must equal iternal name
        virtual std::string GetHelpDescription() {
            return "This strategy will make bots wait longer between casting the same spell twice.\n"
                   "the delay is based on [h:value|mana save level].";
        }
        virtual std::vector<std::string> GetRelatedStrategies() { return { }; }
#endif
    private:
        void InitCombatMultipliers(std::list<Multiplier*> &multipliers) override;
    };

    // AzerothCore's healer mana saving (HealerAutoSaveManaMultiplier in its ConserveManaStrategy.cpp). At 60% mana or
    // less a healer passes over a heal that would heal more than the target is missing, and a heal that costs much for
    // what it gives on a member not in danger. The engine then tries the heal's alternative, a smaller one where the
    // strategy names one, or waits for the member to drop further.
    class HealerAutoSaveManaMultiplier : public Multiplier
    {
    public:
        HealerAutoSaveManaMultiplier(PlayerbotAI* ai) : Multiplier(ai, "save mana") {}

    public:
        virtual float GetValue(Action* action) override;
    };

    class HealerAutoSaveManaStrategy : public Strategy
    {
    public:
        HealerAutoSaveManaStrategy(PlayerbotAI* ai) : Strategy(ai) {}
        std::string getName() override { return "save mana"; }

#ifdef GenerateBotHelp
        virtual std::string GetHelpName() { return "save mana"; } //Must equal iternal name
        virtual std::string GetHelpDescription() {
            return "This strategy makes a healer at 60% mana or less pass over heals that would overheal,\n"
                   "and costly heals on members not in danger.";
        }
        virtual std::vector<std::string> GetRelatedStrategies() { return { }; }
#endif
    private:
        void InitCombatMultipliers(std::list<Multiplier*> &multipliers) override;
        void InitNonCombatMultipliers(std::list<Multiplier*> &multipliers) override;
    };
}
