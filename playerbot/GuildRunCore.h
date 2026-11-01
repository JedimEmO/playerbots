#pragma once

// The lifecycle of a party's run (docs/run-contract.md): its mode and reason, generation and plan revision, expected
// members, holds and end, as the guild tick and the job's reports change them. It reaches the world only through
// RunWorld, so that tests/run_lifecycle drives it with a fake world and watches every lookup and move; GuildRunStates
// keeps one inside each run state, under that state's lock, with a RunWorld of the run's own instance.
// RunMoves keeps the run moves' records apart from the run states, and reaches the members' motion through MoveWorld.

#include "GuildRunRules.h"

#include <cstdint>
#include <ctime>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace runcore
{
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

    struct Held
    {
        float x = 0.0f, y = 0.0f, z = 0.0f;
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
        time_t leaderBackAt = 0;             // in hold: when every reason went, or 0
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
        // A leader's write or the tick's own stop change counts a plan revision, said as run_plan.
        void Revise(const std::string& what, const std::string& fields);
        // An event of the run's, its generation and revision first, so that a cut detail keeps them.
        void Note(const std::string& kind, const std::string& fields);

        runrules::Stamp Stamp() const { return { generation, plan }; }
        runrules::Control Control() const { return runrules::ControlOf(mode); }

    private:
        void SetMode(RunWorld& world, Generations& generations, runrules::Mode to, const std::string& why);
        void Place(RunWorld& world, const std::vector<uint32_t>& members);
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
        bool Find(uint32_t guid, MoveRecord& record) const;
        size_t Size() const;

    private:
        uint32_t TokenOf(uint32_t guid) const;

        mutable std::mutex lock;
        std::map<uint32_t, MoveRecord> records;
        uint32_t counter = 0;
    };
}
