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
}
