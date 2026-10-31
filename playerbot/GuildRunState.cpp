#include "playerbot/playerbot.h"

#include "playerbot/GuildRunState.h"
#include "playerbot/GuildPacks.h"
#include "Entities/Creature.h"
#include "Groups/Group.h"
#include "Maps/Map.h"
#include "MotionGenerators/MoveMap.h"
#include "MotionGenerators/PathFinder.h"

#include <algorithm>
#include <sstream>

namespace
{
    // Whether a point is on the instance's navmesh: a poly within a few yards of it, as the core's path finder looks.
    // On a route leg the travel map has no way for, a point is on a straight line and need not be.
    bool OnNavmesh(Map* map, const ai::WorldPosition& at)
    {
        dtNavMeshQuery const* query = MMAP::MMapFactory::createOrGetMMapManager()->GetNavMeshQuery(map->GetId(), map->GetInstanceId());
        if (!query)
            return false;
        dtQueryFilter filter;
        filter.setIncludeFlags(NAV_GROUND | NAV_WATER);
        filter.setExcludeFlags(NAV_MAGMA_SLIME);
        // Detour's points are (y, z, x).
        const float point[3] = { at.getY(), at.getZ(), at.getX() }, box[3] = { 3.0f, 5.0f, 3.0f };
        dtPolyRef ref = 0;
        float nearest[3];
        return !dtStatusFailed(query->findNearestPoly(point, box, &filter, &ref, nearest)) && ref;
    }
}

std::shared_ptr<GuildRunStates::State> GuildRunStates::Find(uint32 instanceId) const
{
    std::lock_guard<std::mutex> guard(registryLock);
    auto it = byInstance.find(instanceId);
    return it == byInstance.end() ? nullptr : it->second;
}

std::shared_ptr<GuildRunStates::State> GuildRunStates::LeaderState(Player* leader) const
{
    std::shared_ptr<State> state = leader && leader->IsInWorld() ? Find(leader->GetInstanceId()) : nullptr;
    return state && state->leader == leader->GetGUIDLow() ? state : nullptr;
}

void GuildRunStates::End(uint32 leaderGuid)
{
    std::lock_guard<std::mutex> guard(registryLock);
    for (auto it = byInstance.begin(); it != byInstance.end();)
    {
        if (it->second->leader == leaderGuid)
            it = byInstance.erase(it);
        else
            ++it;
    }
}

void GuildRunStates::Reset(State& state, size_t index)
{
    state.index = std::min(index, state.route.size());
    state.pull = GuildRunPull::Approach;
    state.pulled.Clear();
    state.gatherSince = 0;
    state.pullTries = 0;
    state.waitSince = 0;
    state.waited = false;
    state.ownSpot = false;
    state.stopAt = state.changedAt = time(nullptr);
}

size_t GuildRunStates::FirstLiving(const GuildPacks::Route* packs, Map* map, size_t index)
{
    if (!packs || !map || index >= packs->stops.size())
        return packs ? packs->stops.size() : 0;
    const int32 first = GuildPacks::FirstLiveStop(*packs, map, index);
    return first < 0 ? packs->stops.size() : size_t(first);
}

size_t GuildRunStates::NextStop(const GuildPacks::Route* packs, const std::vector<GuildRunStop>& route, Map* map, size_t index)
{
    size_t next = FirstLiving(packs, map, index);
    while (next < route.size() && !route[next].planned)
        next = FirstLiving(packs, map, next + 1);
    return next;
}

bool GuildRunStates::Prepare(Player* leader, const std::string& dungeon, std::vector<std::pair<std::string, std::string>>& events)
{
    Group* group = leader ? leader->GetGroup() : nullptr;
    Map* map = leader && leader->IsInWorld() && !leader->IsBeingTeleported() ? leader->GetMap() : nullptr;
    if (!group || !map || !map->IsDungeon())
    {
        if (leader)
            End(leader->GetGUIDLow());
        return false;
    }

    std::shared_ptr<State> state = Find(map->GetInstanceId());
    if (state && (state->leader != leader->GetGUIDLow() || state->group != group->GetId()))
    {
        // Another run's state in this instance, as after the group changed: this run starts afresh.
        End(state->leader);
        state = nullptr;
    }
    if (!state)
    {
        End(leader->GetGUIDLow());
        state = std::make_shared<State>();
        state->group = group->GetId();
        state->leader = leader->GetGUIDLow();
        state->mapId = map->GetId();
        state->instanceId = map->GetInstanceId();
        std::string error;
        state->route = BuildRoute(leader, dungeon, state->packs, error);
        // A party set down mid-route, as in a lab scenario, starts where it stands, not at a stop behind it.
        size_t start = state->route.size();
        if (state->packs)
        {
            const int32 at = GuildPacks::FirstLiveStop(*state->packs, map,
                G3D::Vector3(leader->GetPositionX(), leader->GetPositionY(), leader->GetPositionZ()));
            start = NextStop(state->packs.get(), state->route, map, at < 0 ? state->route.size() : size_t(at));
        }
        Reset(*state, start);
        {
            std::lock_guard<std::mutex> guard(registryLock);
            byInstance[state->instanceId] = state;
        }
        if (state->route.empty())
            events.push_back({ "run_route", "none: " + error });
        else
        {
            uint32 reached = 0, offMesh = 0;
            for (const GuildRunStop& stop : state->route)
            {
                reached += stop.stillReach || !stop.outsideReach.empty() ? 1 : 0;
                offMesh += stop.planned ? 0 : 1;
            }
            events.push_back({ "run_route", std::to_string(state->route.size()) + " stops at level "
                + std::to_string(state->packs->level) + ", starting at stop " + std::to_string(state->index + 1)
                + (reached ? ", " + std::to_string(reached) + " with a spot others reach" : "")
                + (offMesh ? ", " + std::to_string(offMesh) + " off the navmesh passed over" : "") });
        }
    }

    // The packs are looked at without the lock: the route's planned flags change only from the leader, and are copied.
    size_t index = 0;
    std::vector<GuildRunStop> route;
    {
        std::lock_guard<std::mutex> guard(state->lock);
        index = state->index;
        route = state->route;
    }
    const size_t next = NextStop(state->packs.get(), route, map, index);

    std::lock_guard<std::mutex> guard(state->lock);
    // While the job has the party, for a step of its tables, the stop follows the packs it kills; while the controller
    // has it, the leader moves it.
    if (!state->controls && state->index != next)
        Reset(*state, next);
    state->controls = next < state->route.size() && state->inside && !state->stepDue;
    return state->controls;
}

std::vector<std::pair<std::string, std::string>> GuildRunStates::Settle(Player* leader, bool inside, bool stepDue)
{
    std::vector<std::pair<std::string, std::string>> events;
    Map* map = leader && leader->IsInWorld() && !leader->IsBeingTeleported() ? leader->GetMap() : nullptr;
    std::shared_ptr<State> state = map ? Find(map->GetInstanceId()) : nullptr;
    if (!state || state->leader != leader->GetGUIDLow())
        return events;

    const time_t now = time(nullptr);
    ObjectGuid pulled;
    size_t index = 0;
    GuildRunStop stop;
    GuildRunPull pull = GuildRunPull::Approach;
    bool ownSpot = false;
    bool stalled = false;
    std::vector<std::pair<std::string, std::string>> notes;
    {
        std::lock_guard<std::mutex> guard(state->lock);
        state->inside = inside;
        state->stepDue = stepDue;
        state->controls = state->controls && inside && !stepDue;
        index = state->index;
        notes.swap(state->notes);
        if (state->pullAt && state->pullAt != state->reportedPullAt)
        {
            state->reportedPullAt = state->pullAt;
            pulled = state->pulled;
        }
        // A leader the controller has held still for a minute, out of a fight: said once a minute, with why.
        if (state->controls && leader->IsAlive() && !leader->IsInCombat() && now - state->changedAt >= STALL_SECONDS
            && now - state->stallReportedAt >= STALL_SECONDS)
        {
            state->stallReportedAt = now;
            stalled = index < state->route.size();
            if (stalled)
                stop = state->route[index];
            pull = state->pull;
            ownSpot = state->ownSpot;
        }
    }

    events.insert(events.end(), notes.begin(), notes.end());
    // A new pull, as the job reports its own: the creature's name first.
    if (!pulled.IsEmpty())
    {
        Creature* creature = map->GetCreature(pulled);
        events.push_back({ "pulled", (creature ? creature->GetName() : std::string("a creature")) + ", run stop "
            + std::to_string(index + 1) + " of " + std::to_string(state->route.size()) });
    }
    if (stalled)
        events.push_back({ "run_stall", StallReason(leader, index, stop, pull, ownSpot) });
    return events;
}

std::string GuildRunStates::StallReason(Player* leader, size_t index, const GuildRunStop& stop, GuildRunPull pull, bool ownSpot)
{
    static const char* const pulls[] = { "approach", "gather", "pull", "wait" };
    std::ostringstream out;
    const ai::WorldPosition& spot = FightSpot(stop, pull, ownSpot);
    out << "stop " << index + 1 << " " << pulls[uint8(pull)] << ", "
        << uint32(leader->GetDistance(spot.getX(), spot.getY(), spot.getZ())) << " yd from its spot";
    if (PlayerbotAI* ai = leader->GetPlayerbotAI())
    {
        AiObjectContext* context = ai->GetAiObjectContext();
        if (leader->GetLootGuid() || context->GetValue<bool>("has available loot")->Get())
            out << "; loot pending";
        if (ai->IsStateActive(BotState::BOT_STATE_COMBAT))
            out << "; its AI in a fight";
        if (!ai->HasStrategy("run lead", BotState::BOT_STATE_NON_COMBAT))
            out << "; no run lead";
    }
    // Each member that keeps the pull back, and why.
    if (Group* group = leader->GetGroup())
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player* member = ref->getSource();
            if (!member || member == leader)
                continue;
            std::string why;
            if (!member->IsAlive())
                why = "dead";
            else if (!member->IsInWorld() || member->GetMapId() != leader->GetMapId())
                why = "away";
            else if (member->IsInCombat())
                why = "in a fight";
            else
            {
                const float distance = member->GetDistance(spot.getX(), spot.getY(), spot.getZ());
                if (distance > PARTY_NEAR)
                    why = std::to_string(uint32(distance)) + " yd off";
                else if (member->GetHealthPercent() < PULL_HEALTH)
                    why = std::to_string(uint32(member->GetHealthPercent())) + "% health";
                else if (member->GetPowerType() == POWER_MANA && member->GetMaxPower(POWER_MANA)
                    && member->GetPowerPercent(POWER_MANA) < PULL_MANA)
                    why = std::to_string(uint32(member->GetPowerPercent(POWER_MANA))) + "% mana";
            }
            if (!why.empty())
                out << "; " << member->GetName() << " " << why;
        }
    return out.str();
}

GuildRunView GuildRunStates::View(Player* bot) const
{
    GuildRunView view;
    if (!bot || !bot->IsInWorld() || bot->IsBeingTeleported())
        return view;
    Group* group = bot->GetGroup();
    if (!group)
        return view;
    std::shared_ptr<State> state = Find(bot->GetInstanceId());
    if (!state)
        return view;

    std::lock_guard<std::mutex> guard(state->lock);
    if (state->group != group->GetId() || state->mapId != bot->GetMapId())
        return view;
    view.valid = true;
    // At a stop the leader has just passed over, it lets go at once, before the tick moves it on.
    view.controls = state->controls && state->index < state->route.size() && state->route[state->index].planned;
    view.lead = state->leader == bot->GetGUIDLow();
    view.mapId = state->mapId;
    view.leader = state->leader;
    view.index = state->index;
    view.stops = state->route.size();
    if (state->index < state->route.size())
        view.stop = state->route[state->index];
    view.packs = state->packs;
    view.pull = state->pull;
    view.pulled = state->pulled;
    view.pullAt = state->pullAt;
    view.stopAt = state->stopAt;
    view.waitSince = state->waitSince;
    view.waited = state->waited;
    view.ownSpot = state->ownSpot;
    view.gatherSince = state->gatherSince;
    view.pullTries = state->pullTries;
    return view;
}

void GuildRunStates::Gather(Player* leader)
{
    std::shared_ptr<State> state = LeaderState(leader);
    if (!state)
        return;
    std::lock_guard<std::mutex> guard(state->lock);
    if (state->pull != GuildRunPull::Approach)
        return;
    state->pull = GuildRunPull::Gather;
    state->gatherSince = state->changedAt = time(nullptr);
}

void GuildRunStates::Pulled(Player* leader, ObjectGuid creature)
{
    std::shared_ptr<State> state = LeaderState(leader);
    if (!state)
        return;
    std::lock_guard<std::mutex> guard(state->lock);
    state->pull = GuildRunPull::Pull;
    state->pulled = creature;
    state->pullAt = state->changedAt = time(nullptr);
}

void GuildRunStates::PullFailed(Player* leader)
{
    std::shared_ptr<State> state = LeaderState(leader);
    if (!state)
        return;
    std::lock_guard<std::mutex> guard(state->lock);
    state->pull = GuildRunPull::Gather;
    state->pulled.Clear();
    state->gatherSince = state->changedAt = time(nullptr);
    ++state->pullTries;
}

void GuildRunStates::SetStop(Player* leader, size_t index)
{
    std::shared_ptr<State> state = LeaderState(leader);
    if (!state)
        return;
    std::lock_guard<std::mutex> guard(state->lock);
    Reset(*state, index);
}

void GuildRunStates::Wait(Player* leader, const std::string& detail)
{
    std::shared_ptr<State> state = LeaderState(leader);
    if (!state)
        return;
    std::lock_guard<std::mutex> guard(state->lock);
    if ((state->pull != GuildRunPull::Approach && state->pull != GuildRunPull::Gather) || state->waited)
        return;
    state->pull = GuildRunPull::Wait;
    state->waitSince = state->changedAt = time(nullptr);
    state->notes.push_back({ "run_wait", detail });
}

void GuildRunStates::Go(Player* leader)
{
    std::shared_ptr<State> state = LeaderState(leader);
    if (!state)
        return;
    std::lock_guard<std::mutex> guard(state->lock);
    if (state->pull != GuildRunPull::Wait)
        return;
    state->pull = GuildRunPull::Approach;
    state->changedAt = time(nullptr);
}

void GuildRunStates::WaitedOut(Player* leader, const std::string& detail)
{
    std::shared_ptr<State> state = LeaderState(leader);
    if (!state)
        return;
    std::lock_guard<std::mutex> guard(state->lock);
    if (state->pull != GuildRunPull::Wait)
        return;
    state->pull = GuildRunPull::Approach;
    state->waited = true;
    state->changedAt = time(nullptr);
    state->notes.push_back({ "run_waited", detail });
}

void GuildRunStates::UseOwnSpot(Player* leader, const std::string& detail)
{
    std::shared_ptr<State> state = LeaderState(leader);
    if (!state)
        return;
    std::lock_guard<std::mutex> guard(state->lock);
    if (state->ownSpot)
        return;
    state->ownSpot = true;
    state->pull = GuildRunPull::Approach;
    state->changedAt = time(nullptr);
    state->notes.push_back({ "run_own_spot", detail });
}

void GuildRunStates::Skip(Player* leader, const std::string& detail)
{
    std::shared_ptr<State> state = LeaderState(leader);
    if (!state)
        return;
    {
        std::lock_guard<std::mutex> guard(state->lock);
        if (state->index >= state->route.size())
            return;
        state->route[state->index].planned = false;
        state->route[state->index].skipped = detail;
        state->notes.push_back({ "run_skipped", "stop " + std::to_string(state->index + 1) + ": " + detail });
    }
    // On to the next stop the controller pulls, so the run never stands at one it passed over.
    Advance(leader, true);
}

const ai::WorldPosition& GuildRunStates::FightSpot(const GuildRunStop& stop, GuildRunPull pull, bool ownSpot)
{
    const bool before = stop.hasBefore && (pull == GuildRunPull::Wait || (stop.stillReach && !ownSpot));
    return before ? stop.before : stop.pull;
}

const std::vector<ai::WorldPosition>& GuildRunStates::FightStands(const GuildRunStop& stop, GuildRunPull pull, bool ownSpot)
{
    const bool before = stop.hasBefore && (pull == GuildRunPull::Wait || (stop.stillReach && !ownSpot));
    return before ? stop.beforeStands : stop.stands;
}

bool GuildRunStates::Reached(Map* map, Unit* bot, const GuildRunStop& stop)
{
    // With no stop before there is nowhere to wait, and the party pulls with the creature in reach: otherwise
    // the approach, the gather and the pull would all hold for it, and nothing would start the wait.
    if (!map || !bot || !stop.hasBefore)
        return false;
    for (uint32 spawn : stop.outsideReach)
    {
        Creature* creature = map->GetCreature(spawn);
        // Dead, or not loaded, which a creature near the party is: gone.
        if (!creature || !creature->IsInWorld() || !creature->IsAlive())
            continue;
        const float reach = creature->GetAttackDistance(bot) + creature->GetObjectBoundingRadius() + bot->GetObjectBoundingRadius()
            + REACH_MARGIN;
        if (creature->GetDistance(stop.pull.getX(), stop.pull.getY(), stop.pull.getZ(), DIST_CALC_NONE) <= reach)
            return true;
    }
    return false;
}

std::string GuildRunStates::Names(const std::vector<uint32>& spawns)
{
    std::string names;
    for (uint32 spawn : spawns)
    {
        CreatureData const* data = sObjectMgr.GetCreatureData(spawn);
        CreatureInfo const* info = data ? ObjectMgr::GetCreatureTemplate(data->id) : nullptr;
        names += (names.empty() ? "" : ", ") + (info ? std::string(info->Name) : std::to_string(spawn));
    }
    return names.empty() ? std::string("nothing") : names;
}

void GuildRunStates::Advance(Player* leader, bool skip)
{
    std::shared_ptr<State> state = LeaderState(leader);
    if (!state)
        return;
    size_t index = 0;
    std::vector<GuildRunStop> route;
    {
        std::lock_guard<std::mutex> guard(state->lock);
        index = state->index;
        route = state->route;
    }
    SetStop(leader, NextStop(state->packs.get(), route, leader->GetMap(), skip ? index + 1 : index));
}

std::vector<GuildRunStop> GuildRunStates::BuildRoute(Player* leader, const std::string& dungeon,
    std::shared_ptr<const GuildPacks::Route>& packs, std::string& error)
{
    std::vector<GuildRunStop> route;
    // At the leader's own level: the creatures' reach depends on the gap between the party's level and theirs.
    packs = sGuildPacks.GetRoute(dungeon, leader, leader->GetLevel(), error);
    if (!packs)
        return route;
    if (packs->mapId != leader->GetMapId())
    {
        error = "the route is for map " + std::to_string(packs->mapId);
        packs = nullptr;
        return route;
    }

    // Points of a leg with no travel map route lie on a straight line, and may be off the navmesh. With such a leg,
    // each place is checked: a stand point off it is dropped, and a stop whose pull spot is off it is passed over.
    // A pull spot a creature reaches is split by how it reaches it. One standing still, or of the pack's own, never
    // leaves (spotClear false): the pack is pulled from the stop before. Another pack's wanderers and patrols move:
    // the party waits them out at the stop before, by where they are when it comes.
    Map* map = leader->GetMap();
    const bool check = packs->straightLegs > 0;
    const uint32 mapId = packs->mapId;
    for (const GuildPacks::Stop& from : packs->stops)
    {
        GuildRunStop stop;
        stop.spawns = from.spawns;
        stop.pulled = from.pulled;
        stop.pull = ai::WorldPosition(mapId, from.pullSpot.x, from.pullSpot.y, from.pullSpot.z);
        if (check && !OnNavmesh(map, stop.pull))
        {
            stop.planned = false;
            stop.skipped = "its pull spot is off the navmesh";
        }
        for (const G3D::Vector3& at : from.stand)
        {
            const ai::WorldPosition stand(mapId, at.x, at.y, at.z);
            if (!check || OnNavmesh(map, stand))
                stop.stands.push_back(stand);
        }
        stop.rest = ai::WorldPosition(mapId, from.restSpot.x, from.restSpot.y, from.restSpot.z);
        stop.stillReach = !from.spotClear;
        if (stop.stillReach)
            stop.reachedBy = from.reachedBy;
        for (const GuildPacks::Reacher& reacher : from.reachers)
            if (!reacher.ownPack && reacher.kind != GuildPacks::ReachKind::Static)
                stop.outsideReach.push_back(reacher.spawn);
        // With no stop before, there is nowhere to wait or pull from afar: such a stop is pulled from its own spot.
        if (!route.empty())
        {
            stop.hasBefore = true;
            stop.before = route.back().pull;
            stop.beforeStands = route.back().stands;
        }
        route.push_back(stop);
    }
    return route;
}
