#include "playerbot/playerbot.h"

#include "playerbot/GuildRunState.h"
#include "playerbot/GuildBotMgr.h"
#include "playerbot/GuildPacks.h"
#include "playerbot/GuildDungeon.h"
#include "playerbot/GuildMoveGuard.h"
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
    // A living member as readiness sees it, with its distance from where the party gathers. Food or drink in use counts as
    // carried, so a member drinking its last water is not out of drink.
    runrules::Ready ReadyFacts(Player* member, float distance)
    {
        runrules::Ready ready;
        ready.guid = member->GetGUIDLow();
        ready.inFight = member->IsInCombat();
        ready.distance = distance;
        ready.health = uint32(member->GetHealthPercent());
        ready.usesMana = member->GetPowerType() == POWER_MANA && member->GetMaxPower(POWER_MANA) > 0;
        ready.mana = ready.usesMana ? uint32(member->GetPowerPercent(POWER_MANA)) : 100;
        // The bags are looked through only for one short of health or mana, which is when supplies matter.
        // A mage that conjures carries what it can make: its own AI conjures out of a fight.
        if (ready.health < runrules::READY_HEALTH)
            ready.hasFood = GuildDungeon::Carries(member, false) || member->HasAuraType(SPELL_AURA_MOD_REGEN)
                || GuildDungeon::Makes(member, false);
        if (ready.usesMana && ready.mana < runrules::READY_MANA)
            ready.hasDrink = GuildDungeon::Carries(member, true) || member->HasAuraType(SPELL_AURA_MOD_POWER_REGEN)
                || GuildDungeon::Makes(member, true);
        return ready;
    }

    // Whether a person sits eating or drinking: seated, with a health or mana regeneration aura from an item's spell, as
    // food and drink are. A regeneration aura from anything else, a buff or a talent, is not eating.
    bool Eating(Player* member)
    {
        if (!member->IsSitState())
            return false;
        for (AuraType type : { SPELL_AURA_MOD_REGEN, SPELL_AURA_MOD_POWER_REGEN })
            for (Aura const* aura : member->GetAurasByType(type))
                if (aura && !aura->GetCastItemGuid().IsEmpty())
                    return true;
        return false;
    }

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

    G3D::Vector3 V3(const runrules::Vec& at)
    {
        return G3D::Vector3(at.x, at.y, at.z);
    }

    runrules::Vec VecOf(const G3D::Vector3& at)
    {
        return { at.x, at.y, at.z };
    }

    // The navmesh's way from one point to another, as its corners: false when there is none, or it ends short of `to`.
    bool NavLeg(Map* map, const G3D::Vector3& from, const G3D::Vector3& to, std::vector<runrules::Vec>& path)
    {
        dtNavMeshQuery const* query = map ? MMAP::MMapFactory::createOrGetMMapManager()->GetNavMeshQuery(map->GetId(), map->GetInstanceId())
            : nullptr;
        if (!query)
            return false;
        dtQueryFilter filter;
        filter.setIncludeFlags(NAV_GROUND | NAV_WATER);
        filter.setExcludeFlags(NAV_MAGMA_SLIME);
        // Detour's points are (y, z, x).
        const float start[3] = { from.y, from.z, from.x }, end[3] = { to.y, to.z, to.x }, box[3] = { 3.0f, 5.0f, 3.0f };
        dtPolyRef startRef = 0, endRef = 0;
        float startAt[3], endAt[3];
        if (dtStatusFailed(query->findNearestPoly(start, box, &filter, &startRef, startAt)) || !startRef
            || dtStatusFailed(query->findNearestPoly(end, box, &filter, &endRef, endAt)) || !endRef)
            return false;
        dtPolyRef polys[256];
        int count = 0;
        if (dtStatusFailed(query->findPath(startRef, endRef, startAt, endAt, &filter, polys, &count, 256)) || !count
            || polys[count - 1] != endRef)
            return false;
        float corners[64 * 3];
        int cornerCount = 0;
        if (dtStatusFailed(query->findStraightPath(startAt, endAt, polys, count, corners, nullptr, nullptr, &cornerCount, 64))
            || cornerCount < 2)
            return false;
        path.clear();
        for (int i = 0; i < cornerCount; ++i)
            path.push_back({ corners[i * 3 + 2], corners[i * 3], corners[i * 3 + 1] });
        return runrules::Distance(path.back(), VecOf(to)) <= 1.5f;
    }

    // What a run's walks are planned on: its route, the stop it is at and the last one the leader saw cleared, the way's
    // legs already checked, and the tick's path searches left for every run.
    struct Ground
    {
        std::shared_ptr<const GuildPacks::Route> packs;
        int32 lastCleared = -1;
        size_t index = 0;
        std::map<size_t, bool>* wayChecked = nullptr;
        int* budget = nullptr;
    };

    // The world as one run sees it, on the world thread between map updates: its group by id, and its members through its
    // own instance's map, so that a member of the group in another instance is never looked up as if it were in this one.
    class ServerRunWorld : public runcore::RunWorld
    {
    public:
        ServerRunWorld(uint32 mapId, uint32 instanceId, const Ground& ground = Ground())
            : map(sMapMgr.FindMap(mapId, instanceId)), ground(ground) {}

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

        bool HoldPlace(runcore::Held& place) override
        {
            const GuildPacks::Route* packs = ground.packs.get();
            if (!map || !packs || ground.lastCleared < 0 || size_t(ground.lastCleared) >= packs->stops.size())
                return false;
            const GuildPacks::Stop& stop = packs->stops[ground.lastCleared];
            // Every stop up to the cleared one dead, the route's rest spot found clear when it was made, and clear now.
            const int32 living = GuildPacks::FirstLiveStop(*packs, map, size_t(0));
            if (!stop.restClear || (living >= 0 && living <= ground.lastCleared))
                return false;
            place = { stop.restSpot.x, stop.restSpot.y, stop.restSpot.z };
            size_t onWay = 0;
            return WayIndex(place, onWay) && PlaceClear(place);
        }

        bool PlaceClear(const runcore::Held& at) override
        {
            if (!map || !ground.packs)
                return false;
            std::vector<uint32> dead;
            const std::vector<runrules::Vec> point = { runcore::VecOf(at) };
            return runrules::FirstThreat(sGuildPacks.Threats(*ground.packs, map, point, runrules::THREAT_MARGIN, dead), point) < 0;
        }

        bool StopWayPoint(runcore::Held& at) override
        {
            const GuildPacks::Route* packs = ground.packs.get();
            if (!packs || packs->points.empty() || ground.index >= packs->stops.size())
                return false;
            const size_t i = std::min(packs->points.size() - 1, size_t(packs->stops[ground.index].pullAlong / 2.0f));
            at = { packs->points[i].x, packs->points[i].y, packs->points[i].z };
            return true;
        }

        bool WayPointNear(const runcore::Held& at, runcore::Held& point) override
        {
            size_t index = 0;
            if (!WayIndex(at, index))
                return false;
            const G3D::Vector3& way = ground.packs->points[index];
            point = { way.x, way.y, way.z };
            return true;
        }

        runrules::PlanResult PlanWalk(uint32_t guid, const runcore::Held& to, runcore::Walk& walk, std::string& why) override
        {
            Player* member = Here(guid);
            const GuildPacks::Route* packs = ground.packs.get();
            if (!member || !packs || !ground.budget || !ground.wayChecked)
            {
                why = "no_route";
                return runrules::PlanResult::Blocked;
            }
            const runcore::Held from = { member->GetPositionX(), member->GetPositionY(), member->GetPositionZ() };
            // Near the party already, as a member a few yards from the anchor: one short navmesh path, checked the same way.
            if (runrules::Distance(runcore::VecOf(from), runcore::VecOf(to)) <= runrules::SHORT_WALK)
            {
                if (*ground.budget < 1)
                    return runrules::PlanResult::Pending;
                --*ground.budget;
                std::vector<runrules::Vec> path;
                std::vector<uint32> dead;
                if (!Connector(V3(runcore::VecOf(from)), V3(runcore::VecOf(to)), path))
                    why = "no_short_path";
                else if (!Blocked(path, why, &dead))
                {
                    walk.chunks = runrules::Chunks(path, runrules::WALK_CHUNK_YARDS);
                    walk.to = to;
                    walk.deadNear = dead;
                    if (!walk.chunks.empty())
                        return runrules::PlanResult::Planned;
                    why = "nothing_to_walk";
                }
                return runrules::PlanResult::Blocked;
            }
            // The stretch of the way between the two, the short way along it where the way passes either place twice.
            std::vector<runrules::Vec> wayPoints;
            wayPoints.reserve(packs->points.size());
            for (const G3D::Vector3& point : packs->points)
                wayPoints.push_back(VecOf(point));
            const std::vector<size_t> starts = runrules::WayNear(wayPoints, runcore::VecOf(from), runrules::WAY_NEAR);
            const std::vector<size_t> ends = runrules::WayNear(wayPoints, runcore::VecOf(to), runrules::WAY_NEAR);
            size_t a = 0, b = 0;
            if (starts.empty())
            {
                why = "off_way";
                return runrules::PlanResult::Blocked;
            }
            if (!runrules::WaySpan(starts, ends, a, b))
            {
                why = "target_off_way";
                return runrules::PlanResult::Blocked;
            }
            // The way between, in the walk's direction.
            std::vector<runrules::Vec> way;
            for (size_t i = a;; i = a < b ? i + 1 : i - 1)
            {
                way.push_back(VecOf(packs->points[i]));
                if (i == b)
                    break;
            }
            // First the creatures, which costs no path search: the straight line is near enough to tell a blocked way.
            std::vector<runrules::Vec> rough = way;
            rough.insert(rough.begin(), runcore::VecOf(from));
            rough.push_back(runcore::VecOf(to));
            if (Blocked(rough, why))
                return runrules::PlanResult::Blocked;
            // Then the way's own legs, WAY_CHECK_POINTS at a time, each kept for the run, and the two connectors, never kept.
            const size_t lo = std::min(a, b), hi = std::max(a, b);
            for (size_t first = lo - lo % runrules::WAY_CHECK_POINTS; first < hi; first += runrules::WAY_CHECK_POINTS)
            {
                auto checked = ground.wayChecked->find(first);
                if (checked == ground.wayChecked->end())
                {
                    if (*ground.budget <= 0)
                        return runrules::PlanResult::Pending;
                    --*ground.budget;
                    const size_t last = std::min(first + runrules::WAY_CHECK_POINTS, packs->points.size() - 1);
                    std::vector<runrules::Vec> leg, along;
                    for (size_t i = first; i <= last; ++i)
                        along.push_back(VecOf(packs->points[i]));
                    const bool ok = NavLeg(map, packs->points[first], packs->points[last], leg)
                        && runrules::Along(leg, along, runrules::WAY_CHECK_OFF);
                    checked = ground.wayChecked->emplace(first, ok).first;
                }
                if (!checked->second)
                {
                    why = "way_off_mesh_at_" + std::to_string(first * 2) + "yd";
                    return runrules::PlanResult::Blocked;
                }
            }
            if (*ground.budget < 2)
                return runrules::PlanResult::Pending;
            *ground.budget -= 2;
            std::vector<runrules::Vec> in, out;
            if (!Connector(V3(runcore::VecOf(from)), packs->points[a], in))
            {
                why = "no_path_onto_way";
                return runrules::PlanResult::Blocked;
            }
            if (!Connector(packs->points[b], V3(runcore::VecOf(to)), out))
            {
                why = "no_path_off_way";
                return runrules::PlanResult::Blocked;
            }
            // The line walked: from where the member stands onto the way, along it, and off it to the party.
            std::vector<runrules::Vec> line = in;
            line.insert(line.end(), way.begin() + 1, way.end());
            line.insert(line.end(), out.begin() + 1, out.end());
            std::vector<uint32> dead;
            if (Blocked(line, why, &dead))
                return runrules::PlanResult::Blocked;
            walk.chunks = runrules::Chunks(line, runrules::WALK_CHUNK_YARDS);
            if (walk.chunks.empty())
            {
                why = "nothing_to_walk";
                return runrules::PlanResult::Blocked;
            }
            walk.to = to;
            walk.deadNear = dead;
            return runrules::PlanResult::Planned;
        }

        bool Respawned(const std::vector<uint32_t>& spawns) override
        {
            if (!map)
                return false;
            for (uint32 spawn : spawns)
                if (Creature* creature = map->GetCreature(spawn))
                    if (creature->IsAlive())
                        return true;
            return false;
        }

        bool CreatureNear(const std::vector<runrules::Vec>& part) override
        {
            return map && ground.packs && !sGuildPacks.Unmodelled(*ground.packs, map, part, runrules::THREAT_MARGIN).empty();
        }

        bool Walking(uint32_t guid) override
        {
            Player* member = Here(guid);
            if (!member)
                return false;
            for (MovementGenerator const* generator : *member->GetMotionMaster())
                if (runrules::IsRunToken(generator->GetMoveId()) && generator->GetMovementGeneratorType() == PATH_MOTION_TYPE)
                    return true;
            return false;
        }

        bool ExitPoint(runcore::Held& at) override
        {
            // The route's way begins where the entrance puts a party.
            const GuildPacks::Route* packs = ground.packs.get();
            if (!packs || packs->points.empty())
                return false;
            const G3D::Vector3& first = packs->points.front();
            at = { first.x, first.y, first.z };
            return true;
        }

        bool ReadyOf(uint32_t guid, uint32_t leader, runrules::Ready& ready) override
        {
            // Inside the run's instance only: a member elsewhere is seen offline, and its obligations stay unmet.
            Player* member = Here(guid);
            if (!member)
                return false;
            Player* lead = Here(leader);
            const runrules::Facts facts = ready.facts;
            ready = ReadyFacts(member, lead && lead != member ? member->GetDistance(lead) : 0.0f);
            ready.facts = facts;
            return true;
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

        // The route's way point nearest a place, within WAY_NEAR.
        bool WayIndex(const runcore::Held& at, size_t& index) const
        {
            const GuildPacks::Route* packs = ground.packs.get();
            if (!packs)
                return false;
            float nearest = -1.0f;
            const runrules::Vec point = runcore::VecOf(at);
            for (size_t i = 0; i < packs->points.size(); ++i)
            {
                const float d = runrules::Distance(point, VecOf(packs->points[i]));
                if (d <= runrules::WAY_NEAR && (nearest < 0.0f || d < nearest))
                {
                    nearest = d;
                    index = i;
                }
            }
            return nearest >= 0.0f;
        }

        // A short way on or off the route's way: a whole navmesh path, not much longer than the straight line.
        bool Connector(const G3D::Vector3& from, const G3D::Vector3& to, std::vector<runrules::Vec>& path) const
        {
            const float straight = (to - from).length();
            if (straight < 0.5f)
            {
                path = { VecOf(from), VecOf(to) };
                return true;
            }
            if (!NavLeg(map, from, to, path))
                return false;
            path.front() = VecOf(from);
            return runrules::Length(path) <= straight * runrules::CONNECTOR_DETOUR + 2.0f;
        }

        // Whether a creature that may be alive reaches the line; the dead ones near it in `deadNear`.
        bool Blocked(const std::vector<runrules::Vec>& line, std::string& why, std::vector<uint32>* deadNear = nullptr) const
        {
            std::vector<uint32> dead;
            const std::vector<runrules::Threat> threats = sGuildPacks.Threats(*ground.packs, map, line, runrules::THREAT_MARGIN, dead);
            const int first = runrules::FirstThreat(threats, line);
            if (deadNear)
                *deadNear = dead;
            if (first < 0)
                return false;
            why = runrules::ThreatName(threats[first], GuildPacks::StopOf(*ground.packs, threats[first].spawn));
            return true;
        }

        Map* map;
        Ground ground;
    };

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

uint64 GuildRunStates::Counter::Start() const
{
    return uint64(sWorld.GetStartTime());
}

const char* GuildRunStates::PullName(GuildRunPull pull)
{
    static const char* const pulls[] = { "approach", "gather", "pull", "wait", "blocked" };
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
    // A new stop, or a new generation through Choose: the keepers said are forgotten, and a person's or guarded wait under
    // way ends, so the first keepers' write after it runs and keys the guarded clock to this stop and generation.
    if (!state.keepers.empty())
        SayKeepers(state, "");
    runplayer::ForgetKeepers(state.keepers, state.budget, state.personWaitSince, state.guardedClock, int64_t(time(nullptr)));
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
    if (!packs || !map)
        return FirstLiving(packs, map, index);
    return runrules::NextLiving(index, std::min(route.size(), packs->stops.size()),
        [&](size_t k) { return route[k].planned && GuildPacks::StopAlive(*packs, map, k); });
}

GuildRunControl GuildRunStates::Prepare(Player* leader, const std::string& dungeon, uint32 order,
    std::vector<std::pair<std::string, std::string>>& events)
{
    if (!leader)
        return GuildRunControl::None;
    // A run that failed for this order is not made again: the manager fails the party's jobs first.
    std::string failure;
    if (Failure(leader->GetGUIDLow(), order, failure))
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
        state->order = order;
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
        ServerRunWorld world(state->mapId, state->instanceId,
            Ground{ state->packs, state->lastCleared, state->index, &state->wayChecked, &planBudget });
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
    planBudget = runrules::PLAN_BUDGET;
    std::vector<uint32> cancels;
    for (const std::shared_ptr<State>& state : states)
    {
        std::lock_guard<std::mutex> guard(state->lock);
        ServerRunWorld world(state->mapId, state->instanceId,
            Ground{ state->packs, state->lastCleared, state->index, &state->wayChecked, &planBudget });
        // The leader's block, taken once: a recovery, or the run's end at the third at one stop.
        if (state->blockId)
            state->core.TakeBlock(world, generations, state->blockId, state->blockGeneration, state->blockStop,
                state->blockReasons, state->blockText, now);
        if (!state->core.Reconcile(world, generations, now))
            gone.push_back(state);
        cancels.insert(cancels.end(), state->core.cancels.begin(), state->core.cancels.end());
        state->core.cancels.clear();
        // A failed or stopped run's end outlives it, until the manager ends the party's jobs.
        if ((state->core.failed || state->core.stopped) && !state->failureKept)
        {
            state->failureKept = true;
            failures.Record(state->leader, state->order, state->core.reason,
                state->core.stopped ? runplayer::Outcome::Stopped : runplayer::Outcome::Failed);
        }
        // A fight the leader did not pull, as one a person started: the planned pull is cancelled by a revision, the party
        // defends, and once it is over the leader chooses its stop again from what died.
        if (state->core.mode == GuildRunMode::Lead)
        {
            Map* map = sMapMgr.FindMap(state->mapId, state->instanceId);
            bool fighting = false;
            for (uint32 guid : state->core.expected)
                if (Player* member = map ? map->GetPlayer(ObjectGuid(HIGHGUID_PLAYER, guid)) : nullptr)
                    fighting = fighting || (member->IsInWorld() && member->IsAlive() && member->IsInCombat());
            const bool unplanned = runplayer::UnplannedFight(fighting, state->pull == GuildRunPull::Pull, true);
            if (unplanned && !state->unplannedFight)
            {
                state->unplannedFight = true;
                state->core.Revise("unplanned_fight", "stop=" + std::to_string(state->index + 1) + " pull=" + PullName(state->pull));
            }
            else if (!fighting && state->unplannedFight)
            {
                state->unplannedFight = false;
                state->core.choose = true;
                state->core.Note("run_unplanned", "result=over stop=" + std::to_string(state->index + 1) + " choose=1");
            }
        }
        else
            state->unplannedFight = false;
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
    // bench clearroute's sweeps for summons that come late.
    RunSweeps(events);
    // Walks planned again or given up: their run moves go now, with no lock held, before the maps update.
    if (!cancels.empty())
    {
        ServerMoveWorld world;
        for (uint32 guid : cancels)
            moves.Cancel(world, guid);
    }
    return events;
}

bool GuildRunStates::Permit(uint32 leaderGuid) const
{
    std::shared_ptr<State> state = FindLeader(leaderGuid);
    if (!state)
        return true;
    std::lock_guard<std::mutex> guard(state->lock);
    return state->core.mode == GuildRunMode::Ended ? !state->core.failed : state->core.Permit();
}

bool GuildRunStates::Failure(uint32 leaderGuid, uint32 order, std::string& reason, runplayer::Outcome* outcome) const
{
    return failures.Find(leaderGuid, order, reason, outcome);
}

void GuildRunStates::ControlChanged(uint32 guid, runplayer::Controller to, const std::string& why)
{
    std::vector<std::shared_ptr<State>> states;
    {
        std::lock_guard<std::mutex> guard(registryLock);
        for (auto const& [instanceId, state] : byInstance)
            states.push_back(state);
    }
    for (const std::shared_ptr<State>& state : states)
    {
        std::lock_guard<std::mutex> guard(state->lock);
        if (std::find(state->core.expected.begin(), state->core.expected.end(), guid) == state->core.expected.end())
            continue;
        ServerRunWorld world(state->mapId, state->instanceId);
        auto away = state->core.apart.find(guid);
        if (away != state->core.apart.end() && away->second.walking)
        {
            away->second.walking = false;
            away->second.walk = runcore::Walk();
        }
        state->core.ControlChanged(world, guid, to, why);
    }
}

void GuildRunStates::CancelMoves(uint32 guid)
{
    ServerMoveWorld world;
    moves.Cancel(world, guid);
}

bool GuildRunStates::Expects(uint32 guid) const
{
    // The registry's lock and a state's are never held together.
    std::vector<std::shared_ptr<State>> states;
    {
        std::lock_guard<std::mutex> guard(registryLock);
        for (auto const& [instanceId, state] : byInstance)
            states.push_back(state);
    }
    for (const std::shared_ptr<State>& state : states)
    {
        std::lock_guard<std::mutex> stateGuard(state->lock);
        if (state->core.mode != GuildRunMode::Ended
            && std::find(state->core.expected.begin(), state->core.expected.end(), guid) != state->core.expected.end())
            return true;
    }
    return false;
}

void GuildRunStates::Acknowledge(uint32 leaderGuid, uint32 order)
{
    failures.Acknowledge(leaderGuid, order);
}

bool GuildRunStates::PullAllowed(Player* bot) const
{
    if (!bot || !bot->IsInWorld() || bot->IsBeingTeleported())
        return false;
    const GuildRunView view = View(bot);
    if (!view.valid)
        return true;
    if (!view.permit || !Present(bot, view))
        return false;
    // A fight already on is the party's own to fight, whatever readiness says.
    if (bot->IsInCombat())
        return true;
    return view.pull != GuildRunPull::Blocked && runrules::PullAllowed(false, view.recovering, Readiness(bot, view));
}

std::vector<runrules::Kept> GuildRunStates::Readiness(Player* bot, const GuildRunView& view)
{
    std::vector<runrules::Kept> kept;
    if (!bot || !bot->IsInWorld() || bot->IsBeingTeleported() || !view.valid)
        return kept;
    Map* map = bot->GetMap();
    Player* leader = map->GetPlayer(ObjectGuid(HIGHGUID_PLAYER, view.leader));
    // Where the party gathers: the stop's fight spot in lead, else around the leader, as the job gathers it.
    const bool atStop = view.mode == GuildRunMode::Lead && view.index < view.stops;
    ai::WorldPosition spot, leaderAt;
    if (leader && leader->IsInWorld() && !leader->IsBeingTeleported())
        leaderAt = ai::WorldPosition(leader);
    if (atStop)
        spot = FightSpot(view.stop, view.pull, view.ownSpot);
    else if (leader && leader->IsInWorld())
        spot = ai::WorldPosition(leader);
    // On the way to a stop the members follow the leader (PlaceOf), so they are measured from it, and the leader alone from
    // its spot: a leader the guard keeps on its way is the one kept, guarded and then far, not the members by it.
    const bool approaching = atStop && view.pull == GuildRunPull::Approach;
    // At a stop in lead: the leader must be at its own place, to gather and then to pull (runrules::LeaderNear), and a
    // member the movement guard keeps from its place by a creature that moves is guarded, while the stop's allowance lasts.
    const int64_t now = int64_t(time(nullptr));
    const bool allowance = atStop && runplayer::GuardedLeft(view.guardedClock, view.index, view.stamp.generation, now);
    std::vector<runrules::Ready> members;
    for (uint32 guid : view.expected)
    {
        runrules::Ready ready;
        // Through the bot's own map only, which its thread may read: one elsewhere is not there.
        Player* member = map->GetPlayer(ObjectGuid(HIGHGUID_PLAYER, guid));
        const ai::WorldPosition& from = runrules::MeasuredFromLeader(approaching, guid == view.leader) && leaderAt ? leaderAt
            : spot;
        if (member && member->IsInWorld() && !member->IsBeingTeleported())
            ready = ReadyFacts(member, from ? member->GetDistance(from.getX(), from.getY(), from.getZ()) : 0.0f);
        if (atStop && guid == view.leader && (view.pull == GuildRunPull::Approach || view.pull == GuildRunPull::Wait
            || view.pull == GuildRunPull::Gather))
            ready.near = runrules::LeaderNear(view.pull == GuildRunPull::Gather);
        // A copy of its refusal record, under the guard's lock alone: no run state's lock is held here.
        if (allowance && member && sGuildBotMgr.GetPlayerBot(guid) == member)
            ready.guarded = runguard::GuardedNow(sGuildMoveGuard.RefusalOf(guid), view.stamp.generation, view.index, now);
        ready.guid = guid;
        ready.facts.online = member != nullptr;
        ready.facts.engine = member && sGuildBotMgr.GetPlayerBot(guid) == member;
        ready.facts.inWorld = member && member->IsInWorld();
        ready.facts.teleporting = member && member->IsBeingTeleported();
        ready.facts.inInstance = member != nullptr;
        ready.facts.alive = member && member->IsAlive();
        ready.rejoining = std::find(view.apartMembers.begin(), view.apartMembers.end(), guid) != view.apartMembers.end();
        // A person's member: its own eating and drinking, and a healer's sight of the tank, the run's leader.
        ready.person = std::find(view.persons.begin(), view.persons.end(), guid) != view.persons.end()
            || (member && sGuildBotMgr.GetPlayerBot(guid) != member);
        if (ready.person && member && member->IsInWorld() && !member->IsBeingTeleported())
        {
            ready.eating = Eating(member);
            ready.healer = PlayerbotAI::IsHeal(member, true);
            if (leader && leader != member && leader->IsInWorld())
            {
                ready.seesTank = member->IsWithinLOSInMap(leader);
                ready.tankDistance = member->GetDistance(leader);
            }
        }
        members.push_back(ready);
    }
    return runrules::Readiness(members);
}

bool GuildRunStates::BlockNow(Player* leader, const GuildRunView& view, std::vector<runrules::Kept>& bots)
{
    bots.clear();
    if (!leader || !leader->IsInWorld() || leader->IsBeingTeleported() || !view.valid || view.mode != GuildRunMode::Lead
        || view.index >= view.stops || view.pull == GuildRunPull::Blocked || view.pull == GuildRunPull::Pull
        || view.budget.stop != view.index || view.budget.generation != view.stamp.generation)
        return false;
    // The block at once waits for the leader gathered at its spot, near enough to pull.
    const ai::WorldPosition& spot = FightSpot(view.stop, view.pull, view.ownSpot);
    const bool gathered = view.pull == GuildRunPull::Gather
        && leader->GetDistance(spot.getX(), spot.getY(), spot.getZ()) <= runrules::LeaderNear(true);
    return runplayer::BlockDueNow(view.budget, view.personWaitSince, view.guardedClock, Readiness(leader, view), gathered,
        int64_t(time(nullptr)), bots);
}

std::string GuildRunStates::KeepersText(const std::vector<runrules::Kept>& kept)
{
    return runrules::KeepText(kept, [](uint32_t guid)
    {
        std::string name;
        if (!sObjectMgr.GetPlayerNameByGUID(ObjectGuid(HIGHGUID_PLAYER, guid), name))
            name = std::to_string(guid);
        return name;
    });
}

bool GuildRunStates::Recovery(uint32 leaderGuid, uint32& id, std::vector<uint32>& supplies, bool& walkingOut) const
{
    std::shared_ptr<State> state = FindLeader(leaderGuid);
    if (!state)
        return false;
    std::lock_guard<std::mutex> guard(state->lock);
    if (!state->core.recovering)
        return false;
    id = state->core.lastBlock;
    walkingOut = state->core.exiting || state->core.exitWanted;
    supplies.clear();
    // Only once the party has walked the checked way to the entrance: the job's trip starts from there.
    if (state->core.exitDone)
        for (const runrules::Obligation& obligation : state->core.obligations)
            if (obligation.why == runrules::Keep::Supplies)
                supplies.push_back(obligation.guid);
    return true;
}

std::list<std::string> GuildRunStates::ClearRoute(Player* leader, uint32 stop)
{
    std::shared_ptr<State> state = leader ? FindLeader(leader->GetGUIDLow()) : nullptr;
    if (!state)
        return { "no run for that leader" };
    std::shared_ptr<const GuildPacks::Route> packs;
    uint32 mapId = 0, instanceId = 0;
    {
        std::lock_guard<std::mutex> guard(state->lock);
        packs = state->packs;
        mapId = state->mapId;
        instanceId = state->instanceId;
    }
    Map* map = sMapMgr.FindMap(mapId, instanceId);
    if (!packs || !map)
        return { "the run has no route or no instance" };
    if (!stop || stop > packs->stops.size() + 1)
        return { "stop must be 1 to " + std::to_string(packs->stops.size() + 1) };
    std::set<uint32> spawns;
    for (size_t k = 0; k + 1 < stop; ++k)
    {
        const GuildPacks::Stop& clear = packs->stops[k];
        spawns.insert(clear.spawns.begin(), clear.spawns.end());
        spawns.insert(clear.extras.begin(), clear.extras.end());
        for (const GuildPacks::Reacher& reacher : clear.reachers)
            spawns.insert(reacher.spawn);
    }
    uint32 killed = 0, missing = 0, unmodelled = 0;
    for (uint32 spawn : spawns)
    {
        Creature* creature = map->GetCreature(spawn);
        if (!creature || !creature->IsInWorld())
        {
            ++missing;
            continue;
        }
        if (!creature->IsAlive())
            continue;
        Unit::DealDamage(creature, creature, creature->GetHealth(), nullptr, DIRECT_DAMAGE, SPELL_SCHOOL_MASK_NORMAL, nullptr, false);
        ++killed;
    }
    // Then what the model does not have, within reach of the cleared stops' way: summons, those the kills just brought
    // among them (Sneed, from his Shredder's death). A second pass for what those deaths summon in turn.
    if (stop > 1 && !packs->points.empty())
    {
        const size_t last = std::min(packs->points.size() - 1,
            size_t(packs->stops[stop - 2].pullAlong / 2.0f) + size_t(runrules::WAY_CHECK_POINTS));
        std::vector<runrules::Vec> way;
        for (size_t i = 0; i <= last; ++i)
            way.push_back({ packs->points[i].x, packs->points[i].y, packs->points[i].z });
        std::string names;
        for (int pass = 0; pass < 2; ++pass)
            unmodelled += SweepUnmodelled(map, *packs, way, names);
        // A script summons some a few seconds after the death that calls them, as the Shredder's does Sneed: the same
        // sweep runs again on the tick for SWEEP_SECONDS, and says what it killed with a bench_clear_route event.
        Sweep sweep;
        sweep.leader = leader->GetGUIDLow();
        sweep.mapId = mapId;
        sweep.instanceId = instanceId;
        sweep.packs = packs;
        sweep.way = way;
        sweep.until = time(nullptr) + SWEEP_SECONDS;
        std::lock_guard<std::mutex> guard(registryLock);
        sweeps.push_back(sweep);
    }
    return { "cleared stops 1 to " + std::to_string(stop - 1) + " of " + std::to_string(packs->stops.size()) + ": killed "
        + std::to_string(killed) + ", not loaded " + std::to_string(missing) + ", of " + std::to_string(spawns.size())
        + "; and " + std::to_string(unmodelled) + " not in the route's model" };
}

uint32 GuildRunStates::SweepUnmodelled(Map* map, const GuildPacks::Route& packs, const std::vector<runrules::Vec>& way,
    std::string& names)
{
    uint32 killed = 0;
    std::vector<ObjectGuid> found;
    sGuildPacks.Unmodelled(packs, map, way, runrules::THREAT_MARGIN, &found);
    for (ObjectGuid guid : found)
        if (Creature* creature = map->GetCreature(guid))
            if (creature->IsAlive())
            {
                names += (names.empty() ? "" : ",") + std::string(creature->GetName());
                Unit::DealDamage(creature, creature, creature->GetHealth(), nullptr, DIRECT_DAMAGE,
                    SPELL_SCHOOL_MASK_NORMAL, nullptr, false);
                ++killed;
            }
    return killed;
}

void GuildRunStates::RunSweeps(std::vector<std::tuple<uint32, std::string, std::string>>& events)
{
    std::vector<Sweep> pending;
    {
        std::lock_guard<std::mutex> guard(registryLock);
        pending.swap(sweeps);
    }
    if (pending.empty())
        return;
    const time_t now = time(nullptr);
    std::vector<Sweep> keep;
    for (Sweep& sweep : pending)
    {
        Map* map = sMapMgr.FindMap(sweep.mapId, sweep.instanceId);
        if (!map || !sweep.packs)
            continue;
        std::string names;
        const uint32 killed = SweepUnmodelled(map, *sweep.packs, sweep.way, names);
        if (killed)
        {
            sweep.killed += killed;
            events.emplace_back(sweep.leader, "bench_clear_route", "killed=" + std::to_string(killed) + " not_in_model=" + names);
        }
        if (now < sweep.until)
            keep.push_back(sweep);
        else
            events.emplace_back(sweep.leader, "bench_clear_route", "sweep_done killed=" + std::to_string(sweep.killed));
    }
    std::lock_guard<std::mutex> guard(registryLock);
    sweeps.insert(sweeps.end(), keep.begin(), keep.end());
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
        line << " permit=" << (core.Permit() ? 1 : 0) << " failed=" << (core.failed ? 1 : 0) << " stopped=" << (core.stopped ? 1 : 0)
             << " apart_for=" << (core.apartSince ? now - core.apartSince : 0);
        // Readiness: the budget's age at this stop and generation, who keeps the pull back, the blocks counted, a recovery.
        const bool budgetHere = state->budget.since && state->budget.stop == state->index && state->budget.generation == core.generation;
        line << " budget=" << (budgetHere ? now - state->budget.since : 0) << " keepers=" << (state->keepers.empty() ? "none" : state->keepers);
        std::string counts;
        for (auto const& [stop, count] : core.blockCounts)
            counts += (counts.empty() ? "" : ",") + std::to_string(stop) + ":" + std::to_string(count);
        line << " blocks=" << (counts.empty() ? "none" : counts);
        if (core.recovering)
        {
            std::string due;
            for (const runrules::Obligation& obligation : core.obligations)
                due += (due.empty() ? "" : ",") + nameOf(obligation.guid) + ":" + runrules::KeepName(obligation.why) + ":"
                    + std::to_string(obligation.deadline - int64_t(now)) + "s";
            line << " recovery=" << due;
        }
        if (core.mode == GuildRunMode::Hold)
            line << " anchor=" << (core.hasAnchor ? core.anchorKind + "@" + std::to_string(int32(core.anchor.x)) + ","
                + std::to_string(int32(core.anchor.y)) + "," + std::to_string(int32(core.anchor.z)) : std::string("none"));
        out.push_back(line.str());
        for (uint32 guid : core.expected)
        {
            line.str("");
            line << "  member " << nameOf(guid) << " " << factsOf(guid, state->mapId, state->instanceId);
            auto held = core.held.find(guid);
            if (held != core.held.end())
                line << " holds_at=" << held->second.x << "," << held->second.y << "," << held->second.z;
            auto controller = core.controllers.find(guid);
            if (controller != core.controllers.end() && controller->second == runplayer::Controller::Person)
            {
                auto off = core.offline.find(guid);
                line << " person=1";
                if (off != core.offline.end() && off->second.since)
                    line << " offline_for=" << now - off->second.since;
            }
            auto away = core.apart.find(guid);
            if (away != core.apart.end())
            {
                line << " apart=" << (away->second.missing ? "missing" : away->second.cohort ? "cohort" : "returner")
                     << " for=" << now - away->second.since;
                if (away->second.walking)
                    line << " walk=" << away->second.walk.id << " part=" << away->second.walk.chunk + 1 << "/"
                         << away->second.walk.chunks.size();
                if (!away->second.blocked.empty())
                    line << " blocked=" << away->second.blocked;
            }
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
    view.permit = core.Permit();
    for (auto const& [guid, away] : core.apart)
        view.apartMembers.push_back(guid);
    for (auto const& [guid, controller] : core.controllers)
        if (controller == runplayer::Controller::Person)
            view.persons.push_back(guid);
    view.personWaitSince = state->personWaitSince;
    view.guardedClock = state->guardedClock;
    view.unplannedFight = state->unplannedFight;
    view.budget = state->budget;
    view.keepers = state->keepers;
    view.recovering = core.recovering;
    auto away = core.apart.find(bot->GetGUIDLow());
    if (away != core.apart.end())
    {
        view.apart = true;
        const runcore::Walk& walk = away->second.walk;
        if (away->second.walking && walk.chunk < walk.chunks.size())
        {
            view.walking = true;
            view.walkId = walk.id;
            view.walkPart = walk.chunk;
            for (const runrules::Vec& point : walk.chunks[walk.chunk])
                view.walkPoints.emplace_back(state->mapId, point.x, point.y, point.z);
            view.deadNear = walk.deadNear;
        }
    }
    if (core.mode == GuildRunMode::Hold && !view.walking)
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
        // The waiting budget starts at the first arrival at this stop's spot under this generation, and is kept after.
        runrules::TrackBudget(state.budget, state.index, state.core.generation, true, int64_t(state.gatherSince));
        return true;
    });
}

void GuildRunStates::Block(Player* leader, const GuildRunStamp& stamp, const std::vector<runrules::Kept>& kept)
{
    if (kept.empty())
        return;
    const std::string text = KeepersText(kept);
    Write(leader, stamp, "blocked", [&kept, &text](State& state, Map*, std::string& fields)
    {
        if (state.pull == GuildRunPull::Blocked || state.pull == GuildRunPull::Pull || state.index >= state.route.size())
            return false;
        state.pull = GuildRunPull::Blocked;
        state.changedAt = time(nullptr);
        // Ids are unique while the server runs, so that the job's once-a-block trip never mistakes one run's for another's.
        static std::atomic<uint32> blockIds{ 0 };
        state.blockId = ++blockIds;
        // The generation it is taken under: a revision keeps the generation, a hold or a new five changes it.
        state.blockGeneration = state.core.generation;
        state.blockStop = state.packs && state.index < state.packs->stops.size() ? state.packs->stops[state.index].pack
            : uint32(state.index + 1);
        state.blockReasons = kept;
        state.blockText = text;
        fields = "block=" + std::to_string(state.blockId) + " reasons=" + text;
        return true;
    });
}

void GuildRunStates::Keepers(Player* leader, const GuildRunStamp& stamp, const std::vector<runrules::Kept>& kept)
{
    if (!leader || !leader->IsInWorld() || leader->IsBeingTeleported())
        return;
    std::shared_ptr<State> state = Find(leader->GetInstanceId());
    if (!state || state->leader != leader->GetGUIDLow())
        return;
    const std::string text = KeepersText(kept);
    std::string say;
    {
        std::lock_guard<std::mutex> guard(state->lock);
        // Telemetry: no plan revision, so that the members' gathering moves go on.
        // Only when what it says changes, or a guarded member waits on a clock not yet keyed to this stop and generation.
        if (!runrules::Accepts(state->core.mode, state->core.Stamp(), stamp, true)
            || !runplayer::KeepersDue(text, state->keepers, kept, state->guardedClock, state->index, state->core.generation))
            return;
        // Only a person keeping the pull back: the budget is frozen for as long, and no block comes of it. Only guarded
        // members besides: frozen on the guarded clock, for its allowance at most.
        const time_t now = time(nullptr);
        const runplayer::Wait wait = runplayer::WaitOf(kept);
        runplayer::FreezeBudget(state->budget, state->personWaitSince, wait == runplayer::Wait::Person, int64_t(now));
        runplayer::GuardedFreeze(state->guardedClock, state->budget, wait == runplayer::Wait::Guarded, state->index,
            state->core.generation, int64_t(now));
        const int64_t guardedWait = runplayer::GuardedUsed(state->guardedClock, state->index, state->core.generation, int64_t(now));
        state->keepers = text;
        state->core.Note("run_keep", "stop=" + std::to_string(state->index + 1) + " keepers=" + (text.empty() ? "none" : text)
            + " budget=" + std::to_string(state->budget.since ? now - state->budget.since : 0)
            + (state->personWaitSince ? " person_wait=" + std::to_string(now - state->personWaitSince) : std::string())
            + (guardedWait ? " guarded_wait=" + std::to_string(guardedWait) : std::string()));
        // What it waits for a person for, in party chat: when it changes, and at most once a minute.
        say = runplayer::WaitLine(kept, [](uint32_t guid)
        {
            std::string name;
            if (!sObjectMgr.GetPlayerNameByGUID(ObjectGuid(HIGHGUID_PLAYER, guid), name))
                name = std::to_string(guid);
            return name;
        });
        if (say.empty() || !runplayer::MaySay(say, state->told, int64_t(state->toldAt), int64_t(now)))
            say.clear();
        else
        {
            state->told = say;
            state->toldAt = now;
        }
    }
    if (!say.empty() && sGuildBotMgr.GetPlayerBot(leader->GetGUIDLow()) == leader && leader->GetPlayerbotAI())
        leader->GetPlayerbotAI()->SayToParty(say);
}

void GuildRunStates::LeaderRefused(Player* leader, const GuildRunStamp& stamp)
{
    if (!leader || !leader->IsInWorld() || leader->IsBeingTeleported())
        return;
    std::shared_ptr<State> state = Find(leader->GetInstanceId());
    if (!state || state->leader != leader->GetGUIDLow())
        return;
    std::lock_guard<std::mutex> guard(state->lock);
    // Telemetry, as the keepers are: no plan revision, so the party's moves go on.
    if (!runrules::Accepts(state->core.mode, state->core.Stamp(), stamp, true) || state->pull != GuildRunPull::Approach
        || state->index >= state->route.size())
        return;
    if (state->budget.stop == state->index && state->budget.generation == state->core.generation && state->budget.since)
        return;
    const time_t now = time(nullptr);
    runrules::TrackBudget(state->budget, state->index, state->core.generation, true, int64_t(now));
    state->core.Note("run_budget", "stop=" + std::to_string(state->index + 1) + " from=refused");
}

void GuildRunStates::SayKeepers(State& state, const std::string& text)
{
    state.keepers = text;
    state.core.Note("run_keep", "stop=" + std::to_string(state.index + 1) + " keepers=" + (text.empty() ? "none" : text)
        + " budget=" + std::to_string(state.budget.since ? time(nullptr) - state.budget.since : 0));
}

void GuildRunStates::Pulled(Player* leader, const GuildRunStamp& stamp, ObjectGuid creature)
{
    Write(leader, stamp, "pulled", [creature](State& state, Map*, std::string&)
    {
        // A pull goes only through the gate, with nobody keeping it back: said before the pull, since the keepers' own
        // write, fired the same tick, is refused once the pull has revised the plan.
        if (!state.keepers.empty())
            SayKeepers(state, "");
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
            // A verified clear forgets the stop's blocks.
            if (state.packs && state.index < state.packs->stops.size())
                state.core.StopCleared(state.packs->stops[state.index].pack);
        }
        Reset(state, NextStop(state.packs.get(), state.route, map, skip ? state.index + 1 : state.index));
        return true;
    });
}

void GuildRunStates::Choose(Player* leader, const GuildRunStamp& stamp)
{
    Write(leader, stamp, "chosen", [](State& state, Map* map, std::string& fields)
    {
        if (!state.core.choose)
            return false;
        // By the run's order from the stop it is at, never by where the leader stands: in the shipped build a leader at stop
        // 75 chose stop 132 by its place, passing living packs. Only Prepare places a party by where it stands.
        const size_t from = state.index;
        const size_t start = NextStop(state.packs.get(), state.route, map, from);
        // Each stop passed, as the instance shows it now: its pack dead, or passed over by the controller, with why.
        std::string passed;
        for (size_t k = from; k < start && k < state.route.size(); ++k)
        {
            std::string why;
            if (!state.route[k].planned)
            {
                why = "skipped(" + state.route[k].skipped + ")";
                std::replace(why.begin(), why.end(), ' ', '_');
            }
            else
                why = state.packs && GuildPacks::StopAlive(*state.packs, map, k) ? "alive" : "dead";
            passed += (passed.empty() ? "" : ",") + std::to_string(k + 1) + ":" + why;
            // A stop passed as dead names its pack's spawns, so an export can check each against the instance's creatures.
            if (why == "dead" && state.packs && k < state.packs->stops.size())
            {
                std::string spawns;
                for (uint32 spawn : state.packs->stops[k].spawns)
                    spawns += (spawns.empty() ? "" : ",") + std::to_string(spawn);
                state.core.Note("run_passed", "stop=" + std::to_string(k + 1) + " spawns=" + (spawns.empty() ? "none" : spawns));
            }
        }
        fields = "from=" + std::to_string(from + 1) + " passed=" + (passed.empty() ? "none" : passed);
        Reset(state, start);
        state.core.choose = false;
        return true;
    });
}

std::string GuildRunStates::StallReason(Player* leader, size_t index, const GuildRunStop& stop, GuildRunPull pull, bool ownSpot)
{
    static const char* const pulls[] = { "approach", "gather", "pull", "wait", "blocked" };
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
        // By the one rule: the 3D distance between centres within its aggro radius toward the bot and ReachPad.
        const float reach = creature->GetAttackDistance(bot)
            + runguard::ReachPad(creature->GetObjectBoundingRadius(), bot->GetObjectBoundingRadius());
        const runrules::Vec pull = { stop.pull.getX(), stop.pull.getY(), stop.pull.getZ() };
        const runrules::Vec at = { creature->GetPositionX(), creature->GetPositionY(), creature->GetPositionZ() };
        if (runrules::Distance(pull, at) <= reach)
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
