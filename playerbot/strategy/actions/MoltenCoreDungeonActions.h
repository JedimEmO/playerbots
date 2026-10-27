#pragma once
#include "DungeonActions.h"
#include "ChangeStrategyAction.h"
#include "UseItemAction.h"
#include "AttackAction.h"
#include "playerbot/strategy/generic/MoltenCoreDungeonStrategies.h"

namespace ai
{
    class MoltenCoreEnableDungeonStrategyAction : public ChangeAllStrategyAction
    {
    public:
        MoltenCoreEnableDungeonStrategyAction(PlayerbotAI* ai) : ChangeAllStrategyAction(ai, "enable molten core strategy", "+molten core") {}
    };

    class MoltenCoreDisableDungeonStrategyAction : public ChangeAllStrategyAction
    {
    public:
        MoltenCoreDisableDungeonStrategyAction(PlayerbotAI* ai) : ChangeAllStrategyAction(ai, "disable molten core strategy", "-molten core") {}
    };

    class MagmadarEnableFightStrategyAction : public ChangeAllStrategyAction
    {
    public:
        MagmadarEnableFightStrategyAction(PlayerbotAI* ai) : ChangeAllStrategyAction(ai, "enable magmadar fight strategy", "+magmadar") {}
    };

    class MagmadarDisableFightStrategyAction : public ChangeAllStrategyAction
    {
    public:
        MagmadarDisableFightStrategyAction(PlayerbotAI* ai) : ChangeAllStrategyAction(ai, "disable magmadar fight strategy", "-magmadar") {}
    };

    class MagmadarMoveAwayFromLavaBombAction : public MoveAwayFromHazard
    {
    public:
        MagmadarMoveAwayFromLavaBombAction(PlayerbotAI* ai) : MoveAwayFromHazard(ai, "move away from magmadar lava bomb") {}
    };

    class MagmadarMoveAwayAction : public MoveAwayFromCreature
    {
    public:
        MagmadarMoveAwayAction(PlayerbotAI* ai) : MoveAwayFromCreature(ai, "move away from magmadar", 11982, 31.0f) {}
    };

    class MoveToMCRuneAction : public MoveToAction
    {
    public:
        MoveToMCRuneAction(PlayerbotAI* ai) : MoveToAction(ai, "move to mc rune") { qualifier = "entry filter::{gos in sight,mc runes}"; }
    };

    class DouseMCRuneActionAqual : public UseItemIdAction
    {
    public:
        DouseMCRuneActionAqual(PlayerbotAI* ai) : UseItemIdAction(ai, "douse mc rune aqual") { qualifier = "{17333,entry filter::{gos close,mc runes}}"; }
    };

    class DouseMCRuneActionEternal : public UseItemIdAction
    {
    public:
        DouseMCRuneActionEternal(PlayerbotAI* ai) : UseItemIdAction(ai, "douse mc rune eternal") { qualifier = "{22754,entry filter::{gos close,mc runes}}"; }
    };

    class GolemaggEnableFightStrategyAction : public ChangeAllStrategyAction
    {
    public:
        GolemaggEnableFightStrategyAction(PlayerbotAI* ai) : ChangeAllStrategyAction(ai, "enable golemagg fight strategy", "+golemagg") {}
    };

    class GolemaggDisableFightStrategyAction : public ChangeAllStrategyAction
    {
    public:
        GolemaggDisableFightStrategyAction(PlayerbotAI* ai) : ChangeAllStrategyAction(ai, "disable golemagg fight strategy", "-golemagg") {}
    };

    // Puts the skull on Golemagg, unless it is there already.
    class McGolemaggMarkBossAction : public Action
    {
    public:
        McGolemaggMarkBossAction(PlayerbotAI* ai) : Action(ai, "mc golemagg mark boss") {}
        bool Execute(Event& event) override;
        bool isUseful() override;
    };

    // What Golemagg's tanks share: holding a unit, walking it to a point, and finding the two Core Ragers.
    class McGolemaggTankAction : public AttackAction
    {
    public:
        McGolemaggTankAction(PlayerbotAI* ai, std::string name) : AttackAction(ai, name) {}

    protected:
        Unit* Golemagg();
        // Attacks the target until it attacks the bot, taunting it off anyone else,
        // then steps toward the point while it is further than the distance.
        bool MoveUnitToPosition(Unit* target, float x, float y, float maxDistance, float stepDistance = 3.0f);
        bool FindCoreRagers(Unit*& first, Unit*& second);
        bool Taunt();
        bool AttackUnit(Unit* target) { return Attack(GetMaster(), target); }
    };

    // The main tank holds Golemagg at his tank point while a Core Rager still has Golemagg's Trust.
    class McGolemaggMainTankAttackGolemaggAction : public McGolemaggTankAction
    {
    public:
        McGolemaggMainTankAttackGolemaggAction(PlayerbotAI* ai) : McGolemaggTankAction(ai, "mc golemagg main tank attack golemagg") {}
        bool Execute(Event& event) override;
    };

    // Living assist tanks 0 and 1 hold a Core Rager each, the first and the second found, and take both when the other
    // is dead; they drag their Rager to the Core Rager point, or away from Golemagg, until Golemagg's Trust is gone.
    // Further assist tanks attack Golemagg.
    class McGolemaggAssistTankAttackCoreRagerAction : public McGolemaggTankAction
    {
    public:
        McGolemaggAssistTankAttackCoreRagerAction(PlayerbotAI* ai) : McGolemaggTankAction(ai, "mc golemagg assist tank attack core rager") {}
        bool Execute(Event& event) override;
    };

    // Healers stand at the midpoint of the two tank camps, from where they reach both.
    class McGolemaggHealerPositionAction : public MovementAction
    {
    public:
        McGolemaggHealerPositionAction(PlayerbotAI* ai) : MovementAction(ai, "mc golemagg healer position") {}
        bool Execute(Event& event) override;
    };

    // Steps out of Golemagg's reach, so the Magma Splash stack stops growing and runs out.
    class McGolemaggBackOffAction : public MovementAction
    {
    public:
        McGolemaggBackOffAction(PlayerbotAI* ai) : MovementAction(ai, "mc golemagg back off") {}
        bool Execute(Event& event) override;
    };
}