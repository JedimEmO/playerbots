#pragma once

#include "Common.h"
#include "Entities/ObjectGuid.h"
#include "playerbot/GuildPacks.h"
#include "playerbot/WorldPosition.h"

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

class Map;
class Player;

// One pack on a run's route, and where the party fights it.
struct GuildRunStop
{
    std::vector<uint32> spawns;             // the pack's creatures, by spawn guid
    // The controller pulls it. When false, it passed the stop over (`skipped` says why): its pull spot is off the navmesh,
    // or the pack could not be pulled from any spot. The stop is never handed to the dungeon job.
    bool planned = true;
    std::string skipped;
    uint32 pulled = 0;                      // the one the dungeon model chose the pull spot to pull
    ai::WorldPosition pull;                 // where the party waits and the tank brings the pull
    // The members' places at the pull spot, behind it on the side away from the pack: two for melee, then the
    // ranged one's, then the healer's, farthest back.
    std::vector<ai::WorldPosition> stands;
    ai::WorldPosition rest;                 // where the party rests after it, on cleared ground

    // Creatures that reach the pull spot (GuildPacks' reachers):
    // one standing still, or of the pack's own, which never leaves (spotClear false): the pack is pulled from the stop
    // before's spot;
    bool stillReach = false;
    std::vector<uint32> reachedBy;          // those creatures, for the events
    // another pack's wanderers and patrols, by where they are now: the party first waits at the stop before's spot.
    std::vector<uint32> outsideReach;
    bool hasBefore = false;                 // there is a stop before, whose places the two use
    ai::WorldPosition before;               // the stop before's pull spot
    std::vector<ai::WorldPosition> beforeStands; // and its stand points
};

// Where the leader is with the stop it is at: walking to its pull spot, waiting there for the party, pulling, or waiting
// at the stop before's spot, for up to WAIT_SECONDS, for another pack's creature to leave the pull spot's reach.
enum class GuildRunPull : uint8 { Approach, Gather, Pull, Wait };

// A copy of a run state for one bot, so that its strategies read it without a lock.
struct GuildRunView
{
    bool valid = false;        // a run state for the bot's group, on the bot's map
    bool controls = false;     // the party is the controller's: a living pack ahead, and no step of the job due
    bool lead = false;         // the bot leads the run and pulls
    uint32 mapId = 0;
    uint32 leader = 0;         // the leader's guid
    size_t index = 0;          // the stop the party is at
    size_t stops = 0;
    GuildRunStop stop;         // that stop, while index < stops
    std::shared_ptr<const GuildPacks::Route> packs; // GuildPacks' route, which never changes once made
    GuildRunPull pull = GuildRunPull::Approach;
    ObjectGuid pulled;         // the creature the leader pulled last
    time_t pullAt = 0;
    time_t stopAt = 0;         // when the party went on to this stop
    time_t gatherSince = 0;    // when the leader came to the pull spot
    time_t waitSince = 0;      // when the leader began waiting at the stop before
    bool waited = false;       // it waited as long as it may, and pulls with the other pack's creature still in reach
    bool ownSpot = false;      // a creature reaches the spot for good, and pulling from the stop before failed: from its own
    uint32 pullTries = 0;      // pulls of this stop that brought nothing
};

// The shared state of a guild party's run inside one dungeon instance, which the party's AI plays by: the route of
// packs from GuildPacks, the stop the party is at, and where the leader's pull stands. See docs/dungeon-runs.md.
//
// Who writes what: the guild tick creates a run state when the run's party is inside, keeps whether the controller
// has the party, and ends it; the leader's AI moves the stop and the pull on; members only read.
// The tick runs on the world thread before the maps update, and a bot's AI inside its map's update, so the two never
// run at once. A member on another map, such as a ghost outside, can read while the leader writes, so each run state
// has its own lock, held only to copy plain values in or out. The registry has its own lock too. Neither is taken
// while GuildDungeon's lock is held, nor that one while either is: a std::mutex does not nest.
class GuildRunStates
{
public:
    static GuildRunStates& instance()
    {
        static GuildRunStates states;
        return states;
    }

    // What the leader waits for before a pull: every member within PARTY_NEAR yards of the pull spot, alive, out of a
    // fight, with PULL_HEALTH percent health and PULL_MANA percent mana or more.
    static constexpr float PARTY_NEAR = 12.0f;
    static constexpr uint32 PULL_HEALTH = 80;
    static constexpr uint32 PULL_MANA = 60;
    // How long the controller may hold a leader still, out of a fight, before the tick says why.
    static constexpr time_t STALL_SECONDS = 60;
    // How long the party waits at the stop before for another pack's creature to leave, before it pulls anyway.
    static constexpr time_t WAIT_SECONDS = 90;
    // The margin on a creature's reach, as GuildPacks measures a pull spot's.
    static constexpr float REACH_MARGIN = 3.0f;

    // From the guild tick, for a dungeon leader, outside GuildDungeon's lock, on either side of the job's own tick.
    // Before it: with the leader in a dungeon's instance, the run state is made the first time, from GuildPacks' route
    // at the first stop with a living pack whose pull spot is not behind the party (FirstLiveStop by position), and
    // whether the controller has the party is worked out from the job's
    // last tick. So the job's pull step stands down from the first tick inside. Out of the instance, it is ended.
    // Returns whether the controller has the party.
    bool Prepare(Player* leader, const std::string& dungeon, std::vector<std::pair<std::string, std::string>>& events);
    // After it: whether the run's party is in on its bosses and whether a step of the tables is due. Events for the job:
    // a new pull, the leader's notes (waits, skips, pulls from the pack's own spot), and a leader the controller holds
    // still for a minute, with what it waits for.
    std::vector<std::pair<std::string, std::string>> Settle(Player* leader, bool inside, bool stepDue);
    // The job ended, changed or left the instance: forget the leader's run state.
    void End(uint32 leaderGuid);

    // Any thread: the run state as one bot sees it.
    GuildRunView View(Player* bot) const;

    // The leader's AI, on its map's thread: it stands at its pull spot, it pulled a creature, a pull brought nothing,
    // it goes on to another stop.
    void Gather(Player* leader);
    void Pulled(Player* leader, ObjectGuid creature);
    void PullFailed(Player* leader);
    void SetStop(Player* leader, size_t index);
    // On to the first stop with a living pack from the current one, or past it with skip, as for a pack that pulls
    // bring nothing of; a stop passed over is passed again. Looks at the leader's own map, so only from its map's thread.
    void Advance(Player* leader, bool skip);
    // It waits at the stop before for another pack's wanderer or patrol to leave; it goes on when none is in reach, or when
    // it has waited WAIT_SECONDS; it pulls its pack from its own spot after the stop before's failed; it passes the stop over.
    // Each carries the event's detail, which the tick says.
    void Wait(Player* leader, const std::string& detail);
    void Go(Player* leader);
    void WaitedOut(Player* leader, const std::string& detail);
    void UseOwnSpot(Player* leader, const std::string& detail);
    void Skip(Player* leader, const std::string& detail);

    // Where the party fights a stop: its pull spot, or the stop before's while it waits there, or when a creature that
    // stands still or is of the pack's own reaches the pull spot, unless pulling from there failed. The stand points there.
    static const ai::WorldPosition& FightSpot(const GuildRunStop& stop, GuildRunPull pull, bool ownSpot);
    static const std::vector<ai::WorldPosition>& FightStands(const GuildRunStop& stop, GuildRunPull pull, bool ownSpot);
    // Whether another pack's wanderer or patrol is alive and within reach of the stop's pull spot where it is now: its aggro
    // range toward the bot, both bounding radii and a 3 yard margin, as GuildPacks measures reach. Never for a stop
    // with no stop before, which has nowhere to wait. On the map's own thread.
    static bool Reached(Map* map, Unit* bot, const GuildRunStop& stop);
    // Creatures' names, by spawn guid, for the events.
    static std::string Names(const std::vector<uint32>& spawns);

    // The first stop from index on with a living pack, or the route's size when there is none (GuildPacks::FirstLiveStop).
    // NextStop is the same, passing over the stops the controller passes over. On the world thread between map updates,
    // or on the instance's own map thread.
    static size_t FirstLiving(const GuildPacks::Route* packs, Map* map, size_t index);
    static size_t NextStop(const GuildPacks::Route* packs, const std::vector<GuildRunStop>& route, Map* map, size_t index);

private:
    GuildRunStates() = default;

    struct State
    {
        mutable std::mutex lock;
        uint32 group = 0;          // the group's id
        uint32 leader = 0;         // the leader's guid
        uint32 mapId = 0;
        uint32 instanceId = 0;
        std::shared_ptr<const GuildPacks::Route> packs;
        std::vector<GuildRunStop> route; // the packs' stops with the places the controller moves bots to
        size_t index = 0;
        bool controls = false;
        bool inside = true;        // the job's last word: its party is in on its bosses
        bool stepDue = false;      // and a step of its tables is due
        GuildRunPull pull = GuildRunPull::Approach;
        ObjectGuid pulled;
        time_t pullAt = 0;
        time_t stopAt = 0;
        time_t gatherSince = 0;
        uint32 pullTries = 0;
        time_t waitSince = 0;
        bool waited = false;
        bool ownSpot = false;
        time_t changedAt = 0;      // the stop or the pull last changed
        std::vector<std::pair<std::string, std::string>> notes; // the leader's events, for the tick to say
        time_t reportedPullAt = 0; // the pull the tick last reported as an event
        time_t stallReportedAt = 0;
    };

    std::shared_ptr<State> Find(uint32 instanceId) const;
    // The stop's per-stop fields back to their start, for a new stop. Under the state's lock.
    static void Reset(State& state, size_t index);
    // The leader's write, under the state's lock, when it is the leader. False otherwise.
    std::shared_ptr<State> LeaderState(Player* leader) const;

    // Where a held leader stands and what keeps it: loot, its AI's fight state, and each member not ready for the pull.
    static std::string StallReason(Player* leader, size_t index, const GuildRunStop& stop, GuildRunPull pull, bool ownSpot);
    // The route GuildPacks gives for the dungeon at the leader's level and faction, its stops with their places, and the
    // first stop ahead of the leader. On the world thread, outside map updates, as GetRoute and the navmesh need.
    static std::vector<GuildRunStop> BuildRoute(Player* leader, const std::string& dungeon,
        std::shared_ptr<const GuildPacks::Route>& packs, std::string& error);

    mutable std::mutex registryLock;
    std::map<uint32, std::shared_ptr<State>> byInstance;
};

#define sGuildRunStates GuildRunStates::instance()
