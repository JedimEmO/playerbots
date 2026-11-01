#pragma once

// The run contract's decisions (docs/run-contract.md) as pure functions of plain facts, with no server types, so that
// tests/run_lifecycle builds them without the server.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace runrules
{
    // Who has a party's run: the controller (lead), the controller keeping it where it is (hold), the job for a step of
    // its tables or the repair trip (released), or nobody, kept one tick so that every member sees it end (ended).
    enum class Mode : uint8_t { Lead, Hold, Released, Ended };

    // What the dungeon job is told of the run on its tick: no run state (none), or the run's mode; released for a recovery
    // after a block is Recover, under which the job pulls nothing and runs only its recoveries.
    enum class Control : uint8_t { None, Lead, Hold, Released, Recover };

    // The generation and plan revision a run action was fired under. A generation is never 0.
    struct Stamp
    {
        uint32_t generation = 0;
        uint32_t plan = 0;
    };

    inline bool Same(const Stamp& a, const Stamp& b)
    {
        return a.generation == b.generation && a.plan == b.plan;
    }

    // A stamp as a run trigger's event carries it, "<generation>:<plan>", and back; a malformed text gives generation 0.
    inline std::string StampText(const Stamp& stamp)
    {
        return std::to_string(stamp.generation) + ":" + std::to_string(stamp.plan);
    }

    inline Stamp ParseStamp(const std::string& text)
    {
        Stamp stamp;
        const size_t colon = text.find(':');
        if (colon == std::string::npos || colon == 0 || colon + 1 >= text.size())
            return stamp;
        for (size_t i = 0; i < text.size(); ++i)
            if (i != colon && (text[i] < '0' || text[i] > '9'))
                return stamp;
        stamp.generation = uint32_t(std::stoul(text.substr(0, colon)));
        stamp.plan = uint32_t(std::stoul(text.substr(colon + 1)));
        return stamp;
    }

    inline const char* ModeName(Mode mode)
    {
        switch (mode)
        {
            case Mode::Lead: return "lead";
            case Mode::Hold: return "hold";
            case Mode::Released: return "released";
            default: return "ended";
        }
    }

    inline Control ControlOf(Mode mode)
    {
        switch (mode)
        {
            case Mode::Lead: return Control::Lead;
            case Mode::Hold: return Control::Hold;
            case Mode::Released: return Control::Released;
            default: return Control::None;
        }
    }

    // What the tick knows of a character. In the instance is looked up through the run's own instance.
    struct Facts
    {
        bool online = false;       // a Player exists, in the world or between maps
        bool engine = false;       // the engine plays it
        bool inWorld = false;
        bool teleporting = false;
        bool inInstance = false;   // on the run's map, in the run's instance
        bool alive = false;
    };

    // The dungeon job's report of its last tick.
    struct JobReport
    {
        bool inside = false;       // on its bosses with the leader inside
        bool stepDue = false;      // a step of its tables is the job's now
        bool leaving = false;      // the job takes the party out on purpose: the repair trip, or out and the reset at the end
    };

    // The party's size the dungeon job admits, and so the run's.
    constexpr size_t PARTY_SIZE = 5;

    // Why a run holds, each a word joined by underscores, in this order: the leader, the group's size, a wipe. While the
    // job takes the party out on purpose (leaving), the leader alive outside or between maps is no reason, and a wipe is
    // everyone dead.
    inline std::vector<std::string> HoldReasons(const Facts& leader, const std::vector<Facts>& members, bool leaving)
    {
        std::vector<std::string> reasons;
        if (!leader.online)
            reasons.push_back("leader_offline");
        else if (!leader.engine)
            reasons.push_back("leader_taken_over");
        else if (!leader.alive)
            reasons.push_back(leader.inInstance ? "leader_dead" : "leader_ghost");
        else if (!leader.inWorld || leader.teleporting)
        {
            if (!leaving)
                reasons.push_back("leader_between_maps");
        }
        else if (!leader.inInstance && !leaving)
            reasons.push_back("leader_outside");
        if (members.size() < PARTY_SIZE)
            reasons.push_back("short_handed");
        else if (members.size() > PARTY_SIZE)
            reasons.push_back("over_five");
        const bool anyAlive = std::any_of(members.begin(), members.end(),
            [&](const Facts& member) { return member.alive && (leaving || member.inInstance); });
        if (!anyAlive)
            reasons.push_back("wipe");
        return reasons;
    }

    // How a group's members compare with the run's expected ones: the same, a new five to take, or fewer or more than five.
    enum class Roster : uint8_t { Same, Replace, Short, Over };

    inline Roster RosterChange(std::vector<uint32_t> expected, std::vector<uint32_t> members)
    {
        if (members.size() < PARTY_SIZE)
            return Roster::Short;
        if (members.size() > PARTY_SIZE)
            return Roster::Over;
        std::sort(expected.begin(), expected.end());
        std::sort(members.begin(), members.end());
        return expected == members ? Roster::Same : Roster::Replace;
    }

    // Whether a run action fired under a stamp may act now: the state's mode and stamp. A leader's write only in lead; a
    // member's run move in lead, in hold, or in released for a member walking back to its party. Never in ended, never
    // under another stamp.
    inline bool Accepts(Mode mode, const Stamp& state, const Stamp& action, bool leaderWrite)
    {
        if (!action.generation || !Same(state, action))
            return false;
        return mode == Mode::Lead || ((mode == Mode::Hold || mode == Mode::Released) && !leaderWrite);
    }

    // What the dungeon job does on its tick, by what the run tells it.
    enum class JobAct : uint8_t
    {
        PhaseOut,      // a change of phase away from the boss phase: the resolution into Leave, the repair decision
        LeaveRepair,   // TickLeave and TickRepair
        WayBackIn,     // a leader outside the instance going back in
        MoveOn,        // rest, regroup after a death, the via place, travel to the next boss
        Step,          // a table step's moves and effects: talk, use, kill, sleeper, escort
        FirstBossTick, // the first tick back in the boss phase, before TickBosses
        Recover,       // a recovery after a block: the supply trip's start, or the leader standing while the party mends
    };
    // Act: do it. Request: set the phase or the step's fields, and return before any move or effect. Return: return before
    // it, keeping the phase.
    enum class JobMay : uint8_t { Act, Request, Return };

    inline JobMay Job(Control control, JobAct act, bool leaderAlive)
    {
        if (control == Control::None || act == JobAct::WayBackIn)
            return JobMay::Act;
        // A recovery: only the recovery itself, and the supply trip's way out and back once it has started.
        if (control == Control::Recover)
            return act == JobAct::Recover || act == JobAct::LeaveRepair ? JobMay::Act : JobMay::Return;
        switch (act)
        {
            case JobAct::PhaseOut:
                return control == Control::Released ? JobMay::Act : control == Control::Lead ? JobMay::Request : JobMay::Return;
            case JobAct::LeaveRepair:
                // Under hold, a dead leader's way back is the job's; a living one waits, its trip kept.
                return control == Control::Released || (control == Control::Hold && !leaderAlive) ? JobMay::Act : JobMay::Return;
            case JobAct::MoveOn:
                return JobMay::Return;
            case JobAct::Step:
                return control == Control::Released ? JobMay::Act : control == Control::Lead ? JobMay::Request : JobMay::Return;
            case JobAct::FirstBossTick:
                // Under released the party is handed back before TickBosses acts; under lead it is already the controller's.
                return control == Control::Lead ? JobMay::Act : JobMay::Return;
            default:
                return JobMay::Return;
        }
    }

    // Whether the run may move or stop a character: one the engine plays, alive, in the world and not between maps, and
    // in the run's own instance. Players and ghosts are never moved.
    inline bool Movable(const Facts& facts)
    {
        return facts.engine && facts.alive && facts.inWorld && !facts.teleporting && facts.inInstance;
    }

    // A character's motion stack, bottom first, as the run's cancellation sees it. A point or path move with a run token
    // is the run's own (RunToken), stopped only by its record (RecordStep), never by this cancellation.
    enum class Motion : uint8_t { Idle, Point, Path, Follow, Chase, Other, RunToken };

    // The cancellation of a member's motion when it is placed in hold: a follow, or a point or path move that is not the
    // run's, anywhere in the stack, and the stack is cleared to its idle base; when a chase was on top, the member's fight,
    // it is put back with its target, offset and angle, if the target is still the victim.
    struct CancelPlan
    {
        bool clear = false;
        bool restoreChase = false;
    };

    inline CancelPlan PlanCancel(const std::vector<Motion>& stack, bool chaseTargetStillVictim)
    {
        CancelPlan plan;
        plan.clear = std::any_of(stack.begin(), stack.end(),
            [](Motion motion) { return motion == Motion::Point || motion == Motion::Path || motion == Motion::Follow; });
        plan.restoreChase = plan.clear && !stack.empty() && stack.back() == Motion::Chase && chaseTargetStillVictim;
        return plan;
    }

    // Whether a dungeon's route gives the controller a run at all: with no stop, the job has the party alone, as before.
    inline bool Routed(size_t stops)
    {
        return stops > 0;
    }

    // The dungeon job's phases, in GuildDungeon::Phase's order.
    enum class Phase : uint8_t { Check, Go, Hold, Bosses, Leave, Repair };

    // What the job's tick does after resolving its dungeon, by the run's control and where its party is.
    enum class Dispatch : uint8_t
    {
        WaitRelease,  // it changed phase away from the bosses this tick: it returns, so the run is released first
        Holds,        // the run holds: it returns, keeping its phase
        Leave,        // TickLeave
        Repair,       // TickRepair
        Outside,      // the leader outside the dungeon: its way back in, or the way there
        Entering,     // inside, the party not all in yet: the leader waits for it
        HandBack,     // back on the bosses after a release: it reports the party in and returns before TickBosses
        Bosses,       // TickBosses
        Recover,      // TickRecover: a recovery after a block, inside with the party
    };

    struct DispatchFacts
    {
        Control control = Control::None;
        Phase phase = Phase::Check;
        bool leaderAlive = true;
        bool phasedOut = false;     // this tick's resolution moved the job into Leave
        bool onDungeonMap = false;  // the leader is on the dungeon's map
        bool partyIn = false;       // the party has entered, and is all in or the leader is alive
    };

    inline Dispatch Decide(const DispatchFacts& facts)
    {
        if (facts.phasedOut && Job(facts.control, JobAct::PhaseOut, facts.leaderAlive) != JobMay::Act)
            return Dispatch::WaitRelease;
        if (facts.phase == Phase::Leave || facts.phase == Phase::Repair)
        {
            if (Job(facts.control, JobAct::LeaveRepair, facts.leaderAlive) != JobMay::Act)
                return facts.control == Control::Hold ? Dispatch::Holds : Dispatch::WaitRelease;
            return facts.phase == Phase::Leave ? Dispatch::Leave : Dispatch::Repair;
        }
        if (!facts.onDungeonMap)
            return Dispatch::Outside;
        if (!facts.partyIn)
            return Dispatch::Entering;
        // Under hold nothing of the boss phase runs, the leader dead or alive.
        if (facts.control == Control::Hold)
            return Dispatch::Holds;
        if (facts.control == Control::Recover)
            return Dispatch::Recover;
        if (facts.phase != Phase::Bosses && Job(facts.control, JobAct::FirstBossTick, facts.leaderAlive) != JobMay::Act)
            return Dispatch::HandBack;
        return Dispatch::Bosses;
    }

    // Whether a step of the dungeon's tables is the job's: a step waiting for the release, a use, a kill (one to fight where
    // it is, or one the job's pull takes), a sleeping boss waited for, or a talk before the next boss. Under released the
    // job keeps the party until none is left.
    struct StepFacts
    {
        bool waiting = false;
        bool use = false;
        bool kill = false;
        bool killPull = false;
        bool sleeper = false;
        bool talk = false;
    };

    inline bool StepDue(const StepFacts& step)
    {
        return step.waiting || step.use || step.kill || step.killPull || step.sleeper || step.talk;
    }

    // Every expected member, the leader among them, online, in the world and not between maps, in the run's own instance,
    // and alive: the pull waits for nothing less, without a time limit.
    inline bool Present(const std::vector<Facts>& expected)
    {
        return std::all_of(expected.begin(), expected.end(),
            [](const Facts& member) { return member.online && member.inWorld && !member.teleporting && member.inInstance && member.alive; });
    }

    // A run move's destination: only from the view whose stamp the action's is, and from nothing else. False, with no
    // destination asked for, when the view has moved on.
    template<class View, class Place, class PlaceOf>
    bool ValidatedPlace(Mode mode, const Stamp& state, const Stamp& action, bool leaderWrite, const View& view,
        PlaceOf placeOf, Place& place)
    {
        if (!Accepts(mode, state, action, leaderWrite))
            return false;
        place = placeOf(view);
        return true;
    }

    // Run move ids: the top bit set, so that no other point or path move a player can get shares one. A token is never 0
    // and never reused while a record or the member's stack holds it, so a wrap after 2^31 moves collides with nothing.
    constexpr uint32_t RUN_TOKEN_BIT = 0x80000000u;

    inline bool IsRunToken(uint32_t id)
    {
        return (id & RUN_TOKEN_BIT) != 0 && id != RUN_TOKEN_BIT;
    }

    template<class InUse>
    uint32_t NextToken(uint32_t& counter, InUse inUse)
    {
        for (uint32_t tries = 0; tries < 1000; ++tries)
        {
            counter = counter >= 0x7FFFFFFFu ? 1u : counter + 1u;
            const uint32_t token = RUN_TOKEN_BIT | counter;
            if (!inUse(token))
                return token;
        }
        return 0;
    }

    // A run move's record, and what the cleanup does with it.
    struct RecordFacts
    {
        bool runExists = false;
        bool stampCurrent = false;    // the run's generation and plan revision are the record's
        bool memberInRun = false;     // still one of the run's expected members
        bool onRecordInstance = false; // standing on the record's map and instance
        bool online = false;
        bool engine = false;          // played by the engine, not a player
        bool inWorld = false;
        bool teleporting = false;
        bool tokenInStack = false;    // a move with the record's token is in the member's motion stack
        bool reachable = true;        // the pass may touch the member: the world pass, or the map pass on the member's own map
    };

    enum class RecordAction : uint8_t
    {
        Retire, // the move is gone: the record goes
        Keep,   // current
        Remove, // stale: RemoveMove its token
        Wait,   // stale, but not to be touched now: between maps, a player's, or not on this pass's map
        Drop,   // offline: logging out destroyed the stack
    };

    inline RecordAction RecordStep(const RecordFacts& facts)
    {
        if (!facts.online)
            return RecordAction::Drop;
        const bool settled = facts.inWorld && !facts.teleporting;
        if (settled && !facts.tokenInStack)
            return RecordAction::Retire;
        if (facts.runExists && facts.stampCurrent && facts.memberInRun && facts.onRecordInstance)
            return RecordAction::Keep;
        if (!facts.engine || !settled || !facts.reachable)
            return RecordAction::Wait;
        return RecordAction::Remove;
    }

    // What issuing a new run move does with the member's previous one and any token no record holds: the result of
    // removing each. A deferred removal postpones the new move; the old record stays.
    enum class Removal : uint8_t { Removed, NotFound, Deferred };

    inline bool MayIssue(const std::vector<Removal>& removals)
    {
        return std::none_of(removals.begin(), removals.end(), [](Removal removal) { return removal == Removal::Deferred; });
    }

    // Rejoining (docs/run-contract.md, "Rejoining"): a member apart from its party walks back to it over the route's
    // cleared way, and the party pulls nothing until every member is back.
    constexpr float WAY_NEAR = 10.0f;            // a place this near the route's way is on it
    constexpr float ARRIVE_NEAR = 5.0f;          // a member this near its walk's end has arrived
    constexpr float DISPLACED = 60.0f;           // in lead or released, a member this far from its leader is apart
    constexpr float WALK_DRIFT = 4.0f;           // a walking member this far off its walk is planned again
    constexpr float SHORT_WALK = 20.0f;          // a member this near where it walks to goes by one short path, not the way
    constexpr float CHUNK_START = 2.0f;          // a walk's next part starts only this near its first point
    constexpr float WALK_CHUNK_YARDS = 40.0f;    // one run move walks at most this far
    constexpr size_t WAY_CHECK_POINTS = 10;      // the way's own legs are checked this many points (20 yards) at a time
    constexpr float WAY_CHECK_OFF = 2.0f;        // and a leg's path keeps this near the way
    constexpr float CONNECTOR_DETOUR = 1.5f;     // a connector's path is at most this many times its straight length
    constexpr float THREAT_MARGIN = 3.0f;        // the margin on a creature's reach, as GuildPacks measures a pull spot's
    constexpr int64_t COHORT_DEAD_SECONDS = 30;  // a member seen dead this recently is not of a hold's cohort
    constexpr int64_t HOLD_TRAVEL_SECONDS = 60;  // a cohort member not at the hold place this long after its walk began stands
    constexpr int64_t REPLAN_SECONDS = 10;       // a walk that could not be planned is tried again this often
    constexpr int64_t WALK_STALL_SECONDS = 30;   // a walk that gains no part of its way this long is planned again
    constexpr int64_t REJOIN_SECONDS = 600;      // a run apart this long, nothing else holding it, ends: cannot_rejoin
    constexpr int PLAN_BUDGET = 16;              // path searches a tick, across every run

    struct Vec
    {
        float x = 0.0f, y = 0.0f, z = 0.0f;
    };

    inline float Distance(const Vec& a, const Vec& b)
    {
        const float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
        return std::sqrt(dx * dx + dy * dy + dz * dz);
    }

    inline float Length(const std::vector<Vec>& line)
    {
        float length = 0.0f;
        for (size_t i = 1; i < line.size(); ++i)
            length += Distance(line[i - 1], line[i]);
        return length;
    }

    // The distance between two segments, ab and cd, either of which may be a point (Ericson, Real-Time Collision
    // Detection, 5.1.9).
    inline float SegmentDistance(const Vec& a, const Vec& b, const Vec& c, const Vec& d)
    {
        const Vec d1 = { b.x - a.x, b.y - a.y, b.z - a.z }, d2 = { d.x - c.x, d.y - c.y, d.z - c.z };
        const Vec r = { a.x - c.x, a.y - c.y, a.z - c.z };
        auto dot = [](const Vec& u, const Vec& v) { return u.x * v.x + u.y * v.y + u.z * v.z; };
        const float aa = dot(d1, d1), ee = dot(d2, d2), ff = dot(d2, r);
        const float eps = 1e-6f;
        float s = 0.0f, t = 0.0f;
        if (aa <= eps && ee <= eps)
            return Distance(a, c);
        if (aa <= eps)
            t = std::clamp(ff / ee, 0.0f, 1.0f);
        else
        {
            const float cc = dot(d1, r);
            if (ee <= eps)
                s = std::clamp(-cc / aa, 0.0f, 1.0f);
            else
            {
                const float bb = dot(d1, d2), denom = aa * ee - bb * bb;
                s = denom > eps ? std::clamp((bb * ff - cc * ee) / denom, 0.0f, 1.0f) : 0.0f;
                t = (bb * s + ff) / ee;
                if (t < 0.0f)
                {
                    t = 0.0f;
                    s = std::clamp(-cc / aa, 0.0f, 1.0f);
                }
                else if (t > 1.0f)
                {
                    t = 1.0f;
                    s = std::clamp((bb - cc) / aa, 0.0f, 1.0f);
                }
            }
        }
        const Vec p = { a.x + d1.x * s, a.y + d1.y * s, a.z + d1.z * s }, q = { c.x + d2.x * t, c.y + d2.y * t, c.z + d2.z * t };
        return Distance(p, q);
    }

    // The least distance between two polylines, either of one point or more. -1 when either is empty.
    inline float LinesDistance(const std::vector<Vec>& a, const std::vector<Vec>& b)
    {
        if (a.empty() || b.empty())
            return -1.0f;
        float least = -1.0f;
        for (size_t i = 0; i < std::max<size_t>(a.size() - 1, 1); ++i)
            for (size_t j = 0; j < std::max<size_t>(b.size() - 1, 1); ++j)
            {
                const Vec& a1 = a[i];
                const Vec& a2 = a.size() > 1 ? a[i + 1] : a[i];
                const Vec& b1 = b[j];
                const Vec& b2 = b.size() > 1 ? b[j + 1] : b[j];
                const float d = SegmentDistance(a1, a2, b1, b2);
                if (least < 0.0f || d < least)
                    least = d;
            }
        return least;
    }

    // A creature that may stand in a walk's way: where it may be (one point, or a patrol's path, closed when it loops)
    // and how far it reaches from there.
    struct Threat
    {
        uint32_t spawn = 0;            // 0 for a creature the route's model does not have, as a summon
        uint32_t entry = 0;            // set for a creature the model does not have, to name it
        std::vector<Vec> where;
        float reach = 0.0f;
    };

    // A living creature the route's model does not have, a summon or a script's spawn, reaches this far from where it
    // stands: its aggro reach, the margin, and TRAVEL_MARGIN more when it moves.
    constexpr float TRAVEL_MARGIN = 10.0f;

    // A creature as a blocked walk names it: by its spawn and its stop (0 for none), or one the model does not have by its
    // entry, with its spawn when it has one.
    inline std::string ThreatName(const Threat& threat, int stop)
    {
        if (threat.entry)
            return "creature_" + std::to_string(threat.entry)
                + (threat.spawn ? "_spawn_" + std::to_string(threat.spawn) : std::string()) + "_unmodelled";
        return "spawn_" + std::to_string(threat.spawn) + "_stop_" + std::to_string(stop + 1);
    }

    // The first creature whose reach comes to the line anywhere along it, between its points too, or -1.
    inline int FirstThreat(const std::vector<Threat>& threats, const std::vector<Vec>& line)
    {
        for (size_t i = 0; i < threats.size(); ++i)
        {
            const float d = LinesDistance(threats[i].where, line);
            if (d >= 0.0f && d <= threats[i].reach)
                return int(i);
        }
        return -1;
    }

    // The route's way points within `near` yards of a place, in the way's order.
    inline std::vector<size_t> WayNear(const std::vector<Vec>& way, const Vec& at, float near)
    {
        std::vector<size_t> found;
        for (size_t i = 0; i < way.size(); ++i)
            if (Distance(way[i], at) <= near)
                found.push_back(i);
        return found;
    }

    // Of the way's points near a walk's start and near its end, the pair nearest each other along the way. A way that
    // passes a place twice, as a route coming back past rooms it went through early, is walked the short way between
    // them, not through everything it met between its two passes. False when either list is empty.
    inline bool WaySpan(const std::vector<size_t>& starts, const std::vector<size_t>& ends, size_t& a, size_t& b)
    {
        if (starts.empty() || ends.empty())
            return false;
        size_t i = 0, j = 0, best = SIZE_MAX;
        while (i < starts.size() && j < ends.size())
        {
            const size_t gap = starts[i] > ends[j] ? starts[i] - ends[j] : ends[j] - starts[i];
            if (gap < best)
            {
                best = gap;
                a = starts[i];
                b = ends[j];
            }
            if (starts[i] < ends[j])
                ++i;
            else
                ++j;
        }
        return true;
    }

    // Where a party stands along the route's way, as a way index. Set down fresh, with no progress: its nearest way point.
    // Choosing again after a hold, a hand-back or a new five, with the run's progress as a way index: of the way points
    // within WAY_NEAR of it, the one nearest that progress along the way, so that a way passing the place twice is read
    // by where the run was, not by which pass happens to be nearer. With none that near, the nearest point.
    inline size_t PlaceAlong(const std::vector<size_t>& near, size_t nearest, bool hasProgress, size_t progress)
    {
        if (!hasProgress || near.empty())
            return nearest;
        size_t best = near[0];
        for (size_t i : near)
        {
            const size_t gap = i > progress ? i - progress : progress - i;
            const size_t bestGap = best > progress ? best - progress : progress - best;
            if (gap < bestGap)
                best = i;
        }
        return best;
    }

    // Whether every point of a path keeps within `off` yards of a polyline.
    inline bool Along(const std::vector<Vec>& path, const std::vector<Vec>& line, float off)
    {
        return std::all_of(path.begin(), path.end(), [&](const Vec& point)
        {
            const float d = LinesDistance({ point }, line);
            return d >= 0.0f && d <= off;
        });
    }

    // A walk's line cut into run moves of at most `yards` each, every part starting where the one before ends. A single
    // segment longer than `yards` stays whole.
    inline std::vector<std::vector<Vec>> Chunks(const std::vector<Vec>& line, float yards)
    {
        std::vector<std::vector<Vec>> chunks;
        if (line.size() < 2)
            return chunks;
        std::vector<Vec> chunk = { line[0] };
        float length = 0.0f;
        for (size_t i = 1; i < line.size(); ++i)
        {
            const float step = Distance(line[i - 1], line[i]);
            if (chunk.size() > 1 && length + step > yards)
            {
                chunks.push_back(chunk);
                chunk = { line[i - 1] };
                length = 0.0f;
            }
            chunk.push_back(line[i]);
            length += step;
        }
        if (chunk.size() > 1)
            chunks.push_back(chunk);
        return chunks;
    }

    // A member played by a person, not by the engine: the run never moves one, so it is with the party by where it is,
    // within HUMAN_NEAR of the leader, or of the anchor in hold. A person cannot see the anchor, so the reach is wider than
    // a walking member's ARRIVE_NEAR.
    constexpr float HUMAN_NEAR = 20.0f;

    inline bool Human(const Facts& facts)
    {
        return facts.online && !facts.engine;
    }

    // Whether a person's member is with the party: alive, in the world and not between maps, in the run's instance and
    // in the group, and within HUMAN_NEAR of where the party is. distance < 0: no place to measure from, as with the
    // leader dead and no anchor yet; alive inside is then enough.
    inline bool HumanPresent(const Facts& facts, bool inGroup, float distance)
    {
        return Human(facts) && facts.alive && facts.inWorld && !facts.teleporting && facts.inInstance && inGroup
            && (distance < 0.0f || distance <= HUMAN_NEAR);
    }

    // Of a hold's cohort: a member the run may move, not seen dead in the last COHORT_DEAD_SECONDS. lastDead 0: never.
    inline bool InCohort(const Facts& facts, int64_t lastDead, int64_t now)
    {
        return Movable(facts) && (lastDead == 0 || now - lastDead > COHORT_DEAD_SECONDS);
    }

    // Whether the party may start a fight: a pull, a kill step's pull, or an event start. Only in lead, or in released
    // while the job is not taking the party out, with nobody apart and the run not failed. The live part, every expected
    // member alive inside now, is checked where the pull is made (GuildRunStates::PullAllowed).
    struct PermitFacts
    {
        Mode mode = Mode::Ended;
        bool leaving = false;
        bool apart = false;
        bool failed = false;
        bool recovering = false;   // released for a recovery after a block: nothing is pulled until it is over
    };

    inline bool PullPermit(const PermitFacts& facts)
    {
        if (facts.apart || facts.failed || facts.recovering)
            return false;
        return facts.mode == Mode::Lead || (facts.mode == Mode::Released && !facts.leaving);
    }

    // A run with a member apart since `apartSince`, nothing else holding it, for REJOIN_SECONDS: it cannot regroup.
    inline bool Overdue(int64_t apartSince, int64_t now)
    {
        return apartSince != 0 && now - apartSince >= REJOIN_SECONDS;
    }

    // A party whose run failed as cannot_rejoin keeps its group until the agent's next order, so the instance stays valid
    // for its members and they stand where they are: the order it failed for is its job order still. 0 never keeps it.
    inline bool KeepsGroup(uint32_t keepGroupOrder, uint32_t jobOrder)
    {
        return keepGroupOrder != 0 && keepGroupOrder == jobOrder;
    }

    // Whether a member that joined a party for its party job leaves the group: its job ended or changed, unless it keeps
    // the group after a failed run.
    inline bool LeavesParty(bool joined, bool working, bool partyJob, bool keepsGroup)
    {
        return joined && (!working || !partyJob) && !keepsGroup;
    }

    // A walk's planning: done, not possible from here (with why), or waiting for the next tick's path searches.
    enum class PlanResult : uint8_t { Planned, Blocked, Pending };

    // Readiness (docs/run-contract.md, "Readiness"): why an expected member, the leader among them, keeps the pull back.
    // Dead and away come from Present's facts and A2b's rejoining set; the rest from the member as the leader sees it.
    enum class Keep : uint8_t { Dead, Away, Fight, Far, Health, Mana, Supplies };

    inline const char* KeepName(Keep keep)
    {
        switch (keep)
        {
            case Keep::Dead: return "dead";
            case Keep::Away: return "away";
            case Keep::Fight: return "fight";
            case Keep::Far: return "far";
            case Keep::Health: return "health";
            case Keep::Mana: return "mana";
            default: return "supplies";
        }
    }

    // A member as readiness sees it. Health and mana are percents; mana counts only for one that uses mana.
    // A member that can make the drink or food itself, as a mage conjures water and bread, carries it: hasDrink and hasFood
    // say so, and waiting mends it, since its own AI makes some out of a fight.
    struct Ready
    {
        uint32_t guid = 0;
        Facts facts;               // as Present reads them
        bool rejoining = false;    // in the run's rejoining set: on its way back, not yet with the party
        bool inFight = false;
        float distance = 0.0f;     // from the fight spot
        uint32_t health = 100;
        bool usesMana = false;
        uint32_t mana = 100;
        bool hasFood = true;       // carries food suited to its level
        bool hasDrink = true;      // carries drink suited to its level; asked only of one that uses mana
    };

    // The thresholds, which never change while a party waits.
    constexpr float READY_NEAR = 12.0f;
    constexpr uint32_t READY_HEALTH = 80;
    constexpr uint32_t READY_MANA = 60;

    struct Kept
    {
        uint32_t guid = 0;
        Keep why = Keep::Dead;
    };

    // Each member that keeps the pull back, with each of its reasons, in the members' order. One not there is only dead or
    // away. One short of health with no food, or of mana with no drink, is supplies in place of that shortfall, since
    // waiting does not mend it.
    inline std::vector<Kept> Readiness(const std::vector<Ready>& members)
    {
        std::vector<Kept> kept;
        for (const Ready& member : members)
        {
            const Facts& f = member.facts;
            const bool present = f.online && f.inWorld && !f.teleporting && f.inInstance && f.alive;
            if (!present || member.rejoining)
            {
                kept.push_back({ member.guid, f.online && !f.alive ? Keep::Dead : Keep::Away });
                continue;
            }
            if (member.inFight)
                kept.push_back({ member.guid, Keep::Fight });
            if (member.distance > READY_NEAR)
                kept.push_back({ member.guid, Keep::Far });
            const bool noFood = member.health < READY_HEALTH && !member.hasFood;
            const bool noDrink = member.usesMana && member.mana < READY_MANA && !member.hasDrink;
            if (member.health < READY_HEALTH && !noFood)
                kept.push_back({ member.guid, Keep::Health });
            if (member.usesMana && member.mana < READY_MANA && !noDrink)
                kept.push_back({ member.guid, Keep::Mana });
            if (noFood || noDrink)
                kept.push_back({ member.guid, Keep::Supplies });
        }
        return kept;
    }

    // A member short of what waiting cannot mend blocks the pull at once, before the budget runs out.
    inline bool BlocksAtOnce(const std::vector<Kept>& kept)
    {
        return std::any_of(kept.begin(), kept.end(), [](const Kept& k) { return k.why == Keep::Supplies; });
    }

    // The waiting budget: one per stop and generation, from the first time the leader gathers at the stop's spot under
    // that generation. A change of reasons, a pull that brought nothing, or a wait at the stop before keeps it.
    constexpr int64_t READY_BUDGET = 300;

    struct Budget
    {
        size_t stop = SIZE_MAX;
        uint32_t generation = 0;
        int64_t since = 0;         // 0 before the leader first gathers
    };

    inline void TrackBudget(Budget& budget, size_t stop, uint32_t generation, bool gathered, int64_t now)
    {
        if (budget.stop != stop || budget.generation != generation)
            budget = { stop, generation, 0 };
        if (gathered && !budget.since)
            budget.since = now;
    }

    inline bool BudgetOut(const Budget& budget, int64_t now)
    {
        return budget.since && now - budget.since >= READY_BUDGET;
    }

    // Whether a block is due: the budget out, or a reason waiting cannot mend; and only with reasons, rechecked now, so
    // that a party ready at the deadline never gets an empty block. The block at once waits for the leader gathered at its
    // spot (`gathered`), so the rest of its reasons are not those of a party still walking there or back from a fight.
    inline bool BlockDue(const Budget& budget, int64_t now, const std::vector<Kept>& kept, bool gathered)
    {
        return !kept.empty() && (BudgetOut(budget, now) || (gathered && BlocksAtOnce(kept)));
    }

    // Whether the run may pull now, checked by every pull, at the trigger and at execution: never while the pull phase is
    // blocked or the job recovers, and only with nobody keeping the pull back. Fighting what comes stays allowed.
    inline bool PullAllowed(bool blocked, bool recovering, const std::vector<Kept>& kept)
    {
        return !blocked && !recovering && kept.empty();
    }

    // A block the leader wrote, with its own id; the tick takes each one once, however often it reconciles.
    inline bool TakeBlock(uint32_t& lastTaken, uint32_t blockId)
    {
        if (!blockId || blockId == lastTaken)
            return false;
        lastTaken = blockId;
        return true;
    }

    // The job's recovery after a block: an obligation for each member and reason the block named, each with its own
    // deadline from the block. Health, mana, a far member and a fight get 3 minutes, a member dead or away 5, and a supply
    // trip 15 in all, out, buying and back. New reasons add no obligation and move no deadline.
    constexpr int64_t RECOVERY_SECONDS = 180;
    constexpr int64_t RECOVERY_DEAD_SECONDS = 300;
    constexpr int64_t RECOVERY_SUPPLY_SECONDS = 900;

    struct Obligation
    {
        uint32_t guid = 0;
        Keep why = Keep::Dead;
        int64_t deadline = 0;
    };

    inline std::vector<Obligation> Obligations(const std::vector<Kept>& block, int64_t now)
    {
        std::vector<Obligation> obligations;
        for (const Kept& k : block)
        {
            const int64_t seconds = k.why == Keep::Supplies ? RECOVERY_SUPPLY_SECONDS
                : k.why == Keep::Dead || k.why == Keep::Away ? RECOVERY_DEAD_SECONDS : RECOVERY_SECONDS;
            obligations.push_back({ k.guid, k.why, now + seconds });
        }
        return obligations;
    }

    // Whether an obligation is met by the member as it is now. A member not there, or still rejoining, meets none but
    // its own return, so a member gone off the map keeps its health or supplies unresolved. Supplies are met only with
    // the member back with the party and carrying what it lacked.
    inline bool Met(const Obligation& obligation, const Ready& member)
    {
        const Facts& f = member.facts;
        const bool back = f.online && f.inWorld && !f.teleporting && f.inInstance && f.alive && !member.rejoining;
        if (!back)
            return false;
        switch (obligation.why)
        {
            case Keep::Dead: case Keep::Away: return true;
            case Keep::Fight: return !member.inFight;
            case Keep::Far: return member.distance <= READY_NEAR;
            case Keep::Health: return member.health >= READY_HEALTH;
            case Keep::Mana: return !member.usesMana || member.mana >= READY_MANA;
            default:
                return (member.health >= READY_HEALTH || member.hasFood)
                    && (!member.usesMana || member.mana >= READY_MANA || member.hasDrink);
        }
    }

    // A supply trip with no safe way out: no trip, and each supplies obligation turns into a wait for the member's own
    // health and mana to come back, met when readiness clears them, from now with the usual deadline. No block is counted
    // for it by itself. How long the way out may stay unplanned, the tick's path searches spent, before it counts as none.
    constexpr int64_t EXIT_PLAN_SECONDS = 15;

    inline void NoSafeWay(std::vector<Obligation>& obligations, int64_t now)
    {
        std::vector<Obligation> out;
        for (const Obligation& obligation : obligations)
        {
            if (obligation.why != Keep::Supplies)
            {
                out.push_back(obligation);
                continue;
            }
            out.push_back({ obligation.guid, Keep::Health, now + RECOVERY_SECONDS });
            out.push_back({ obligation.guid, Keep::Mana, now + RECOVERY_SECONDS });
        }
        obligations = out;
    }

    // Where a recovery is: going on, done with every obligation met, or over once an unmet one passes its deadline.
    // A member the tick cannot see is passed as not there.
    enum class Recovery : uint8_t { Going, Done, Over };

    inline Recovery RecoveryStep(const std::vector<Obligation>& obligations, const std::vector<Ready>& members, int64_t now)
    {
        bool all = true;
        for (const Obligation& obligation : obligations)
        {
            auto it = std::find_if(members.begin(), members.end(), [&](const Ready& r) { return r.guid == obligation.guid; });
            if (it != members.end() && Met(obligation, *it))
                continue;
            if (now >= obligation.deadline)
                return Recovery::Over;
            all = false;
        }
        return all ? Recovery::Done : Recovery::Going;
    }

    // Blocks are counted by stop identity, the stop's pack, across holds, recoveries and the stop chosen again; a count
    // is cleared only when the stop is verified cleared. The third block at one stop fails the run, with no recovery.
    constexpr uint32_t BLOCKS_TO_FAIL = 3;

    enum class BlockOutcome : uint8_t { Recover, Fail };

    inline BlockOutcome CountBlock(std::map<uint32_t, uint32_t>& counts, uint32_t stopKey)
    {
        return ++counts[stopKey] >= BLOCKS_TO_FAIL ? BlockOutcome::Fail : BlockOutcome::Recover;
    }

    inline void StopCleared(std::map<uint32_t, uint32_t>& counts, uint32_t stopKey)
    {
        counts.erase(stopKey);
    }

    // A member far from its stand: a new path, its cached one dropped, when it has not come REPATH_GAIN yards nearer in
    // REPATH_SECONDS. A new generation or a new destination starts the tracking again.
    constexpr int64_t REPATH_SECONDS = 20;
    constexpr float REPATH_GAIN = 1.0f;

    struct Nearing
    {
        uint32_t generation = 0;
        uint64_t destination = 0;  // a key of the place it walks to
        float best = -1.0f;
        int64_t at = 0;
    };

    inline bool Repath(Nearing& nearing, uint32_t generation, uint64_t destination, float distance, int64_t now)
    {
        if (nearing.generation != generation || nearing.destination != destination || nearing.best < 0.0f
            || distance <= nearing.best - REPATH_GAIN)
        {
            nearing = { generation, destination, distance, now };
            return false;
        }
        if (now - nearing.at < REPATH_SECONDS)
            return false;
        nearing.best = distance;
        nearing.at = now;
        return true;
    }

    // Keepers as an event's field: name:why, by the given names.
    template<class NameOf>
    std::string KeepText(const std::vector<Kept>& kept, NameOf nameOf)
    {
        std::string text;
        for (const Kept& k : kept)
            text += (text.empty() ? "" : ",") + nameOf(k.guid) + ":" + KeepName(k.why);
        return text;
    }

    // Joins words with commas, for a key=value field.
    inline std::string Join(const std::vector<std::string>& words)
    {
        std::string text;
        for (const std::string& word : words)
            text += (text.empty() ? "" : ",") + word;
        return text;
    }
}
