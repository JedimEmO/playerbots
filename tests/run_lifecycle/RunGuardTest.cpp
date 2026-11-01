// Tests of the run's movement guard (playerbot/GuildRunGuard.h): the path test, the reach index, the permits, the refusal
// record and the intercept verdict. Built and run by tests/run_lifecycle/run.sh, without the server.

#include "GuildRunGuard.h"

#include <cstdio>
#include <random>

using namespace runguard;

namespace
{
    int failures = 0, checks = 0;

#define CHECK(cond) do { ++checks; if (!(cond)) { ++failures; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

    GuardThreat Still(uint32_t spawn, Vec at, float reach)
    {
        GuardThreat threat;
        threat.spawn = spawn;
        threat.where = { at };
        threat.reach = reach;
        return threat;
    }

    GuardThreat Moving(uint32_t spawn, Vec at, float reach, float speed)
    {
        GuardThreat threat = Still(spawn, at, reach);
        threat.speed = speed;
        return threat;
    }

    void TestPath()
    {
        // A straight 40-yard path along x, and a creature 6 yards off its middle with a 10-yard reach: both vertices are
        // far outside, the segment between them is not.
        const std::vector<Vec> path = { { 0, 0, 0 }, { 40, 0, 0 } };
        {
            GuardResult r = GuardPath(path, { Still(7, { 20, 6, 0 }, 10.0f) }, 7.0f, false);
            CHECK(r.verdict == Verdict::Refuse);
            CHECK(r.spawn == 7);
            // It enters at x = 20 - 8 = 12.
            CHECK(std::fabs(r.along - 12.0f) <= SAMPLE);
        }
        // The same with a clip asked for: the end at x = 40 is out of reach, so it clips 1 yard short of the entry.
        {
            GuardResult r = GuardPath(path, { Still(7, { 20, 6, 0 }, 10.0f) }, 7.0f, true);
            CHECK(r.verdict == Verdict::Clip);
            CHECK(!r.endInReach);
            CHECK(std::fabs(r.clipAlong - (r.along - CLIP_BACK)) < 0.01f);
            const std::vector<Vec> clipped = Clip(path, r.clipAlong);
            CHECK(clipped.size() == 2 && std::fabs(clipped.back().x - r.clipAlong) < 0.01f);
        }
        // An end in reach is refused even with a clip asked for.
        {
            GuardResult r = GuardPath(path, { Still(7, { 38, 3, 0 }, 10.0f) }, 7.0f, true);
            CHECK(r.verdict == Verdict::Refuse);
            CHECK(r.endInReach);
        }
        // A clip that would gain under 2 yards is refused.
        {
            GuardResult r = GuardPath(path, { Still(7, { 12, 0, 0 }, 10.0f) }, 7.0f, true);
            CHECK(r.verdict == Verdict::Refuse);
        }
        // Out of every reach: allowed.
        {
            GuardResult r = GuardPath(path, { Still(7, { 20, 30, 0 }, 10.0f) }, 7.0f, false);
            CHECK(r.verdict == Verdict::Allow);
        }
        // Leaving a reach the path starts in is allowed; going back in is refused.
        {
            const GuardThreat t = Still(7, { 0, 0, 0 }, 10.0f);
            CHECK(GuardPath({ { 2, 0, 0 }, { 30, 0, 0 } }, { t }, 7.0f, false).verdict == Verdict::Allow);
            CHECK(GuardPath({ { 2, 0, 0 }, { 30, 0, 0 }, { 5, 1, 0 } }, { t }, 7.0f, false).verdict == Verdict::Refuse);
            // Inside and getting nearer: refused at once.
            CHECK(GuardPath({ { 8, 0, 0 }, { 30, 0, 0 } }, { Still(7, { 30, 0, 0 }, 25.0f) }, 7.0f, false).verdict == Verdict::Refuse);
        }
        // A launch that moves less than a yard across is no move: a facing, a fall straight down.
        {
            CHECK(GuardPath({ { 0, 0, 10 }, { 0.3f, 0, 0 } }, { Still(7, { 0, 0, 0 }, 10.0f) }, 7.0f, false).verdict == Verdict::Allow);
        }
        // A patrol's loop: a segment of it crosses the path, though none of its points is near.
        {
            GuardThreat patrol;
            patrol.spawn = 9;
            patrol.where = { { 20, -40, 0 }, { 20, 40, 0 }, { 60, 40, 0 }, { 20, -40, 0 } };
            patrol.reach = 8.0f;
            CHECK(GuardPath(path, { patrol }, 7.0f, false).verdict == Verdict::Refuse);
        }
        // An exempt creature, in the fight or the plan's pull, is passed.
        {
            GuardThreat t = Still(7, { 20, 0, 0 }, 10.0f);
            t.exempt = true;
            CHECK(GuardPath(path, { t }, 7.0f, false).verdict == Verdict::Allow);
        }
        // Heights: a creature 12 yards below the path with a 10-yard reach is passed; 8 yards below is refused.
        {
            CHECK(GuardPath(path, { Still(7, { 20, 0, -12 }, 10.0f) }, 7.0f, false).verdict == Verdict::Allow);
            CHECK(GuardPath(path, { Still(7, { 20, 0, -8 }, 10.0f) }, 7.0f, false).verdict == Verdict::Refuse);
        }
    }

    void TestMoving()
    {
        const std::vector<Vec> path = { { 0, 0, 0 }, { 60, 0, 0 } };
        // A walking patrol 15 yards off the path at x = 10, reach 10: at 10 yards along, 10/7 + 1 seconds of walking at
        // 2.5 yd/s add about 6 yards, so it reaches the path: refused inside the horizon.
        CHECK(GuardPath(path, { Moving(5, { 10, 15, 0 }, 10.0f, 2.5f) }, 7.0f, false).verdict == Verdict::Refuse);
        // Well off the path, it does not.
        CHECK(GuardPath(path, { Moving(5, { 10, 25, 0 }, 10.0f, 2.5f) }, 7.0f, false).verdict == Verdict::Allow);
        // The same creature by the path beyond the horizon, at x = 50: the watcher has it, not the launch.
        CHECK(GuardPath(path, { Moving(5, { 50, 5, 0 }, 10.0f, 2.5f) }, 7.0f, false).verdict == Verdict::Allow);
        // A running creature gets a larger margin than a walking one at the same place.
        CHECK(GuardPath(path, { Moving(5, { 10, 22, 0 }, 10.0f, 2.5f) }, 7.0f, false).verdict == Verdict::Allow);
        CHECK(GuardPath(path, { Moving(5, { 10, 22, 0 }, 10.0f, 7.0f) }, 7.0f, false).verdict == Verdict::Refuse);
        // A creature not loaded counts by its envelope everywhere, beyond the horizon too.
        CHECK(GuardPath(path, { Still(5, { 50, 5, 0 }, 10.0f) }, 7.0f, false).verdict == Verdict::Refuse);
        CHECK(SpreadAt(Moving(5, {}, 10.0f, 2.5f), HORIZON + 1.0f, 7.0f) < 0.0f);
        // A modelled wanderer not drawn off is bound by its envelope: its live margin would reach the path at x = 10, its
        // envelope (spawn 20 yards off the path, reach 10, range 6) does not.
        {
            GuardThreat bound = Moving(5, { 10, 14, 0 }, 10.0f, 7.0f);
            CHECK(GuardPath(path, { bound }, 7.0f, false).verdict == Verdict::Refuse);
            bound.bound = { { 10, 20, 0 } };
            bound.boundReach = 16.0f;
            CHECK(GuardPath(path, { bound }, 7.0f, false).verdict == Verdict::Allow);
            bound.boundReach = 21.0f;
            CHECK(GuardPath(path, { bound }, 7.0f, false).verdict == Verdict::Refuse);
        }
    }

    Envelope Env(uint32_t spawn, Kind kind, std::vector<Vec> where, float reach, float range = 0.0f)
    {
        Envelope e;
        e.spawn = spawn;
        e.kind = kind;
        e.where = std::move(where);
        e.reach = reach;
        e.range = range;
        return e;
    }

    void TestIndex()
    {
        // A wanderer's envelope covers its whole range: a path 25 yards from its spawn with reach 10 and range 20 finds it.
        {
            ReachIndex index({ Env(1, Kind::Wanderer, { { 0, 0, 0 } }, 10.0f, 20.0f) });
            CHECK(index.Candidates({ { -50, 25, 0 }, { 50, 25, 0 } }).size() == 1);
            CHECK(index.Candidates({ { -50, 45, 0 }, { 50, 45, 0 } }).empty());
            CHECK(index.BySpawn(1) && index.BySpawn(1)->EnvelopeReach() == 30.0f);
        }
        // A patrol is found by a path crossing the middle of a long leg of its loop, far from every point of it.
        {
            ReachIndex index({ Env(2, Kind::Patrol, { { 0, -100, 0 }, { 0, 100, 0 }, { 0, -100, 0 } }, 8.0f) });
            CHECK(index.Candidates({ { -30, 0, 0 }, { 30, 0, 0 } }).size() == 1);
        }
        // Against a brute-force pass over random envelopes and paths: the cells never miss an envelope that reaches the path.
        {
            std::mt19937 rng(42);
            std::uniform_real_distribution<float> pos(-200.0f, 200.0f), reach(5.0f, 25.0f), range(0.0f, 15.0f);
            std::vector<Envelope> envelopes;
            for (uint32_t i = 0; i < 300; ++i)
            {
                const Kind kind = i % 3 == 0 ? Kind::Still : i % 3 == 1 ? Kind::Wanderer : Kind::Patrol;
                std::vector<Vec> where = { { pos(rng), pos(rng), 0 } };
                if (kind == Kind::Patrol)
                {
                    where.push_back({ pos(rng), pos(rng), 0 });
                    where.push_back(where.front());
                }
                envelopes.push_back(Env(i + 1, kind, where, reach(rng), kind == Kind::Wanderer ? range(rng) : 0.0f));
            }
            const ReachIndex index(envelopes);
            int missed = 0;
            for (int trial = 0; trial < 200; ++trial)
            {
                std::vector<Vec> path = { { pos(rng), pos(rng), 0 } };
                for (int k = 0; k < 3; ++k)
                    path.push_back({ path.back().x + pos(rng) / 5.0f, path.back().y + pos(rng) / 5.0f, 0 });
                const std::vector<uint32_t> found = index.Candidates(path);
                for (uint32_t i = 0; i < envelopes.size(); ++i)
                {
                    const float d = runrules::LinesDistance(envelopes[i].where, path);
                    if (d <= envelopes[i].EnvelopeReach() && std::find(found.begin(), found.end(), i) == found.end())
                        ++missed;
                }
            }
            CHECK(missed == 0);
        }
    }

    void TestPermits()
    {
        const Vec end = { 10, 0, 0 };
        const Permit heal = HealPermit(1, 2, 77, 5, 9, end, PERMIT_NEAR, 1000);
        CHECK(Honours(heal, 1, { 11, 1, 0 }, 1001));
        CHECK(!Honours(heal, 1, { 14, 0, 0 }, 1001));      // 4 yards off its end
        CHECK(!Honours(heal, 3, end, 1001));               // another bot
        CHECK(!Honours(heal, 1, end, 1010));               // after 10 seconds

        PermitFacts facts;
        facts.at = { 0, 0, 0 };
        facts.patientAlive = true;
        facts.patientHealth = 40;
        facts.fight = 77;
        facts.generation = 5;
        facts.revision = 9;
        facts.now = 1002;
        CHECK(Expired(heal, facts) == PermitEnd::None);
        PermitFacts f = facts;
        f.at = { 9, 0, 0 };
        CHECK(Expired(heal, f) == PermitEnd::Arrived);
        f = facts;
        f.patientHealth = 71;
        CHECK(Expired(heal, f) == PermitEnd::NoNeed);
        f = facts;
        f.patientAlive = false;
        CHECK(Expired(heal, f) == PermitEnd::NoNeed);
        f = facts;
        f.fight = 0;
        CHECK(Expired(heal, f) == PermitEnd::FightOver);
        f = facts;
        f.generation = 6;
        CHECK(Expired(heal, f) == PermitEnd::Invalidated);
        f = facts;
        f.revision = 10;
        CHECK(Expired(heal, f) == PermitEnd::Invalidated);
        f = facts;
        f.now = 1010;
        CHECK(Expired(heal, f) == PermitEnd::TimedOut);

        // A portal's permit is scoped to its trigger, a surface's to its point and the breath timer.
        const Permit portal = PortalPermit(1, 5, 9, { 100, 0, 0 }, 3.0f, 1000);
        CHECK(Honours(portal, 1, { 107, 0, 0 }, 1059));
        CHECK(!Honours(portal, 1, { 109, 0, 0 }, 1001));
        CHECK(!Honours(portal, 1, { 100, 0, 0 }, 1060));
        const Permit surface = SurfacePermit(1, 5, 9, { 0, 0, 20 }, 1000);
        CHECK(Honours(surface, 1, { 0, 0, 19 }, 1001));
        CHECK(!Honours(surface, 1, { 10, 0, 20 }, 1001));
        f = facts;
        f.breathing = true;
        CHECK(Expired(surface, f) == PermitEnd::None);
        f.breathing = false;
        CHECK(Expired(surface, f) == PermitEnd::NoNeed);

        // The heal spots: the patient's place first, then the ring nearest the healer.
        const std::vector<Vec> spots = HealSpots({ 0, 0, 0 }, 40.0f, { 100, 0, 0 });
        CHECK(spots.size() == size_t(HEAL_RING_POINTS + 1));
        CHECK(spots[0].x == 0.0f && spots[0].y == 0.0f);
        CHECK(std::fabs(spots[1].x - 35.0f) < 0.01f && std::fabs(spots[1].y) < 0.01f);
    }

    void TestRefusal()
    {
        Refusal record;
        uint32_t told = 0;
        const Vec dest = { 5, 5, 0 };
        CHECK(RecordStep(record, told, 100) == RecordEvent::None);
        Note(record, 4242, nullptr, 7, 100);
        CHECK(Refuses(record, 4242, nullptr, 104));
        CHECK(!Refuses(record, 4242, nullptr, 105));        // 5 seconds on
        CHECK(!Refuses(record, 4343, nullptr, 101));        // a new target is checked afresh
        CHECK(RecordStep(record, told, 100) == RecordEvent::Start);
        CHECK(RecordStep(record, told, 101) == RecordEvent::None);
        // More refusals in the same stretch tell nothing more.
        Note(record, 0, &dest, 7, 102);
        CHECK(Refuses(record, 0, &dest, 103));
        const Vec near = { 6, 6, 0 };
        CHECK(Refuses(record, 0, &near, 103));
        CHECK(RecordStep(record, told, 103) == RecordEvent::None);
        // A move accepted after it ends it: one start, one end.
        Accepted(record, 104);
        CHECK(RecordStep(record, told, 104) == RecordEvent::End);
        CHECK(RecordStep(record, told, 105) == RecordEvent::None);
        // A refusal under one plan does not refuse under the next.
        {
            Refusal planned;
            Note(planned, 4242, nullptr, 7, 300, PlanKey(5, 9));
            CHECK(Refuses(planned, 4242, nullptr, 301, PlanKey(5, 9)));
            CHECK(!Refuses(planned, 4242, nullptr, 301, PlanKey(5, 10)));
            CHECK(!Refuses(planned, 4242, nullptr, 301, PlanKey(6, 9)));
        }
        // A new stretch starts again; ten seconds without a refusal end it too.
        Note(record, 4242, nullptr, 7, 200);
        CHECK(RecordStep(record, told, 200) == RecordEvent::Start);
        CHECK(RecordStep(record, told, 209) == RecordEvent::None);
        CHECK(RecordStep(record, told, 210) == RecordEvent::End);
    }

    InterceptFacts Blocker()
    {
        InterceptFacts facts;
        facts.blocker = 10;
        facts.at = { 20, 0, 0 };
        facts.spot = { 0, 0, 0 };
        facts.callRadius = 10.0f;
        facts.pack = { 10, 11 };
        facts.companions = { { 11, true, true, { 22, 2, 0 } } };
        facts.pullSet = { 10, 11 };
        return facts;
    }

    void TestIntercept()
    {
        CHECK(Interceptable(Blocker()) == InterceptNo::Yes);
        InterceptFacts f = Blocker();
        f.alive = false;
        CHECK(Interceptable(f) == InterceptNo::Dead);
        f = Blocker();
        f.at = { 31, 0, 0 };
        CHECK(Interceptable(f) == InterceptNo::Far);
        f = Blocker();
        f.inSight = false;
        CHECK(Interceptable(f) == InterceptNo::NoSight);
        f = Blocker();
        f.companions[0].loaded = false;
        CHECK(Interceptable(f) == InterceptNo::CompanionUnknown);
        f = Blocker();
        f.companions[0].at = { 35, 0, 0 };                // a follower out of call radius
        CHECK(Interceptable(f) == InterceptNo::CompanionFar);
        f = Blocker();
        f.companions[0].alive = false;                    // a dead companion is accounted for
        CHECK(Interceptable(f) == InterceptNo::Yes);
        f = Blocker();
        f.pullSet.push_back(99);
        CHECK(Interceptable(f) == InterceptNo::Extras);
        f = Blocker();
        f.late = { { 50, { 60, 0, 0 }, 59 } };
        CHECK(Interceptable(f) == InterceptNo::LateArrival);
        f.late[0].second = 60;
        CHECK(Interceptable(f) == InterceptNo::Yes);
        f = Blocker();
        f.spotPasses = false;
        CHECK(Interceptable(f) == InterceptNo::SpotRefused);
        f = Blocker();
        f.corridorPasses = false;
        CHECK(Interceptable(f) == InterceptNo::CorridorRefused);

        // Certified, then checked at the cast.
        InterceptFacts certified = Blocker();
        certified.late = { { 50, { 80, 0, 0 }, 70 } };
        const InterceptVerdict verdict = Certify(certified, 1, 5, 9, 1000);
        CastFacts cast;
        cast.blockerAt = { 21, 0, 0 };
        cast.companions = { { 11, true, true, { 23, 2, 0 } } };
        cast.late = { { 50, { 79, 0, 0 }, 0 } };
        cast.generation = 5;
        cast.revision = 9;
        cast.now = 1002;
        CHECK(StillValid(verdict, cast) == Stale::None);
        CastFacts c = cast;
        c.now = 1004;
        CHECK(StillValid(verdict, c) == Stale::Old);
        c = cast;
        c.revision = 10;
        CHECK(StillValid(verdict, c) == Stale::Plan);
        c = cast;
        c.blockerAlive = false;
        CHECK(StillValid(verdict, c) == Stale::BlockerDead);
        c = cast;
        c.blockerAt = { 31, 0, 0 };
        CHECK(StillValid(verdict, c) == Stale::BlockerFar);
        c = cast;
        c.inSight = false;
        CHECK(StillValid(verdict, c) == Stale::NoSight);
        c = cast;
        c.companions[0].alive = false;
        CHECK(StillValid(verdict, c) == Stale::CompanionChanged);
        c = cast;
        c.companions.clear();
        CHECK(StillValid(verdict, c) == Stale::CompanionChanged);
        c = cast;
        c.companions[0].at = { 33, 2, 0 };               // 10 yards off its offset
        CHECK(StillValid(verdict, c) == Stale::CompanionMoved);
        c = cast;
        c.late[0].at = { 70, 0, 0 };                     // 10 yards nearer the spot
        CHECK(StillValid(verdict, c) == Stale::LateNearer);
        c = cast;
        c.corridorPasses = false;
        CHECK(StillValid(verdict, c) == Stale::Corridor);
        c = cast;
        c.newCreature = true;
        CHECK(StillValid(verdict, c) == Stale::NewCreature);
    }

    void TestFirstStop()
    {
        const std::vector<Vec> way = { { 0, 0, 0 }, { 10, 0, 0 }, { 20, 0, 0 }, { 30, 0, 0 }, { 40, 0, 0 } };
        Vec out;
        // The arrival point when the guard passes it.
        CHECK(FirstWaitingSpot(way, 4, { -5, 0, 0 }, [](const Vec&) { return true; }, out) == WaitAt::Arrival);
        CHECK(out.x == -5.0f);
        // Else the nearest way point behind the pull spot that passes: here x <= 10.
        CHECK(FirstWaitingSpot(way, 4, { -5, 0, 0 }, [](const Vec& p) { return p.x >= 0.0f && p.x <= 10.0f; }, out) == WaitAt::Way);
        CHECK(out.x == 10.0f);
        // None: the stop blocks at once.
        CHECK(FirstWaitingSpot(way, 4, { -5, 0, 0 }, [](const Vec&) { return false; }, out) == WaitAt::None);
        // Never a point at or past the pull spot's.
        CHECK(FirstWaitingSpot(way, 2, { -5, 0, 0 }, [](const Vec& p) { return p.x >= 20.0f; }, out) == WaitAt::None);

        // A way that passes the pull spot's place twice, out and back: points 1 and 7 stand at x = 10. Read with the run's
        // progress at point 0, the pull spot is the first pass, so nothing past point 0 is behind it; read by nearest point
        // with the second pass nearer, the spot would be point 7 and the party would wait at points it has not reached.
        const std::vector<Vec> twice = { { 0, 0, 0 }, { 10, 0, 0 }, { 20, 0, 0 }, { 30, 0, 0 }, { 30, 10, 0 }, { 20, 10, 0 },
            { 15, 5, 0 }, { 10, 0.5f, 0 } };
        const size_t pull = runrules::PlaceAlong({ 1, 7 }, 7, true, 0);
        CHECK(pull == 1);
        CHECK(FirstWaitingSpot(twice, pull, { -5, 0, 0 }, [](const Vec& p) { return p.x >= 0.0f && p.x < 30.0f; }, out) == WaitAt::Way);
        CHECK(out.x == 0.0f);
    }

    void TestUnmodelled()
    {
        // A summon standing still, as the live scan gives it: judged where it stands, along the whole path, and live.
        const GuardThreat threat = LiveThreat(0, 4417, { 50, 5, 0 }, 8.0f, 0.0f);
        CHECK(threat.speed == 0.0f && threat.spawn == 0 && threat.entry == 4417 && threat.reach == 8.0f && threat.live);
        GuardResult r = GuardPath({ { 0, 0, 0 }, { 60, 0, 0 } }, { threat }, 7.0f, false);
        CHECK(r.verdict == Verdict::Refuse);
        CHECK(ThreatName(threat, -1) == "creature_4417_unmodelled");
        GuardThreat modelled = Still(812, { 0, 0, 0 }, 5.0f);
        CHECK(ThreatName(modelled, 3) == "spawn_812_stop_4");
        // A modelled creature dragged 40 yards off its envelope, next to the line: the index's cells do not find it there,
        // the live scan does, and it refuses the path.
        {
            const Envelope home = Env(812, Kind::Still, { { 30, -45, 0 } }, 10.0f);
            const ReachIndex index({ home });
            const std::vector<Vec> line = { { 0, 0, 0 }, { 60, 0, 0 } };
            CHECK(index.Candidates(line).empty());
            const Vec dragged = { 30, -5, 0 };
            CHECK(OffEnvelope(home, dragged));
            CHECK(!OffEnvelope(home, { 32, -44, 0 }));
            const GuardThreat live = LiveThreat(812, 0, dragged, home.reach, 0.0f);
            GuardResult refused = GuardPath(line, { live }, 7.0f, false);
            CHECK(refused.verdict == Verdict::Refuse && refused.spawn == 812);
            // Moving, it reaches farther across by its walk over the horizon, as SpreadAt has it, and the scan takes it by that.
            const GuardThreat walking = LiveThreat(812, 0, dragged, home.reach, 2.5f);
            CHECK(SpreadAt(walking, 0.0f, 7.0f) == 2.5f * RENEW_SECONDS);
            CHECK(MaxSpread(2.5f, 7.0f) == SpreadAt(walking, HORIZON, 7.0f));
            // A wanderer within its range plus the margin is on its envelope.
            const Envelope wanderer = Env(813, Kind::Wanderer, { { 0, 0, 0 } }, 10.0f, 15.0f);
            CHECK(!OffEnvelope(wanderer, { 19, 0, 0 }));
            CHECK(OffEnvelope(wanderer, { 21, 0, 0 }));
        }
        // In the current fight, whoever started it (item 6: a fight the run did not start is defended all the same);
        // not in it, out of combat or fighting someone else.
        CHECK(InCurrentFight(true, true, false));
        CHECK(InCurrentFight(true, false, true));
        CHECK(!InCurrentFight(true, false, false));
        CHECK(!InCurrentFight(false, true, true));
        // A heal permit reaching to the spell's range: arrived anywhere within it.
        {
            const Permit reach = HealPermit(1, 2, 77, 5, 9, { 0, 0, 0 }, 30.0f, 1000);
            PermitFacts at;
            at.at = { 25, 0, 0 };
            at.patientHealth = 40;
            at.fight = 77;
            at.generation = 5;
            at.revision = 9;
            at.now = 1001;
            CHECK(Expired(reach, at) == PermitEnd::Arrived);
            at.at = { 35, 0, 0 };
            CHECK(Expired(reach, at) == PermitEnd::None);
            CHECK(Honours(reach, 1, { 28, 0, 0 }, 1001));
        }
        // In the current fight: exempt.
        GuardThreat fighting = threat;
        fighting.exempt = true;
        CHECK(GuardPath({ { 0, 0, 0 }, { 60, 0, 0 } }, { fighting }, 7.0f, false).verdict == Verdict::Allow);
    }
    // Item 4a's fix: one reach shared by the route and the guard, for creatures standing still or wandering.
    void TestSharedReach()
    {
        // Random envelopes, live places on them, and random bent paths: wherever StaticRefuses passes a path, the guard
        // passes it too, for the creature loaded anywhere on its envelope with any live reach and speed, or not loaded.
        std::mt19937 rng(7);
        std::uniform_real_distribution<float> pos(-60.0f, 60.0f), reach(5.0f, 25.0f), range(0.0f, 20.0f), unit(0.0f, 1.0f);
        int passed = 0, broken = 0;
        for (int trial = 0; trial < 4000; ++trial)
        {
            const bool wanders = trial % 2 == 1;
            Envelope e = Env(1, wanders ? Kind::Wanderer : Kind::Still, { { pos(rng) / 3.0f, pos(rng) / 3.0f, 0 } }, reach(rng),
                wanders ? range(rng) : 0.0f);
            std::vector<Vec> path = { { pos(rng), pos(rng), 0 } };
            for (int k = 0; k < 3; ++k)
                path.push_back({ path.back().x + pos(rng) / 2.0f, path.back().y + pos(rng) / 2.0f, 0 });
            if (StaticRefuses(e, path))
                continue;
            ++passed;
            // Not loaded: anywhere in its envelope.
            if (GuardPath(path, { EnvelopeThreat(e, nullptr, 0.0f, 0.0f) }, 7.0f, false).verdict != Verdict::Allow)
                ++broken;
            // Loaded, somewhere in its range, with a live reach and a speed that may well reach the path. A live reach
            // beyond the envelope's is a bot below the route's level: the shared reach grown by as much passes it too.
            const float angle = unit(rng) * 6.2831853f, off = unit(rng) * e.range;
            const Vec live = { e.where[0].x + off * std::cos(angle), e.where[0].y + off * std::sin(angle), 0 };
            const float liveReach = reach(rng) * 2.0f;
            const GuardThreat threat = EnvelopeThreat(e, &live, liveReach, unit(rng) * 7.0f);
            Envelope grown = e;
            grown.reach = std::max(e.reach, liveReach);
            if (threat.live || (!StaticRefuses(grown, path) && GuardPath(path, { threat }, 7.0f, false).verdict != Verdict::Allow))
                ++broken;
        }
        CHECK(passed > 500);
        CHECK(broken == 0);

        // A wanderer's point at its reach, margin and range less half a yard is refused by both; half a yard beyond, by neither.
        {
            const Envelope e = Env(2, Kind::Wanderer, { { 0, 0, 0 } }, 10.0f + 3.0f, 15.0f);
            const float edge = 10.0f + 3.0f + 15.0f;
            const std::vector<Vec> in = { { 40, 0, 0 }, { edge - 0.5f, 0, 0 } }, out = { { 40, 0, 0 }, { edge + 0.5f, 0, 0 } };
            CHECK(StaticRefuses(e, in));
            CHECK(GuardPath(in, { EnvelopeThreat(e, nullptr, 0.0f, 0.0f) }, 7.0f, false).verdict == Verdict::Refuse);
            const Vec ranging = { 15, 0, 0 };
            CHECK(GuardPath(in, { EnvelopeThreat(e, &ranging, 13.0f, 2.5f) }, 7.0f, false).verdict == Verdict::Refuse);
            CHECK(!StaticRefuses(e, out));
            CHECK(GuardPath(out, { EnvelopeThreat(e, &ranging, 13.0f, 2.5f) }, 7.0f, false).verdict == Verdict::Allow);
        }

        // A bot 4 levels under the route's level: a creature standing still reaches it 4 yards farther than its envelope,
        // made at the route's level, says. The bound never cuts that: a place 2 yards beyond the envelope's reach, inside
        // the creature's real one, is refused, live and as a point; one beyond the real reach passes.
        {
            const Envelope e = Env(6, Kind::Still, { { 0, 0, 0 } }, 13.0f);
            const Vec at = { 0, 0, 0 };
            const float liveReach = 13.0f + 4.0f;
            const GuardThreat threat = EnvelopeThreat(e, &at, liveReach, 0.0f);
            CHECK(!threat.live && threat.boundReach == liveReach);
            const std::vector<Vec> inside = { { 40, 0, 0 }, { 15, 0, 0 } }, beyond = { { 40, 0, 0 }, { 17.5f, 0, 0 } };
            CHECK(!StaticRefuses(e, inside));
            CHECK(GuardPath(inside, { threat }, 7.0f, false).verdict == Verdict::Refuse);
            CHECK(Reaches(threat, { 15, 0, 0 }, SpreadAt(threat, 0.0f, 7.0f)));
            CHECK(GuardPath(beyond, { threat }, 7.0f, false).verdict == Verdict::Allow);
            // At the route's level, or above it, the bound is the shared reach, as before.
            CHECK(EnvelopeThreat(e, &at, 13.0f, 0.0f).boundReach == 13.0f);
            CHECK(EnvelopeThreat(e, &at, 10.0f, 0.0f).boundReach == 13.0f);
            CHECK(GuardPath(inside, { EnvelopeThreat(e, &at, 13.0f, 0.0f) }, 7.0f, false).verdict == Verdict::Allow);
        }

        // A wanderer with a 50-yard range, its spawn 55 yards from a stand: a 40-yard look around the stand would miss it,
        // the index's cells do not.
        {
            const ReachIndex index({ Env(3, Kind::Wanderer, { { 55, 0, 0 } }, 13.0f, 50.0f), Env(4, Kind::Still, { { 0, 60, 0 } }, 13.0f) });
            auto all = [](const Envelope&) { return true; };
            const std::vector<uint32_t> found = StaticBlockers(index, { { 0, 0, 0 } }, all);
            CHECK(found.size() == 1 && index.Envelopes()[found[0]].spawn == 3);
            // Dead at this point of the run's order: no blocker.
            CHECK(StaticBlockers(index, { { 0, 0, 0 } }, [](const Envelope& e) { return e.spawn != 3; }).empty());
        }

        // A stand path that bends round a creature standing still: both its ends clear, its middle in reach. Refused by the
        // shared test, which is what the route's stand certification now asks.
        {
            const Envelope still = Env(5, Kind::Still, { { 10, 10, 0 } }, 8.0f);
            const std::vector<Vec> bent = { { 0, 0, 0 }, { 8, 5, 0 }, { 20, 0, 0 } };
            CHECK(runrules::LinesDistance({ bent.front() }, still.where) > 8.0f);
            CHECK(runrules::LinesDistance({ bent.back() }, still.where) > 8.0f);
            CHECK(StaticRefuses(still, bent));
            const ReachIndex index({ still });
            CHECK(StaticBlockers(index, bent, [](const Envelope&) { return true; }).size() == 1);
            CHECK(StaticBlockers(index, { bent.front(), { 10, -5, 0 }, bent.back() }, [](const Envelope&) { return true; }).empty());
        }
    }

    // Patrols and followers are judged live: where they stand, with their walk over the horizon, bound by their loop.
    void TestLiveMovers()
    {
        // A patrol's 100-yard loop along x.
        const Envelope patrol = Env(10, Kind::Patrol, { { 0, 0, 0 }, { 100, 0, 0 }, { 0, 0, 0 } }, 13.0f);
        CHECK(StaticReach(patrol) < 0.0f);
        CHECK(!StaticRefuses(patrol, { { 50, 0, 0 } }));
        // Not loaded: no threat at all; a party near it has loaded it.
        CHECK(EnvelopeThreat(patrol, nullptr, 0.0f, 0.0f).where.empty());
        CHECK(GuardPath({ { 50, 30, 0 }, { 50, 0, 0 } }, { EnvelopeThreat(patrol, nullptr, 0.0f, 0.0f) }, 7.0f, false).verdict
            == Verdict::Allow);
        // Loaded at x = 10, walking: a loop point 40 yards ahead of it, reached from the side, passes.
        const Vec at = { 10, 0, 0 };
        const GuardThreat walking = EnvelopeThreat(patrol, &at, 13.0f, 2.5f);
        CHECK(walking.live && walking.bound.size() == patrol.where.size() && walking.boundReach == 13.0f);
        CHECK(GuardPath({ { 50, 15, 0 }, { 50, 0, 0 } }, { walking }, 7.0f, false).verdict == Verdict::Allow);
        // Its live place is refused, and the refusal is live. A walk to it from 35 yards off meets its loop's reach only
        // beyond the horizon, where the watcher looks again; from 15 yards off, inside it.
        CHECK(GuardPath({ { 10, 35, 0 }, { 10, 0, 0 } }, { walking }, 7.0f, false).verdict == Verdict::Allow);
        const GuardResult r = GuardPath({ { 10, 15, 0 }, { 10, 0, 0 } }, { walking }, 7.0f, false);
        CHECK(r.verdict == Verdict::Refuse && r.spawn == 10);
        // A follower of it is judged the same way, bound by its master's loop with its follow distance across: the core
        // keeps it at a fixed angle to its master's facing, so it turns about the master. One spawned 3 across and 4 up
        // the loop's line follows 5 yards off, and may stand anywhere 5 across from the loop.
        const Envelope follower = Env(11, Kind::Follower, { { 0, 0, 0 }, { 100, 0, 0 }, { 0, 0, 0 } }, 13.0f, 5.0f);
        CHECK(StaticReach(follower) < 0.0f && EnvelopeThreat(follower, nullptr, 0.0f, 0.0f).where.empty());
        const Vec followerAt = { 13, 5, 0 };
        CHECK(!OffEnvelope(follower, followerAt) && !OffEnvelope(follower, { 13, -5, 0 }));
        const GuardThreat following = EnvelopeThreat(follower, &followerAt, 13.0f, 2.5f);
        CHECK(following.live && following.boundSpread == 5.0f && following.boundReach == 13.0f);
        CHECK(GuardPath({ { 13, 20, 0 }, { 13, 5, 0 } }, { following }, 7.0f, false).verdict == Verdict::Refuse);
        CHECK(GuardPath({ { 50, 15, 0 }, { 50, 0, 0 } }, { following }, 7.0f, false).verdict == Verdict::Allow);
        // Turned to the loop's other side, it is still on its envelope, and bound, and refuses what it reaches from there;
        // the old envelope, the loop moved by the spawn offset, put it 10 yards off, as though a fight had drawn it.
        const Vec turned = { 13, -5, 0 };
        const GuardThreat turnedThreat = EnvelopeThreat(follower, &turned, 13.0f, 0.0f);
        CHECK(GuardPath({ { 13, -30, 0 }, { 13, -17.5f, 0 } }, { turnedThreat }, 7.0f, false).verdict == Verdict::Refuse);
        const Envelope moved = Env(11, Kind::Follower, { { 0, 5, 0 }, { 100, 5, 0 }, { 0, 5, 0 } }, 13.0f);
        CHECK(PlaceDistance(turned, moved.where, moved.range) > 9.9f);

        // A patrol standing between its loop's points, 4.5 yards off its 100-yard loop: not off its envelope (5), so bound.
        // Its aggro radius 18, ReachPad 3.8: a point 18 yards from its centre the server aggroes from. The bound alone, at
        // the envelope's reach from the loop, took that point (22.5 from the loop, reach 21.8); it grows by the live
        // place's own distance from the loop, so it never cuts what the creature reaches where it stands.
        {
            const float aggro = 18.0f, pad = ReachPad(0.4f, 0.4f);
            const Envelope loop = Env(12, Kind::Patrol, { { 0, 0, 0 }, { 100, 0, 0 }, { 0, 0, 0 } }, aggro + pad);
            const Vec off = { 50, 4.5f, 0 };
            CHECK(!OffEnvelope(loop, off));
            const GuardThreat t = EnvelopeThreat(loop, &off, aggro + pad, 2.5f);
            CHECK(std::fabs(t.boundReach - (aggro + pad + 4.5f)) < 1e-3f);
            const std::vector<Vec> walkIn = { { 50, 40, 0 }, { 50, 22.5f, 0 } };
            CHECK(runrules::Distance(walkIn.back(), off) <= aggro);
            CHECK(ReachMargin(t, walkIn.back(), SpreadAt(t, 0.0f, 7.0f)) <= 0.0f);
            CHECK(GuardPath(walkIn, { t }, 7.0f, false).verdict == Verdict::Refuse);
            // On its loop the bound is the envelope's reach, and the same walk passes outside it.
            const Vec on = { 50, 0, 0 };
            const GuardThreat onLoop = EnvelopeThreat(loop, &on, aggro + pad, 0.0f);
            CHECK(onLoop.boundReach == aggro + pad);
            CHECK(GuardPath(walkIn, { onLoop }, 7.0f, false).verdict == Verdict::Allow);
        }

        // A creature running toward a path from 15 yards beyond its reach: the old scan's 10-yard travel margin left it out,
        // its walk over the horizon at its own speed takes it, and the guard refuses the path where it gets there.
        {
            const float reach = 12.0f;
            const Vec runner = { 10, reach + 15.0f, 0 };
            const std::vector<Vec> path = { { 0, 0, 0 }, { 30, 0, 0 } };
            CHECK(runrules::LinesDistance({ runner }, path) > reach + runrules::TRAVEL_MARGIN);
            CHECK(reach + MaxSpread(7.0f, 7.0f) >= runrules::LinesDistance({ runner }, path));
            CHECK(GuardPath(path, { LiveThreat(0, 4417, runner, reach, 7.0f) }, 7.0f, false).verdict == Verdict::Refuse);
            // Standing, it does not.
            CHECK(GuardPath(path, { LiveThreat(0, 4417, runner, reach, 0.0f) }, 7.0f, false).verdict == Verdict::Allow);
            // Nor walking, which covers less before the bot is by.
            CHECK(GuardPath(path, { LiveThreat(0, 4417, runner, reach, 2.5f) }, 7.0f, false).verdict == Verdict::Allow);
        }
    }

    // The plan's own pull: who may close on the stop's pack, and when (Codex r24, point 5).
    void TestPlanExempt()
    {
        CHECK(!PlanExempt(true, false, false));       // the leader in Approach or Gather
        CHECK(PlanExempt(true, true, false));         // the leader in Pull, before anything fights
        CHECK(!PlanExempt(false, true, false));       // a member in Pull, the pulled creature not fighting the party
        CHECK(PlanExempt(false, true, true));         // a member in Pull, the pulled creature fighting the party
        CHECK(!PlanExempt(false, false, true));       // a member out of Pull

        // The leader's melee fallback chases into the pack: the pull phase is written first, so the chase launches under the
        // exemption; a pull that started nothing goes back to Gather, and the same chase is refused again.
        const std::vector<Vec> chase = { { 0, 0, 0 }, { 24, 0, 0 } };
        auto pack = [](bool exempt)
        {
            GuardThreat a = Still(20, { 25, 0, 0 }, 12.0f), b = Still(21, { 27, 3, 0 }, 12.0f);
            a.exempt = b.exempt = exempt;
            return std::vector<GuardThreat>{ a, b };
        };
        CHECK(GuardPath(chase, pack(PlanExempt(true, true, false)), 7.0f, false).verdict == Verdict::Allow);
        CHECK(GuardPath(chase, pack(PlanExempt(true, false, false)), 7.0f, false).verdict == Verdict::Refuse);
        // A member's move into the pack while the pull is on its way: refused until the pulled creature fights the party.
        CHECK(GuardPath(chase, pack(PlanExempt(false, true, false)), 7.0f, false).verdict == Verdict::Refuse);
        CHECK(GuardPath(chase, pack(PlanExempt(false, true, true)), 7.0f, false).verdict == Verdict::Allow);
        // An extra the pull brings is never the plan's: refused for the leader in Pull too.
        std::vector<GuardThreat> threats = pack(PlanExempt(true, true, false));
        threats.push_back(Still(22, { 15, 6, 0 }, 8.0f));
        CHECK(GuardPath(chase, threats, 7.0f, false).verdict == Verdict::Refuse);
    }

    // The refusal record as readiness, the leader's budget and the triggers' holding read it.
    void TestRefusalUse()
    {
        Refusal record;
        const Vec dest = { 5, 5, 0 };
        const uint64_t plan = PlanKey(5, 9);
        CHECK(!Fresh(record, 5, 3, 100) && !GuardedNow(record, 5, 3, 100) && !Holding(record, plan, 3, 100));
        Note(record, 0, &dest, 3600054, 100, plan, 3, true);
        CHECK(Fresh(record, 5, 3, 105) && GuardedNow(record, 5, 3, 105) && Holding(record, plan, 3, 104));
        // Holding lapses with REFUSAL_SECONDS, guarded with REFUSAL_END_SECONDS.
        CHECK(!Holding(record, plan, 3, 105));
        CHECK(GuardedNow(record, 5, 3, 109) && !GuardedNow(record, 5, 3, 110));
        // Another generation or stop: neither; another revision of the plan: still guarded, but no longer holding.
        CHECK(!GuardedNow(record, 6, 3, 101) && !GuardedNow(record, 5, 4, 101));
        CHECK(!Holding(record, PlanKey(6, 9), 3, 101) && !Holding(record, plan, 4, 101));
        CHECK(GuardedNow(record, 5, 3, 101) && !Holding(record, PlanKey(5, 10), 3, 101));
        // A move accepted after it ends both, at once.
        Accepted(record, 102);
        CHECK(!GuardedNow(record, 5, 3, 102) && !Holding(record, plan, 3, 102) && !Fresh(record, 5, 3, 102));
        // Refused again: back on.
        Note(record, 0, &dest, 3600054, 103, plan, 3, true);
        CHECK(GuardedNow(record, 5, 3, 103) && Holding(record, plan, 3, 103));
        // By a still creature: fresh, for the leader's budget, but never guarded nor holding; it is far.
        Refusal still;
        Note(still, 0, &dest, 812, 200, plan, 3, false);
        CHECK(Fresh(still, 5, 3, 201) && !GuardedNow(still, 5, 3, 201) && !Holding(still, plan, 3, 201));
        // A facing launch moves nothing, and so accepts nothing.
        CHECK(!Moves({ { 0, 0, 0 }, { 0.5f, 0.2f, 3.0f } }) && Moves({ { 0, 0, 0 }, { 1.0f, 0, 0 } }));
    }
    // Item 4a's height fix: one rule of whether a creature reaches a point, for the route, the guard and the evidence. The
    // server's 3D distance between centres, against its aggro radius, both bounding radii and THREAT_MARGIN; a creature's
    // movement spreads its places across only, never up or down.
    void TestAggroRadius()
    {
        // Unit::GetAttackDistance: detection less the bot's level over the creature's, the gap no lower than -25, at least 5.
        CHECK(AggroRadius(18.0f, 50, 20) == 18.0f + 25.0f);   // a gap of -30, held at -25
        CHECK(AggroRadius(18.0f, 20, 19) == 19.0f);           // -1
        CHECK(AggroRadius(18.0f, 20, 20) == 18.0f);           // 0
        CHECK(AggroRadius(18.0f, 20, 25) == 13.0f);           // +5
        CHECK(AggroRadius(8.0f, 20, 25) == 5.0f);             // the 5-yard floor
        CHECK(AggroRadius(0.0f, 20, 20) == 0.0f);             // detects nothing
        CHECK(std::fabs(ReachPad(0.5f, 0.4f) - (0.9f + runrules::THREAT_MARGIN)) < 1e-5f);
    }

    void TestHeight()
    {
        // A foundry floor creature at z 21, the walkway 23 yards above it. Level 19 against a level 19 bot: 18 yards, both
        // radii and the margin, 21.9 yards.
        const float reach = AggroRadius(18.0f, 19, 19) + ReachPad(0.5f, 0.4f);
        const Vec floor = { 0, 0, 21 };
        const Vec above = { 0, 0, 44 };
        CHECK(reach < 23.0f);
        // Directly above: not reached. On the floor 15 yards off: reached.
        CHECK(!CreatureReaches({ above }, { floor }, 0.0f, reach));
        CHECK(CreatureReaches({ { 15, 0, 21 } }, { floor }, 0.0f, reach));
        const std::vector<Vec> walkway = { { -30, 0, 44 }, { 30, 0, 44 } };
        const std::vector<Vec> onFloor = { { -30, 15, 21 }, { 30, 15, 21 } };
        {
            const Envelope still = Env(1, Kind::Still, { floor }, reach);
            CHECK(!StaticRefuses(still, walkway));
            CHECK(StaticRefuses(still, onFloor));
            CHECK(GuardPath(walkway, { EnvelopeThreat(still, nullptr, 0.0f, 0.0f) }, 7.0f, false).verdict == Verdict::Allow);
            CHECK(GuardPath(onFloor, { EnvelopeThreat(still, nullptr, 0.0f, 0.0f) }, 7.0f, false).verdict == Verdict::Refuse);
            CHECK(GuardPath(walkway, { EnvelopeThreat(still, &floor, reach, 0.0f) }, 7.0f, false).verdict == Verdict::Allow);
        }

        // A wanderer's range spreads across only. Range 10 under the walkway: a walkway point 12 yards across is 2 yards
        // past its range and 23 above, 23.1 yards, out of reach; the old sphere of reach and range, 31.9, took it. A floor
        // point 25 yards across is 15 past its range: reached.
        {
            const Envelope wanderer = Env(2, Kind::Wanderer, { floor }, reach, 10.0f);
            const Vec walk12 = { 12, 0, 44 };
            CHECK(std::fabs(PlaceDistance(walk12, wanderer.where, 10.0f) - std::sqrt(4.0f + 529.0f)) < 1e-3f);
            CHECK(runrules::Distance(walk12, floor) <= reach + 10.0f);
            CHECK(!StaticRefuses(wanderer, { walk12 }));
            CHECK(StaticRefuses(wanderer, { { 25, 0, 21 } }));
            CHECK(!StaticRefuses(wanderer, walkway));
            // Not loaded, or loaded anywhere in its range with any walk: the walkway passes, the floor does not.
            CHECK(GuardPath(walkway, { EnvelopeThreat(wanderer, nullptr, 0.0f, 0.0f) }, 7.0f, false).verdict == Verdict::Allow);
            const Vec ranging = { 9, 3, 21 };
            CHECK(GuardPath(walkway, { EnvelopeThreat(wanderer, &ranging, reach, 7.0f) }, 7.0f, false).verdict == Verdict::Allow);
            CHECK(GuardPath(onFloor, { EnvelopeThreat(wanderer, &ranging, reach, 2.5f) }, 7.0f, false).verdict == Verdict::Refuse);
        }

        // A two-point pacer at z 21, its loop 7 yards long, live reach 19.6, walking 2.5 a second, the leader running at 7:
        // a path 22 to 25 yards above its loop passes. Its walk over the horizon, 9.6 yards, spreads across only; the old
        // sphere growth, 22.1 to 29.2 yards, refused it.
        {
            const Envelope pacer = Env(3600052, Kind::Patrol, { { 0, 0, 21 }, { 7, 0, 21 }, { 0, 0, 21 } }, 21.8f);
            const Vec live = { 3, 0, 21 };
            const std::vector<Vec> over = { { -20, 0, 43 }, { 3, 0, 43 }, { 25, 0, 46 } };
            const GuardThreat walking = EnvelopeThreat(pacer, &live, 19.6f, 2.5f);
            CHECK(walking.live && !walking.bound.empty());
            CHECK(19.6f + MaxSpread(2.5f, 7.0f) > 29.0f && 19.6f + SpreadAt(walking, 0.0f, 7.0f) > 22.0f);
            CHECK(GuardPath(over, { walking }, 7.0f, false).verdict == Verdict::Allow);
            // In a fight or drawn off its loop: unbound, but its walk still spreads across only, so the path above passes,
            // and one on its floor 12 yards across, where its walk takes it, does not.
            const GuardThreat drawn = EnvelopeThreat(pacer, &live, 19.6f, 7.0f, true);
            CHECK(drawn.live && drawn.bound.empty());
            CHECK(GuardPath(over, { drawn }, 7.0f, false).verdict == Verdict::Allow);
            CHECK(GuardPath({ { 3, 40, 21 }, { 3, 25, 21 } }, { drawn }, 7.0f, false).verdict == Verdict::Refuse);
            // An unmodelled creature's walk the same: across only.
            CHECK(GuardPath(over, { LiveThreat(0, 4417, live, 19.6f, 7.0f) }, 7.0f, false).verdict == Verdict::Allow);
        }

        // A ramp: a loop climbing from z 19 to z 35, a point 18 yards above the ramp's top across from it. The loop's top
        // reaches it; the patrol at the ramp's foot, its walk across never climbing, does not, and the bound does.
        {
            const Envelope ramp = Env(7, Kind::Patrol, { { 0, 0, 19 }, { 40, 0, 35 }, { 0, 0, 19 } }, 21.8f);
            const Vec point = { 40, 0, 53 };
            CHECK(CreatureReaches({ point }, ramp.where, 0.0f, ramp.reach));
            const Vec foot = { 0, 0, 19 };
            const GuardThreat atFoot = EnvelopeThreat(ramp, &foot, 21.8f, 2.5f);
            CHECK(!Reaches(atFoot, point, SpreadAt(atFoot, 0.0f, 7.0f)));
            const Vec top = { 38, 0, 34.2f };
            const GuardThreat atTop = EnvelopeThreat(ramp, &top, 21.8f, 2.5f);
            CHECK(Reaches(atTop, point, SpreadAt(atTop, 0.0f, 7.0f)));
        }
    }

    // A random envelope of each kind (0 still, 1 wandering, 2 patrolling, 3 following a patrol: its master's loop with its
    // follow distance across), and a place on it.
    Envelope RandomEnvelope(std::mt19937& rng, int kind, float reach)
    {
        std::uniform_real_distribution<float> pos(-40.0f, 40.0f), height(-25.0f, 25.0f), rangeOf(0.0f, 15.0f), unit(0.0f, 1.0f);
        Envelope e;
        e.spawn = 1;
        e.reach = reach;
        if (kind >= 2)
        {
            e.kind = kind == 2 ? Kind::Patrol : Kind::Follower;
            e.range = kind == 2 ? 0.0f : 1.0f + unit(rng) * 7.0f;
            const int points = 2 + int(unit(rng) * 4.0f);
            for (int k = 0; k < points; ++k)
                e.where.push_back({ pos(rng) / 2.0f, pos(rng) / 2.0f, height(rng) / 2.0f });
            e.where.push_back(e.where.front());
        }
        else
        {
            e.kind = kind == 0 ? Kind::Still : Kind::Wanderer;
            e.range = kind == 0 ? 0.0f : rangeOf(rng);
            e.where = { { pos(rng) / 2.0f, pos(rng) / 2.0f, height(rng) / 2.0f } };
        }
        return e;
    }

    Vec RandomOnEnvelope(std::mt19937& rng, const Envelope& e)
    {
        std::uniform_real_distribution<float> unit(0.0f, 1.0f);
        Vec at = e.where[0];
        if (e.where.size() > 1)
        {
            const size_t seg = size_t(unit(rng) * float(e.where.size() - 1)) % (e.where.size() - 1);
            at = Lerp(e.where[seg], e.where[seg + 1], unit(rng));
        }
        // Across its range at that height: a wanderer about its spawn, a follower turned about its master.
        const float angle = unit(rng) * 6.2831853f, off = unit(rng) * e.range;
        return { at.x + off * std::cos(angle), at.y + off * std::sin(angle), at.z };
    }

    // The planner's test passes a path (StaticRefuses for one standing still or wandering, the whole loop with its spread for
    // a patrol or a follower) only when the guard passes it too, for the creature anywhere on its envelope, with any live
    // reach and walk, in 3D.
    void TestPlannerGuardAgree()
    {
        std::mt19937 rng(11);
        std::uniform_real_distribution<float> pos(-40.0f, 40.0f), height(-25.0f, 25.0f), reachOf(5.0f, 25.0f), unit(0.0f, 1.0f);
        int clear[4] = { 0, 0, 0, 0 }, broken = 0, pointsClear = 0;
        for (int trial = 0; trial < 8000; ++trial)
        {
            const int kind = trial % 4;
            const Envelope e = RandomEnvelope(rng, kind, reachOf(rng));
            std::vector<Vec> path = { { pos(rng), pos(rng), height(rng) } };
            for (int k = 0; k < 3; ++k)
                path.push_back({ path.back().x + pos(rng) / 2.0f, path.back().y + pos(rng) / 2.0f, path.back().z + height(rng) / 3.0f });
            // The planner: the shared reach, or for a patrol or follower moversOf's whole loop at each loop point's height,
            // with its spread across.
            const bool plannerClear = kind >= 2 ? !CreatureReaches(path, e.where, e.range, e.reach) : !StaticRefuses(e, path);
            // Somewhere on its envelope: a wanderer across its range at its spawn's height, a patrol on its loop, a follower
            // turned about its master on the loop.
            const Vec live = RandomOnEnvelope(rng, e);
            // A live reach up to the envelope's, a bot at the route's level or above; any walk.
            const float liveReach = e.reach * (0.6f + 0.4f * unit(rng));
            const float speed = unit(rng) * 7.0f;
            const GuardThreat threat = EnvelopeThreat(e, &live, liveReach, speed);
            if (plannerClear)
            {
                ++clear[kind];
                if (GuardPath(path, { threat }, 7.0f, false).verdict != Verdict::Allow)
                    ++broken;
                if (kind < 2 && GuardPath(path, { EnvelopeThreat(e, nullptr, 0.0f, 0.0f) }, 7.0f, false).verdict != Verdict::Allow)
                    ++broken;
            }
            // And a point: a stand or a pull spot the planner clears, which the guard's point test (PointClear) passes.
            const Vec point = path.back();
            const bool pointPlannerClear = kind >= 2 ? !CreatureReaches({ point }, e.where, e.range, e.reach)
                : !StaticRefuses(e, { point });
            if (pointPlannerClear)
            {
                ++pointsClear;
                if (Reaches(threat, point, SpreadAt(threat, 0.0f, 7.0f)))
                    ++broken;
            }
        }
        CHECK(clear[0] > 300 && clear[1] > 300 && clear[2] > 300 && clear[3] > 300);
        CHECK(pointsClear > 2000);
        CHECK(broken == 0);
        if (broken)
            printf("planner and guard disagree %d times\n", broken);
    }

    // The server's truth: a creature aggroes a bot whose centre comes within its aggro radius of its own (UnitAI's
    // DetectOrAttack). A creature not drawn may stand up to OFF_ENVELOPE_MARGIN off its envelope, and its live reach may
    // be above the envelope's (a bot below the route's level). Wherever it stands, GuardPath refuses every path a sample
    // of which comes within its aggro radius of the creature's centre, within the horizon for one that moves, when the
    // path starts outside it (one that starts in it is leaving, and never gets nearer).
    void TestServerTruth()
    {
        std::mt19937 rng(23);
        std::uniform_real_distribution<float> pos(-40.0f, 40.0f), height(-25.0f, 25.0f), reachOf(9.0f, 28.0f), unit(0.0f, 1.0f);
        int reached[4] = { 0, 0, 0, 0 }, walkIns = 0, offCases = 0;
        for (int trial = 0; trial < 12000; ++trial)
        {
            const int kind = trial % 4;
            const Envelope e = RandomEnvelope(rng, kind, reachOf(rng));
            // Off its envelope by up to OFF_ENVELOPE_MARGIN, in any direction, mostly across.
            Vec live = RandomOnEnvelope(rng, e);
            const float angle = unit(rng) * 6.2831853f, off = unit(rng) * OFF_ENVELOPE_MARGIN, up = (unit(rng) - 0.5f) * 0.6f;
            live = { live.x + off * std::cos(angle), live.y + off * std::sin(angle), live.z + off * up };
            if (PlaceDistance(live, e.where, e.range) > 3.5f)
                ++offCases;
            const bool drawn = OffEnvelope(e, live);
            // Its live reach from 0.7 to 1.4 times the envelope's, and its aggro radius that less ReachPad.
            const float pad = ReachPad(0.3f + unit(rng) * 1.2f, 0.3f + unit(rng) * 0.3f);
            const float liveReach = e.reach * (0.7f + 0.7f * unit(rng));
            const float aggro = liveReach - pad;
            if (aggro < 0.5f)
                continue;
            const float speed = kind == 0 ? 0.0f : unit(rng) * 7.0f;
            const GuardThreat threat = EnvelopeThreat(e, &live, liveReach, speed, drawn);
            // A path from somewhere toward somewhere near the creature; or every other one, a walk straight at it from up
            // to 20 yards out that ends just inside its aggro radius, the walk-in only a cut bound lets through.
            std::vector<Vec> path = { { pos(rng), pos(rng), height(rng) } };
            Vec target = { live.x + pos(rng) / 2.0f, live.y + pos(rng) / 2.0f, live.z + height(rng) / 4.0f };
            if (trial % 8 >= 4)
            {
                const float a = unit(rng) * 6.2831853f, tilt = (unit(rng) - 0.5f) * 0.8f;
                const Vec dir = { std::cos(a) * std::cos(tilt), std::sin(a) * std::cos(tilt), std::sin(tilt) };
                const float in = aggro * (0.9f + 0.1f * unit(rng)), out = aggro + 2.0f + unit(rng) * 16.0f;
                target = { live.x + dir.x * in, live.y + dir.y * in, live.z + dir.z * in };
                path[0] = { live.x + dir.x * out, live.y + dir.y * out, live.z + dir.z * out };
            }
            path.push_back(Lerp(path[0], target, 0.5f));
            path.push_back(target);
            // A launch that moves under STILL_MOVE across is a fall, which the guard never judges.
            if (runrules::Distance(path[0], live) <= aggro + 0.5f || !Moves(path))
                continue;
            bool aggroes = false;
            for (const PathPoint& sample : Samples(path))
            {
                if (speed > 0.0f && sample.along > HORIZON)
                    break;
                if (runrules::Distance(sample.at, live) <= aggro)
                {
                    aggroes = true;
                    break;
                }
            }
            if (!aggroes)
                continue;
            ++reached[kind];
            if (GuardPath(path, { threat }, 7.0f, false).verdict == Verdict::Allow)
            {
                ++walkIns;
                if (walkIns <= 3)
                    printf("walk-in: kind %d, live %.1f,%.1f,%.1f, %.2f off its envelope, aggro %.2f, envelope reach %.2f\n",
                        kind, live.x, live.y, live.z, PlaceDistance(live, e.where, e.range), aggro, e.reach);
            }
        }
        CHECK(reached[0] > 300 && reached[1] > 300 && reached[2] > 300 && reached[3] > 300);
        CHECK(offCases > 1000);
        CHECK(walkIns == 0);
        if (walkIns)
            printf("the guard let %d walk-ins through\n", walkIns);
    }

    // GuardResult.margin: the least margin over the samples GuardPath looked at, which guard_last logs with its creature.
    void TestMargin()
    {
        std::mt19937 rng(5);
        std::uniform_real_distribution<float> pos(-40.0f, 40.0f), height(-10.0f, 10.0f), unit(0.0f, 1.0f);
        int allowed = 0, wrong = 0, textWrong = 0;
        for (int trial = 0; trial < 2000; ++trial)
        {
            std::vector<GuardThreat> threats;
            for (int k = 0; k < 3; ++k)
            {
                GuardThreat t = LiveThreat(uint32_t(100 + k), 0, { pos(rng), pos(rng), height(rng) }, 8.0f + unit(rng) * 10.0f,
                    k == 0 ? 0.0f : unit(rng) * 7.0f);
                t.aggro = t.reach - 3.9f;
                if (k == 2)
                {
                    t.bound = { t.where[0], { t.where[0].x + 10.0f, t.where[0].y, t.where[0].z } };
                    t.boundReach = t.reach;
                }
                threats.push_back(t);
            }
            const std::vector<Vec> path = { { pos(rng), pos(rng), height(rng) }, { pos(rng), pos(rng), height(rng) } };
            const GuardResult r = GuardPath(path, threats, 7.0f, false);
            if (r.verdict != Verdict::Allow || !Moves(path))
                continue;
            // A brute-force pass over the same samples, each threat to its horizon, gives the same least.
            float least = std::numeric_limits<float>::max();
            bool startsIn = false;
            for (const GuardThreat& t : threats)
                for (const PathPoint& sample : Samples(path))
                {
                    const float spread = SpreadAt(t, sample.along, 7.0f);
                    if (spread < 0.0f)
                        break;
                    const float m = ReachMargin(t, sample.at, spread);
                    least = std::min(least, m);
                    if (sample.along == 0.0f && m <= 0.0f)
                        startsIn = true;
                }
            if (std::fabs(least - r.margin) > 1e-4f)
                ++wrong;
            if (!startsIn)
            {
                ++allowed;
                if (r.margin <= 0.0f)
                    ++wrong;
            }
            // guard_last's margin is GuardPath's, with the creature it belongs to.
            const std::string text = CheckText(path, threats, r);
            const std::string name = "nearest=spawn_" + std::to_string(threats[r.marginThreat].spawn) + " ";
            if (text.find(" margin=" + Fixed1(r.margin) + " ") == std::string::npos || text.find(name) == std::string::npos)
                ++textWrong;
        }
        CHECK(allowed > 200);
        CHECK(wrong == 0);
        CHECK(textWrong == 0);

        // Gilnid's walk-in: the end 25.47 yards from where it stands at its spawn, its live reach 22.13 and a spline's walk,
        // bound by its envelope at 24.86. guard_last once added the walk over the whole horizon to the live reach and logged
        // -6.3; GuardPath passes the end by the bound, +0.6, and guard_last now says that.
        {
            GuardThreat gilnid = LiveThreat(3600095, 0, { 0, 0, 0 }, 22.13f, 2.5f);
            gilnid.live = false;
            gilnid.aggro = 18.6f;
            gilnid.bound = { { 0, 0, 0 } };
            gilnid.boundReach = 24.86f;
            const std::vector<Vec> path = { { 45, 0, 0 }, { 25.47f, 0, 0 } };
            CHECK(std::fabs(runrules::Distance(path.back(), gilnid.where[0]) - (gilnid.reach + MaxSpread(2.5f, 7.0f)) + 6.3f) < 0.05f);
            const GuardResult r = GuardPath(path, { gilnid }, 7.0f, false);
            CHECK(r.verdict == Verdict::Allow);
            CHECK(std::fabs(r.margin - 0.61f) < 0.02f);
            const std::string text = CheckText(path, { gilnid }, r);
            CHECK(text.find("nearest=spawn_3600095 ") != std::string::npos && text.find(" margin=0.6 ") != std::string::npos);
            // The server's own margin, between centres from that sample to where it stands, less its radius: 6.9.
            CHECK(text.find(" server=6.9") != std::string::npos);
        }
        // A refused path's margin is at or under 0, at the creature that refused it.
        {
            const GuardResult r = GuardPath({ { 0, 0, 0 }, { 40, 0, 0 } }, { Still(7, { 20, 6, 0 }, 10.0f) }, 7.0f, false);
            CHECK(r.verdict == Verdict::Refuse && r.margin <= 0.0f && r.marginThreat == 0);
        }
        // A path leaving a reach it starts in is allowed, judged by not getting nearer, and its margin says it is in reach.
        {
            const GuardResult r = GuardPath({ { 2, 0, 0 }, { 30, 0, 0 } }, { Still(7, { 0, 0, 0 }, 10.0f) }, 7.0f, false);
            CHECK(r.verdict == Verdict::Allow && r.margin <= 0.0f);
        }
    }

    void TestChaseEnd()
    {
        // Stop 69: the leader on the walkway at z 44.4, Gilnid at z 30.1, 27.2 yards across, a chase to 29.5 yards. The end
        // is on the 3D segment, 1.2 yards from the leader, never under it at the target's height.
        const Vec bot = { 0, 0, 44.4f }, target = { 27.2f, 0, 30.1f };
        Vec end;
        CHECK(ChaseEnd(bot, target, 29.5f, end));
        CHECK(std::fabs(runrules::Distance(end, bot) - 1.23f) < 0.05f);
        CHECK(std::fabs(runrules::Distance(end, target) - 29.5f) < 1e-3f);
        CHECK(std::fabs(runrules::Distance(bot, end) + runrules::Distance(end, target) - runrules::Distance(bot, target)) < 1e-3f);
        // A yard and a quarter down the slope toward the target, not 14 yards under the walkway at its height.
        CHECK(end.z > 43.5f && end.z < 44.4f);
        // Within the distance already: no move.
        CHECK(!ChaseEnd(bot, target, 31.0f, end));
        // Random: no end ever pairs the bot's place across with the target's height, and every end is on the segment.
        std::mt19937 rng(3);
        std::uniform_real_distribution<float> pos(-30.0f, 30.0f), dist(0.0f, 30.0f);
        int off = 0;
        for (int i = 0; i < 1000; ++i)
        {
            const Vec a = { pos(rng), pos(rng), pos(rng) }, b = { pos(rng), pos(rng), pos(rng) };
            const float d = dist(rng);
            Vec e;
            if (!ChaseEnd(a, b, d, e))
            {
                if (runrules::Distance(a, b) > d)
                    ++off;
                continue;
            }
            if (std::fabs(runrules::Distance(a, e) + runrules::Distance(e, b) - runrules::Distance(a, b)) > 1e-2f
                || std::fabs(runrules::Distance(e, b) - d) > 1e-2f)
                ++off;
            if (Distance2d(e, a) < 0.01f && std::fabs(e.z - b.z) < 0.01f && std::fabs(a.z - b.z) > 0.1f)
                ++off;
        }
        CHECK(off == 0);
    }
    // Item 4b-1, the wait's model (runguard::WaitReaches): on random patrols and followers, live anywhere on or about their
    // loops, in or out of a fight, walking or running, a path whose every sample the wait passes is passed by GuardPath,
    // within the horizon and past it; so whenever the guard refuses a path, the wait holds for that creature. A wait that
    // ends when no blocker reaches the way, the spot and the stands ends only when the guard lets the walk.
    void TestWaitModelAgrees()
    {
        std::mt19937 rng(41);
        std::uniform_real_distribution<float> pos(-40.0f, 40.0f), height(-10.0f, 10.0f), reachOf(8.0f, 22.0f), unit(0.0f, 1.0f);
        int clearPaths = 0, reachedPaths = 0, refused = 0, broken = 0, oldMissed = 0;
        for (int trial = 0; trial < 12000; ++trial)
        {
            const Envelope e = RandomEnvelope(rng, 2 + trial % 2, reachOf(rng));
            Vec live = RandomOnEnvelope(rng, e);
            // Drawn a little off its loop now and then, up to OFF_ENVELOPE_MARGIN, as a patrol between its points.
            if (trial % 5 == 0)
                live = { live.x + unit(rng) * 3.0f, live.y - unit(rng) * 3.0f, live.z };
            const float liveReach = e.reach * (0.7f + 0.5f * unit(rng));
            const float speed = trial % 7 == 0 ? 7.0f : 2.5f;
            const bool drawn = trial % 11 == 0;
            const GuardThreat threat = EnvelopeThreat(e, &live, liveReach, speed, drawn);
            // A path of two or three points, up to twice the horizon long.
            std::vector<Vec> path = { { pos(rng), pos(rng), height(rng) } };
            const int legs = 1 + trial % 2;
            for (int k = 0; k < legs; ++k)
            {
                const float angle = unit(rng) * 6.2831853f, length = unit(rng) * HORIZON;
                path.push_back({ path.back().x + length * std::cos(angle), path.back().y + length * std::sin(angle),
                    path.back().z + height(rng) / 4.0f });
            }
            std::vector<Vec> places;
            for (const PathPoint& sample : Samples(path))
                places.push_back(sample.at);
            const bool waits = WaitReaches(threat, places, 7.0f);
            const GuardResult verdict = GuardPath(path, { threat }, 7.0f, false);
            if (verdict.verdict != Verdict::Allow)
                ++refused;
            if (!waits)
            {
                ++clearPaths;
                if (verdict.verdict != Verdict::Allow)
                    ++broken;
            }
            else
                ++reachedPaths;
            // The model the wait had before 4b-1: its live place and reach, no walk and no bound, the pull spot only.
            const bool oldReached = runrules::Distance(path.back(), live) <= liveReach;
            if (!oldReached && verdict.verdict != Verdict::Allow)
                ++oldMissed;
        }
        CHECK(broken == 0);
        // Both answers were exercised, and the guard refused often enough to matter.
        CHECK(clearPaths > 1000 && reachedPaths > 1000 && refused > 1000);
        // The old model let the party go to spots the guard then refused.
        CHECK(oldMissed > 0);
        printf("wait model: %d clear, %d reached, %d refused by the guard, %d the old model missed\n", clearPaths, reachedPaths,
            refused, oldMissed);
    }

    // The foundry's stop 71 in 4a (3600052): a two-point loop 7.1 yards long at z 21, and a walkway spot its loop reaches
    // from the near end only. Live at the far end, the old model (its live place within its live reach of the spot) said
    // clear, Go fired, and the guard refused the walk: its walk across over the horizon (9.6 yards at 2.5 a second for a
    // mover at 7) carries it into reach. The wait's model holds there, from both ends; it clears once the spot is out of
    // the loop's reach.
    void TestStop71()
    {
        Envelope e;
        e.spawn = 3600052;
        e.kind = Kind::Patrol;
        e.reach = 15.0f;
        e.where = { { 0.0f, 0.0f, 21.0f }, { 7.1f, 0.0f, 21.0f }, { 0.0f, 0.0f, 21.0f } };
        CHECK(std::fabs(MaxSpread(2.5f, 7.0f) - 9.64f) < 0.05f);
        const Vec spot = { 20.0f, 0.0f, 21.0f }, waiting = { 40.0f, 0.0f, 21.0f };
        const std::vector<Vec> walk = { waiting, spot };
        for (const Vec& live : { Vec{ 0.0f, 0.0f, 21.0f }, Vec{ 7.1f, 0.0f, 21.0f } })
        {
            const GuardThreat threat = EnvelopeThreat(e, &live, 15.0f, 2.5f);
            CHECK(WaitReaches(threat, WaitPlaces({ waiting }, 0, spot, {}), 7.0f));
            CHECK(GuardPath(walk, { threat }, 7.0f, false).verdict == Verdict::Refuse);
        }
        // At the far end the old model said clear.
        CHECK(runrules::Distance(spot, Vec{ 0.0f, 0.0f, 21.0f }) > 15.0f);
        // A spot out of the loop's reach: clear by the wait, and the guard lets the walk.
        const Vec farSpot = { 30.0f, 0.0f, 21.0f };
        const GuardThreat nearEnd = EnvelopeThreat(e, &e.where[1], 15.0f, 2.5f);
        CHECK(!WaitReaches(nearEnd, WaitPlaces({ waiting }, 0, farSpot, {}), 7.0f));
        CHECK(GuardPath({ waiting, farSpot }, { nearEnd }, 7.0f, false).verdict == Verdict::Allow);
        // A stand in its reach holds the wait, as the guard would refuse the member's walk to it.
        CHECK(WaitReaches(nearEnd, WaitPlaces({ waiting }, 0, farSpot, { { 20.0f, 2.0f, 21.0f } }), 7.0f));
        // Evading or exempt, or with no place: no blocker.
        GuardThreat exempt = nearEnd;
        exempt.exempt = true;
        CHECK(!WaitReaches(exempt, { spot }, 7.0f));
        CHECK(!WaitReaches(GuardThreat(), { spot }, 7.0f));
    }

    // The route and the wait agree on a patrol that never leaves (WaitNeverClears): a patrol the route counts as pacing at a
    // place holds the wait there from every live place on its loop, so the route never plans a wait that cannot end. The
    // old test, reach alone at every loop point, is the narrower: what it called pacing still is.
    void TestWaitNeverClears()
    {
        // 3600052's loop: the walkway spot 20 yards from its near end, 27.1 from its far end, reach 15. The old test said
        // a patrol to wait for; the wait's model never clears, so the route counts it as pacing.
        const std::vector<Vec> loop = { { 0.0f, 0.0f, 21.0f }, { 7.1f, 0.0f, 21.0f }, { 0.0f, 0.0f, 21.0f } };
        const Vec spot = { 20.0f, 0.0f, 21.0f };
        CHECK(!std::all_of(loop.begin(), loop.end(), [&](const Vec& p) { return CreatureReaches({ spot }, { p }, 0.0f, 15.0f); }));
        CHECK(WaitNeverClears(spot, loop, 0.0f, 15.0f, 2.5f, 7.0f));
        // Out of the loop's reach at all: the bound clears it, whatever the walk.
        CHECK(!WaitNeverClears({ 30.0f, 0.0f, 21.0f }, loop, 0.0f, 15.0f, 2.5f, 7.0f));
        // A long loop that leaves: one to wait for.
        CHECK(!WaitNeverClears(spot, { { 0, 0, 21 }, { -40, 0, 21 }, { 0, 0, 21 } }, 0.0f, 15.0f, 2.5f, 7.0f));

        std::mt19937 rng(73);
        std::uniform_real_distribution<float> pos(-30.0f, 30.0f), height(-8.0f, 8.0f), reachOf(8.0f, 22.0f), unit(0.0f, 1.0f);
        int never = 0, narrower = 0, clears = 0;
        for (int trial = 0; trial < 4000; ++trial)
        {
            const Envelope e = RandomEnvelope(rng, 2 + trial % 2, reachOf(rng));
            const Vec place = { pos(rng), pos(rng), height(rng) };
            const float walk = trial % 9 == 0 ? 7.0f : 2.5f;
            const bool old = CreatureReaches({ place }, e.where, e.range, e.reach)
                && std::all_of(e.where.begin(), e.where.end(),
                    [&](const Vec& p) { return CreatureReaches({ place }, { p }, e.range, e.reach); });
            const bool now = WaitNeverClears(place, e.where, e.range, e.reach, walk, 7.0f);
            CHECK(!old || now);
            narrower += now && !old;
            if (!now)
            {
                ++clears;
                continue;
            }
            ++never;
            // A patrol: from every live place on its loop, between its points too, the wait holds.
            if (e.kind != Kind::Patrol)
                continue;
            for (int k = 0; k < 20; ++k)
            {
                const Vec live = RandomOnEnvelope(rng, e);
                CHECK(WaitReaches(EnvelopeThreat(e, &live, e.reach, walk), { place }, 7.0f));
            }
        }
        printf("wait never clears: %d of %d places (%d the old test called patrols to wait for), %d clear\n", never,
            never + clears, narrower, clears);
        CHECK(never > 100 && narrower > 20 && clears > 100);
    }

    // The wait judges each member with its own reach (GuildRunStates::Blockers): a creature reaches farther toward a
    // member of a lower level, about a yard a level, so a spot clear for the leader can be reached for that member, and the
    // guard refuses that member's walk. A blocker for any member is the wait's blocker.
    void TestWaitEachMember()
    {
        const float pad = ReachPad(0.5f, 0.4f);
        const float leaderReach = AggroRadius(15.0f, 24, 24) + pad, memberReach = AggroRadius(15.0f, 24, 20) + pad;
        CHECK(memberReach - leaderReach == 4.0f);
        Envelope e;
        e.spawn = 7;
        e.kind = Kind::Patrol;
        e.reach = leaderReach;
        e.where = { { 0, 0, 0 }, { 4, 0, 0 }, { 0, 0, 0 } };
        const Vec live = { 0, 0, 0 };
        // A stand two yards past the leader's reach of the loop, two inside the member's, the spot far off.
        const float stand = 4.0f + leaderReach + 2.0f;
        const std::vector<Vec> places = WaitPlaces({}, 0, { 60, 0, 0 }, { { stand, 0, 0 } });
        const GuardThreat forLeader = EnvelopeThreat(e, &live, leaderReach, 2.5f);
        const GuardThreat forMember = EnvelopeThreat(e, &live, memberReach, 2.5f);
        CHECK(!WaitReaches(forLeader, places, 7.0f));
        CHECK(WaitReaches(forMember, places, 7.0f));
        // The walk to the stand, inside the horizon: the guard lets the leader's and refuses the member's.
        const std::vector<Vec> walk = { { stand + 10.0f, 0, 0 }, { stand, 0, 0 } };
        CHECK(GuardPath(walk, { forLeader }, 7.0f, false).verdict == Verdict::Allow);
        CHECK(GuardPath(walk, { forMember }, 7.0f, false).verdict != Verdict::Allow);
    }

    // Off-way spots: for the cast alone only when the pack does not reach the way's point nearest it.
    void TestOffWayWanted()
    {
        CHECK(OffWayWanted(33.0f, 32.0f, false, true, true));     // the way passes far from the pack
        CHECK(OffWayWanted(25.0f, 32.0f, true, true, true));      // every spot on it is reached
        CHECK(OffWayWanted(25.0f, 32.0f, false, false, false));   // none castable, and the way out of the pack's reach
        CHECK(!OffWayWanted(25.0f, 32.0f, false, false, true));   // none castable, but the way passes within its reach
        CHECK(!OffWayWanted(25.0f, 32.0f, false, true, false));   // a castable, clear spot on the way
    }

    // The places the wait judges: the leg from its point `from` on, sampled as GuardPath samples, the spot, the stands.
    void TestWaitPlaces()
    {
        const std::vector<Vec> leg = { { 0, 0, 0 }, { 10, 0, 0 }, { 20, 0, 0 } };
        const Vec spot = { 30, 0, 0 };
        const std::vector<Vec> all = WaitPlaces(leg, 0, spot, { { 33, 3, 0 } });
        // 30 yards at SAMPLE, the start included, and the stand.
        CHECK(all.size() == size_t(30.0f / SAMPLE) + 1 + 1);
        CHECK(runrules::Distance(all.front(), leg.front()) < 0.01f && runrules::Distance(all[all.size() - 2], spot) < 0.01f);
        CHECK(runrules::Distance(all.back(), Vec{ 33, 3, 0 }) < 0.01f);
        // From the second point: the first ten yards are behind.
        const std::vector<Vec> ahead = WaitPlaces(leg, 1, spot, {});
        CHECK(ahead.size() == size_t(20.0f / SAMPLE) + 1 && runrules::Distance(ahead.front(), leg[1]) < 0.01f);
        // Past the leg: the spot alone.
        const std::vector<Vec> none = WaitPlaces(leg, leg.size(), spot, {});
        CHECK(none.size() == 1 && runrules::Distance(none.front(), spot) < 0.01f);
        // A blocker on the leg behind the mover holds nothing once the mover is past it.
        const GuardThreat behind = Moving(9, { 2, 6, 0 }, 8.0f, 2.5f);
        CHECK(WaitReaches(behind, WaitPlaces(leg, 0, spot, {}), 7.0f));
        CHECK(!WaitReaches(behind, WaitPlaces(leg, 2, spot, {}), 7.0f));
    }

    // Pull spots: the cast window, the off-way spot placed across, and the choice, which prefers a castable spot and
    // keeps every stop.
    void TestPullSpots()
    {
        CHECK(CastFits(28.0f, 0.0f) && !CastFits(28.1f, 0.0f) && CastFits(26.0f, 2.0f) && !CastFits(27.0f, 2.0f));
        CHECK(OnWayTry(20.0f) && OnWayTry(30.0f) && !OnWayTry(19.9f) && !OnWayTry(30.1f));
        // Level with the creature: PULL_BEST across, as before. Twenty yards below a walkway: across so 3D fits.
        CHECK(std::fabs(OffWayAcross(0.0f, 0.0f) - PULL_BEST) < 0.01f);
        const float across = OffWayAcross(20.0f, 0.0f);
        CHECK(std::fabs(across - std::sqrt(27.0f * 27.0f - 400.0f)) < 0.01f);
        CHECK(CastFits(std::sqrt(across * across + 400.0f), 0.0f));
        // Too far below, or with a wide wander: none fits.
        CHECK(OffWayAcross(27.5f, 0.0f) == 0.0f && OffWayAcross(20.0f, 6.5f) == 0.0f);
        std::mt19937 rng(5);
        std::uniform_real_distribution<float> dzOf(-30.0f, 30.0f), rangeOf(0.0f, 8.0f);
        int fits = 0;
        for (int trial = 0; trial < 2000; ++trial)
        {
            const float dz = dzOf(rng), range = rangeOf(rng);
            const float a = OffWayAcross(dz, range);
            if (a <= 0.0f)
                continue;
            ++fits;
            CHECK(a >= PULL_ACROSS_LEAST && a <= PULL_BEST && CastFits(std::sqrt(a * a + dz * dz), range));
        }
        CHECK(fits > 500);

        // The choice: walking there pulling nothing first, then castable, then the patrols to wait for.
        SpotRank reach, cast;
        reach.distance = 25.0f;
        cast.castable = true;
        cast.distance = 27.0f;
        CHECK(BetterSpot(cast, reach) && !BetterSpot(reach, cast));
        SpotRank castPatrol = cast;
        castPatrol.patrol = 1;
        CHECK(BetterSpot(castPatrol, reach));
        SpotRank castStrict = cast;
        castStrict.strict = 1;
        CHECK(BetterSpot(reach, castStrict));
        CHECK(BestSpot({ reach, castStrict, castPatrol, cast }) == 3);
        CHECK(BestSpot({}) == -1);
        // A stop whose spots are none castable keeps its best one: never dropped for it.
        std::uniform_int_distribution<int> count(0, 3), size(1, 8);
        std::uniform_real_distribution<float> distanceOf(20.0f, 30.0f), unit(0.0f, 1.0f);
        int stops = 0, chosen = 0, castChosen = 0, castAvailable = 0;
        for (int trial = 0; trial < 3000; ++trial)
        {
            std::vector<SpotRank> spots(size_t(size(rng)));
            for (SpotRank& spot : spots)
            {
                spot.strict = size_t(count(rng)) / 2;
                spot.patrol = size_t(count(rng));
                spot.extras = size_t(count(rng));
                spot.distance = distanceOf(rng);
                spot.castable = trial % 3 != 0 && unit(rng) < 0.4f;
            }
            ++stops;
            const int best = BestSpot(spots);
            chosen += best >= 0;
            // When a castable spot is as clear of strict reachers as any, a castable one is chosen.
            size_t least = spots[0].strict;
            for (const SpotRank& spot : spots)
                least = std::min(least, spot.strict);
            const bool castClear = std::any_of(spots.begin(), spots.end(),
                [&](const SpotRank& spot) { return spot.castable && spot.strict == least; });
            if (castClear)
            {
                ++castAvailable;
                castChosen += best >= 0 && spots[best].castable;
            }
            CHECK(best >= 0 && spots[best].strict == least);
        }
        CHECK(chosen == stops && castChosen == castAvailable && castAvailable > 500);
    }

}

int main()
{
    TestPath();
    TestMoving();
    TestIndex();
    TestPermits();
    TestRefusal();
    TestIntercept();
    TestFirstStop();
    TestUnmodelled();
    TestSharedReach();
    TestLiveMovers();
    TestPlanExempt();
    TestRefusalUse();
    TestAggroRadius();
    TestHeight();
    TestPlannerGuardAgree();
    TestServerTruth();
    TestMargin();
    TestChaseEnd();
    TestWaitModelAgrees();
    TestStop71();
    TestWaitPlaces();
    TestPullSpots();
    TestWaitNeverClears();
    TestWaitEachMember();
    TestOffWayWanted();
    printf("%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
