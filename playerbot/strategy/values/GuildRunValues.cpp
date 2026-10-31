#include "playerbot/playerbot.h"
#include "GuildRunValues.h"
#include "Groups/Group.h"

using namespace ai;

GuildRunView RunStateValue::Calculate()
{
    return sGuildRunStates.View(bot);
}

WorldPosition RunSpotValue::Calculate()
{
    GuildRunView view = AI_VALUE(GuildRunView, "run state");
    if (!view.valid || view.index >= view.stops || !view.stop.pull)
        return WorldPosition();

    // Where the party fights this stop: its pull spot, or the stop before's while it waits there or pulls from afar.
    const WorldPosition& spot = GuildRunStates::FightSpot(view.stop, view.pull, view.ownSpot);
    if (view.lead)
        return spot;

    // On the way, the members keep with the leader rather than walking to the spot by a way of their own.
    if (view.pull == GuildRunPull::Approach)
    {
        Player* leader = sObjectMgr.GetPlayer(ObjectGuid(HIGHGUID_PLAYER, view.leader));
        if (leader && leader->IsInWorld() && !leader->IsBeingTeleported() && leader->GetMapId() == bot->GetMapId())
            return WorldPosition(leader);
        return spot;
    }

    // At the spot, each member stands by its role: melee at the two nearest points, a caster at the third, a healer at
    // the fourth, farthest back. Two of a role take that role's points in the group's order.
    const std::vector<WorldPosition>& stands = GuildRunStates::FightStands(view.stop, view.pull, view.ownSpot);
    Group* group = bot->GetGroup();
    if (!group || stands.empty())
        return spot;
    auto role = [this](Player* player) { return PlayerbotAI::IsHeal(player) ? 2 : ai->IsRanged(player) ? 1 : 0; };
    static const std::vector<size_t> places[] = { { 0, 1 }, { 2 }, { 3 } };
    const int mine = role(bot);
    size_t rank = 0;
    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->getSource();
        if (!member || member->GetGUIDLow() == view.leader)
            continue;
        if (member == bot)
            break;
        if (role(member) == mine)
            ++rank;
    }
    const std::vector<size_t>& mineAt = places[mine];
    const size_t index = std::min(mineAt[rank % mineAt.size()], stands.size() - 1);
    return stands[index];
}
