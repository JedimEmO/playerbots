
#include "playerbot/playerbot.h"
#include "MoltenCoreDungeonStrategies.h"
#include "DungeonMultipliers.h"
#include "playerbot/strategy/actions/MoltenCoreDungeonActions.h"
#include "playerbot/strategy/actions/ChooseTargetActions.h"
#include "playerbot/strategy/actions/GenericActions.h"
#include "playerbot/strategy/actions/ReachTargetActions.h"

using namespace ai;
using namespace ai::MoltenCore;

void MoltenCoreDungeonStrategy::InitCombatTriggers(std::list<TriggerNode*>& triggers)
{
    triggers.push_back(new TriggerNode(
        "start magmadar fight",
        NextAction::array(0, new NextAction("enable magmadar fight strategy", 100.0f), NULL)));

    triggers.push_back(new TriggerNode(
        "start golemagg fight",
        NextAction::array(0, new NextAction("enable golemagg fight strategy", 100.0f), NULL)));
}

void MoltenCoreDungeonStrategy::InitNonCombatTriggers(std::list<TriggerNode*>& triggers)
{
    /*
    triggers.push_back(new TriggerNode(
        "val::and::{"
        "action possible::use id::17333,"
        "has object::go usable filter::go trapped filter::entry filter::{gos in sight,mc runes},"
        "not::has object::entry filter::{gos close,mc runes}"
        "}",
        NextAction::array(0, new NextAction("move to::entry filter::{gos in sight,mc runes}", 1.0f), NULL)));

    triggers.push_back(new TriggerNode(
        "val::has object::go usable filter::entry filter::{gos close,mc runes}",
        NextAction::array(0, new NextAction("use id::{17333,entry filter::{gos close,mc runes}}", 1.0f), NULL)));
        */

    triggers.push_back(new TriggerNode(
        "mc rune in sight",
        NextAction::array(0, new NextAction("move to mc rune", 1.0f), NULL)));

    triggers.push_back(new TriggerNode(
        "mc rune close",
        NextAction::array(0,
            new NextAction("douse mc rune eternal", 2.0f),
            new NextAction("douse mc rune aqual", 1.0f), NULL)));

    triggers.push_back(new TriggerNode(
        "fire protection potion ready",
        NextAction::array(0, new NextAction("fire protection potion", 100.0f), NULL)));
}

void MagmadarFightStrategy::InitCombatTriggers(std::list<TriggerNode*>& triggers)
{
    Player* bot = ai->GetBot();
    if (ai->IsRanged(bot) || ai->IsHeal(bot))
    {
        triggers.push_back(new TriggerNode(
            "magmadar too close",
            NextAction::array(0, new NextAction("move away from magmadar", 100.0f), NULL)));
    }

    triggers.push_back(new TriggerNode(
        "fire protection potion ready",
        NextAction::array(0, new NextAction("fire protection potion", 100.0f), NULL)));
}

void MagmadarFightStrategy::InitNonCombatTriggers(std::list<TriggerNode*>& triggers)
{
    triggers.push_back(new TriggerNode(
        "end magmadar fight",
        NextAction::array(0, new NextAction("disable magmadar fight strategy", 100.0f), NULL)));
}

void MagmadarFightStrategy::InitDeadTriggers(std::list<TriggerNode*>& triggers)
{
    triggers.push_back(new TriggerNode(
        "end magmadar fight",
        NextAction::array(0, new NextAction("disable magmadar fight strategy", 100.0f), NULL)));
}

void MagmadarFightStrategy::InitReactionTriggers(std::list<TriggerNode*>& triggers)
{
    triggers.push_back(new TriggerNode(
        "magmadar lava bomb",
        NextAction::array(0, new NextAction("move away from hazard", 100.0f), NULL)));
}

void MagmadarFightStrategy::InitCombatMultipliers(std::list<Multiplier*>& multipliers)
{
    Player* bot = ai->GetBot();
    if (ai->IsRanged(bot) || ai->IsHeal(bot))
    {
        multipliers.push_back(new PreventMoveAwayFromCreatureOnReachToCastMultiplier(ai));
    }
}


// AzerothCore's GolemaggMultiplier: who may do what while Golemagg is in the fight.
class GolemaggMultiplier : public Multiplier
{
public:
    GolemaggMultiplier(PlayerbotAI* ai) : Multiplier(ai, "golemagg") {}

    float GetValue(ai::Action* action) override
    {
        Unit* golemagg = ai->GetUnit(AI_VALUE2(ObjectGuid, "find target", "golemagg the incinerator"));
        if (!action || !golemagg)
            return 1.0f;

        Player* bot = ai->GetBot();
        bool burn = golemagg->GetHealthPercent() <= GOLEMAGG_BURN_HEALTH_PCT;
        bool tank = PlayerbotAI::IsTank(bot);

        // A lone tank takes Golemagg and both Core Ragers by its usual targeting.
        if (tank && IsOnlyLivingTank(bot) &&
            (dynamic_cast<McGolemaggMainTankAttackGolemaggAction*>(action) || dynamic_cast<McGolemaggAssistTankAttackCoreRagerAction*>(action)))
            return 0.0f;

        // The first two assist tanks keep to their Core Ragers; any further ones fight the boss through their own action.
        if (PlayerbotAI::IsAssistTank(bot) && dynamic_cast<TankAssistAction*>(action))
            return 0.0f;

        // No area damage from the dps: it would hit the Core Ragers.
        if (!tank && !PlayerbotAI::IsHeal(bot) && action->getThreatType() == ActionThreatType::ACTION_THREAT_AOE)
            return 0.0f;

        // Ranged do not walk in to melee him, which stacks Magma Splash on them.
        if (!burn && ai->IsRanged(bot) && dynamic_cast<MeleeAction*>(action))
            return 0.0f;

        // Melee that backed off stay out until their whole stack has expired, 30 seconds after the last.
        SpellAuraHolder* splash = bot->GetSpellAuraHolder(SPELL_MAGMA_SPLASH);
        bool backedOff = splash && splash->GetStackAmount() >= MAGMA_SPLASH_BACK_OFF_STACKS;
        bool engages = !dynamic_cast<McGolemaggBackOffAction*>(action) &&
            (dynamic_cast<AttackAction*>(action) || dynamic_cast<ReachTargetAction*>(action) || dynamic_cast<CastReachTargetSpellAction*>(action));
        if (!burn && !tank && backedOff && engages)
            return 0.0f;

        return 1.0f;
    }

private:
    static bool IsOnlyLivingTank(Player* bot)
    {
        Group* group = bot->GetGroup();
        if (!group)
            return true;

        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player* member = ref->getSource();
            if (member && member != bot && member->IsAlive() && PlayerbotAI::IsTank(member))
                return false;
        }

        return true;
    }
};

void GolemaggFightStrategy::InitCombatTriggers(std::list<TriggerNode*>& triggers)
{
    triggers.push_back(new TriggerNode(
        "mc golemagg mark boss",
        NextAction::array(0, new NextAction("mc golemagg mark boss", ACTION_RAID), NULL)));

    triggers.push_back(new TriggerNode(
        "mc golemagg is main tank",
        NextAction::array(0, new NextAction("mc golemagg main tank attack golemagg", ACTION_RAID), NULL)));

    triggers.push_back(new TriggerNode(
        "mc golemagg is assist tank",
        NextAction::array(0, new NextAction("mc golemagg assist tank attack core rager", ACTION_RAID), NULL)));

    triggers.push_back(new TriggerNode(
        "mc golemagg magma splash",
        NextAction::array(0, new NextAction("mc golemagg back off", 100.0f), NULL)));

    triggers.push_back(new TriggerNode(
        "mc golemagg is healer",
        NextAction::array(0, new NextAction("mc golemagg healer position", ACTION_RAID), NULL)));

    triggers.push_back(new TriggerNode(
        "fire protection potion ready",
        NextAction::array(0, new NextAction("fire protection potion", 100.0f), NULL)));
}

void GolemaggFightStrategy::InitNonCombatTriggers(std::list<TriggerNode*>& triggers)
{
    triggers.push_back(new TriggerNode(
        "end golemagg fight",
        NextAction::array(0, new NextAction("disable golemagg fight strategy", 100.0f), NULL)));
}

void GolemaggFightStrategy::InitDeadTriggers(std::list<TriggerNode*>& triggers)
{
    triggers.push_back(new TriggerNode(
        "end golemagg fight",
        NextAction::array(0, new NextAction("disable golemagg fight strategy", 100.0f), NULL)));
}

void GolemaggFightStrategy::InitCombatMultipliers(std::list<Multiplier*>& multipliers)
{
    multipliers.push_back(new GolemaggMultiplier(ai));
}

void GolemaggFightStrategy::AppendTargetExclusions(GuidSet& exclusions, TargetValueExclusionType type)
{
    // Tanks pick the Core Ragers up by their own choice, and their area threat may hit them.
    if (type == TargetValueExclusionType::TankTarget || (type == TargetValueExclusionType::Aoe && PlayerbotAI::IsTank(ai->GetBot())))
        return;

    AiObjectContext* context = ai->GetAiObjectContext();
    if (!AI_VALUE2(ObjectGuid, "find target", "golemagg the incinerator"))
        return;

    for (const ObjectGuid& guid : AI_VALUE(std::list<ObjectGuid>, "attackers"))
    {
        if (guid.GetEntry() == NPC_CORE_RAGER)
            exclusions.insert(guid);
    }
}
