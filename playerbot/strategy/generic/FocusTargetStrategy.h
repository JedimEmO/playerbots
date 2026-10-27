#pragma once
#include "playerbot/strategy/Strategy.h"

namespace ai
{
    class FocusHealTargetsStrategy : public Strategy
    {
    public:
        FocusHealTargetsStrategy(PlayerbotAI* ai) : Strategy(ai) {}
        std::string getName() override { return "focus heal targets"; }

#ifdef GenerateBotHelp
        virtual std::string GetHelpName() { return "focus heal targets"; } //Must equal iternal name
        virtual std::string GetHelpDescription()
        {
            return "This strategy will make the bot focus heal the specified target using the 'set focus heal <targetname>' command";
        }
#endif
    };

    class FocusRtiTargetsStrategy : public Strategy
    {
    public:
        FocusRtiTargetsStrategy(PlayerbotAI* ai) : Strategy(ai) {}
        std::string getName() override { return "focus rti targets"; }

#ifdef GenerateBotHelp
        virtual std::string GetHelpName() { return "focus rti targets"; } //Must equal iternal name
        virtual std::string GetHelpDescription()
        {
            return "This strategy will make the bot ignore all targets except it's current rti target.";
        }
#endif
    };    

    // Keeps the bot from choosing whatever wears the cross mark as its dps or debuff target, and from counting it
    // for area damage; a tank still picks it up. It tests the target exclusions, and is only ever added by hand.
    class AvoidCrossStrategy : public Strategy
    {
    public:
        AvoidCrossStrategy(PlayerbotAI* ai) : Strategy(ai) {}
        std::string getName() override { return "avoid cross"; }
        void AppendTargetExclusions(GuidSet& exclusions, TargetValueExclusionType type) override;
        bool HasTargetExclusions() const override { return true; }
    };
}