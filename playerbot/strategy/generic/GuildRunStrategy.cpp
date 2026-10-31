#include "playerbot/playerbot.h"
#include "GuildRunStrategy.h"

using namespace ai;

void RunLeadStrategy::InitNonCombatTriggers(std::list<TriggerNode*>& triggers)
{
    // A cleared stop moves the run on before anything else, so the leader never walks to a dead pack's spot.
    triggers.push_back(new TriggerNode(
        "run cleared",
        NextAction::array(0, new NextAction("run next", ACTION_MOVE + 8), NULL)));

    // Then the wait at the stop before while another pack's creature reaches the pull spot: its end after
    // GuildRunStates::WAIT_SECONDS, when the party goes on anyway, its start, and the creature gone.
    triggers.push_back(new TriggerNode(
        "run wait over",
        NextAction::array(0, new NextAction("run waited", ACTION_MOVE + 7), NULL)));

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

void RunPartyStrategy::InitCombatTriggers(std::list<TriggerNode*>& triggers)
{
    // A trial: a member drawn far from its place in the fight goes back to it.
    triggers.push_back(new TriggerNode(
        "run leash",
        NextAction::array(0, new NextAction("move to run spot", ACTION_MOVE + 4), NULL)));
}
