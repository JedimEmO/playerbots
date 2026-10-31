#pragma once

// The lifecycle of a party's run (docs/run-contract.md): its mode and reason, generation and plan revision, expected
// members, holds and end, as the guild tick and the job's reports change them. It reaches the world only through
// RunWorld, so that tests/run_lifecycle drives it with a fake world and watches every lookup and move; GuildRunStates
// keeps one inside each run state, under that state's lock, with a RunWorld of the run's own instance.

#include "GuildRunRules.h"

#include <cstdint>
#include <ctime>
#include <map>
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
        // Stops a living member's point, path or follow motion, on top or beneath, and puts back the chase it fought by;
        // only for one the engine plays, found through the run's instance, in the world and not between maps.
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
        void Cancel(RunWorld& world);
        void Place(RunWorld& world, const std::vector<uint32_t>& members);
    };
}
