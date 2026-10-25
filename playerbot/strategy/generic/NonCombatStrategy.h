#pragma once
#include "PassTroughStrategy.h"

namespace ai
{
    // TO DO: Remove this when no dependencies are left
    class NonCombatStrategy : public Strategy
    {
    public:
        NonCombatStrategy(PlayerbotAI* ai) : Strategy(ai) {}
		virtual int GetType() override { return STRATEGY_TYPE_NONCOMBAT; }
        std::string getName() override { return "nc"; }
#ifdef GenerateBotHelp
        virtual std::string GetHelpName() { return "nc"; } //Must equal iternal name
        virtual std::string GetHelpDescription() {
            return "Generic noncombat behavior strategy. Currently handles bots mounting and moves them for the dark portal event.";
        }
        virtual std::vector<std::string> GetRelatedStrategies() { return {}; }
#endif
    protected:
        virtual void InitNonCombatTriggers(std::list<TriggerNode*> &triggers) override;
    };

    class CollisionStrategy : public Strategy
    {
    public:
        CollisionStrategy(PlayerbotAI* ai) : Strategy(ai) {}
		int GetType() override { return STRATEGY_TYPE_NONCOMBAT; }
        std::string getName() override { return "collision"; }
#ifdef GenerateBotHelp
        virtual std::string GetHelpName() { return "collision"; } //Must equal iternal name
        virtual std::string GetHelpDescription() {
            return "This strategy makes bots move away from other bots a little if they are touching.";
        }
        virtual std::vector<std::string> GetRelatedStrategies() { return {}; }
#endif
    private:
        void InitNonCombatTriggers(std::list<TriggerNode*> &triggers) override;
    };

    class MountStrategy : public Strategy
    {
    public:
        MountStrategy(PlayerbotAI* ai) : Strategy(ai) {};
        int GetType() override { return STRATEGY_TYPE_NONCOMBAT; }
        std::string getName() override { return "mount"; }
#ifdef GenerateBotHelp
        virtual std::string GetHelpName() { return "mount"; } //Must equal iternal name
        virtual std::string GetHelpDescription() {
            return "This is a toggle strategy which tells bots if they are allowed to mount up or not.";
        }
        virtual std::vector<std::string> GetRelatedStrategies() { return {}; }
#endif
    private:
        void InitNonCombatTriggers(std::list<TriggerNode*>& triggers) override;
    };

    class AttackTaggedStrategy : public Strategy
    {
    public:
        AttackTaggedStrategy(PlayerbotAI* ai) : Strategy(ai) {}
        int GetType() override { return STRATEGY_TYPE_NONCOMBAT; }
        std::string getName() override { return "attack tagged"; }
#ifdef GenerateBotHelp
        virtual std::string GetHelpName() { return "attack tagged"; } //Must equal iternal name
        virtual std::string GetHelpDescription() {
            return "This is a toggle strategy which tells bots if they are allowed to attack mobs tagged by players (not in their group).";
        }
        virtual std::vector<std::string> GetRelatedStrategies() { return {}; }
#endif
    };

    class WorldBuffStrategy : public Strategy
    {
    public:
        WorldBuffStrategy(PlayerbotAI* ai) : Strategy(ai) {}
        virtual int GetType() override { return STRATEGY_TYPE_NONCOMBAT; }
        std::string getName() override { return "wbuff"; }
#ifdef GenerateBotHelp
        virtual std::string GetHelpName() { return "wbuff"; } //Must equal iternal name
        virtual std::string GetHelpDescription() {
            return "This cheat strategy which automatically applies predefined worldbuffs to the bots if they don't have them.";
        }
        virtual std::vector<std::string> GetRelatedStrategies() { return {}; }
#endif
    protected:
        virtual void InitNonCombatTriggers(std::list<TriggerNode*>& triggers) override;
        void OnStrategyRemoved(BotState state) override;
    };

    class SilentStrategy : public Strategy
    {
    public:
        SilentStrategy(PlayerbotAI* ai) : Strategy(ai) {}
        virtual int GetType() override { return STRATEGY_TYPE_NONCOMBAT; }
        virtual std::string getName() override { return "silent"; }
#ifdef GenerateBotHelp
        virtual std::string GetHelpName() { return "wbuff"; } //Must equal iternal name
        virtual std::string GetHelpDescription() {
            return "This cheat strategy which automatically applies predefined worldbuffs to the bots if they don't have them.";
        }
        virtual std::vector<std::string> GetRelatedStrategies() { return {}; }
#endif
    };

    class NoWarStrategy : public Strategy
    {
    public:
        NoWarStrategy(PlayerbotAI* ai) : Strategy(ai) {}
        virtual int GetType() override { return STRATEGY_TYPE_NONCOMBAT; }
        virtual std::string getName() override { return "nowar"; }
#ifdef GenerateBotHelp
        virtual std::string GetHelpName() { return "nowar"; } //Must equal iternal name
        virtual std::string GetHelpDescription() {
            return "This a toggle strategy that removes the 'at war' check for all factions from time to time.";
        }
        virtual std::vector<std::string> GetRelatedStrategies() { return {}; }
#endif
    protected:
        virtual void InitNonCombatTriggers(std::list<TriggerNode*>& triggers) override;
    };

    class GlyphStrategy : public Strategy
    {
    public:
        GlyphStrategy(PlayerbotAI* ai) : Strategy(ai) {}
        virtual int GetType() override { return STRATEGY_TYPE_NONCOMBAT; }
        virtual std::string getName() override { return "glyph"; }
#ifdef GenerateBotHelp
        virtual std::string GetHelpName() { return "glyph"; } //Must equal iternal name
        virtual std::string GetHelpDescription() {
            return "Automatically applies the best glyphs based on the config.";
        }
        virtual std::vector<std::string> GetRelatedStrategies() { return {}; }
#endif
    protected:
        virtual void InitNonCombatTriggers(std::list<TriggerNode*>& triggers) override;
    };

    class FishStrategy : public Strategy
    {
    public:
        FishStrategy(PlayerbotAI* ai) : Strategy(ai) {}
        virtual int GetType() override { return STRATEGY_TYPE_NONCOMBAT; }
        virtual std::string getName() override { return "fish"; }
#ifdef GenerateBotHelp
        virtual std::string GetHelpName() { return "fish"; } //Must equal iternal name
        virtual std::string GetHelpDescription() {
            return "Automatically tries to fish in some water nearby.";
        }
        virtual std::vector<std::string> GetRelatedStrategies() { return {}; }
#endif
    protected:
        virtual void InitNonCombatTriggers(std::list<TriggerNode*>& triggers) override;
        std::string modifier = "";
    };

    class TFishStrategy : public FishStrategy
    {
    public:
        TFishStrategy(PlayerbotAI* ai) : FishStrategy(ai) { modifier = "::travel"; }

        virtual std::string getName() override { return "tfish"; }
#ifdef GenerateBotHelp
        virtual std::string GetHelpName() { return "tfish"; } //Must equal iternal name
        virtual std::string GetHelpDescription() {
            return "Fishing during travel.";
        }
        virtual std::vector<std::string> GetRelatedStrategies() { return {}; }
#endif
    };

    // wow-playground: farming gathering materials for a player. The strategy has no triggers of its own; the values and
    // actions that ask for it keep gathering trips needed past the skill cap, keep trade goods out of vendor and
    // auction sales, and stop crafting from using them up. `.rndbot mailgoods` then mails them to the player.
    class FarmStrategy : public Strategy
    {
    public:
        FarmStrategy(PlayerbotAI* ai) : Strategy(ai) {}

        virtual std::string getName() override { return "farm"; }
#ifdef GenerateBotHelp
        virtual std::string GetHelpName() { return "farm"; } //Must equal iternal name
        virtual std::string GetHelpDescription() {
            return "Keeps gathering past the skill cap and keeps trade goods instead of selling or crafting them.";
        }
        virtual std::vector<std::string> GetRelatedStrategies() { return {}; }
#endif
    };

    // wow-playground: levelling a player's character by grinding. The travel values that ask for it leave only the
    // trips that serve grinding: grind spots (GrindSpotTravelDestination) at any time of the hour, vendors, repairs,
    // mail, the bank and trainers. Auction house, boss, explore, generic rpg, city, world pvp and gathering trips are
    // off, and ChooseRpgTargetAction skips npcs it has no business with. Its one trigger walks the bot between the
    // spawns of the grind spot it works when it has nothing to fight.
    class LevellingStrategy : public Strategy
    {
    public:
        LevellingStrategy(PlayerbotAI* ai) : Strategy(ai) {}
    protected:
        virtual void InitNonCombatTriggers(std::list<TriggerNode*>& triggers) override;
    public:

        virtual std::string getName() override { return "levelling"; }
#ifdef GenerateBotHelp
        virtual std::string GetHelpName() { return "levelling"; } //Must equal iternal name
        virtual std::string GetHelpDescription() {
            return "Travels only to grind, sell, repair, get mail, bank and train.";
        }
        virtual std::vector<std::string> GetRelatedStrategies() { return {}; }
#endif
    };
}
