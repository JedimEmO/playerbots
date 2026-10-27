#pragma once
#include "playerbot/strategy/Strategy.h"

namespace ai
{
    // The Molten Core creatures and spells the fight strategies use, with the numbers AzerothCore tuned them by.
    namespace MoltenCore
    {
        enum : uint32
        {
            NPC_GOLEMAGG = 11988,
            NPC_CORE_RAGER = 11672,

            SPELL_GOLEMAGGS_TRUST = 20553,
            SPELL_MAGMA_SPLASH = 13880,
        };

        // Melee back off Golemagg at this many Magma Splash stacks, to this distance.
        constexpr uint32 MAGMA_SPLASH_BACK_OFF_STACKS = 20;
        constexpr float MAGMA_SPLASH_BACK_OFF_DISTANCE = 12.0f;
        // Below this much health Golemagg's fight is a burn: everyone attacks him.
        constexpr float GOLEMAGG_BURN_HEALTH_PCT = 10.0f;
    }

    class MoltenCoreDungeonStrategy : public Strategy
    {
    public:
        MoltenCoreDungeonStrategy(PlayerbotAI* ai) : Strategy(ai) {}
        std::string getName() override { return "molten core"; }

    private:
        void InitCombatTriggers(std::list<TriggerNode*>& triggers) override;
        void InitNonCombatTriggers(std::list<TriggerNode*>& triggers) override;
    };

    class MagmadarFightStrategy : public Strategy
    {
    public:
        MagmadarFightStrategy(PlayerbotAI* ai) : Strategy(ai) {}
        std::string getName() override { return "magmadar"; }

    private:
        void InitCombatTriggers(std::list<TriggerNode*>& triggers) override;
        void InitNonCombatTriggers(std::list<TriggerNode*>& triggers) override;
        void InitDeadTriggers(std::list<TriggerNode*>& triggers) override;
        void InitReactionTriggers(std::list<TriggerNode*>& triggers) override;
        void InitCombatMultipliers(std::list<Multiplier*>& multipliers) override;
    };

    // Golemagg the Incinerator, ported from AzerothCore. The main tank holds him at a fixed point, the first two
    // assist tanks drag a Core Rager each out of the range of Golemagg's Trust, healers stand between the two camps,
    // and melee back off when Magma Splash stacks up. Damage into the Core Ragers is wasted while he lives,
    // since they heal to full, so only the tanks may choose them. On from his pull until the bot leaves combat.
    class GolemaggFightStrategy : public Strategy
    {
    public:
        GolemaggFightStrategy(PlayerbotAI* ai) : Strategy(ai) {}
        std::string getName() override { return "golemagg"; }
        void AppendTargetExclusions(GuidSet& exclusions, TargetValueExclusionType type) override;
        bool HasTargetExclusions() const override { return true; }

    private:
        void InitCombatTriggers(std::list<TriggerNode*>& triggers) override;
        void InitNonCombatTriggers(std::list<TriggerNode*>& triggers) override;
        void InitDeadTriggers(std::list<TriggerNode*>& triggers) override;
        void InitCombatMultipliers(std::list<Multiplier*>& multipliers) override;
    };
}