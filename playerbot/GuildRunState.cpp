#include "playerbot/playerbot.h"

#include "playerbot/GuildRunState.h"
#include "playerbot/GuildBotMgr.h"
#include "playerbot/GuildPacks.h"
#include "playerbot/strategy/values/LastMovementValue.h"
#include "Entities/Creature.h"
#include "Groups/Group.h"
#include "Maps/Map.h"
#include "Maps/MapManager.h"
#include "MotionGenerators/MoveMap.h"
#include "MotionGenerators/PathFinder.h"
#include "MotionGenerators/TargetedMovementGenerator.h"
#include "World/World.h"

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

    // The world as one run sees it, on the world thread between map updates: its group by id, and its members through its
    // own instance's map, so that a member of the group in another instance is never looked up as if it were in this one.
    class ServerRunWorld : public runcore::RunWorld
    {
    public:
        ServerRunWorld(uint32 mapId, uint32 instanceId) : map(sMapMgr.FindMap(mapId, instanceId)) {}

        bool Group(uint32_t groupId, std::vector<uint32_t>& members, uint32_t& leader) override
        {
            ::Group* group = sObjectMgr.GetGroupById(groupId);
            if (!group)
                return false;
            for (auto const& slot : group->GetMemberSlots())
                members.push_back(slot.guid.GetCounter());
            leader = group->GetLeaderGuid().GetCounter();
            return true;
        }

        runrules::Facts FactsOf(uint32_t guid) override
        {
            runrules::Facts facts;
            // Looked up in the world or between maps, so that a character between maps is not taken for one offline.
            Player* player = sObjectMgr.GetPlayer(ObjectGuid(HIGHGUID_PLAYER, guid), false);
            if (!player)
                return facts;
            facts.online = true;
            facts.engine = sGuildBotMgr.GetPlayerBot(guid) == player;
            facts.inWorld = player->IsInWorld();
            facts.teleporting = player->IsBeingTeleported();
            facts.alive = player->IsAlive();
            facts.inInstance = Here(guid) != nullptr;
            return facts;
        }

        std::string ActiveJob(uint32_t guid) override { return sGuildBotMgr.GetActiveJob(guid); }

        std::string Name(uint32_t guid) override
        {
            std::string name;
            if (!sObjectMgr.GetPlayerNameByGUID(ObjectGuid(HIGHGUID_PLAYER, guid), name))
                name = std::to_string(guid);
            return name;
        }

        bool Position(uint32_t guid, float& x, float& y, float& z) override
        {
            Player* player = Here(guid);
            if (!player)
                return false;
            x = player->GetPositionX();
            y = player->GetPositionY();
            z = player->GetPositionZ();
            return true;
        }

        void CancelMotion(uint32_t guid) override
        {
            Player* member = Here(guid);
            PlayerbotAI* ai = member ? member->GetPlayerbotAI() : nullptr;
            if (!ai || sGuildBotMgr.GetPlayerBot(guid) != member || !member->IsAlive())
                return;
            MotionMaster* motion = member->GetMotionMaster();
            std::vector<runrules::Motion> stack;
            for (MovementGenerator const* generator : *motion)
            {
                if (runrules::IsRunToken(generator->GetMoveId()))
                {
                    stack.push_back(runrules::Motion::RunToken);
                    continue;
                }
                switch (generator->GetMovementGeneratorType())
                {
                    case IDLE_MOTION_TYPE: stack.push_back(runrules::Motion::Idle); break;
                    case POINT_MOTION_TYPE: stack.push_back(runrules::Motion::Point); break;
                    case PATH_MOTION_TYPE: stack.push_back(runrules::Motion::Path); break;
                    case FOLLOW_MOTION_TYPE: stack.push_back(runrules::Motion::Follow); break;
                    case CHASE_MOTION_TYPE: stack.push_back(runrules::Motion::Chase); break;
                    default: stack.push_back(runrules::Motion::Other); break;
                }
            }
            // The fight's chase on top: its target, offset and angle, to put back.
            Unit* target = nullptr;
            float offset = 0.0f, angle = 0.0f;
            if (!motion->empty() && motion->GetCurrentMovementGeneratorType() == CHASE_MOTION_TYPE)
                if (auto const* chase = dynamic_cast<ChaseMovementGenerator const*>(motion->GetCurrent()))
                {
                    target = chase->GetCurrentTarget();
                    offset = chase->GetOffset();
                    angle = chase->GetAngle();
                }
            const runrules::CancelPlan plan = runrules::PlanCancel(stack,
                target && target->IsAlive() && member->GetVictim() == target);
            if (!plan.clear)
                return;
            ai->StopMoving();
            motion->Clear(true, false);
            ai->GetAiObjectContext()->GetValue<ai::LastMovement&>("last movement")->Get().clear();
            if (plan.restoreChase)
                motion->MoveChase(target, offset, angle);
        }

    private:
        // The member on this run's own instance's map, in the world and not between maps; none otherwise.
        Player* Here(uint32_t guid)
        {
            if (!map)
                return nullptr;
            Player* player = map->GetPlayer(ObjectGuid(HIGHGUID_PLAYER, guid));
            return player && player->IsInWorld() && !player->IsBeingTeleported() ? player : nullptr;
        }

        Map* map;
    };
}

uint64 GuildRunStates::Counter::Start() const
{
    return uint64(sWorld.GetStartTime());
}

const char* GuildRunStates::PullName(GuildRunPull pull)
{
    static const char* const pulls[] = { "approach", "gather", "pull", "wait" };
    return pulls[uint8(pull)];
}

std::shared_ptr<GuildRunStates::State> GuildRunStates::Find(uint32 instanceId) const
{
    std::lock_guard<std::mutex> guard(registryLock);
    auto it = byInstance.find(instanceId);
    return it == byInstance.end() ? nullptr : it->second;
}

std::shared_ptr<GuildRunStates::State> GuildRunStates::FindGroup(uint32 groupId) const
{
    std::lock_guard<std::mutex> guard(registryLock);
    for (auto const& [instanceId, state] : byInstance)
        if (state->group == groupId)
            return state;
    return nullptr;
}

std::shared_ptr<GuildRunStates::State> GuildRunStates::FindLeader(uint32 leaderGuid) const
{
    std::lock_guard<std::mutex> guard(registryLock);
    for (auto const& [instanceId, state] : byInstance)
        if (state->leader == leaderGuid)
            return state;
    return nullptr;
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

GuildRunControl GuildRunStates::Prepare(Player* leader, const std::string& dungeon,
    std::vector<std::pair<std::string, std::string>>& events)
{
    if (!leader)
        return GuildRunControl::None;
    Group* group = leader->GetGroup();
    Map* map = leader->IsInWorld() && !leader->IsBeingTeleported() ? leader->GetMap() : nullptr;
    // Outside the instance, or between maps, the leader leaves its run as it is: the tick holds it or the job has it, and
    // the job is told which, so that its way back in goes on and nothing else does.
    if (!group || !map || !map->IsDungeon())
    {
        std::shared_ptr<State> state = FindLeader(leader->GetGUIDLow());
        if (!state)
            return GuildRunControl::None;
        std::lock_guard<std::mutex> guard(state->lock);
        return state->core.Control();
    }

    std::shared_ptr<State> state = Find(map->GetInstanceId());
    if (state && (state->leader != leader->GetGUIDLow() || state->group != group->GetId()))
    {
        // Another run's state in this instance: it ends in order, and this run starts once it is gone.
        End(state->leader, "another_run_in_instance");
        return GuildRunControl::None;
    }
    if (!state)
    {
        // The group's run in another instance, as after a reset, ends first.
        if (std::shared_ptr<State> other = FindGroup(group->GetId()))
        {
            End(other->leader, "party_in_another_instance");
            return GuildRunControl::None;
        }
        state = std::make_shared<State>();
        state->group = group->GetId();
        state->leader = leader->GetGUIDLow();
        state->mapId = map->GetId();
        state->instanceId = map->GetInstanceId();
        state->dungeon = dungeon;
        std::string error;
        state->route = BuildRoute(leader, dungeon, state->packs, error);
        // No route, no controller: the job has the party alone, as it did before the controller, and no lifecycle starts.
        if (!runrules::Routed(state->route.size()))
        {
            bool first = false;
            {
                std::lock_guard<std::mutex> guard(registryLock);
                first = unrouted.insert({ leader->GetGUIDLow(), map->GetInstanceId() }).second;
            }
            if (first)
                events.push_back({ "run_route", "stops=0 error=" + error });
            return GuildRunControl::None;
        }
        // A party set down mid-route, as in a lab scenario, starts where it stands, not at a stop behind it.
        size_t start = state->route.size();
        if (state->packs)
        {
            const int32 at = GuildPacks::FirstLiveStop(*state->packs, map,
                G3D::Vector3(leader->GetPositionX(), leader->GetPositionY(), leader->GetPositionZ()));
            start = NextStop(state->packs.get(), state->route, map, at < 0 ? state->route.size() : size_t(at));
        }
        Reset(*state, start);
        std::vector<uint32_t> members;
        for (auto const& slot : group->GetMemberSlots())
            members.push_back(slot.guid.GetCounter());
        ServerRunWorld world(state->mapId, state->instanceId);
        state->core.Begin(world, generations, state->group, state->leader, members);
        {
            std::lock_guard<std::mutex> guard(registryLock);
            byInstance[state->instanceId] = state;
        }
        uint32 reached = 0, offMesh = 0;
        for (const GuildRunStop& stop : state->route)
        {
            reached += stop.stillReach || !stop.outsideReach.empty() ? 1 : 0;
            offMesh += stop.planned ? 0 : 1;
        }
        state->core.Note("run_route", "stops=" + std::to_string(state->route.size()) + " level=" + std::to_string(state->packs->level)
            + " start=" + std::to_string(state->index + 1) + " reached=" + std::to_string(reached)
            + " off_mesh=" + std::to_string(offMesh) + " members=" + std::to_string(members.size()));
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
    // While the job has the party, the leader writes nothing, and the stop follows the packs the job kills.
    if (state->core.mode == GuildRunMode::Released && state->index != next)
    {
        Reset(*state, next);
        state->core.Revise("job_kill", "stop=" + std::to_string(state->index + 1));
    }
    ServerRunWorld world(state->mapId, state->instanceId);
    return state->core.Prepare(world, generations, next < state->route.size());
}

std::vector<std::pair<std::string, std::string>> GuildRunStates::Settle(Player* leader, const runrules::JobReport& report)
{
    std::vector<std::pair<std::string, std::string>> events;
    if (!leader)
        return events;
    std::shared_ptr<State> state = FindLeader(leader->GetGUIDLow());
    if (!state)
        return events;
    Map* map = leader->IsInWorld() && !leader->IsBeingTeleported() ? leader->GetMap() : nullptr;

    const time_t now = time(nullptr);
    ObjectGuid pulled;
    size_t index = 0;
    GuildRunStop stop;
    GuildRunPull pull = GuildRunPull::Approach;
    bool ownSpot = false;
    bool stalled = false;
    std::string tag;
    {
        std::lock_guard<std::mutex> guard(state->lock);
        ServerRunWorld world(state->mapId, state->instanceId);
        state->core.Settle(world, generations, report);
        index = state->index;
        tag = "gen=" + std::to_string(state->core.start) + "." + std::to_string(state->core.generation)
            + " rev=" + std::to_string(state->core.plan);
        if (state->pullAt && state->pullAt != state->reportedPullAt)
        {
            state->reportedPullAt = state->pullAt;
            pulled = state->pulled;
        }
        // A leader the controller has held still for a minute, out of a fight: said once a minute, with why.
        if (state->core.mode == GuildRunMode::Lead && map && map->GetInstanceId() == state->instanceId && leader->IsAlive()
            && !leader->IsInCombat() && now - state->changedAt >= STALL_SECONDS && now - state->stallReportedAt >= STALL_SECONDS)
        {
            state->stallReportedAt = now;
            stalled = index < state->route.size();
            if (stalled)
                stop = state->route[index];
            pull = state->pull;
            ownSpot = state->ownSpot;
        }
    }

    // A new pull, as the job reports its own: the creature's name first after the tag.
    if (!pulled.IsEmpty())
    {
        Creature* creature = map ? map->GetCreature(pulled) : nullptr;
        events.push_back({ "pulled", tag + " stop=" + std::to_string(index + 1) + " of=" + std::to_string(state->route.size())
            + " " + (creature ? creature->GetName() : std::string("a creature")) });
    }
    if (stalled)
        events.push_back({ "run_stall", tag + " " + StallReason(leader, index, stop, pull, ownSpot) });
    return events;
}

void GuildRunStates::End(uint32 leaderGuid, const std::string& why)
{
    std::vector<std::shared_ptr<State>> states;
    {
        std::lock_guard<std::mutex> guard(registryLock);
        for (auto const& [instanceId, state] : byInstance)
            if (state->leader == leaderGuid)
                states.push_back(state);
    }
    for (const std::shared_ptr<State>& state : states)
    {
        std::lock_guard<std::mutex> guard(state->lock);
        ServerRunWorld world(state->mapId, state->instanceId);
        state->core.End(world, generations, why);
    }
}

std::vector<std::tuple<uint32, std::string, std::string>> GuildRunStates::Reconcile()
{
    std::vector<std::tuple<uint32, std::string, std::string>> events;
    std::vector<std::shared_ptr<State>> states;
    {
        std::lock_guard<std::mutex> guard(registryLock);
        for (auto const& [instanceId, state] : byInstance)
            states.push_back(state);
    }
    const time_t now = time(nullptr);
    std::vector<std::shared_ptr<State>> gone;
    for (const std::shared_ptr<State>& state : states)
    {
        std::lock_guard<std::mutex> guard(state->lock);
        ServerRunWorld world(state->mapId, state->instanceId);
        if (!state->core.Reconcile(world, generations, now))
            gone.push_back(state);
        for (const runcore::Event& event : state->core.events)
            events.emplace_back(state->leader, event.kind, event.detail);
        state->core.events.clear();
    }
    if (!gone.empty())
    {
        std::lock_guard<std::mutex> guard(registryLock);
        for (const std::shared_ptr<State>& state : gone)
        {
            auto it = byInstance.find(state->instanceId);
            if (it != byInstance.end() && it->second == state)
                byInstance.erase(it);
        }
    }
    return events;
}

std::list<std::string> GuildRunStates::Describe(const std::string& leaderName) const
{
    std::list<std::string> out;
    std::vector<std::shared_ptr<State>> states;
    {
        std::lock_guard<std::mutex> guard(registryLock);
        for (auto const& [instanceId, state] : byInstance)
            states.push_back(state);
    }
    auto nameOf = [](uint32 guid)
    {
        std::string name;
        if (!sObjectMgr.GetPlayerNameByGUID(ObjectGuid(HIGHGUID_PLAYER, guid), name))
            name = std::to_string(guid);
        return name;
    };
    // What a character is now, by its guid alone, never through a group pointer.
    auto factsOf = [](uint32 guid, uint32 mapId, uint32 instanceId)
    {
        Player* player = sObjectMgr.GetPlayer(ObjectGuid(HIGHGUID_PLAYER, guid), false);
        if (!player)
            return std::string("offline");
        std::string facts = sGuildBotMgr.GetPlayerBot(guid) == player ? "engine" : "player";
        facts += player->IsInWorld() && !player->IsBeingTeleported() ? "" : ",between_maps";
        facts += player->IsInWorld() && player->GetMapId() == mapId && player->GetInstanceId() == instanceId ? ",inside" : ",outside";
        facts += player->IsAlive() ? ",alive" : ",dead";
        return facts;
    };
    const time_t now = time(nullptr);
    for (const std::shared_ptr<State>& state : states)
    {
        std::lock_guard<std::mutex> guard(state->lock);
        const runcore::RunCore& core = state->core;
        const std::string leader = nameOf(state->leader);
        if (!leaderName.empty() && leaderName != leader)
            continue;
        std::ostringstream line;
        line << "run " << state->dungeon << " map=" << state->mapId << " instance=" << state->instanceId << " group=" << state->group
             << " leader=" << leader << "(" << factsOf(state->leader, state->mapId, state->instanceId) << ")";
        out.push_back(line.str());
        line.str("");
        line << "  mode=" << runrules::ModeName(core.mode) << " reason=" << (core.reason.empty() ? "-" : core.reason)
             << " held_from=" << runrules::ModeName(core.heldFrom) << " gen=" << core.start << "." << core.generation
             << " rev=" << core.plan << " membership=" << core.membership << " choose=" << (core.choose ? 1 : 0)
             << " hold_reasons=" << (core.holdReasons.empty() ? "-" : runrules::Join(core.holdReasons))
             << " job=" << (core.job.inside ? "inside" : "not_inside") << (core.job.stepDue ? ",step_due" : "")
             << (core.job.leaving ? ",leaving" : "");
        out.push_back(line.str());
        line.str("");
        line << "  stop=" << state->index + 1 << "/" << state->route.size() << " pull=" << PullName(state->pull)
             << " since_stop=" << (state->stopAt ? now - state->stopAt : 0)
             << " since_gather=" << (state->gatherSince ? now - state->gatherSince : 0)
             << " since_pull=" << (state->pullAt ? now - state->pullAt : 0)
             << " since_wait=" << (state->waitSince ? now - state->waitSince : 0)
             << " last_cleared=" << (state->lastCleared < 0 ? 0 : state->lastCleared + 1);
        if (core.mode == GuildRunMode::Hold)
            line << " leader_back=" << (core.leaderBackAt ? now - core.leaderBackAt : 0);
        out.push_back(line.str());
        for (uint32 guid : core.expected)
        {
            line.str("");
            line << "  member " << nameOf(guid) << " " << factsOf(guid, state->mapId, state->instanceId);
            auto held = core.held.find(guid);
            if (held != core.held.end())
                line << " holds_at=" << held->second.x << "," << held->second.y << "," << held->second.z;
            auto refused = state->refused.find(guid);
            if (refused != state->refused.end())
                line << " refused=" << refused->second;
            // Its run move's record, and its motion stack, bottom first, as type:id.
            GuildRunStamp moveStamp;
            if (const uint32 token = RunMoveOf(guid, moveStamp))
                line << " run_move=" << std::hex << token << std::dec << "@" << moveStamp.generation << ":" << moveStamp.plan;
            if (Player* player = sObjectMgr.GetPlayer(ObjectGuid(HIGHGUID_PLAYER, guid), false))
            {
                line << " stack=";
                bool first = true;
                for (MovementGenerator const* generator : *player->GetMotionMaster())
                {
                    line << (first ? "" : ",") << uint32(generator->GetMovementGeneratorType());
                    if (generator->GetMoveId())
                        line << ":" << std::hex << generator->GetMoveId() << std::dec;
                    first = false;
                }
            }
            out.push_back(line.str());
        }
    }
    if (out.empty())
        out.push_back(leaderName.empty() ? "no runs" : "no run led by " + leaderName);
    return out;
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
    const runcore::RunCore& core = state->core;
    view.valid = true;
    view.mode = core.mode;
    view.stamp = core.Stamp();
    view.choose = core.choose;
    view.instanceId = state->instanceId;
    view.expected.assign(core.expected.begin(), core.expected.end());
    // The controller leads only in the mode lead; at a stop the leader has just passed over, it lets go at once.
    view.controls = core.mode == GuildRunMode::Lead && state->index < state->route.size() && state->route[state->index].planned;
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
    if (core.mode == GuildRunMode::Hold)
    {
        auto held = core.held.find(bot->GetGUIDLow());
        if (held != core.held.end())
        {
            view.holds = true;
            view.holdAt = ai::WorldPosition(state->mapId, held->second.x, held->second.y, held->second.z);
        }
    }
    return view;
}

bool GuildRunStates::Current(Player* bot, const GuildRunView& view, const GuildRunStamp& stamp, bool leaderWrite)
{
    if (view.valid && runrules::Accepts(view.mode, view.stamp, stamp, leaderWrite))
        return true;
    Refused(bot);
    return false;
}

void GuildRunStates::Refused(Player* bot)
{
    if (std::shared_ptr<State> state = bot && bot->IsInWorld() ? Find(bot->GetInstanceId()) : nullptr)
    {
        std::lock_guard<std::mutex> guard(state->lock);
        ++state->refused[bot->GetGUIDLow()];
    }
}

bool GuildRunStates::Present(Player* leader, const GuildRunView& view)
{
    if (!leader || !leader->IsInWorld() || leader->IsBeingTeleported() || !view.valid
        || leader->GetMapId() != view.mapId || leader->GetInstanceId() != view.instanceId)
        return false;
    Map* map = leader->GetMap();
    std::vector<runrules::Facts> facts;
    for (uint32 guid : view.expected)
    {
        // Through the leader's own map only: on its thread, a player on another map is another thread's to read.
        // One not on this map is not present, whatever else it is.
        runrules::Facts member;
        Player* player = map->GetPlayer(ObjectGuid(HIGHGUID_PLAYER, guid));
        member.online = player != nullptr;
        member.inInstance = player != nullptr;
        member.inWorld = player && player->IsInWorld();
        member.teleporting = player && player->IsBeingTeleported();
        member.alive = player && player->IsAlive();
        facts.push_back(member);
    }
    return runrules::Present(facts);
}

ai::WorldPosition GuildRunStates::PlaceOf(const GuildRunView& view, Player* bot)
{
    if (!view.valid || !bot)
        return ai::WorldPosition();
    // In hold, where the tick placed this bot.
    if (view.mode == GuildRunMode::Hold)
        return view.holds ? view.holdAt : ai::WorldPosition();
    if (view.index >= view.stops || !view.stop.pull)
        return ai::WorldPosition();

    // Where the party fights this stop: its pull spot, or the stop before's while it waits there or pulls from afar.
    const ai::WorldPosition& spot = FightSpot(view.stop, view.pull, view.ownSpot);
    if (view.lead)
        return spot;

    // On the way, the members keep with the leader rather than walking to the spot by a way of their own.
    if (view.pull == GuildRunPull::Approach)
    {
        // The leader through the bot's own map, which its thread may read.
        Player* leader = bot->IsInWorld() && !bot->IsBeingTeleported()
            ? bot->GetMap()->GetPlayer(ObjectGuid(HIGHGUID_PLAYER, view.leader)) : nullptr;
        if (leader && leader->IsInWorld() && !leader->IsBeingTeleported())
            return ai::WorldPosition(leader);
        return spot;
    }

    // At the spot, each member stands by its role: melee at the two nearest points, a caster at the third, a healer at
    // the fourth, farthest back. Two of a role take that role's points in the group's order.
    const std::vector<ai::WorldPosition>& stands = FightStands(view.stop, view.pull, view.ownSpot);
    Group* group = bot->GetGroup();
    PlayerbotAI* ai = bot->GetPlayerbotAI();
    if (!group || stands.empty() || !ai)
        return spot;
    auto role = [ai](Player* player) { return PlayerbotAI::IsHeal(player) ? 2 : ai->IsRanged(player) ? 1 : 0; };
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

namespace
{
    runrules::Removal Removal(MotionMaster::RemoveResult result)
    {
        switch (result)
        {
            case MotionMaster::RemoveResult::Removed: return runrules::Removal::Removed;
            case MotionMaster::RemoveResult::Deferred: return runrules::Removal::Deferred;
            default: return runrules::Removal::NotFound;
        }
    }

    // The members' motion, for the run moves: any member in the world on the world pass, or only those on `map` on a map's
    // pass, which runs on that map's thread.
    class ServerMoveWorld : public runcore::MoveWorld
    {
    public:
        explicit ServerMoveWorld(Map* map = nullptr) : map(map) {}

        runcore::MoveMember Member(uint32_t guid) override
        {
            runcore::MoveMember member;
            Player* player = Find(guid);
            member.visible = !map || player;
            if (!player)
                return member;
            member.online = true;
            member.engine = sGuildBotMgr.GetPlayerBot(guid) == player;
            member.inWorld = player->IsInWorld();
            member.teleporting = player->IsBeingTeleported();
            member.mapId = player->GetMapId();
            member.instanceId = player->GetInstanceId();
            return member;
        }
        bool HasMove(uint32_t guid, uint32_t token) override
        {
            Player* player = Find(guid);
            return player && player->GetMotionMaster()->HasMove(token);
        }
        uint32_t TopMove(uint32_t guid) override
        {
            Player* player = Find(guid);
            return player && !player->GetMotionMaster()->empty() ? player->GetMotionMaster()->GetCurrent()->GetMoveId() : 0;
        }
        std::vector<uint32_t> MoveIds(uint32_t guid) override
        {
            std::vector<uint32_t> ids;
            if (Player* player = Find(guid))
                for (MovementGenerator const* generator : *player->GetMotionMaster())
                    ids.push_back(generator->GetMoveId());
            return ids;
        }
        runrules::Removal RemoveMove(uint32_t guid, uint32_t token) override
        {
            Player* player = Find(guid);
            return player ? Removal(player->GetMotionMaster()->RemoveMove(token)) : runrules::Removal::NotFound;
        }

    private:
        Player* Find(uint32_t guid) const
        {
            const ObjectGuid object(HIGHGUID_PLAYER, guid);
            return map ? map->GetPlayer(object) : sObjectMgr.GetPlayer(object, false);
        }

        Map* map;
    };
}

bool GuildRunStates::StartRunMove(Player* bot, const GuildRunView& view, const std::function<bool(uint32 token)>& move)
{
    // Still the run and the stamp the move was chosen under, after the removals' callbacks.
    auto current = [this, bot, &view]()
    {
        const GuildRunView now = View(bot);
        return now.valid && now.leader == view.leader && runrules::Accepts(now.mode, now.stamp, view.stamp, false);
    };
    return StartRunMove(bot, view, current, move);
}

bool GuildRunStates::StartRunMove(Player* bot, const GuildRunView& view, const std::function<bool()>& current,
    const std::function<bool(uint32 token)>& move)
{
    if (!bot || !bot->IsInWorld() || bot->IsBeingTeleported() || !view.valid)
        return false;
    ServerMoveWorld world(bot->GetMap());
    return moves.Start(world, bot->GetGUIDLow(), view.stamp, view.leader, current, move);
}

void GuildRunStates::StopStaleMoves(Map* map, uint32 leaderGuid, const runcore::RunNow& run)
{
    if (!map)
        return;
    ServerMoveWorld world(map);
    moves.Clean(world, { { leaderGuid, run } }, leaderGuid);
}

void GuildRunStates::CleanMoves()
{
    // The runs as they stand: each state's lock taken alone, and let go, before the records' lock.
    std::map<uint32, runcore::RunNow> runs;   // by leader
    std::vector<std::shared_ptr<State>> states;
    {
        std::lock_guard<std::mutex> guard(registryLock);
        for (auto const& [instanceId, state] : byInstance)
            states.push_back(state);
    }
    for (const std::shared_ptr<State>& state : states)
    {
        std::lock_guard<std::mutex> guard(state->lock);
        runcore::RunNow& run = runs[state->leader];
        run.stamp = state->core.Stamp();
        run.mode = state->core.mode;
        run.instanceId = state->instanceId;
        run.expected.assign(state->core.expected.begin(), state->core.expected.end());
    }
    ServerMoveWorld world;
    moves.Clean(world, runs);
}

uint32 GuildRunStates::RunMoveOf(uint32 guid, GuildRunStamp& stamp) const
{
    runcore::MoveRecord record;
    if (!moves.Find(guid, record))
        return 0;
    stamp = record.stamp;
    return record.token;
}

bool GuildRunStates::Write(Player* leader, const GuildRunStamp& stamp, const char* what,
    const std::function<bool(State&, Map*, std::string&)>& write)
{
    // The leader in the world and not between maps, its state by its instance, then the mode and the stamp: only then the map.
    if (!leader || !leader->IsInWorld() || leader->IsBeingTeleported())
        return false;
    std::shared_ptr<State> state = Find(leader->GetInstanceId());
    if (!state || state->leader != leader->GetGUIDLow())
        return false;
    Map* map = nullptr;
    runcore::RunNow now;
    {
        std::lock_guard<std::mutex> guard(state->lock);
        if (!runrules::Accepts(state->core.mode, state->core.Stamp(), stamp, true))
        {
            ++state->refused[leader->GetGUIDLow()];
            return false;
        }
        map = leader->GetMap();
        std::string fields;
        if (!write(*state, map, fields))
            return false;
        state->core.Revise(what, "stop=" + std::to_string(state->index + 1) + " pull=" + PullName(state->pull)
            + (fields.empty() ? "" : " " + fields));
        now.stamp = state->core.Stamp();
        now.mode = state->core.mode;
        now.instanceId = state->instanceId;
        now.expected.assign(state->core.expected.begin(), state->core.expected.end());
    }
    // The plan changed: what the old plan set moving on this map stops now, with the state's lock let go.
    StopStaleMoves(map, leader->GetGUIDLow(), now);
    return true;
}

void GuildRunStates::Gather(Player* leader, const GuildRunStamp& stamp)
{
    Write(leader, stamp, "gathered", [](State& state, Map*, std::string&)
    {
        if (state.pull != GuildRunPull::Approach)
            return false;
        state.pull = GuildRunPull::Gather;
        state.gatherSince = state.changedAt = time(nullptr);
        return true;
    });
}

void GuildRunStates::Pulled(Player* leader, const GuildRunStamp& stamp, ObjectGuid creature)
{
    Write(leader, stamp, "pulled", [creature](State& state, Map*, std::string&)
    {
        state.pull = GuildRunPull::Pull;
        state.pulled = creature;
        state.pullAt = state.changedAt = time(nullptr);
        return true;
    });
}

void GuildRunStates::PullFailed(Player* leader, const GuildRunStamp& stamp)
{
    Write(leader, stamp, "pull_brought_nothing", [](State& state, Map*, std::string&)
    {
        state.pull = GuildRunPull::Gather;
        state.pulled.Clear();
        state.gatherSince = state.changedAt = time(nullptr);
        ++state.pullTries;
        return true;
    });
}

void GuildRunStates::Wait(Player* leader, const GuildRunStamp& stamp, const std::string& detail)
{
    Write(leader, stamp, "waiting", [&detail](State& state, Map*, std::string& fields)
    {
        if ((state.pull != GuildRunPull::Approach && state.pull != GuildRunPull::Gather) || state.waited)
            return false;
        state.pull = GuildRunPull::Wait;
        state.waitSince = state.changedAt = time(nullptr);
        fields = "why=" + detail;
        return true;
    });
}

void GuildRunStates::Go(Player* leader, const GuildRunStamp& stamp)
{
    Write(leader, stamp, "spot_clear", [](State& state, Map*, std::string&)
    {
        if (state.pull != GuildRunPull::Wait)
            return false;
        state.pull = GuildRunPull::Approach;
        state.changedAt = time(nullptr);
        return true;
    });
}

void GuildRunStates::WaitedOut(Player* leader, const GuildRunStamp& stamp, const std::string& detail)
{
    Write(leader, stamp, "waited_out", [&detail](State& state, Map*, std::string& fields)
    {
        if (state.pull != GuildRunPull::Wait)
            return false;
        state.pull = GuildRunPull::Approach;
        state.waited = true;
        state.changedAt = time(nullptr);
        fields = "why=" + detail;
        return true;
    });
}

void GuildRunStates::UseOwnSpot(Player* leader, const GuildRunStamp& stamp, const std::string& detail)
{
    Write(leader, stamp, "own_spot", [&detail](State& state, Map*, std::string& fields)
    {
        if (state.ownSpot)
            return false;
        state.ownSpot = true;
        state.pull = GuildRunPull::Approach;
        state.changedAt = time(nullptr);
        fields = "why=" + detail;
        return true;
    });
}

void GuildRunStates::Skip(Player* leader, const GuildRunStamp& stamp, const std::string& detail)
{
    // On to the next stop the controller pulls, so the run never stands at one it passed over.
    Write(leader, stamp, "passed_over", [&detail](State& state, Map* map, std::string& fields)
    {
        if (state.index >= state.route.size())
            return false;
        fields = "skipped=" + std::to_string(state.index + 1) + " why=" + detail;
        state.route[state.index].planned = false;
        state.route[state.index].skipped = detail;
        Reset(state, NextStop(state.packs.get(), state.route, map, state.index + 1));
        return true;
    });
}

void GuildRunStates::Advance(Player* leader, const GuildRunStamp& stamp, bool skip)
{
    Write(leader, stamp, skip ? "on_past" : "cleared", [skip](State& state, Map* map, std::string& fields)
    {
        // Without skip, a stop whose pack is dead: the leader saw it cleared.
        if (!skip && state.index < state.route.size() && FirstLiving(state.packs.get(), map, state.index) != state.index)
        {
            state.lastCleared = int32(state.index);
            fields = "cleared=" + std::to_string(state.index + 1);
        }
        Reset(state, NextStop(state.packs.get(), state.route, map, skip ? state.index + 1 : state.index));
        return true;
    });
}

void GuildRunStates::Choose(Player* leader, const GuildRunStamp& stamp)
{
    Write(leader, stamp, "chosen", [leader](State& state, Map* map, std::string&)
    {
        if (!state.core.choose)
            return false;
        size_t start = state.route.size();
        if (state.packs)
        {
            const int32 first = GuildPacks::FirstLiveStop(*state.packs, map,
                G3D::Vector3(leader->GetPositionX(), leader->GetPositionY(), leader->GetPositionZ()));
            start = NextStop(state.packs.get(), state.route, map, first < 0 ? state.route.size() : size_t(first));
        }
        Reset(state, start);
        state.core.choose = false;
        return true;
    });
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
