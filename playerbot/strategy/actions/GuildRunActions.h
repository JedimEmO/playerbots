#pragma once
#include "playerbot/strategy/Action.h"
#include "AttackAction.h"
#include "MovementActions.h"

namespace ai
{
    // Rebuilds the bot's strategies, out of a fight, as its run state now calls for (GuildRunEnterTrigger).
    class GuildRunStrategiesAction : public Action
    {
    public:
        GuildRunStrategiesAction(PlayerbotAI* ai) : Action(ai, "guild run strategies") {}
        bool Execute(Event& event) override;
    };

    // To the bot's place at the stop: the pull spot, a stand point, or by the leader on the way.
    // Always a pathed move, and only with the bot in the world and on the place's map.
    class MoveToRunSpotAction : public MovementAction
    {
    public:
        MoveToRunSpotAction(PlayerbotAI* ai) : MovementAction(ai, "move to run spot") {}
        bool Execute(Event& event) override;
        bool isUseful() override;
    };

    // The leader is at the pull spot: from here it waits for the party.
    class RunGatherAction : public Action
    {
    public:
        RunGatherAction(PlayerbotAI* ai) : Action(ai, "run gather") {}
        bool Execute(Event& event) override;
    };

    // The leader pulls the stop's pack: its creature nearest the pull spot, from range by the pull strategy, which brings
    // the pull back to the spot. A tank that cannot pull from range attacks it. A pack whose own creatures reach its pull
    // spot is pulled from the stop before's, from range and in sight only, else from its own spot. A pack out of range
    // and out of sight from its own spot as well is passed over.
    class RunPullAction : public AttackAction
    {
    public:
        RunPullAction(PlayerbotAI* ai) : AttackAction(ai, "run pull") {}
        bool Execute(Event& event) override;
        bool isPossible() override { return true; }
    };

    // A pull that brought nothing: the leader gathers and pulls again, and after a few tries goes on to the next stop.
    class RunPullFailedAction : public Action
    {
    public:
        RunPullFailedAction(PlayerbotAI* ai) : Action(ai, "run pull failed") {}
        bool Execute(Event& event) override;
    };

    // The stop's pack is dead: on to the next stop with a living pack.
    class RunNextAction : public Action
    {
    public:
        RunNextAction(PlayerbotAI* ai) : Action(ai, "run next") {}
        bool Execute(Event& event) override;
    };

    // Another pack's creature reaches the pull spot: the party waits at the stop before until it leaves.
    class RunWaitAction : public Action
    {
    public:
        RunWaitAction(PlayerbotAI* ai) : Action(ai, "run wait") {}
        bool Execute(Event& event) override;
    };

    // The pull spot is out of reach again: on to it.
    class RunGoAction : public Action
    {
    public:
        RunGoAction(PlayerbotAI* ai) : Action(ai, "run go") {}
        bool Execute(Event& event) override;
    };

    // Waited long enough: on to the pull spot with the other pack's creature still in reach, so that a run never stands.
    class RunWaitedAction : public Action
    {
    public:
        RunWaitedAction(PlayerbotAI* ai) : Action(ai, "run waited") {}
        bool Execute(Event& event) override;
    };
}
