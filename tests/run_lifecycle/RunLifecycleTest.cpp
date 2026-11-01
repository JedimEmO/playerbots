// Tests of a party's run lifecycle (playerbot/GuildRunCore, playerbot/GuildRunRules), with a fake world that records
// every lookup and motion call. Built and run by tests/run_lifecycle/run.sh, without the server.

#include "GuildRunCore.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
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

        // The route, as the rejoin rules see it: a hold place or none, which places are clear, the current stop's way
        // point, every place on the way, and how each member's walk plans.
        bool hasHoldPlace = false;
        Held holdPlace;
        std::function<bool(const Held&)> clear = [](const Held&) { return true; };
        Held stopPoint;
        bool onWay = true;
        std::map<uint32_t, runrules::PlanResult> plans;   // Planned when not given
        std::map<uint32_t, int> planCalls;
        bool respawned = false;
        bool walkingMoves = true;

        bool HoldPlace(Held& place) override
        {
            place = holdPlace;
            return hasHoldPlace;
        }
        bool PlaceClear(const Held& at) override { return clear(at); }
        bool StopWayPoint(Held& at) override
        {
            at = stopPoint;
            return true;
        }
        bool WayPointNear(const Held& at, Held& point) override
        {
            point = at;
            return onWay;
        }
        runrules::PlanResult PlanWalk(uint32_t guid, const Held& to, Walk& walk, std::string& why) override
        {
            ++planCalls[guid];
            const runrules::PlanResult result = plans.count(guid) ? plans[guid] : runrules::PlanResult::Planned;
            if (result == runrules::PlanResult::Blocked)
                why = "spawn_99_stop_3";
            if (result != runrules::PlanResult::Planned)
                return result;
            // A straight way, a point every 2 yards, as the route's way has them.
            const std::vector<float>& at = places[guid];
            const runrules::Vec from = { at[0], at[1], at[2] }, end = { to.x, to.y, to.z };
            const int steps = std::max(1, int(runrules::Distance(from, end) / 2.0f));
            std::vector<runrules::Vec> line;
            for (int i = 0; i <= steps; ++i)
            {
                const float t = float(i) / float(steps);
                line.push_back({ from.x + (end.x - from.x) * t, from.y + (end.y - from.y) * t, from.z + (end.z - from.z) * t });
            }
            // A summon in reach of the line blocks it, as the server's reach test does.
            if (summon.reach > 0.0f && runrules::FirstThreat({ summon }, line) >= 0)
            {
                why = runrules::ThreatName(summon, -1);
                return runrules::PlanResult::Blocked;
            }
            walk.chunks = runrules::Chunks(line, runrules::WALK_CHUNK_YARDS);
            walk.to = to;
            return runrules::PlanResult::Planned;
        }
        bool Respawned(const std::vector<uint32_t>&) override { return respawned; }
        // A summon standing in the way, as Sneed by the lumber mill, at this place with this reach; none when reach is 0.
        runrules::Threat summon;
        bool CreatureNear(const std::vector<runrules::Vec>& part) override
        {
            return summon.reach > 0.0f && runrules::FirstThreat({ summon }, part) >= 0;
        }
        bool Walking(uint32_t) override { return walkingMoves; }
        // The way's point at the entrance, where a supply trip's walk out ends.
        bool hasExit = true;
        Held exit{ -200.0f, 0.0f, 0.0f };
        bool ExitPoint(Held& at) override
        {
            at = exit;
            return hasExit;
        }
        // Readiness as the tick sees each member: full health and mana, near the leader, unless set here.
        std::map<uint32_t, runrules::Ready> ready;
        bool ReadyOf(uint32_t guid, uint32_t, runrules::Ready& out) override
        {
            if (!facts[guid].online)
                return false;
            const Facts f = out.facts;
            out = ready.count(guid) ? ready[guid] : runrules::Ready();
            out.guid = guid;
            out.facts = f;
            return true;
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
        CHECK(runrules::Accepts(Mode::Released, state, { 3, 7 }, false));    // a member walking back to its party
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
        // The pull permit: lead, or released not leaving; nobody apart; not failed.
        runrules::PermitFacts permit;
        permit.mode = Mode::Lead;
        CHECK(runrules::PullPermit(permit));
        permit.apart = true;
        CHECK(!runrules::PullPermit(permit));
        permit.apart = false;
        permit.failed = true;
        CHECK(!runrules::PullPermit(permit));
        permit.failed = false;
        permit.mode = Mode::Released;
        CHECK(runrules::PullPermit(permit));
        permit.leaving = true;
        CHECK(!runrules::PullPermit(permit));
        permit.leaving = false;
        permit.mode = Mode::Hold;
        CHECK(!runrules::PullPermit(permit));
        // The deadline: ten minutes apart, nothing else holding; never without a start.
        CHECK(!runrules::Overdue(0, 100000));
        CHECK(!runrules::Overdue(100, 100 + runrules::REJOIN_SECONDS - 1));
        CHECK(runrules::Overdue(100, 100 + runrules::REJOIN_SECONDS));
        // The cohort: alive inside and not seen dead in the last 30 seconds.
        CHECK(runrules::InCohort(Inside(), 0, 1000));
        CHECK(!runrules::InCohort(Inside(), 990, 1000));
        CHECK(runrules::InCohort(Inside(), 969, 1000));
        Facts dead = Inside();
        dead.alive = false;
        CHECK(!runrules::InCohort(dead, 0, 1000));
        // Members walk back in released too; the leader writes only in lead.
        CHECK(runrules::Accepts(Mode::Released, { 3, 1 }, { 3, 1 }, false));
        CHECK(!runrules::Accepts(Mode::Released, { 3, 1 }, { 3, 1 }, true));
    }

    // The Deadmines' way passes the upper mine twice: early, and again near the end after the ship. A walk from the
    // entrance to an anchor in the upper mine goes the short way, not through the ship's cove, where a pack still lives.
    void WayPassedTwice()
    {
        using runrules::Vec;
        std::vector<Vec> way;
        auto leg = [&](const Vec& from, const Vec& to)
        {
            const int steps = std::max(1, int(runrules::Distance(from, to) / 2.0f));
            for (int i = way.empty() ? 0 : 1; i <= steps; ++i)
            {
                const float t = float(i) / float(steps);
                way.push_back({ from.x + (to.x - from.x) * t, from.y + (to.y - from.y) * t, from.z + (to.z - from.z) * t });
            }
        };
        const Vec entrance = { -14.6f, -385.5f, 62.5f }, anchor = { -215.0f, -555.0f, 52.0f };
        const Vec firstPass = { -219.0f, -559.0f, 52.0f }, cove = { -19.0f, -818.0f, 20.0f }, end = { -120.0f, -640.0f, 14.0f };
        leg(entrance, firstPass);
        leg(firstPass, cove);
        leg(cove, anchor);          // the second pass goes right over the anchor, nearer than the first
        leg(anchor, end);
        runrules::Threat pirate;    // spawn 3600174, stop 104: still alive, unloaded
        pirate.spawn = 3600174;
        pirate.where = { { -19.2f, -830.0f, 19.7f } };
        pirate.reach = 25.0f;
        const std::vector<size_t> starts = runrules::WayNear(way, entrance, runrules::WAY_NEAR);
        const std::vector<size_t> ends = runrules::WayNear(way, anchor, runrules::WAY_NEAR);
        CHECK(!starts.empty() && ends.size() > 2);
        size_t a = 0, b = 0;
        CHECK(runrules::WaySpan(starts, ends, a, b));
        auto stretch = [&](size_t from, size_t to)
        {
            std::vector<Vec> line;
            for (size_t i = from;; i = from < to ? i + 1 : i - 1)
            {
                line.push_back(way[i]);
                if (i == to)
                    break;
            }
            return line;
        };
        CHECK(b < way.size() / 3 && runrules::FirstThreat({ pirate }, stretch(a, b)) < 0);
        // The nearest point to the anchor alone is the second pass, through the cove: the walk the live test blocked.
        size_t nearest = 0;
        for (size_t i = 0; i < way.size(); ++i)
            if (runrules::Distance(way[i], anchor) < runrules::Distance(way[nearest], anchor))
                nearest = i;
        CHECK(nearest > way.size() / 2 && runrules::FirstThreat({ pirate }, stretch(a, nearest)) == 0);
        // Either list empty: no stretch.
        CHECK(!runrules::WaySpan({}, ends, a, b) && !runrules::WaySpan(starts, {}, a, b));

        // The leader chooses its stop again at the anchor after the hold. Copy a's third run: the run was at stop 68,
        // on the first pass; the second pass is the nearer, by Miner Johnson, and the stop after it was 142.
        const Vec atAnchor = { -207.0f, -558.0f, 52.0f };
        const std::vector<size_t> nearAnchor = runrules::WayNear(way, atAnchor, runrules::WAY_NEAR);
        size_t closest = 0;
        for (size_t i = 0; i < way.size(); ++i)
            if (runrules::Distance(way[i], atAnchor) < runrules::Distance(way[closest], atAnchor))
                closest = i;
        const size_t firstIndex = ends.front();
        // Stop 68 a little past the first pass, stop 142 a little past the second; a stop is chosen at or ahead of the
        // place, as GuildPacks::FirstLiveStop does with START_SLACK.
        const float stop68 = float(firstIndex + 5) * 2.0f, stop142 = float(closest + 15) * 2.0f;
        auto chosen = [&](size_t place)
        {
            const float along = float(place) * 2.0f;
            return stop68 + 10.0f >= along ? 68 : stop142 + 10.0f >= along ? 142 : 0;
        };
        CHECK(closest > way.size() / 2 && chosen(closest) == 142);
        // Without progress, a party set down fresh: the nearest point, as before.
        CHECK(runrules::PlaceAlong(nearAnchor, closest, false, 0) == closest);
        // With the run's progress, stop 68's place: the first pass, and stop 68.
        const size_t place = runrules::PlaceAlong(nearAnchor, closest, true, size_t(stop68 / 2.0f));
        CHECK(place < way.size() / 3 && chosen(place) == 68);
        // Nothing within reach of the way: the nearest point whatever the progress.
        CHECK(runrules::PlaceAlong({}, closest, true, 3) == closest);
    }

    void Geometry()
    {
        using runrules::Vec;
        // Point and segment, segment and segment, crossing between vertices.
        CHECK(std::fabs(runrules::SegmentDistance({ 0, 5, 0 }, { 0, 5, 0 }, { -10, 0, 0 }, { 10, 0, 0 }) - 5.0f) < 0.01f);
        CHECK(std::fabs(runrules::SegmentDistance({ 0, -5, 0 }, { 0, 5, 0 }, { -10, 0, 0 }, { 10, 0, 0 })) < 0.01f);
        CHECK(std::fabs(runrules::SegmentDistance({ 12, 0, 0 }, { 12, 0, 0 }, { -10, 0, 0 }, { 10, 0, 0 }) - 2.0f) < 0.01f);
        CHECK(std::fabs(runrules::SegmentDistance({ 0, 3, 4 }, { 1, 3, 4 }, { 0, 0, 0 }, { 1, 0, 0 }) - 5.0f) < 0.01f);
        // A line two waypoints wide: a creature between its vertices is seen, one beside its reach is not.
        const std::vector<Vec> line = { { 0, 0, 0 }, { 100, 0, 0 } };
        runrules::Threat still;
        still.spawn = 1;
        still.where = { { 50, 20, 0 } };
        still.reach = 19.0f;
        CHECK(runrules::FirstThreat({ still }, line) < 0);
        still.reach = 21.0f;
        CHECK(runrules::FirstThreat({ still }, line) == 0);
        // A patrol whose path crosses the line between both lines' vertices, with its points far from it.
        runrules::Threat patrol;
        patrol.spawn = 2;
        patrol.where = { { 40, -60, 0 }, { 60, 60, 0 } };
        patrol.reach = 5.0f;
        CHECK(runrules::FirstThreat({ patrol }, line) == 0);
        patrol.where = { { 40, 30, 0 }, { 60, 60, 0 } };
        CHECK(runrules::FirstThreat({ patrol }, line) < 0);
        // A path kept near the way, and one that leaves it.
        CHECK(runrules::Along({ { 10, 1, 0 }, { 50, -1.5f, 0 } }, line, 2.0f));
        CHECK(!runrules::Along({ { 10, 1, 0 }, { 50, 3.0f, 0 } }, line, 2.0f));
        // Parts of at most 40 yards, each from the last one's end.
        std::vector<Vec> way;
        for (int i = 0; i <= 50; ++i)
            way.push_back({ float(i * 2), 0, 0 });
        const std::vector<std::vector<Vec>> chunks = runrules::Chunks(way, runrules::WALK_CHUNK_YARDS);
        CHECK(chunks.size() == 3);
        CHECK(std::fabs(runrules::Length(chunks[0]) - 40.0f) < 0.01f);
        CHECK(std::fabs(chunks[1].front().x - chunks[0].back().x) < 0.01f);
        CHECK(std::fabs(chunks.back().back().x - 100.0f) < 0.01f);
        CHECK(runrules::Chunks({ { 0, 0, 0 } }, 40.0f).empty());
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
        CHECK(world.cancelled.empty());   // the run's moves are the token cleanup's, by the new generation
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

        // Back alive inside, far from the party: a returner, walking to the anchor; no resume by time, however long.
        world.facts[1] = Inside();
        world.places[1] = { 500.0f, 0.0f, 0.0f };
        core.Reconcile(world, counter, 200);
        CHECK(core.mode == Mode::Hold && core.apart.count(1) && core.apart[1].walking);
        CHECK(HasEvent(core, "run_rejoin", "member=m1 result=walking"));
        core.Reconcile(world, counter, 380);
        CHECK(core.mode == Mode::Hold);
        // At the anchor: every member with the party, the run resumes and the leader chooses its stop from there.
        world.places[1] = { 2.0f, 1.0f, 0.0f };
        world.cancelled.clear();
        core.Reconcile(world, counter, 390);
        CHECK(core.mode == Mode::Lead && core.choose && core.reason == "resume_rejoined");
        CHECK(core.held.empty() && core.apart.empty());
        CHECK(world.cancelled.empty());
    }

    void ResumeNear()
    {
        // 39 yards from the party is not with it: arrival is at the anchor.
        FakeWorld world = Party();
        Counter counter;
        RunCore core = Started(world, counter);
        world.facts[1].alive = false;
        core.Reconcile(world, counter, 100);
        world.facts[1] = Inside();
        world.places[1] = { 2.0f, 39.0f, 0.0f };
        core.Reconcile(world, counter, 120);
        CHECK(core.mode == Mode::Hold && core.apart.count(1));
        world.places[1] = { 2.0f, 4.0f, 0.0f };
        core.Reconcile(world, counter, 130);
        CHECK(core.mode == Mode::Lead && core.reason == "resume_rejoined");
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
        CHECK(!five && !six);             // the old roster's run moves go stale with the generation, for the token cleanup
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
        CHECK(core.generation != generation && world.cancelled.empty());
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
        CHECK(core.generation != generation && world.cancelled.empty());

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

        // Back: the hold began on the way out, so it resumes to released at once, the trip going on, with nobody walking
        // back in.
        world.facts[1] = Inside();
        world.facts[1].inInstance = false;
        core.Reconcile(world, counter, 200);
        CHECK(core.mode == Mode::Released && !core.choose && core.reason == "resume_leaving" && core.apart.empty());
        CHECK(!core.Permit());

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
        // In hold, a member's move to its place is accepted, a leader's write is not; in released only a member's.
        View held{ Mode::Hold, { 5, 1 }, 33 };
        CHECK(runrules::ValidatedPlace(held.mode, held.stamp, runrules::Stamp{ 5, 1 }, false, held, placeOf, place) && place == 33);
        CHECK(!runrules::ValidatedPlace(held.mode, held.stamp, runrules::Stamp{ 5, 1 }, true, held, placeOf, place));
        View released{ Mode::Released, { 6, 2 }, 44 };
        CHECK(runrules::ValidatedPlace(released.mode, released.stamp, runrules::Stamp{ 6, 2 }, false, released, placeOf, place));
        CHECK(place == 44);
        CHECK(!runrules::ValidatedPlace(released.mode, released.stamp, runrules::Stamp{ 6, 2 }, true, released, placeOf, place));
    }

    void Tokens()
    {
        uint32_t counter = 0;
        std::set<uint32_t> used;
        auto inUse = [&](uint32_t token) { return used.count(token) > 0; };
        const uint32_t first = runrules::NextToken(counter, inUse);
        CHECK(first == (runrules::RUN_TOKEN_BIT | 1u) && runrules::IsRunToken(first));
        CHECK(!runrules::IsRunToken(36));                   // a map id, as MoveTo used
        CHECK(!runrules::IsRunToken(runrules::RUN_TOKEN_BIT)); // never the bit alone
        // Skips a token a record or a stack still holds.
        used.insert(runrules::RUN_TOKEN_BIT | 2u);
        CHECK(runrules::NextToken(counter, inUse) == (runrules::RUN_TOKEN_BIT | 3u));
        // Wraps to 1, never to 0, and skips a surviving token there too.
        counter = 0x7FFFFFFEu;
        CHECK(runrules::NextToken(counter, inUse) == (runrules::RUN_TOKEN_BIT | 0x7FFFFFFFu));
        used.insert(runrules::RUN_TOKEN_BIT | 1u);
        // 1 and 2 are both held: the wrap lands on 3.
        CHECK(runrules::NextToken(counter, inUse) == (runrules::RUN_TOKEN_BIT | 3u));
        CHECK(counter == 3u);
    }

    void Records()
    {
        using runrules::RecordAction;
        runrules::RecordFacts current;
        current.runExists = current.stampCurrent = current.memberInRun = current.onRecordInstance = true;
        current.online = current.engine = current.inWorld = current.tokenInStack = true;
        CHECK(runrules::RecordStep(current) == RecordAction::Keep);
        runrules::RecordFacts gone = current;
        gone.tokenInStack = false;
        CHECK(runrules::RecordStep(gone) == RecordAction::Retire);
        runrules::RecordFacts stale = current;
        stale.stampCurrent = false;
        CHECK(runrules::RecordStep(stale) == RecordAction::Remove);
        // Left the instance with no stamp changed: stale all the same.
        runrules::RecordFacts left = current;
        left.onRecordInstance = false;
        CHECK(runrules::RecordStep(left) == RecordAction::Remove);
        runrules::RecordFacts ended = current;
        ended.runExists = false;
        CHECK(runrules::RecordStep(ended) == RecordAction::Remove);
        runrules::RecordFacts between = stale;
        between.teleporting = true;
        CHECK(runrules::RecordStep(between) == RecordAction::Wait);
        runrules::RecordFacts player = stale;
        player.engine = false;
        CHECK(runrules::RecordStep(player) == RecordAction::Wait);
        runrules::RecordFacts otherMap = stale;
        otherMap.reachable = false;          // the map pass, the member on another map: the world pass's
        CHECK(runrules::RecordStep(otherMap) == RecordAction::Wait);
        runrules::RecordFacts offline;
        CHECK(runrules::RecordStep(offline) == RecordAction::Drop);
    }

    void Replacement()
    {
        using runrules::Removal;
        CHECK(runrules::MayIssue({}));
        CHECK(runrules::MayIssue({ Removal::Removed, Removal::NotFound }));
        CHECK(!runrules::MayIssue({ Removal::Removed, Removal::Deferred }));
        // Placement in hold stops follow and moves not the run's; the run's own token alone is left to its record.
        CHECK(!runrules::PlanCancel({ Motion::Idle, Motion::RunToken }, true).clear);
        CHECK(runrules::PlanCancel({ Motion::Idle, Motion::RunToken, Motion::Follow }, true).clear);
        CHECK(!runrules::PlanCancel({ Motion::Idle, Motion::RunToken, Motion::Chase }, true).clear);
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

    // The members' motion stacks for RunMoves, bottom first, with hooks on removal for the callbacks a Finalize runs.
    struct FakeMoves : MoveWorld
    {
        std::map<uint32_t, MoveMember> members;
        std::map<uint32_t, std::vector<uint32_t>> stacks;
        std::map<uint32_t, std::function<void()>> onRemove;   // by token, run once as the move is removed
        std::set<uint32_t> deferred;                          // tokens whose removal the core defers
        std::vector<std::string> removed;                     // "guid:token", in order

        MoveMember Member(uint32_t guid) override { return members[guid]; }
        bool HasMove(uint32_t guid, uint32_t token) override
        {
            const std::vector<uint32_t>& stack = stacks[guid];
            return members[guid].online && std::find(stack.begin(), stack.end(), token) != stack.end();
        }
        uint32_t TopMove(uint32_t guid) override { return stacks[guid].empty() ? 0 : stacks[guid].back(); }
        std::vector<uint32_t> MoveIds(uint32_t guid) override { return stacks[guid]; }
        runrules::Removal RemoveMove(uint32_t guid, uint32_t token) override
        {
            std::vector<uint32_t>& stack = stacks[guid];
            auto it = std::find(stack.begin(), stack.end(), token);
            if (it == stack.end())
                return runrules::Removal::NotFound;
            if (deferred.count(token))
                return runrules::Removal::Deferred;
            stack.erase(it);
            removed.push_back(std::to_string(guid) + ":" + std::to_string(token));
            auto hook = onRemove.find(token);
            if (hook != onRemove.end())
            {
                std::function<void()> run = hook->second;
                onRemove.erase(hook);
                run();
            }
            return runrules::Removal::Removed;
        }

        // A member in the world on map 1, instance `instance`, played by the engine.
        void Add(uint32_t guid, uint32_t instance = 7)
        {
            MoveMember member;
            member.visible = member.online = member.engine = member.inWorld = true;
            member.mapId = 1;
            member.instanceId = instance;
            members[guid] = member;
            stacks[guid] = { 0 };   // the idle move at the bottom
        }
        // A move that pushes `token` on top, as MovePoint does.
        std::function<bool(uint32_t)> Push(uint32_t guid)
        {
            return [this, guid](uint32_t token) { stacks[guid].push_back(token); return true; };
        }
    };

    const std::function<bool()> Always = []() { return true; };

    std::map<uint32_t, RunNow> RunOf(uint32_t leader, runrules::Stamp stamp, Mode mode = Mode::Lead,
        std::vector<uint32_t> expected = { 1, 2, 3, 4, 5 })
    {
        RunNow run;
        run.stamp = stamp;
        run.mode = mode;
        run.instanceId = 7;
        run.expected = expected;
        return { { leader, run } };
    }

    void RunMoveStart()
    {
        const runrules::Stamp stamp = { 3, 1 };
        FakeMoves world;
        world.Add(2);
        RunMoves moves;
        // Recorded with the token on top, where the member stands.
        CHECK(moves.Start(world, 2, stamp, 1, Always, world.Push(2)));
        MoveRecord first;
        CHECK(moves.Find(2, first) && runrules::IsRunToken(first.token) && world.TopMove(2) == first.token);
        CHECK(first.mapId == 1 && first.instanceId == 7 && first.leader == 1 && runrules::Same(first.stamp, stamp));
        // The next replaces it: the previous token removed, one record.
        CHECK(moves.Start(world, 2, stamp, 1, Always, world.Push(2)));
        MoveRecord second;
        CHECK(moves.Find(2, second) && second.token != first.token && !world.HasMove(2, first.token) && moves.Size() == 1);
        // A run token no record holds is swept; an id outside the run's namespace stays.
        const uint32_t leaked = runrules::RUN_TOKEN_BIT | 0x7FFFFF01u;
        world.stacks[2].insert(world.stacks[2].begin() + 1, leaked);
        world.stacks[2].insert(world.stacks[2].begin() + 1, 42u);
        CHECK(moves.Start(world, 2, stamp, 1, Always, world.Push(2)));
        CHECK(!world.HasMove(2, leaked) && world.HasMove(2, 42u) && !world.HasMove(2, second.token));
        // A move that started nothing on top records nothing, and the old record is gone with its move.
        MoveRecord third;
        CHECK(moves.Find(2, third));
        CHECK(moves.Start(world, 2, stamp, 1, Always, [](uint32_t) { return true; }));
        CHECK(!moves.Find(2, third) && moves.Size() == 0);
        // Between maps, or not visible to the pass: nothing.
        world.members[2].teleporting = true;
        CHECK(!moves.Start(world, 2, stamp, 1, Always, world.Push(2)) && moves.Size() == 0);
        world.members[2].teleporting = false;
        world.members[2].visible = false;
        CHECK(!moves.Start(world, 2, stamp, 1, Always, world.Push(2)) && moves.Size() == 0);
    }

    void RunMoveDeferredAndStale()
    {
        const runrules::Stamp stamp = { 3, 1 };
        FakeMoves world;
        world.Add(2);
        RunMoves moves;
        moves.Start(world, 2, stamp, 1, Always, world.Push(2));
        MoveRecord first;
        CHECK(moves.Find(2, first));
        // The previous token's removal deferred: nothing issued, the record kept.
        world.deferred.insert(first.token);
        bool issued = false;
        CHECK(!moves.Start(world, 2, stamp, 1, Always, [&](uint32_t) { issued = true; return true; }));
        MoveRecord kept;
        CHECK(!issued && moves.Find(2, kept) && kept.token == first.token);
        world.deferred.clear();
        // The plan moved on while the previous move was removed: nothing issued, and the old record retired.
        bool current = true;
        world.onRemove[first.token] = [&]() { current = false; };
        CHECK(!moves.Start(world, 2, stamp, 1, [&]() { return current; }, [&](uint32_t) { issued = true; return true; }));
        CHECK(!issued && moves.Size() == 0 && !world.HasMove(2, first.token));
    }

    void RunMoveCallbackReplacement()
    {
        const runrules::Stamp stamp = { 3, 1 };
        FakeMoves world;
        world.Add(2);
        RunMoves moves;
        moves.Start(world, 2, stamp, 1, Always, world.Push(2));
        MoveRecord first;
        CHECK(moves.Find(2, first));
        // The previous move's Finalize starts and records a run move of its own: it stands, and the outer call issues
        // nothing and neither overwrites nor removes it.
        bool inner = false;
        world.onRemove[first.token] = [&]() { inner = moves.Start(world, 2, stamp, 1, Always, world.Push(2)); };
        bool issued = false;
        CHECK(!moves.Start(world, 2, stamp, 1, Always, [&](uint32_t) { issued = true; return true; }));
        MoveRecord now;
        CHECK(inner && !issued && moves.Find(2, now) && now.token != first.token && world.TopMove(2) == now.token);
    }

    void RunMoveIssueReentrancy()
    {
        const runrules::Stamp stamp = { 3, 1 };
        MoveRecord record;
        // A callback during the move starts a run move after this one: its sweep takes this one, its record stands.
        {
            FakeMoves world;
            world.Add(2);
            RunMoves moves;
            uint32_t ours = 0;
            bool inner = false;
            const bool outer = moves.Start(world, 2, stamp, 1, Always, [&](uint32_t token)
            {
                ours = token;
                world.stacks[2].push_back(token);
                inner = moves.Start(world, 2, stamp, 1, Always, world.Push(2));
                return true;
            });
            CHECK(!outer && inner && moves.Find(2, record) && record.token != ours && world.TopMove(2) == record.token
                && !world.HasMove(2, ours));
        }
        // A callback records its move first and this one lands on top: this one yields and is removed.
        // With the removal deferred, the callback's record stands all the same.
        for (bool deferred : { false, true })
        {
            FakeMoves world;
            world.Add(2);
            RunMoves moves;
            uint32_t ours = 0;
            bool inner = false;
            const bool outer = moves.Start(world, 2, stamp, 1, Always, [&](uint32_t token)
            {
                ours = token;
                inner = moves.Start(world, 2, stamp, 1, Always, world.Push(2));
                world.stacks[2].push_back(token);
                if (deferred)
                    world.deferred.insert(token);
                return true;
            });
            CHECK(!outer && inner && moves.Find(2, record) && record.token != ours && world.HasMove(2, record.token));
            CHECK(world.HasMove(2, ours) == deferred);
        }
        // The yielding removal's own callback replaces the callback's move again: the last record stands.
        {
            FakeMoves world;
            world.Add(2);
            RunMoves moves;
            uint32_t first = 0, ours = 0;
            bool second = false;
            const bool outer = moves.Start(world, 2, stamp, 1, Always, [&](uint32_t token)
            {
                ours = token;
                moves.Start(world, 2, stamp, 1, Always, world.Push(2));
                MoveRecord made;
                moves.Find(2, made);
                first = made.token;
                world.stacks[2].push_back(token);
                world.onRemove[token] = [&]() { second = moves.Start(world, 2, stamp, 1, Always, world.Push(2)); };
                return true;
            });
            CHECK(!outer && second && moves.Find(2, record) && record.token != first && record.token != ours
                && world.TopMove(2) == record.token && !world.HasMove(2, first) && !world.HasMove(2, ours) && moves.Size() == 1);
        }
        // A callback whose own run move cannot go out, its sweep of this one deferred: this one is recorded.
        {
            FakeMoves world;
            world.Add(2);
            RunMoves moves;
            uint32_t ours = 0;
            bool inner = true;
            const bool outer = moves.Start(world, 2, stamp, 1, Always, [&](uint32_t token)
            {
                ours = token;
                world.stacks[2].push_back(token);
                world.deferred.insert(token);
                inner = moves.Start(world, 2, stamp, 1, Always, world.Push(2));
                return true;
            });
            CHECK(outer && !inner && moves.Find(2, record) && record.token == ours && world.TopMove(2) == ours);
        }
    }

    // Copy a's hand test: the healer dies, comes back at the entrance, then the leader dies. The healer just back is not
    // of the cohort; the party's place is the cohort's, nearest the stop; both walk back, and only then the run resumes.
    void CopyA()
    {
        FakeWorld world = Party();
        Counter counter;
        RunCore core = Started(world, counter);
        world.facts[5].alive = false;
        core.Reconcile(world, counter, 90);
        CHECK(core.mode == Mode::Lead && core.apart.count(5) && core.apart[5].missing && !core.Permit());
        world.facts[5] = Inside();
        world.places[5] = { 300.0f, 0.0f, 0.0f };   // the entrance
        world.facts[1].alive = false;
        core.Reconcile(world, counter, 100);
        CHECK(core.mode == Mode::Hold && core.reason == "leader_dead");
        CHECK(core.hasAnchor && core.anchorKind == "cohort_m2");
        CHECK(core.apart.count(5) && !core.apart[5].cohort && core.apart[5].walking);
        CHECK(!core.apart.count(2) && !core.apart.count(3) && !core.apart.count(4));   // within 5 yd of the anchor
        CHECK(HasEvent(core, "run_anchor", "kind=cohort_m2") && HasEvent(core, "run_anchor", "cohort=m2,m3,m4"));
        // The leader back at the entrance walks too; the leader at the party with the healer still away: no resume.
        world.facts[1] = Inside();
        world.places[1] = { 300.0f, 2.0f, 0.0f };
        core.Reconcile(world, counter, 150);
        CHECK(core.mode == Mode::Hold && core.apart.count(1) && core.apart[1].walking);
        world.places[1] = { 2.0f, 2.0f, 0.0f };
        core.Reconcile(world, counter, 200);
        CHECK(core.mode == Mode::Hold && !core.apart.count(1) && core.apart.count(5));
        // Every reason gone, the healer still away: the hold's reason says the party regroups.
        CHECK(core.reason == "regrouping" && HasEvent(core, "run_mode", "mode=hold reason=regrouping"));
        world.places[5] = { 3.0f, -2.0f, 0.0f };
        core.Reconcile(world, counter, 210);
        CHECK(core.mode == Mode::Lead && core.reason == "resume_rejoined" && core.choose && core.Permit());
    }

    void HoldPlaceAndMinute()
    {
        FakeWorld world = Party();
        world.hasHoldPlace = true;
        world.holdPlace = { 50.0f, 0.0f, 0.0f };
        Counter counter;
        RunCore core = Started(world, counter);
        world.facts[1].alive = false;
        core.Reconcile(world, counter, 100);
        CHECK(core.anchorKind == "hold_place");
        for (uint32_t guid : { 2u, 3u, 4u, 5u })
            CHECK(core.apart.count(guid) && core.apart[guid].cohort && core.apart[guid].walking);
        // Three get there; one does not within its minute: it stands where it is, its run move goes, and it is a
        // returner, tried again after REPLAN_SECONDS.
        for (uint32_t guid : { 2u, 3u, 4u })
            world.places[guid] = { 50.0f, float(guid) - 3.0f, 0.0f };
        world.places[5] = { 10.0f, 0.0f, 0.0f };
        core.Reconcile(world, counter, 120);
        CHECK(!core.apart.count(2) && core.apart.count(5));
        core.cancels.clear();
        core.Reconcile(world, counter, 160);
        CHECK(core.apart.count(5) && !core.apart[5].cohort && !core.apart[5].walking);
        CHECK(core.cancels == std::vector<uint32_t>{ 5 });
        CHECK(HasEvent(core, "run_hold_at", "member=m5 result=gave_up"));
        CHECK(core.held.count(5) && core.held[5].x == 10.0f);
        const int calls = world.planCalls[5];
        core.Reconcile(world, counter, 165);
        CHECK(world.planCalls[5] == calls && !core.apart[5].walking);
        core.Reconcile(world, counter, 171);
        CHECK(world.planCalls[5] == calls + 1 && core.apart[5].walking);
    }

    void BlockedEndsTheRun()
    {
        FakeWorld world = Party();
        Counter counter;
        RunCore core = Started(world, counter);
        world.facts[1].alive = false;
        world.facts[5].alive = false;
        core.Reconcile(world, counter, 100);
        world.facts[1] = Inside();
        world.facts[5] = Inside();
        world.places[5] = { 300.0f, 0.0f, 0.0f };
        world.plans[5] = runrules::PlanResult::Blocked;
        core.Reconcile(world, counter, 110);
        CHECK(core.apart.count(5) && core.apart[5].blocked == "spawn_99_stop_3" && !core.apart[5].walking);
        CHECK(HasEvent(core, "run_rejoin", "member=m5 result=blocked why=spawn_99_stop_3"));
        CHECK(core.apartSince == 110);
        // Tried again every REPLAN_SECONDS, not every tick.
        const int calls = world.planCalls[5];
        core.Reconcile(world, counter, 115);
        CHECK(world.planCalls[5] == calls);
        core.Reconcile(world, counter, 120);
        CHECK(world.planCalls[5] == calls + 1);
        // A hold reason back stops the clock; gone again, it starts over.
        world.facts[1].alive = false;
        core.Reconcile(world, counter, 300);
        CHECK(core.apartSince == 0);
        world.facts[1] = Inside();
        core.Reconcile(world, counter, 310);
        CHECK(core.apartSince == 310 && core.mode == Mode::Hold);
        core.Reconcile(world, counter, 310 + runrules::REJOIN_SECONDS - 1);
        CHECK(core.mode == Mode::Hold && !core.failed);
        core.Reconcile(world, counter, 310 + runrules::REJOIN_SECONDS);
        CHECK(core.mode == Mode::Ended && core.reason == "cannot_rejoin" && core.failed && !core.Permit());
        CHECK(HasEvent(core, "run_rejoin", "result=cannot_rejoin apart=m5:spawn_99_stop_3"));
    }

    void ApartInLead()
    {
        FakeWorld world = Party();
        Counter counter;
        RunCore core = Started(world, counter);
        core.Reconcile(world, counter, 100);
        CHECK(core.Permit() && core.settled.size() == 5);
        // A member dead in lead is apart, and the party may start no fight: the job's pulls and steps wait too.
        world.facts[3].alive = false;
        core.Reconcile(world, counter, 110);
        CHECK(core.mode == Mode::Lead && core.apart.count(3) && !core.Permit() && core.settled.size() == 4);
        // A step of the tables releases the party: still apart, still no fight.
        JobReport step;
        step.inside = true;
        step.stepDue = true;
        core.Settle(world, counter, step);
        CHECK(core.mode == Mode::Released && core.apart.count(3) && !core.Permit());
        // Back alive at the entrance: it walks to the leader's way point, and arrives.
        world.facts[3] = Inside();
        world.places[3] = { 300.0f, 0.0f, 0.0f };
        core.Reconcile(world, counter, 120);
        CHECK(core.apart.count(3) && core.apart[3].walking && core.apart[3].walk.to.x == 1.0f);
        CHECK(HasEvent(core, "run_rejoin", "member=m3 result=back"));
        world.places[3] = { 3.0f, 1.0f, 0.0f };
        core.Reconcile(world, counter, 130);
        CHECK(core.apart.empty() && core.Permit());
        // Far from the leader in lead: displaced, and apart.
        JobReport back;
        back.inside = true;
        core.Settle(world, counter, back);
        CHECK(core.mode == Mode::Lead);
        world.places[4] = { 100.0f, 0.0f, 0.0f };
        core.Reconcile(world, counter, 140);
        CHECK(core.apart.count(4) && HasEvent(core, "run_rejoin", "member=m4 result=displaced") && !core.Permit());
        // The leader leaving the party behind is no reason for the leader to be apart.
        CHECK(!core.apart.count(1));
    }

    void WalksPlannedAgain()
    {
        FakeWorld world = Party();
        Counter counter;
        RunCore core = Started(world, counter);
        world.facts[1].alive = false;
        world.facts[5].alive = false;
        core.Reconcile(world, counter, 100);
        world.facts[5] = Inside();
        world.places[5] = { 100.0f, 0.0f, 0.0f };
        core.Reconcile(world, counter, 110);
        CHECK(core.apart[5].walking && core.apart[5].walk.chunks.size() == 3);
        const uint32_t first = core.apart[5].walk.id;
        core.cancels.clear();
        // A part's end reached: on to the next.
        world.places[5] = { core.apart[5].walk.chunks[0].back().x + 1.0f, 0.0f, 0.0f };
        core.Reconcile(world, counter, 115);
        CHECK(core.apart[5].walking && core.apart[5].walk.chunk == 1 && core.apart[5].walk.id == first);
        // Knocked off the walk: planned again from where it stands, its old run move removed.
        world.places[5][1] = 10.0f;
        core.Reconcile(world, counter, 116);
        CHECK(core.cancels == std::vector<uint32_t>{ 5 } && core.apart[5].walking && core.apart[5].walk.id != first);
        CHECK(HasEvent(core, "run_rejoin", "member=m5 result=replan why=drift"));
        // A creature near it alive again; a part not gained in WALK_STALL_SECONDS; its move gone mid-part.
        core.cancels.clear();
        world.respawned = true;
        core.Reconcile(world, counter, 117);
        CHECK(core.cancels.size() == 1 && HasEvent(core, "run_rejoin", "why=respawn"));
        world.respawned = false;
        core.Reconcile(world, counter, 117 + runrules::WALK_STALL_SECONDS);
        CHECK(HasEvent(core, "run_rejoin", "why=stalled"));
        const runrules::Vec start = core.apart[5].walk.chunks[0].front();
        world.places[5] = { start.x - 3.0f, start.y, start.z };
        world.walkingMoves = false;
        core.Reconcile(world, counter, 150);
        CHECK(HasEvent(core, "run_rejoin", "why=stopped"));
        // No path search left this tick: no walk yet, and none lost; the next tick tries again.
        world.walkingMoves = true;
        world.plans[5] = runrules::PlanResult::Pending;
        world.respawned = true;
        core.Reconcile(world, counter, 160);
        CHECK(!core.apart[5].walking && core.apart[5].blocked.empty());
        world.respawned = false;
        world.plans.erase(5);
        core.Reconcile(world, counter, 161);
        CHECK(core.apart[5].walking);
    }

    void NoAnchor()
    {
        // No hold place, and no cohort member stands clear: no anchor until the leader is back alive inside, clear.
        FakeWorld world = Party();
        world.clear = [](const Held& at) { return at.x > 100.0f; };
        Counter counter;
        RunCore core = Started(world, counter);
        world.facts[1].alive = false;
        core.Reconcile(world, counter, 100);
        CHECK(!core.hasAnchor && HasEvent(core, "run_anchor", "kind=none"));
        for (uint32_t guid : { 2u, 3u, 4u, 5u })
            CHECK(core.apart.count(guid) && !core.apart[guid].walking);
        world.facts[1] = Inside();
        world.places[1] = { 200.0f, 0.0f, 0.0f };
        core.Reconcile(world, counter, 110);
        CHECK(core.hasAnchor && core.anchorKind == "leader" && !core.apart.count(1));
        CHECK(core.apart[2].walking && core.apart[2].walk.to.x == 200.0f);

        // Off the route's way, no place is the party's: a member coming back could not walk to it.
        FakeWorld offWay = Party();
        offWay.onWay = false;
        RunCore held = Started(offWay, counter);
        offWay.facts[1].alive = false;
        held.Reconcile(offWay, counter, 100);
        CHECK(!held.hasAnchor);
        offWay.facts[1] = Inside();
        held.Reconcile(offWay, counter, 110);
        CHECK(!held.hasAnchor && held.mode == Mode::Hold);
    }

    void FailureOutlivesTheRun()
    {
        Failures kept;
        std::string why;
        CHECK(!kept.Find(1, 7, why));
        kept.Record(1, 7, "cannot_rejoin");
        // The run state can be gone; the failure stands for its order only, until acknowledged for that order.
        CHECK(kept.Find(1, 7, why) && why == "cannot_rejoin");
        CHECK(!kept.Find(1, 8, why));
        kept.Acknowledge(1, 8);
        CHECK(kept.Find(1, 7, why));
        kept.Acknowledge(1, 7);
        CHECK(!kept.Find(1, 7, why));
    }

    // A party failed by cannot_rejoin keeps its group for the order it failed, so the instance stays valid and nobody is
    // sent home; the agent's next order lets it go as any party job's end does.
    void FailedPartyKeepsGroup()
    {
        CHECK(!runrules::KeepsGroup(0, 0));
        CHECK(runrules::KeepsGroup(41, 41));
        CHECK(!runrules::KeepsGroup(41, 42));
        // The failed order: joined, not working, a party job, kept: it stays.
        CHECK(!runrules::LeavesParty(true, false, true, runrules::KeepsGroup(41, 41)));
        // The next order, another job or the party job again, ends the keeping: a job not working leaves, as before.
        CHECK(runrules::LeavesParty(true, false, true, runrules::KeepsGroup(41, 42)));
        CHECK(runrules::LeavesParty(true, true, false, runrules::KeepsGroup(41, 42)));
        CHECK(!runrules::LeavesParty(true, true, true, runrules::KeepsGroup(41, 42)));
        // A failure for any other reason, as before: it leaves.
        CHECK(runrules::LeavesParty(true, false, true, runrules::KeepsGroup(0, 41)));
        CHECK(!runrules::LeavesParty(false, false, true, false));
    }

    // Copy a's retest: clearroute killed Sneed's Shredder, whose death summoned Sneed by the lumber mill. He has no spawn,
    // so the route's model never had him; the reach test takes him as a live creature, and no walk goes past him.
    void SummonInTheWay()
    {
        using runrules::Vec;
        runrules::Threat sneed;
        sneed.entry = 643;
        sneed.where = { { -306.0f, -513.0f, 56.0f } };
        sneed.reach = 20.0f + runrules::THREAT_MARGIN;
        CHECK(runrules::ThreatName(sneed, -1) == "creature_643_unmodelled");
        sneed.spawn = 79000;
        CHECK(runrules::ThreatName(sneed, -1) == "creature_643_spawn_79000_unmodelled");
        sneed.spawn = 0;
        runrules::Threat modelled;
        modelled.spawn = 3600174;
        CHECK(runrules::ThreatName(modelled, 103) == "spawn_3600174_stop_104");
        // The way from the entrance to the foundry passes the mill: blocked while he lives.
        const std::vector<Vec> way = { { -14.6f, -385.5f, 62.5f }, { -180.0f, -470.0f, 55.0f }, { -300.0f, -505.0f, 56.0f },
            { -215.0f, -555.0f, 52.0f } };
        CHECK(runrules::FirstThreat({ sneed }, way) == 0);
        // A way that keeps out of his reach is clear.
        const std::vector<Vec> clear = { { -14.6f, -385.5f, 62.5f }, { -180.0f, -470.0f, 55.0f }, { -215.0f, -555.0f, 52.0f } };
        CHECK(runrules::FirstThreat({ sneed }, clear) < 0);

        // The run: the healer back at the entrance, a summon in its way. No walk while he lives; one once he is dead.
        FakeWorld world = Party();
        Counter counter;
        RunCore core = Started(world, counter);
        world.facts[1].alive = false;
        world.facts[5].alive = false;
        core.Reconcile(world, counter, 100);
        world.facts[1] = Inside();
        world.places[1] = { 2.0f, 1.0f, 0.0f };
        world.facts[5] = Inside();
        world.places[5] = { 300.0f, 0.0f, 0.0f };
        world.summon.entry = 643;
        world.summon.where = { { 150.0f, 6.0f, 0.0f } };
        world.summon.reach = 20.0f;
        core.Reconcile(world, counter, 110);
        CHECK(core.apart.count(5) && !core.apart[5].walking && core.apart[5].blocked == "creature_643_unmodelled");
        CHECK(HasEvent(core, "run_rejoin", "member=m5 result=blocked why=creature_643_unmodelled"));
        world.summon.reach = 0.0f;
        core.Reconcile(world, counter, 120);
        CHECK(core.apart[5].walking);
        // Summoned again into the part being walked: the walk stops and is planned again, and blocked.
        world.summon.reach = 20.0f;
        world.summon.where = { { core.apart[5].walk.chunks[0].back().x, 3.0f, 0.0f } };
        core.cancels.clear();
        core.Reconcile(world, counter, 121);
        CHECK(HasEvent(core, "run_rejoin", "member=m5 result=replan why=creature_near"));
        CHECK(core.cancels == std::vector<uint32_t>{ 5 } && !core.apart[5].walking
            && core.apart[5].blocked == "creature_643_unmodelled");
        CHECK(core.mode == Mode::Hold);
    }

    // The user plays a member, a healer or a dps, while a bot tank leads: with the party by where it is, never moved,
    // and never the reason a run fails.
    void HumanMember()
    {
        Facts human = Inside();
        human.engine = false;
        CHECK(runrules::Human(human) && !runrules::Movable(human));
        CHECK(runrules::HumanPresent(human, true, 19.0f) && !runrules::HumanPresent(human, true, 21.0f));
        CHECK(runrules::HumanPresent(human, true, -1.0f) && !runrules::HumanPresent(human, false, 1.0f));
        Facts dead = human;
        dead.alive = false;
        Facts outside = human;
        outside.inInstance = false;
        CHECK(!runrules::HumanPresent(dead, true, 1.0f) && !runrules::HumanPresent(outside, true, 1.0f));
        CHECK(!runrules::Human(Inside()) && !runrules::Human(Facts()));

        // In lead, near the leader: with the party, the pull allowed, settled, and never planned, placed or cancelled.
        FakeWorld world = Party();
        world.facts[4] = human;
        world.places[4] = { 15.0f, 0.0f, 0.0f };
        Counter counter;
        RunCore core = Started(world, counter);
        core.Reconcile(world, counter, 100);
        CHECK(core.mode == Mode::Lead && core.apart.empty() && core.Permit());
        CHECK(std::find(core.settled.begin(), core.settled.end(), 4u) != core.settled.end());
        // Far off: apart, the pull held, and still nothing moves it; ten minutes and more end nothing.
        world.places[4] = { 100.0f, 0.0f, 0.0f };
        core.Reconcile(world, counter, 110);
        CHECK(core.apart.count(4) && core.apart[4].human && !core.apart[4].walking && !core.Permit());
        CHECK(HasEvent(core, "run_rejoin", "member=m4 result=away human=1"));
        core.Reconcile(world, counter, 110 + runrules::REJOIN_SECONDS + 60);
        CHECK(core.mode == Mode::Lead && !core.failed && core.apartSince == 0);
        // Dead, the same; back within reach: with the party again, the pull allowed.
        world.facts[4].alive = false;
        core.Reconcile(world, counter, 800);
        CHECK(core.apart.count(4) && core.apart[4].human && core.mode == Mode::Lead && core.apartSince == 0);
        world.facts[4] = human;
        world.places[4] = { 12.0f, 3.0f, 0.0f };
        core.Reconcile(world, counter, 810);
        CHECK(core.apart.empty() && core.Permit() && HasEvent(core, "run_rejoin", "member=m4 result=arrived human=1"));
        CHECK(world.planCalls[4] == 0 && !core.held.count(4));
        for (uint32_t guid : world.cancelled)
            CHECK(guid != 4);

        // A bot apart beside the person, in lead: no deadline there, since the blocks at its stop govern (slice B).
        world.facts[3].alive = false;
        world.places[4] = { 100.0f, 0.0f, 0.0f };
        core.Reconcile(world, counter, 900);
        CHECK(core.mode == Mode::Lead && core.apartSince == 0);
        // In hold, the bot's ten minutes end the run, and only the bot is named.
        world.facts[1].alive = false;
        core.Reconcile(world, counter, 910);
        world.facts[1] = Inside();
        core.Reconcile(world, counter, 920);
        CHECK(core.mode == Mode::Hold && core.apartSince == 920);
        core.Reconcile(world, counter, 920 + runrules::REJOIN_SECONDS);
        CHECK(core.mode == Mode::Ended && core.failed && HasEvent(core, "run_rejoin", "result=cannot_rejoin apart=m3"));
        CHECK(!HasEvent(core, "run_rejoin", "apart=m3,m4"));
    }

    void HumanInHold()
    {
        Facts human = Inside();
        human.engine = false;
        FakeWorld world = Party();
        world.facts[4] = human;
        world.places[4] = { 17.0f, 0.0f, 0.0f };   // 15 yd from the anchor, m2's place
        Counter counter;
        RunCore core = Started(world, counter);
        world.facts[1].alive = false;
        core.Reconcile(world, counter, 100);
        CHECK(core.mode == Mode::Hold && core.anchorKind == "cohort_m2");
        CHECK(!core.apart.count(4) && !core.held.count(4) && world.planCalls[4] == 0);
        // The leader back at the anchor: the run resumes with the person 15 yd off.
        world.facts[1] = Inside();
        world.places[1] = { 2.0f, 1.0f, 0.0f };
        core.Reconcile(world, counter, 120);
        CHECK(core.mode == Mode::Lead && core.reason == "resume_rejoined");
        // Again, the person 30 yd off: held until it comes.
        world.facts[1].alive = false;
        core.Reconcile(world, counter, 130);
        world.places[4] = { 32.0f, 0.0f, 0.0f };
        world.facts[1] = Inside();
        core.Reconcile(world, counter, 140);
        CHECK(core.mode == Mode::Hold && core.apart.count(4) && core.apart[4].human && core.apartSince == 0);
        core.Reconcile(world, counter, 140 + runrules::REJOIN_SECONDS + 60);
        CHECK(core.mode == Mode::Hold && !core.failed);
        world.places[4] = { 10.0f, 0.0f, 0.0f };
        core.Reconcile(world, counter, 900);
        CHECK(core.mode == Mode::Lead && core.reason == "resume_rejoined");

        // The leader taken over by a person: the run holds, as before, and fails nothing.
        world.facts[1].engine = false;
        core.Reconcile(world, counter, 1000);
        CHECK(core.mode == Mode::Hold && core.reason == "leader_taken_over");
        core.Reconcile(world, counter, 1000 + runrules::REJOIN_SECONDS + 60);
        CHECK(core.mode == Mode::Hold && !core.failed);
    }

    void PullPermitLost()
    {
        // A pull fired while the party was together, run after a member died: the permit is gone, whatever the stamp.
        FakeWorld world = Party();
        Counter counter;
        RunCore core = Started(world, counter);
        core.Reconcile(world, counter, 100);
        const runrules::Stamp fired = core.Stamp();
        CHECK(core.Permit());
        world.facts[2].alive = false;
        core.Reconcile(world, counter, 101);
        CHECK(runrules::Same(core.Stamp(), fired) && !core.Permit());
        // Alive again where it fell, by a resurrection: apart until the tick sees it with the party.
        world.facts[2] = Inside();
        CHECK(!core.Permit());
        core.Reconcile(world, counter, 102);
        CHECK(core.apart.empty() && core.Permit());
        // Released while the job takes the party out: no fight, and nobody walks back in.
        JobReport leave;
        leave.leaving = true;
        core.Settle(world, counter, leave);
        core.Reconcile(world, counter, 103);
        CHECK(core.mode == Mode::Released && !core.Permit() && core.apart.empty());
    }

    void RunMoveCleanup()
    {
        const runrules::Stamp stamp = { 3, 1 };
        // A current record is kept, its move untouched.
        {
            FakeMoves world;
            world.Add(2);
            RunMoves moves;
            moves.Start(world, 2, stamp, 1, Always, world.Push(2));
            moves.Clean(world, RunOf(1, stamp));
            CHECK(moves.Size() == 1 && world.removed.empty());
        }
        // After the run ended, and after it was removed: the move removed and the record retired.
        for (bool removedRun : { false, true })
        {
            FakeMoves world;
            world.Add(2);
            RunMoves moves;
            moves.Start(world, 2, stamp, 1, Always, world.Push(2));
            MoveRecord record;
            CHECK(moves.Find(2, record));
            moves.Clean(world, removedRun ? std::map<uint32_t, RunNow>() : RunOf(1, { 4, 0 }, Mode::Ended));
            CHECK(moves.Size() == 0 && !world.HasMove(2, record.token) && world.removed.size() == 1);
        }
        // A new generation or plan revision, or a member no longer expected: stale.
        for (int which = 0; which < 3; ++which)
        {
            FakeMoves world;
            world.Add(2);
            RunMoves moves;
            moves.Start(world, 2, stamp, 1, Always, world.Push(2));
            std::map<uint32_t, RunNow> runs = which == 0 ? RunOf(1, { 4, 0 }) : which == 1 ? RunOf(1, { 3, 2 })
                : RunOf(1, stamp, Mode::Lead, { 1, 3, 4, 5, 6 });
            moves.Clean(world, runs);
            CHECK(moves.Size() == 0 && world.removed.size() == 1);
        }
        // Off the record's instance: a map's pass, which cannot see the member, leaves it; the world pass removes it.
        {
            FakeMoves world;
            world.Add(2);
            RunMoves moves;
            moves.Start(world, 2, stamp, 1, Always, world.Push(2));
            world.members[2].instanceId = 8;
            world.members[2].visible = false;
            moves.Clean(world, RunOf(1, stamp), 1);
            CHECK(moves.Size() == 1 && world.removed.empty());
            world.members[2].visible = true;
            moves.Clean(world, RunOf(1, stamp));
            CHECK(moves.Size() == 0 && world.removed.size() == 1);
        }
        // Between maps, or a player's: left for later, the move untouched. Offline: dropped, the stack gone with it.
        {
            FakeMoves world;
            world.Add(2);
            world.Add(3);
            world.Add(4);
            RunMoves moves;
            for (uint32_t guid : { 2u, 3u, 4u })
                moves.Start(world, guid, stamp, 1, Always, world.Push(guid));
            world.members[2].teleporting = true;
            world.members[3].engine = false;
            world.members[4].online = false;
            moves.Clean(world, RunOf(1, { 4, 0 }));
            MoveRecord record;
            CHECK(moves.Find(2, record) && moves.Find(3, record) && !moves.Find(4, record) && world.removed.empty());
        }
        // A move gone from the stack without the run's removal: retired, nothing removed.
        {
            FakeMoves world;
            world.Add(2);
            RunMoves moves;
            moves.Start(world, 2, stamp, 1, Always, world.Push(2));
            world.stacks[2].pop_back();
            moves.Clean(world, RunOf(1, stamp));
            CHECK(moves.Size() == 0 && world.removed.empty());
        }
        // A removal whose callback records a replacement: the replacement's record stays.
        {
            FakeMoves world;
            world.Add(2);
            RunMoves moves;
            moves.Start(world, 2, stamp, 1, Always, world.Push(2));
            MoveRecord stale;
            CHECK(moves.Find(2, stale));
            const runrules::Stamp next = { 4, 0 };
            world.onRemove[stale.token] = [&]() { moves.Start(world, 2, next, 1, Always, world.Push(2)); };
            moves.Clean(world, RunOf(1, next));
            MoveRecord now;
            CHECK(moves.Find(2, now) && now.token != stale.token && runrules::Same(now.stamp, next)
                && world.HasMove(2, now.token));
        }
        // A map's pass sees its own run's records only.
        {
            FakeMoves world;
            world.Add(2);
            world.Add(12);
            RunMoves moves;
            moves.Start(world, 2, stamp, 1, Always, world.Push(2));
            moves.Start(world, 12, stamp, 11, Always, world.Push(12));
            moves.Clean(world, RunOf(1, { 4, 0 }), 1);
            MoveRecord record;
            CHECK(!moves.Find(2, record) && moves.Find(12, record) && world.removed.size() == 1);
        }
    }

    // Readiness: one member that keeps the pull back for each reason alone, the thresholds' edges, and the leader as one
    // of the members like any other.
    runrules::Ready ReadyMember(uint32_t guid)
    {
        runrules::Ready r;
        r.guid = guid;
        r.facts.online = r.facts.engine = r.facts.inWorld = r.facts.inInstance = r.facts.alive = true;
        r.distance = 4.0f;
        return r;
    }

    std::vector<runrules::Keep> WhyOf(const std::vector<runrules::Ready>& members, uint32_t guid)
    {
        std::vector<runrules::Keep> why;
        for (const runrules::Kept& k : runrules::Readiness(members))
            if (k.guid == guid)
                why.push_back(k.why);
        return why;
    }

    void ReadinessReasons()
    {
        using runrules::Keep;
        std::vector<runrules::Ready> party;
        for (uint32_t guid = 1; guid <= 5; ++guid)
            party.push_back(ReadyMember(guid));
        party[1].usesMana = true;
        CHECK(runrules::Readiness(party).empty());

        auto one = [&](std::function<void(runrules::Ready&)> change, uint32_t guid) {
            std::vector<runrules::Ready> p = party;
            change(p[guid - 1]);
            return WhyOf(p, guid);
        };
        CHECK(one([](runrules::Ready& r) { r.facts.alive = false; }, 3) == std::vector<Keep>{ Keep::Dead });
        CHECK(one([](runrules::Ready& r) { r.facts.inInstance = false; }, 3) == std::vector<Keep>{ Keep::Away });
        CHECK(one([](runrules::Ready& r) { r.facts.online = false; }, 3) == std::vector<Keep>{ Keep::Away });
        CHECK(one([](runrules::Ready& r) { r.facts.teleporting = true; }, 3) == std::vector<Keep>{ Keep::Away });
        CHECK(one([](runrules::Ready& r) { r.rejoining = true; }, 3) == std::vector<Keep>{ Keep::Away });
        // One not there is only dead or away, whatever else is true of it.
        CHECK(one([](runrules::Ready& r) { r.facts.alive = false; r.health = 0; r.distance = 90.0f; }, 3) == std::vector<Keep>{ Keep::Dead });
        CHECK(one([](runrules::Ready& r) { r.inFight = true; }, 3) == std::vector<Keep>{ Keep::Fight });
        CHECK(one([](runrules::Ready& r) { r.distance = 12.0f; }, 3).empty());
        CHECK(one([](runrules::Ready& r) { r.distance = 12.5f; }, 3) == std::vector<Keep>{ Keep::Far });
        CHECK(one([](runrules::Ready& r) { r.health = 80; }, 3).empty());
        CHECK(one([](runrules::Ready& r) { r.health = 79; }, 3) == std::vector<Keep>{ Keep::Health });
        CHECK(one([](runrules::Ready& r) { r.mana = 60; }, 2).empty());
        CHECK(one([](runrules::Ready& r) { r.mana = 59; }, 2) == std::vector<Keep>{ Keep::Mana });
        // A rage or energy user's mana never counts.
        CHECK(one([](runrules::Ready& r) { r.mana = 0; r.hasDrink = false; }, 3).empty());
        // The leader is the first member, and counts like the rest.
        CHECK(one([](runrules::Ready& r) { r.health = 50; }, 1) == std::vector<Keep>{ Keep::Health });
        // Several reasons of one member are each said.
        CHECK(one([](runrules::Ready& r) { r.distance = 30.0f; r.mana = 10; }, 2) == (std::vector<Keep>{ Keep::Far, Keep::Mana }));
    }

    void ReadinessSupplies()
    {
        using runrules::Keep;
        runrules::Ready r = ReadyMember(2);
        r.usesMana = true;
        r.mana = 10;
        r.hasDrink = false;
        CHECK(WhyOf({ r }, 2) == std::vector<Keep>{ Keep::Supplies });
        CHECK(runrules::BlocksAtOnce(runrules::Readiness({ r })));
        r.hasDrink = true;
        CHECK(WhyOf({ r }, 2) == std::vector<Keep>{ Keep::Mana });
        CHECK(!runrules::BlocksAtOnce(runrules::Readiness({ r })));
        // No drink at full mana is nothing yet.
        r.mana = 100;
        r.hasDrink = false;
        CHECK(WhyOf({ r }, 2).empty());
        runrules::Ready h = ReadyMember(3);
        h.health = 40;
        h.hasFood = false;
        CHECK(WhyOf({ h }, 3) == std::vector<Keep>{ Keep::Supplies });
        h.hasFood = true;
        CHECK(WhyOf({ h }, 3) == std::vector<Keep>{ Keep::Health });
        // Short of both with no food and no drink: supplies once.
        r.mana = 10;
        r.health = 40;
        r.hasFood = false;
        CHECK(WhyOf({ r }, 2) == std::vector<Keep>{ Keep::Supplies });
        auto names = [](uint32_t guid) { return std::string("m") + std::to_string(guid); };
        CHECK(runrules::KeepText(runrules::Readiness({ r, h }), names) == "m2:supplies,m3:health");
    }

    void ReadinessBudget()
    {
        runrules::Budget budget;
        // Not started before the leader gathers.
        runrules::TrackBudget(budget, 4, 7, false, 100);
        CHECK(budget.since == 0 && !runrules::BudgetOut(budget, 10000));
        runrules::TrackBudget(budget, 4, 7, true, 120);
        CHECK(budget.since == 120);
        // Kept across a change of reasons, a pull that brought nothing, and a wait at the stop before: the stop and the
        // generation are the same, gathered or not.
        runrules::TrackBudget(budget, 4, 7, false, 200);
        runrules::TrackBudget(budget, 4, 7, true, 260);
        CHECK(budget.since == 120);
        CHECK(!runrules::BudgetOut(budget, 120 + 299));
        CHECK(runrules::BudgetOut(budget, 120 + 300));
        // A new generation starts it again at the next gather.
        runrules::TrackBudget(budget, 4, 8, false, 500);
        CHECK(budget.since == 0 && !runrules::BudgetOut(budget, 900));
        runrules::TrackBudget(budget, 4, 8, true, 510);
        CHECK(budget.since == 510);
        // So does a new stop.
        runrules::TrackBudget(budget, 5, 8, true, 600);
        CHECK(budget.since == 600 && budget.stop == 5);
    }

    void ReadinessRecovery()
    {
        using runrules::Keep;
        using runrules::Recovery;
        // Mixed reasons keep their own deadlines: a mana obligation does not last as long as a dead member's.
        std::vector<runrules::Obligation> obligations = runrules::Obligations({ { 2, Keep::Mana }, { 4, Keep::Dead } }, 1000);
        CHECK(obligations.size() == 2 && obligations[0].deadline == 1180 && obligations[1].deadline == 1300);
        runrules::Ready healer = ReadyMember(2), rogue = ReadyMember(4);
        healer.usesMana = true;
        healer.mana = 30;
        rogue.facts.alive = false;
        CHECK(runrules::RecoveryStep(obligations, { healer, rogue }, 1100) == Recovery::Going);
        // The mana obligation passes its own deadline unmet: the recovery is over, though the dead member's runs on.
        CHECK(runrules::RecoveryStep(obligations, { healer, rogue }, 1180) == Recovery::Over);
        healer.mana = 80;
        CHECK(runrules::RecoveryStep(obligations, { healer, rogue }, 1180) == Recovery::Going);
        rogue.facts.alive = true;
        CHECK(runrules::RecoveryStep(obligations, { healer, rogue }, 1200) == Recovery::Done);
        // One the tick cannot see at all stays unmet.
        CHECK(runrules::RecoveryStep(obligations, { healer }, 1200) == Recovery::Going);
        // A member gone off the map keeps its health obligation unmet, at full health or not.
        std::vector<runrules::Obligation> health = runrules::Obligations({ { 3, Keep::Health } }, 0);
        runrules::Ready mage = ReadyMember(3);
        mage.facts.inInstance = false;
        CHECK(runrules::RecoveryStep(health, { mage }, 10) == Recovery::Going);
        CHECK(runrules::RecoveryStep(health, { mage }, 180) == Recovery::Over);
        mage.facts.inInstance = true;
        CHECK(runrules::RecoveryStep(health, { mage }, 10) == Recovery::Done);
    }

    void ReadinessSupplyTrip()
    {
        using runrules::Keep;
        using runrules::Recovery;
        std::vector<runrules::Obligation> trip = runrules::Obligations({ { 3, Keep::Supplies } }, 0);
        CHECK(trip[0].deadline == 900);
        runrules::Ready mage = ReadyMember(3);
        mage.usesMana = true;
        mage.mana = 10;
        mage.hasDrink = false;
        // On the way out the member is off the map: its supplies leave readiness, but the obligation stays unmet.
        mage.facts.inInstance = false;
        CHECK(runrules::Readiness({ mage }).size() == 1 && runrules::Readiness({ mage })[0].why == Keep::Away);
        CHECK(runrules::RecoveryStep(trip, { mage }, 60) == Recovery::Going);
        // Drink bought outside is not enough: it must be back with the party.
        mage.hasDrink = true;
        CHECK(runrules::RecoveryStep(trip, { mage }, 400) == Recovery::Going);
        mage.facts.inInstance = true;
        mage.rejoining = true;
        CHECK(runrules::RecoveryStep(trip, { mage }, 500) == Recovery::Going);
        mage.rejoining = false;
        CHECK(runrules::RecoveryStep(trip, { mage }, 500) == Recovery::Done);
        // A trip interrupted, the member back with no drink: over at 15 minutes, not before.
        mage.hasDrink = false;
        CHECK(runrules::RecoveryStep(trip, { mage }, 899) == Recovery::Going);
        CHECK(runrules::RecoveryStep(trip, { mage }, 900) == Recovery::Over);
        // Drinking now, its last drink in use, counts as supplied (the caller's fact).
        mage.hasDrink = true;
        CHECK(runrules::Readiness({ mage })[0].why == Keep::Mana);
        // So does a mage that knows Conjure Water, with none in its bags: short of mana, it waits and conjures; no trip.
        runrules::Ready conjurer = ReadyMember(3);
        conjurer.usesMana = true;
        conjurer.mana = 10;
        conjurer.hasDrink = true;
        CHECK(WhyOf({ conjurer }, 3) == std::vector<Keep>{ Keep::Mana });
        CHECK(!runrules::BlocksAtOnce(runrules::Readiness({ conjurer })));
    }

    void ReadinessBlocks()
    {
        using runrules::BlockOutcome;
        using runrules::Keep;
        std::map<uint32_t, uint32_t> counts;
        // Keyed by the stop's pack: the same stop chosen again at another index counts on.
        CHECK(runrules::CountBlock(counts, 41) == BlockOutcome::Recover);
        CHECK(runrules::CountBlock(counts, 52) == BlockOutcome::Recover);
        CHECK(runrules::CountBlock(counts, 41) == BlockOutcome::Recover);
        // The third at one stop fails the run, with no third recovery.
        CHECK(runrules::CountBlock(counts, 41) == BlockOutcome::Fail);
        // Only a verified clear forgets a stop's count.
        runrules::StopCleared(counts, 52);
        CHECK(!counts.count(52) && counts[41] == 3);
        // One block reconciled twice is taken once.
        uint32_t taken = 0;
        CHECK(runrules::TakeBlock(taken, 7));
        CHECK(!runrules::TakeBlock(taken, 7));
        CHECK(runrules::TakeBlock(taken, 8));
        CHECK(!runrules::TakeBlock(taken, 0));
        // A block is due only with reasons: a party ready at the deadline gets none.
        runrules::Budget budget{ 3, 1, 100 };
        CHECK(!runrules::BlockDue(budget, 400, {}, true));
        CHECK(runrules::BlockDue(budget, 400, { { 2, Keep::Far } }, true));
        CHECK(!runrules::BlockDue(budget, 399, { { 2, Keep::Far } }, true));
        CHECK(runrules::BlockDue(budget, 120, { { 2, Keep::Supplies } }, true));
        // Supplies block at once only with the leader gathered at its spot: walking there, or back from a fight, the
        // party's far members are no reason, and the budget decides.
        CHECK(!runrules::BlockDue(budget, 120, { { 1, Keep::Far }, { 2, Keep::Supplies } }, false));
        CHECK(runrules::BlockDue(budget, 400, { { 1, Keep::Far }, { 2, Keep::Supplies } }, false));
    }

    void ReadinessPullGate()
    {
        using runrules::Keep;
        CHECK(runrules::PullAllowed(false, false, {}));
        // A pull fired before the block, executed after it: refused.
        CHECK(!runrules::PullAllowed(true, false, {}));
        CHECK(!runrules::PullAllowed(false, true, {}));
        // A pull fired while ready, executed after a member's mana dropped: refused by the fresh readiness.
        CHECK(!runrules::PullAllowed(false, false, { { 2, Keep::Mana } }));
    }

    void ReadinessRepath()
    {
        runrules::Nearing nearing;
        CHECK(!runrules::Repath(nearing, 1, 77, 30.0f, 0));
        CHECK(!runrules::Repath(nearing, 1, 77, 25.0f, 5));
        // Not nearer by a yard for 20 seconds from the last gain: a new path, and the clock starts again.
        CHECK(!runrules::Repath(nearing, 1, 77, 24.5f, 24));
        CHECK(runrules::Repath(nearing, 1, 77, 24.5f, 25));
        CHECK(!runrules::Repath(nearing, 1, 77, 24.5f, 44));
        CHECK(runrules::Repath(nearing, 1, 77, 24.5f, 45));
        // A new destination or generation starts the tracking again.
        CHECK(!runrules::Repath(nearing, 1, 78, 24.5f, 70));
        CHECK(!runrules::Repath(nearing, 1, 78, 24.5f, 89));
        CHECK(!runrules::Repath(nearing, 2, 78, 24.5f, 95));
        CHECK(!runrules::Repath(nearing, 2, 78, 24.5f, 114));
        CHECK(runrules::Repath(nearing, 2, 78, 24.5f, 115));
    }

    // The core's side of a block: taken once, a recovery with its obligations, and back to lead.
    void BlockTakenOnce()
    {
        FakeWorld world = Party();
        Counter counter;
        RunCore core = Started(world, counter);
        core.Reconcile(world, counter, 100);
        CHECK(core.mode == Mode::Lead && core.Permit());
        const uint32_t generation = core.generation;
        world.ready[3].usesMana = true;
        world.ready[3].mana = 10;
        core.TakeBlock(world, counter, 1, generation, 41, { { 3, runrules::Keep::Mana } }, "m3:mana", 100);
        CHECK(core.mode == Mode::Released && core.reason == "recovery,m3:mana" && core.recovering);
        CHECK(core.Control() == Control::Recover && !core.Permit());
        CHECK(HasEvent(core, "run_block", "result=recover stop=41 count=1/3 reasons=m3:mana"));
        // The same block again, reconciled twice: nothing more.
        core.events.clear();
        core.TakeBlock(world, counter, 1, generation, 41, { { 3, runrules::Keep::Mana } }, "m3:mana", 101);
        CHECK(core.events.empty() && core.blockCounts[41] == 1);
        // The job's report does not hand a recovery back.
        JobReport inside;
        inside.inside = true;
        core.Settle(world, counter, inside);
        CHECK(core.mode == Mode::Released && core.recovering);
        // Under Recover the job pulls nothing and runs no step; its recovery and the trip's way out and back act.
        CHECK(runrules::Job(Control::Recover, JobAct::Step, true) == JobMay::Return);
        CHECK(runrules::Job(Control::Recover, JobAct::MoveOn, true) == JobMay::Return);
        CHECK(runrules::Job(Control::Recover, JobAct::Recover, true) == JobMay::Act);
        CHECK(runrules::Job(Control::Recover, JobAct::LeaveRepair, true) == JobMay::Act);
        runrules::DispatchFacts facts;
        facts.control = Control::Recover;
        facts.phase = runrules::Phase::Bosses;
        facts.onDungeonMap = facts.partyIn = true;
        CHECK(runrules::Decide(facts) == runrules::Dispatch::Recover);
        facts.phase = runrules::Phase::Leave;
        CHECK(runrules::Decide(facts) == runrules::Dispatch::Leave);
        // Mana not back: going on. Back: lead, a new generation, the leader choosing its stop again.
        core.Reconcile(world, counter, 150);
        CHECK(core.mode == Mode::Released && core.recovering);
        world.ready[3].mana = 70;
        const uint32_t before = core.generation;
        core.Reconcile(world, counter, 160);
        CHECK(core.mode == Mode::Lead && core.reason == "recovered" && !core.recovering && core.choose);
        CHECK(core.generation != before && core.Permit());
        CHECK(HasEvent(core, "run_recovery", "result=done obligations=m3:mana:met"));
    }

    void BlockStaleAndOver()
    {
        FakeWorld world = Party();
        Counter counter;
        RunCore core = Started(world, counter);
        core.Reconcile(world, counter, 100);
        // A block written under a generation since replaced is the old plan's.
        core.TakeBlock(world, counter, 1, core.generation + 5, 41, { { 3, runrules::Keep::Far } }, "m3:far", 100);
        CHECK(core.mode == Mode::Lead && core.blockCounts.empty());
        // An empty block is no block.
        core.TakeBlock(world, counter, 2, core.generation, 41, {}, "", 100);
        CHECK(core.mode == Mode::Lead && core.blockCounts.empty());
        // A far member that never comes nearer: over at its own deadline, back to lead.
        world.ready[3].distance = 40.0f;
        core.TakeBlock(world, counter, 3, core.generation, 41, { { 3, runrules::Keep::Far } }, "m3:far", 100);
        CHECK(core.recovering);
        core.Reconcile(world, counter, 279);
        CHECK(core.recovering);
        core.Reconcile(world, counter, 280);
        CHECK(core.mode == Mode::Lead && core.reason == "recovery_over");
        CHECK(HasEvent(core, "run_recovery", "result=over obligations=m3:far:unmet"));
    }

    void ThirdBlockFails()
    {
        FakeWorld world = Party();
        Counter counter;
        RunCore core = Started(world, counter);
        core.Reconcile(world, counter, 100);
        world.ready[2].health = 50;
        time_t now = 100;
        for (uint32_t id = 1; id <= 2; ++id)
        {
            core.TakeBlock(world, counter, id, core.generation, 41, { { 2, runrules::Keep::Health } }, "m2:health", now);
            CHECK(core.recovering && core.blockCounts[41] == id);
            now += runrules::RECOVERY_SECONDS;
            core.Reconcile(world, counter, now);
            CHECK(core.mode == Mode::Lead && !core.recovering);
        }
        // Another stop's block counts apart.
        core.TakeBlock(world, counter, 3, core.generation, 52, { { 2, runrules::Keep::Health } }, "m2:health", now);
        CHECK(core.blockCounts[52] == 1);
        now += runrules::RECOVERY_SECONDS;
        core.Reconcile(world, counter, now);
        // The third at stop 41: the run ends failed, and no third recovery starts there.
        core.events.clear();
        core.TakeBlock(world, counter, 4, core.generation, 41, { { 2, runrules::Keep::Health } }, "m2:health", now);
        CHECK(core.mode == Mode::Ended && core.failed && core.reason == "blocked,m2:health" && !core.recovering);
        CHECK(HasEvent(core, "run_block", "result=fail stop=41 count=3/3"));
        CHECK(!HasEvent(core, "run_mode", "mode=released"));
        // A verified clear forgets a stop's count.
        core.StopCleared(52);
        CHECK(!core.blockCounts.count(52));
    }

    void RecoveryThroughHold()
    {
        FakeWorld world = Party();
        Counter counter;
        RunCore core = Started(world, counter);
        core.Reconcile(world, counter, 100);
        world.ready[3].usesMana = true;
        world.ready[3].mana = 10;
        core.TakeBlock(world, counter, 1, core.generation, 41, { { 3, runrules::Keep::Mana } }, "m3:mana", 100);
        // The leader dies during the recovery: a hold, and the recovery is kept.
        world.facts[1].alive = false;
        core.Reconcile(world, counter, 110);
        CHECK(core.mode == Mode::Hold && core.recovering && core.Control() == Control::Hold);
        world.facts[1] = Inside();
        for (time_t now = 120; now < 200 && core.mode == Mode::Hold; now += 5)
            core.Reconcile(world, counter, now);
        CHECK(core.mode == Mode::Released && core.recovering && core.Control() == Control::Recover);
        world.ready[3].mana = 90;
        core.Reconcile(world, counter, 210);
        CHECK(core.mode == Mode::Lead && !core.recovering);
    }

    void ApartOverdueOnlyInHold()
    {
        FakeWorld world = Party();
        Counter counter;
        RunCore core = Started(world, counter);
        core.Reconcile(world, counter, 100);
        // A member apart in lead: the pull is held, but the run is not ended by time; blocks govern.
        world.places[5] = { 300.0f, 0.0f, 0.0f };
        world.plans[5] = runrules::PlanResult::Blocked;
        core.Reconcile(world, counter, 110);
        CHECK(core.mode == Mode::Lead && core.apart.count(5) && !core.Permit());
        core.Reconcile(world, counter, 110 + runrules::REJOIN_SECONDS + 5);
        CHECK(core.mode == Mode::Lead && !core.failed && core.apartSince == 0);
    }

    // A supply trip with no safe way out: no trip and no walk, the supplies obligation a wait for the member's own mana.
    void SupplyNoSafeWay()
    {
        FakeWorld world = Party();
        Counter counter;
        RunCore core = Started(world, counter);
        core.Reconcile(world, counter, 100);
        world.ready[3].usesMana = true;
        world.ready[3].mana = 10;
        world.ready[3].hasDrink = false;
        world.plans[1] = runrules::PlanResult::Blocked;
        core.TakeBlock(world, counter, 1, core.generation, 41, { { 3, runrules::Keep::Supplies } }, "m3:supplies", 100);
        CHECK(core.exitWanted && core.recovering);
        core.Reconcile(world, counter, 105);
        CHECK(!core.exitWanted && !core.exiting && core.apart.empty());
        CHECK(HasEvent(core, "supply_trip", "result=no_safe_way blocker=spawn_99_stop_3"));
        // The obligation is now the member's own health and mana, from now with the usual deadline; no block counted.
        CHECK(core.obligations.size() == 2 && core.obligations[1].why == runrules::Keep::Mana && core.obligations[1].deadline == 105 + 180);
        CHECK(core.blockCounts[41] == 1);
        world.ready[3].mana = 70;
        core.Reconcile(world, counter, 150);
        CHECK(core.mode == Mode::Lead && HasEvent(core, "run_recovery", "result=done"));

        // The way out not planned in time, the tick's searches spent: the same.
        FakeWorld second = Party();
        Counter counter2;
        RunCore other = Started(second, counter2);
        other.Reconcile(second, counter2, 100);
        second.ready[3].usesMana = true;
        second.ready[3].mana = 10;
        second.ready[3].hasDrink = false;
        second.plans[1] = runrules::PlanResult::Pending;
        other.TakeBlock(second, counter2, 1, other.generation, 41, { { 3, runrules::Keep::Supplies } }, "m3:supplies", 100);
        other.Reconcile(second, counter2, 101);
        CHECK(other.exitWanted && !HasEvent(other, "supply_trip", "no_safe_way"));
        other.Reconcile(second, counter2, 101 + runrules::EXIT_PLAN_SECONDS);
        CHECK(!other.exitWanted && HasEvent(other, "supply_trip", "result=no_safe_way blocker=no_plan_in_time"));
    }

    // A clear way out: every member walks the checked way to the exit point, and only then does the job take them out.
    void SupplyWalksOut()
    {
        FakeWorld world = Party();
        Counter counter;
        RunCore core = Started(world, counter);
        core.Reconcile(world, counter, 100);
        world.ready[3].usesMana = true;
        world.ready[3].mana = 10;
        world.ready[3].hasDrink = false;
        core.TakeBlock(world, counter, 1, core.generation, 41, { { 3, runrules::Keep::Supplies } }, "m3:supplies", 100);
        core.Reconcile(world, counter, 105);
        CHECK(core.exiting && !core.exitDone && HasEvent(core, "supply_trip", "result=walking_out"));
        // All five apart, the leader among them; on the next reconcile each walks a checked part toward the exit point.
        CHECK(core.apart.size() == 5);
        core.Reconcile(world, counter, 106);
        for (uint32_t guid : world.members)
            CHECK(core.apart[guid].walking && core.apart[guid].walk.to.x == -200.0f);
        CHECK(!core.Permit());
        // The members reach the exit: the walk out is done, and the job may take them out.
        for (uint32_t guid : world.members)
            world.places[guid] = { -200.0f + float(guid), 0.0f, 0.0f };
        core.Reconcile(world, counter, 160);
        CHECK(!core.exiting && core.exitDone && core.apart.empty() && HasEvent(core, "supply_trip", "result=at_exit"));
        CHECK(core.mode == Mode::Released && core.recovering);
        // Out on the trip the obligations wait; back inside with drink, rejoined: done.
        JobReport out;
        out.leaving = true;
        core.Settle(world, counter, out);
        core.Reconcile(world, counter, 300);
        CHECK(core.recovering);
        JobReport back;
        back.inside = true;
        core.Settle(world, counter, back);
        world.ready[3].hasDrink = true;
        core.Reconcile(world, counter, 400);
        CHECK(core.mode == Mode::Lead && !core.recovering && HasEvent(core, "run_recovery", "result=done"));
    }
}

int main()
{
    Stamps();
    HoldReasons();
    Resume();
    Geometry();
    WayPassedTwice();
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
    Tokens();
    Records();
    Replacement();
    Tags();
    RunMoveStart();
    RunMoveDeferredAndStale();
    RunMoveCallbackReplacement();
    RunMoveIssueReentrancy();
    RunMoveCleanup();
    CopyA();
    HoldPlaceAndMinute();
    BlockedEndsTheRun();
    ApartInLead();
    WalksPlannedAgain();
    NoAnchor();
    FailureOutlivesTheRun();
    PullPermitLost();
    HumanMember();
    HumanInHold();
    FailedPartyKeepsGroup();
    SummonInTheWay();
    ReadinessReasons();
    ReadinessSupplies();
    ReadinessBudget();
    ReadinessRecovery();
    ReadinessSupplyTrip();
    ReadinessBlocks();
    ReadinessPullGate();
    ReadinessRepath();
    BlockTakenOnce();
    BlockStaleAndOver();
    ThirdBlockFails();
    RecoveryThroughHold();
    ApartOverdueOnlyInHold();
    SupplyNoSafeWay();
    SupplyWalksOut();
    printf("%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
