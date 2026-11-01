#include "playerbot/playerbot.h"
#include "GuildRunStrategy.h"

using namespace ai;

void RunLeadStrategy::InitNonCombatTriggers(std::list<TriggerNode*>& triggers)
{
    // After a hold, a hand-back or a change of members, the stop is chosen again from where the leader stands, first.
    triggers.push_back(new TriggerNode(
        "run choose",
        NextAction::array(0, new NextAction("run choose", ACTION_MOVE + 9), NULL)));

    // A party that cannot get ready blocks before anything moves it on (docs/run-contract.md, "A block"), and who keeps the
    // pull back is said as it changes.
    triggers.push_back(new TriggerNode(
        "run block",
        NextAction::array(0, new NextAction("run block", ACTION_MOVE + 10), NULL)));

    triggers.push_back(new TriggerNode(
        "run keepers",
        NextAction::array(0, new NextAction("run keepers", ACTION_MOVE + 0.5f), NULL)));

    // The movement guard refused the leader's way to its spot: its waiting budget starts, as at a gather.
    triggers.push_back(new TriggerNode(
        "run refused",
        NextAction::array(0, new NextAction("run refused", ACTION_MOVE + 9.5f), NULL)));

    // A cleared stop moves the run on before anything else, so the leader never walks to a dead pack's spot.
    triggers.push_back(new TriggerNode(
        "run cleared",
        NextAction::array(0, new NextAction("run next", ACTION_MOVE + 8), NULL)));

    // Then the wait at the stop before while another pack's creature reaches the way to the pull spot, the spot or its
    // stands: its start, and the creature gone. It has no timer: one that never clears blocks through "run block".
    triggers.push_back(new TriggerNode(
        "run reached",
        NextAction::array(0, new NextAction("run wait", ACTION_MOVE + 6), NULL)));

    triggers.push_back(new TriggerNode(
        "run clear",
        NextAction::array(0, new NextAction("run go", ACTION_MOVE + 6), NULL)));

    // Then the pull, the walk to the spot, and the stop at it.
    triggers.push_back(new TriggerNode(
        "run pull ready",
        NextAction::array(0, new NextAction("run pull", ACTION_MOVE + 3), NULL)));

    triggers.push_back(new TriggerNode(
        "run approach",
        NextAction::array(0, new NextAction("move to run spot", ACTION_MOVE + 2), NULL)));

    triggers.push_back(new TriggerNode(
        "run gather",
        NextAction::array(0, new NextAction("run gather", ACTION_MOVE + 1), NULL)));

    triggers.push_back(new TriggerNode(
        "run pull failed",
        NextAction::array(0, new NextAction("run pull failed", ACTION_HIGH), NULL)));
}

void RunLeadStrategy::InitCombatTriggers(std::list<TriggerNode*>& triggers)
{
    // The budget is watched in a fight too: a fight that does not end at the spot keeps the pull back, and blocks it.
    triggers.push_back(new TriggerNode(
        "run block",
        NextAction::array(0, new NextAction("run block", ACTION_MOVE + 10), NULL)));

    // While the pull comes, the leader stays at the spot rather than meeting the pack on its way.
    triggers.push_back(new TriggerNode(
        "run hold",
        NextAction::array(0, new NextAction("move to run spot", ACTION_MOVE + 5), NULL)));
}

void RunPartyStrategy::InitNonCombatTriggers(std::list<TriggerNode*>& triggers)
{
    triggers.push_back(new TriggerNode(
        "run stand",
        NextAction::array(0, new NextAction("move to run spot", ACTION_MOVE + 2), NULL)));
}

void RunHoldingStrategy::InitNonCombatTriggers(std::list<TriggerNode*>& triggers)
{
    // A member apart walks back to its party over the route's checked way, one part at a time.
    triggers.push_back(new TriggerNode(
        "run walk",
        NextAction::array(0, new NextAction("move to run walk", ACTION_MOVE + 3), NULL)));
    triggers.push_back(new TriggerNode(
        "run hold place",
        NextAction::array(0, new NextAction("move to run spot", ACTION_MOVE + 2), NULL)));
}

void RunPartyStrategy::InitCombatTriggers(std::list<TriggerNode*>& triggers)
{
    // A trial: a member drawn far from its place in the fight goes back to it.
    triggers.push_back(new TriggerNode(
        "run leash",
        NextAction::array(0, new NextAction("move to run spot", ACTION_MOVE + 4), NULL)));
}
