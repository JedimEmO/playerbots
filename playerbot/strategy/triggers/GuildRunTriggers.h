#pragma once
#include "playerbot/strategy/Trigger.h"

namespace ai
{
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
    class RunApproachTrigger : public Trigger
    {
    public:
        RunApproachTrigger(PlayerbotAI* ai) : Trigger(ai, "run approach") {}
        bool IsActive() override;
    };

    // The leader has come to the pull spot.
    class RunGatherTrigger : public Trigger
    {
    public:
        RunGatherTrigger(PlayerbotAI* ai) : Trigger(ai, "run gather") {}
        bool IsActive() override;
    };

    // The leader at the pull spot with the party in place, alive, rested and out of a fight, or waited long enough.
    class RunPullReadyTrigger : public Trigger
    {
    public:
        RunPullReadyTrigger(PlayerbotAI* ai) : Trigger(ai, "run pull ready") {}
        bool IsActive() override;
    };

    // A pull that started no fight in a while.
    class RunPullFailedTrigger : public Trigger
    {
    public:
        RunPullFailedTrigger(PlayerbotAI* ai) : Trigger(ai, "run pull failed") {}
        bool IsActive() override;
    };

    // No creature of the stop's pack alive.
    class RunClearedTrigger : public Trigger
    {
    public:
        RunClearedTrigger(PlayerbotAI* ai) : Trigger(ai, "run cleared") {}
        bool IsActive() override;
    };

    // The leader drawn off its pull spot while the pull comes to it.
    class RunHoldTrigger : public Trigger
    {
    public:
        RunHoldTrigger(PlayerbotAI* ai) : Trigger(ai, "run hold") {}
        bool IsActive() override;
    };

    // A member away from its place, out of a fight.
    class RunStandTrigger : public Trigger
    {
    public:
        RunStandTrigger(PlayerbotAI* ai) : Trigger(ai, "run stand") {}
        bool IsActive() override;
    };

    // A member too far from its place in a fight from a pull.
    class RunLeashTrigger : public Trigger
    {
    public:
        RunLeashTrigger(PlayerbotAI* ai) : Trigger(ai, "run leash") {}
        bool IsActive() override;
    };

    // On its way, or gathered, the leader finds another pack's wanderer or patrol in reach of the stop's pull spot.
    class RunReachedTrigger : public Trigger
    {
    public:
        RunReachedTrigger(PlayerbotAI* ai) : Trigger(ai, "run reached") {}
        bool IsActive() override;
    };

    // Waiting at the stop before, the leader finds the pull spot out of every other pack's reach again.
    class RunClearTrigger : public Trigger
    {
    public:
        RunClearTrigger(PlayerbotAI* ai) : Trigger(ai, "run clear") {}
        bool IsActive() override;
    };

    // The leader has waited at the stop before as long as it may, and pulls with the creature still in reach.
    class RunWaitOverTrigger : public Trigger
    {
    public:
        RunWaitOverTrigger(PlayerbotAI* ai) : Trigger(ai, "run wait over") {}
        bool IsActive() override;
    };
}
