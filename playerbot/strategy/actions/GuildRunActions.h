#pragma once
#include "playerbot/strategy/Action.h"
#include "AttackAction.h"
#include "MovementActions.h"
#include "playerbot/GuildRunRules.h"

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

    private:
        // How near a member far from its stand has come, for a new path when it stops gaining (runrules::Repath).
        runrules::Nearing nearing;
    };

    // A member apart from its party walks the next part of its walk back, exactly along the points the tick checked, as one
    // run move (GuildRunStates::StartRunMove, MotionMaster::MovePath with the token).
    class MoveToRunWalkAction : public Action
    {
    public:
        MoveToRunWalkAction(PlayerbotAI* ai) : Action(ai, "move to run walk") {}
        bool Execute(Event& event) override;
    };

    // After a hold, a hand-back or a change of members: the leader chooses its stop again from where it stands.
    class RunChooseAction : public Action
    {
    public:
        RunChooseAction(PlayerbotAI* ai) : Action(ai, "run choose") {}
        bool Execute(Event& event) override;
    };

    // The leader is at the pull spot: from here it waits for the party.
    class RunGatherAction : public Action
    {
    public:
        RunGatherAction(PlayerbotAI* ai) : Action(ai, "run gather") {}
        bool Execute(Event& event) override;
    };

    // The leader says who keeps the pull back now, as telemetry.
    class RunKeepersAction : public Action
    {
    public:
        RunKeepersAction(PlayerbotAI* ai) : Action(ai, "run keepers") {}
        bool Execute(Event& event) override;
    };

    // The leader blocks the pull with its reasons, the whole block predicate judged again now on a fresh view, and lets go of
    // any pull it had asked for.
    class RunBlockAction : public Action
    {
    public:
        RunBlockAction(PlayerbotAI* ai) : Action(ai, "run block") {}
        bool Execute(Event& event) override;
    };

    // The leader's waiting budget starts at the movement guard's refusal of its way to the spot (GuildRunStates::LeaderRefused).
    class RunRefusedAction : public Action
    {
    public:
        RunRefusedAction(PlayerbotAI* ai) : Action(ai, "run refused") {}
        bool Execute(Event& event) override;
    };

    // The leader pulls the stop's pack: its creature nearest the pull spot, from range by the pull strategy, which brings
    // the pull back to the spot. Within the pull's range and in sight of where the leader stands, the spell is cast from
    // there with no reach move; otherwise the strategy's pull reaches, as the movement guard allows. A tank that cannot
    // pull from range attacks it. A pack whose own creatures reach its pull spot is pulled from the stop before's, from
    // range and in sight only, else from its own spot. A pack out of range and out of sight from its own spot as well is
    // passed over. The pull phase is written before anything moves, so the movement guard lets the pull's own move close
    // on the pack, and goes back when nothing started. Sight is the pull spell's own (M2 doodads ignored). A leader more
    // than a yard off its spot that cannot cast from where it stands steps onto the spot once per gather before it reaches
    // or passes the stop over, and waits while that step's own move lasts, never past its bound (runrules::PullWayOf). The
    // step is kept in the value "run pull step", and said once it is over, before anything the pull writes.
    class RunPullAction : public AttackAction
    {
    public:
        RunPullAction(PlayerbotAI* ai) : AttackAction(ai, "run pull") {}
        bool Execute(Event& event) override;
        bool isPossible() override { return true; }
    };

    // The leader's step onto its spot is over where no pull comes to say it: in a fight, or once the run moved on from the
    // step's gather (a re-choose, a wait for a patrol, a pull that brought nothing, a new generation). Said once
    // (run_stepped), and the tick goes on.
    class RunSteppedAction : public Action
    {
    public:
        RunSteppedAction(PlayerbotAI* ai) : Action(ai, "run stepped") {}
        bool Execute(Event& event) override;
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

    // Another pack's creature reaches the way to the pull spot, the spot or its stands, by the guard's model: the party
    // waits at the stop before until none does, and the waiting budget starts.
    class RunWaitAction : public Action
    {
    public:
        RunWaitAction(PlayerbotAI* ai) : Action(ai, "run wait") {}
        bool Execute(Event& event) override;
    };

    // The leader decides the run's tactics again, on a fresh view and only under the trigger's stamp: the kill order and its
    // marks, the tank's target and the interrupts (GuildTactics::Decide).
    class RunTacticsAction : public Action
    {
    public:
        RunTacticsAction(PlayerbotAI* ai) : Action(ai, "run tactics") {}
        bool Execute(Event& event) override;
    };

    // A member interrupts the caster the run's plan gave it, under the trigger's stamp only (GuildTactics::Interrupt).
    class RunInterruptAction : public Action
    {
    public:
        RunInterruptAction(PlayerbotAI* ai) : Action(ai, "run interrupt") {}
        bool Execute(Event& event) override;
        bool isUseful() override;
    };

    // No blocker reaches the way, the spot or its stands, so the guard lets the walk: on to the spot.
    class RunGoAction : public Action
    {
    public:
        RunGoAction(PlayerbotAI* ai) : Action(ai, "run go") {}
        bool Execute(Event& event) override;
    };
}
