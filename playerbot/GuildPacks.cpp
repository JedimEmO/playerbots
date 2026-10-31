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
#include "MotionGenerators/MoveMap.h"
#include "MotionGenerators/PathFinder.h"
#include "MotionGenerators/WaypointManager.h"
#include "Server/DBCStores.h"
#include "Server/SQLStorages.h"
#include "Spells/SpellMgr.h"
#include "World/World.h"

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
        // Only what fights a party: a faction hostile to players.
        FactionTemplateEntry const* faction = sFactionTemplateStore.LookupEntry(info->Faction);
        if (!faction || !faction->IsHostileToPlayers())
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
        // The search reaches past the largest bounding radius a creature here could have.
        const float reach = std::max(a.callRadius, a.scriptCall) + a.bounding + 8.0f;
        for (uint32 j : Near(model, a.at, reach))
            if (j != i && (model.nodes[j].at - a.at).length() <= std::max(a.callRadius, a.scriptCall) + a.bounding + model.nodes[j].bounding)
                join(i, j);
        if (a.master >= 0 && (a.fromMaster || a.toMaster))
            join(i, uint32(a.master));
    }
    std::map<uint32, uint32> packOf;
    for (uint32 i = 0; i < model.nodes.size(); ++i)
    {
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

GuildPacks::Prediction GuildPacks::Predict(const MapModel& model, Map* map, const G3D::Vector3& spot, uint32 level) const
{
    Prediction result;
    result.live = map != nullptr;

    // The creatures in reach of the spot, and every patrol, where they are now in the instance: the dead and those
    // already fighting are left out, and one not loaded is taken at its spawn.
    std::map<uint32, G3D::Vector3> where;
    auto consider = [&](uint32 i)
    {
        if (where.count(i))
            return;
        const Node& node = model.nodes[i];
        G3D::Vector3 at = node.at;
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
    for (uint32 i : Near(model, spot, PREDICT_REACH))
        consider(i);
    for (uint32 i = 0; i < model.nodes.size(); ++i)
        if (!model.nodes[i].patrol.empty())
            for (auto const& point : model.nodes[i].patrol)
                if ((point.first - spot).length() <= PREDICT_REACH)
                {
                    consider(i);
                    break;
                }

    // The pulled creature: the nearest to the spot, as the dungeon job takes the next creature in its way.
    float nearest = PULL_LOOK;
    for (auto const& [i, at] : where)
        if ((at - spot).length() < nearest)
        {
            nearest = (at - spot).length();
            result.pulled = int32(i);
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
    auto reach = [&](const Node& node)
    {
        const float creatureLevel = float(node.minLevel + node.maxLevel) / 2.0f;
        return std::max(5.0f, node.detection - (float(level) - creatureLevel)) + node.bounding + PLAYER_BOUNDING;
    };

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
        // Aggro at the spot, from where each creature stands now.
        for (auto const& [b, at] : where)
            if (!joined.count(b) && (at - spot).length() <= reach(model.nodes[b]) && sees(at, spot))
                add(b, "sight", 0);
        changed = !queue.empty();
    }
    for (auto const& [i, j] : joined)
        result.pull.push_back(j);

    // Late reach: wanderers whose range comes within their aggro of the spot, and patrols whose path does, with the
    // earliest second, from where they are now; and what follows them.
    for (auto const& [b, at] : where)
    {
        if (joined.count(b))
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
        out.push_back(line.str().substr(0, 240));
    }
    return out;
}

std::list<std::string> GuildPacks::ShowPrediction(const MapModel& model, Map* map, const G3D::Vector3& spot, uint32 level) const
{
    std::list<std::string> out;
    const Prediction prediction = Predict(model, map, spot, level);
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

std::list<std::string> GuildPacks::HandleCommand(const std::string& args)
{
    std::istringstream in(args);
    std::vector<std::string> words;
    for (std::string word; in >> word;)
        words.push_back(word);
    // <map>, <map> <instance>, <map> x y z, <map> <instance> x y z, <map> <instance> x y z <level>; instance 0 is spawns only.
    if (words.empty() || words.size() == 3 || words.size() > 6)
        return { "guildbot packs <map> [instance] [x y z [level]]   the map's packs, or what a pull from a spot brings",
                 "  with x y z and no instance, from spawns only; instance 0 for spawns with a level" };

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
