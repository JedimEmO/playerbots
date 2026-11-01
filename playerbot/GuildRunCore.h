#pragma once

// The lifecycle of a party's run (docs/run-contract.md): its mode and reason, generation and plan revision, expected
// members, holds and end, as the guild tick and the job's reports change them. It reaches the world only through
// RunWorld, so that tests/run_lifecycle drives it with a fake world and watches every lookup and move; GuildRunStates
// keeps one inside each run state, under that state's lock, with a RunWorld of the run's own instance.
// RunMoves keeps the run moves' records apart from the run states, and reaches the members' motion through MoveWorld.

#include "GuildRunRules.h"
#include "GuildRunPlayer.h"

#include <cstdint>
#include <ctime>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace runcore
{
    struct Held
    {
        float x = 0.0f, y = 0.0f, z = 0.0f;
    };

    inline runrules::Vec VecOf(const Held& at)
    {
        return { at.x, at.y, at.z };
    }

    // A member's walk back to its party over the route's checked way, in parts of one run move each.
    struct Walk
    {
        uint32_t id = 0;                              // new with each plan
        std::vector<std::vector<runrules::Vec>> chunks;
        size_t chunk = 0;                             // the part being walked
        Held to;                                      // where it ends
        std::vector<uint32_t> deadNear;               // creatures near it that were dead when it was checked
        time_t chunkSince = 0;                        // when the member began the current part
    };

    // What the run asks of the world, all for the run's own map and instance. On the world thread, between map updates.
    class RunWorld
    {
    public:
        virtual ~RunWorld() = default;
        // The group's members and its leader, by the group's id; false when the group is gone.
        virtual bool Group(uint32_t groupId, std::vector<uint32_t>& members, uint32_t& leader) = 0;
        virtual runrules::Facts FactsOf(uint32_t guid) = 0;
        virtual std::string ActiveJob(uint32_t guid) = 0;
        virtual std::string Name(uint32_t guid) = 0;
        // Where a living member the engine plays stands, found through the run's instance; false otherwise.
        virtual bool Position(uint32_t guid, float& x, float& y, float& z) = 0;
        // Stops a living member's follow, and its point or path moves that are not the run's, on top or beneath, and puts back
        // the chase it fought by; only for one the engine plays, found through the run's instance, in the world and not
        // between maps. Called for a member as it is placed in hold.
        virtual void CancelMotion(uint32_t guid) = 0;

        // The rest spot after the last stop the leader saw cleared, when every stop up to it is dead and the spot is out of
        // every living creature's reach (PlaceClear); false otherwise.
        virtual bool HoldPlace(Held& place) = 0;
        // Whether a place is out of the reach of every creature that may be alive, and of every patrol's path.
        virtual bool PlaceClear(const Held& at) = 0;
        // The point of the route's way at the current stop.
        virtual bool StopWayPoint(Held& at) = 0;
        // The route's way point nearest a place, within runrules::WAY_NEAR; false when the place is off the way.
        virtual bool WayPointNear(const Held& at, Held& point) = 0;
        // A walk from where the member stands to `to` over the route's way, checked against every creature's reach and
        // the navmesh; Pending when the tick's path searches are spent.
        virtual runrules::PlanResult PlanWalk(uint32_t guid, const Held& to, Walk& walk, std::string& why) = 0;
        // Whether any of these creatures is alive again.
        virtual bool Respawned(const std::vector<uint32_t>& spawns) = 0;
        // Whether a living creature the route's model does not have, as a summon, now reaches a part of a walk.
        virtual bool CreatureNear(const std::vector<runrules::Vec>& part) = 0;
        // Whether the member's motion holds a run move walking its way.
        virtual bool Walking(uint32_t guid) = 0;
        // The point of the route's way at the instance's entrance, where a supply trip's walk out ends; false with none.
        virtual bool ExitPoint(Held& at) = 0;
        // A member as readiness sees it (runrules::Ready), for a recovery's obligations: its distance is to the leader.
        // False when it cannot be seen, as one offline; its facts are FactsOf's either way.
        virtual bool ReadyOf(uint32_t guid, uint32_t leader, runrules::Ready& ready) = 0;
    };

    // A member apart from its party: dead, outside, offline or a player's (missing), back but not yet with the party, new in
    // it, or far from the leader. In hold every member is apart until it reaches the anchor.
    struct Apart
    {
        time_t since = 0;
        bool missing = false;
        bool human = false;        // played by a person: never walked, and never counted for cannot_rejoin
        bool cohort = false;       // of the hold's cohort, walking to the anchor within its minute
        time_t walkSince = 0;      // when its first walk began, for the cohort's minute
        bool walking = false;
        Walk walk;
        time_t plannedAt = 0;      // when a plan was last tried
        std::string blocked;       // why the last plan failed
    };

    // The generation counter, shared by every run, and the server's start, which a generation is told with.
    class Generations
    {
    public:
        virtual ~Generations() = default;
        virtual uint32_t Next() = 0;
        virtual uint64_t Start() const = 0;
    };

    struct Event
    {
        std::string kind;
        std::string detail;
    };

    class RunCore
    {
    public:
        uint32_t group = 0;
        uint32_t leader = 0;
        runrules::Mode mode = runrules::Mode::Lead;
        runrules::Mode heldFrom = runrules::Mode::Lead; // the mode a hold began in, which it resumes to
        std::string reason;
        uint32_t generation = 0;
        uint32_t plan = 0;
        uint32_t membership = 0;
        std::vector<uint32_t> expected;
        bool choose = false;                 // the leader chooses its stop again from where it stands
        std::map<uint32_t, Held> held;       // in hold, each living member the engine plays inside: where it holds
        std::map<uint32_t, Apart> apart;     // the members apart from the party, in every mode
        std::vector<uint32_t> settled;       // the expected members alive inside and not apart, as of the last reconcile,
                                             // a person's member with the party among them
        bool hasAnchor = false;              // in hold: the party's place, fixed for the hold
        Held anchor;
        std::string anchorKind;
        bool hasLeaderWay = false;           // in lead and released: the leader's nearest way point, as last seen
        Held leaderWay;
        std::map<uint32_t, time_t> lastDead; // when each member was last seen dead
        time_t apartSince = 0;               // since when a member is apart with nothing else holding the run
        bool failed = false;                 // ended because the party could not regroup, or blocked at one stop too often
        bool stopped = false;                // ended because its person was gone too long (player_gone): not a failure
        // Who plays each expected member, kept while it is offline, so a person's disconnect is never taken for a bot gone;
        // each person's time offline; each bot's own time apart, for cannot_rejoin.
        std::map<uint32_t, runplayer::Controller> controllers;
        std::map<uint32_t, runplayer::OfflineClock> offline;
        std::map<uint32_t, runplayer::BotClock> botClocks;
        // Blocks (docs/run-contract.md, "A block"): the last one taken, the count at each stop by its pack, and the
        // recovery under way: its obligations, kept across a hold, and the reasons it was for.
        uint32_t lastBlock = 0;
        std::map<uint32_t, uint32_t> blockCounts;
        bool recovering = false;
        std::vector<runrules::Obligation> obligations;
        std::string recoveryReasons;
        // A supply trip's walk out, over the checked way to the entrance: wanted for a block naming supplies, then walking
        // (every member apart, its target the exit point), then done, when the job takes the party out.
        bool exitWanted = false;
        bool exiting = false;
        bool exitDone = false;
        time_t exitAsked = 0;
        Held exitPoint;
        std::vector<uint32_t> cancels;       // members whose run move the tick removes once the state's lock is let go
        uint32_t walkIds = 0;
        bool endSeen = false;
        std::vector<std::string> holdReasons; // as the last reconcile found them, in any mode
        runrules::JobReport job;             // the job's last report
        bool hasNext = true;                 // a living pack ahead, as the last Prepare found
        uint64_t start = 0;
        std::vector<Event> events;           // to say, with the leader's guid

        // A new run, in lead, its expected members the group's.
        void Begin(RunWorld& world, Generations& generations, uint32_t groupId, uint32_t leaderGuid,
            const std::vector<uint32_t>& members);
        // The guild tick's reconcile. False once the run is to be removed: a tick after it ended. An ended run looks at
        // nothing in the world.
        bool Reconcile(RunWorld& world, Generations& generations, time_t now);
        // Before the job's tick: what it is told. A run with no living pack ahead is released.
        runrules::Control Prepare(RunWorld& world, Generations& generations, bool livingAhead);
        // After it: the job's report. Lead is released for a step or the leaving; released is handed back, to hold
        // when a reason stands, else to lead, once the job reports the party on its bosses with no step due.
        void Settle(RunWorld& world, Generations& generations, const runrules::JobReport& report);
        // The job ended, changed or failed: mode ended, every member's motion stopped.
        void End(RunWorld& world, Generations& generations, const std::string& why);
        // A change of control at its boundary (GuildBotMgr::TransferControl): a person took the member over, or it was
        // handed back to the engine. A plan revision, so every run action queued before it is refused.
        void ControlChanged(RunWorld& world, uint32_t guid, runplayer::Controller to, const std::string& why);
        // Whether a person plays the member, by what is known of it: one online and not the engine's is a person's, one the
        // engine plays is the engine's, and one offline is what it was.
        bool IsPerson(uint32_t guid, const runrules::Facts& facts);
        // A leader's write or the tick's own stop change counts a plan revision, said as run_plan.
        void Revise(const std::string& what, const std::string& fields);
        // An event of the run's, its generation and revision first, so that a cut detail keeps them.
        void Note(const std::string& kind, const std::string& fields);

        // The leader's block, taken once, by its id, and only in lead under the generation it was written in: counted at its
        // stop, then a recovery (released, Control Recover) or, at the third, the run's end, failed, with no recovery.
        void TakeBlock(RunWorld& world, Generations& generations, uint32_t id, uint32_t blockGeneration, uint32_t stopKey,
            const std::vector<runrules::Kept>& reasons, const std::string& text, time_t now);
        // The leader saw this stop cleared: its count of blocks goes.
        void StopCleared(uint32_t stopKey) { runrules::StopCleared(blockCounts, stopKey); }

        runrules::Stamp Stamp() const { return { generation, plan }; }
        runrules::Control Control() const
        {
            return mode == runrules::Mode::Released && recovering ? runrules::Control::Recover : runrules::ControlOf(mode);
        }
        // Whether the party may start a fight now, as of the last reconcile (runrules::PullPermit).
        bool Permit() const;

    private:
        void SetMode(RunWorld& world, Generations& generations, runrules::Mode to, const std::string& why);
        void Place(RunWorld& world, const std::vector<uint32_t>& members);
        // A new hold: every member apart, the cohort walking to the anchor, which is chosen here.
        void BeginHold(RunWorld& world, time_t now);
        // Who is apart, where each walks and how far it got, and who is settled.
        void Track(RunWorld& world, const std::vector<uint32_t>& members, time_t now);
        // Nobody apart and no walk: on End, and when the job takes the party out.
        void ClearApart();
        // Each bot's own time apart (runplayer::BotClock), counted only in hold with nothing holding the run.
        void TickClocks(RunWorld& world, time_t now, bool holding);
        void StopWalk(uint32_t guid, Apart& away);
        // In released for a recovery: the obligations against the members as they are now, and back to lead, with a new
        // generation, once every one is met or one unmet is past its deadline.
        void CheckRecovery(RunWorld& world, Generations& generations, time_t now);
        // The supply trip's way out: checked from the leader to the exit point, then walked by every member, or none.
        void PlanExit(RunWorld& world, time_t now);
    };

    // A member as a pass over the run moves sees it.
    struct MoveMember
    {
        bool visible = false;  // the pass may read and change its motion: any member on the world pass, only one on the
                               // pass's own map on a map's pass
        bool online = false;
        bool engine = false;   // played by the engine, not a player
        bool inWorld = false;
        bool teleporting = false;
        uint32_t mapId = 0;
        uint32_t instanceId = 0;
    };

    // The members' motion stacks, as the run moves reach them. RemoveMove may run a Finalize whose callback starts a move
    // and records it in the same RunMoves.
    class MoveWorld
    {
    public:
        virtual ~MoveWorld() = default;
        virtual MoveMember Member(uint32_t guid) = 0;
        virtual bool HasMove(uint32_t guid, uint32_t token) = 0;
        // The id of the move on top of the stack, or 0.
        virtual uint32_t TopMove(uint32_t guid) = 0;
        // A copy of the ids in the stack.
        virtual std::vector<uint32_t> MoveIds(uint32_t guid) = 0;
        virtual runrules::Removal RemoveMove(uint32_t guid, uint32_t token) = 0;
    };

    // Runs that ended failed, by leader, with the job order they were made for and why: kept apart from the run states, so
    // a failure outlives its run's removal, until the guild manager has failed the party's jobs and acknowledges it.
    class Failures
    {
    public:
        // Failed, or stopped as player_gone, which is neither done nor failed (runplayer::Outcome).
        void Record(uint32_t leader, uint32_t order, const std::string& why,
            runplayer::Outcome outcome = runplayer::Outcome::Failed);
        bool Find(uint32_t leader, uint32_t order, std::string& why, runplayer::Outcome* outcome = nullptr) const;
        void Acknowledge(uint32_t leader, uint32_t order);

    private:
        struct Ended
        {
            uint32_t order = 0;
            std::string why;
            runplayer::Outcome outcome = runplayer::Outcome::Failed;
        };
        mutable std::mutex lock;
        std::map<uint32_t, Ended> failed;
    };

    // A run move: its token, the stamp it was issued under, where the member stood, and the run's leader.
    struct MoveRecord
    {
        uint32_t token = 0;
        runrules::Stamp stamp;
        uint32_t mapId = 0;
        uint32_t instanceId = 0;
        uint32_t leader = 0;
    };

    // A run as a pass over the run moves sees it.
    struct RunNow
    {
        runrules::Stamp stamp;
        runrules::Mode mode = runrules::Mode::Ended;
        uint32_t instanceId = 0;
        std::vector<uint32_t> expected;
    };

    // The run moves' records, one live run move a member at most, under their own lock. The lock is never held across a
    // removal or a move, only across lookups in a stack.
    class RunMoves
    {
    public:
        // A member's run move: its previous run move removed, then any run token in its stack no record holds. A removal
        // the core deferred postpones the move, the record kept. A run move a removal's callback started and recorded
        // meanwhile stands, and nothing is issued. Then, with `current` still true, `move` is called with a new token, and
        // the move is recorded only if the token's move is then on top, the member in the world and not between maps. A run
        // move a callback recorded during `move` stands as well: this one is removed and not recorded.
        bool Start(MoveWorld& world, uint32_t guid, const runrules::Stamp& stamp, uint32_t leader,
            const std::function<bool()>& current, const std::function<bool(uint32_t token)>& move);
        // Each record the pass may see, by RecordStep against its run in `runs`, by leader: retired once its move is gone,
        // kept while current, its token removed when stale, or left. With `leader`, only that run's records.
        void Clean(MoveWorld& world, const std::map<uint32_t, RunNow>& runs, uint32_t leader = 0);
        // Retires a member's record if it still holds this token and the token is gone from the member's stack.
        void Retire(MoveWorld& world, uint32_t guid, uint32_t token);
        // Removes a member's run move, whatever its stamp, as a walk planned again does; only for a member the pass may see,
        // in the world and not between maps. With no lock held, since the removal can reach a callback.
        void Cancel(MoveWorld& world, uint32_t guid);
        bool Find(uint32_t guid, MoveRecord& record) const;
        size_t Size() const;

    private:
        uint32_t TokenOf(uint32_t guid) const;

        mutable std::mutex lock;
        std::map<uint32_t, MoveRecord> records;
        uint32_t counter = 0;
    };
}
