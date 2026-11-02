#include "playerbot/playerbot.h"
#include "GuildRunValues.h"
#include "playerbot/ServerFacade.h"
#include "Entities/Creature.h"
#include "Groups/Group.h"

using namespace ai;

GuildRunView RunStateValue::Calculate()
{
    return sGuildRunStates.View(bot);
}

WorldPosition RunSpotValue::Calculate()
{
    // The same place a run move takes the bot to (GuildRunStates::PlaceOf), for the triggers to decide by.
    return GuildRunStates::PlaceOf(AI_VALUE(GuildRunView, "run state"), bot);
}

runrules::PullGather ai::GatherOf(const GuildRunView& view)
{
    return { view.stamp.generation, view.index, int64_t(view.gatherSince), view.pullTries };
}

bool ai::StepNow(Player* bot, const GuildRunView& view, runrules::PullStep& step)
{
    const runrules::PullGather now = GatherOf(view);
    const bool gathering = view.valid && view.lead && view.pull == GuildRunPull::Gather;
    const bool moving = sServerFacade.isMoving(bot);
    GuildRunStamp moveStamp;
    const bool own = step.token && sGuildRunStates.RunMoveOf(bot->GetGUIDLow(), moveStamp) == step.token;
    const uint32 elapsed = WorldTimer::getMSTimeDiff(step.startMs, WorldTimer::getMSTime());
    const bool stepping = runrules::Stepped(step, now) && runrules::StepUnderway(moving, own, elapsed, step.from);
    if (!runrules::MarkStepEnd(step, runrules::StepEndOf(step, now, gathering, bot->IsInCombat(), stepping), elapsed))
        return stepping;
    step.moving = moving;
    step.own = own;
    const bool sameMap = bot->GetMapId() == step.map;
    step.off = sameMap ? bot->GetDistance(step.x, step.y, step.z) : -1.0f;
    Creature* target = sameMap && step.target ? bot->GetMap()->GetCreature(ObjectGuid(step.target)) : nullptr;
    if (target && target->IsInWorld() && target->IsAlive())
    {
        step.sight = bot->IsWithinLOSInMap(target, true) ? 1 : 0;
        step.sightM2 = bot->IsWithinLOSInMap(target) ? 1 : 0;
    }
    return stepping;
}
