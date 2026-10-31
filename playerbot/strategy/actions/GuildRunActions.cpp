#include "playerbot/playerbot.h"
#include "GuildRunActions.h"
#include "playerbot/GuildRunState.h"
#include "playerbot/GuildRuns.h"
#include "playerbot/strategy/generic/PullStrategy.h"
#include "Entities/Creature.h"

using namespace ai;

namespace
{
    // Pulls of one stop that may bring nothing before the leader goes on past it.
    constexpr uint32 RUN_PULL_TRIES = 3;

    // After the leader writes the run state, its own copy is read afresh.
    void Refresh(AiObjectContext* context)
    {
        context->GetValue<GuildRunView>("run state")->Reset();
        context->GetValue<WorldPosition>("run spot")->Reset();
    }

    // The stamp of the run trigger that fired the action (RunTrigger).
    GuildRunStamp StampOf(Event& event)
    {
        return runrules::ParseStamp(event.getParam());
    }

    // In the world and not between maps: every run action looks at nothing else before this.
    bool Settled(Player* bot)
    {
        return bot->IsInWorld() && !bot->IsBeingTeleported();
    }
}

bool GuildRunStrategiesAction::Execute(Event& event)
{
    ai->ResetStrategies(false);
    return true;
}

bool MoveToRunSpotAction::isUseful()
{
    if (!Settled(bot))
        return false;
    WorldPosition spot = AI_VALUE(WorldPosition, "run spot");
    return spot && spot.getMapId() == bot->GetMapId();
}

bool MoveToRunSpotAction::Execute(Event& event)
{
    if (!Settled(bot))
        return false;
    // One fresh view of the run: the stamp is checked against it, and the destination comes from it, never from a value
    // cached apart. Fired under a run state that has since moved on, or ended: nothing.
    const GuildRunView view = sGuildRunStates.View(bot);
    WorldPosition spot;
    if (!view.valid || !runrules::ValidatedPlace(view.mode, view.stamp, StampOf(event), view.lead && view.mode == GuildRunMode::Lead,
        view, [this](const GuildRunView& at) { return GuildRunStates::PlaceOf(at, bot); }, spot))
    {
        sGuildRunStates.Refused(bot);
        return false;
    }
    if (!spot || spot.getMapId() != bot->GetMapId())
        return false;
    // MoveTo builds a path on the navmesh to the spot, which the dungeon model put on the navmesh.
    return MoveTo(spot.getMapId(), spot.getX(), spot.getY(), spot.getZ());
}

bool RunChooseAction::Execute(Event& event)
{
    if (!Settled(bot))
        return false;
    sGuildRunStates.Choose(bot, StampOf(event));
    Refresh(context);
    return true;
}

bool RunGatherAction::Execute(Event& event)
{
    if (!Settled(bot))
        return false;
    sGuildRunStates.Gather(bot, StampOf(event));
    Refresh(context);
    return true;
}

bool RunPullAction::Execute(Event& event)
{
    if (!Settled(bot))
        return false;
    // The run as it is now, not as the trigger saw it: a pull is the one move the run cannot take back.
    const GuildRunStamp stamp = StampOf(event);
    const GuildRunView view = sGuildRunStates.View(bot);
    if (!view.valid || !view.lead || !view.controls || view.index >= view.stops || !sGuildRunStates.Current(bot, view, stamp, true))
        return false;

    // The creature the dungeon model chose the spot to pull, else the pack's living one nearest the spot.
    Map* map = bot->GetMap();
    Creature* target = view.stop.pulled ? map->GetCreature(view.stop.pulled) : nullptr;
    if (target && (!target->IsInWorld() || !target->IsAlive()))
        target = nullptr;
    if (!target)
    {
        float nearest = 0.0f;
        for (uint32 spawn : view.stop.spawns)
        {
            Creature* creature = map->GetCreature(spawn);
            if (!creature || !creature->IsInWorld() || !creature->IsAlive())
                continue;
            const float distance = creature->GetDistance(view.stop.pull.getX(), view.stop.pull.getY(), view.stop.pull.getZ());
            if (!target || distance < nearest)
            {
                target = creature;
                nearest = distance;
            }
        }
    }
    if (!target)
    {
        sGuildRunStates.Advance(bot, stamp, false);
        Refresh(context);
        return true;
    }

    // From where the leader stands, the pull spot's or the stop before's: whether the creature can be pulled from here.
    PullStrategy* strategy = PullStrategy::Get(ai);
    const float range = strategy ? strategy->GetRange() : ai->GetRange("spell");
    const float distance = bot->GetDistance(target);
    const bool inSight = bot->IsWithinLOSInMap(target);
    const bool fromRange = strategy && strategy->CanDoPullAction(target);
    const std::string stop = "stop " + std::to_string(view.index + 1);

    // A creature that stands still, or of the pack's own, reaches the pull spot, so the pack is pulled from the stop
    // before's, where the party stands: only from range and in sight, so the fight comes to the party. Otherwise from
    // its own spot.
    const bool fromBefore = view.stop.stillReach && view.stop.hasBefore && !view.ownSpot;
    if (fromBefore && (!fromRange || distance > range || !inSight))
    {
        sGuildRunStates.UseOwnSpot(bot, stamp, stop + ": its pull spot is reached by " + GuildRunStates::Names(view.stop.reachedBy)
            + ", and " + std::string(target->GetName()) + " is " + std::to_string(uint32(distance)) + " yd from the stop before"
            + (inSight ? "" : ", out of sight") + (fromRange ? "" : ", with no pull from range") + "; pulling from its own spot");
        Refresh(context);
        return true;
    }
    // Out of range and out of sight from the last spot it could pull from: passed over, and the run goes on.
    if (!fromBefore && distance > range && !inSight)
    {
        sGuildRunStates.Skip(bot, stamp, std::string(target->GetName()) + " is " + std::to_string(uint32(distance))
            + " yd off and out of sight of its pull spot");
        Refresh(context);
        return true;
    }

    // The party, checked again right before the pull: a member can die or leave after the trigger fired, with the stamp
    // unchanged.
    if (!GuildRunStates::Present(bot, view))
        return false;
    bool started = ai->DoSpecificAction("pull my target", Event("attack anything", target->GetObjectGuid()), true);
    if (!started && !fromBefore)
        started = Attack(nullptr, target);
    if (!started)
    {
        sGuildRunStates.PullFailed(bot, stamp);
        Refresh(context);
        return false;
    }

    sGuildRunStates.Pulled(bot, stamp, target->GetObjectGuid());
    sGuildRuns.NotePull(bot, target);
    Refresh(context);
    return true;
}

bool RunPullFailedAction::Execute(Event& event)
{
    if (!Settled(bot))
        return false;
    GuildRunView view = AI_VALUE(GuildRunView, "run state");
    if (view.pullTries + 1 >= RUN_PULL_TRIES)
        sGuildRunStates.Advance(bot, StampOf(event), true);
    else
        sGuildRunStates.PullFailed(bot, StampOf(event));
    Refresh(context);
    return true;
}

bool RunNextAction::Execute(Event& event)
{
    if (!Settled(bot))
        return false;
    sGuildRunStates.Advance(bot, StampOf(event), false);
    Refresh(context);
    return true;
}

bool RunWaitAction::Execute(Event& event)
{
    if (!Settled(bot))
        return false;
    GuildRunView view = AI_VALUE(GuildRunView, "run state");
    sGuildRunStates.Wait(bot, StampOf(event), "stop " + std::to_string(view.index + 1) + ", at the stop before until "
        + GuildRunStates::Names(view.stop.outsideReach) + " leaves its pull spot's reach");
    Refresh(context);
    return true;
}

bool RunGoAction::Execute(Event& event)
{
    if (!Settled(bot))
        return false;
    sGuildRunStates.Go(bot, StampOf(event));
    Refresh(context);
    return true;
}

bool RunWaitedAction::Execute(Event& event)
{
    if (!Settled(bot))
        return false;
    GuildRunView view = AI_VALUE(GuildRunView, "run state");
    sGuildRunStates.WaitedOut(bot, StampOf(event), "stop " + std::to_string(view.index + 1) + ", waited "
        + std::to_string(uint32(GuildRunStates::WAIT_SECONDS)) + " s at the stop before; pulling with "
        + GuildRunStates::Names(view.stop.outsideReach) + " still about");
    Refresh(context);
    return true;
}
