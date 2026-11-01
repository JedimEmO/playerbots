#pragma once

#include "Common.h"
#include "G3D/Vector3.h"
#include "Entities/ObjectGuid.h"
#include "playerbot/GuildRunRules.h"
#include "playerbot/GuildRunGuard.h"

#include <functional>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

class Creature;
class Map;
class Player;
class Unit;

// The dungeon model's packs and its pull predictor, from the core's own rules: what a pull of one creature brings.
// It reads the world database's spawns as the core holds them, and the live instance when there is one; nothing in
// the engine acts on it yet. See docs/engine.md, "Packs", and the rules in docs/dungeon-runs.md.
//
// Per map id, built the first time it is asked for, the map model holds each creature that could fight a party:
// its spawn, level, aggro range, call-for-help radius, bounding radius, movement, patrol path, links and scripts,
// and the packs, the creatures one pull brings by call for help and links alone.
// A prediction is for one instance and one spot: the pulled creature, what joins it and how, and apart from those,
// the wanderers and patrols that could reach the fight later, with the earliest second.
// A route is the way the dungeon job walks a dungeon, with a pull spot for each pack near it and rest spots between.
//
// It runs on the world thread outside map updates only, since it reads the instance's creatures, its line of sight and
// its navmesh query. It has its own lock and calls nothing in GuildDungeon.
class GuildPacks
{
public:
    static GuildPacks& instance()
    {
        static GuildPacks packs;
        return packs;
    }

    // `guildbot packs <map> [instance] [x y z [level]]`, for the console, SOAP and a GM's chat.
    std::list<std::string> HandleCommand(const std::string& args);

    // How a creature reaches a pull spot: standing still within its aggro reach, wandering within it, or on a patrol's path.
    enum class ReachKind : uint8 { Static, Wanderer, Patrol };
    struct Reacher
    {
        uint32 spawn = 0;                       // its row in the world database's creature table
        ReachKind kind = ReachKind::Static;
        bool ownPack = false;                   // one of the stop's own pack
        bool onLeg = false;                     // it reaches the way from the stop before to the pull spot, not the spot
    };

    // One stop of a route: a pack the way meets, where the tank pulls it from, where the party stands, and where it rests.
    // Spawns are the creature table's guids; Map::GetCreature(spawn) finds one in an instance.
    struct Stop
    {
        uint32 pack = 0;                        // the pack's id in the map model, as `guildbot packs <map>` lists it
        std::vector<uint32> spawns;             // its creatures, those not already counted dead at an earlier stop
        uint32 pulled = 0;                      // the creature the tank pulls: the nearest of the pack to the pull spot
        bool boss = false;                      // one of the dungeon's bosses is in the pack
        float along = 0.0f;                     // yards along the route of the pack's nearest point, near where the stop is needed
        G3D::Vector3 pullSpot;                  // where the tank pulls from, and where the fight is held
        float pullAlong = 0.0f;                 // yards along the route of the pull spot, or of the way's point nearest it when off it
        bool offWay = false;                    // the pull spot is off the way, by a navmesh path from it
        float pullDistance = 0.0f;              // yards from the pull spot to the pulled creature
        // The pull is cast from the spot: the pulled creature within runguard::PULL_CAST yards of it in 3D, its wander range
        // included, and in sight from eye height. The route prefers such a spot, and keeps its best one when none is.
        bool pullCast = false;
        bool clean = false;                     // the pull brings nothing beyond the pack
        std::vector<uint32> extras;             // what the pull brings beyond the pack
        // Whether no creature standing still or wandering, and none of the pack's own, reaches the pull spot itself, by the
        // reach the movement guard shares (runguard::StaticRefuses): its aggro reach in 3D, 3 yards more, and a wanderer's
        // whole range across, never up or down. Walking there pulls such a creature, so a stop with spotClear false should not be approached. Other
        // packs' patrols and followers do not count here; they are in reachers, to be waited for by where they are.
        bool spotClear = true;
        std::vector<uint32> reachedBy;          // the creatures that make spotClear false
        // Every creature that reaches the spot or the way to it (onLeg), with how: one standing still or wandering, by the
        // shared reach, a patrol or follower of another pack (from any point of its path; one whose whole path stays within
        // that reach never leaves, and is as one standing still), and the pack's own.
        std::vector<Reacher> reachers;
        // Whether nothing standing still or wandering reaches the way from the stop before's pull spot to this one, and none
        // of the pack's own reaches it short of the spot, by the shared reach. The route plans such a creature's pack first
        // where it can. The way's patrols and followers are in reachers, onLeg.
        bool legClear = true;
        std::vector<uint32> legReachedBy;       // the creatures that make legClear false
        bool clearsWay = false;                 // planned first, because its pack reached the next stop's pull spot
        // For a stop whose spot or way another pack still reaches: each such pack, by one of its creatures, and why it
        // was not planned first, as "Goblin Engineer (pack 103): its way reached by Gilnid".
        std::vector<std::string> notCleared;
        uint32 lateSpawn = 0;                   // the earliest wanderer or patrol that could reach the fight, or 0
        uint32 lateSecond = 0;                  // the second it could
        // Where the party stands at the pull spot: four points on the navmesh 4 to 8 yards behind it, on the side away
        // from the pack, in sight of it, each with its whole navmesh path from the spot outside the shared reach of every
        // creature standing still or wandering alive at this point of the run's order, the pack's own included, and, at
        // every yard, outside what the spot's own test counts strict among the creatures that walk about: another pack's
        // pacing patrol or a wanderer's follower, and the pack's own patrols and followers.
        // Nearest first: two for melee, then the ranged one's, then the healer's, farthest back.
        std::vector<G3D::Vector3> stand;
        G3D::Vector3 restSpot;                  // where the party rests after the pull, before the next stop's spot
        float restClearance = 0.0f;             // yards outside every living creature's reach there; negative when it is inside one
        bool restClear = false;                 // the clearance is at least the rest margin, 5 yards
    };

    // The way the dungeon job walks a dungeon, as `guildbot packs route` prints it.
    struct Route
    {
        std::string dungeon;                    // the name the job takes, such as stratholme_live
        uint32 mapId = 0;
        uint32 team = 0;                        // the core's Team the route was made for, from the leader
        uint32 level = 0;                       // the party level its reach was worked out at
        std::string levelFrom;                  // where that level came from: "level=", a character's name, or the bosses' median
        G3D::Vector3 arrival;                   // where the entrance puts a party
        std::vector<std::pair<int32, G3D::Vector3>> bosses; // each boss's entry and spawn, in the job's order
        std::vector<G3D::Vector3> points;       // the way, a point every 2 yards along it: point i is i * 2 yards along
        float length = 0.0f;                    // yards
        uint32 straightLegs = 0;                // legs with no travel map route, walked as straight lines
        std::vector<Stop> stops;                // in the order the way meets them
        // Every creature of the map model as the movement guard's envelopes, at the route's level, with their cells. Made with
        // the route and never changed, so map threads read it with no lock.
        std::shared_ptr<const runguard::ReachIndex> reach;
    };

    // The route of a dungeon for the leader's faction, at a party level, or at the bosses' median for level 0.
    // Made the first time it is asked for and kept; the same object is handed out after. World thread only, outside map
    // updates. Null, with why in error, when the dungeon does not resolve.
    std::shared_ptr<const Route> GetRoute(const std::string& dungeon, Player* leader, uint32 level, std::string& error);

    // Where a party is on a route: the living stop whose pull spot is at or ahead of the point's place along the route,
    // nearest along it, judged by pullAlong; then the living clearing stops just before it whose spots are ahead too,
    // which are pulled first. A clearing stop whose spot is behind the point is passed over. -1 when none lives.
    // A creature not loaded and not waiting to respawn counts as living. World thread, or the instance's own map thread.
    // `progress`, when not negative, is the run's place along the way in yards, as its stop before a hold: the party's
    // place is then read by it where the way passes `at` more than once (runrules::PlaceAlong).
    static int32 FirstLiveStop(const Route& route, Map* map, const G3D::Vector3& at, float progress = -1.0f);
    // The same from a stop's index on: the first stop at or after it whose pack still has a living creature, or -1.
    static int32 FirstLiveStop(const Route& route, Map* map, size_t from);
    // Whether one stop's pack still has a living creature, as FirstLiveStop judges it.
    static bool StopAlive(const Route& route, Map* map, size_t stop);

    // The creatures of the route's map that may stand in a line's way: each that attacks on sight and may be alive in the
    // instance, at its spawn and where it is now, a patrol along its path, each with its reach at the route's level and
    // `margin`, and a wanderer's range as its spread (across only, by runrules::PlaceDistance, never added to its reach).
    // One not loaded counts as alive; one missing from a loaded grid is not up.
    // The living creatures the model does not have that reach the line come last (Unmodelled).
    // Those near the line that are dead go in `dead`, with their spawns. World thread, outside map updates.
    std::vector<runrules::Threat> Threats(const Route& route, Map* map, const std::vector<runrules::Vec>& line, float margin,
        std::vector<uint32>& dead);
    // The living creatures hostile to players on the instance's map that the route's model does not have, as a summon or a
    // script's spawn, that reach the line: where each stands now, its aggro reach at the route's level with both radii and
    // `margin`, and when it moves runrules::TRAVEL_MARGIN as its spread, across only. Their spawn is 0, or their unmodelled
    // spawn; `entry` names them. On the
    // world thread outside map updates, or on the instance's own map thread.
    // `found`, when given, gets each one's guid, in the same order.
    std::vector<runrules::Threat> Unmodelled(const Route& route, Map* map, const std::vector<runrules::Vec>& line, float margin,
        std::vector<ObjectGuid>* found = nullptr);
    // The movement guard's live scan, one pass over the instance's creatures, for what the guard judges where it stands
    // (runguard::LiveThreat): every living creature hostile to players that the route's model does not have (as
    // Unmodelled), every patrol and follower of the model wherever it is, and every other modelled one standing off its own
    // envelope (runguard::OffEnvelope), as one drawn away by a fight. Each at its live place, with its aggro reach toward
    // `mover` (or at the route's level with no mover) and runguard::ReachPad, and its speed (LiveSpeed); taken when that
    // reach and its walk over the guard's horizon at `moverSpeed` (runguard::MaxSpread) come to the line. A patrol or a
    // follower on its loop and out of a fight is bound by its loop (runguard::EnvelopeThreat). Creatures standing still or
    // wandering where they belong are the reach index's. Reads only the route's own index, so it takes no lock: the
    // instance's own map thread, or the world thread outside map updates. `found` gets each one's guid, in the same order.
    static std::vector<runguard::GuardThreat> LiveThreats(const Route& route, Map* map, const std::vector<runrules::Vec>& line,
        Unit* mover, float moverSpeed, std::vector<ObjectGuid>* found = nullptr);
    // How fast a creature may move toward a path, in yards a second: its spline's speed while it moves, its run speed in a
    // fight, its walk speed when it is one that walks about (`walks`: a wanderer, a patrol, a follower), else 0.
    static float LiveSpeed(Creature* creature, bool walks);
    // The movement guard's threat for one living creature toward `mover` (or with none at `level`, the route's when 0), as the guard
    // builds it at each check: a modelled one by its envelope (runguard::EnvelopeThreat), drawn when in a fight or off
    // its envelope, and any other where it stands (runguard::LiveThreat); its aggro reach with runguard::ReachPad, and its
    // speed (LiveSpeed). LiveThreats and the guard's own reach index take theirs from it, and the wait judges its
    // blockers by it (GuildRunStates::Blockers), so the three share one model. The creature's own map thread, or the world
    // thread outside map updates.
    static runguard::GuardThreat GuardThreatOf(const Route& route, Creature* creature, Unit* mover, uint32 level = 0);
    // The stop whose pack, or whose pull's extras, a creature is of, or -1.
    static int32 StopOf(const Route& route, uint32 spawn);

private:
    GuildPacks() = default;

    // A creature of the map as the world database spawns it.
    struct Node
    {
        uint32 spawn = 0;              // the creature table's guid
        uint32 entry = 0;
        G3D::Vector3 at;               // where it spawns
        float bounding = 0.5f;         // its model's bounding radius, scaled
        float detection = 18.0f;       // the template's aggro range before the level difference
        float callRadius = 5.0f;       // whom it calls on a pull: CallForHelp, or the family assistance radius
        float spawnDist = 0.0f;        // how far it wanders
        float walk = 2.5f, run = 7.0f; // yards a second
        uint32 minLevel = 1, maxLevel = 1;
        uint32 rank = 0;
        uint32 elites = 0;             // itself when elite, and each elite entry it summons, as the Goblin Engineer's golem
        uint8 movement = 0;            // the core's motion type: 1 wanders, 2 walks a path
        bool caster = false;
        bool aggroes = true;           // hostile to players, so it attacks on sight; a neutral one only answers calls
        bool maybe = false;            // one of a pool's or spawn group's spawns, of which fewer are up than listed
        std::vector<std::pair<G3D::Vector3, uint32>> patrol; // its path's points and the wait at each, in ms
        // From its EventAI scripts: the entries it summons, whether on its death, whether it flees for help at low health,
        // and a scripted call-for-help radius.
        std::vector<uint32> summons;
        bool summonsOnDeath = false;
        bool flees = false;
        float scriptCall = 0.0f;
        // Its link to a master, from creature_linking: whether the master's aggro brings it, whether its aggro brings
        // the master, and whether it walks with the master.
        int32 master = -1;
        bool fromMaster = false, toMaster = false, follows = false;
        std::vector<uint32> slaves;
        uint32 pack = 0;
    };

    struct Pack
    {
        std::vector<uint32> members;
        G3D::Vector3 centre;
        float radius = 0.0f;
        uint32 elites = 0;
        bool patrol = false, linked = false, summons = false, flees = false;
        bool aggroes = false;          // any of it attacks on sight
    };

    struct MapModel
    {
        uint32 mapId = 0;
        std::string name;
        std::vector<Node> nodes;
        std::unordered_map<uint32, uint32> bySpawn;          // spawn guid to node
        std::unordered_map<int64, std::vector<uint32>> cells; // 10 yard cells of spawn positions to nodes
        std::vector<Pack> packs;
        std::map<std::string, uint32> maybe;                 // spawns that may not be up, by name, left out of the packs
    };

    // One creature in a prediction: its node, where it is now, how it joins and the second it is expected.
    struct Joined
    {
        uint32 node = 0;
        G3D::Vector3 at;
        std::string source;
        uint32 second = 0;
    };

    struct Prediction
    {
        int32 pulled = -1;
        std::vector<Joined> pull;      // what the pull brings
        std::vector<Joined> late;      // wanderers and patrols that could reach the spot during the fight
        uint32 unknownSight = 0;       // sight checks it could not make, taken as clear
        uint32 notLoaded = 0;          // creatures not loaded in the instance, taken at their spawns
        bool live = false;
    };

    // What a prediction looks at: creatures within reach of the spot, a pulled creature when it is given, and creatures
    // already dead, as the packs a route clears before.
    struct PredictOptions
    {
        float reach = 150.0f;
        int32 pulled = -1;
        const std::set<uint32>* dead = nullptr;
    };

    MapModel& Model(uint32 mapId);
    std::vector<runrules::Threat> UnmodelledLocked(const MapModel& model, const Route& route, Map* map,
        const std::vector<runrules::Vec>& line, float margin, std::vector<ObjectGuid>* found = nullptr) const;
    void Build(MapModel& model);
    // The movement guard's envelopes of every creature of the model, at a party level (Route::reach).
    std::shared_ptr<const runguard::ReachIndex> BuildReach(const MapModel& model, uint32 level) const;
    // Where a follower may be, as the route and the guard model it: its master's loop, or its master's spawn, in `places`,
    // with `range` yards across it may stand from them (its follow distance, and a wandering master's range). False when
    // its master neither patrols nor wanders.
    static bool FollowerPlaces(const Node& node, const Node& master, std::vector<G3D::Vector3>& places, float& range);
    void LinkAndGroup(MapModel& model);
    std::vector<uint32> Near(const MapModel& model, const G3D::Vector3& at, float radius) const;
    Prediction Predict(const MapModel& model, Map* map, const G3D::Vector3& spot, uint32 level,
        const PredictOptions& options) const;
    // A creature's aggro radius toward a bot of `level` (runguard::AggroRadius) at its highest level, and both bounding
    // radii: what the prediction and the rest spots measure by.
    float Reach(const Node& node, uint32 level) const;
    // The one reach the route plans by and the guard shares: Reach and THREAT_MARGIN (runguard::ReachPad).
    float GuardReach(const Node& node, uint32 level) const;
    // The route, planned by the reach index's shared reach (runguard::StaticRefuses), which is made first.
    std::shared_ptr<Route> BuildRoute(const MapModel& model, const runguard::ReachIndex& index, const std::string& name,
        uint32 level, const std::string& levelFrom, const std::vector<std::pair<int32, G3D::Vector3>>& stops) const;
    // `clear` takes the navmesh path from the spot to a stand, its last point the stand.
    std::vector<G3D::Vector3> StandPoints(const MapModel& model, const G3D::Vector3& spot, const G3D::Vector3& pack,
        const std::function<bool(const std::vector<G3D::Vector3>&)>& clear) const;
    std::vector<std::string> PrintRoute(const MapModel& model, const Route& route) const;
    std::shared_ptr<const Route> FindRoute(const std::string& dungeon, Player* leader, uint32 level, Player* named,
        std::string& error);
    std::list<std::string> HandleRoute(const std::vector<std::string>& words);
    std::list<std::string> ListPacks(const MapModel& model, Map* map) const;
    std::list<std::string> ShowPrediction(const MapModel& model, Map* map, const G3D::Vector3& spot, uint32 level) const;

    std::mutex lock;
    std::map<uint32, std::unique_ptr<MapModel>> models;
    std::map<std::string, std::shared_ptr<const Route>> routes; // by dungeon, faction and level
    // Each dungeon's map and stops, by dungeon and faction, as GuildDungeon::BossRoute gives them.
    std::map<std::string, std::pair<uint32, std::vector<std::pair<int32, G3D::Vector3>>>> routeStops;
};

#define sGuildPacks GuildPacks::instance()
