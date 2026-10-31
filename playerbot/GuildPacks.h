#pragma once

#include "Common.h"
#include "G3D/Vector3.h"

#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

class Map;

// The dungeon model's packs and its pull predictor, from the core's own rules: what a pull of one creature brings.
// It reads the world database's spawns as the core holds them, and the live instance when there is one; nothing in
// the engine acts on it yet. See docs/engine.md, "Packs", and the rules in docs/dungeon-runs.md.
//
// Per map id, built the first time it is asked for, the map model holds each creature that could fight a party:
// its spawn, level, aggro range, call-for-help radius, bounding radius, movement, patrol path, links and scripts,
// and the packs, the creatures one pull brings by call for help and links alone.
// A prediction is for one instance and one spot: the pulled creature, what joins it and how, and apart from those,
// the wanderers and patrols that could reach the fight later, with the earliest second.
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
    };

    struct MapModel
    {
        uint32 mapId = 0;
        std::string name;
        std::vector<Node> nodes;
        std::unordered_map<uint32, uint32> bySpawn;          // spawn guid to node
        std::unordered_map<int64, std::vector<uint32>> cells; // 10 yard cells of spawn positions to nodes
        std::vector<Pack> packs;
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

    MapModel& Model(uint32 mapId);
    void Build(MapModel& model);
    void LinkAndGroup(MapModel& model);
    std::vector<uint32> Near(const MapModel& model, const G3D::Vector3& at, float radius) const;
    Prediction Predict(const MapModel& model, Map* map, const G3D::Vector3& spot, uint32 level) const;
    std::list<std::string> ListPacks(const MapModel& model, Map* map) const;
    std::list<std::string> ShowPrediction(const MapModel& model, Map* map, const G3D::Vector3& spot, uint32 level) const;

    std::mutex lock;
    std::map<uint32, std::unique_ptr<MapModel>> models;
};

#define sGuildPacks GuildPacks::instance()
