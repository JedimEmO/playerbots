#pragma once

// The run contract's decisions (docs/run-contract.md) as pure functions of plain facts, with no server types, so that
// tests/run_lifecycle builds them without the server.

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace runrules
{
    // Who has a party's run: the controller (lead), the controller keeping it where it is (hold), the job for a step of
    // its tables or the repair trip (released), or nobody, kept one tick so that every member sees it end (ended).
    enum class Mode : uint8_t { Lead, Hold, Released, Ended };

    // What the dungeon job is told of the run on its tick: no run state (none), or the run's mode.
    enum class Control : uint8_t { None, Lead, Hold, Released };

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

    // A held run resumes, its reasons gone, once the leader is within RESUME_NEAR yards of a member's hold place, or
    // RESUME_SECONDS after every reason went. nearest < 0: no member holds.
    constexpr float RESUME_NEAR = 40.0f;
    constexpr int64_t RESUME_SECONDS = 180;

    inline bool Resumes(bool reasonsGone, float nearest, int64_t secondsBack)
    {
        return reasonsGone && ((nearest >= 0.0f && nearest <= RESUME_NEAR) || secondsBack >= RESUME_SECONDS);
    }

    // Whether a run action fired under a stamp may act now: the state's mode and stamp. A leader's write only in lead; a
    // member's run move in lead, or in hold toward its hold place. Never in released or ended, never under another stamp.
    inline bool Accepts(Mode mode, const Stamp& state, const Stamp& action, bool leaderWrite)
    {
        if (!action.generation || !Same(state, action))
            return false;
        return mode == Mode::Lead || (mode == Mode::Hold && !leaderWrite);
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
    };
    // Act: do it. Request: set the phase or the step's fields, and return before any move or effect. Return: return before
    // it, keeping the phase.
    enum class JobMay : uint8_t { Act, Request, Return };

    inline JobMay Job(Control control, JobAct act, bool leaderAlive)
    {
        if (control == Control::None || act == JobAct::WayBackIn)
            return JobMay::Act;
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

    // A character's motion stack, bottom first, as the run's cancellation sees it.
    enum class Motion : uint8_t { Idle, Point, Path, Follow, Chase, Other };

    // The run's coarse cancellation of a member's motion: when a point, path or follow is anywhere in the stack, the
    // stack is cleared to its idle base, since the core cannot take one generator from under another; when a chase was on
    // top, the member's fight, it is put back with its target, offset and angle, if the target is still the victim.
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

    // Joins words with commas, for a key=value field.
    inline std::string Join(const std::vector<std::string>& words)
    {
        std::string text;
        for (const std::string& word : words)
            text += (text.empty() ? "" : ",") + word;
        return text;
    }
}
