#pragma once

#include "Common.h"
#include "Entities/ObjectGuid.h"
#include "playerbot/GuildPacks.h"
#include "playerbot/GuildRunCore.h"
#include "playerbot/WorldPosition.h"

#include <atomic>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <tuple>
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
// Blocked: the party could not get ready in its budget, or a member lacks what waiting cannot mend; the leader wrote a
// block, and nothing pulls until the tick hands the party to the job for a recovery (docs/run-contract.md, "A block").
enum class GuildRunPull : uint8 { Approach, Gather, Pull, Wait, Blocked };

using GuildRunMode = runrules::Mode;
using GuildRunControl = runrules::Control;
using GuildRunStamp = runrules::Stamp;

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
    GuildRunMode mode = GuildRunMode::Ended;
    GuildRunStamp stamp;       // the run's generation and plan revision
    bool choose = false;       // the leader is to choose its stop again from where it stands
    uint32 instanceId = 0;
    std::vector<uint32> expected; // the members the run waits for, the leader among them
    // In hold: where this bot holds, when it is a living member the engine plays inside the instance and not walking.
    bool holds = false;
    ai::WorldPosition holdAt;
    // Apart from its party (docs/run-contract.md, "Rejoining"), in any mode; walking back to it over the route's checked
    // way, the next part of the walk as points and the creatures near it that were dead when it was checked.
    bool apart = false;
    bool walking = false;
    uint32 walkId = 0;
    size_t walkPart = 0;
    std::vector<ai::WorldPosition> walkPoints;
    std::vector<uint32> deadNear;
    // Whether the party may start a fight, as of the last reconcile; the live check is GuildRunStates::PullAllowed.
    bool permit = false;
    // Readiness (docs/run-contract.md, "Readiness"): the members apart from the party, the waiting budget, the keepers as
    // the leader last said them, and whether the job has the party for a recovery after a block.
    std::vector<uint32> apartMembers;
    runrules::Budget budget;
    std::string keepers;
    bool recovering = false;
    // The expected members a person plays (docs/plan.md, item 6), and since when only a person has kept the pull back, the
    // waiting budget frozen for as long (runplayer::FreezeBudget), or 0.
    std::vector<uint32> persons;
    int64_t personWaitSince = 0;
    // A fight the leader did not pull is on, as one a person started: defended, the planned pull cancelled.
    bool unplannedFight = false;
};

// The shared state of a guild party's run inside one dungeon instance, which the party's AI plays by: the route of
// packs from GuildPacks, the stop the party is at, and where the leader's pull stands. See docs/dungeon-runs.md.
//
// Who writes what (docs/run-contract.md): the guild tick creates a run state when the run's party is inside, and writes
// its mode and the reason for it, the generation, the expected members and the hold places, in runcore::RunCore. It
// reconciles every run it keeps on each tick, by the run, so a run whose leader is dead, outside, offline or a player's is
// still looked at. The leader's AI writes the stop, the pull phase and the last stop it saw cleared, only in the mode lead
// and under the stamp of the trigger that fired it; each such write counts a plan revision. Members write nothing.
// The tick runs on the world thread before the maps update, and a bot's AI inside its map's update, so the two never
// run at once. A member on another map, such as a ghost outside, can read while the leader writes, so each run state
// has its own lock, held only to copy plain values in or out. The registry has its own lock too, never taken while a
// state's is held. Neither is taken while GuildDungeon's lock is held, nor that one while either is.
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
    static constexpr float PARTY_NEAR = runrules::READY_NEAR;
    static constexpr uint32 PULL_HEALTH = runrules::READY_HEALTH;
    static constexpr uint32 PULL_MANA = runrules::READY_MANA;
    // How long the controller may hold a leader still, out of a fight, before the tick says why.
    static constexpr time_t STALL_SECONDS = 60;
    // How long the party waits at the stop before for another pack's creature to leave, before it pulls anyway.
    static constexpr time_t WAIT_SECONDS = 90;
    // The margin on a creature's reach, as GuildPacks measures a pull spot's.
    static constexpr float REACH_MARGIN = 3.0f;

    // From the guild tick, for a dungeon leader, outside GuildDungeon's lock, on either side of the job's own tick.
    // Before it: with the leader in a dungeon's instance, the run state is made the first time, from GuildPacks' route
    // at the first stop with a living pack whose pull spot is not behind the party (FirstLiveStop by position). While the
    // job has the party, the stop follows the packs it kills. A leader outside its instance leaves the run as it is.
    // Returns what the job is told: no run, or the run's mode (GuildRunRules' Job table says what the job may do by it).
    // `order` is the leader's job order: a run that failed for it (Failure) is not made again for it.
    GuildRunControl Prepare(Player* leader, const std::string& dungeon, uint32 order,
        std::vector<std::pair<std::string, std::string>>& events);
    // Whether the leader's party may start a fight (runrules::PullPermit): true with no run. Before the job's tick, which
    // takes it as a parameter, so the job never asks for it under its own lock.
    bool Permit(uint32 leaderGuid) const;
    // A run that ended failed, as cannot_rejoin, for the leader's job order: kept, past the run's removal, until the guild
    // manager has failed the party's jobs and says so (Acknowledge).
    bool Failure(uint32 leaderGuid, uint32 order, std::string& reason, runplayer::Outcome* outcome = nullptr) const;
    // A change of control at its boundary (GuildBotMgr::TransferControl), on the world thread: the member's run moves go
    // first, when it goes to a person, then the run counts a revision and remembers who plays it. Any run expecting it.
    void ControlChanged(uint32 guid, runplayer::Controller to, const std::string& why);
    // The member's run move and walk, removed with no lock held, on the world thread: the first step of a takeover.
    void CancelMoves(uint32 guid);
    // Whether any run expects this character: a reservation for its person lasts only while one does.
    bool Expects(uint32 guid) const;
    void Acknowledge(uint32 leaderGuid, uint32 order);
    // `bench clearroute`: kills every creature of the route's stops before `stop` (1 on), their pulls' extras and their
    // reachers, in the leader's instance only, then the creatures the model does not have along the cleared way, and again
    // on the tick for SWEEP_SECONDS for summons that come late. World thread. The lines to print.
    std::list<std::string> ClearRoute(Player* leader, uint32 stop);
    // After it: the job's report. Events for the job: a new pull, and a leader the controller holds still for a minute.
    std::vector<std::pair<std::string, std::string>> Settle(Player* leader, const runrules::JobReport& report);
    // The job ended, changed or failed: the leader's run ends in order, and is removed two ticks on.
    void End(uint32 leaderGuid, const std::string& why);
    // The guild tick, on the world thread before the maps update, once a tick and before the jobs: every run it keeps.
    // Returns its events, as (the leader's guid, kind, detail).
    std::vector<std::tuple<uint32, std::string, std::string>> Reconcile();
    // The console's `run state`: every run kept, ended ones and those with a leader offline included, from what each state
    // stores. Reads only.
    std::list<std::string> Describe(const std::string& leaderName) const;

    // Any thread: the run state as one bot sees it.
    GuildRunView View(Player* bot) const;
    // Where a member's run move takes it, from a view: its place in hold, its stand at the stop, or, on the way, the leader.
    // Empty when there is none.
    static ai::WorldPosition PlaceOf(const GuildRunView& view, Player* bot);
    // Whether a run action fired under this stamp may still act, in a fresh view of the bot's run (GuildRunRules'
    // Accepts). A refusal is counted for the console.
    bool Current(Player* bot, const GuildRunView& view, const GuildRunStamp& stamp, bool leaderWrite);
    // Counts a run action refused for a stale stamp, for the console.
    void Refused(Player* bot);
    // Whether every expected member of the leader's run is there for a pull (GuildRunRules' Present), looked up through
    // the leader's own map, which is the run's instance. On the leader's map thread.
    static bool Present(Player* leader, const GuildRunView& view);
    // Whether the bot's party may start a fight now: no run, or the run's permit, every expected member alive inside
    // through the bot's own map, the pull phase not blocked, and nobody keeping the pull back (Readiness). A bot already in a
    // fight is not held to readiness: it fights what came. On the bot's map thread, right before a pull, a kill step's
    // attack or an event start.
    bool PullAllowed(Player* bot) const;
    // Who keeps the pull back, and why (runrules::Readiness), through the bot's own map: at the stop's fight spot in lead,
    // else around the leader, where the job gathers the party.
    static std::vector<runrules::Kept> Readiness(Player* bot, const GuildRunView& view);
    // The keepers' text for events, name:why.
    static std::string KeepersText(const std::vector<runrules::Kept>& kept);
    // For the job's recovery after a block: the block's id; the members with a supplies obligation, once the party has
    // walked the checked way out to the entrance; and whether that walk is still to come or under way. False with none.
    bool Recovery(uint32 leaderGuid, uint32& id, std::vector<uint32>& supplies, bool& walkingOut) const;

    // A member's run move, on its own map thread with no lock held (move to run spot), by runcore::RunMoves::Start. Its
    // previous run move and any run token no record holds are removed from its motion stack first; a removal deferred by
    // the core postpones the move, the previous record kept, and a run move a removal's callback started stands. Then,
    // with the view's run and stamp still the bot's, `move` is called with a new token (GuildRunRules' NextToken), and the
    // move is recorded only if a move with that token is then on top. Returns whether a move started.
    bool StartRunMove(Player* bot, const GuildRunView& view, const std::function<bool(uint32 token)>& move);
    // The same, with `current` in place of the view's check; for the self-test, whose characters have no run.
    bool StartRunMove(Player* bot, const GuildRunView& view, const std::function<bool()>& current,
        const std::function<bool(uint32 token)>& move);
    // Every run move's record, on the world thread between map updates: retired once its move is gone, kept while current,
    // its token removed when stale, or left for the next tick (GuildRunRules' RecordStep). The only pass that reaches a
    // member outside its run's instance. Each tick, and again after the jobs.
    void CleanMoves();
    // The member's run move, for the console: its token and stamp, or 0.
    uint32 RunMoveOf(uint32 guid, GuildRunStamp& stamp) const;

    // The leader's AI, on its map's thread, each under the stamp of the trigger that fired it, and only in the mode lead:
    // it stands at its pull spot, it pulled a creature, a pull brought nothing. Each that changes something counts a
    // plan revision, with its own event.
    void Gather(Player* leader, const GuildRunStamp& stamp);
    void Pulled(Player* leader, const GuildRunStamp& stamp, ObjectGuid creature);
    void PullFailed(Player* leader, const GuildRunStamp& stamp);
    // The leader blocks the pull with these reasons: its pull phase blocked, a plan revision, and a block for the tick to
    // take once. Never empty.
    void Block(Player* leader, const GuildRunStamp& stamp, const std::vector<runrules::Kept>& kept);
    // Who keeps the pull back now, as telemetry: no plan revision, and run_keep said only when the list changes.
    void Keepers(Player* leader, const GuildRunStamp& stamp, const std::vector<runrules::Kept>& kept);
    // On to the first stop with a living pack from the current one, or past it with skip, as for a pack that pulls
    // bring nothing of; a stop passed over is passed again. Without skip, the current stop's pack is dead: the leader saw
    // it cleared. Looks at the leader's own map, so only from its map's thread.
    void Advance(Player* leader, const GuildRunStamp& stamp, bool skip);
    // After a hold, a hand-back or a change of members: the first living stop ahead of where the leader stands, as for a
    // party set down mid-route. The tick never chooses a stop.
    void Choose(Player* leader, const GuildRunStamp& stamp);
    // It waits at the stop before for another pack's wanderer or patrol to leave; it goes on when none is in reach, or when
    // it has waited WAIT_SECONDS; it pulls its pack from its own spot after the stop before's failed; it passes the stop over.
    void Wait(Player* leader, const GuildRunStamp& stamp, const std::string& detail);
    void Go(Player* leader, const GuildRunStamp& stamp);
    void WaitedOut(Player* leader, const GuildRunStamp& stamp, const std::string& detail);
    void UseOwnSpot(Player* leader, const GuildRunStamp& stamp, const std::string& detail);
    void Skip(Player* leader, const GuildRunStamp& stamp, const std::string& detail);

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
        runcore::RunCore core;     // the mode, generation, plan revision, members and holds
        int32 lastCleared = -1;    // the last stop the leader saw cleared, or -1
        std::string dungeon;
        std::map<uint32, uint32> refused; // run actions refused for a stale stamp, by bot
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
        time_t reportedPullAt = 0; // the pull the tick last reported as an event
        time_t stallReportedAt = 0;
        uint32 order = 0;          // the leader's job order the run was made for
        bool failureKept = false;  // its failure is in `failures`
        bool unplannedFight = false; // in lead, a fight the leader did not pull is on (runplayer::UnplannedFight)
        std::map<size_t, bool> wayChecked; // the route's way, by its first point, WAY_CHECK_POINTS at a time: on the navmesh
        // Readiness: the waiting budget, the keepers as last said, and the block the leader wrote for the tick to take.
        runrules::Budget budget;
        std::string keepers;
        int64_t personWaitSince = 0;  // only a person keeps the pull back since then: the budget is frozen
        time_t toldAt = 0;            // the leader last said in party chat what it waits for
        std::string told;
        uint32 blockId = 0;
        uint32 blockGeneration = 0;
        uint32 blockStop = 0;      // the blocked stop's pack
        std::vector<runrules::Kept> blockReasons;
        std::string blockText;
    };

    std::shared_ptr<State> Find(uint32 instanceId) const;
    // The run state of a group in any instance, or of a leader, or none.
    std::shared_ptr<State> FindGroup(uint32 groupId) const;
    std::shared_ptr<State> FindLeader(uint32 leaderGuid) const;
    // The stop's per-stop fields back to their start, for a new stop. Under the state's lock.
    static void Reset(State& state, size_t index);
    // A leader's write: the leader in the world and not between maps, its state found by its instance, then under the
    // state's lock the mode lead and the stamp the state's; only then is the map looked at and the change made. A change
    // counts a plan revision with an event; a refusal is counted.
    bool Write(Player* leader, const GuildRunStamp& stamp, const char* what,
        const std::function<bool(State&, Map*, std::string&)>& write);
    static const char* PullName(GuildRunPull pull);
    // Who keeps the pull back, written and said as run_keep, as telemetry with no plan revision. Under the state's lock.
    static void SayKeepers(State& state, const std::string& text);
    // A leader's write changed the plan: on the instance's map thread, with no lock held, the run's stale run moves of
    // members on that map are removed at once; the rest wait for CleanMoves.
    void StopStaleMoves(Map* map, uint32 leaderGuid, const runcore::RunNow& run);

    // Where a held leader stands and what keeps it: loot, its AI's fight state, and each member not ready for the pull.
    static std::string StallReason(Player* leader, size_t index, const GuildRunStop& stop, GuildRunPull pull, bool ownSpot);
    // The route GuildPacks gives for the dungeon at the leader's level and faction, its stops with their places, and the
    // first stop ahead of the leader. On the world thread, outside map updates, as GetRoute and the navmesh need.
    static std::vector<GuildRunStop> BuildRoute(Player* leader, const std::string& dungeon,
        std::shared_ptr<const GuildPacks::Route>& packs, std::string& error);

    mutable std::mutex registryLock;
    std::map<uint32, std::shared_ptr<State>> byInstance;
    // Leaders and instances whose dungeon has no route, told once: the job has the party alone there, as before.
    std::set<std::pair<uint32, uint32>> unrouted;
    // Failed runs, until the manager acknowledges them.
    runcore::Failures failures;
    // Path searches left in this tick for every run's walks (runrules::PLAN_BUDGET).
    int planBudget = 0;
    // bench clearroute's sweep of the creatures the route's model does not have, along the cleared way, kept for
    // SWEEP_SECONDS so that a summon a script brings late is taken too. Under the registry's lock.
    static constexpr time_t SWEEP_SECONDS = 10;
    struct Sweep
    {
        uint32 leader = 0;
        uint32 mapId = 0;
        uint32 instanceId = 0;
        std::shared_ptr<const GuildPacks::Route> packs;
        std::vector<runrules::Vec> way;
        time_t until = 0;
        uint32 killed = 0;
    };
    std::vector<Sweep> sweeps;
    // Kills the living hostile creatures the model does not have within reach of the way; their names in `names`.
    static uint32 SweepUnmodelled(Map* map, const GuildPacks::Route& packs, const std::vector<runrules::Vec>& way,
        std::string& names);
    // On the world thread, each tick: every pending sweep once more, its kills as bench_clear_route events.
    void RunSweeps(std::vector<std::tuple<uint32, std::string, std::string>>& events);
    // The generations of every run, never reused while the server runs, told with its start.
    struct Counter : runcore::Generations
    {
        std::atomic<uint32> next{ 0 };
        uint32 Next() override { return ++next; }
        uint64 Start() const override;
    };
    Counter generations;
    // Run moves, by member guid, apart from the run states, so that a run's moves are cleaned up after it ends. Their own
    // lock, taken after a state's and never before it, and never held across a removal or a move.
    runcore::RunMoves moves;
};

#define sGuildRunStates GuildRunStates::instance()
