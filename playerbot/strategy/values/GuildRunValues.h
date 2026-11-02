#pragma once
#include "playerbot/strategy/Value.h"
#include "playerbot/GuildRunState.h"

namespace ai
{
    // The run state as this bot sees it, copied from GuildRunStates, so triggers and actions never touch its lock.
    // Not valid outside a guild run's instance.
    class RunStateValue : public CalculatedValue<GuildRunView>
    {
    public:
        RunStateValue(PlayerbotAI* ai) : CalculatedValue<GuildRunView>(ai, "run state", 1) {}
        GuildRunView Calculate() override;
    };

    // Where this bot belongs at the stop its party is at. The leader's place is the pull spot. A member's is its stand
    // point there, by its place in the group; while the leader walks to the spot, it is the leader, and the member
    // follows. Empty outside a run, or once the route is done.
    class RunSpotValue : public CalculatedValue<WorldPosition>
    {
    public:
        RunSpotValue(PlayerbotAI* ai) : CalculatedValue<WorldPosition>(ai, "run spot", 1) {}
        WorldPosition Calculate() override;
    };

    // The leader's last step onto its pull spot (runrules::PullStep): taken by "run pull", its end marked by whichever
    // looks first, "run pull" or "run stepped", and its outcome said once, in a fight or after the run moved on too.
    class RunPullStepValue : public ManualSetValue<runrules::PullStep>
    {
    public:
        RunPullStepValue(PlayerbotAI* ai) : ManualSetValue<runrules::PullStep>(ai, runrules::PullStep(), "run pull step") {}
    };

    // The run's gather at the leader's stop as the view has it (runrules::PullGather): the generation, the stop, when the
    // leader gathered and the pulls that brought nothing.
    runrules::PullGather GatherOf(const GuildRunView& view);

    // The leader's step as it is now: whether its own move is under way in its gather (runrules::StepUnderway), and, the
    // first time it is seen over, its end marked on it with where the leader then stands, whether it still moves and by
    // whose move, and whether the creature is in sight. Only for a bot in the world and not between maps.
    bool StepNow(Player* bot, const GuildRunView& view, runrules::PullStep& step);
}
