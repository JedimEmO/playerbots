#pragma once
#include "playerbot/strategy/Trigger.h"

namespace ai
{
    // A trigger of the run's: its event carries the generation and plan revision of the run state it saw, so that the
    // action it fires does nothing once the state has moved on (GuildRunStates::Current).
    class RunTrigger : public Trigger
    {
    public:
        RunTrigger(PlayerbotAI* ai, std::string name) : Trigger(ai, name) {}
        Event Check() override;
    };

    // A guild character whose run strategies are not the ones its run state calls for, out of a fight: it enters the
    // run (`run lead` or `run party`) or leaves it. Strategies are rebuilt only out of a fight, since a rebuild
    // re-initialises the engines and has cut fights short.
    class GuildRunEnterTrigger : public Trigger
    {
    public:
        GuildRunEnterTrigger(PlayerbotAI* ai) : Trigger(ai, "guild run enter", 2) {}
        bool IsActive() override;
    };

    class GuildRunLeaveTrigger : public Trigger
    {
    public:
        GuildRunLeaveTrigger(PlayerbotAI* ai) : Trigger(ai, "guild run leave", 2) {}
        bool IsActive() override;
    };

    // The leader on its way to the stop's pull spot.
    class RunApproachTrigger : public RunTrigger
    {
    public:
        RunApproachTrigger(PlayerbotAI* ai) : RunTrigger(ai, "run approach") {}
        bool IsActive() override;
    };

    // After a hold, a hand-back or a change of members, the leader chooses its stop again from where it stands.
    class RunChooseTrigger : public RunTrigger
    {
    public:
        RunChooseTrigger(PlayerbotAI* ai) : RunTrigger(ai, "run choose") {}
        bool IsActive() override;
    };

    // In the mode hold, a living member the engine plays inside, off the place the run holds it at.
    class RunHoldPlaceTrigger : public RunTrigger
    {
    public:
        RunHoldPlaceTrigger(PlayerbotAI* ai) : RunTrigger(ai, "run hold place") {}
        bool IsActive() override;
    };

    // A member apart from its party, with a walk back to it: at the start of its next part, and not already walking one.
    class RunWalkTrigger : public RunTrigger
    {
    public:
        RunWalkTrigger(PlayerbotAI* ai) : RunTrigger(ai, "run walk") {}
        bool IsActive() override;
    };

    // The leader has come to the pull spot.
    class RunGatherTrigger : public RunTrigger
    {
    public:
        RunGatherTrigger(PlayerbotAI* ai) : RunTrigger(ai, "run gather") {}
        bool IsActive() override;
    };

    // The leader at the pull spot with every expected member ready: alive and there, out of a fight, near, rested
    // (runrules::Readiness).
    class RunPullReadyTrigger : public RunTrigger
    {
    public:
        RunPullReadyTrigger(PlayerbotAI* ai) : RunTrigger(ai, "run pull ready") {}
        bool IsActive() override;
    };

    // While the waiting budget runs, the members keeping the pull back are not the ones the leader last said.
    class RunKeepersTrigger : public RunTrigger
    {
    public:
        RunKeepersTrigger(PlayerbotAI* ai) : RunTrigger(ai, "run keepers") {}
        bool IsActive() override;
    };

    // The waiting budget is out with members still keeping the pull back, or a member is out of drink or food it needs.
    class RunBlockTrigger : public RunTrigger
    {
    public:
        RunBlockTrigger(PlayerbotAI* ai) : RunTrigger(ai, "run block") {}
        bool IsActive() override;
    };

    // The movement guard refused the leader on its way to the stop's spot, and no waiting budget runs for this stop and
    // generation yet: it starts now, so that a leader kept from its spot still comes to a block.
    class RunRefusedTrigger : public RunTrigger
    {
    public:
        RunRefusedTrigger(PlayerbotAI* ai) : RunTrigger(ai, "run refused") {}
        bool IsActive() override;
    };

    // A pull that started no fight in a while.
    class RunPullFailedTrigger : public RunTrigger
    {
    public:
        RunPullFailedTrigger(PlayerbotAI* ai) : RunTrigger(ai, "run pull failed") {}
        bool IsActive() override;
    };

    // No creature of the stop's pack alive.
    class RunClearedTrigger : public RunTrigger
    {
    public:
        RunClearedTrigger(PlayerbotAI* ai) : RunTrigger(ai, "run cleared") {}
        bool IsActive() override;
    };

    // The leader drawn off its pull spot while the pull comes to it.
    class RunHoldTrigger : public RunTrigger
    {
    public:
        RunHoldTrigger(PlayerbotAI* ai) : RunTrigger(ai, "run hold") {}
        bool IsActive() override;
    };

    // A member away from its place, out of a fight.
    class RunStandTrigger : public RunTrigger
    {
    public:
        RunStandTrigger(PlayerbotAI* ai) : RunTrigger(ai, "run stand") {}
        bool IsActive() override;
    };

    // A member too far from its place in a fight from a pull.
    class RunLeashTrigger : public RunTrigger
    {
    public:
        RunLeashTrigger(PlayerbotAI* ai) : RunTrigger(ai, "run leash") {}
        bool IsActive() override;
    };

    // On its way, or gathered, the leader finds another pack's creature that walks about reaching the way to the stop's
    // pull spot, the spot or its stands, by the guard's model (GuildRunStates::Reached).
    class RunReachedTrigger : public RunTrigger
    {
    public:
        RunReachedTrigger(PlayerbotAI* ai) : RunTrigger(ai, "run reached") {}
        bool IsActive() override;
    };

    // Waiting at the stop before, the leader finds the way to the pull spot, the spot and its stands out of every blocker's
    // reach by the same model, so the guard would let the walk there.
    class RunClearTrigger : public RunTrigger
    {
    public:
        RunClearTrigger(PlayerbotAI* ai) : RunTrigger(ai, "run clear") {}
        bool IsActive() override;
    };
}
