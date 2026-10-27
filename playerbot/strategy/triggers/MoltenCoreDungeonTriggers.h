#pragma once
#include "DungeonTriggers.h"
#include "GenericTriggers.h"
#include "playerbot/strategy/generic/MoltenCoreDungeonStrategies.h"

namespace ai
{
    class MoltenCoreEnterDungeonTrigger : public EnterDungeonTrigger
    {
    public:
        MoltenCoreEnterDungeonTrigger(PlayerbotAI* ai) : EnterDungeonTrigger(ai, "enter molten core", "molten core", 409) {}
    };

    class MoltenCoreLeaveDungeonTrigger : public LeaveDungeonTrigger
    {
    public:
        MoltenCoreLeaveDungeonTrigger(PlayerbotAI* ai) : LeaveDungeonTrigger(ai, "leave molten core", "molten core", 409) {}
    };

    class MagmadarStartFightTrigger : public StartBossFightTrigger
    {
    public:
        MagmadarStartFightTrigger(PlayerbotAI* ai) : StartBossFightTrigger(ai, "start magmadar fight", "magmadar", 11982) {}
    };

    class MagmadarEndFightTrigger : public EndBossFightTrigger
    {
    public:
        MagmadarEndFightTrigger(PlayerbotAI* ai) : EndBossFightTrigger(ai, "end magmadar fight", "magmadar", 11982) {}
    };

    class MagmadarLavaBombTrigger : public CloseToGameObjectHazardTrigger
    {
    public:
        MagmadarLavaBombTrigger(PlayerbotAI* ai) : CloseToGameObjectHazardTrigger(ai, "magmadar lava bomb", 177704, 5.0f, 60) {}
    };

    class MagmadarTooCloseTrigger : public CloseToCreatureTrigger
    {
    public:
        MagmadarTooCloseTrigger(PlayerbotAI* ai) : CloseToCreatureTrigger(ai, "magmadar too close", 11982, 30.0f) {}
    };

    class FireProtectionPotionReadyTrigger : public ItemBuffReadyTrigger
    {
    public:
        FireProtectionPotionReadyTrigger(PlayerbotAI* ai) : ItemBuffReadyTrigger(ai, "fire protection potion ready", 13457, 17543) {}
    };

    class MCRuneInSightTrigger : public ValueTrigger
    {
    public:
        MCRuneInSightTrigger(PlayerbotAI* ai) : ValueTrigger(ai, "mc rune in sight", 1)
        {
            qualifier = "and::{"
                "action possible::use id::17333,"
                "has object::go usable filter::go trapped filter::entry filter::{gos in sight,mc runes},"
                "not::has object::entry filter::{gos close,mc runes}"
                "}";
        }
    };

    class MCRuneCloseTrigger : public ValueTrigger
    {
    public:
        MCRuneCloseTrigger(PlayerbotAI* ai) : ValueTrigger(ai, "mc rune close", 1) { qualifier = "has object::go usable filter::entry filter::{gos close,mc runes}"; }
    };

    class GolemaggStartFightTrigger : public StartBossFightTrigger
    {
    public:
        GolemaggStartFightTrigger(PlayerbotAI* ai) : StartBossFightTrigger(ai, "start golemagg fight", "golemagg", MoltenCore::NPC_GOLEMAGG) {}
    };

    class GolemaggEndFightTrigger : public EndBossFightTrigger
    {
    public:
        GolemaggEndFightTrigger(PlayerbotAI* ai) : EndBossFightTrigger(ai, "end golemagg fight", "golemagg", MoltenCore::NPC_GOLEMAGG) {}
    };

    // A trigger of the Golemagg fight, active only while the group fights him.
    class GolemaggTrigger : public Trigger
    {
    public:
        GolemaggTrigger(PlayerbotAI* ai, std::string name) : Trigger(ai, name, 1) {}

    protected:
        Unit* Golemagg();
    };

    // Any tank puts the skull on Golemagg.
    class McGolemaggMarkBossTrigger : public GolemaggTrigger
    {
    public:
        McGolemaggMarkBossTrigger(PlayerbotAI* ai) : GolemaggTrigger(ai, "mc golemagg mark boss") {}
        bool IsActive() override;
    };

    class McGolemaggIsMainTankTrigger : public GolemaggTrigger
    {
    public:
        McGolemaggIsMainTankTrigger(PlayerbotAI* ai) : GolemaggTrigger(ai, "mc golemagg is main tank") {}
        bool IsActive() override;
    };

    class McGolemaggIsAssistTankTrigger : public GolemaggTrigger
    {
    public:
        McGolemaggIsAssistTankTrigger(PlayerbotAI* ai) : GolemaggTrigger(ai, "mc golemagg is assist tank") {}
        bool IsActive() override;
    };

    class McGolemaggIsHealerTrigger : public GolemaggTrigger
    {
    public:
        McGolemaggIsHealerTrigger(PlayerbotAI* ai) : GolemaggTrigger(ai, "mc golemagg is healer") {}
        bool IsActive() override;
    };

    // A bot other than a tank, within reach of Golemagg with 20 or more Magma Splash stacks, outside the burn.
    class McGolemaggMagmaSplashTrigger : public GolemaggTrigger
    {
    public:
        McGolemaggMagmaSplashTrigger(PlayerbotAI* ai) : GolemaggTrigger(ai, "mc golemagg magma splash") {}
        bool IsActive() override;
    };
}