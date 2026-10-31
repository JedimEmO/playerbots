#include "playerbot/playerbot.h"
#include "GuildRunTriggers.h"
#include "playerbot/GuildBotMgr.h"
#include "playerbot/GuildRunState.h"
#include "playerbot/strategy/generic/PullStrategy.h"
#include "Groups/Group.h"

using namespace ai;

namespace
{
    // How near a bot counts as at its place.
    constexpr float RUN_AT_SPOT = 3.0f;
    // How far behind its leader a member may fall on the way before it follows.
    constexpr float RUN_FOLLOW = 8.0f;
    // How long after the party goes on to a stop it waits for the last fight's loot before it walks.
    // Loot a member cannot take stays pending, as it did in the foundry, and an unbounded wait held parties for good.
    constexpr time_t RUN_LOOT_WAIT = 20;
    // How long the leader waits at the pull spot for members far off or low, before it pulls anyway. A dead member
    // is waited for without a limit: its corpse run is the job's.
    constexpr time_t RUN_GATHER_MAX = 90;
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

    // The run strategy the run state calls for, or empty.
    std::string Wanted(const GuildRunView& view)
    {
        if (!view.valid || !view.controls)
            return "";
        return view.lead ? "run lead" : "run party";
    }

    // Whether the bot loots before it walks: only in the first RUN_LOOT_WAIT seconds at a stop, and only with loot there.
    bool LootFirst(PlayerbotAI* ai, Player* bot, const GuildRunView& view)
    {
        if (time(nullptr) - view.stopAt >= RUN_LOOT_WAIT)
            return false;
        AiObjectContext* context = ai->GetAiObjectContext();
        return bot->GetLootGuid() || AI_VALUE(bool, "has available loot");
    }

    // The run strategy the bot has, or empty.
    std::string Current(PlayerbotAI* ai)
    {
        if (ai->HasStrategy("run lead", BotState::BOT_STATE_NON_COMBAT))
            return "run lead";
        if (ai->HasStrategy("run party", BotState::BOT_STATE_NON_COMBAT))
            return "run party";
        return "";
    }
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
    if (!view.valid || !view.controls || !view.lead || view.index >= view.stops || !InWorld(bot) || view.pull == GuildRunPull::Pull)
        return false;
    // The last fight's loot first, for a while: looting ranks far below the walk, and would never come otherwise.
    if (LootFirst(ai, bot, view))
        return false;
    // Gathered, the leader goes back to its spot only when a fight drew it off, as a pull that left part of the pack alive
    // or a creature that came to the party: run pull ready waits for it within RUN_AT_SPOT * 2, and nothing else moves it.
    // In the foundry the leaders stood 11 to 23 yards off for the rest of the run.
    if (view.pull == GuildRunPull::Gather)
        return DistanceTo(bot, GuildRunStates::FightSpot(view.stop, view.pull, view.ownSpot)) > RUN_AT_SPOT * 2;
    // Not to a pull spot another pack's creature reaches: "run reached" has the party wait at the stop before,
    // unless it has waited as long as it may.
    if (view.pull == GuildRunPull::Approach && !view.waited && GuildRunStates::Reached(bot->GetMap(), bot, view.stop))
        return false;
    return DistanceTo(bot, GuildRunStates::FightSpot(view.stop, view.pull, view.ownSpot)) > RUN_AT_SPOT;
}

bool RunGatherTrigger::IsActive()
{
    GuildRunView view = AI_VALUE(GuildRunView, "run state");
    if (!view.valid || !view.controls || !view.lead || view.index >= view.stops || view.pull != GuildRunPull::Approach || !InWorld(bot))
        return false;
    return DistanceTo(bot, GuildRunStates::FightSpot(view.stop, view.pull, view.ownSpot)) <= RUN_AT_SPOT
        && (view.waited || !GuildRunStates::Reached(bot->GetMap(), bot, view.stop));
}

bool RunPullReadyTrigger::IsActive()
{
    GuildRunView view = AI_VALUE(GuildRunView, "run state");
    if (!view.valid || !view.controls || !view.lead || view.index >= view.stops || view.pull != GuildRunPull::Gather
        || !InWorld(bot) || bot->IsInCombat())
        return false;
    const WorldPosition& spot = GuildRunStates::FightSpot(view.stop, view.pull, view.ownSpot);
    // Not with another pack's creature in reach of the pull spot, as a patrol that came by while the party gathered,
    // unless the party has waited for it as long as it may.
    if (DistanceTo(bot, spot) > RUN_AT_SPOT * 2 || (!view.waited && GuildRunStates::Reached(bot->GetMap(), bot, view.stop)))
        return false;

    Group* group = bot->GetGroup();
    if (!group)
        return false;
    bool ready = true;
    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->getSource();
        if (!member || member == bot)
            continue;
        // A member dead, or on another map after its release, holds the pull until it is back.
        if (!member->IsAlive() || !member->IsInWorld() || member->GetMapId() != bot->GetMapId())
            return false;
        if (member->IsInCombat())
            return false;
        const bool hasMana = member->GetPowerType() == POWER_MANA && member->GetMaxPower(POWER_MANA);
        if (DistanceTo(member, spot) > GuildRunStates::PARTY_NEAR || member->GetHealthPercent() < GuildRunStates::PULL_HEALTH
            || (hasMana && member->GetPowerPercent(POWER_MANA) < GuildRunStates::PULL_MANA))
            ready = false;
    }
    return ready || time(nullptr) - view.gatherSince >= RUN_GATHER_MAX;
}

bool RunPullFailedTrigger::IsActive()
{
    GuildRunView view = AI_VALUE(GuildRunView, "run state");
    if (!view.valid || !view.controls || !view.lead || view.pull != GuildRunPull::Pull || bot->IsInCombat())
        return false;
    return time(nullptr) - view.pullAt >= RUN_PULL_WAIT;
}

bool RunClearedTrigger::IsActive()
{
    GuildRunView view = AI_VALUE(GuildRunView, "run state");
    if (!view.valid || !view.lead || view.index >= view.stops || !InWorld(bot))
        return false;
    return GuildRunStates::FirstLiving(view.packs.get(), bot->GetMap(), view.index) != view.index;
}

bool RunHoldTrigger::IsActive()
{
    GuildRunView view = AI_VALUE(GuildRunView, "run state");
    if (!view.valid || !view.controls || !view.lead || view.pull != GuildRunPull::Pull || !InWorld(bot)
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
    return DistanceTo(bot, spot) > (view.pull == GuildRunPull::Approach ? RUN_FOLLOW : RUN_AT_SPOT);
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
    // On the way, or gathered at the spot when a patrol or wanderer comes by: the wait, and its cap, apply to both.
    if (!view.valid || !view.controls || !view.lead || view.index >= view.stops
        || (view.pull != GuildRunPull::Approach && view.pull != GuildRunPull::Gather)
        || view.waited || !view.stop.hasBefore || view.stop.outsideReach.empty() || !InWorld(bot))
        return false;
    return GuildRunStates::Reached(bot->GetMap(), bot, view.stop);
}

bool RunClearTrigger::IsActive()
{
    GuildRunView view = AI_VALUE(GuildRunView, "run state");
    if (!view.valid || !view.controls || !view.lead || view.pull != GuildRunPull::Wait || !InWorld(bot))
        return false;
    return !GuildRunStates::Reached(bot->GetMap(), bot, view.stop);
}

bool RunWaitOverTrigger::IsActive()
{
    GuildRunView view = AI_VALUE(GuildRunView, "run state");
    if (!view.valid || !view.controls || !view.lead || view.pull != GuildRunPull::Wait)
        return false;
    return time(nullptr) - view.waitSince >= GuildRunStates::WAIT_SECONDS;
}
