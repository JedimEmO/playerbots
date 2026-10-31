#include "playerbot/playerbot.h"
#include "playerbot/GuildPacks.h"
#include "AI/EventAI/CreatureEventAI.h"
#include "AI/EventAI/CreatureEventAIMgr.h"
#include "Entities/Creature.h"
#include "Entities/CreatureLinkingMgr.h"
#include "Globals/ObjectMgr.h"
#include "Maps/Map.h"
#include "Maps/MapManager.h"
#include "Maps/MapPersistentStateMgr.h"
#include "Maps/SpawnGroupDefines.h"
#include "MotionGenerators/MoveMap.h"
#include "MotionGenerators/PathFinder.h"
#include "MotionGenerators/WaypointManager.h"
#include "Pools/PoolManager.h"
#include "playerbot/GuildDungeon.h"
#include "playerbot/TravelNode.h"
#include "Server/DBCStores.h"
#include "Server/SQLStorages.h"
#include "Spells/SpellMgr.h"
#include "World/World.h"
#include "vmap/VMapFactory.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <functional>
#include <limits>
#include <set>
#include <sstream>

// Where this runs: the console's and SOAP's commands are queued to the world and run in World::Update's
// ProcessCliCommands, after sMapMgr.Update has waited for the map threads; a GM's chat command runs in World::UpdateSessions,
// before the map update, as CMSG_MESSAGECHAT is a thread-unsafe opcode. Both are on the world thread with no map updating,
// which is what reading an instance's creatures, its line of sight and its navmesh query needs.

namespace
{
    // The cell size of the spatial hash, in yards.
    constexpr float CELL = 10.0f;
    // A prediction looks at creatures whose spawn is this near the spot, and at every patrol of the map.
    constexpr float PREDICT_REACH = 150.0f;
    // The pulled creature is the nearest one to the spot within this, as the dungeon job looks for its next pull.
    constexpr float PULL_LOOK = 60.0f;
    // A creature that moves within this of an idle one, in combat, brings it (CreatureCheckForHelpRadius).
    constexpr float RUNNER_REACH = 5.0f;
    // A called creature joins this many seconds after its caller (CreatureCheckForHelpAggroDelay).
    constexpr uint32 CALL_SECONDS = 2;
    // A player's bounding radius, added to a creature's aggro range as the core measures it.
    constexpr float PLAYER_BOUNDING = 0.4f;
    // Sight is checked between points this high above the feet.
    constexpr float EYE = 2.0f;
    // Base speeds of a creature with speed 1, in yards a second.
    constexpr float WALK_SPEED = 2.5f, RUN_SPEED = 7.0f;
    // A reply holds at most this many lines, so that it fits a SOAP answer.
    constexpr size_t MAX_LINES = 40;
    // A route is walked in points this far apart.
    constexpr float ROUTE_STEP = 2.0f;
    // A pack is on the way when one of it would attack a party this much nearer than its aggro range to the route,
    // as the dungeon job looks ahead for its next pull.
    constexpr float ROUTE_AHEAD = 15.0f;
    // A pull spot is this far from the nearest of its pack, back along the route, and the best is the middle.
    constexpr float PULL_NEAR = 20.0f, PULL_FAR = 30.0f, PULL_BEST = 25.0f;
    // At most this many spots are tried for one pack, and a route's predictions look at creatures this near a spot.
    constexpr size_t PULL_TRIES = 8;
    constexpr float ROUTE_REACH = 80.0f;
    // A rest spot is clear when every living creature's aggro range and every patrol's path is this much farther off.
    // It is looked for from this far back behind its pull, over the ground the pull cleared, up to the next pull's spot,
    // and one farther than REST_NEAR before that spot is noted as the nearest there is.
    constexpr float REST_MARGIN = 5.0f, REST_BACK = 60.0f, REST_NEAR = 40.0f;
    // A named character's own level is the route's when within this of the bosses' median.
    constexpr int32 ROUTE_LEVEL_NEAR = 5;
    // A stand point is taken where a navmesh path to it ends, when that is within this of it.
    constexpr float STAND_SLACK = 1.5f;
    // How many packs deep a route goes to clear a pull spot another creature reaches: that creature's pack is planned
    // first, and its own spot may need one more before it.
    constexpr int32 ROUTE_CLEAR_DEPTH = 2;
    // A pull spot or stand point is clear of a creature this far outside its aggro reach.
    constexpr float REACH_MARGIN = 3.0f;
    // A pack's and a spot's place along the way is looked for this far either side of where the stop is needed.
    constexpr float ANCHOR_WINDOW = 120.0f;
    // A party set down on a route counts a pull spot this far behind it along the way as still ahead: it can stand a
    // little past the point of the way its spot is placed by.
    constexpr float START_SLACK = 10.0f;

    int64 CellKey(float x, float y)
    {
        return (int64(std::floor(x / CELL)) << 32) ^ int64(uint32(int32(std::floor(y / CELL))));
    }

    // The distance from a point to a polyline, the yards along it to the nearest point, and that point.
    float DistanceToPath(const G3D::Vector3& p, const std::vector<G3D::Vector3>& path, float& along, G3D::Vector3& nearest)
    {
        float best = std::numeric_limits<float>::max(), walked = 0.0f;
        along = 0.0f;
        nearest = path.front();
        for (size_t i = 0; i + 1 < path.size(); ++i)
        {
            const G3D::Vector3 a = path[i], v = path[i + 1] - a;
            const float length = v.length();
            const float t = length > 0.0f ? std::max(0.0f, std::min(1.0f, (p - a).dot(v) / (length * length))) : 0.0f;
            const G3D::Vector3 q = a + v * t;
            const float d = (p - q).length();
            if (d < best)
            {
                best = d;
                along = walked + length * t;
                nearest = q;
            }
            walked += length;
        }
        if (path.size() == 1)
            best = (p - path.front()).length();
        return best;
    }

    // Sight between two points of an instance, as the core checks it: yes, no, or unknown when either end is in a grid
    // that is not loaded, where the vmap has no tree and would answer yes.
    enum class Sight { Yes, No, Unknown };

    Sight See(Map* map, const G3D::Vector3& a, const G3D::Vector3& b)
    {
        if (!map || !map->IsLoaded(a.x, a.y) || !map->IsLoaded(b.x, b.y))
            return Sight::Unknown;
        return map->IsInLineOfSight(a.x, a.y, a.z + EYE, b.x, b.y, b.z + EYE, false) ? Sight::Yes : Sight::No;
    }

    // The navmesh way from one point to another in an instance, as straight-path corners, or just the two points.
    std::vector<G3D::Vector3> NavPath(Map* map, const G3D::Vector3& from, const G3D::Vector3& to)
    {
        std::vector<G3D::Vector3> path = { from, to };
        if (!map)
            return path;
        dtNavMeshQuery const* query = MMAP::MMapFactory::createOrGetMMapManager()->GetNavMeshQuery(map->GetId(), map->GetInstanceId());
        if (!query)
            return path;
        dtQueryFilter filter;
        filter.setIncludeFlags(NAV_GROUND | NAV_WATER);
        filter.setExcludeFlags(NAV_MAGMA_SLIME);
        // Detour's points are (y, z, x).
        const float start[3] = { from.y, from.z, from.x }, end[3] = { to.y, to.z, to.x }, box[3] = { 3.0f, 5.0f, 3.0f };
        dtPolyRef startRef = 0, endRef = 0;
        float startAt[3], endAt[3];
        if (dtStatusFailed(query->findNearestPoly(start, box, &filter, &startRef, startAt)) || !startRef
            || dtStatusFailed(query->findNearestPoly(end, box, &filter, &endRef, endAt)) || !endRef)
            return path;
        dtPolyRef polys[128];
        int count = 0;
        if (dtStatusFailed(query->findPath(startRef, endRef, startAt, endAt, &filter, polys, &count, 128)) || !count)
            return path;
        float corners[32 * 3];
        int cornerCount = 0;
        if (dtStatusFailed(query->findStraightPath(startAt, endAt, polys, count, corners, nullptr, nullptr, &cornerCount, 32))
            || cornerCount < 2)
            return path;
        path.clear();
        for (int i = 0; i < cornerCount; ++i)
            path.emplace_back(corners[i * 3 + 2], corners[i * 3], corners[i * 3 + 1]);
        return path;
    }

    std::string CreatureName(uint32 entry)
    {
        CreatureInfo const* info = ObjectMgr::GetCreatureTemplate(entry);
        return info && info->Name ? info->Name : std::to_string(entry);
    }

    std::string Format(const char* format, ...)
    {
        char text[512];
        va_list args;
        va_start(args, format);
        vsnprintf(text, sizeof(text), format, args);
        va_end(args);
        return text;
    }
}

GuildPacks::MapModel& GuildPacks::Model(uint32 mapId)
{
    auto& model = models[mapId];
    if (!model)
    {
        model = std::make_unique<MapModel>();
        model->mapId = mapId;
        MapEntry const* entry = sMapStore.LookupEntry(mapId);
        model->name = entry && entry->name[0] ? entry->name[0] : "map " + std::to_string(mapId);
        Build(*model);
    }
    return *model;
}

void GuildPacks::Build(MapModel& model)
{
    const float familyRadius = sWorld.getConfig(CONFIG_FLOAT_CREATURE_FAMILY_ASSISTANCE_RADIUS);
    std::map<uint32, std::vector<uint32>> pooled;
    std::shared_ptr<SpawnGroupEntryContainer> spawnGroups = sObjectMgr.GetSpawnGroupContainer();
    auto collect = [&](CreatureDataPair const& pair) -> bool
    {
        CreatureData const& data = pair.second;
        if (data.mapid != model.mapId || !data.id)
            return false;
        CreatureInfo const* info = ObjectMgr::GetCreatureTemplate(data.id);
        if (!info || info->MaxLevel <= 1 || info->CreatureType == CREATURE_TYPE_CRITTER
            || (info->UnitFlags & (UNIT_FLAG_SPAWNING | UNIT_FLAG_IMMUNE_TO_PLAYER | UNIT_FLAG_UNINTERACTIBLE))
            || (info->ExtraFlags & CREATURE_EXTRA_FLAG_NO_AGGRO_ON_SIGHT))
            return false;
        // Only what can fight a party: not a faction friendly to players. A neutral one does not attack on sight, but in
        // an instance it answers a call for help like any other (Unit::CanAssistInCombatAgainst).
        FactionTemplateEntry const* faction = sFactionTemplateStore.LookupEntry(info->Faction);
        if (!faction || (faction->friendGroupMask & FACTION_GROUP_MASK_PLAYER))
            return false;

        Node node;
        node.spawn = pair.first;
        node.entry = data.id;
        node.at = G3D::Vector3(data.posX, data.posY, data.posZ);
        node.minLevel = info->MinLevel;
        node.maxLevel = info->MaxLevel;
        node.rank = info->Rank;
        node.detection = float(info->Detection);
        node.callRadius = info->CallForHelp > 0 ? float(info->CallForHelp) : familyRadius;
        node.spawnDist = data.spawndist;
        node.movement = data.movementType;
        node.walk = info->SpeedWalk * WALK_SPEED;
        node.run = info->SpeedRun * RUN_SPEED;
        node.caster = info->UnitClass == CLASS_MAGE || info->UnitClass == CLASS_PALADIN;
        node.aggroes = faction->IsHostileToPlayers();
        // One of several spawns of a pool or spawn group that puts up fewer than it lists, such as a rare's places.
        if (data.GuidPoolId)
            pooled[data.GuidPoolId].push_back(uint32(model.nodes.size()));
        if (data.EntryPoolId)
            pooled[data.EntryPoolId].push_back(uint32(model.nodes.size()));
        if (spawnGroups)
        {
            auto group = spawnGroups->spawnGroupByGuidMap.find({ pair.first, uint32(TYPEID_UNIT) });
            if (group != spawnGroups->spawnGroupByGuidMap.end() && group->second->MaxCount
                && group->second->MaxCount < group->second->DbGuids.size())
                node.maybe = true;
        }
        for (uint32 display : info->DisplayId)
            if (display)
            {
                if (CreatureModelInfo const* modelInfo = ObjectMgr::GetCreatureModelInfo(display))
                    node.bounding = modelInfo->bounding_radius * (info->Scale > 0.0f ? info->Scale : 1.0f);
                break;
            }
        if (node.movement == WAYPOINT_MOTION_TYPE)
            if (WaypointPath const* path = sWaypointMgr.GetDefaultPath(data.id, pair.first))
                for (auto const& [point, wp] : *path)
                    node.patrol.emplace_back(G3D::Vector3(wp.x, wp.y, wp.z), wp.delay);

        // The entry's EventAI scripts: summons, flight for help, and calls for help with a radius of their own.
        auto const& scripts = sEventAIMgr.GetCreatureEventEntryAIMap();
        if (scripts)
        {
            auto events = scripts->find(data.id);
            if (events != scripts->end())
                for (CreatureEventAI_Event const& event : events->second)
                    for (CreatureEventAI_Action const& action : event.action)
                    {
                        uint32 summoned = 0;
                        if (action.type == ACTION_T_SPAWN)
                            summoned = action.summon.creatureId;
                        else if (action.type == ACTION_T_SUMMON_ID)
                            summoned = action.summon_id.creatureId;
                        else if (action.type == ACTION_T_CAST)
                        {
                            if (SpellEntry const* spell = sSpellTemplate.LookupEntry<SpellEntry>(action.cast.spellId))
                                for (uint32 i = 0; i < MAX_EFFECT_INDEX; ++i)
                                    if (spell->Effect[i] == SPELL_EFFECT_SUMMON || spell->Effect[i] == SPELL_EFFECT_SUMMON_WILD
                                        || spell->Effect[i] == SPELL_EFFECT_SUMMON_GUARDIAN)
                                        summoned = uint32(spell->EffectMiscValue[i]);
                        }
                        else if (action.type == ACTION_T_FLEE_FOR_ASSIST)
                            node.flees = true;
                        else if (action.type == ACTION_T_CALL_FOR_HELP)
                            node.scriptCall = std::max(node.scriptCall, float(action.call_for_help.radius));
                        if (summoned)
                        {
                            node.summons.push_back(summoned);
                            if (event.event_type == EVENT_T_DEATH)
                                node.summonsOnDeath = true;
                        }
                    }
        }

        model.bySpawn[node.spawn] = uint32(model.nodes.size());
        model.cells[CellKey(node.at.x, node.at.y)].push_back(uint32(model.nodes.size()));
        model.nodes.push_back(std::move(node));
        return false;
    };
    sObjectMgr.DoCreatureData(collect);
    for (auto const& [pool, members] : pooled)
    {
        const uint32 limit = sPoolMgr.GetPoolTemplate(uint16(pool)).MaxLimit;
        if (limit && limit < members.size())
            for (uint32 i : members)
                model.nodes[i].maybe = true;
    }
    LinkAndGroup(model);
}

void GuildPacks::LinkAndGroup(MapModel& model)
{
    // Links: a slave's master by its guid, or the nearest creature of the master's entry within the link's range.
    for (uint32 i = 0; i < model.nodes.size(); ++i)
    {
        Node& node = model.nodes[i];
        CreatureLinkingInfo const* link = sCreatureLinkingMgr.GetLinkedTriggerInformation(node.entry, node.spawn, model.mapId);
        if (!link)
            continue;
        int32 master = -1;
        if (link->masterDBGuid)
        {
            auto found = model.bySpawn.find(link->masterDBGuid);
            if (found != model.bySpawn.end())
                master = int32(found->second);
        }
        else
        {
            float best = link->searchRange ? float(link->searchRange) : std::numeric_limits<float>::max();
            for (uint32 j = 0; j < model.nodes.size(); ++j)
                if (j != i && model.nodes[j].entry == link->masterId && (model.nodes[j].at - node.at).length() <= best)
                {
                    best = (model.nodes[j].at - node.at).length();
                    master = int32(j);
                }
        }
        if (master < 0)
            continue;
        node.master = master;
        node.fromMaster = link->linkingFlag & FLAG_AGGRO_ON_AGGRO;
        node.toMaster = link->linkingFlag & FLAG_TO_AGGRO_ON_AGGRO;
        node.follows = link->linkingFlag & FLAG_FOLLOW;
        model.nodes[master].slaves.push_back(i);
    }

    // Packs: the creatures a pull of any one of them brings by call for help, at spawn positions, and by aggro links.
    std::vector<uint32> parent(model.nodes.size());
    for (uint32 i = 0; i < parent.size(); ++i)
        parent[i] = i;
    std::function<uint32(uint32)> root = [&](uint32 i) { return parent[i] == i ? i : parent[i] = root(parent[i]); };
    auto join = [&](uint32 a, uint32 b) { parent[root(a)] = root(b); };
    for (uint32 i = 0; i < model.nodes.size(); ++i)
    {
        const Node& a = model.nodes[i];
        if (a.maybe)
            continue;
        // The search reaches past the largest bounding radius a creature here could have.
        const float reach = std::max(a.callRadius, a.scriptCall) + a.bounding + 8.0f;
        for (uint32 j : Near(model, a.at, reach))
            if (j != i && !model.nodes[j].maybe && (model.nodes[j].at - a.at).length() <= std::max(a.callRadius, a.scriptCall) + a.bounding + model.nodes[j].bounding)
                join(i, j);
        if (a.master >= 0 && (a.fromMaster || a.toMaster) && !model.nodes[a.master].maybe)
            join(i, uint32(a.master));
    }
    std::map<uint32, uint32> packOf;
    for (uint32 i = 0; i < model.nodes.size(); ++i)
    {
        if (model.nodes[i].maybe)
        {
            model.nodes[i].pack = std::numeric_limits<uint32>::max();
            model.maybe[CreatureName(model.nodes[i].entry)]++;
            continue;
        }
        auto found = packOf.find(root(i));
        if (found == packOf.end())
        {
            found = packOf.emplace(root(i), uint32(model.packs.size())).first;
            model.packs.emplace_back();
        }
        model.nodes[i].pack = found->second;
        Pack& pack = model.packs[found->second];
        pack.members.push_back(i);
        const Node& node = model.nodes[i];
        pack.elites += node.rank > 0 ? 1 : 0;
        pack.patrol = pack.patrol || node.movement == WAYPOINT_MOTION_TYPE;
        pack.linked = pack.linked || node.master >= 0;
        pack.summons = pack.summons || !node.summons.empty();
        pack.flees = pack.flees || node.flees;
        pack.aggroes = pack.aggroes || node.aggroes;
    }
    for (Pack& pack : model.packs)
    {
        G3D::Vector3 sum(0, 0, 0);
        for (uint32 i : pack.members)
            sum += model.nodes[i].at;
        pack.centre = sum / float(pack.members.size());
        for (uint32 i : pack.members)
            pack.radius = std::max(pack.radius, (model.nodes[i].at - pack.centre).length());
    }
}

std::vector<uint32> GuildPacks::Near(const MapModel& model, const G3D::Vector3& at, float radius) const
{
    std::vector<uint32> found;
    const int32 x0 = int32(std::floor((at.x - radius) / CELL)), x1 = int32(std::floor((at.x + radius) / CELL));
    const int32 y0 = int32(std::floor((at.y - radius) / CELL)), y1 = int32(std::floor((at.y + radius) / CELL));
    for (int32 x = x0; x <= x1; ++x)
        for (int32 y = y0; y <= y1; ++y)
        {
            auto cell = model.cells.find((int64(x) << 32) ^ int64(uint32(y)));
            if (cell == model.cells.end())
                continue;
            for (uint32 i : cell->second)
                if ((model.nodes[i].at - at).length() <= radius)
                    found.push_back(i);
        }
    return found;
}

float GuildPacks::Reach(const Node& node, uint32 level) const
{
    // Detection less the level difference, at least 5 yards, as Unit::GetAttackDistance has it, plus both bounding radii.
    const float creatureLevel = float(node.minLevel + node.maxLevel) / 2.0f;
    return std::max(5.0f, node.detection - (float(level) - creatureLevel)) + node.bounding + PLAYER_BOUNDING;
}

GuildPacks::Prediction GuildPacks::Predict(const MapModel& model, Map* map, const G3D::Vector3& spot, uint32 level,
    const PredictOptions& options) const
{
    Prediction result;
    result.live = map != nullptr;

    // The creatures in reach of the spot, and every patrol, where they are now in the instance: the dead and those
    // already fighting are left out, and one not loaded is taken at its spawn.
    std::map<uint32, G3D::Vector3> where;
    auto consider = [&](uint32 i)
    {
        if (where.count(i) || (options.dead && options.dead->count(i)))
            return;
        const Node& node = model.nodes[i];
        G3D::Vector3 at = node.at;
        // A spawn that may not be up counts only where the instance has it.
        if (node.maybe && !(map && map->GetCreature(node.spawn)))
            return;
        if (map)
        {
            // Killed and not yet back: the instance keeps its respawn time, also once the corpse is gone.
            if (MapPersistentState* state = map->GetPersistentState())
                if (state->GetCreatureRespawnTime(node.spawn) > time(nullptr))
                    return;
            if (Creature* creature = map->GetCreature(node.spawn))
            {
                if (!creature->IsAlive() || creature->IsInCombat())
                    return;
                at = G3D::Vector3(creature->GetPositionX(), creature->GetPositionY(), creature->GetPositionZ());
            }
            else
                result.notLoaded++;
        }
        where[i] = at;
    };
    for (uint32 i : Near(model, spot, options.reach))
        consider(i);
    for (uint32 i = 0; i < model.nodes.size(); ++i)
        if (!model.nodes[i].patrol.empty())
            for (auto const& point : model.nodes[i].patrol)
                if ((point.first - spot).length() <= options.reach)
                {
                    consider(i);
                    break;
                }

    // The pulled creature: the one given, or the nearest to the spot that attacks, as the dungeon job takes the next
    // creature in its way.
    if (options.pulled >= 0)
    {
        if (where.count(uint32(options.pulled)))
            result.pulled = options.pulled;
    }
    else
    {
        float nearest = PULL_LOOK;
        for (auto const& [i, at] : where)
            if (model.nodes[i].aggroes && (at - spot).length() < nearest)
            {
                nearest = (at - spot).length();
                result.pulled = int32(i);
            }
    }
    if (result.pulled < 0)
        return result;

    auto sees = [&](const G3D::Vector3& a, const G3D::Vector3& b)
    {
        const Sight sight = See(map, a, b);
        if (sight == Sight::Unknown)
            result.unknownSight++;
        return sight != Sight::No;
    };
    auto reach = [&](const Node& node) { return Reach(node, level); };

    std::map<uint32, Joined> joined;
    std::vector<uint32> queue;
    auto add = [&](uint32 i, const std::string& source, uint32 second)
    {
        if (joined.count(i) || !where.count(i))
            return;
        joined[i] = { i, where[i], source, second };
        queue.push_back(i);
    };
    add(uint32(result.pulled), "pulled", 0);

    std::set<uint32> ran;
    bool changed = true;
    while (changed)
    {
        changed = false;
        // Calls for help, chained, and aggro links, both ways as their flags say.
        while (!queue.empty())
        {
            const uint32 a = queue.back();
            queue.pop_back();
            const Node& caller = model.nodes[a];
            const Joined from = joined[a];
            const float radius = std::max(caller.callRadius, caller.scriptCall);
            for (auto const& [b, at] : where)
                if (!joined.count(b) && (at - from.at).length() <= radius + caller.bounding + model.nodes[b].bounding
                    && sees(from.at, at))
                    add(b, a == uint32(result.pulled) ? "call" : "chain", from.second + CALL_SECONDS);
            for (uint32 slave : caller.slaves)
                if (model.nodes[slave].fromMaster)
                    add(slave, "linked", from.second);
            if (caller.master >= 0 && caller.toMaster)
                add(uint32(caller.master), "linked", from.second);
        }
        // Runners: each joined creature runs to the spot, by a straight line it can see or the navmesh, and brings what
        // it passes within reach of.
        for (auto const& [a, from] : std::map<uint32, Joined>(joined))
        {
            if (!ran.insert(a).second)
                continue;
            const Node& runner = model.nodes[a];
            std::vector<G3D::Vector3> path = { from.at, spot };
            if (See(map, from.at, spot) == Sight::No)
                path = NavPath(map, from.at, spot);
            for (auto const& [b, at] : where)
            {
                if (joined.count(b))
                    continue;
                float along;
                G3D::Vector3 passing;
                if (DistanceToPath(at, path, along, passing) <= RUNNER_REACH + runner.bounding + model.nodes[b].bounding
                    && sees(passing, at))
                    add(b, "runner", from.second + uint32(along / std::max(runner.run, 1.0f)));
            }
        }
        // Aggro at the spot, from where each creature that attacks on sight stands now.
        for (auto const& [b, at] : where)
            if (!joined.count(b) && model.nodes[b].aggroes && (at - spot).length() <= reach(model.nodes[b]) && sees(at, spot))
                add(b, "sight", 0);
        changed = !queue.empty();
    }
    for (auto const& [i, j] : joined)
        result.pull.push_back(j);

    // Late reach: wanderers whose range comes within their aggro of the spot, and patrols whose path does, with the
    // earliest second, from where they are now; and what follows them.
    for (auto const& [b, at] : where)
    {
        if (joined.count(b) || !model.nodes[b].aggroes)
            continue;
        const Node& node = model.nodes[b];
        const float r = reach(node);
        if (node.movement == RANDOM_MOTION_TYPE && (node.at - spot).length() - node.spawnDist <= r)
            result.late.push_back({ b, at, "wanderer", uint32(std::max(0.0f, (at - spot).length() - r) / std::max(node.walk, 0.5f)) });
        else if (node.movement == WAYPOINT_MOTION_TYPE && !node.patrol.empty())
        {
            // From the nearest point of its path, on along it once round.
            size_t start = 0;
            for (size_t k = 0; k < node.patrol.size(); ++k)
                if ((node.patrol[k].first - at).length() < (node.patrol[start].first - at).length())
                    start = k;
            float seconds = (node.patrol[start].first - at).length() / std::max(node.walk, 0.5f);
            for (size_t step = 0; step < node.patrol.size(); ++step)
            {
                const size_t k = (start + step) % node.patrol.size();
                if (step)
                    seconds += (node.patrol[k].first - node.patrol[(k + node.patrol.size() - 1) % node.patrol.size()].first).length()
                        / std::max(node.walk, 0.5f);
                if ((node.patrol[k].first - spot).length() <= r)
                {
                    result.late.push_back({ b, at, "patrol", uint32(seconds) });
                    for (uint32 slave : node.slaves)
                        if (model.nodes[slave].follows && !joined.count(slave) && where.count(slave))
                            result.late.push_back({ slave, where[slave], "with its patrol", uint32(seconds) });
                    break;
                }
                seconds += node.patrol[k].second / 1000.0f;
            }
        }
    }
    std::sort(result.late.begin(), result.late.end(), [](const Joined& a, const Joined& b) { return a.second < b.second; });
    return result;
}

std::list<std::string> GuildPacks::ListPacks(const MapModel& model, Map* map) const
{
    std::vector<uint32> order(model.packs.size());
    for (uint32 i = 0; i < order.size(); ++i)
        order[i] = i;
    std::sort(order.begin(), order.end(), [&](uint32 a, uint32 b)
        { return model.packs[a].members.size() != model.packs[b].members.size()
            ? model.packs[a].members.size() > model.packs[b].members.size() : a < b; });

    std::list<std::string> out;
    out.push_back(Format("%s (%u): %u creatures in %u packs, largest first%s", model.name.c_str(), model.mapId,
        uint32(model.nodes.size()), uint32(model.packs.size()), map ? Format(", alive in instance %u", map->GetInstanceId()).c_str() : ""));
    for (uint32 p : order)
    {
        if (out.size() >= MAX_LINES)
        {
            out.push_back(Format("... %u more packs not shown; give a spot to see one pull", uint32(order.size() - (out.size() - 1))));
            break;
        }
        const Pack& pack = model.packs[p];
        std::map<std::string, uint32> names;
        uint32 alive = 0, minLevel = 255, maxLevel = 0;
        for (uint32 i : pack.members)
        {
            const Node& node = model.nodes[i];
            names[CreatureName(node.entry)]++;
            minLevel = std::min(minLevel, node.minLevel);
            maxLevel = std::max(maxLevel, node.maxLevel);
            if (map)
            {
                MapPersistentState* state = map->GetPersistentState();
                Creature* creature = map->GetCreature(node.spawn);
                if (!(state && state->GetCreatureRespawnTime(node.spawn) > time(nullptr)) && (!creature || creature->IsAlive()))
                    ++alive;
            }
        }
        std::ostringstream line;
        line << "pack " << p << ": " << pack.members.size();
        if (map)
            line << " (" << alive << " alive)";
        line << ", " << pack.elites << " elite, L" << minLevel << "-" << maxLevel << ", ("
             << int32(pack.centre.x) << " " << int32(pack.centre.y) << " " << int32(pack.centre.z) << ") r" << int32(pack.radius) << ": ";
        bool first = true;
        for (auto const& [name, count] : names)
        {
            line << (first ? "" : ", ") << name << (count > 1 ? " x" + std::to_string(count) : "");
            first = false;
        }
        if (pack.patrol) line << "; patrol";
        if (pack.linked) line << "; linked";
        if (pack.summons) line << "; summons";
        if (pack.flees) line << "; flees";
        if (!pack.aggroes) line << "; neutral";
        out.push_back(line.str().substr(0, 240));
    }
    if (!model.maybe.empty())
    {
        std::ostringstream line;
        line << "may not be up (pooled or a spawn group's choice), left out of the packs: ";
        bool first = true;
        for (auto const& [name, count] : model.maybe)
        {
            line << (first ? "" : ", ") << name << (count > 1 ? " x" + std::to_string(count) : "");
            first = false;
        }
        out.push_back(line.str().substr(0, 240));
    }
    return out;
}

std::list<std::string> GuildPacks::ShowPrediction(const MapModel& model, Map* map, const G3D::Vector3& spot, uint32 level) const
{
    std::list<std::string> out;
    const Prediction prediction = Predict(model, map, spot, level, PredictOptions());
    if (prediction.pulled < 0)
    {
        out.push_back(Format("%s (%u): no creature within %.0f yards of (%.1f %.1f %.1f)", model.name.c_str(), model.mapId,
            PULL_LOOK, spot.x, spot.y, spot.z));
        return out;
    }
    const Node& pulled = model.nodes[prediction.pulled];
    uint32 elites = 0;
    for (const Joined& j : prediction.pull)
        elites += model.nodes[j.node].rank > 0 ? 1 : 0;
    out.push_back(Format("%s (%u), %s, level %u at (%.1f %.1f %.1f): pulls %s %u, brings %u (%u elite); %u wanderers or patrols "
        "could come later; %u sight checks unknown%s", model.name.c_str(), model.mapId,
        map ? Format("instance %u", map->GetInstanceId()).c_str() : "spawns only", level, spot.x, spot.y, spot.z,
        CreatureName(pulled.entry).c_str(), pulled.spawn, uint32(prediction.pull.size()), elites, uint32(prediction.late.size()),
        prediction.unknownSight, prediction.notLoaded ? Format(", %u not loaded", prediction.notLoaded).c_str() : ""));

    std::vector<Joined> pull = prediction.pull;
    std::sort(pull.begin(), pull.end(), [](const Joined& a, const Joined& b) { return a.second != b.second ? a.second < b.second : a.node < b.node; });
    size_t total = pull.size() + prediction.late.size(), shown = 0;
    for (const Joined& j : pull)
    {
        if (out.size() >= MAX_LINES)
            break;
        const Node& node = model.nodes[j.node];
        std::string extra;
        for (uint32 summoned : node.summons)
            extra += (extra.empty() ? "; summons " : ", ") + CreatureName(summoned) + (node.summonsOnDeath ? " on death" : "");
        if (node.flees)
            extra += "; flees for help";
        out.push_back(Format("%s %u %s %us %.0f yd%s%s", CreatureName(node.entry).c_str(), node.spawn, j.source.c_str(), j.second,
            (j.at - spot).length(), node.rank > 0 ? " elite" : "", extra.c_str()));
        ++shown;
    }
    if (!prediction.late.empty() && out.size() < MAX_LINES)
        out.push_back("later:");
    for (const Joined& j : prediction.late)
    {
        if (out.size() >= MAX_LINES)
            break;
        const Node& node = model.nodes[j.node];
        out.push_back(Format("  %s %u %s from %us, %.0f yd now%s", CreatureName(node.entry).c_str(), node.spawn, j.source.c_str(),
            j.second, (j.at - spot).length(), node.rank > 0 ? " elite" : ""));
        ++shown;
    }
    if (shown < total)
        out.push_back(Format("... %u more not shown", uint32(total - shown)));
    return out;
}


std::vector<G3D::Vector3> GuildPacks::StandPoints(const MapModel& model, const G3D::Vector3& spot, const G3D::Vector3& pack,
    const std::function<bool(const G3D::Vector3&)>& clear) const
{
    // Behind the pull spot, away from the pack: two melee places either side near it, then the ranged one's and the healer's
    // farther back. Each is taken where a navmesh path from the spot ends, if it ends near it and the spot is in sight.
    struct Place { float yards; float degrees; };
    static const Place places[4] = { { 4.0f, -40.0f }, { 4.5f, 40.0f }, { 7.0f, -15.0f }, { 8.0f, 15.0f } };
    G3D::Vector3 away = spot - pack;
    away.z = 0.0f;
    if (away.length() < 0.5f)
        away = G3D::Vector3(1.0f, 0.0f, 0.0f);
    away = away / away.length();
    const ai::WorldPosition from(model.mapId, spot.x, spot.y, spot.z);
    std::vector<G3D::Vector3> points;
    for (const Place& place : places)
        for (float turn : { 0.0f, 25.0f, -25.0f })
        {
            const float angle = (place.degrees + turn) * float(M_PI) / 180.0f;
            const G3D::Vector3 direction(away.x * std::cos(angle) - away.y * std::sin(angle),
                away.x * std::sin(angle) + away.y * std::cos(angle), 0.0f);
            const G3D::Vector3 wanted = spot + direction * place.yards;
            const ai::WorldPosition target(model.mapId, wanted.x, wanted.y, wanted.z);
            const std::vector<ai::WorldPosition> path = from.getPathTo(target, nullptr);
            if (!target.isPathTo(path, STAND_SLACK))
                continue;
            const G3D::Vector3 at(path.back().getX(), path.back().getY(), path.back().getZ());
            if (!clear(at) || !VMAP::VMapFactory::createOrGetVMapManager()->isInLineOfSight(model.mapId, at.x, at.y, at.z + EYE,
                spot.x, spot.y, spot.z + EYE, false))
                continue;
            points.push_back(at);
            break;
        }
    return points;
}

std::shared_ptr<GuildPacks::Route> GuildPacks::BuildRoute(const MapModel& model, const std::string& name, uint32 level,
    const std::string& levelFrom, const std::vector<std::pair<int32, G3D::Vector3>>& stops) const
{
    auto result = std::make_shared<Route>();
    Route& r = *result;
    r.dungeon = name;
    r.mapId = model.mapId;
    r.level = level;
    r.levelFrom = levelFrom;
    if (!stops.empty())
        r.arrival = stops.front().second;
    for (auto const& stop : stops)
        if (stop.first)
            r.bosses.push_back(stop);

    // The way, leg by leg from the arrival through each boss, by the travel map's routes and the navmesh paths it joins,
    // as the job's travel takes it; a leg the travel map has no way for is a straight line, and counted.
    std::vector<G3D::Vector3> way;
    for (size_t k = 1; k < stops.size(); ++k)
    {
        const G3D::Vector3 a = stops[k - 1].second, b = stops[k].second;
        ai::TravelPath travel = ai::TravelNodeMap::getFullPath(ai::WorldPosition(model.mapId, a.x, a.y, a.z),
            ai::WorldPosition(model.mapId, b.x, b.y, b.z), nullptr);
        std::vector<G3D::Vector3> leg;
        for (const ai::WorldPosition& point : travel.getPointPath())
            if (point.getMapId() == model.mapId)
                leg.emplace_back(point.getX(), point.getY(), point.getZ());
        if (leg.size() < 2)
        {
            leg = { a, b };
            ++r.straightLegs;
        }
        way.insert(way.end(), way.empty() ? leg.begin() : leg.begin() + 1, leg.end());
    }
    // In points exactly ROUTE_STEP apart along it, so that point i is i * ROUTE_STEP yards along and a spot back along
    // it is a point of it. The way's own points can be nearer each other than that.
    std::vector<G3D::Vector3>& route = r.points;
    if (!way.empty())
        route.push_back(way.front());
    float carried = 0.0f;   // yards walked since the last point taken
    for (size_t i = 1; i < way.size(); ++i)
    {
        const G3D::Vector3 a = way[i - 1], b = way[i];
        const float length = (b - a).length();
        float t = ROUTE_STEP - carried;
        for (; t <= length; t += ROUTE_STEP)
            route.push_back(a + (b - a) * (t / length));
        carried = length - (t - ROUTE_STEP);
    }
    if (!way.empty() && (route.back() - way.back()).length() > 0.1f)
        route.push_back(way.back());
    for (size_t i = 1; i < route.size(); ++i)
        r.length += (route[i] - route[i - 1]).length();
    if (route.size() < 2)
        return result;

    // The packs on the way, in the order the route meets them, by the point of the route nearest any of them.
    struct OnWay { float along; uint32 pack; size_t index; };
    std::vector<OnWay> onWay;
    for (uint32 p = 0; p < model.packs.size(); ++p)
    {
        const Pack& pack = model.packs[p];
        if (!pack.aggroes)
            continue;
        float bestAlong = -1.0f, bestDistance = std::numeric_limits<float>::max();
        for (uint32 i : pack.members)
        {
            const Node& node = model.nodes[i];
            if (!node.aggroes)
                continue;
            float along;
            G3D::Vector3 nearest;
            const float d = DistanceToPath(node.at, route, along, nearest);
            if (d <= Reach(node, level) + ROUTE_AHEAD && d < bestDistance)
            {
                bestDistance = d;
                bestAlong = along;
            }
        }
        if (bestAlong >= 0.0f)
            onWay.push_back({ bestAlong, p, std::min(route.size() - 1, size_t(bestAlong / ROUTE_STEP + 0.5f)) });
    }
    std::sort(onWay.begin(), onWay.end(), [](const OnWay& a, const OnWay& b) { return a.along < b.along; });

    std::set<int32> bossEntries;
    for (auto const& [entry, at] : r.bosses)
        bossEntries.insert(entry);

    // Each pack in turn, with those before it dead: the spot whose approach pulls nothing else, then whose pull brings
    // least else, with the latest late arrival, nearest the middle distance. A spot is reached by a creature outside the
    // pack when its aggro range covers the spot, wandering included; walking there pulls it. When every spot is reached,
    // the nearest such creature's pack is planned first as a stop of its own, and this one again with it dead.
    std::vector<uint32> patrols;
    for (uint32 i = 0; i < model.nodes.size(); ++i)
        if (!model.nodes[i].patrol.empty() && model.nodes[i].aggroes && !model.nodes[i].maybe)
            patrols.push_back(i);
    std::set<uint32> dead;
    std::vector<size_t> pullSpots;              // each stop's pull spot's point on the way
    std::vector<std::set<uint32>> deadAfter;    // what is dead once each stop's pull is done

    // The living creatures that reach a spot, by kind. Strict: one standing still within its aggro reach in 3D and
    // REACH_MARGIN, another pack's wanderer whose spawn is that near, and any of the pack's own, a wanderer of it from
    // anywhere in its range; walking there pulls them. Another pack's wanderer from the rest of its spawn plus half its
    // range, no margin, and another pack's patrol from any point of its path: those move, and are waited for.
    struct Reached { std::vector<uint32> strict, wander, patrol; };
    std::map<uint32, std::vector<G3D::Vector3>> patrolPaths;   // each patrol's path as a closed loop
    for (uint32 i : patrols)
    {
        std::vector<G3D::Vector3>& path = patrolPaths[i];
        for (auto const& point : model.nodes[i].patrol)
            path.push_back(point.first);
        path.push_back(path.front());
    }
    auto reachOf = [&](const G3D::Vector3& spot, const std::set<uint32>& own)
    {
        Reached found;
        for (uint32 i : Near(model, spot, ROUTE_REACH / 2.0f))
        {
            const Node& node = model.nodes[i];
            if (dead.count(i) || node.maybe || !node.aggroes)
                continue;
            const float d = (node.at - spot).length(), reach = Reach(node, level);
            if (own.count(i))
            {
                if (d <= reach + REACH_MARGIN + (node.movement == RANDOM_MOTION_TYPE ? node.spawnDist : 0.0f))
                    found.strict.push_back(i);
            }
            else if (node.movement == RANDOM_MOTION_TYPE)
            {
                // At its spawn it reaches the spot as one standing still would; only the rest of its range is waited for.
                if (d <= reach + REACH_MARGIN)
                    found.strict.push_back(i);
                else if (d <= reach + node.spawnDist / 2.0f)
                    found.wander.push_back(i);
            }
            else if (node.movement != WAYPOINT_MOTION_TYPE && d <= reach + REACH_MARGIN)
                found.strict.push_back(i);
        }
        for (uint32 i : patrols)
        {
            if (dead.count(i) || own.count(i))
                continue;
            float along;
            G3D::Vector3 nearest;
            if (DistanceToPath(spot, patrolPaths[i], along, nearest) <= Reach(model.nodes[i], level) + REACH_MARGIN)
                found.patrol.push_back(i);
        }
        return found;
    };
    // The way from the stop before's pull spot to a pull spot: the points of the route between them, either way round,
    // and to an off-way spot the straight line from its point of the route. What reaches any of it, by the same kinds,
    // except the pack's own, whose reach of the last stretch the spot's own test covers; one of the pack's own that
    // reaches the way earlier is strict, since walking past it pulls it.
    auto legOf = [&](size_t to, const G3D::Vector3& spot, bool offWay, const std::set<uint32>& own)
    {
        Reached found;
        if (pullSpots.empty())
            return found;
        const size_t from = pullSpots.back();
        std::vector<G3D::Vector3> points;
        for (size_t j = std::min(from, to); j <= std::max(from, to) && j < route.size(); ++j)
            points.push_back(route[j]);
        if (offWay)
        {
            const G3D::Vector3 a = route[to];
            const float length = (spot - a).length();
            for (float t = ROUTE_STEP; t < length; t += ROUTE_STEP)
                points.push_back(a + (spot - a) * (t / length));
        }
        std::set<uint32> strict, wander, patrol;
        for (const G3D::Vector3& at : points)
        {
            const Reached here = reachOf(at, std::set<uint32>());
            for (uint32 i : here.strict)
                if (!own.count(i) || (at - spot).length() > PULL_FAR)
                    strict.insert(i);
            for (uint32 i : here.wander)
                if (!own.count(i))
                    wander.insert(i);
            for (uint32 i : here.patrol)
                if (!own.count(i))
                    patrol.insert(i);
        }
        found.strict.assign(strict.begin(), strict.end());
        for (uint32 i : wander)
            if (!strict.count(i))
                found.wander.push_back(i);
        for (uint32 i : patrol)
            if (!strict.count(i))
                found.patrol.push_back(i);
        return found;
    };
    // The point of the way nearest a point, among those within ANCHOR_WINDOW of the point of the way where it is needed:
    // the way can pass one place twice, as the Deadmines' passes its mine, and the pass that matters is the near one.
    auto nearestPoint = [&](const G3D::Vector3& at, size_t hint)
    {
        const size_t window = size_t(ANCHOR_WINDOW / ROUTE_STEP);
        const size_t from = hint > window ? hint - window : 0, to = std::min(route.size() - 1, hint + window);
        size_t best = from;
        for (size_t j = from; j <= to; ++j)
            if ((route[j] - at).squaredLength() < (route[best] - at).squaredLength())
                best = j;
        return best;
    };
    // The point of the way nearest a pack, by any of its living members that attack, near the point where it is needed.
    auto anchorOf = [&](const Pack& pack, size_t hint)
    {
        size_t best = hint;
        float bestDistance = std::numeric_limits<float>::max();
        for (uint32 i : pack.members)
        {
            if (dead.count(i) || !model.nodes[i].aggroes)
                continue;
            const size_t j = nearestPoint(model.nodes[i].at, hint);
            if ((route[j] - model.nodes[i].at).length() < bestDistance)
            {
                bestDistance = (route[j] - model.nodes[i].at).length();
                best = j;
            }
        }
        return std::make_pair(float(best) * ROUTE_STEP, best);
    };

    // The packs whose plan is under way, so that clearing never comes back round to one of them: planning A can clear B
    // first, and B's own spot may be reached by A.
    std::set<uint32> planning;
    std::function<bool(uint32, int32, size_t)> plan = [&](uint32 packId, int32 depth, size_t hint) -> bool
    {
        const Pack& pack = model.packs[packId];
        auto gone = [&]()
        {
            return std::all_of(pack.members.begin(), pack.members.end(),
                [&](uint32 i) { return dead.count(i) > 0 || !model.nodes[i].aggroes; });
        };
        if (gone() || planning.count(packId))
            return false;
        planning.insert(packId);
        struct Done { std::set<uint32>& planning; uint32 packId; ~Done() { planning.erase(packId); } } done{ planning, packId };
        const std::set<uint32> own(pack.members.begin(), pack.members.end());
        auto distanceToPack = [&](const G3D::Vector3& q, int32& nearestMember)
        {
            float best = std::numeric_limits<float>::max();
            for (uint32 i : pack.members)
                if (!dead.count(i) && model.nodes[i].aggroes && (model.nodes[i].at - q).length() < best)
                {
                    best = (model.nodes[i].at - q).length();
                    nearestMember = int32(i);
                }
            return best;
        };

        struct Choice { G3D::Vector3 spot; size_t index = 0; bool offWay = false; int32 member = -1; Prediction prediction;
                        std::vector<uint32> extras; Reached reached, leg; uint32 lateFirst = 0; float distance = 0.0f; };
        Choice best;
        bool have = false;
        bool clearing = true;   // false once clearing another pack first has failed: the next round only chooses
        for (int32 round = 0; round <= ROUTE_CLEAR_DEPTH; ++round)
        {
            const auto [along, anchor] = anchorOf(pack, hint);
            have = false;
            auto weigh = [&](const G3D::Vector3& spot, size_t index, bool offWay, int32 member)
            {
                Choice c;
                c.spot = spot;
                c.index = index;
                c.offWay = offWay;
                c.member = member;
                c.distance = (model.nodes[member].at - spot).length();
                c.reached = reachOf(spot, own);
                PredictOptions options;
                options.reach = ROUTE_REACH;
                options.pulled = member;
                options.dead = &dead;
                c.prediction = Predict(model, nullptr, spot, level, options);
                for (const Joined& joined : c.prediction.pull)
                    if (!own.count(joined.node))
                        c.extras.push_back(joined.node);
                c.lateFirst = c.prediction.late.empty() ? std::numeric_limits<uint32>::max() : c.prediction.late.front().second;
                // Fewest that would be pulled by walking there, then fewest wanderers to wait for, then the rest as before.
                const auto rank = [](const Choice& x) { return std::make_pair(x.reached.strict.size(), x.reached.wander.size()); };
                const bool better = !have || rank(c) < rank(best)
                    || (rank(c) == rank(best) && (c.extras.size() < best.extras.size()
                        || (c.extras.size() == best.extras.size() && (c.lateFirst > best.lateFirst
                            || (c.lateFirst == best.lateFirst && std::fabs(c.distance - PULL_BEST) < std::fabs(best.distance - PULL_BEST))))));
                if (better)
                {
                    best = c;
                    have = true;
                }
            };

            // On the way: points PULL_NEAR to PULL_FAR back from the pack, or the one nearest PULL_BEST.
            std::vector<size_t> tries;
            const size_t back = size_t((PULL_FAR + ROUTE_AHEAD * 2) / ROUTE_STEP);
            size_t fallback = anchor;
            float fallbackOff = std::numeric_limits<float>::max();
            for (size_t step = 0; step <= back && step <= anchor && tries.size() < PULL_TRIES; ++step)
            {
                const size_t j = anchor - step;
                int32 member = -1;
                const float d = distanceToPack(route[j], member);
                if (d >= PULL_NEAR && d <= PULL_FAR)
                    tries.push_back(j);
                else if (d >= PULL_NEAR / 2.0f && std::fabs(d - PULL_BEST) < fallbackOff)
                {
                    fallbackOff = std::fabs(d - PULL_BEST);
                    fallback = j;
                }
            }
            if (tries.empty())
                tries.push_back(fallback);
            for (size_t j : tries)
            {
                int32 member = -1;
                distanceToPack(route[j], member);
                if (member >= 0)
                    weigh(route[j], j, false, member);
            }

            // Off the way, when the way passes no nearer than PULL_FAR or every spot on it is reached: PULL_BEST from the
            // nearest of the pack, toward the way and turned either side, on the navmesh by a path from the way, and in
            // sight of the creature.
            if (have && (best.distance > PULL_FAR + ROUTE_STEP || !best.reached.strict.empty() || !best.reached.wander.empty()))
            {
                int32 member = -1;
                distanceToPack(route[anchor], member);
                if (member >= 0)
                {
                    const G3D::Vector3 from = model.nodes[member].at, toward = route[anchor];
                    G3D::Vector3 heading = toward - from;
                    heading.z = 0.0f;
                    if (heading.length() < 1.0f)
                        heading = G3D::Vector3(1.0f, 0.0f, 0.0f);
                    heading = heading / heading.length();
                    const ai::WorldPosition onWayPoint(model.mapId, toward.x, toward.y, toward.z);
                    for (float degrees : { 0.0f, 35.0f, -35.0f, 70.0f, -70.0f })
                    {
                        const float angle = degrees * float(M_PI) / 180.0f;
                        const G3D::Vector3 direction(heading.x * std::cos(angle) - heading.y * std::sin(angle),
                            heading.x * std::sin(angle) + heading.y * std::cos(angle), 0.0f);
                        const G3D::Vector3 wanted = from + direction * PULL_BEST;
                        const ai::WorldPosition target(model.mapId, wanted.x, wanted.y, toward.z);
                        const std::vector<ai::WorldPosition> path = onWayPoint.getPathTo(target, nullptr);
                        if (!target.isPathTo(path, 3.0f))
                            continue;
                        const G3D::Vector3 spot(path.back().getX(), path.back().getY(), path.back().getZ());
                        if (VMAP::VMapFactory::createOrGetVMapManager()->isInLineOfSight(model.mapId, spot.x, spot.y, spot.z + EYE,
                            from.x, from.y, from.z + EYE, false))
                            weigh(spot, anchor, true, member);
                    }
                }
            }
            if (have)
                best.leg = legOf(best.index, best.spot, best.offWay, own);
            if (!have || !clearing || (best.reached.strict.empty() && best.leg.strict.empty()) || depth >= ROUTE_CLEAR_DEPTH
                || round == ROUTE_CLEAR_DEPTH)
                break;
            // Every spot is reached, or the way to the best one is: the nearest creature of another pack standing still
            // within reach of the spot, else of the way, is pulled first, from its own pack's spot. The pack's own reach
            // only rules a spot out; it is no stop of its own.
            int32 first = -1;
            for (const std::vector<uint32>* strict : { &best.reached.strict, &best.leg.strict })
            {
                for (uint32 i : *strict)
                    if (!own.count(i) && (first < 0 || (model.nodes[i].at - best.spot).length() < (model.nodes[first].at - best.spot).length()))
                        first = int32(i);
                if (first >= 0)
                    break;
            }
            if (first < 0)
                break;
            const uint32 otherPack = model.nodes[first].pack;
            if (otherPack == packId || otherPack >= model.packs.size() || planning.count(otherPack))
                break;
            // Whatever the other plan did, the choice is chosen again: it may have killed this pack's creatures, or some of them.
            const size_t stopsBefore = r.stops.size();
            if (plan(otherPack, depth + 1, best.index) && r.stops.size() > stopsBefore && r.stops.back().pack == otherPack)
                r.stops.back().clearsWay = true;
            else
                clearing = false;
            if (gone())
                return false;
        }
        if (!have)
            return false;

        Stop stop;
        stop.pack = packId;
        for (uint32 i : pack.members)
            if (!dead.count(i))
            {
                stop.spawns.push_back(model.nodes[i].spawn);
                stop.boss = stop.boss || bossEntries.count(int32(model.nodes[i].entry));
            }
        // Never an empty stop: a pack whose creatures died in the pulls planned before it is no stop.
        if (stop.spawns.empty())
            return false;
        stop.pulled = model.nodes[best.member].spawn;
        stop.along = anchorOf(pack, hint).first;
        stop.pullSpot = best.spot;
        // An off-way spot is placed by the point of the way nearest it, near where it is needed.
        stop.pullAlong = float(best.offWay ? nearestPoint(best.spot, best.index) : best.index) * ROUTE_STEP;
        stop.offWay = best.offWay;
        stop.pullDistance = best.distance;
        stop.clean = best.extras.empty();
        for (uint32 i : best.extras)
            stop.extras.push_back(model.nodes[i].spawn);
        stop.spotClear = best.reached.strict.empty();
        for (uint32 i : best.reached.strict)
        {
            stop.reachedBy.push_back(model.nodes[i].spawn);
            const bool ownPack = own.count(i) > 0;
            const ReachKind kind = model.nodes[i].movement == RANDOM_MOTION_TYPE ? ReachKind::Wanderer
                : model.nodes[i].movement == WAYPOINT_MOTION_TYPE ? ReachKind::Patrol : ReachKind::Static;
            stop.reachers.push_back({ model.nodes[i].spawn, kind, ownPack });
        }
        for (uint32 i : best.reached.wander)
            stop.reachers.push_back({ model.nodes[i].spawn, ReachKind::Wanderer, false });
        for (uint32 i : best.reached.patrol)
            stop.reachers.push_back({ model.nodes[i].spawn, ReachKind::Patrol, false });
        // The way there: what stands on it, and the wanderers and patrols to wait for before walking it.
        const Reached leg = legOf(best.index, best.spot, best.offWay, own);
        stop.legClear = leg.strict.empty();
        std::set<uint32> listed(stop.reachedBy.begin(), stop.reachedBy.end());
        for (const Reacher& reacher : stop.reachers)
            listed.insert(reacher.spawn);
        for (uint32 i : leg.strict)
        {
            stop.legReachedBy.push_back(model.nodes[i].spawn);
            const ReachKind kind = model.nodes[i].movement == RANDOM_MOTION_TYPE ? ReachKind::Wanderer
                : model.nodes[i].movement == WAYPOINT_MOTION_TYPE ? ReachKind::Patrol : ReachKind::Static;
            if (listed.insert(model.nodes[i].spawn).second)
                stop.reachers.push_back({ model.nodes[i].spawn, kind, own.count(i) > 0, true });
        }
        for (uint32 i : leg.wander)
            if (listed.insert(model.nodes[i].spawn).second)
                stop.reachers.push_back({ model.nodes[i].spawn, ReachKind::Wanderer, false, true });
        for (uint32 i : leg.patrol)
            if (listed.insert(model.nodes[i].spawn).second)
                stop.reachers.push_back({ model.nodes[i].spawn, ReachKind::Patrol, false, true });
        if (!best.prediction.late.empty())
        {
            stop.lateSpawn = model.nodes[best.prediction.late.front().node].spawn;
            stop.lateSecond = best.lateFirst;
        }
        stop.stand = StandPoints(model, best.spot, model.nodes[best.member].at,
            [&](const G3D::Vector3& at)
            {
                const Reached reached = reachOf(at, own);
                return reached.strict.empty() && reached.wander.empty();
            });
        r.stops.push_back(std::move(stop));
        pullSpots.push_back(best.index);

        for (uint32 i : pack.members)
            dead.insert(i);
        for (const Joined& joined : best.prediction.pull)
            dead.insert(joined.node);
        deadAfter.push_back(dead);
        return true;
    };
    for (size_t n = 0; n < onWay.size(); ++n)
        plan(onWay[n].pack, 0, onWay[n].index);

    // Rest spots, after each pull: the clear point of the way nearest the next pull's spot, from the ground just cleared
    // behind this pull up to that spot. Clear is REST_MARGIN outside every living creature's aggro range, a wanderer's
    // range and every patrol's path, with what that pull and those before it killed taken as dead.
    for (size_t k = 0; k < r.stops.size(); ++k)
    {
        const std::set<uint32>& cleared = deadAfter[k];
        const size_t next = k + 1 < pullSpots.size() ? pullSpots[k + 1] : route.size() - 1;
        const size_t backTo = pullSpots[k] > size_t(REST_BACK / ROUTE_STEP) ? pullSpots[k] - size_t(REST_BACK / ROUTE_STEP) : 0;
        float bestMargin = -std::numeric_limits<float>::max();
        size_t bestAt = next;
        int32 clearAt = -1;
        float clearMargin = 0.0f;
        for (size_t j = std::min(backTo, next); j <= next && j < route.size(); ++j)
        {
            float margin = std::numeric_limits<float>::max();
            for (uint32 i : Near(model, route[j], ROUTE_REACH))
            {
                const Node& node = model.nodes[i];
                if (cleared.count(i) || node.maybe || !node.aggroes)
                    continue;
                margin = std::min(margin, (node.at - route[j]).length() - Reach(node, level)
                    - (node.movement == RANDOM_MOTION_TYPE ? node.spawnDist : 0.0f));
            }
            for (uint32 i : patrols)
                if (!cleared.count(i))
                    for (auto const& point : model.nodes[i].patrol)
                        margin = std::min(margin, (point.first - route[j]).length() - Reach(model.nodes[i], level));
            if (margin > bestMargin)
            {
                bestMargin = margin;
                bestAt = j;
            }
            // The clear point nearest the next pull's spot is the last clear one met going forward.
            if (margin >= REST_MARGIN)
            {
                clearAt = int32(j);
                clearMargin = margin;
            }
        }
        Stop& stop = r.stops[k];
        stop.restClear = clearAt >= 0;
        stop.restSpot = route[stop.restClear ? size_t(clearAt) : bestAt];
        stop.restClearance = std::min(stop.restClear ? clearMargin : bestMargin, ROUTE_REACH);
    }
    return result;
}

std::vector<std::string> GuildPacks::PrintRoute(const MapModel& model, const Route& route) const
{
    std::vector<std::string> lines;
    uint32 clean = 0;
    for (const Stop& stop : route.stops)
        clean += stop.clean ? 1 : 0;
    lines.push_back(Format("%s (%u), level %u from %s: %u bosses, %.0f yd%s; %u packs on the way, %u with a clean pull",
        route.dungeon.c_str(), route.mapId, route.level, route.levelFrom.c_str(), uint32(route.bosses.size()), route.length,
        route.straightLegs ? Format(", %u legs straight for want of a route", route.straightLegs).c_str() : "",
        uint32(route.stops.size()), clean));
    std::string bossNames;
    for (auto const& [entry, at] : route.bosses)
        bossNames += (bossNames.empty() ? "" : ", ") + CreatureName(uint32(entry));
    lines.push_back(("bosses: " + bossNames).substr(0, 300));
    if (route.points.size() < 2)
    {
        lines.push_back("no route: the dungeon has no stops after its arrival");
        return lines;
    }

    auto entryOf = [&](uint32 spawn)
    {
        auto found = model.bySpawn.find(spawn);
        return found == model.bySpawn.end() ? 0 : model.nodes[found->second].entry;
    };
    for (size_t n = 0; n < route.stops.size(); ++n)
    {
        const Stop& stop = route.stops[n];
        std::map<std::string, uint32> names;
        uint32 elites = 0;
        for (uint32 spawn : stop.spawns)
        {
            auto found = model.bySpawn.find(spawn);
            if (found == model.bySpawn.end())
                continue;
            names[CreatureName(model.nodes[found->second].entry)]++;
            elites += model.nodes[found->second].rank > 0 ? 1 : 0;
        }
        std::string packText;
        for (auto const& [nm, count] : names)
            packText += (packText.empty() ? "" : ", ") + nm + (count > 1 ? " x" + std::to_string(count) : "");
        std::string verdict;
        if (!stop.spotClear)
        {
            std::map<std::string, uint32> reachNames;
            for (uint32 spawn : stop.reachedBy)
                reachNames[CreatureName(entryOf(spawn))]++;
            verdict = "spot reached by";
            for (auto const& [nm, count] : reachNames)
                verdict += " " + nm + (count > 1 ? " x" + std::to_string(count) : "") + ",";
            verdict.pop_back();
            verdict += "; ";
        }
        if (!stop.legClear)
        {
            std::map<std::string, uint32> legNames;
            for (uint32 spawn : stop.legReachedBy)
                legNames[CreatureName(entryOf(spawn))]++;
            verdict += "way reached by";
            for (auto const& [nm, count] : legNames)
                verdict += " " + nm + (count > 1 ? " x" + std::to_string(count) : "") + ",";
            verdict.pop_back();
            verdict += "; ";
        }
        if (stop.clean && !stop.lateSpawn)
            verdict += "clean";
        else
        {
            if (!stop.extras.empty())
            {
                std::map<std::string, uint32> extraNames;
                for (uint32 spawn : stop.extras)
                    extraNames[CreatureName(entryOf(spawn))]++;
                verdict += "also brings";
                for (auto const& [nm, count] : extraNames)
                    verdict += " " + nm + (count > 1 ? " x" + std::to_string(count) : "") + ",";
                verdict.pop_back();
            }
            if (stop.lateSpawn)
                verdict += (verdict.empty() || verdict.back() == ' ' ? "" : "; ") + Format("%s from %us", CreatureName(entryOf(stop.lateSpawn)).c_str(),
                    stop.lateSecond);
        }
        // The wanderers and patrols of other packs to wait for at the spot, by count.
        uint32 wanderers = 0, patrolling = 0;
        for (const Reacher& reacher : stop.reachers)
            if (!reacher.ownPack)
            {
                wanderers += reacher.kind == ReachKind::Wanderer ? 1 : 0;
                patrolling += reacher.kind == ReachKind::Patrol ? 1 : 0;
            }
        std::string wait;
        if (wanderers || patrolling)
            wait = Format("; wait for %u wanderer%s, %u patrol%s", wanderers, wanderers == 1 ? "" : "s", patrolling, patrolling == 1 ? "" : "s");
        lines.push_back(Format("%u. pack %u%s%s, %u (%u elite): %s; pull at (%.1f %.1f %.1f)%s %.0f yd, %s%s; %u stand", uint32(n + 1),
            stop.pack, stop.boss ? " boss" : "", stop.clearsWay ? " first, to clear the next spot" : "", uint32(stop.spawns.size()), elites, packText.c_str(), stop.pullSpot.x, stop.pullSpot.y,
            stop.pullSpot.z, stop.offWay ? " off the way" : "", stop.pullDistance, verdict.c_str(), wait.c_str(), uint32(stop.stand.size())).substr(0, 260));
        const G3D::Vector3 next = n + 1 < route.stops.size() ? route.stops[n + 1].pullSpot : route.points.back();
        if (stop.restClear)
            lines.push_back(Format("   rest at (%.1f %.1f %.1f), %.0f yd from the next pull", stop.restSpot.x, stop.restSpot.y,
                stop.restSpot.z, (next - stop.restSpot).length()));
        else
            lines.push_back(Format("   no clear rest before the next pull; best (%.1f %.1f %.1f), %.0f yd short", stop.restSpot.x,
                stop.restSpot.y, stop.restSpot.z, REST_MARGIN - stop.restClearance));
    }
    return lines;
}

std::shared_ptr<const GuildPacks::Route> GuildPacks::FindRoute(const std::string& dungeon, Player* leader, uint32 level,
    Player* named, std::string& error)
{
    if (!leader)
    {
        error = "a route needs a character, for its faction";
        return nullptr;
    }
    const std::string stopsKey = dungeon + "/" + std::to_string(uint32(leader->GetTeam()));
    auto known = routeStops.find(stopsKey);
    if (known == routeStops.end())
    {
        uint32 mapId = 0;
        std::vector<std::pair<int32, ai::WorldPosition>> stops;
        if (!GuildDungeon::BossRoute(dungeon, leader, mapId, stops, error))
            return nullptr;
        std::vector<std::pair<int32, G3D::Vector3>> points;
        for (auto const& [entry, place] : stops)
            points.push_back({ entry, G3D::Vector3(place.getX(), place.getY(), place.getZ()) });
        known = routeStops.emplace(stopsKey, std::make_pair(mapId, points)).first;
    }
    const uint32 mapId = known->second.first;
    const auto& points = known->second.second;

    // The level a party should be: the one given, else the named character's when it is within ROUTE_LEVEL_NEAR of the
    // bosses' median, else that median.
    std::vector<uint32> bossLevels;
    for (auto const& [entry, at] : points)
        if (entry)
            if (CreatureInfo const* info = ObjectMgr::GetCreatureTemplate(uint32(entry)))
                bossLevels.push_back(info->MaxLevel);
    std::sort(bossLevels.begin(), bossLevels.end());
    const uint32 median = bossLevels.empty() ? leader->GetLevel() : bossLevels[bossLevels.size() / 2];
    std::string levelFrom = "level=";
    if (!level)
    {
        level = median;
        levelFrom = "the bosses' median";
        if (named && std::abs(int32(named->GetLevel()) - int32(median)) <= ROUTE_LEVEL_NEAR)
        {
            level = named->GetLevel();
            levelFrom = std::string(named->GetName());
        }
    }

    const std::string key = stopsKey + "/" + std::to_string(level);
    auto cached = routes.find(key);
    if (cached == routes.end())
    {
        std::shared_ptr<Route> built = BuildRoute(Model(mapId), dungeon, level, levelFrom, points);
        built->team = uint32(leader->GetTeam());
        cached = routes.emplace(key, std::shared_ptr<const Route>(std::move(built))).first;
    }
    return cached->second;
}

std::shared_ptr<const GuildPacks::Route> GuildPacks::GetRoute(const std::string& dungeon, Player* leader, uint32 level,
    std::string& error)
{
    std::lock_guard<std::mutex> guard(lock);
    return FindRoute(dungeon, leader, level, nullptr, error);
}

namespace
{
    // Whether a stop's pack still has a living creature in the instance: no respawn time, and found alive, or not found
    // in a grid that is not loaded, as it would be once the party comes near.
    bool StopLives(const GuildPacks::Stop& stop, Map* map, MapPersistentState* state, time_t now)
    {
        for (uint32 spawn : stop.spawns)
        {
            if (state && state->GetCreatureRespawnTime(spawn) > now)
                continue;
            if (Creature* creature = map->GetCreature(spawn))
            {
                if (creature->IsAlive())
                    return true;
                continue;
            }
            if (CreatureData const* data = sObjectMgr.GetCreatureData(spawn))
                if (!map->IsLoaded(data->posX, data->posY))
                    return true;
        }
        return false;
    }
}

int32 GuildPacks::FirstLiveStop(const Route& route, Map* map, const G3D::Vector3& at)
{
    if (!map || route.points.empty())
        return -1;
    // The party's place along the way: its nearest point of it.
    size_t nearest = 0;
    for (size_t i = 1; i < route.points.size(); ++i)
        if ((route.points[i] - at).squaredLength() < (route.points[nearest] - at).squaredLength())
            nearest = i;
    const float along = float(nearest) * ROUTE_STEP;
    // The living stop whose pull spot is at or ahead of that place, nearest along the way, judged by the spot's place,
    // not the pack's. Then the clearing stops just before it in the list, living and ahead too: they open its spot and
    // are pulled first. A clearing stop whose spot lies behind the party is passed over.
    MapPersistentState* state = map->GetPersistentState();
    const time_t now = time(nullptr);
    int32 best = -1;
    for (size_t k = 0; k < route.stops.size(); ++k)
    {
        const Stop& stop = route.stops[k];
        if (stop.clearsWay || stop.pullAlong + START_SLACK < along)
            continue;
        if ((best < 0 || stop.pullAlong < route.stops[best].pullAlong) && StopLives(stop, map, state, now))
            best = int32(k);
    }
    if (best < 0)
        return -1;
    while (best > 0 && route.stops[best - 1].clearsWay && route.stops[best - 1].pullAlong + START_SLACK >= along
        && StopLives(route.stops[best - 1], map, state, now))
        --best;
    return best;
}

int32 GuildPacks::FirstLiveStop(const Route& route, Map* map, size_t from)
{
    if (!map)
        return -1;
    MapPersistentState* state = map->GetPersistentState();
    const time_t now = time(nullptr);
    for (size_t k = from; k < route.stops.size(); ++k)
        if (StopLives(route.stops[k], map, state, now))
            return int32(k);
    return -1;
}

std::list<std::string> GuildPacks::HandleRoute(const std::vector<std::string>& words)
{
    // route <dungeon> [character] [level=N] [page]: by dungeon name, not map, since one map can hold wings run apart, as
    // Stratholme's and Dire Maul's do.
    if (words.size() < 2)
        return { "guildbot packs route <dungeon> [character] [level=N] [page]   the way the job walks it, pull and rest spots" };
    const std::string& dungeon = words[1];
    uint32 page = 1, levelArg = 0;
    std::string characterName;
    for (size_t i = 2; i < words.size(); ++i)
    {
        if (words[i].rfind("level=", 0) == 0)
            levelArg = uint32(atoi(words[i].c_str() + 6));
        else if (std::all_of(words[i].begin(), words[i].end(), ::isdigit))
            page = std::max(1u, uint32(atoi(words[i].c_str())));
        else
            characterName = words[i];
    }
    // A character decides the faction's steps, and its level when it is near the dungeon's; any one online when none is
    // named or the named one is offline.
    Player* named = characterName.empty() ? nullptr : ObjectAccessor::FindPlayerByName(characterName.c_str());
    Player* leader = named;
    if (!leader)
        for (auto const& [guid, player] : sObjectAccessor.GetPlayers())
            if (player && player->IsInWorld())
            {
                leader = player;
                break;
            }
    if (!leader)
        return { "a route needs a character online, for its faction" };

    std::string error;
    std::shared_ptr<const Route> route = FindRoute(dungeon, leader, levelArg, named, error);
    if (!route)
        return { error };
    const std::vector<std::string> lines = PrintRoute(Model(route->mapId), *route);

    // The two header lines head every page.
    const size_t head = std::min<size_t>(2, lines.size()), perPage = MAX_LINES - 3;
    const size_t pages = lines.size() <= head ? 1 : (lines.size() - head + perPage - 1) / perPage;
    std::list<std::string> out(lines.begin(), lines.begin() + head);
    for (size_t i = head + (page - 1) * perPage; i < lines.size() && i < head + page * perPage; ++i)
        out.push_back(lines[i]);
    if (page < pages)
        out.push_back(Format("page %u of %u; next: guildbot packs route %s %s level=%u %u", page, uint32(pages), dungeon.c_str(),
            leader->GetName(), route->level, page + 1));
    return out;
}

std::list<std::string> GuildPacks::HandleCommand(const std::string& args)
{
    std::istringstream in(args);
    std::vector<std::string> words;
    for (std::string word; in >> word;)
        words.push_back(word);
    if (!words.empty() && words[0] == "route")
    {
        std::lock_guard<std::mutex> guard(lock);
        return HandleRoute(words);
    }
    // <map>, <map> <instance>, <map> x y z, <map> <instance> x y z, <map> <instance> x y z <level>; instance 0 is spawns only.
    if (words.empty() || words.size() == 3 || words.size() > 6)
        return { "guildbot packs <map> [instance] [x y z [level]]   the map's packs, or what a pull from a spot brings",
                 "  with x y z and no instance, from spawns only; instance 0 for spawns with a level",
                 "guildbot packs route <dungeon> [character] [level=N] [page]   the way the job walks it, with pull and rest spots" };

    const uint32 mapId = uint32(atoi(words[0].c_str()));
    MapEntry const* entry = sMapStore.LookupEntry(mapId);
    if (!entry)
        return { "no map " + words[0] };

    uint32 instanceId = 0;
    G3D::Vector3 spot;
    uint32 level = 0;
    bool predict = false;
    if (words.size() == 2)
        instanceId = uint32(atoi(words[1].c_str()));
    else if (words.size() == 4)
    {
        spot = G3D::Vector3(float(atof(words[1].c_str())), float(atof(words[2].c_str())), float(atof(words[3].c_str())));
        predict = true;
    }
    else if (words.size() >= 5)
    {
        instanceId = uint32(atoi(words[1].c_str()));
        spot = G3D::Vector3(float(atof(words[2].c_str())), float(atof(words[3].c_str())), float(atof(words[4].c_str())));
        if (words.size() == 6)
            level = uint32(atoi(words[5].c_str()));
        predict = true;
    }

    std::lock_guard<std::mutex> guard(lock);
    const MapModel& model = Model(mapId);
    Map* map = instanceId ? sMapMgr.FindMap(mapId, instanceId) : nullptr;
    if (instanceId && !map)
        return { Format("no instance %u of map %u is loaded", instanceId, mapId) };
    if (!predict)
        return ListPacks(model, map);
    if (!level)
    {
        // The party at the level of the creature nearest the spot.
        float nearest = PULL_LOOK;
        for (uint32 i : Near(model, spot, PULL_LOOK))
            if ((model.nodes[i].at - spot).length() < nearest)
            {
                nearest = (model.nodes[i].at - spot).length();
                level = model.nodes[i].maxLevel;
            }
        if (!level)
            level = 60;
    }
    return ShowPrediction(model, map, spot, level);
}
