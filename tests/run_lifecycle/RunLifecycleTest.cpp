// Tests of a party's run lifecycle (playerbot/GuildRunCore, playerbot/GuildRunRules), with a fake world that records
// every lookup and motion call. Built and run by tests/run_lifecycle/run.sh, without the server.

#include "GuildRunCore.h"
#include "GuildRunPlayer.h"
#include "GuildRunGuard.h"
#include "GuildRunTactics.h"

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
        // The blockers no longer reaching the stop's places (runguard::WaitReaches), by spawn.
        std::set<uint32_t> blockersGone;
        bool BlockerGone(uint32_t spawn) override { return blockersGone.count(spawn) > 0; }
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
        // A wanderer's range spreads its places across only, by the one rule (runrules::PlaceDistance): a foundry
        // wanderer 23 yards under a walkway, ranging 12, reach 21.9, never reaches the walkway above its range, which the
        // old sphere of reach and range (33.9) took; on its own floor its range carries its reach 12 yards farther.
        {
            runrules::Threat wanderer;
            wanderer.spawn = 3;
            wanderer.where = { { 50, 0, 21 } };
            wanderer.reach = 21.9f;
            wanderer.spread = 12.0f;
            const std::vector<Vec> walkway = { { 30, 10, 44 }, { 70, 10, 44 } };
            CHECK(runrules::LinesDistance(wanderer.where, walkway) <= wanderer.reach + wanderer.spread);
            CHECK(runrules::FirstThreat({ wanderer }, walkway) < 0);
            CHECK(std::fabs(runrules::PlaceDistance(walkway, wanderer.where, 12.0f) - 23.0f) < 0.05f);
            const std::vector<Vec> floor = { { 30, 30, 21 }, { 70, 30, 21 } };
            CHECK(runrules::FirstThreat({ wanderer }, floor) == 0);
            wanderer.spread = 0.0f;
            CHECK(runrules::FirstThreat({ wanderer }, floor) < 0);
            // A mover's travel the same: an unmodelled creature in a fight under the walkway, TRAVEL_MARGIN across.
            runrules::Threat mover;
            mover.entry = 4417;
            mover.where = { { 50, 10, 21 } };
            mover.reach = 21.5f;
            mover.spread = runrules::TRAVEL_MARGIN;
            CHECK(runrules::FirstThreat({ mover }, walkway) < 0);
            CHECK(runrules::FirstThreat({ mover }, { { 30, 40, 21 }, { 70, 40, 21 } }) == 0);
        }
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

    // Item 6: a person in the party, its pure decisions (GuildRunPlayer.h).
    void PersonControl()
    {
        using namespace runplayer;
        // A logout the client asked for hands back; a closed socket, even after the request, and a kick do not.
        CHECK(LogoutHandsBack(true, false));
        CHECK(!LogoutHandsBack(true, true));     // requested, then the client closed mid-countdown
        CHECK(!LogoutHandsBack(false, true));    // the socket closed
        CHECK(!LogoutHandsBack(false, false));   // an AFK or GM kick
        // The login loop: never a reserved character; a handed-back one at once; otherwise after the relog delay.
        CHECK(!EngineLogsIn(true, true, false, 10000, 60));
        CHECK(EngineLogsIn(true, false, true, 0, 60));
        CHECK(!EngineLogsIn(true, false, false, 59, 60) && EngineLogsIn(true, false, false, 60, 60));
        CHECK(!EngineLogsIn(false, false, true, 0, 60));
        // The offline timer: from the disconnect, not moved by a second one, reset only at the reconnection.
        OfflineClock offline;
        offline.Disconnected(100);
        offline.Disconnected(500);
        CHECK(offline.since == 100 && !offline.Gone(100 + PLAYER_GONE_SECONDS - 1) && offline.Gone(100 + PLAYER_GONE_SECONDS));
        offline.Reconnected();
        CHECK(!offline.Gone(100000));
        offline.Disconnected(3000);
        CHECK(!offline.Gone(3000 + PLAYER_GONE_SECONDS - 1));
        // A stop is neither done nor failed, and never counts as a run done.
        CHECK(std::string(JobStatus(Outcome::Stopped)) == "stopped" && std::string(JobStatus(Outcome::Failed)) == "failed");
        CHECK(!CountsAsDone(Outcome::Stopped) && !CountsAsDone(Outcome::Failed));
        // The leader played by a person: held while played, offline held as player_offline.
        CHECK(LeaderHold(Controller::Engine, true).empty());
        CHECK(LeaderHold(Controller::Person, true) == "leader_taken_over");
        CHECK(LeaderHold(Controller::Person, false) == "player_offline");
        // A change of control, in order: cancel before the revision, the revision before ownership; back the other way.
        CHECK((TransferSteps(Controller::Person) == std::vector<Step>{ Step::CancelMoves, Step::CancelPull, Step::Revise, Step::Ownership }));
        CHECK((TransferSteps(Controller::Engine) == std::vector<Step>{ Step::Revise, Step::Ownership, Step::Strategies, Step::Rejoin }));
    }

    void PersonWaiting()
    {
        using namespace runplayer;
        // Only a person keeps the pull back: no budget, no block, as long as it lasts. Mixed: the bots' recovery runs.
        CHECK(WaitOf(0, 0) == Wait::None && WaitOf(2, 0) == Wait::Person && WaitOf(0, 1) == Wait::Bots && WaitOf(1, 1) == Wait::Mixed);
        CHECK(!BudgetRuns(Wait::Person) && !BlockCounts(Wait::Person) && !BudgetRuns(Wait::None));
        CHECK(BudgetRuns(Wait::Mixed) && BlockCounts(Wait::Mixed) && BlockCounts(Wait::Bots));
        // Past a third block's timeline the person's wait counts nothing: still no block after an hour.
        int blocks = 0;
        for (int minute = 0; minute < 60; ++minute)
            blocks += BlockCounts(WaitOf(1, 0)) ? 1 : 0;
        CHECK(blocks == 0);
        CHECK(!MayResupply(true) && MayResupply(false));
        // Each bot's own time apart: a long wait for a person uses none of it; a bot newly apart starts at zero, and a
        // reason to hold starts it over.
        BotClock bot;
        for (int64_t t = 0; t <= 3000; t += 10)
            bot.Tick(t, false, false);
        CHECK(bot.apartFor == 0);
        bot.Tick(3010, true, false);
        bot.Tick(3020, true, false);
        CHECK(bot.apartFor == 10);
        bot.Tick(3500, true, true);
        CHECK(bot.apartFor == 0);
        bot.Tick(3510, true, false);
        bot.Tick(3520, true, false);
        CHECK(bot.apartFor == 10 && !bot.Overdue(runrules::REJOIN_SECONDS));
        bot.Tick(3510 + runrules::REJOIN_SECONDS, true, false);
        CHECK(bot.Overdue(runrules::REJOIN_SECONDS));
        bot.Tick(5000, false, false);
        CHECK(bot.apartFor == 0);
        // Eating or drinking from an item, seated: waited for at any mana; a buff's regeneration does not count.
        CHECK(Eating(true, true) && !Eating(false, true) && !Eating(true, false));
        // A person healer: in sight of the tank and within 40 yd.
        CHECK(HealerReady(false, false, 100.0f));
        CHECK(HealerReady(true, true, 39.0f) && !HealerReady(true, false, 10.0f) && !HealerReady(true, true, 41.0f));
        CHECK(!HealerReady(true, true, -1.0f));
        // A fight the leader did not pull: outside the pull phase, or not with the pull's creature.
        CHECK(UnplannedFight(true, false, false) && UnplannedFight(true, true, false));
        CHECK(!UnplannedFight(true, true, true) && !UnplannedFight(false, false, false));
    }

    // A person's member disconnects: still the person's, the run holds as player_offline and runs no bot's clock; back,
    // the hold ends; gone half an hour, the run stops, which is no failure.
    void PersonOffline()
    {
        Facts human = Inside();
        human.engine = false;
        FakeWorld world = Party();
        world.facts[4] = human;
        world.places[4] = { 6.0f, 0.0f, 0.0f };
        Counter counter;
        RunCore core = Started(world, counter);
        core.Reconcile(world, counter, 100);
        CHECK(core.controllers[4] == runplayer::Controller::Person && core.Permit());
        world.facts[4] = Facts();                 // offline: nothing known of it but who played it
        core.Reconcile(world, counter, 110);
        CHECK(core.mode == Mode::Hold && core.reason == "player_offline" && core.controllers[4] == runplayer::Controller::Person);
        CHECK(core.apart.count(4) && core.apart[4].human && !core.botClocks[4].Overdue(1));
        core.Reconcile(world, counter, 110 + runrules::REJOIN_SECONDS + 60);
        CHECK(core.mode == Mode::Hold && !core.failed && !core.stopped);
        // Back after twenty minutes: the clock starts over, and the hold ends once it is with the party.
        world.facts[4] = human;
        core.Reconcile(world, counter, 1310);
        CHECK(core.offline[4].since == 0 && core.mode == Mode::Lead && core.reason == "resume_rejoined");
        // Off again, for good: thirty minutes from this disconnect, not the first.
        world.facts[4] = Facts();
        core.Reconcile(world, counter, 1400);
        core.Reconcile(world, counter, 1400 + runplayer::PLAYER_GONE_SECONDS - 1);
        CHECK(core.mode == Mode::Hold && !core.stopped);
        core.Reconcile(world, counter, 1400 + runplayer::PLAYER_GONE_SECONDS);
        CHECK(core.mode == Mode::Ended && core.reason == "player_gone" && core.stopped && !core.failed && !core.Permit());
        CHECK(HasEvent(core, "run_player", "result=gone members=m4"));

        // The leader played by a person: held as taken over, and offline as player_offline, not leader_offline.
        FakeWorld led = Party();
        RunCore lead = Started(led, counter);
        led.facts[1].engine = false;
        lead.Reconcile(led, counter, 100);
        CHECK(lead.mode == Mode::Hold && lead.reason == "leader_taken_over");
        led.facts[1] = Facts();
        lead.Reconcile(led, counter, 110);
        CHECK(lead.reason == "player_offline");
        // A bot offline is still leader_offline: who played it is what counts.
        FakeWorld bot = Party();
        RunCore botLed = Started(bot, counter);
        bot.facts[1] = Facts();
        botLed.Reconcile(bot, counter, 100);
        CHECK(botLed.reason.find("leader_offline") != std::string::npos);
    }

    // Takeover and hand-back at the control boundary: each a plan revision, so every queued run action is refused; handed
    // back, the member is the engine's again, apart until it walks back to the party.
    void TakeoverAndHandBack()
    {
        FakeWorld world = Party();
        Counter counter;
        RunCore core = Started(world, counter);
        core.Reconcile(world, counter, 100);
        const runrules::Stamp before = core.Stamp();
        core.ControlChanged(world, 3, runplayer::Controller::Person, "takeover");
        CHECK(!runrules::Same(core.Stamp(), before) && core.controllers[3] == runplayer::Controller::Person);
        CHECK(!runrules::Accepts(core.mode, core.Stamp(), before, false));
        CHECK(HasEvent(core, "run_plan", "what=control member=m3 to=person why=takeover"));
        // Played by the person, near the leader: with the party, no hold, never planned.
        world.facts[3].engine = false;
        core.Reconcile(world, counter, 110);
        CHECK(core.mode == Mode::Lead && core.apart.empty() && world.planCalls[3] == 0);
        // A clean logout hands it back: offline a moment as the engine's, then the engine logs it in at the entrance.
        core.ControlChanged(world, 3, runplayer::Controller::Engine, "logout");
        world.facts[3] = Facts();
        core.Reconcile(world, counter, 120);
        CHECK(core.mode == Mode::Lead && core.controllers[3] == runplayer::Controller::Engine);
        CHECK(core.apart.count(3) && !core.apart[3].human && core.apart[3].missing);
        world.facts[3] = Inside();
        world.places[3] = { 300.0f, 0.0f, 0.0f };
        core.Reconcile(world, counter, 125);
        CHECK(core.apart.count(3) && core.apart[3].walking && HasEvent(core, "run_rejoin", "member=m3 result=back"));
    }

    // Readiness of a person: never supplies, waited for while eating and, as a healer, out of the tank's sight; the bots'
    // reasons alone block, and the budget is frozen while only a person keeps the pull back.
    void PersonReadiness()
    {
        runrules::Ready person;
        person.guid = 4;
        person.facts = Inside();
        person.facts.engine = false;
        person.person = true;
        person.usesMana = true;
        person.mana = 30;
        person.hasDrink = false;                  // a bot would block on supplies; a person drinks or goes for more
        std::vector<runrules::Kept> kept = runrules::Readiness({ person });
        CHECK(kept.size() == 1 && kept[0].why == runrules::Keep::Mana && kept[0].person);
        CHECK(!runrules::BlocksAtOnce(kept));
        // Eating at full mana still keeps the pull back; a healer out of the tank's sight too.
        person.mana = 100;
        person.eating = true;
        person.healer = true;
        person.seesTank = false;
        kept = runrules::Readiness({ person });
        CHECK(kept.size() == 2 && kept[0].why == runrules::Keep::Eating && kept[1].why == runrules::Keep::Sight);
        person.eating = false;
        person.seesTank = true;
        person.tankDistance = 41.0f;
        CHECK(runrules::Readiness({ person }).size() == 1);
        person.tankDistance = 30.0f;
        CHECK(runrules::Readiness({ person }).empty());
        // A bot short of drink beside it: mixed, and only the bot's reason is blocked on.
        runrules::Ready bot;
        bot.guid = 2;
        bot.facts = Inside();
        bot.usesMana = true;
        bot.mana = 30;
        bot.hasDrink = false;
        person.mana = 30;
        person.hasDrink = false;
        kept = runrules::Readiness({ bot, person });
        CHECK(runplayer::WaitOf(kept) == runplayer::Wait::Mixed);
        const std::vector<runrules::Kept> bots = runplayer::BotReasons(kept);
        CHECK(bots.size() == 1 && bots[0].guid == 2 && bots[0].why == runrules::Keep::Supplies);
        CHECK(runplayer::WaitOf(runrules::Readiness({ person })) == runplayer::Wait::Person);
        // The budget frozen through a person-only wait past what B's three blocks would take: no block, ever.
        runrules::Budget budget;
        runrules::TrackBudget(budget, 3, 7, true, 1000);
        int64_t personSince = 0;
        int blocks = 0;
        for (int64_t now = 1000; now <= 1000 + 3 * runrules::READY_BUDGET + 600; now += 30)
        {
            const std::vector<runrules::Kept> waiting = runrules::Readiness({ person });
            runplayer::FreezeBudget(budget, personSince, runplayer::WaitOf(waiting) == runplayer::Wait::Person, now);
            if (runplayer::WaitOf(waiting) != runplayer::Wait::Person
                && runrules::BlockDue(runplayer::Frozen(budget, personSince, now), now, runplayer::BotReasons(waiting), true))
                ++blocks;
        }
        CHECK(blocks == 0 && personSince == 1000);
        // The person ready at last: the budget goes on from where it was, all its time still to come.
        runplayer::FreezeBudget(budget, personSince, false, 2800);
        CHECK(personSince == 0 && budget.since == 2800 && !runrules::BudgetOut(budget, 2800 + runrules::READY_BUDGET - 1));
        // A recovery's obligations are never a person's: eating and sight are met by the person as it is.
        runrules::Obligation eating{ 4, runrules::Keep::Eating, 5000 };
        person.facts = Inside();
        person.eating = false;
        CHECK(runrules::Met(eating, person));
    }

    void PersonLines()
    {
        std::vector<runrules::Kept> kept = { { 2, runrules::Keep::Mana, false }, { 4, runrules::Keep::Mana, true },
            { 4, runrules::Keep::Far, true }, { 4, runrules::Keep::Away, true } };
        const std::string line = runplayer::WaitLine(kept, [](uint32_t guid) { return "m" + std::to_string(guid); });
        CHECK(line == "Waiting for m4 mana, need 60%; m4 to come to me");
        CHECK(runplayer::WaitLine({ { 2, runrules::Keep::Mana, false } }, [](uint32_t) { return std::string("x"); }).empty());
        CHECK(runplayer::MaySay(line, "", 0, 100) && !runplayer::MaySay(line, line, 100, 159) && runplayer::MaySay(line, line, 100, 160));
        CHECK(!runplayer::MaySay("", "", 0, 100));
        // A stop is kept like a failure, with its outcome, until acknowledged.
        Failures kept2;
        kept2.Record(1, 9, "player_gone", runplayer::Outcome::Stopped);
        std::string why;
        runplayer::Outcome outcome = runplayer::Outcome::Failed;
        CHECK(kept2.Find(1, 9, why, &outcome) && why == "player_gone" && outcome == runplayer::Outcome::Stopped);
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
    // Item 4a's fix: a member the movement guard keeps from its place by a creature that moves is guarded, not far.
    void GuardedReadiness()
    {
        using runrules::Keep;
        runrules::Ready refused = ReadyMember(3);
        refused.distance = 20.0f;
        refused.guarded = true;
        std::vector<runrules::Kept> kept = runrules::Readiness({ ReadyMember(1), refused });
        CHECK(kept.size() == 1 && kept[0].why == Keep::Guarded && std::string(runrules::KeepName(kept[0].why)) == "guarded");
        // Guarded alone: waited for, the budget frozen, no block, ever, while the allowance lasts.
        CHECK(runplayer::WaitOf(kept) == runplayer::Wait::Guarded);
        CHECK(runplayer::BotReasons(kept).empty());
        runrules::Budget budget;
        runrules::TrackBudget(budget, 4, 9, true, 1000);
        runplayer::GuardedClock clock;
        runplayer::GuardedFreeze(clock, budget, true, 4, 9, 1000);
        std::vector<runrules::Kept> bots;
        CHECK(!runplayer::BlockDueNow(budget, 0, clock, kept, true, 1000 + 3 * runrules::READY_BUDGET, bots) && bots.empty());
        // Guarded beside a far member: blocked on far only, and no obligation is guarded.
        runrules::Ready far = ReadyMember(4);
        far.distance = 30.0f;
        kept = runrules::Readiness({ ReadyMember(1), refused, far });
        CHECK(runplayer::WaitOf(kept) == runplayer::Wait::Bots);
        runrules::Budget running;
        runrules::TrackBudget(running, 4, 9, true, 1000);
        CHECK(runplayer::BlockDueNow(running, 0, runplayer::GuardedClock(), kept, true, 1000 + runrules::READY_BUDGET, bots));
        CHECK(bots.size() == 1 && bots[0].guid == 4 && bots[0].why == Keep::Far);
        CHECK(runrules::Obligations(kept, 0).size() == 1 && runrules::Obligations(kept, 0)[0].why == Keep::Far);
        // A person's wait beside a guarded member is the guarded one's, on its clock; a person alone has the person's.
        runrules::Ready person = ReadyMember(5);
        person.person = true;
        person.health = 50;
        CHECK(runplayer::WaitOf(runrules::Readiness({ refused, person })) == runplayer::Wait::Guarded);
        CHECK(runplayer::WaitOf(runrules::Readiness({ person })) == runplayer::Wait::Person);
        // Spent, the member is far again, an ordinary keeper beside the person's: mixed, and the budget runs.
        refused.guarded = false;
        CHECK(runplayer::WaitOf(runrules::Readiness({ refused, person })) == runplayer::Wait::Mixed);
    }

    // The guarded allowance: 120 seconds per stop and generation, over every stretch, and the budget frozen by as much.
    void GuardedClockAccounting()
    {
        runrules::Budget budget;
        runrules::TrackBudget(budget, 4, 9, true, 1000);
        runplayer::GuardedClock clock;
        CHECK(runplayer::GuardedLeft(clock, 4, 9, 1000));
        // 50 seconds guarded, then 30 with no guarded member, then guarded again.
        runplayer::GuardedFreeze(clock, budget, true, 4, 9, 1000);
        CHECK(runplayer::GuardedUsed(clock, 4, 9, 1030) == 30);
        // A block decision mid-stretch sees the budget frozen so far, with no keepers' write.
        CHECK(runplayer::Frozen(budget, 0, clock, 1030).since == 1030);
        runplayer::GuardedFreeze(clock, budget, false, 4, 9, 1050);
        CHECK(clock.used == 50 && !clock.since && budget.since == 1050);
        CHECK(runplayer::GuardedUsed(clock, 4, 9, 1080) == 50);
        runplayer::GuardedFreeze(clock, budget, true, 4, 9, 1080);
        // 70 more seconds are left: at 1150 the allowance is spent, whatever the keepers last said.
        CHECK(runplayer::GuardedLeft(clock, 4, 9, 1149));
        CHECK(!runplayer::GuardedLeft(clock, 4, 9, 1150));
        // The freeze a block sees is capped by the allowance, though the stretch is not yet closed.
        CHECK(runplayer::Frozen(budget, 0, clock, 1300).since == 1050 + 70);
        runplayer::GuardedFreeze(clock, budget, false, 4, 9, 1300);
        CHECK(clock.used == runplayer::GUARDED_WAIT_SECONDS && budget.since == 1120);
        // Spent: a new guarded-only wait freezes nothing more.
        runplayer::GuardedFreeze(clock, budget, true, 4, 9, 1310);
        CHECK(!clock.since && runplayer::Frozen(budget, 0, clock, 1400).since == 1120);
        // A new generation starts a new allowance, as does another stop.
        CHECK(runplayer::GuardedLeft(clock, 4, 10, 1400) && runplayer::GuardedLeft(clock, 5, 9, 1400));
        runrules::TrackBudget(budget, 4, 10, true, 1400);
        runplayer::GuardedFreeze(clock, budget, true, 4, 10, 1400);
        CHECK(clock.generation == 10 && clock.used == 0 && clock.since == 1400);
        // A person's freeze is apart: it ends when the person is ready, and leaves the guarded clock alone.
        int64_t personSince = 0;
        runplayer::FreezeBudget(budget, personSince, true, 1400);
        CHECK(personSince == 1400 && clock.used == 0);
    }

    // The leader must reach its own place: within 3 yards to gather, 6 to pull. Kept short of it by the guard, it is
    // guarded and then far, so its refused budget always comes to a block (Codex r24, point 3).
    void LeaderNear()
    {
        using runrules::Keep;
        // Refused 5 yards from its spot on the way: kept, guarded while a creature that moves refuses it.
        runrules::Ready leader = ReadyMember(1);
        leader.distance = 5.0f;
        leader.near = runrules::LeaderNear(false);
        leader.guarded = true;
        std::vector<runrules::Kept> kept = runrules::Readiness({ leader });
        CHECK(kept.size() == 1 && kept[0].why == Keep::Guarded);
        // Refused 8 yards off once gathered: kept, since it pulls from 6.
        leader.distance = 8.0f;
        leader.near = runrules::LeaderNear(true);
        leader.guarded = false;
        kept = runrules::Readiness({ leader });
        CHECK(kept.size() == 1 && kept[0].why == Keep::Far);
        // 5 yards off once gathered is near enough to pull.
        leader.distance = 5.0f;
        CHECK(runrules::Readiness({ leader }).empty());
        // As any member, 8 yards was not far, and a refused leader's budget would have run out with nobody kept.
        leader.distance = 8.0f;
        leader.near = runrules::READY_NEAR;
        CHECK(runrules::Readiness({ leader }).empty());
        // Refused 5 yards off on the way by a creature standing still: the refusal starts the budget, a later gather keeps
        // it, and it runs out into a block naming the leader far.
        runrules::Budget budget;
        runrules::TrackBudget(budget, 4, 9, true, 2000);
        runrules::TrackBudget(budget, 4, 9, true, 2100);
        CHECK(budget.since == 2000);
        leader.distance = 5.0f;
        leader.near = runrules::LeaderNear(false);
        std::vector<runrules::Kept> bots;
        CHECK(!runplayer::BlockDueNow(budget, 0, runplayer::GuardedClock(), runrules::Readiness({ leader }), false,
            2000 + runrules::READY_BUDGET - 1, bots));
        CHECK(runplayer::BlockDueNow(budget, 0, runplayer::GuardedClock(), runrules::Readiness({ leader }), false,
            2000 + runrules::READY_BUDGET, bots));
        CHECK(bots.size() == 1 && bots[0].guid == 1 && bots[0].why == Keep::Far);
        // Its far obligation is met by its own place, as near as the obligation's member asks.
        runrules::Obligation obligation{ 1, Keep::Far, 5000 };
        leader.distance = 2.0f;
        CHECK(runrules::Met(obligation, leader));
        leader.distance = 4.0f;
        CHECK(!runrules::Met(obligation, leader));
    }

    // Choose by the run's order: the first living planned stop at or after the current one, wherever the leader stands.
    void NextLivingByProgress()
    {
        // 150 stops; stop 75 (index 74) is dead, 76 lives, and so does every later one. A leader standing by stop 132 still
        // goes to 76: in the shipped build it chose 132 by its place.
        std::vector<bool> alive(150, true), planned(150, true);
        alive[74] = false;
        auto takes = [&](size_t k) { return planned[k] && alive[k]; };
        CHECK(runrules::NextLiving(74, 150, takes) == 75);
        // A stop passed over is passed again.
        planned[75] = false;
        CHECK(runrules::NextLiving(74, 150, takes) == 76);
        // Nothing living after: the route's end.
        for (size_t k = 74; k < 150; ++k)
            alive[k] = false;
        CHECK(runrules::NextLiving(74, 150, takes) == 150);
        // The current stop living: it stays.
        alive[74] = true;
        CHECK(runrules::NextLiving(74, 150, takes) == 74);
    }

    // The leader's adapter in simulated seconds, as GuildRunStates' readiness, keepers and block decisions use the pure
    // rules: member 3 refused at its stand, every 5 seconds, by a patrol that never goes. Each stop and generation the
    // budget starts at the gather; the guarded allowance freezes it for 120 seconds; member 3 is far after that, the budget
    // runs out and the stop blocks; the recovery's far obligation passes unmet, back to lead with a new generation; the
    // third block at the stop fails the run (Codex r24, point 6).
    void GuardedThroughThreeBlocks()
    {
        FakeWorld world = Party();
        Counter counter;
        RunCore core = Started(world, counter);
        core.Reconcile(world, counter, 99);
        runrules::Ready apart = ReadyMember(3);
        apart.distance = 30.0f;
        world.ready[3] = apart;
        const size_t stop = 4;
        const uint32_t pack = 41;
        const runrules::Vec stand = { 10, 0, 0 };
        runrules::Budget budget;
        int64_t personSince = 0;
        runplayer::GuardedClock clock;
        std::string keepers;
        runguard::Refusal refusal;
        uint32_t blockId = 0;
        std::vector<int64_t> blockAt;
        bool guardedBlocked = false, frozenTooLong = false;
        for (int64_t now = 100; now < 3000 && core.mode != Mode::Ended; ++now)
        {
            core.Reconcile(world, counter, now);
            if (core.mode != Mode::Lead || core.recovering)
                continue;
            const uint32_t generation = core.generation;
            if (now % 5 == 0)
                runguard::Note(refusal, 0, &stand, 3600054, now, runguard::PlanKey(generation, 0), stop, true);
            // The leader at its spot: the budget of this stop and generation starts at the gather.
            runrules::TrackBudget(budget, stop, generation, true, now);
            // Readiness, as GuildRunStates::Readiness fills it.
            runrules::Ready m3 = apart;
            m3.guarded = runplayer::GuardedLeft(clock, stop, generation, now)
                && runguard::GuardedNow(refusal, generation, stop, now);
            const std::vector<runrules::Kept> kept = runrules::Readiness({ ReadyMember(1), ReadyMember(2), m3, ReadyMember(4),
                ReadyMember(5) });
            // The keepers' write, only when what it says changes.
            const std::string text = runrules::KeepText(kept, [](uint32_t guid) { return "m" + std::to_string(guid); });
            if (text != keepers)
            {
                const runplayer::Wait wait = runplayer::WaitOf(kept);
                runplayer::FreezeBudget(budget, personSince, wait == runplayer::Wait::Person, now);
                runplayer::GuardedFreeze(clock, budget, wait == runplayer::Wait::Guarded, stop, generation, now);
                keepers = text;
            }
            frozenTooLong = frozenTooLong || runplayer::GuardedUsed(clock, stop, generation, now) > runplayer::GUARDED_WAIT_SECONDS;
            // The trigger, and the action again on the run as it is: the same predicate.
            std::vector<runrules::Kept> bots;
            if (!runplayer::BlockDueNow(budget, personSince, clock, kept, true, now, bots))
                continue;
            for (const runrules::Kept& k : bots)
                guardedBlocked = guardedBlocked || k.why == runrules::Keep::Guarded;
            core.TakeBlock(world, counter, ++blockId, generation, pack, bots,
                runrules::KeepText(bots, [](uint32_t guid) { return "m" + std::to_string(guid); }), now);
            blockAt.push_back(now);
            keepers.clear();
        }
        CHECK(!guardedBlocked && !frozenTooLong);
        CHECK(blockAt.size() == 3);
        // Gather at 100, frozen to 220, out at 520; the recovery over at 700, a new budget and allowance: 1120; then 1720.
        CHECK(blockAt.size() == 3 && blockAt[0] == 520 && blockAt[1] == 1120 && blockAt[2] == 1720);
        CHECK(core.mode == Mode::Ended && core.failed && core.reason == "blocked,m3:far");
        CHECK(HasEvent(core, "run_block", "result=fail stop=41 count=3/3"));
        CHECK(HasEvent(core, "run_recovery", "result=over obligations=m3:far:unmet"));
    }

    // The keepers' write and the block as the leader's adapter makes them, in simulated seconds: `keep` gives readiness at
    // a time from the clock as it stands; `due` decides whether the keepers' write runs (KeepersDue, or the old rule of a
    // change of text alone). Returns the first second a block is due, with its reasons, or 0 by `until`.
    template<class Keep, class Due>
    int64_t FirstBlock(int64_t from, int64_t until, size_t stop, uint32_t generation, runrules::Budget& budget,
        runplayer::GuardedClock& clock, std::string& keepers, Keep keep, Due due, std::vector<runrules::Kept>& bots)
    {
        int64_t personSince = 0;
        for (int64_t now = from; now <= until; ++now)
        {
            const std::vector<runrules::Kept> kept = keep(now);
            const std::string text = runrules::KeepText(kept, [](uint32_t guid) { return "m" + std::to_string(guid); });
            if (due(text, kept))
            {
                const runplayer::Wait wait = runplayer::WaitOf(kept);
                runplayer::FreezeBudget(budget, personSince, wait == runplayer::Wait::Person, now);
                runplayer::GuardedFreeze(clock, budget, wait == runplayer::Wait::Guarded, stop, generation, now);
                keepers = text;
            }
            if (runplayer::BlockDueNow(budget, personSince, clock, kept, false, now, bots))
                return now;
        }
        return 0;
    }

    // A leader the guard refuses 20 yards short of its spot on the way, its four members following within 8 yards of it:
    // measured from the leader, the members keep nothing back, so the leader alone is guarded, the budget it started is
    // frozen for the allowance, and the block at 420 seconds names the leader far. Measured from the spot, the members
    // were far, the budget ran at once and the block named them, 120 seconds early.
    void RefusedLeaderOnTheWay()
    {
        using runrules::Keep;
        const size_t stop = 6;
        const uint32_t generation = 3;
        CHECK(runrules::MeasuredFromLeader(true, false) && !runrules::MeasuredFromLeader(true, true)
            && !runrules::MeasuredFromLeader(false, false));
        auto party = [&](const runplayer::GuardedClock& clock, int64_t now, bool fromLeader)
        {
            std::vector<runrules::Ready> members;
            runrules::Ready leader = ReadyMember(1);
            leader.distance = 20.0f;
            leader.near = runrules::LeaderNear(false);
            // Refused afresh every few seconds, by a creature that moves: guarded while the allowance lasts.
            leader.guarded = runplayer::GuardedLeft(clock, stop, generation, now);
            members.push_back(leader);
            for (uint32_t guid = 2; guid <= 5; ++guid)
            {
                runrules::Ready member = ReadyMember(guid);
                const float fromIt = 3.0f + float(guid);   // 5 to 8 yards behind the leader
                member.distance = runrules::MeasuredFromLeader(fromLeader, false) ? fromIt : 20.0f + fromIt;
                members.push_back(member);
            }
            return runrules::Readiness(members);
        };
        // The refusal starts the budget at 1000 (GuildRunStates::LeaderRefused).
        {
            runrules::Budget budget;
            runrules::TrackBudget(budget, stop, generation, true, 1000);
            runplayer::GuardedClock clock;
            std::string keepers;
            const std::vector<runrules::Kept> first = party(clock, 1000, true);
            CHECK(first.size() == 1 && first[0].guid == 1 && first[0].why == Keep::Guarded);
            CHECK(runplayer::WaitOf(first) == runplayer::Wait::Guarded);
            std::vector<runrules::Kept> bots;
            const int64_t at = FirstBlock(1000, 2000, stop, generation, budget, clock, keepers,
                [&](int64_t now) { return party(clock, now, true); },
                [&](const std::string& text, const std::vector<runrules::Kept>& kept)
                { return runplayer::KeepersDue(text, keepers, kept, clock, stop, generation); }, bots);
            CHECK(at == 1000 + runplayer::GUARDED_WAIT_SECONDS + runrules::READY_BUDGET);
            CHECK(bots.size() == 1 && bots[0].guid == 1 && bots[0].why == Keep::Far);
        }
        // As the members were measured before: far from the spot, an ordinary wait, a block at 300 naming them only.
        {
            runrules::Budget budget;
            runrules::TrackBudget(budget, stop, generation, true, 1000);
            runplayer::GuardedClock clock;
            std::string keepers;
            CHECK(runplayer::WaitOf(party(clock, 1000, false)) == runplayer::Wait::Bots);
            std::vector<runrules::Kept> bots;
            const int64_t at = FirstBlock(1000, 2000, stop, generation, budget, clock, keepers,
                [&](int64_t now) { return party(clock, now, false); },
                [&](const std::string& text, const std::vector<runrules::Kept>& kept)
                { return runplayer::KeepersDue(text, keepers, kept, clock, stop, generation); }, bots);
            CHECK(at == 1000 + runrules::READY_BUDGET);
            CHECK(bots.size() == 4 && std::none_of(bots.begin(), bots.end(), [](const runrules::Kept& k) { return k.guid == 1; }));
        }
    }

    // The same keepers, "m3:guarded", carried from one generation into the next: the clock must be keyed afresh, or its
    // allowance is never spent and the guarded wait has no end. KeepersDue keys it though the text is the same, and
    // GuildRunStates::Reset forgets the keepers at the new stop or generation besides (ForgetKeepers).
    void GuardedKeepersCarriedOver()
    {
        using runrules::Keep;
        const size_t stop = 4;
        // Generation 9: member 3 guarded from 1000, the clock keyed to it, the keepers said.
        runrules::Budget budget;
        runrules::TrackBudget(budget, stop, 9, true, 1000);
        runplayer::GuardedClock clock;
        runplayer::GuardedFreeze(clock, budget, true, stop, 9, 1000);
        std::string keepers = "m3:guarded";
        // A hold, generation 10, Choose back to the same stop; the party gathers again at 2000.
        const uint32_t generation = 10;
        runrules::TrackBudget(budget, stop, generation, true, 2000);
        const runrules::Budget fresh = budget;
        const runplayer::GuardedClock carried = clock;
        auto keep = [&](const runplayer::GuardedClock& c, int64_t now)
        {
            runrules::Ready refused = ReadyMember(3);
            refused.distance = 20.0f;
            refused.guarded = runplayer::GuardedLeft(c, stop, generation, now);
            return runrules::Readiness({ ReadyMember(1), ReadyMember(2), refused });
        };
        CHECK(runrules::KeepText(keep(clock, 2000), [](uint32_t guid) { return "m" + std::to_string(guid); }) == keepers);
        // The old rule, a change of text alone: the write never runs, the clock stays the old generation's, and no block
        // comes in an hour.
        {
            std::vector<runrules::Kept> bots;
            const int64_t at = FirstBlock(2000, 5600, stop, generation, budget, clock, keepers,
                [&](int64_t now) { return keep(clock, now); },
                [&](const std::string& text, const std::vector<runrules::Kept>&) { return text != keepers; }, bots);
            CHECK(at == 0 && clock.generation == 9);
        }
        // KeepersDue: the write runs at 2000 and keys the clock; the allowance is spent at 2120, and the block comes at 2420.
        {
            budget = fresh;
            clock = carried;
            keepers = "m3:guarded";
            CHECK(runplayer::KeepersDue(keepers, keepers, keep(clock, 2000), clock, stop, generation));
            CHECK(!runplayer::KeepersDue(keepers, keepers, runrules::Readiness({ ReadyMember(1) }), clock, stop, generation));
            std::vector<runrules::Kept> bots;
            const int64_t at = FirstBlock(2000, 5600, stop, generation, budget, clock, keepers,
                [&](int64_t now) { return keep(clock, now); },
                [&](const std::string& text, const std::vector<runrules::Kept>& kept)
                { return runplayer::KeepersDue(text, keepers, kept, clock, stop, generation); }, bots);
            CHECK(at == 2000 + runplayer::GUARDED_WAIT_SECONDS + runrules::READY_BUDGET);
            CHECK(clock.generation == generation && bots.size() == 1 && bots[0].guid == 3 && bots[0].why == Keep::Far);
        }
        // Reset's ForgetKeepers: the keepers are forgotten, so the old rule alone keys the clock too, and the block is due
        // at the same second.
        {
            budget = fresh;
            clock = carried;
            keepers = "m3:guarded";
            int64_t personSince = 0;
            runplayer::ForgetKeepers(keepers, budget, personSince, clock, 2000);
            CHECK(keepers.empty() && !clock.since);
            // The old generation's stretch is closed on its clock and moves no budget of another generation.
            CHECK(clock.used == runplayer::GUARDED_WAIT_SECONDS && budget.since == 2000);
            std::vector<runrules::Kept> bots;
            const int64_t at = FirstBlock(2000, 5600, stop, generation, budget, clock, keepers,
                [&](int64_t now) { return keep(clock, now); },
                [&](const std::string& text, const std::vector<runrules::Kept>&) { return text != keepers; }, bots);
            CHECK(at == 2000 + runplayer::GUARDED_WAIT_SECONDS + runrules::READY_BUDGET);
        }
        // Within one stop and generation, ForgetKeepers closes a person's wait and a guarded stretch as their ends would.
        {
            runrules::Budget same;
            runrules::TrackBudget(same, 2, 5, true, 100);
            runplayer::GuardedClock c;
            runplayer::GuardedFreeze(c, same, true, 2, 5, 150);
            int64_t personSince = 120;
            std::string said = "m3:guarded";
            runplayer::ForgetKeepers(said, same, personSince, c, 190);
            CHECK(said.empty() && !personSince && !c.since && c.used == 40);
            CHECK(same.since == 100 + 70 + 40);
        }
    }


    // Item 4b-1: a wait at the stop before has no timer. The Wait write starts the budget (GuildRunStates::Wait, TrackBudget);
    // each blocker still reaching keeps the pull back as the leader's (runrules::BlockerKeepers); the wait goes on only
    // when none reaches, and otherwise comes to a block when the budget runs out, never to a pull.
    void WaitHasNoTimer()
    {
        using runrules::Keep;
        using runrules::WaitStep;
        const size_t stop = 8;
        const uint32_t generation = 4;
        CHECK(runrules::WaitNext(true, false) == WaitStep::Go && runrules::WaitNext(true, true) == WaitStep::Go);
        CHECK(runrules::WaitNext(false, false) == WaitStep::Wait && runrules::WaitNext(false, true) == WaitStep::Block);
        const std::vector<runrules::Kept> blockers = runrules::BlockerKeepers(1, { 3600052 });
        CHECK(blockers.size() == 1 && blockers[0].guid == 1 && blockers[0].why == Keep::Blocker && blockers[0].spawn == 3600052
            && !blockers[0].person);
        CHECK(runrules::KeepText(blockers, [](uint32_t guid) { return "m" + std::to_string(guid); }) == "blocker:3600052");
        CHECK(std::string(runrules::KeepName(Keep::Blocker)) == "blocker");
        // A blocker is a bot's reason: the budget runs, a block may name it, and waiting never meets it at once.
        CHECK(runplayer::WaitOf(blockers) == runplayer::Wait::Bots && runplayer::BotReasons(blockers).size() == 1);
        CHECK(!runrules::BlocksAtOnce(blockers));

        // A blocker that never leaves, the party otherwise ready at the waiting spot. The budget starts at the Wait write,
        // at 1000; from then on, at every second for an hour, the wait either waits or blocks, and never goes on.
        {
            runrules::Budget budget;
            runrules::TrackBudget(budget, stop, generation, true, 1000);
            runplayer::GuardedClock clock;
            int64_t firstBlock = 0;
            bool went = false;
            for (int64_t now = 1000; now <= 4600; ++now)
            {
                std::vector<runrules::Kept> bots;
                const bool due = runplayer::BlockDueNow(budget, 0, clock, blockers, false, now, bots);
                const WaitStep step = runrules::WaitNext(false, due);
                went = went || step == WaitStep::Go;
                if (step == WaitStep::Block && !firstBlock)
                {
                    firstBlock = now;
                    CHECK(bots.size() == 1 && bots[0].why == Keep::Blocker && bots[0].spawn == 3600052);
                }
            }
            CHECK(!went && firstBlock == 1000 + runrules::READY_BUDGET);
        }
        // One that leaves at 1100: the wait goes on then, and nothing blocks before.
        {
            runrules::Budget budget;
            runrules::TrackBudget(budget, stop, generation, true, 1000);
            runplayer::GuardedClock clock;
            int64_t wentAt = 0;
            bool blocked = false;
            for (int64_t now = 1000; now <= 1400 && !wentAt; ++now)
            {
                const bool clear = now >= 1100;
                const std::vector<runrules::Kept> kept = clear ? std::vector<runrules::Kept>() : blockers;
                std::vector<runrules::Kept> bots;
                const WaitStep step = runrules::WaitNext(clear, runplayer::BlockDueNow(budget, 0, clock, kept, false, now, bots));
                blocked = blocked || step == WaitStep::Block;
                if (step == WaitStep::Go)
                    wentAt = now;
            }
            CHECK(wentAt == 1100 && !blocked);
        }
        // Beside a guarded member: the guarded clock does not freeze the budget, since a blocker is a bot's reason, and the
        // block names the blocker alone.
        {
            runrules::Budget budget;
            runrules::TrackBudget(budget, stop, generation, true, 1000);
            runplayer::GuardedClock clock;
            std::string keepers;
            std::vector<runrules::Kept> kept = blockers;
            kept.push_back({ 3, Keep::Guarded, false });
            CHECK(runplayer::WaitOf(kept) == runplayer::Wait::Bots);
            std::vector<runrules::Kept> bots;
            const int64_t at = FirstBlock(1000, 2000, stop, generation, budget, clock, keepers,
                [&](int64_t) { return kept; },
                [&](const std::string& text, const std::vector<runrules::Kept>& now)
                { return runplayer::KeepersDue(text, keepers, now, clock, stop, generation); }, bots);
            CHECK(at == 1000 + runrules::READY_BUDGET);
            CHECK(bots.size() == 1 && bots[0].why == Keep::Blocker);
        }
        // A wait begun after a gather keeps the gather's budget; one at a new stop or generation starts its own.
        {
            runrules::Budget budget;
            runrules::TrackBudget(budget, stop, generation, true, 900);
            runrules::TrackBudget(budget, stop, generation, true, 1000);
            CHECK(budget.since == 900);
            runrules::TrackBudget(budget, stop, generation + 1, true, 1000);
            CHECK(budget.since == 1000 && budget.generation == generation + 1);
        }
    }

    // A block on a blocker is counted, and its recovery is met only when the blocker no longer reaches the stop's places
    // (RunWorld::BlockerGone), whatever the leader's own state; one that never leaves runs out at RECOVERY_SECONDS; the
    // third at one stop fails the run.
    void BlockerBlocksAndRecovers()
    {
        using runrules::Keep;
        const std::vector<runrules::Kept> reasons = runrules::BlockerKeepers(1, { 3600052 });
        // Its obligation: three minutes, its creature kept.
        {
            const std::vector<runrules::Obligation> owed = runrules::Obligations(reasons, 100);
            CHECK(owed.size() == 1 && owed[0].why == Keep::Blocker && owed[0].spawn == 3600052
                && owed[0].deadline == 100 + runrules::RECOVERY_SECONDS);
            runrules::Ready leader = ReadyMember(1);
            CHECK(!runrules::Met(owed[0], leader));
            CHECK(runrules::RecoveryStep(owed, { leader }, 150) == runrules::Recovery::Going);
            CHECK(runrules::RecoveryStep(owed, { leader }, 150, { 3600099 }) == runrules::Recovery::Going);
            CHECK(runrules::RecoveryStep(owed, { leader }, 150, { 3600052 }) == runrules::Recovery::Done);
            // Gone with the leader not seen: met, since it is the creature's.
            CHECK(runrules::RecoveryStep(owed, {}, 150, { 3600052 }) == runrules::Recovery::Done);
            CHECK(runrules::RecoveryStep(owed, { leader }, 100 + runrules::RECOVERY_SECONDS) == runrules::Recovery::Over);
        }
        // Through the core: counted, recovered once the blocker is gone.
        {
            FakeWorld world = Party();
            Counter counter;
            RunCore core = Started(world, counter);
            core.Reconcile(world, counter, 100);
            core.TakeBlock(world, counter, 1, core.generation, 71, reasons, "blocker:3600052", 100);
            CHECK(core.recovering && core.blockCounts[71] == 1);
            CHECK(HasEvent(core, "run_block", "result=recover stop=71 count=1/3 reasons=blocker:3600052"));
            core.Reconcile(world, counter, 150);
            CHECK(core.recovering && core.mode == Mode::Released);
            world.blockersGone.insert(3600052);
            core.Reconcile(world, counter, 160);
            CHECK(core.mode == Mode::Lead && core.reason == "recovered" && !core.recovering && core.choose);
            CHECK(HasEvent(core, "run_recovery", "result=done obligations=blocker:3600052:met"));
        }
        // One that never leaves: over at its deadline, and the third block at the stop fails the run.
        {
            FakeWorld world = Party();
            Counter counter;
            RunCore core = Started(world, counter);
            core.Reconcile(world, counter, 100);
            time_t now = 100;
            for (uint32_t id = 1; id <= 2; ++id)
            {
                core.TakeBlock(world, counter, id, core.generation, 71, reasons, "blocker:3600052", now);
                CHECK(core.recovering && core.blockCounts[71] == id);
                now += runrules::RECOVERY_SECONDS - 1;
                core.Reconcile(world, counter, now);
                CHECK(core.recovering);
                ++now;
                core.Reconcile(world, counter, now);
                CHECK(core.mode == Mode::Lead && core.reason == "recovery_over");
                CHECK(HasEvent(core, "run_recovery", "result=over obligations=blocker:3600052:unmet"));
            }
            core.TakeBlock(world, counter, 3, core.generation, 71, reasons, "blocker:3600052", now);
            CHECK(core.mode == Mode::Ended && core.failed && core.reason == "blocked,blocker:3600052");
            CHECK(HasEvent(core, "run_block", "result=fail stop=71 count=3/3 reasons=blocker:3600052"));
        }
    }

    // Tactics (GuildRunTactics.h): fixtures by what creatures can do, never by entry.
    using runtactics::Cc;
    using runtactics::Foe;
    using runtactics::Member;
    using runtactics::Type;

    Foe MakeFoe(uint32_t guid, Type type, bool elite, uint32_t health = 100)
    {
        Foe f;
        f.guid = guid;
        f.type = type;
        f.elite = elite;
        f.health = health;
        return f;
    }

    // A warrior tank, a priest, a mage, a rogue and a hunter, as the lab's parties are.
    std::vector<Member> Party5()
    {
        std::vector<Member> party(5);
        for (uint32_t i = 0; i < 5; ++i)
            party[i].guid = i + 1;
        party[0].interrupt = true;                  // Shield Bash
        party[1].cc = { Cc::Shackle };
        party[2].cc = { Cc::Polymorph };
        party[2].snare = true;                      // Frost Nova
        party[3].cc = { Cc::Sap };
        party[3].interrupt = party[3].snare = true; // Kick, Gouge
        party[4].snare = true;                      // Concussive Shot
        return party;
    }

    bool HasCc(const runtactics::Plan& plan, uint32_t member, uint32_t foe, Cc cc)
    {
        return std::any_of(plan.cc.begin(), plan.cc.end(), [&](const runtactics::Assignment& a)
            { return a.member == member && a.foe == foe && a.cc == cc; });
    }

    void TacticsOrderAndCc()
    {
        // A foundry-like pack: two summoners and two that flee, humanoid elites, before the pull.
        std::vector<Foe> pack = { MakeFoe(11, Type::Humanoid, true), MakeFoe(12, Type::Humanoid, true),
            MakeFoe(13, Type::Humanoid, true, 90), MakeFoe(14, Type::Humanoid, true) };
        pack[2].summons = pack[3].summons = true;
        pack[0].flees = pack[1].flees = true;
        runtactics::Plan plan = runtactics::Decide(pack, Party5());
        CHECK(plan.skull == 13 && plan.cross == 14 && plan.tank == 13);
        // Crowd control on the two that are last in the order, by whoever can: the mage's Polymorph, the rogue's Sap.
        CHECK(plan.cc.size() == 2 && HasCc(plan, 3, 12, Cc::Polymorph) && HasCc(plan, 4, 11, Cc::Sap));
        CHECK(plan.order == (std::vector<uint32_t>{ 13, 14 }));
        // In a fight Sap no longer fits: only the Polymorph.
        for (Foe& f : pack)
            f.inFight = true;
        plan = runtactics::Decide(pack, Party5());
        CHECK(plan.cc.size() == 1 && HasCc(plan, 3, 12, Cc::Polymorph));
        // The same facts on other creatures, beasts this time: the same order, and Polymorph still fits.
        std::vector<Foe> beasts = pack;
        for (Foe& f : beasts)
        {
            f.guid += 100;
            f.type = Type::Beast;
        }
        plan = runtactics::Decide(beasts, Party5());
        CHECK(plan.skull == 113 && plan.cross == 114 && HasCc(plan, 3, 112, Cc::Polymorph));
        // An immune creature is skipped for that crowd control.
        pack[1].immune = 1u << uint8_t(Cc::Polymorph);
        plan = runtactics::Decide(pack, Party5());
        CHECK(!HasCc(plan, 3, 12, Cc::Polymorph) && HasCc(plan, 3, 11, Cc::Polymorph));
    }

    void TacticsUndeadAndSmall()
    {
        // A gate-like pack of undead: the priest's Shackle, no Polymorph and no Sap.
        std::vector<Foe> pack = { MakeFoe(21, Type::Undead, true), MakeFoe(22, Type::Undead, true, 60),
            MakeFoe(23, Type::Undead, false), MakeFoe(24, Type::Undead, false, 80), MakeFoe(25, Type::Undead, false, 50) };
        runtactics::Plan plan = runtactics::Decide(pack, Party5());
        CHECK(plan.skull == 25 && plan.cross == 22);
        CHECK(plan.cc.size() == 1 && plan.cc[0].member == 2 && plan.cc[0].cc == Cc::Shackle);
        CHECK(plan.cc[0].foe != plan.skull && plan.cc[0].foe != plan.cross);
        // Two non-elites: no crowd control at all.
        std::vector<Foe> small = { MakeFoe(31, Type::Humanoid, false), MakeFoe(32, Type::Humanoid, false) };
        CHECK(runtactics::Decide(small, Party5()).cc.empty());
        CHECK(runtactics::BigPull({ MakeFoe(41, Type::Beast, true), MakeFoe(42, Type::Beast, true) }));
        // A person's character is never assigned anything.
        std::vector<Member> party = Party5();
        party[2].engine = false;
        party[1].engine = false;
        plan = runtactics::Decide(pack, party);
        CHECK(plan.cc.empty());
    }

    void TacticsInterruptsAndFlee()
    {
        std::vector<Foe> pack = { MakeFoe(51, Type::Humanoid, true), MakeFoe(52, Type::Humanoid, true, 24),
            MakeFoe(53, Type::Humanoid, true, 30) };
        pack[0].summons = pack[0].casting = pack[0].interruptible = true;
        pack[1].flees = pack[2].flees = true;
        pack[1].inFight = pack[2].inFight = pack[0].inFight = true;
        runtactics::Plan plan = runtactics::Decide(pack, Party5());
        // The summon is interrupted, by a free member that has one.
        CHECK(plan.interrupts.size() == 1 && plan.interrupts[0].foe == 51);
        // The one at 24% is snared before it runs; the one at 30% is not.
        CHECK(plan.snares.size() == 1 && plan.snares[0].foe == 52);
        // An uninterruptible cast gets no interrupt.
        pack[0].interruptible = false;
        CHECK(runtactics::Decide(pack, Party5()).interrupts.empty());
        // One on the healer is the tank's target, ahead of its own skull.
        pack[2].onHealer = true;
        plan = runtactics::Decide(pack, Party5());
        CHECK(plan.skull == 51 && plan.tank == 53);
    }

    void TacticsStale()
    {
        std::vector<Foe> pack = { MakeFoe(61, Type::Humanoid, true), MakeFoe(62, Type::Humanoid, true),
            MakeFoe(63, Type::Humanoid, true) };
        pack[0].summons = true;
        runtactics::Plan plan = runtactics::Decide(pack, Party5());
        const std::vector<uint32_t> planned = { 61, 62, 63 };
        CHECK(!runtactics::Stale(plan, pack, planned, false));
        // A summon joins: stale.
        std::vector<Foe> withGolem = pack;
        withGolem.push_back(MakeFoe(64, Type::Mechanical, true));
        CHECK(runtactics::Stale(plan, withGolem, planned, false));
        // The skull dies: stale.
        std::vector<Foe> skullDead = { pack[1], pack[2] };
        CHECK(runtactics::Stale(plan, skullDead, planned, false));
        // A crowd control failed, or a new cast to interrupt: stale.
        CHECK(runtactics::Stale(plan, pack, planned, true));
        std::vector<Foe> casting = pack;
        casting[0].casting = casting[0].interruptible = true;
        CHECK(runtactics::Stale(plan, casting, planned, false));
        // A failed crowd control is retried twice, then let go.
        CHECK(runtactics::KeepAssignment(false, 1) && !runtactics::KeepAssignment(false, 2) && runtactics::KeepAssignment(true, 5));
    }

    // Interrupts as the adapter uses them (item 5, first experiment): only a member whose interrupt reaches the caster, the
    // members in the order given, a cast nobody can take not deciding again on every look, and a retry for it.
    void TacticsInterruptReach()
    {
        std::vector<Foe> pack = { MakeFoe(71, Type::Humanoid, true), MakeFoe(72, Type::Humanoid, true) };
        pack[0].summons = pack[0].casting = pack[0].interruptible = true;
        for (Foe& f : pack)
            f.inFight = true;
        std::vector<Member> party = Party5();
        // The rogue first, as the adapter asks those that deal damage first; out of its Kick's reach, the warrior's.
        std::swap(party[0], party[3]);
        party[0].beyond = { 71 };
        runtactics::Plan plan = runtactics::Decide(pack, party);
        CHECK(plan.interrupts.size() == 1 && plan.interrupts[0].member == 1 && plan.interrupts[0].foe == 71);
        CHECK(runtactics::InterrupterOf(plan, 71) == 1 && runtactics::InterruptFor(plan, 1) == 71 && runtactics::InterruptFor(plan, 4) == 0);
        // Nobody reaches it: no pair, and still not stale on the next look, so the plan is not decided on every tick.
        party[3].beyond = { 71 };
        plan = runtactics::Decide(pack, party);
        plan.at = 1000;
        const std::vector<uint32_t> planned = { 71, 72 };
        CHECK(plan.interrupts.empty() && plan.casts == (std::vector<uint32_t>{ 71 }));
        CHECK(!runtactics::Stale(plan, pack, planned, false));
        // Due again only after the retry time, while the cast goes on.
        CHECK(!runtactics::Due(plan, pack, planned, 1000 + runtactics::RETRY_MS - 1));
        CHECK(runtactics::Due(plan, pack, planned, 1000 + runtactics::RETRY_MS));
        std::vector<Foe> stopped = pack;
        stopped[0].casting = false;
        CHECK(!runtactics::Due(plan, stopped, planned, 1000 + runtactics::RETRY_MS));
        // A person's character is never asked, even with the only interrupt in reach.
        party = Party5();
        for (Member& m : party)
            if (m.guid != 1)
                m.interrupt = false;
        party[0].engine = false;
        CHECK(runtactics::Decide(pack, party).interrupts.empty());
    }

    void TacticsSpentAndEnded()
    {
        std::vector<Foe> pack = { MakeFoe(81, Type::Humanoid, true), MakeFoe(82, Type::Humanoid, true) };
        pack[0].summons = pack[0].casting = pack[0].interruptible = true;
        runtactics::Plan plan = runtactics::Decide(pack, Party5());
        plan.at = 0;
        const std::vector<uint32_t> planned = { 81, 82 };
        const uint32_t first = runtactics::InterrupterOf(plan, 81);
        CHECK(first != 0 && !runtactics::Due(plan, pack, planned, 10000));
        // The interrupt tried and the cast goes on (a dodge): the pair is spent, and the retry asks the others.
        runtactics::Spent(plan, first, 81);
        CHECK(runtactics::InterrupterOf(plan, 81) == 0 && runtactics::Due(plan, pack, planned, 10000));
        std::vector<Member> party = Party5();
        for (Member& m : party)
            if (m.guid == first)
                m.interrupt = false;      // its interrupt is on cooldown now
        runtactics::Plan again = runtactics::Decide(pack, party);
        CHECK(runtactics::InterrupterOf(again, 81) != 0 && runtactics::InterrupterOf(again, 81) != first);
        // The cast ended: its next cast is a new one, and the plan is stale for it.
        runtactics::Ended(again, 81);
        CHECK(again.casts.empty() && again.interrupts.empty() && runtactics::Stale(again, pack, planned, false));
        std::vector<Foe> quiet = pack;
        quiet[0].casting = false;
        CHECK(!runtactics::Stale(again, quiet, planned, false));
    }

    void TacticsNoLoops()
    {
        // About to flee with nobody to snare it (no snares in the first experiment): stale once, then not again.
        std::vector<Foe> pack = { MakeFoe(91, Type::Humanoid, true, 20), MakeFoe(92, Type::Humanoid, true) };
        pack[0].flees = true;
        std::vector<Member> party = Party5();
        for (Member& m : party)
            m.snare = false;
        const std::vector<uint32_t> planned = { 91, 92 };
        runtactics::Plan plan = runtactics::Decide({ MakeFoe(91, Type::Humanoid, true), pack[1] }, party);
        CHECK(runtactics::Stale(plan, pack, planned, false));
        plan = runtactics::Decide(pack, party);
        CHECK(plan.snares.empty() && plan.fleeing == (std::vector<uint32_t>{ 91 }) && !runtactics::Stale(plan, pack, planned, false));
        // A creature newly on the healer: stale, and the tank's target; still on the healer after, not stale again. A
        // healer of their own stays the skull, ahead of it.
        std::vector<Foe> onHealer = pack;
        onHealer[1].onHealer = true;
        CHECK(runtactics::Stale(plan, onHealer, planned, false));
        plan = runtactics::Decide(onHealer, party);
        CHECK(plan.tank == 92 && plan.skull == 92 && !runtactics::Stale(plan, onHealer, planned, false));
        onHealer[0].heals = true;
        plan = runtactics::Decide(onHealer, party);
        CHECK(plan.tank == 92 && plan.skull == 91 && !runtactics::Stale(plan, onHealer, planned, false));
        // Two on the healer: the tank takes the first of the order, and the other does not make it stale on every look.
        std::vector<Foe> both = onHealer;
        both[0].onHealer = true;
        plan = runtactics::Decide(both, party);
        CHECK(plan.tank == 91 && !runtactics::Stale(plan, both, planned, false));
    }

    void TacticsSpellKinds()
    {
        namespace fx = runtactics::effect;
        // From effects alone: summons of creatures and totems, heals direct and over time; not a critter, an object or
        // an item, nor a damaging spell.
        CHECK(runtactics::EffectKind(fx::SUMMON_GUARDIAN, 0) == runtactics::KIND_SUMMON);
        CHECK(runtactics::EffectKind(fx::SUMMON, 0) == runtactics::KIND_SUMMON && runtactics::EffectKind(fx::SUMMON_WILD, 0) == runtactics::KIND_SUMMON);
        CHECK(runtactics::EffectKind(88, 0) == runtactics::KIND_SUMMON && runtactics::EffectKind(fx::SUMMON_DEMON, 0) == runtactics::KIND_SUMMON);
        CHECK(runtactics::EffectKind(fx::HEAL, 0) == runtactics::KIND_HEAL && runtactics::EffectKind(fx::HEAL_MAX_HEALTH, 0) == runtactics::KIND_HEAL);
        CHECK(runtactics::EffectKind(fx::APPLY_AURA, fx::AURA_PERIODIC_HEAL) == runtactics::KIND_HEAL);
        CHECK(runtactics::EffectKind(fx::APPLY_AURA, 3) == runtactics::KIND_NONE);   // periodic damage
        CHECK(runtactics::EffectKind(97, 0) == runtactics::KIND_NONE);               // a critter
        CHECK(runtactics::EffectKind(76, 0) == runtactics::KIND_NONE);               // an object
        CHECK(runtactics::EffectKind(2, 0) == runtactics::KIND_NONE);                // school damage
        CHECK(runtactics::Triggers(fx::TRIGGER_SPELL) && runtactics::Triggers(fx::TRIGGER_MISSILE) && !runtactics::Triggers(fx::SUMMON));
        // The same rules on two creatures that do the same things, whatever they are: the same order.
        std::vector<Foe> a = { MakeFoe(101, Type::Humanoid, true), MakeFoe(102, Type::Humanoid, true, 50) };
        a[0].heals = true;
        std::vector<Foe> b = { MakeFoe(201, Type::Elemental, false), MakeFoe(202, Type::Dragonkin, false, 50) };
        b[0].heals = true;
        CHECK(runtactics::KillOrder(a) == (std::vector<uint32_t>{ 101, 102 }) && runtactics::KillOrder(b) == (std::vector<uint32_t>{ 201, 202 }));
    }

    void TacticsOutcomesAndStamps()
    {
        CHECK(runtactics::Judge(true, false) == runtactics::Outcome::Interrupted);
        CHECK(runtactics::Judge(true, true) == runtactics::Outcome::Missed);
        CHECK(runtactics::Judge(false, true) == runtactics::Outcome::NotCast && runtactics::Judge(false, false) == runtactics::Outcome::NotCast);
        CHECK(std::string(runtactics::OutcomeName(runtactics::Outcome::Interrupted)) == "interrupted");
        // A plan acts only under the run's stamp now and the action's: an older revision or generation does nothing.
        CHECK(runtactics::PlanCurrent(7, 40, 7, 40, 7, 40));
        CHECK(!runtactics::PlanCurrent(7, 39, 7, 40, 7, 40) && !runtactics::PlanCurrent(6, 40, 7, 40, 7, 40));
        CHECK(!runtactics::PlanCurrent(7, 40, 7, 40, 7, 39) && !runtactics::PlanCurrent(0, 0, 0, 0, 0, 0));
        CHECK(runtactics::RoleRank(runtactics::Role::Dps) < runtactics::RoleRank(runtactics::Role::Tank)
            && runtactics::RoleRank(runtactics::Role::Tank) < runtactics::RoleRank(runtactics::Role::Healer));
    }

    // The tank's pick (runtactics::TankPick): the first of the kill order on the healer; else the last pick while it is
    // still in the order on the tank or a member; else the first on a member who is not a tank, the skull left out; else
    // none; never a creature left out of the order.
    void TacticsTankPick()
    {
        using runtactics::Victim;
        using Victims = std::vector<std::pair<uint32_t, Victim>>;
        const std::vector<uint32_t> order = { 1, 2, 3 };
        // No skull and no last pick.
        // The healer first, whatever the order.
        CHECK(runtactics::TankPick(order, Victims{ { 2, Victim::Member }, { 3, Victim::Healer } }, 0, 0) == 3);
        // One on the tank is not picked; the one on a member is.
        CHECK(runtactics::TankPick(order, Victims{ { 1, Victim::Tank }, { 2, Victim::Member } }, 0, 0) == 2);
        // Two on members: the kill order first.
        CHECK(runtactics::TankPick(order, Victims{ { 3, Victim::Member }, { 2, Victim::Member } }, 0, 0) == 2);
        // Every creature on the tank, or on none: nothing.
        CHECK(runtactics::TankPick(order, Victims{ { 1, Victim::Tank }, { 2, Victim::Tank }, { 3, Victim::Tank } }, 0, 0) == 0);
        CHECK(runtactics::TankPick(order, Victims{ { 1, Victim::None }, { 2, Victim::None }, { 3, Victim::None } }, 0, 0) == 0);
        CHECK(runtactics::TankPick(order, Victims{}, 0, 0) == 0);
        // A creature on the healer left out of the order, crowd-controlled, is never picked; the next on a member is.
        CHECK(runtactics::TankPick({ 1, 3 }, Victims{ { 2, Victim::Healer }, { 3, Victim::Member } }, 0, 0) == 3);
        CHECK(runtactics::TankPick({ 1, 3 }, Victims{ { 2, Victim::Healer } }, 0, 0) == 0);
        // An empty order: nothing, whoever is on whom.
        CHECK(runtactics::TankPick({}, Victims{ { 1, Victim::Healer }, { 2, Victim::Member } }, 0, 0) == 0);
        // A victim for a creature not in the order, and a creature in the order with no victim: no effect.
        CHECK(runtactics::TankPick(order, Victims{ { 9, Victim::Healer }, { 2, Victim::Member } }, 0, 0) == 2);
        CHECK(runtactics::TankPick(order, Victims{ { 9, Victim::Healer } }, 0, 0) == 0);

        // The skull, first in the order (a summoner), is never picked off a member: the tank goes to it through the mark.
        // The summoner on a member from range and a golem on a member: the golem.
        CHECK(runtactics::TankPick(order, Victims{ { 1, Victim::Member }, { 2, Victim::Member } }, 1, 0) == 2);
        CHECK(runtactics::TankPick(order, Victims{ { 1, Victim::Member }, { 3, Victim::Member }, { 2, Victim::Tank } }, 1, 0) == 3);
        // The summoner alone on a member: nothing, the mark.
        CHECK(runtactics::TankPick(order, Victims{ { 1, Victim::Member }, { 2, Victim::Tank } }, 1, 0) == 0);
        // The summoner on the healer: the summoner, whatever is on a member.
        CHECK(runtactics::TankPick(order, Victims{ { 1, Victim::Healer }, { 2, Victim::Member } }, 1, 0) == 1);
        // The skull on a member is skipped only as the skull: not first in the order, the same.
        CHECK(runtactics::TankPick(order, Victims{ { 2, Victim::Member }, { 3, Victim::Member } }, 2, 0) == 3);

        // The last pick is kept while it lives in the order on the tank or a member.
        // On the tank now, another on a member: the last, kept.
        CHECK(runtactics::TankPick(order, Victims{ { 2, Victim::Member }, { 3, Victim::Tank } }, 1, 3) == 3);
        // On the tank now, alone: kept, not let go back to the mark.
        CHECK(runtactics::TankPick(order, Victims{ { 1, Victim::Member }, { 3, Victim::Tank } }, 1, 3) == 3);
        // Still on a member: kept over one before it in the order.
        CHECK(runtactics::TankPick(order, Victims{ { 2, Victim::Member }, { 3, Victim::Member } }, 1, 3) == 3);
        // On the tank, with another on the healer: the healer's.
        CHECK(runtactics::TankPick(order, Victims{ { 2, Victim::Healer }, { 3, Victim::Tank } }, 1, 3) == 2);
        // On nobody now (crowd-controlled, or let go): dropped, the next on a member.
        CHECK(runtactics::TankPick(order, Victims{ { 2, Victim::Member }, { 3, Victim::None } }, 1, 3) == 2);
        // Dead or gone (no victim given), or left out of the order: dropped.
        CHECK(runtactics::TankPick(order, Victims{ { 2, Victim::Member } }, 1, 3) == 2);
        CHECK(runtactics::TankPick({ 1, 2 }, Victims{ { 2, Victim::Member }, { 3, Victim::Tank } }, 1, 3) == 2);
        CHECK(runtactics::TankPick({ 1, 2 }, Victims{ { 3, Victim::Tank } }, 1, 3) == 0);
        // The skull as the last pick, taken off the healer, is not kept once on the tank: the mark has it.
        CHECK(runtactics::TankPick(order, Victims{ { 1, Victim::Tank }, { 2, Victim::Tank } }, 1, 1) == 0);
        CHECK(runtactics::TankPick(order, Victims{ { 1, Victim::Tank }, { 2, Victim::Member } }, 1, 1) == 2);

        // Decide's own tank target on a pack with one on the healer: the same creature.
        std::vector<Foe> pack = { MakeFoe(51, Type::Humanoid, true), MakeFoe(52, Type::Humanoid, true, 24),
            MakeFoe(53, Type::Humanoid, true, 30) };
        pack[0].summons = true;
        pack[1].flees = pack[2].flees = true;
        pack[0].inFight = pack[1].inFight = pack[2].inFight = true;
        pack[2].onHealer = true;
        const runtactics::Plan plan = runtactics::Decide(pack, Party5());
        Victims victims;
        for (const Foe& foe : pack)
            victims.push_back({ foe.guid, foe.onHealer ? Victim::Healer : Victim::Tank });
        CHECK(plan.tank == 53 && runtactics::TankPick(plan.order, victims, plan.skull, 0) == plan.tank);
        // The summoner skull there, first in the order, and every creature of the order on a member: the next in the order,
        // not the skull.
        Victims onMembers;
        for (uint32_t foe : plan.order)
            onMembers.push_back({ foe, Victim::Member });
        CHECK(plan.order.size() >= 2 && plan.order[0] == plan.skull && plan.skull == 51
            && runtactics::TankPick(plan.order, onMembers, plan.skull, 0) == plan.order[1]);
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
    PersonControl();
    PersonWaiting();
    PersonOffline();
    TakeoverAndHandBack();
    PersonReadiness();
    PersonLines();
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
    GuardedReadiness();
    GuardedClockAccounting();
    LeaderNear();
    NextLivingByProgress();
    GuardedThroughThreeBlocks();
    RefusedLeaderOnTheWay();
    GuardedKeepersCarriedOver();
    WaitHasNoTimer();
    BlockerBlocksAndRecovers();
    TacticsOrderAndCc();
    TacticsUndeadAndSmall();
    TacticsInterruptsAndFlee();
    TacticsStale();
    TacticsInterruptReach();
    TacticsSpentAndEnded();
    TacticsNoLoops();
    TacticsSpellKinds();
    TacticsOutcomesAndStamps();
    TacticsTankPick();
    printf("%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
