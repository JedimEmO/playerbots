// Tests of a party's run lifecycle (playerbot/GuildRunCore, playerbot/GuildRunRules), with a fake world that records
// every lookup and motion call. Built and run by tests/run_lifecycle/run.sh, without the server.

#include "GuildRunCore.h"

#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace runcore;
using runrules::Control;
using runrules::Facts;
using runrules::JobAct;
using runrules::JobMay;
using runrules::JobReport;
using runrules::Mode;
using runrules::Motion;

namespace
{
    int failures = 0, checks = 0;

#define CHECK(cond) do { ++checks; if (!(cond)) { ++failures; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

    struct Counter : Generations
    {
        uint32_t next = 0;
        uint32_t Next() override { return ++next; }
        uint64_t Start() const override { return 1730000000; }
    };

    // A world of one instance: the group, each character's facts and place, and the calls the run made.
    struct FakeWorld : RunWorld
    {
        bool groupExists = true;
        std::vector<uint32_t> members;
        uint32_t groupLeader = 0;
        std::map<uint32_t, Facts> facts;
        std::map<uint32_t, std::vector<float>> places;
        std::map<uint32_t, std::string> jobs;
        int groupLookups = 0;
        std::vector<uint32_t> cancelled;
        std::vector<std::string> calls;   // in order: "group", "cancel <guid>"

        bool Group(uint32_t, std::vector<uint32_t>& out, uint32_t& leader) override
        {
            ++groupLookups;
            calls.push_back("group");
            if (!groupExists)
                return false;
            out = members;
            leader = groupLeader;
            return true;
        }
        runrules::Facts FactsOf(uint32_t guid) override { return facts[guid]; }
        std::string ActiveJob(uint32_t guid) override { return jobs.count(guid) ? jobs[guid] : "dungeon"; }
        std::string Name(uint32_t guid) override { return "m" + std::to_string(guid); }
        bool Position(uint32_t guid, float& x, float& y, float& z) override
        {
            if (!places.count(guid))
                return false;
            x = places[guid][0];
            y = places[guid][1];
            z = places[guid][2];
            return true;
        }
        void CancelMotion(uint32_t guid) override
        {
            cancelled.push_back(guid);
            calls.push_back("cancel " + std::to_string(guid));
        }
    };

    runrules::Facts Inside()
    {
        runrules::Facts f;
        f.online = f.engine = f.inWorld = f.inInstance = f.alive = true;
        return f;
    }

    // A party of five, 1 the leader, all alive inside, at the pull spot.
    FakeWorld Party()
    {
        FakeWorld world;
        world.members = { 1, 2, 3, 4, 5 };
        world.groupLeader = 1;
        for (uint32_t guid : world.members)
        {
            world.facts[guid] = Inside();
            world.places[guid] = { float(guid), 0.0f, 0.0f };
        }
        return world;
    }

    RunCore Started(FakeWorld& world, Counter& counter)
    {
        RunCore core;
        core.Begin(world, counter, 7, 1, world.members);
        return core;
    }

    bool HasEvent(const RunCore& core, const std::string& kind, const std::string& part)
    {
        for (const Event& event : core.events)
            if (event.kind == kind && event.detail.find(part) != std::string::npos)
                return true;
        return false;
    }

    void Stamps()
    {
        CHECK(runrules::ParseStamp("12:34").generation == 12);
        CHECK(runrules::ParseStamp("12:34").plan == 34);
        CHECK(runrules::ParseStamp("").generation == 0);
        CHECK(runrules::ParseStamp("x:1").generation == 0);
        CHECK(runrules::ParseStamp(runrules::StampText({ 5, 9 })).plan == 9);

        const runrules::Stamp state{ 3, 7 };
        CHECK(runrules::Accepts(Mode::Lead, state, { 3, 7 }, true));
        CHECK(runrules::Accepts(Mode::Lead, state, { 3, 7 }, false));
        CHECK(!runrules::Accepts(Mode::Lead, state, { 3, 6 }, true));
        CHECK(!runrules::Accepts(Mode::Lead, state, { 2, 7 }, false));
        CHECK(!runrules::Accepts(Mode::Lead, state, { 0, 0 }, false));
        CHECK(runrules::Accepts(Mode::Hold, state, { 3, 7 }, false));
        CHECK(!runrules::Accepts(Mode::Hold, state, { 3, 7 }, true));
        CHECK(!runrules::Accepts(Mode::Released, state, { 3, 7 }, false));
        CHECK(!runrules::Accepts(Mode::Released, state, { 3, 7 }, true));
        CHECK(!runrules::Accepts(Mode::Ended, state, { 3, 7 }, false));
    }

    void HoldReasons()
    {
        std::vector<Facts> five(5, Inside());
        Facts leader = Inside();
        CHECK(runrules::HoldReasons(leader, five, false).empty());

        Facts offline;
        CHECK(runrules::HoldReasons(offline, five, false) == std::vector<std::string>{ "leader_offline" });
        Facts player = Inside();
        player.engine = false;
        CHECK(runrules::HoldReasons(player, five, false) == std::vector<std::string>{ "leader_taken_over" });
        Facts dead = Inside();
        dead.alive = false;
        CHECK(runrules::HoldReasons(dead, five, false) == std::vector<std::string>{ "leader_dead" });
        Facts ghost = dead;
        ghost.inInstance = false;
        CHECK(runrules::HoldReasons(ghost, five, false) == std::vector<std::string>{ "leader_ghost" });
        Facts outside = Inside();
        outside.inInstance = false;
        CHECK(runrules::HoldReasons(outside, five, false) == std::vector<std::string>{ "leader_outside" });
        Facts between = Inside();
        between.teleporting = true;
        CHECK(runrules::HoldReasons(between, five, false) == std::vector<std::string>{ "leader_between_maps" });

        std::vector<Facts> four(4, Inside()), six(6, Inside());
        CHECK(runrules::HoldReasons(leader, four, false) == std::vector<std::string>{ "short_handed" });
        CHECK(runrules::HoldReasons(leader, six, false) == std::vector<std::string>{ "over_five" });

        std::vector<Facts> fallen(5, dead);
        CHECK(runrules::HoldReasons(dead, fallen, false) == (std::vector<std::string>{ "leader_dead", "wipe" }));

        // The repair trip's departure: alive outside is no reason, nor the party out; dead is, and everyone dead is a wipe.
        std::vector<Facts> out(5, outside);
        CHECK(runrules::HoldReasons(outside, out, true).empty());
        CHECK(runrules::HoldReasons(between, out, true).empty());
        CHECK(runrules::HoldReasons(ghost, out, true) == std::vector<std::string>{ "leader_ghost" });
        CHECK(runrules::HoldReasons(offline, out, true) == std::vector<std::string>{ "leader_offline" });
        CHECK(runrules::HoldReasons(outside, four, true) == std::vector<std::string>{ "short_handed" });
        std::vector<Facts> deadOut(5, ghost);
        CHECK(runrules::HoldReasons(outside, deadOut, true) == std::vector<std::string>{ "wipe" });
        CHECK(runrules::HoldReasons(outside, out, false) == (std::vector<std::string>{ "leader_outside", "wipe" }));
    }

    void Resume()
    {
        CHECK(runrules::Resumes(true, 39.0f, 0));
        CHECK(!runrules::Resumes(true, 41.0f, 0));
        CHECK(!runrules::Resumes(true, 41.0f, 179));
        CHECK(runrules::Resumes(true, 41.0f, 180));
        CHECK(runrules::Resumes(true, -1.0f, 180));
        CHECK(!runrules::Resumes(true, -1.0f, 179));
        CHECK(!runrules::Resumes(false, 1.0f, 1000));
    }

    void JobTable()
    {
        // The initial resolution into Leave under lead: the phase is set and the job returns; the leave tick waits for the
        // release, and then acts.
        CHECK(runrules::Job(Control::Lead, JobAct::PhaseOut, true) == JobMay::Request);
        CHECK(runrules::Job(Control::Lead, JobAct::LeaveRepair, true) == JobMay::Return);
        CHECK(runrules::Job(Control::Released, JobAct::LeaveRepair, true) == JobMay::Act);
        CHECK(runrules::Job(Control::Hold, JobAct::PhaseOut, true) == JobMay::Return);
        // Under hold a living leader's trip waits, a dead leader's way back goes on; the way in always does.
        CHECK(runrules::Job(Control::Hold, JobAct::LeaveRepair, true) == JobMay::Return);
        CHECK(runrules::Job(Control::Hold, JobAct::LeaveRepair, false) == JobMay::Act);
        CHECK(runrules::Job(Control::Hold, JobAct::WayBackIn, true) == JobMay::Act);
        // Table steps hand off under lead, act under released, wait under hold.
        CHECK(runrules::Job(Control::Lead, JobAct::Step, true) == JobMay::Request);
        CHECK(runrules::Job(Control::Released, JobAct::Step, true) == JobMay::Act);
        CHECK(runrules::Job(Control::Hold, JobAct::Step, true) == JobMay::Return);
        // Rest, regroup, the via place and boss travel only without a run.
        CHECK(runrules::Job(Control::None, JobAct::MoveOn, true) == JobMay::Act);
        CHECK(runrules::Job(Control::Lead, JobAct::MoveOn, true) == JobMay::Return);
        CHECK(runrules::Job(Control::Released, JobAct::MoveOn, true) == JobMay::Return);
        CHECK(runrules::Job(Control::Hold, JobAct::MoveOn, true) == JobMay::Return);
        // The first tick back on the bosses under released hands back before TickBosses acts.
        CHECK(runrules::Job(Control::Released, JobAct::FirstBossTick, true) == JobMay::Return);
        CHECK(runrules::Job(Control::None, JobAct::FirstBossTick, true) == JobMay::Act);
    }

    void CancelBeneathChase()
    {
        using P = runrules::CancelPlan;
        P plan = runrules::PlanCancel({ Motion::Idle, Motion::Chase }, true);
        CHECK(!plan.clear && !plan.restoreChase);
        plan = runrules::PlanCancel({ Motion::Idle, Motion::Point }, true);
        CHECK(plan.clear && !plan.restoreChase);
        plan = runrules::PlanCancel({ Motion::Idle, Motion::Point, Motion::Chase }, true);
        CHECK(plan.clear && plan.restoreChase);
        plan = runrules::PlanCancel({ Motion::Idle, Motion::Path, Motion::Chase }, false);
        CHECK(plan.clear && !plan.restoreChase);
        plan = runrules::PlanCancel({ Motion::Idle, Motion::Follow }, true);
        CHECK(plan.clear && !plan.restoreChase);
        plan = runrules::PlanCancel({ Motion::Idle, Motion::Chase, Motion::Point }, true);
        CHECK(plan.clear && !plan.restoreChase);
        plan = runrules::PlanCancel({ Motion::Idle, Motion::Other }, true);
        CHECK(!plan.clear);
    }

    void EndAndRetention()
    {
        FakeWorld world = Party();
        Counter counter;
        RunCore core = Started(world, counter);
        const uint32_t generation = core.generation;
        core.End(world, counter, "job_done");
        CHECK(core.mode == Mode::Ended);
        CHECK(core.generation != generation);
        CHECK(world.cancelled.size() == 5);
        CHECK(HasEvent(core, "run_mode", "mode=ended reason=job_done"));
        const int lookups = world.groupLookups;
        world.groupExists = false;
        CHECK(core.Reconcile(world, counter, 100));
        CHECK(!core.Reconcile(world, counter, 101));
        CHECK(world.groupLookups == lookups);
    }

    void Disbanded()
    {
        FakeWorld world = Party();
        Counter counter;
        RunCore core = Started(world, counter);
        world.groupExists = false;
        CHECK(core.Reconcile(world, counter, 100));
        CHECK(core.mode == Mode::Ended);
        CHECK(HasEvent(core, "run_mode", "reason=group_gone"));
        const int lookups = world.groupLookups;
        CHECK(core.Reconcile(world, counter, 101));
        CHECK(!core.Reconcile(world, counter, 102));
        CHECK(world.groupLookups == lookups);
    }

    void EndReasons()
    {
        Counter counter;
        {
            FakeWorld world = Party();
            RunCore core = Started(world, counter);
            world.groupLeader = 2;
            core.Reconcile(world, counter, 1);
            CHECK(core.mode == Mode::Ended && core.reason == "new_group_leader");
        }
        {
            FakeWorld world = Party();
            RunCore core = Started(world, counter);
            world.jobs[1] = "idle";
            core.Reconcile(world, counter, 1);
            CHECK(core.mode == Mode::Ended && core.reason == "job_changed");
        }
        {
            FakeWorld world = Party();
            RunCore core = Started(world, counter);
            world.members = { 2, 3, 4, 5, 6 };
            world.groupLeader = 2;
            core.Reconcile(world, counter, 1);
            CHECK(core.mode == Mode::Ended && core.reason == "leader_left_group");
        }
    }

    void LeaderDiesAndResumes()
    {
        FakeWorld world = Party();
        Counter counter;
        RunCore core = Started(world, counter);
        world.facts[1].alive = false;
        const uint32_t generation = core.generation;
        core.Reconcile(world, counter, 100);
        CHECK(core.mode == Mode::Hold && core.reason == "leader_dead");
        CHECK(core.generation != generation);
        CHECK(core.held.size() == 4);          // the living members, not the dead leader
        CHECK(!core.held.count(1));
        CHECK(HasEvent(core, "run_hold_at", "member=m2 result=arrived"));

        // The ghost goes out: the reason changes, the hold stays, the generation with it.
        world.facts[1].inInstance = false;
        const uint32_t held = core.generation;
        core.Reconcile(world, counter, 110);
        CHECK(core.mode == Mode::Hold && core.reason == "leader_ghost" && core.generation == held);

        // Back alive inside, far from every member: no resume before 180 s; then it resumes, the leader to choose its stop.
        world.facts[1] = Inside();
        world.places[1] = { 500.0f, 0.0f, 0.0f };
        core.Reconcile(world, counter, 200);
        CHECK(core.mode == Mode::Hold);
        core.Reconcile(world, counter, 379);
        CHECK(core.mode == Mode::Hold);
        world.cancelled.clear();
        core.Reconcile(world, counter, 380);
        CHECK(core.mode == Mode::Lead && core.choose && core.reason == "resume_waited");
        CHECK(core.held.empty());
        CHECK(world.cancelled.size() == 5);
    }

    void ResumeNear()
    {
        FakeWorld world = Party();
        Counter counter;
        RunCore core = Started(world, counter);
        world.facts[1].alive = false;
        core.Reconcile(world, counter, 100);
        world.facts[1] = Inside();
        world.places[1] = { 2.0f, 39.0f, 0.0f };   // 39 yd from m2's place
        core.Reconcile(world, counter, 120);
        CHECK(core.mode == Mode::Lead && core.reason == "resume_near");
    }

    void AnotherInstance()
    {
        // Member 5 is in the group but in another instance: never placed and never stopped.
        FakeWorld world = Party();
        world.facts[5].inInstance = false;
        world.places.erase(5);
        Counter counter;
        RunCore core = Started(world, counter);
        world.facts[1].alive = false;
        core.Reconcile(world, counter, 100);
        CHECK(core.mode == Mode::Hold);
        CHECK(!core.held.count(5));
        for (uint32_t guid : world.cancelled)
            CHECK(guid != 5);
    }

    void PlayersAndGhostsUntouched()
    {
        FakeWorld world = Party();
        world.facts[3].engine = false;            // a player
        world.facts[4].alive = false;             // dead
        Counter counter;
        RunCore core = Started(world, counter);
        world.facts[1].alive = false;
        core.Reconcile(world, counter, 100);
        CHECK(!core.held.count(3) && !core.held.count(4));
        for (uint32_t guid : world.cancelled)
            CHECK(guid != 3 && guid != 4);
    }

    void ReplacementInLead()
    {
        FakeWorld world = Party();
        Counter counter;
        RunCore core = Started(world, counter);
        world.calls.clear();
        // 5 leaves and 6 comes in, in one tick; 5 is still inside.
        world.members = { 1, 2, 3, 4, 6 };
        world.facts[6] = Inside();
        world.places[6] = { 6.0f, 0.0f, 0.0f };
        const uint32_t generation = core.generation;
        core.Reconcile(world, counter, 100);
        CHECK(core.mode == Mode::Lead);
        CHECK(core.membership == 1 && core.choose && core.generation != generation);
        CHECK((core.expected == std::vector<uint32_t>{ 1, 2, 3, 4, 6 }));
        // The old roster's moves stopped first, the departed 5 among them, and the newcomer not yet.
        bool five = false, six = false;
        for (const std::string& call : world.calls)
        {
            five = five || call == "cancel 5";
            six = six || call == "cancel 6";
        }
        CHECK(five && !six);
        CHECK(HasEvent(core, "run_members", "membership=1"));
    }

    void ReplacementInHold()
    {
        FakeWorld world = Party();
        Counter counter;
        RunCore core = Started(world, counter);
        world.facts[1].alive = false;
        core.Reconcile(world, counter, 100);
        CHECK(core.held.count(5));
        world.members = { 1, 2, 3, 4, 6 };
        world.facts[6] = Inside();
        world.places[6] = { 6.0f, 0.0f, 0.0f };
        core.Reconcile(world, counter, 110);
        CHECK(core.mode == Mode::Hold && !core.choose);
        CHECK(!core.held.count(5) && core.held.count(6));
        CHECK(core.membership == 1);
    }

    void ShortAndOver()
    {
        FakeWorld world = Party();
        Counter counter;
        RunCore core = Started(world, counter);
        world.members = { 1, 2, 3, 4 };
        core.Reconcile(world, counter, 100);
        CHECK(core.mode == Mode::Hold && core.reason == "short_handed");
        CHECK(core.membership == 0 && !core.held.count(5));
        world.members = { 1, 2, 3, 4, 5, 6 };
        world.facts[6] = Inside();
        core.Reconcile(world, counter, 110);
        CHECK(core.mode == Mode::Hold && core.reason == "over_five" && core.membership == 0);
        // Five again, the same: the hold ends once the leader is near a member's place.
        world.members = { 1, 2, 3, 4, 5 };
        core.Reconcile(world, counter, 120);
        CHECK(core.mode == Mode::Lead && core.membership == 0);
    }

    void ReleaseHandoffs()
    {
        FakeWorld world = Party();
        Counter counter;
        RunCore core = Started(world, counter);
        core.Prepare(world, counter, true);
        JobReport inside;
        inside.inside = true;
        core.Settle(world, counter, inside);
        CHECK(core.mode == Mode::Lead);

        JobReport step = inside;
        step.stepDue = true;
        world.cancelled.clear();
        uint32_t generation = core.generation;
        core.Settle(world, counter, step);
        CHECK(core.mode == Mode::Released && core.reason == "step_due");
        CHECK(core.generation != generation && world.cancelled.size() == 5);
        CHECK(core.Control() == Control::Released);
        CHECK(!runrules::Accepts(core.mode, core.Stamp(), core.Stamp(), true));

        // The tick follows the job's kills: a revision, and its own event.
        const uint32_t plan = core.plan;
        core.Revise("job_kill", "stop=4");
        CHECK(core.plan == plan + 1 && HasEvent(core, "run_plan", "what=job_kill"));

        world.cancelled.clear();
        generation = core.generation;
        core.Reconcile(world, counter, 100);
        core.Prepare(world, counter, true);
        core.Settle(world, counter, inside);
        CHECK(core.mode == Mode::Lead && core.choose && core.reason == "handed_back");
        CHECK(core.generation != generation && world.cancelled.size() == 5);

        // No living pack ahead: released.
        core.Prepare(world, counter, false);
        CHECK(core.mode == Mode::Released && core.reason == "no_living_pack_ahead");
    }

    void RepairTrip()
    {
        FakeWorld world = Party();
        Counter counter;
        RunCore core = Started(world, counter);
        // The repair decision: the job leaves its boss phase, the run is released in the same pass.
        JobReport repair;
        repair.leaving = true;
        core.Settle(world, counter, repair);
        CHECK(core.mode == Mode::Released && core.reason == "job_not_on_bosses");

        // The party walks out: alive outside is no reason to hold during the trip.
        for (uint32_t guid : world.members)
        {
            world.facts[guid].inInstance = false;
            world.places.erase(guid);
        }
        core.Reconcile(world, counter, 100);
        CHECK(core.mode == Mode::Released);

        // The engine lets the leader go mid-trip: the run holds, and remembers it held from released.
        world.facts[1].online = false;
        core.Reconcile(world, counter, 110);
        CHECK(core.mode == Mode::Hold && core.reason == "leader_offline" && core.heldFrom == Mode::Released);
        CHECK(core.held.empty());

        // Back: no member holds a place inside, so the hold resumes 180 s later, to released, the trip going on.
        world.facts[1] = Inside();
        world.facts[1].inInstance = false;
        core.Reconcile(world, counter, 200);
        CHECK(core.mode == Mode::Hold);
        core.Reconcile(world, counter, 380);
        CHECK(core.mode == Mode::Released && !core.choose);

        // A dead leader during the trip holds.
        world.facts[1].alive = false;
        core.Reconcile(world, counter, 390);
        CHECK(core.mode == Mode::Hold && core.reason == "leader_ghost");
        world.facts[1].alive = true;
        core.Reconcile(world, counter, 400);
        core.Reconcile(world, counter, 580);
        CHECK(core.mode == Mode::Released);

        // Back in on the bosses: handed back to lead, the leader to choose its stop.
        for (uint32_t guid : world.members)
        {
            world.facts[guid].inInstance = true;
            world.places[guid] = { float(guid), 0.0f, 0.0f };
        }
        JobReport back;
        back.inside = true;
        core.Reconcile(world, counter, 600);
        core.Prepare(world, counter, true);
        core.Settle(world, counter, back);
        CHECK(core.mode == Mode::Lead && core.choose);
    }

    void HandBackIntoHold()
    {
        // A step release ends while a member is gone: the hand-back goes to hold, not lead.
        FakeWorld world = Party();
        Counter counter;
        RunCore core = Started(world, counter);
        JobReport step;
        step.inside = true;
        step.stepDue = true;
        core.Settle(world, counter, step);
        CHECK(core.mode == Mode::Released);
        world.members = { 1, 2, 3, 4 };
        core.Reconcile(world, counter, 100);
        CHECK(core.mode == Mode::Hold && core.heldFrom == Mode::Released);
        JobReport back;
        back.inside = true;
        core.Settle(world, counter, back);
        CHECK(core.mode == Mode::Hold);
        world.members = { 1, 2, 3, 4, 5 };
        core.Reconcile(world, counter, 110);
        CHECK(core.mode == Mode::Released);
        core.Prepare(world, counter, true);
        core.Settle(world, counter, back);
        CHECK(core.mode == Mode::Lead && core.choose);
    }

    void Dispatch()
    {
        using runrules::Dispatch;
        using runrules::Phase;
        auto decide = [](Control control, Phase phase, bool alive, bool phasedOut, bool onMap, bool partyIn)
        {
            runrules::DispatchFacts facts;
            facts.control = control;
            facts.phase = phase;
            facts.leaderAlive = alive;
            facts.phasedOut = phasedOut;
            facts.onDungeonMap = onMap;
            facts.partyIn = partyIn;
            return runrules::Decide(facts);
        };
        // The resolution straight into Leave under lead or hold: nothing is dispatched until the release.
        CHECK(decide(Control::Lead, Phase::Leave, true, true, true, true) == Dispatch::WaitRelease);
        CHECK(decide(Control::Hold, Phase::Leave, true, true, true, true) == Dispatch::WaitRelease);
        CHECK(decide(Control::None, Phase::Leave, true, true, true, true) == Dispatch::Leave);
        // Leaving and repairing: the job's under released and with no run; under lead it waits for the release.
        CHECK(decide(Control::Released, Phase::Leave, true, false, false, false) == Dispatch::Leave);
        CHECK(decide(Control::Released, Phase::Repair, true, false, false, false) == Dispatch::Repair);
        CHECK(decide(Control::Lead, Phase::Leave, true, false, true, true) == Dispatch::WaitRelease);
        // Under hold a living leader's trip waits, a dead one's way back goes on.
        CHECK(decide(Control::Hold, Phase::Repair, true, false, false, false) == Dispatch::Holds);
        CHECK(decide(Control::Hold, Phase::Leave, false, false, false, false) == Dispatch::Leave);
        // Under hold nothing of the boss phase runs, the leader dead (the repair decision included) or alive.
        CHECK(decide(Control::Hold, Phase::Bosses, false, false, true, true) == Dispatch::Holds);
        CHECK(decide(Control::Hold, Phase::Bosses, true, false, true, true) == Dispatch::Holds);
        // Outside, the way back in is the job's under any control; inside with the party not all in, it waits for it.
        CHECK(decide(Control::Hold, Phase::Bosses, true, false, false, false) == Dispatch::Outside);
        CHECK(decide(Control::Lead, Phase::Hold, true, false, true, false) == Dispatch::Entering);
        // Back on the bosses after a release: handed back before TickBosses; under lead or with no run, TickBosses.
        CHECK(decide(Control::Released, Phase::Hold, true, false, true, true) == Dispatch::HandBack);
        CHECK(decide(Control::Released, Phase::Check, true, false, true, true) == Dispatch::HandBack);
        CHECK(decide(Control::Released, Phase::Bosses, true, false, true, true) == Dispatch::Bosses);
        CHECK(decide(Control::Lead, Phase::Hold, true, false, true, true) == Dispatch::Bosses);
        CHECK(decide(Control::None, Phase::Hold, true, false, true, true) == Dispatch::Bosses);
    }

    void StepsAndKillPull()
    {
        runrules::StepFacts none;
        CHECK(!runrules::StepDue(none));
        runrules::StepFacts pull;
        pull.killPull = true;
        CHECK(runrules::StepDue(pull));
        // Under released, a step still due keeps the job's party: no hand-back while the kill's pull is pending.
        FakeWorld world = Party();
        Counter counter;
        RunCore core = Started(world, counter);
        JobReport due;
        due.inside = true;
        due.stepDue = runrules::StepDue(pull);
        core.Settle(world, counter, due);
        CHECK(core.mode == Mode::Released);
        core.Reconcile(world, counter, 100);
        core.Prepare(world, counter, true);
        core.Settle(world, counter, due);
        CHECK(core.mode == Mode::Released);
        JobReport done;
        done.inside = true;
        core.Settle(world, counter, done);
        CHECK(core.mode == Mode::Lead);
    }

    void Completion()
    {
        // Out and the reset at the end: the job's departure on purpose, like the repair trip.
        FakeWorld world = Party();
        Counter counter;
        RunCore core = Started(world, counter);
        JobReport leaving;
        leaving.leaving = true;
        core.Settle(world, counter, leaving);
        CHECK(core.mode == Mode::Released);
        for (uint32_t guid : world.members)
        {
            world.facts[guid].inInstance = false;
            world.places.erase(guid);
        }
        core.Reconcile(world, counter, 100);
        CHECK(core.mode == Mode::Released);
        CHECK(core.holdReasons.empty());
    }

    void Presence()
    {
        std::vector<Facts> five(5, Inside());
        CHECK(runrules::Present(five));
        for (int which = 0; which < 5; ++which)
        {
            std::vector<Facts> one = five;
            switch (which)
            {
                case 0: one[2].online = false; break;
                case 1: one[2].inWorld = false; break;
                case 2: one[2].teleporting = true; break;
                case 3: one[2].inInstance = false; break;
                default: one[2].alive = false; break;
            }
            CHECK(!runrules::Present(one));
        }
    }

    void NoRoute()
    {
        CHECK(!runrules::Routed(0));
        CHECK(runrules::Routed(1));
    }

    void NewlyHeldCancelled()
    {
        FakeWorld world = Party();
        world.facts[4].alive = false;
        Counter counter;
        RunCore core = Started(world, counter);
        world.facts[1].alive = false;
        core.Reconcile(world, counter, 100);
        CHECK(!core.held.count(4));
        // Member 4 comes back alive inside, following someone: its motion stops before it is placed.
        world.facts[4].alive = true;
        world.calls.clear();
        core.events.clear();
        core.Reconcile(world, counter, 110);
        CHECK(core.held.count(4));
        CHECK(!world.calls.empty() && world.calls.front() == "group");
        bool cancelled = false;
        for (const std::string& call : world.calls)
            cancelled = cancelled || call == "cancel 4";
        CHECK(cancelled);
        CHECK(HasEvent(core, "run_hold_at", "member=m4"));
    }

    // A view as the move action sees it: its own stamp, mode and place.
    struct View
    {
        Mode mode = Mode::Lead;
        runrules::Stamp stamp;
        int place = 0;
    };

    void CachedDestination()
    {
        int asked = 0;
        auto placeOf = [&](const View& view) { ++asked; return view.place; };
        View fresh{ Mode::Lead, { 4, 9 }, 11 };
        int place = -1;
        // The action's stamp is an older revision: no destination is asked for, none is given.
        CHECK(!runrules::ValidatedPlace(fresh.mode, fresh.stamp, runrules::Stamp{ 4, 8 }, false, fresh, placeOf, place));
        CHECK(asked == 0 && place == -1);
        // Current: the destination is the validated view's own, from no other.
        View other{ Mode::Lead, { 4, 9 }, 22 };
        CHECK(runrules::ValidatedPlace(fresh.mode, fresh.stamp, runrules::Stamp{ 4, 9 }, false, fresh, placeOf, place));
        CHECK(asked == 1 && place == 11 && place != other.place);
        // In hold, a member's move to its place is accepted, a leader's write is not; released never.
        View held{ Mode::Hold, { 5, 1 }, 33 };
        CHECK(runrules::ValidatedPlace(held.mode, held.stamp, runrules::Stamp{ 5, 1 }, false, held, placeOf, place) && place == 33);
        CHECK(!runrules::ValidatedPlace(held.mode, held.stamp, runrules::Stamp{ 5, 1 }, true, held, placeOf, place));
        View released{ Mode::Released, { 6, 2 }, 44 };
        CHECK(!runrules::ValidatedPlace(released.mode, released.stamp, runrules::Stamp{ 6, 2 }, false, released, placeOf, place));
        CHECK(place == 33);
    }

    void Tags()
    {
        FakeWorld world = Party();
        Counter counter;
        RunCore core = Started(world, counter);
        for (const Event& event : core.events)
            CHECK(event.detail.rfind("gen=1730000000.", 0) == 0);
        core.Revise("gathered", "stop=3 pull=gather");
        CHECK(core.events.back().detail.rfind("gen=1730000000." + std::to_string(core.generation) + " rev=1 ", 0) == 0);
    }
}

int main()
{
    Stamps();
    HoldReasons();
    Resume();
    JobTable();
    CancelBeneathChase();
    EndAndRetention();
    Disbanded();
    EndReasons();
    LeaderDiesAndResumes();
    ResumeNear();
    AnotherInstance();
    PlayersAndGhostsUntouched();
    ReplacementInLead();
    ReplacementInHold();
    ShortAndOver();
    ReleaseHandoffs();
    RepairTrip();
    HandBackIntoHold();
    Dispatch();
    StepsAndKillPull();
    Completion();
    Presence();
    NoRoute();
    NewlyHeldCancelled();
    CachedDestination();
    Tags();
    printf("%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
