#include "playerbot/playerbot.h"
#include "GuildRunTriggers.h"
#include "playerbot/GuildBotMgr.h"
#include "playerbot/GuildMoveGuard.h"
#include "playerbot/GuildRunState.h"
#include "playerbot/GuildTactics.h"
#include "playerbot/strategy/generic/PullStrategy.h"
#include "Groups/Group.h"
#include "MotionGenerators/MovementGenerator.h"

using namespace ai;

namespace
{
    // How near a bot counts as at its place: the leader gathers this near its spot, and pulls from twice as near, as
    // readiness asks of it (runrules::LeaderNear).
    constexpr float RUN_AT_SPOT = runrules::AT_SPOT;
    // How far behind its leader a member may fall on the way before it follows.
    constexpr float RUN_FOLLOW = 8.0f;
    // How long after the party goes on to a stop it waits for the last fight's loot before it walks.
    // Loot a member cannot take stays pending, as it did in the foundry, and an unbounded wait held parties for good.
    constexpr time_t RUN_LOOT_WAIT = 20;
    // How long a pull may go without a fight before it counts as having brought nothing.
    constexpr time_t RUN_PULL_WAIT = 20;
    // How long after the pull the leader holds at its spot, and how far it may be drawn off it meanwhile.
    constexpr time_t RUN_HOLD_SECONDS = 12;
    constexpr float RUN_HOLD_NEAR = 8.0f;
    // How far a member may go from its place in a fight: melee from the pull spot, casters from their stand point.
    constexpr float RUN_LEASH_MELEE = 25.0f;
    constexpr float RUN_LEASH_RANGED = 12.0f;

    bool InWorld(Player* bot)
    {
        return bot->IsInWorld() && !bot->IsBeingTeleported();
    }

    float DistanceTo(Player* bot, const WorldPosition& at)
    {
        return bot->GetDistance(at.getX(), at.getY(), at.getZ());
    }

    // The run strategy the run state calls for, or empty. In hold, a living member the engine plays inside holds; in any
    // mode, a member apart from its party holds or walks back to it.
    std::string Wanted(const GuildRunView& view)
    {
        if (view.valid && (view.apart || (view.mode == GuildRunMode::Hold && view.holds)))
            return "run holding";
        if (!view.valid || !view.controls)
            return "";
        return view.lead ? "run lead" : "run party";
    }

    // The leader leads: the run is the controller's, in the mode lead, and the leader is not to choose its stop again.
    bool Leads(const GuildRunView& view)
    {
        return view.valid && view.controls && view.lead && !view.choose;
    }

    // Whether the bot loots before it walks: only in the first RUN_LOOT_WAIT seconds at a stop, and only with loot there.
    bool LootFirst(PlayerbotAI* ai, Player* bot, const GuildRunView& view)
    {
        if (time(nullptr) - view.stopAt >= RUN_LOOT_WAIT)
            return false;
        AiObjectContext* context = ai->GetAiObjectContext();
        return bot->GetLootGuid() || AI_VALUE(bool, "has available loot");
    }

    // Whether the movement guard refused this bot's run move lately, by a creature that moves, under this very plan and
    // stop: it stands, rather than ask again at once, and tries again once the refusal lapses or the plan changes.
    bool Holding(Player* bot, const GuildRunView& view)
    {
        return runguard::Holding(sGuildMoveGuard.RefusalOf(bot->GetGUIDLow()),
            runguard::PlanKey(view.stamp.generation, view.stamp.plan), view.index, int64_t(time(nullptr)));
    }

    // The run strategy the bot has, or empty.
    std::string Current(PlayerbotAI* ai)
    {
        if (ai->HasStrategy("run lead", BotState::BOT_STATE_NON_COMBAT))
            return "run lead";
        if (ai->HasStrategy("run party", BotState::BOT_STATE_NON_COMBAT))
            return "run party";
        if (ai->HasStrategy("run holding", BotState::BOT_STATE_NON_COMBAT))
            return "run holding";
        return "";
    }
}

Event RunTrigger::Check()
{
    if (!IsActive())
        return Event();
    return Event(getName(), runrules::StampText(AI_VALUE(GuildRunView, "run state").stamp));
}

bool RunChooseTrigger::IsActive()
{
    GuildRunView view = AI_VALUE(GuildRunView, "run state");
    return view.valid && view.controls && view.lead && view.choose && InWorld(bot);
}

bool RunHoldPlaceTrigger::IsActive()
{
    GuildRunView view = AI_VALUE(GuildRunView, "run state");
    if (!view.valid || view.mode != GuildRunMode::Hold || !view.holds || !InWorld(bot) || !bot->IsAlive())
        return false;
    return view.holdAt.getMapId() == bot->GetMapId() && DistanceTo(bot, view.holdAt) > RUN_AT_SPOT;
}

bool RunWalkTrigger::IsActive()
{
    GuildRunView view = AI_VALUE(GuildRunView, "run state");
    if (!view.valid || !view.walking || view.walkPoints.empty() || !InWorld(bot) || !bot->IsAlive())
        return false;
    // A part under way: its run move is on top.
    MotionMaster* motion = bot->GetMotionMaster();
    if (!motion->empty() && runrules::IsRunToken(motion->GetCurrent()->GetMoveId())
        && motion->GetCurrentMovementGeneratorType() == PATH_MOTION_TYPE)
        return false;
    return view.walkPoints.front().getMapId() == bot->GetMapId() && DistanceTo(bot, view.walkPoints.front()) <= runrules::CHUNK_START;
}

bool GuildRunEnterTrigger::IsActive()
{
    if (!sGuildBotMgr.IsGuildBot(bot) || !InWorld(bot) || bot->IsInCombat())
        return false;
    const std::string wanted = Wanted(AI_VALUE(GuildRunView, "run state"));
    return !wanted.empty() && wanted != Current(ai);
}

bool GuildRunLeaveTrigger::IsActive()
{
    if (!sGuildBotMgr.IsGuildBot(bot) || !InWorld(bot) || bot->IsInCombat())
        return false;
    return Wanted(AI_VALUE(GuildRunView, "run state")).empty() && !Current(ai).empty();
}

bool RunApproachTrigger::IsActive()
{
    GuildRunView view = AI_VALUE(GuildRunView, "run state");
    if (!Leads(view) || view.index >= view.stops || !InWorld(bot) || view.pull == GuildRunPull::Pull)
        return false;
    // The last fight's loot first, for a while: looting ranks far below the walk, and would never come otherwise.
    if (LootFirst(ai, bot, view))
        return false;
    // Gathered, the leader goes back to its spot only when a fight drew it off, as a pull that left part of the pack alive
    // or a creature that came to the party: run pull ready waits for it within RUN_AT_SPOT * 2, and nothing else moves it.
    // In the foundry the leaders stood 11 to 23 yards off for the rest of the run.
    if (view.pull == GuildRunPull::Gather)
        return DistanceTo(bot, GuildRunStates::FightSpot(view.stop, view.pull, view.ownSpot)) > RUN_AT_SPOT * 2;
    // Not on toward a pull spot while another pack's creature reaches the way there, the spot or its stands, by the
    // guard's model of it (GuildRunStates::Reached): "run reached" has the party wait at the stop before.
    if (view.pull == GuildRunPull::Approach && GuildRunStates::Reached(bot, view))
        return false;
    if (DistanceTo(bot, GuildRunStates::FightSpot(view.stop, view.pull, view.ownSpot)) <= RUN_AT_SPOT)
        return false;
    // Refused lately by a creature that moves: it stands until the refusal lapses.
    return !Holding(bot, view);
}

bool RunGatherTrigger::IsActive()
{
    GuildRunView view = AI_VALUE(GuildRunView, "run state");
    if (!Leads(view) || view.index >= view.stops || view.pull != GuildRunPull::Approach || !InWorld(bot))
        return false;
    return DistanceTo(bot, GuildRunStates::FightSpot(view.stop, view.pull, view.ownSpot)) <= RUN_AT_SPOT
        && !GuildRunStates::Reached(bot, view);
}

bool RunPullReadyTrigger::IsActive()
{
    GuildRunView view = AI_VALUE(GuildRunView, "run state");
    // Only in the mode lead (Leads): never while the run holds, is released or ended.
    if (!Leads(view) || view.index >= view.stops || view.pull != GuildRunPull::Gather
        || !InWorld(bot) || bot->IsInCombat())
        return false;
    // The one gate every pull goes through: the run's permit, nobody apart, every expected member there, and nobody,
    // the leader included, keeping the pull back (GuildRunStates::PullAllowed, runrules::Readiness). No timer pulls: a
    // party that cannot get ready blocks ("run block").
    if (!sGuildRunStates.PullAllowed(bot))
        return false;
    const WorldPosition& spot = GuildRunStates::FightSpot(view.stop, view.pull, view.ownSpot);
    // Not with another pack's creature in reach of the pull spot or its stands, as a patrol that came by while the party
    // gathered: the party waits for it, and a wait that never clears blocks. No timer pulls.
    return DistanceTo(bot, spot) <= RUN_AT_SPOT * 2 && !GuildRunStates::Reached(bot, view);
}

bool RunKeepersTrigger::IsActive()
{
    GuildRunView view = AI_VALUE(GuildRunView, "run state");
    // Once the waiting budget of this stop and generation runs: who keeps the pull back, when it changes.
    if (!Leads(view) || view.index >= view.stops || !InWorld(bot) || view.pull == GuildRunPull::Blocked
        || view.budget.stop != view.index || view.budget.generation != view.stamp.generation || !view.budget.since)
        return false;
    // Also a guarded member whose stop's clock is not yet keyed to this stop and generation, though the keepers read the same.
    const std::vector<runrules::Kept> kept = GuildRunStates::Readiness(bot, view);
    return runplayer::KeepersDue(GuildRunStates::KeepersText(kept), view.keepers, kept, view.guardedClock, view.index,
        view.stamp.generation);
}

bool RunBlockTrigger::IsActive()
{
    GuildRunView view = AI_VALUE(GuildRunView, "run state");
    // Watched in and out of a fight, apart from the pull's own checks, which return early for a fight or a member away.
    // The budget out, or a member out of what waiting cannot mend, that one only with the leader gathered at its spot;
    // never with nobody keeping the pull back. Only the bots' reasons may block, and the budget is frozen while only a
    // person, or only guarded members while their allowance lasts, keep the pull back (GuildRunStates::BlockNow).
    if (!Leads(view) || !InWorld(bot))
        return false;
    std::vector<runrules::Kept> bots;
    return GuildRunStates::BlockNow(bot, view, bots);
}

bool RunRefusedTrigger::IsActive()
{
    GuildRunView view = AI_VALUE(GuildRunView, "run state");
    if (!Leads(view) || view.index >= view.stops || view.pull != GuildRunPull::Approach || !InWorld(bot))
        return false;
    // A budget of this stop and generation runs already: a later refusal changes nothing.
    if (view.budget.stop == view.index && view.budget.generation == view.stamp.generation && view.budget.since)
        return false;
    return runguard::Fresh(sGuildMoveGuard.RefusalOf(bot->GetGUIDLow()), view.stamp.generation, view.index,
        int64_t(time(nullptr)));
}

bool RunPullFailedTrigger::IsActive()
{
    GuildRunView view = AI_VALUE(GuildRunView, "run state");
    if (!Leads(view) || view.pull != GuildRunPull::Pull || bot->IsInCombat())
        return false;
    return time(nullptr) - view.pullAt >= RUN_PULL_WAIT;
}

bool RunClearedTrigger::IsActive()
{
    GuildRunView view = AI_VALUE(GuildRunView, "run state");
    if (!Leads(view) || view.index >= view.stops || !InWorld(bot))
        return false;
    return GuildRunStates::FirstLiving(view.packs.get(), bot->GetMap(), view.index) != view.index;
}

bool RunHoldTrigger::IsActive()
{
    GuildRunView view = AI_VALUE(GuildRunView, "run state");
    if (!Leads(view) || view.pull != GuildRunPull::Pull || !InWorld(bot)
        || time(nullptr) - view.pullAt >= RUN_HOLD_SECONDS)
        return false;
    // Only a pull from range comes to the spot; a tank that had to walk in fights where the pack stands.
    PullStrategy* strategy = PullStrategy::Get(ai);
    Unit* pulled = ai->GetUnit(view.pulled);
    if (!strategy || !pulled || !strategy->CanDoPullAction(pulled))
        return false;
    return DistanceTo(bot, GuildRunStates::FightSpot(view.stop, view.pull, view.ownSpot)) > RUN_HOLD_NEAR;
}

bool RunStandTrigger::IsActive()
{
    GuildRunView view = AI_VALUE(GuildRunView, "run state");
    if (!view.valid || !view.controls || view.lead || !InWorld(bot))
        return false;
    WorldPosition spot = AI_VALUE(WorldPosition, "run spot");
    if (!spot || spot.getMapId() != bot->GetMapId())
        return false;
    if (LootFirst(ai, bot, view))
        return false;
    if (DistanceTo(bot, spot) <= (view.pull == GuildRunPull::Approach ? RUN_FOLLOW : RUN_AT_SPOT))
        return false;
    // Refused lately by a creature that moves: it stands until the refusal lapses, and readiness says guarded meanwhile.
    return !Holding(bot, view);
}

bool RunLeashTrigger::IsActive()
{
    GuildRunView view = AI_VALUE(GuildRunView, "run state");
    if (!view.valid || !view.controls || view.lead || view.pull != GuildRunPull::Pull || !InWorld(bot))
        return false;
    if (ai->IsRanged(bot))
    {
        WorldPosition spot = AI_VALUE(WorldPosition, "run spot");
        return spot && spot.getMapId() == bot->GetMapId() && DistanceTo(bot, spot) > RUN_LEASH_RANGED;
    }
    return DistanceTo(bot, GuildRunStates::FightSpot(view.stop, view.pull, view.ownSpot)) > RUN_LEASH_MELEE;
}

bool RunReachedTrigger::IsActive()
{
    GuildRunView view = AI_VALUE(GuildRunView, "run state");
    // On the way, or gathered at the spot when a patrol comes by: the party waits at the stop before.
    if (!Leads(view) || view.index >= view.stops
        || (view.pull != GuildRunPull::Approach && view.pull != GuildRunPull::Gather)
        || !view.stop.hasBefore || view.stop.outsideReach.empty() || !InWorld(bot))
        return false;
    return GuildRunStates::Reached(bot, view);
}

bool RunClearTrigger::IsActive()
{
    GuildRunView view = AI_VALUE(GuildRunView, "run state");
    if (!Leads(view) || view.index >= view.stops || view.pull != GuildRunPull::Wait || !InWorld(bot))
        return false;
    // Go only when the guard's model has no blocker on the whole way to the spot, the spot or its stands
    // (runguard::WaitReaches), so the guard lets the walk there. The block, when it comes, is "run block"'s.
    return runrules::WaitNext(!GuildRunStates::Reached(bot, view), false) == runrules::WaitStep::Go;
}

bool RunTacticsTrigger::IsActive()
{
    GuildRunView view = AI_VALUE(GuildRunView, "run state");
    if (!Leads(view) || !InWorld(bot) || !bot->IsAlive())
        return false;
    return sGuildTactics.Look(bot, view, why);
}

Event RunTacticsTrigger::Check()
{
    if (!IsActive())
        return Event();
    return Event(getName(), runrules::StampText(AI_VALUE(GuildRunView, "run state").stamp) + " " + why);
}

bool RunInterruptTrigger::IsActive()
{
    // A cheap look first: most members are given no interrupt in most fights.
    if (!InWorld(bot) || !bot->IsAlive() || !sGuildTactics.Assigned(bot))
        return false;
    // Then the run as it is now, not as cached: an interrupt is late in a second.
    const GuildRunView view = sGuildRunStates.View(bot);
    if (!view.valid || !view.controls || view.mode != GuildRunMode::Lead)
        return false;
    Unit* foe = nullptr;
    uint32 spell = 0;
    if (!sGuildTactics.InterruptDue(bot, view, foe, spell))
        return false;
    stamp = view.stamp;
    return true;
}

Event RunInterruptTrigger::Check()
{
    if (!IsActive())
        return Event();
    return Event(getName(), runrules::StampText(stamp));
}
