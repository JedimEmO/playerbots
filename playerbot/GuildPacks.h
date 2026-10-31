#pragma once

#include "Common.h"
#include "G3D/Vector3.h"

#include <functional>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

class Map;
class Player;

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
        bool clean = false;                     // the pull brings nothing beyond the pack
        std::vector<uint32> extras;             // what the pull brings beyond the pack
        // Whether no creature standing still, and none of the pack's own, reaches the pull spot itself: within its aggro reach
        // in 3D and 3 yards more, another pack's wanderer from its spawn, a wanderer of the pack's own from anywhere in its range. Walking there pulls such a
        // creature, so a stop with spotClear false should not be approached. Other packs' wanderers and patrols do not
        // count here; they are in reachers, to be waited for by where they are when the party comes.
        bool spotClear = true;
        std::vector<uint32> reachedBy;          // the creatures that make spotClear false
        // Every creature that reaches the spot or the way to it (onLeg), with how: one standing still (aggro reach and 3
        // yards), a wanderer (its spawn that near is as one standing still; else from its spawn plus half its range, no
        // margin), a patrol of another pack (from any point of its path; one whose whole path stays within that reach
        // never leaves, and is as one standing still), and the pack's own.
        std::vector<Reacher> reachers;
        // Whether nothing standing still reaches the way from the stop before's pull spot to this one, and none of the
        // pack's own reaches it short of the spot: within aggro reach in 3D and 3 yards, a wanderer from its spawn. The
        // route plans such a creature's pack first where it can. The way's wanderers and patrols are in reachers, onLeg.
        bool legClear = true;
        std::vector<uint32> legReachedBy;       // the creatures that make legClear false
        bool clearsWay = false;                 // planned first, because its pack reached the next stop's pull spot
        uint32 lateSpawn = 0;                   // the earliest wanderer or patrol that could reach the fight, or 0
        uint32 lateSecond = 0;                  // the second it could
        // Where the party stands at the pull spot: four points on the navmesh 4 to 8 yards behind it, on the side away
        // from the pack, in sight of it, each outside every living creature's reach as the pull spot is.
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
    };

    // The route of a dungeon for the leader's faction, at a party level, or at the bosses' median for level 0.
    // Made the first time it is asked for and kept; the same object is handed out after. World thread only, outside map
    // updates. Null, with why in error, when the dungeon does not resolve.
    std::shared_ptr<const Route> GetRoute(const std::string& dungeon, Player* leader, uint32 level, std::string& error);

    // Where a party is on a route: the living stop whose pull spot is at or ahead of the point's place along the route,
    // nearest along it, judged by pullAlong; then the living clearing stops just before it whose spots are ahead too,
    // which are pulled first. A clearing stop whose spot is behind the point is passed over. -1 when none lives.
    // A creature not loaded and not waiting to respawn counts as living. World thread, or the instance's own map thread.
    static int32 FirstLiveStop(const Route& route, Map* map, const G3D::Vector3& at);
    // The same from a stop's index on: the first stop at or after it whose pack still has a living creature, or -1.
    static int32 FirstLiveStop(const Route& route, Map* map, size_t from);

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
    void Build(MapModel& model);
    void LinkAndGroup(MapModel& model);
    std::vector<uint32> Near(const MapModel& model, const G3D::Vector3& at, float radius) const;
    Prediction Predict(const MapModel& model, Map* map, const G3D::Vector3& spot, uint32 level,
        const PredictOptions& options) const;
    float Reach(const Node& node, uint32 level) const;
    std::shared_ptr<Route> BuildRoute(const MapModel& model, const std::string& name, uint32 level, const std::string& levelFrom,
        const std::vector<std::pair<int32, G3D::Vector3>>& stops) const;
    std::vector<G3D::Vector3> StandPoints(const MapModel& model, const G3D::Vector3& spot, const G3D::Vector3& pack,
        const std::function<bool(const G3D::Vector3&)>& clear) const;
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
