#pragma once

// The movement guard of a party's run (plan item 4, docs/plan.md "Movement inside a run"): the pure rules, with no
// server types, so tests/run_lifecycle builds them without the server. A guild bot in a run never starts or keeps a move
// whose executed path or end enters the reach of a living creature outside the current fight. The geometry is A2b's
// (runrules::Vec, SegmentDistance, LinesDistance); this adds the one rule of whether a creature reaches a point (AggroRadius,
// ReachPad, PlaceDistance: the server's 3D distance, a creature's movement spreading its places across only), the moving
// creatures' horizon, leaving a reach, the reach index the map threads read without a lock, the permits, the refusal
// record and the intercept verdict.

#include "GuildRunRules.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <limits>
#include <unordered_map>
#include <string>
#include <utility>
#include <vector>

namespace runguard
{
    using runrules::Vec;

    constexpr float HORIZON = 20.0f;             // a moving creature counts only this far along a path; the watcher renews it
    constexpr float RENEW_SECONDS = 1.0f;        // added to a moving creature's travel time, for the watcher's next pass
    constexpr float STILL_MOVE = 1.0f;           // a launch moving less than this, across, is no move: a facing, a fall
    constexpr float CLIP_BACK = 1.0f;            // a clipped path ends this short of the first point in reach
    constexpr float CLIP_GAIN = 2.0f;            // and must gain at least this much
    constexpr float SAMPLE = 0.5f;               // the step that finds where a path enters a reach, or leaves it
    constexpr float CELL = 10.0f;                // the reach index's cells
    constexpr float CELL_SAMPLE = 2.5f;          // a path finds its cells by points this far apart
    constexpr int64_t REFUSAL_SECONDS = 5;       // a refused target or destination is refused again this long
    constexpr int64_t REFUSAL_END_SECONDS = 10;  // a refusal with no other this long has ended
    constexpr float REFUSAL_NEAR = 2.0f;         // a destination this near a refused one is the same
    constexpr float INTERCEPT_RANGE = 30.0f;     // a blocker this near the waiting spot can be pulled to it
    constexpr int64_t LATE_OK_SECONDS = 60;      // a late arrival before this second of the fight spoils an intercept
    constexpr int64_t VERDICT_FRESH_SECONDS = 3; // an intercept verdict older than this is not cast on
    constexpr float COMPANION_DRIFT = 8.0f;      // a companion this far off its certified offset changes the verdict
    constexpr float LATE_NEARER = 5.0f;          // as does a late arrival this much nearer the spot
    constexpr float PERMIT_NEAR = 3.0f;          // a launch ending this near a permit's end is the permitted one
    constexpr int64_t HEAL_PERMIT_SECONDS = 10;
    constexpr uint32_t HEAL_NO_NEED_PCT = 70;    // a patient over this is no longer a need
    constexpr int64_t PORTAL_PERMIT_SECONDS = 60;
    constexpr float PORTAL_MARGIN = 5.0f;
    constexpr int64_t SURFACE_PERMIT_SECONDS = 10;
    constexpr float HEAL_RING_IN = 5.0f;         // the heal ring stands this far inside the spell's range
    constexpr int HEAL_RING_POINTS = 8;

    inline float Distance2d(const Vec& a, const Vec& b)
    {
        const float dx = a.x - b.x, dy = a.y - b.y;
        return std::sqrt(dx * dx + dy * dy);
    }

    inline Vec Flat(const Vec& v)
    {
        return { v.x, v.y, 0.0f };
    }

    // The distance from a point to a creature's place: one point, or a patrol's path. In 3D, as A2b measures.
    inline float PointDistance(const Vec& point, const std::vector<Vec>& where)
    {
        return runrules::LinesDistance({ point }, where);
    }

    // The server's aggro radius, Unit::GetAttackDistance without auras and at an aggro rate of 1: the creature's detection
    // range less the bot's level over its own, that gap no lower than -25, and never under 5 yards. A creature that
    // detects nothing (range 0) aggroes nothing by sight.
    inline float AggroRadius(float detection, int32_t creatureLevel, int32_t botLevel)
    {
        if (detection <= 0.0f)
            return 0.0f;
        const int32_t gap = std::max<int32_t>(-25, botLevel - creatureLevel);
        return std::max(5.0f, detection - float(gap));
    }

    // What a creature's reach adds to its aggro radius: both bounding radii, since the reach is measured between centres,
    // and THREAT_MARGIN, the one margin of the route, the guard and the evidence.
    inline float ReachPad(float creatureBounding, float botBounding)
    {
        return creatureBounding + botBounding + runrules::THREAT_MARGIN;
    }

    // The one rule's distances live with A2b's geometry (GuildRunRules.h), so A2b's walk threats measure by them too.
    using runrules::Lerp;
    using runrules::LeastConvex;
    using runrules::SpreadDistance;
    using runrules::SegmentSpreadDistance;
    using runrules::PlaceDistance;

    // The one rule: whether a creature reaches a point, or a path, from where it can be.
    inline bool CreatureReaches(const std::vector<Vec>& path, const std::vector<Vec>& where, float spread, float reach)
    {
        const float d = PlaceDistance(path, where, spread);
        return d >= 0.0f && d <= reach;
    }

    // A creature as one move sees it.
    struct GuardThreat
    {
        uint32_t spawn = 0;            // 0 for a creature the route's model does not have, as a summon
        uint32_t entry = 0;            // set for a creature the model does not have, to name it
        std::vector<Vec> where;        // its live place, or for one not loaded its spawn
        float reach = 0.0f;            // how far it reaches from there: its aggro radius and ReachPad, in 3D
        float spread = 0.0f;           // yards across it may be from there, a wanderer's range when not loaded; never up
        float speed = 0.0f;            // yards a second it may move, for a loaded moving creature; 0 for any other
        float aggro = 0.0f;            // the server's radius (GetAttackDistance) toward the bot, for the evidence; 0 unknown
        bool exempt = false;           // in the current fight, or the plan's own pull
        // For a modelled creature not drawn off: its envelope, which it does not leave, so a point it reaches must be in
        // both its live reach and its envelope's. Empty for any other.
        std::vector<Vec> bound;
        float boundReach = 0.0f;
        float boundSpread = 0.0f;      // a wanderer's range around its spawn
        // Judged where it is, with its walk over the horizon: a patrol, a follower, or the live scan's creature drawn off
        // its envelope or not in the model. A refusal by one is waited for; by any other it is a defect.
        bool live = false;
    };

    // How far a point is from being reached by a threat that may be `spread` yards across from where it is: its distance
    // less its reach, and with a bound the larger of that and the bound's. At 0 or under, it is reached.
    inline float ReachMargin(const GuardThreat& threat, const Vec& point, float spread, float* distance = nullptr)
    {
        const float d = PlaceDistance(point, threat.where, spread);
        if (distance)
            *distance = d;
        float margin = d - threat.reach;
        if (!threat.bound.empty())
            margin = std::max(margin, PlaceDistance(point, threat.bound, threat.boundSpread) - threat.boundReach);
        return margin;
    }

    // Whether a threat reaches a point at this spread: within its live reach of where it is, and of its envelope when bound.
    inline bool Reaches(const GuardThreat& threat, const Vec& point, float spread, float* distance = nullptr)
    {
        return ReachMargin(threat, point, spread, distance) <= 0.0f;
    }

    // Whether a creature is in the party's current fight, and so passed by the guard: in combat, with a member of the run's
    // group, or a member's pet, on its threat list or as its victim. Whoever started the fight: the run's pull, the job's,
    // a patrol that came to the party, or a player in the party (plan item 6), so a party always defends itself.
    inline bool InCurrentFight(bool inCombat, bool memberOnThreatList, bool memberIsVictim)
    {
        return inCombat && (memberOnThreatList || memberIsVictim);
    }

    // Whether the plan's own pull is exempt for this bot: only in the pull phase, for the leader at once, and for a member
    // only while the pulled creature is alive, not evading and in the party's fight. Only the stop's own pack: never its
    // extras, nor the creatures the prediction has joining.
    inline bool PlanExempt(bool lead, bool inPull, bool pulledAliveInFight)
    {
        return inPull && (lead || pulledAliveInFight);
    }

    // A creature as a refusal names it, as A2b names a blocked walk's: by its spawn and its stop, or by its entry.
    inline std::string ThreatName(const GuardThreat& threat, int stop)
    {
        runrules::Threat named;
        named.spawn = threat.spawn;
        named.entry = threat.entry;
        return runrules::ThreatName(named, stop);
    }

    // How far across a creature may be from where it is at a point `along` yards into a path: its spread, and for a moving
    // one, only inside the horizon, what it can walk before the mover gets there and the watcher looks again. -1 beyond the
    // horizon, where the watcher has it. The walk spreads its places across, never up or down (SpreadDistance).
    inline float SpreadAt(const GuardThreat& threat, float along, float moverSpeed)
    {
        if (threat.speed <= 0.0f)
            return threat.spread;
        if (along > HORIZON)
            return -1.0f;
        const float seconds = (moverSpeed > 0.0f ? along / moverSpeed : 0.0f) + RENEW_SECONDS;
        return threat.spread + threat.speed * seconds;
    }

    // The most a creature with this speed walks across before the mover is past the horizon: what the live scan takes it
    // by, so that it never leaves out one the guard would refuse.
    inline float MaxSpread(float speed, float moverSpeed)
    {
        if (speed <= 0.0f)
            return 0.0f;
        return speed * ((moverSpeed > 0.0f ? HORIZON / moverSpeed : 0.0f) + RENEW_SECONDS);
    }

    // Whether a launch moves at all: STILL_MOVE across or more. A facing or a fall is no move, and accepts nothing.
    inline bool Moves(const std::vector<Vec>& path)
    {
        for (const Vec& point : path)
            if (Distance2d(path.front(), point) >= STILL_MOVE)
                return true;
        return false;
    }

    struct PathPoint
    {
        Vec at;
        float along = 0.0f;
    };

    // The point `along` yards into a path.
    inline Vec PointAlong(const std::vector<Vec>& path, float along)
    {
        if (path.empty())
            return Vec{};
        float done = 0.0f;
        for (size_t i = 1; i < path.size(); ++i)
        {
            const float length = runrules::Distance(path[i - 1], path[i]);
            if (done + length >= along)
            {
                const float t = length > 0.0f ? (along - done) / length : 0.0f;
                const Vec& a = path[i - 1];
                const Vec& b = path[i];
                return { a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t };
            }
            done += length;
        }
        return path.back();
    }

    // The path's points every SAMPLE yards, its vertices among them, each with how far along it is.
    inline std::vector<PathPoint> Samples(const std::vector<Vec>& path)
    {
        std::vector<PathPoint> samples;
        if (path.empty())
            return samples;
        float done = 0.0f;
        samples.push_back({ path[0], 0.0f });
        for (size_t i = 1; i < path.size(); ++i)
        {
            const Vec& a = path[i - 1];
            const Vec& b = path[i];
            const float length = runrules::Distance(a, b);
            const int steps = std::max(1, int(std::ceil(length / SAMPLE)));
            for (int s = 1; s <= steps; ++s)
            {
                const float t = float(s) / float(steps);
                samples.push_back({ { a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t }, done + length * t });
            }
            done += length;
        }
        return samples;
    }

    // The path's start to the first point `along` yards in, as a path.
    inline std::vector<Vec> Clip(const std::vector<Vec>& path, float along)
    {
        std::vector<Vec> clipped;
        if (path.empty())
            return clipped;
        clipped.push_back(path[0]);
        float done = 0.0f;
        for (size_t i = 1; i < path.size(); ++i)
        {
            const float length = runrules::Distance(path[i - 1], path[i]);
            if (done + length >= along)
            {
                clipped.push_back(PointAlong(path, along));
                return clipped;
            }
            clipped.push_back(path[i]);
            done += length;
        }
        return clipped;
    }

    enum class Verdict : uint8_t { Allow, Clip, Refuse };

    struct GuardResult
    {
        Verdict verdict = Verdict::Allow;
        uint32_t spawn = 0;            // the creature that refused it, or clipped it
        int threat = -1;               // its index in the threats given
        Vec at;                        // the first point of the path in its reach
        float along = 0.0f;            // how far along that is
        float clipAlong = 0.0f;        // for a clip: where the clipped path ends
        bool endInReach = false;       // the path's end is inside a creature's reach it did not start in
        // The margin GuardPath judged by: the least, over every threat not exempt and every sample it looked at, of how far
        // that sample is from being reached (ReachMargin). A path allowed that starts in no reach has it above 0.
        float margin = std::numeric_limits<float>::max();
        int marginThreat = -1;         // the threat it belongs to, -1 for none
        Vec marginAt;                  // and its sample
    };

    // Whether a path may be walked. A path that starts in a creature's reach may leave it, never getting nearer, but not
    // go back in; any other creature's reach it may not enter anywhere, between its points too. A moving creature counts
    // inside the horizon only. `clip` asks for a clip instead of a refusal where one gains CLIP_GAIN and the end is out of
    // every reach; the hook never clips, since it cannot change a generator's path.
    inline GuardResult GuardPath(const std::vector<Vec>& path, const std::vector<GuardThreat>& threats, float moverSpeed,
        bool clip)
    {
        GuardResult result;
        if (path.size() < 2)
            return result;
        // A launch that moves less than STILL_MOVE across is a facing or a fall.
        if (!Moves(path))
            return result;

        const std::vector<PathPoint> samples = Samples(path);
        float first = std::numeric_limits<float>::max();
        auto judged = [&](size_t t, const Vec& at, float margin)
        {
            if (margin < result.margin)
            {
                result.margin = margin;
                result.marginThreat = int(t);
                result.marginAt = at;
            }
        };
        for (size_t t = 0; t < threats.size(); ++t)
        {
            const GuardThreat& threat = threats[t];
            if (threat.exempt || threat.where.empty())
                continue;
            const float startSpread = SpreadAt(threat, 0.0f, moverSpeed);
            float previous = 0.0f;
            bool leaving = false;
            if (startSpread >= 0.0f)
            {
                const float margin = ReachMargin(threat, samples[0].at, startSpread);
                judged(t, samples[0].at, margin);
                leaving = margin <= 0.0f;
                // Leaving is judged by the creature's place itself, not by its walk, which grows along the path.
                previous = PointDistance(samples[0].at, threat.where);
            }
            for (size_t s = 1; s < samples.size(); ++s)
            {
                const PathPoint& sample = samples[s];
                if (sample.along >= first)
                    break;
                const float spread = SpreadAt(threat, sample.along, moverSpeed);
                if (spread < 0.0f)
                    break;
                const float margin = ReachMargin(threat, sample.at, spread);
                judged(t, sample.at, margin);
                const bool reached = margin <= 0.0f;
                if (leaving)
                {
                    // Out of it at last, or still in it and not getting nearer: on its way out.
                    const float d = PointDistance(sample.at, threat.where);
                    if (!reached)
                        leaving = false;
                    else if (d + 0.01f < previous)
                    {
                        first = sample.along;
                        result.threat = int(t);
                        break;
                    }
                    previous = d;
                    continue;
                }
                if (reached)
                {
                    first = sample.along;
                    result.threat = int(t);
                    break;
                }
            }
        }
        if (result.threat < 0)
            return result;

        result.spawn = threats[result.threat].spawn;
        result.along = first;
        result.at = PointAlong(path, first);
        // The end in a reach the path did not start in.
        const float total = samples.back().along;
        for (const GuardThreat& threat : threats)
        {
            if (threat.exempt || threat.where.empty())
                continue;
            const float endSpread = SpreadAt(threat, total, moverSpeed);
            const float startSpread = SpreadAt(threat, 0.0f, moverSpeed);
            const bool startIn = startSpread >= 0.0f && Reaches(threat, path[0], startSpread);
            if (endSpread >= 0.0f && !startIn && Reaches(threat, path.back(), endSpread))
                result.endInReach = true;
        }
        const float clipAlong = first - CLIP_BACK;
        if (clip && !result.endInReach && clipAlong >= CLIP_GAIN)
        {
            result.verdict = Verdict::Clip;
            result.clipAlong = clipAlong;
        }
        else
            result.verdict = Verdict::Refuse;
        return result;
    }

    // Where a chase or a follow of a target at `distance` ends: on the 3D segment from the target to the mover, `distance`
    // from the target, so the guard's look before the core's generator judges the point the move goes to, never one under
    // the mover at the target's height. False, no move, when the mover is that near already.
    inline bool ChaseEnd(const Vec& mover, const Vec& target, float distance, Vec& end)
    {
        const float length = runrules::Distance(mover, target);
        const float keep = std::max(0.0f, distance);
        if (length <= keep)
            return false;
        end = Lerp(target, mover, keep / length);
        return true;
    }

    inline std::string Fixed1(float value)
    {
        char text[32];
        std::snprintf(text, sizeof(text), "%.1f", value);
        return text;
    }

    // What guard_last says of a check GuardPath let through: its end, how many threats, and the margin GuardPath judged by
    // (GuardResult.margin) with the creature it belongs to, its place and speed. `server=` is the server's own margin: the
    // 3D distance between centres from that sample to the creature's place, less its aggro radius, when that is known.
    inline std::string CheckText(const std::vector<Vec>& path, const std::vector<GuardThreat>& threats, const GuardResult& result)
    {
        auto at = [](const Vec& v) { return Fixed1(v.x) + "," + Fixed1(v.y) + "," + Fixed1(v.z); };
        std::string text = std::string(result.verdict == Verdict::Clip ? "clipped" : "allowed")
            + " to=" + (path.empty() ? std::string("none") : at(path.back())) + " threats=" + std::to_string(threats.size());
        if (result.marginThreat < 0 || size_t(result.marginThreat) >= threats.size())
            return text;
        const GuardThreat& nearest = threats[result.marginThreat];
        text += " nearest=" + (nearest.entry ? "creature_" + std::to_string(nearest.entry) : "spawn_" + std::to_string(nearest.spawn))
            + " live=" + (nearest.live ? "1" : "0") + " at=" + at(nearest.where.front()) + " margin=" + Fixed1(result.margin)
            + " speed=" + Fixed1(nearest.speed);
        if (nearest.aggro > 0.0f)
            text += " server=" + Fixed1(runrules::Distance(result.marginAt, nearest.where.front()) - nearest.aggro);
        return text;
    }

    // How a creature of the map model may stand in a way. A follower walks with a master that patrols or wanders.
    enum class Kind : uint8_t { Still, Wanderer, Patrol, Follower };

    // A creature's envelope: everywhere it may be while nothing draws it off, and how far it reaches.
    struct Envelope
    {
        uint32_t spawn = 0;
        uint32_t pack = 0;
        Kind kind = Kind::Still;
        std::vector<Vec> where;        // its spawn, or its patrol's loop, closed (a follower's: its master's loop, or spawn)
        float reach = 0.0f;            // its aggro reach at the route's level, with ReachPad
        float range = 0.0f;            // a wanderer's wander range across; a follower's follow distance, and its master's range
        float walk = 2.5f, run = 7.0f; // yards a second
        bool aggroes = true;           // attacks on sight; one that does not only answers calls, and never reaches a path

        // How far across the whole envelope reaches: a wanderer may be anywhere within its range of its spawn. The reach
        // index's cells cover this much; the one rule (PlaceDistance) takes the range across only.
        float EnvelopeReach() const { return reach + range; }
    };

    // The one reach the route, its stand points and the guard share, for a creature standing still or wandering: its
    // envelope's reach, with its range across (PlaceDistance). -1 for a patrol or a follower, which the live scan gives
    // where it is, bound by its loop (EnvelopeThreat), and for one that does not attack on sight.
    inline float StaticReach(const Envelope& envelope)
    {
        if (!envelope.aggroes || envelope.where.empty())
            return -1.0f;
        return envelope.kind == Kind::Still || envelope.kind == Kind::Wanderer ? envelope.reach : -1.0f;
    }

    // Whether a creature's envelope refuses a path by the shared reach: anywhere along it, between its points too, a path
    // of one point being a place. A path this passes, the guard passes for that creature wherever it stands on its envelope.
    inline bool StaticRefuses(const Envelope& envelope, const std::vector<Vec>& path)
    {
        const float reach = StaticReach(envelope);
        if (reach < 0.0f || path.empty())
            return false;
        return CreatureReaches(path, envelope.where, envelope.range, reach);
    }

    constexpr float OFF_ENVELOPE_MARGIN = 5.0f;  // a modelled creature this far beyond its own shape is off its envelope

    // Whether a modelled creature stands outside its own envelope: beyond its spawn, its loop, or its range across, by more
    // than OFF_ENVELOPE_MARGIN, as one drawn off by a fight. The index's cells cannot find it there, so the live scan
    // (GuildPacks::LiveThreats) gives it, at its live place, as it gives the creatures the model does not have.
    inline bool OffEnvelope(const Envelope& envelope, const Vec& at)
    {
        if (envelope.where.empty())
            return true;
        return PlaceDistance(at, envelope.where, envelope.range) > OFF_ENVELOPE_MARGIN;
    }

    // A creature the guard judges where it is only: its live place, its live reach, and the yards a second it may move,
    // which SpreadAt turns into its walk across over the path's first HORIZON yards. The live scan's creatures drawn off
    // their envelopes or not in the model. Speed 0 stands still, and counts along the whole path.
    inline GuardThreat LiveThreat(uint32_t spawn, uint32_t entry, const Vec& at, float reach, float speed)
    {
        GuardThreat threat;
        threat.spawn = spawn;
        threat.entry = entry;
        threat.where = { at };
        threat.reach = reach;
        threat.speed = speed;
        threat.live = true;
        return threat;
    }

    // The guard's threat for a modelled creature, from its envelope and, when it is loaded, its live place (`live`, null
    // when it is not loaded), live reach and speed. `drawn`: in a fight, or off its envelope.
    // Still or wandering: not loaded, anywhere in its envelope; loaded, where it stands, with its walk across, bound by its
    // envelope unless drawn, so a path StaticRefuses passes is passed live too.
    // Patrol or follower: not loaded, nothing, since a party near it has loaded it; loaded, where it stands with its walk
    // across over the horizon, live (waited for), and unless drawn bound by its loop at the route's reach, so a place
    // the route plans clear of the whole loop is passed live too, wherever on the loop it walks.
    // The envelope's reach is at the route's level; a bot below it is reached from farther, so the bound grows by as much
    // as its live reach exceeds the envelope's. A creature not drawn may still stand up to OFF_ENVELOPE_MARGIN off its
    // envelope (a patrol's path between its points, a follower turning about its master), so the bound grows too by how
    // far its live place is from the envelope. The envelope's distance (PlaceDistance) is a distance to a set, so a point
    // the live place reaches is never more than that much farther from the envelope: the bound never cuts what the
    // creature really reaches. No threat is one with no place (`where` empty), which GuardPath passes over.
    inline GuardThreat EnvelopeThreat(const Envelope& envelope, const Vec* live, float liveReach, float speed, bool drawn = false)
    {
        GuardThreat threat;
        threat.spawn = envelope.spawn;
        if (!envelope.aggroes || envelope.where.empty())
            return threat;
        const bool still = StaticReach(envelope) >= 0.0f;
        if (!live)
        {
            if (still)
            {
                threat.where = envelope.where;
                threat.reach = envelope.reach;
                threat.spread = envelope.range;
            }
            return threat;
        }
        threat.where = { *live };
        threat.reach = liveReach;
        threat.speed = speed;
        threat.live = drawn || !still;
        if (!drawn)
        {
            threat.bound = envelope.where;
            threat.boundSpread = envelope.range;
            threat.boundReach = std::max(envelope.reach, liveReach)
                + std::max(0.0f, PlaceDistance(*live, envelope.where, envelope.range));
        }
        return threat;
    }

    // Every envelope of a route's map, found by the cells a path passes through. Made once, before the route, which plans
    // by it, and never changed, so a map thread reads it with no lock. The guard takes the creatures standing still or
    // wandering from it; patrols, followers, modelled creatures off their envelope (OffEnvelope) and the living ones the
    // model does not have come from the live scan, GuildPacks::LiveThreats, at each check.
    class ReachIndex
    {
    public:
        ReachIndex() = default;
        explicit ReachIndex(std::vector<Envelope> envelopes) : envelopes(std::move(envelopes))
        {
            // An envelope is listed in every cell that a path point within its reach could be sampled into: its shape plus
            // its reach, plus half a cell's diagonal and a sampling step, from the cell's centre.
            const float pad = CELL * 0.70711f + CELL_SAMPLE;
            for (uint32_t i = 0; i < this->envelopes.size(); ++i)
            {
                const Envelope& e = this->envelopes[i];
                if (e.where.empty())
                    continue;
                const float reach = e.EnvelopeReach() + pad;
                float x0 = e.where[0].x, x1 = x0, y0 = e.where[0].y, y1 = y0;
                for (const Vec& p : e.where)
                {
                    x0 = std::min(x0, p.x);
                    x1 = std::max(x1, p.x);
                    y0 = std::min(y0, p.y);
                    y1 = std::max(y1, p.y);
                }
                std::vector<Vec> flat;
                for (const Vec& p : e.where)
                    flat.push_back(Flat(p));
                for (int32_t cx = CellOf(x0 - reach); cx <= CellOf(x1 + reach); ++cx)
                    for (int32_t cy = CellOf(y0 - reach); cy <= CellOf(y1 + reach); ++cy)
                    {
                        const Vec centre = { (float(cx) + 0.5f) * CELL, (float(cy) + 0.5f) * CELL, 0.0f };
                        if (PointDistance(centre, flat) <= reach)
                            cells[Key(cx, cy)].push_back(i);
                    }
                bySpawn[e.spawn] = i;
            }
        }

        const std::vector<Envelope>& Envelopes() const { return envelopes; }

        const Envelope* BySpawn(uint32_t spawn) const
        {
            auto it = bySpawn.find(spawn);
            return it == bySpawn.end() ? nullptr : &envelopes[it->second];
        }

        // The envelopes that may reach any point of the path, each once, in index order.
        std::vector<uint32_t> Candidates(const std::vector<Vec>& path) const
        {
            std::vector<uint32_t> found;
            if (path.empty())
                return found;
            std::vector<char> seen(envelopes.size(), 0);
            auto visit = [&](const Vec& p)
            {
                auto it = cells.find(Key(CellOf(p.x), CellOf(p.y)));
                if (it == cells.end())
                    return;
                for (uint32_t i : it->second)
                    if (!seen[i])
                    {
                        seen[i] = 1;
                        found.push_back(i);
                    }
            };
            visit(path[0]);
            for (size_t i = 1; i < path.size(); ++i)
            {
                const Vec& a = path[i - 1];
                const Vec& b = path[i];
                const float length = Distance2d(a, b);
                const int steps = std::max(1, int(std::ceil(length / CELL_SAMPLE)));
                for (int s = 1; s <= steps; ++s)
                {
                    const float t = float(s) / float(steps);
                    visit({ a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, 0.0f });
                }
            }
            std::sort(found.begin(), found.end());
            return found;
        }

    private:
        static int32_t CellOf(float v) { return int32_t(std::floor(v / CELL)); }
        static int64_t Key(int32_t cx, int32_t cy) { return (int64_t(cx) << 32) ^ int64_t(uint32_t(cy)); }

        std::vector<Envelope> envelopes;
        std::unordered_map<int64_t, std::vector<uint32_t>> cells;
        std::unordered_map<uint32_t, uint32_t> bySpawn;
    };

    // The envelopes, in index order, that refuse a path by the shared reach (StaticRefuses), among those `living` takes:
    // the creatures alive at that point of the run's order. Found by the index's cells, which cover every envelope's whole
    // reach, so no wanderer is missed however far it ranges. The route's spots and stand paths are certified by it.
    inline std::vector<uint32_t> StaticBlockers(const ReachIndex& index, const std::vector<Vec>& path,
        const std::function<bool(const Envelope&)>& living)
    {
        std::vector<uint32_t> found;
        for (uint32_t i : index.Candidates(path))
        {
            const Envelope& envelope = index.Envelopes()[i];
            if (StaticReach(envelope) >= 0.0f && living(envelope) && StaticRefuses(envelope, path))
                found.push_back(i);
        }
        return found;
    }

    // A scoped exception to the guard: a healer's move to its patient, the way out through a portal, or surfacing for breath.
    enum class PermitKind : uint8_t { Heal, Portal, Surface };

    struct Permit
    {
        PermitKind kind = PermitKind::Heal;
        uint32_t subject = 0;          // the bot it lets move
        uint32_t patient = 0;          // for a heal
        uint64_t fight = 0;            // for a heal: the group's open fight
        uint32_t generation = 0, revision = 0;
        Vec end;                       // where the permitted move ends
        float radius = PERMIT_NEAR;    // how near that a launch must end
        int64_t expires = 0;
    };

    // A healer's permit to reach its patient: a move ending within `radius` of `end`, the patient's place and the spell's
    // reach, while the patient needs it, in this fight, under this plan, for HEAL_PERMIT_SECONDS at most.
    inline Permit HealPermit(uint32_t healer, uint32_t patient, uint64_t fight, uint32_t generation, uint32_t revision,
        const Vec& end, float radius, int64_t now)
    {
        Permit permit;
        permit.kind = PermitKind::Heal;
        permit.subject = healer;
        permit.patient = patient;
        permit.fight = fight;
        permit.generation = generation;
        permit.revision = revision;
        permit.end = end;
        permit.radius = radius;
        permit.expires = now + HEAL_PERMIT_SECONDS;
        return permit;
    }

    inline Permit PortalPermit(uint32_t bot, uint32_t generation, uint32_t revision, const Vec& trigger, float triggerRadius,
        int64_t now)
    {
        Permit permit;
        permit.kind = PermitKind::Portal;
        permit.subject = bot;
        permit.generation = generation;
        permit.revision = revision;
        permit.end = trigger;
        permit.radius = triggerRadius + PORTAL_MARGIN;
        permit.expires = now + PORTAL_PERMIT_SECONDS;
        return permit;
    }

    inline Permit SurfacePermit(uint32_t bot, uint32_t generation, uint32_t revision, const Vec& end, int64_t now)
    {
        Permit permit;
        permit.kind = PermitKind::Surface;
        permit.subject = bot;
        permit.generation = generation;
        permit.revision = revision;
        permit.end = end;
        permit.expires = now + SURFACE_PERMIT_SECONDS;
        return permit;
    }

    // Whether a permit lets this bot launch a move ending there, now. The hook and the watcher both ask it.
    inline bool Honours(const Permit& permit, uint32_t bot, const Vec& end, int64_t now)
    {
        return permit.subject == bot && now < permit.expires && runrules::Distance(end, permit.end) <= permit.radius;
    }

    struct PermitFacts
    {
        Vec at;                        // where the bot is
        bool patientAlive = true;
        uint32_t patientHealth = 0;    // percent
        uint64_t fight = 0;            // the group's open fight, 0 for none
        bool breathing = false;        // the bot's breath timer runs
        uint32_t generation = 0, revision = 0;
        int64_t now = 0;
    };

    enum class PermitEnd : uint8_t { None, Arrived, NoNeed, FightOver, Invalidated, TimedOut };

    // Whether, and why, a permit has run out. Every move after it is guarded again.
    inline PermitEnd Expired(const Permit& permit, const PermitFacts& facts)
    {
        if (facts.generation != permit.generation || facts.revision != permit.revision)
            return PermitEnd::Invalidated;
        if (facts.now >= permit.expires)
            return PermitEnd::TimedOut;
        const bool arrived = runrules::Distance(facts.at, permit.end) <= permit.radius;
        switch (permit.kind)
        {
            case PermitKind::Heal:
                if (arrived)
                    return PermitEnd::Arrived;
                if (!facts.patientAlive || facts.patientHealth > HEAL_NO_NEED_PCT)
                    return PermitEnd::NoNeed;
                if (facts.fight != permit.fight)
                    return PermitEnd::FightOver;
                return PermitEnd::None;
            case PermitKind::Portal:
                return arrived ? PermitEnd::Arrived : PermitEnd::None;
            case PermitKind::Surface:
                if (arrived)
                    return PermitEnd::Arrived;
                return facts.breathing ? PermitEnd::None : PermitEnd::NoNeed;
        }
        return PermitEnd::None;
    }

    // Where a healer may stand to heal a patient without a permit: the patient's own place, then a ring inside the spell's
    // range, nearest the healer first. The caller keeps the first the guard passes and the patient can be seen from.
    inline std::vector<Vec> HealSpots(const Vec& patient, float spellRange, const Vec& healer)
    {
        std::vector<Vec> spots;
        spots.push_back(patient);
        const float radius = std::max(0.0f, spellRange - HEAL_RING_IN);
        for (int i = 0; i < HEAL_RING_POINTS; ++i)
        {
            const float angle = float(i) * 2.0f * 3.14159265f / float(HEAL_RING_POINTS);
            spots.push_back({ patient.x + radius * std::cos(angle), patient.y + radius * std::sin(angle), patient.z });
        }
        std::stable_sort(spots.begin() + 1, spots.end(), [&](const Vec& a, const Vec& b)
        {
            return runrules::Distance(a, healer) < runrules::Distance(b, healer);
        });
        return spots;
    }

    // A bot's refusals: the last refused target or destination, refused again for REFUSAL_SECONDS so the AI does not issue
    // it again, and the open stretch of refusals the tick tells as one start and one end. Written by the bot's own map
    // thread, or by the world thread between map updates; read by the tick between map updates.
    struct Refusal
    {
        uint64_t target = 0;           // a chase or follow target's guid, or 0
        bool hasDest = false;
        Vec dest;
        uint32_t spawn = 0;            // the creature that refused it, 0 for one not in the model or none
        bool live = false;             // that creature is judged live (GuardThreat::live): a moving blocker, waited for
        int64_t until = 0;
        bool open = false;             // a stretch of refusals not yet ended
        uint32_t seq = 0;              // counts stretches
        int64_t lastAt = 0;            // the last refusal
        int64_t acceptedAt = 0;        // the last move accepted since
        bool acceptedSince = false;    // a move was accepted after the last refusal
        uint64_t plan = 0;             // the run's generation and plan revision it was made under
        size_t stop = SIZE_MAX;        // the run's stop it was made at, SIZE_MAX outside a run
    };

    // A run's generation and plan revision as one number, for a refusal's plan.
    inline uint64_t PlanKey(uint32_t generation, uint32_t revision)
    {
        return (uint64_t(generation) << 32) | revision;
    }

    inline uint32_t GenerationOf(uint64_t plan)
    {
        return uint32_t(plan >> 32);
    }

    inline void Note(Refusal& record, uint64_t target, const Vec* dest, uint32_t spawn, int64_t now, uint64_t plan = 0,
        size_t stop = SIZE_MAX, bool live = false)
    {
        if (!record.open)
        {
            record.open = true;
            ++record.seq;
        }
        record.target = target;
        record.hasDest = dest != nullptr;
        if (dest)
            record.dest = *dest;
        record.spawn = spawn;
        record.live = live;
        record.until = now + REFUSAL_SECONDS;
        record.lastAt = now;
        record.acceptedSince = false;
        record.plan = plan;
        record.stop = stop;
    }

    // A move that moves (Moves) was let through. A facing or a fall accepts nothing.
    inline void Accepted(Refusal& record, int64_t now)
    {
        if (record.open && now >= record.lastAt)
        {
            record.acceptedAt = now;
            record.acceptedSince = true;
        }
    }

    // Whether the bot refused here, at this stop and generation, lately: within REFUSAL_END_SECONDS of the last refusal,
    // with no move accepted since. By any creature: the leader's waiting budget starts at its first.
    inline bool Fresh(const Refusal& record, uint32_t generation, size_t stop, int64_t now)
    {
        return record.lastAt != 0 && !record.acceptedSince && GenerationOf(record.plan) == generation && record.stop == stop
            && now - record.lastAt < REFUSAL_END_SECONDS;
    }

    // Whether a refused bot is guarded, for readiness: fresh, and by a live creature, which moves and is waited for. One
    // refused by a creature standing still or wandering on its envelope, where the route certified the place, is far.
    inline bool GuardedNow(const Refusal& record, uint32_t generation, size_t stop, int64_t now)
    {
        return record.live && Fresh(record, generation, stop, now);
    }

    // Whether a bot holds still rather than ask for its run move again: refused by a live creature under this very plan,
    // generation and revision, at this stop, within REFUSAL_SECONDS, and nothing accepted since. It tries again once
    // that lapses, or at once when the plan changes.
    inline bool Holding(const Refusal& record, uint64_t plan, size_t stop, int64_t now)
    {
        return record.live && !record.acceptedSince && record.plan == plan && record.stop == stop && now < record.until;
    }

    // Whether a move to this target or destination was refused lately, under this plan, so the AI does not ask for it
    // again. A new plan revision, as the leader's pull, judges afresh.
    inline bool Refuses(const Refusal& record, uint64_t target, const Vec* dest, int64_t now, uint64_t plan = 0)
    {
        if (now >= record.until || record.plan != plan)
            return false;
        if (target && target == record.target)
            return true;
        return dest && record.hasDest && runrules::Distance(*dest, record.dest) <= REFUSAL_NEAR;
    }

    enum class RecordEvent : uint8_t { None, Start, End };

    // The tick's look at a record: the start of a stretch it has not told, or its end, when a move was accepted after the
    // last refusal or none came for REFUSAL_END_SECONDS. `told` is the stretch the tick told last.
    inline RecordEvent RecordStep(Refusal& record, uint32_t& told, int64_t now)
    {
        if (!record.open)
            return RecordEvent::None;
        if (told != record.seq)
        {
            told = record.seq;
            return RecordEvent::Start;
        }
        if (record.acceptedAt > record.lastAt || now - record.lastAt >= REFUSAL_END_SECONDS)
        {
            record.open = false;
            return RecordEvent::End;
        }
        return RecordEvent::None;
    }

    // A creature of a blocker's pack other than the blocker, as the tick sees it.
    struct Companion
    {
        uint32_t spawn = 0;
        bool alive = true;
        bool loaded = true;
        Vec at;
    };

    // A wanderer or patrol GuildPacks' prediction has reaching the fight later, and when.
    struct Late
    {
        uint32_t spawn = 0;
        Vec at;
        int64_t second = 0;
    };

    // What the tick knows about a blocker while the leader waits.
    struct InterceptFacts
    {
        uint32_t blocker = 0;
        bool alive = true;
        Vec at;                         // where it is
        Vec spot;                       // the waiting spot, where the fight would be held
        bool inSight = true;            // from the spot
        float callRadius = 5.0f;        // whom its pull calls
        std::vector<uint32_t> pack;     // its pack, itself included
        std::vector<Companion> companions;
        std::vector<uint32_t> pullSet;  // what the prediction has the pull bring
        std::vector<Late> late;
        bool spotPasses = true;         // the waiting spot passes the guard
        bool corridorPasses = true;     // and so does the way the leader walks if it must close to pull
    };

    enum class InterceptNo : uint8_t
    {
        Yes, Dead, Far, NoSight, CompanionUnknown, CompanionFar, Extras, LateArrival, SpotRefused, CorridorRefused
    };

    // Whether a blocker can be pulled to the waiting spot on purpose: near and seen, its companions accounted for, the
    // pull bringing nothing beyond its pack and nothing coming late, the spot and the way to the pull clear.
    inline InterceptNo Interceptable(const InterceptFacts& facts)
    {
        if (!facts.alive)
            return InterceptNo::Dead;
        if (runrules::Distance(facts.at, facts.spot) > INTERCEPT_RANGE)
            return InterceptNo::Far;
        if (!facts.inSight)
            return InterceptNo::NoSight;
        for (const Companion& companion : facts.companions)
        {
            if (!companion.alive)
                continue;
            if (!companion.loaded)
                return InterceptNo::CompanionUnknown;
            if (runrules::Distance(companion.at, facts.at) > facts.callRadius)
                return InterceptNo::CompanionFar;
        }
        for (uint32_t spawn : facts.pullSet)
            if (std::find(facts.pack.begin(), facts.pack.end(), spawn) == facts.pack.end())
                return InterceptNo::Extras;
        for (const Late& late : facts.late)
            if (late.second < LATE_OK_SECONDS)
                return InterceptNo::LateArrival;
        if (!facts.spotPasses)
            return InterceptNo::SpotRefused;
        if (!facts.corridorPasses)
            return InterceptNo::CorridorRefused;
        return InterceptNo::Yes;
    }

    // An intercept verdict: what the tick certified, for the leader to check again at the cast.
    struct InterceptVerdict
    {
        uint32_t id = 0;
        uint32_t blocker = 0;
        std::vector<uint32_t> pack;
        Vec at;                        // the blocker's place
        Vec spot;
        std::vector<std::pair<uint32_t, bool>> states;  // each companion, alive or not
        std::vector<std::pair<uint32_t, Vec>> offsets;  // each living companion's offset from the blocker
        std::vector<Late> late;
        uint32_t generation = 0, revision = 0;
        int64_t made = 0;
    };

    inline InterceptVerdict Certify(const InterceptFacts& facts, uint32_t id, uint32_t generation, uint32_t revision, int64_t now)
    {
        InterceptVerdict verdict;
        verdict.id = id;
        verdict.blocker = facts.blocker;
        verdict.pack = facts.pack;
        verdict.at = facts.at;
        verdict.spot = facts.spot;
        for (const Companion& companion : facts.companions)
        {
            verdict.states.push_back({ companion.spawn, companion.alive });
            if (companion.alive)
                verdict.offsets.push_back({ companion.spawn,
                    { companion.at.x - facts.at.x, companion.at.y - facts.at.y, companion.at.z - facts.at.z } });
        }
        verdict.late = facts.late;
        verdict.generation = generation;
        verdict.revision = revision;
        verdict.made = now;
        return verdict;
    }

    // What the leader sees at the cast, on its map thread.
    struct CastFacts
    {
        bool blockerAlive = true;
        Vec blockerAt;
        bool inSight = true;
        std::vector<Companion> companions;
        std::vector<Late> late;          // the certified late arrivals where they are now
        bool corridorPasses = true;
        bool newCreature = false;        // a creature not certified now reaches the spot
        uint32_t generation = 0, revision = 0;
        int64_t now = 0;
    };

    enum class Stale : uint8_t
    {
        None, Old, Plan, BlockerDead, BlockerFar, NoSight, CompanionChanged, CompanionMoved, LateNearer, Corridor, NewCreature
    };

    // Whether the verdict still holds at the cast. Any changed fact refuses the cast; the tick judges again.
    inline Stale StillValid(const InterceptVerdict& verdict, const CastFacts& facts)
    {
        if (facts.now - verdict.made > VERDICT_FRESH_SECONDS)
            return Stale::Old;
        if (facts.generation != verdict.generation || facts.revision != verdict.revision)
            return Stale::Plan;
        if (!facts.blockerAlive)
            return Stale::BlockerDead;
        if (runrules::Distance(facts.blockerAt, verdict.spot) > INTERCEPT_RANGE)
            return Stale::BlockerFar;
        if (!facts.inSight)
            return Stale::NoSight;
        for (const auto& [spawn, alive] : verdict.states)
        {
            auto live = std::find_if(facts.companions.begin(), facts.companions.end(),
                [spawn = spawn](const Companion& c) { return c.spawn == spawn; });
            if (live == facts.companions.end() || live->alive != alive || (alive && !live->loaded))
                return Stale::CompanionChanged;
        }
        for (const auto& [spawn, offset] : verdict.offsets)
        {
            auto live = std::find_if(facts.companions.begin(), facts.companions.end(),
                [spawn = spawn](const Companion& c) { return c.spawn == spawn; });
            const Vec now = { live->at.x - facts.blockerAt.x, live->at.y - facts.blockerAt.y, live->at.z - facts.blockerAt.z };
            if (runrules::Distance(now, offset) > COMPANION_DRIFT)
                return Stale::CompanionMoved;
        }
        for (const Late& certified : verdict.late)
        {
            auto live = std::find_if(facts.late.begin(), facts.late.end(),
                [&](const Late& l) { return l.spawn == certified.spawn; });
            if (live != facts.late.end()
                && runrules::Distance(live->at, verdict.spot) < runrules::Distance(certified.at, verdict.spot) - LATE_NEARER)
                return Stale::LateNearer;
        }
        if (!facts.corridorPasses)
            return Stale::Corridor;
        if (facts.newCreature)
            return Stale::NewCreature;
        return Stale::None;
    }

    enum class WaitAt : uint8_t { None, Arrival, Way };

    // Where the party waits for the first stop, which has no stop before: the route's arrival point when the guard passes
    // it, else the nearest point of the way behind the pull spot that it passes, else nowhere. `pullIndex` is the pull
    // spot's way index read with the run's progress (runrules::PlaceAlong), never by nearest point alone: a way that
    // passes a place twice, as Deadmines' upper mine, would otherwise put the spot on the wrong pass.
    inline WaitAt FirstWaitingSpot(const std::vector<Vec>& way, size_t pullIndex, const Vec& arrival,
        const std::function<bool(const Vec&)>& passes, Vec& out)
    {
        if (passes(arrival))
        {
            out = arrival;
            return WaitAt::Arrival;
        }
        for (size_t i = std::min(pullIndex, way.size()); i-- > 0;)
            if (passes(way[i]))
            {
                out = way[i];
                return WaitAt::Way;
            }
        return WaitAt::None;
    }
}
