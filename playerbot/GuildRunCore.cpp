#include "GuildRunCore.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

using namespace runcore;
using runrules::Mode;

namespace
{
    std::string Num(float value)
    {
        char text[32];
        snprintf(text, sizeof(text), "%.1f", value);
        return text;
    }

    bool Contains(const std::vector<uint32_t>& guids, uint32_t guid)
    {
        return std::find(guids.begin(), guids.end(), guid) != guids.end();
    }
}

void RunCore::Note(const std::string& kind, const std::string& fields)
{
    events.push_back({ kind, "gen=" + std::to_string(start) + "." + std::to_string(generation) + " rev=" + std::to_string(plan)
        + (fields.empty() ? "" : " " + fields) });
}

void RunCore::Revise(const std::string& what, const std::string& fields)
{
    ++plan;
    Note("run_plan", "what=" + what + (fields.empty() ? "" : " " + fields));
}

void RunCore::SetMode(RunWorld& world, Generations& generations, Mode to, const std::string& why)
{
    // Every change of mode is a new generation, so every run move of the old one is stale, and the token cleanup
    // (GuildRunStates::CleanMoves) stops it before the maps update.
    (void)world;
    mode = to;
    reason = why;
    generation = generations.Next();
    Note("run_mode", std::string("mode=") + runrules::ModeName(to) + " reason=" + why);
}

void RunCore::Begin(RunWorld& world, Generations& generations, uint32_t groupId, uint32_t leaderGuid,
    const std::vector<uint32_t>& members)
{
    (void)world;
    group = groupId;
    leader = leaderGuid;
    expected = members;
    start = generations.Start();
    mode = Mode::Lead;
    generation = generations.Next();
    Note("run_mode", "mode=lead reason=start members=" + std::to_string(members.size()));
}

void RunCore::End(RunWorld& world, Generations& generations, const std::string& why)
{
    if (mode == Mode::Ended)
        return;
    held.clear();
    ClearApart();
    settled.clear();
    choose = false;
    endSeen = false;
    recovering = false;
    obligations.clear();
    SetMode(world, generations, Mode::Ended, why);
}

void RunCore::Place(RunWorld& world, const std::vector<uint32_t>& members)
{
    // The members gone from the instance, dead, or out of the group let go of their places; a living member of the group
    // the engine plays inside gets one, once: where it stands, with no hold place yet.
    for (auto it = held.begin(); it != held.end();)
    {
        if (!Contains(expected, it->first) || !Contains(members, it->first) || !runrules::Movable(world.FactsOf(it->first)))
            it = held.erase(it);
        else
            ++it;
    }
    for (uint32_t guid : expected)
    {
        if (held.count(guid) || !Contains(members, guid) || !runrules::Movable(world.FactsOf(guid)))
            continue;
        // What it was doing stops first, a follow included, then it holds where that left it.
        world.CancelMotion(guid);
        Held place;
        if (!world.Position(guid, place.x, place.y, place.z))
            continue;
        held[guid] = place;
        const std::string at = Num(place.x) + "," + Num(place.y) + "," + Num(place.z);
        Note("run_hold_at", "member=" + world.Name(guid) + " result=arrived pos=" + at + " place=" + at
            + " dist=0 kind=stood why=no_hold_place_yet");
    }
}

bool RunCore::Reconcile(RunWorld& world, Generations& generations, time_t now)
{
    // Ended: seen once, kept; seen again, removed. Nothing of the world is looked at, the group least of all.
    if (mode == Mode::Ended)
    {
        if (endSeen)
            return false;
        endSeen = true;
        return true;
    }

    std::vector<uint32_t> members;
    uint32_t groupLeader = 0;
    if (!world.Group(group, members, groupLeader))
    {
        End(world, generations, "group_gone");
        return true;
    }
    if (!Contains(members, leader))
    {
        End(world, generations, "leader_left_group");
        return true;
    }
    if (groupLeader != leader)
    {
        End(world, generations, "new_group_leader");
        return true;
    }
    if (world.ActiveJob(leader) != "dungeon")
    {
        End(world, generations, "job_changed");
        return true;
    }

    // A new five: the departed let go of their places, then the five are expected. The new generation makes every run
    // move of the old roster stale, and the token cleanup stops it.
    const runrules::Roster roster = runrules::RosterChange(expected, members);
    if (roster == runrules::Roster::Replace)
    {
        for (auto it = held.begin(); it != held.end();)
            it = Contains(members, it->first) ? std::next(it) : held.erase(it);
        expected = members;
        ++membership;
        generation = generations.Next();
        if (mode == Mode::Lead)
            choose = true;
        std::string names;
        for (uint32_t guid : members)
            names += (names.empty() ? "" : ",") + world.Name(guid);
        Note("run_members", "membership=" + std::to_string(membership) + " members=" + names);
    }
    else if (roster != runrules::Roster::Same)
        for (auto it = held.begin(); it != held.end();)
            it = Contains(members, it->first) ? std::next(it) : held.erase(it);

    std::vector<runrules::Facts> facts;
    for (uint32_t guid : members)
        facts.push_back(world.FactsOf(guid));
    for (uint32_t guid : expected)
    {
        const runrules::Facts member = world.FactsOf(guid);
        if (member.online && !member.alive)
            lastDead[guid] = now;
    }
    // The job's departure on purpose is exempt only while the job has the party for it, or the hold began there.
    const bool leaving = job.leaving && (mode == Mode::Released || (mode == Mode::Hold && heldFrom == Mode::Released));
    holdReasons = runrules::HoldReasons(world.FactsOf(leader), facts, leaving);

    if (!holdReasons.empty())
    {
        const std::string why = runrules::Join(holdReasons);
        if (mode != Mode::Hold)
        {
            heldFrom = mode;
            held.clear();
            choose = false;
            SetMode(world, generations, Mode::Hold, why);
            BeginHold(world, now);
        }
        else if (reason != why)
        {
            reason = why;
            Note("run_mode", "mode=hold reason=" + why);
        }
        Place(world, members);
        if (leaving)
            ClearApart();
        else
            Track(world, members, now);
        apartSince = 0;
        return true;
    }

    // The job takes the party out: nobody walks back into the dungeon, and nothing is pulled (Permit).
    if (leaving)
    {
        ClearApart();
        apartSince = 0;
        if (mode == Mode::Hold)
        {
            held.clear();
            SetMode(world, generations, Mode::Released, "resume_leaving");
        }
        return true;
    }

    if (mode == Mode::Hold)
        Place(world, members);
    Track(world, members, now);

    // Held with every reason gone, while the party regroups: the reason says so.
    if (mode == Mode::Hold && !apart.empty() && reason != "regrouping")
    {
        reason = "regrouping";
        Note("run_mode", "mode=hold reason=regrouping");
    }

    // Every reason gone, and every member back with the party at the anchor: the run resumes, and the leader chooses its
    // stop from there. Never by time: a party apart stays held until it regroups or cannot.
    if (mode == Mode::Hold && apart.empty())
    {
        held.clear();
        hasAnchor = false;
        const Mode to = heldFrom == Mode::Released ? Mode::Released : Mode::Lead;
        choose = to == Mode::Lead;
        SetMode(world, generations, to, "resume_rejoined");
        apartSince = 0;
        return true;
    }

    // A member the engine plays apart with nothing else holding the run: REJOIN_SECONDS of that, and the run cannot
    // regroup. A person's member apart holds the pull, and the run waits for it without end. Only in hold: in lead a
    // member apart keeps the pull back, and the blocks at that stop end the run if it never comes back.
    const bool engineApart = std::any_of(apart.begin(), apart.end(), [](const auto& entry) { return !entry.second.human; });
    if (!engineApart || mode != Mode::Hold)
        apartSince = 0;
    else if (!apartSince)
        apartSince = now;
    if (runrules::Overdue(int64_t(apartSince), int64_t(now)))
    {
        std::string who;
        for (auto const& [guid, away] : apart)
            if (!away.human)
                who += (who.empty() ? "" : ",") + world.Name(guid) + (away.blocked.empty() ? "" : ":" + away.blocked);
        Note("run_rejoin", "result=cannot_rejoin apart=" + who);
        failed = true;
        End(world, generations, "cannot_rejoin");
        return true;
    }
    if (mode == Mode::Released && recovering)
        CheckRecovery(world, generations, now);
    return true;
}

bool RunCore::Permit() const
{
    runrules::PermitFacts facts;
    facts.mode = mode;
    facts.leaving = job.leaving;
    facts.apart = !apart.empty();
    facts.failed = failed;
    facts.recovering = recovering;
    return runrules::PullPermit(facts);
}

void RunCore::StopWalk(uint32_t guid, Apart& away)
{
    if (away.walking)
        cancels.push_back(guid);
    away.walking = false;
    away.walk = Walk();
}

void RunCore::ClearApart()
{
    for (auto& [guid, away] : apart)
        StopWalk(guid, away);
    apart.clear();
    hasAnchor = false;
}

void RunCore::BeginHold(RunWorld& world, time_t now)
{
    // Walks of the mode before are the old plan's: the new generation makes their moves stale, and they go.
    for (auto& [guid, away] : apart)
        StopWalk(guid, away);
    apart.clear();
    hasAnchor = false;
    // A supply trip's walk out stops with them; it is planned again from where the party stands once the hold is over.
    if (exiting || exitDone)
    {
        exiting = exitDone = false;
        exitWanted = true;
        exitAsked = 0;
    }
    anchorKind.clear();
    // The cohort: the members alive inside who were not dead a moment ago, so a member just back at the entrance is not
    // taken for the party. Everyone is apart until it reaches the anchor; the cohort walks there within its minute.
    std::vector<uint32_t> cohort;
    for (uint32_t guid : expected)
    {
        const runrules::Facts facts = world.FactsOf(guid);
        auto dead = lastDead.find(guid);
        Apart away;
        away.since = now;
        away.missing = !runrules::Movable(facts);
        away.cohort = runrules::InCohort(facts, dead == lastDead.end() ? 0 : int64_t(dead->second), int64_t(now));
        if (away.cohort)
            cohort.push_back(guid);
        apart[guid] = away;
    }
    // The anchor: the hold place, else the cohort member nearest the current stop's way point whose place is clear.
    Held place;
    if (world.HoldPlace(place))
    {
        anchor = place;
        hasAnchor = true;
        anchorKind = "hold_place";
    }
    else
    {
        Held stopPoint;
        const bool hasStop = world.StopWayPoint(stopPoint);
        float nearest = -1.0f;
        for (uint32_t guid : cohort)
        {
            // On the route's way, so that a member coming back can walk to it, and out of every creature's reach.
            Held at, onWay;
            if (!world.Position(guid, at.x, at.y, at.z) || !world.WayPointNear(at, onWay) || !world.PlaceClear(at))
                continue;
            const float d = hasStop ? runrules::Distance(VecOf(at), VecOf(stopPoint)) : 0.0f;
            if (nearest < 0.0f || d < nearest)
            {
                nearest = d;
                anchor = at;
                hasAnchor = true;
                anchorKind = "cohort_" + world.Name(guid);
            }
        }
    }
    std::string names;
    for (uint32_t guid : cohort)
        names += (names.empty() ? "" : ",") + world.Name(guid);
    Note("run_anchor", hasAnchor ? "kind=" + anchorKind + " at=" + Num(anchor.x) + "," + Num(anchor.y) + "," + Num(anchor.z)
        + " cohort=" + names : "kind=none cohort=" + names);
}

void RunCore::Track(RunWorld& world, const std::vector<uint32_t>& members, time_t now)
{
    // Who is apart: missing, or in lead and released far from the leader. A member back from missing walks to the party.
    Held leaderAt;
    const bool leaderHere = runrules::Movable(world.FactsOf(leader)) && world.Position(leader, leaderAt.x, leaderAt.y, leaderAt.z);
    for (uint32_t guid : expected)
    {
        const runrules::Facts facts = world.FactsOf(guid);
        auto it = apart.find(guid);
        // A person's member is never moved: with the party by where it is, else apart until it comes, holding the pull
        // but never failing the order.
        if (runrules::Human(facts))
        {
            // Where the party is: the anchor in hold, else the leader when the engine plays it inside.
            Held at, from = leaderAt;
            bool hasFrom = leaderHere;
            if (mode == Mode::Hold && hasAnchor)
            {
                from = anchor;
                hasFrom = true;
            }
            const bool located = world.Position(guid, at.x, at.y, at.z);
            const float distance = hasFrom && located ? runrules::Distance(VecOf(at), VecOf(from)) : -1.0f;
            if (runrules::HumanPresent(facts, Contains(members, guid), distance))
            {
                if (it != apart.end())
                {
                    StopWalk(guid, it->second);
                    apart.erase(it);
                    Note("run_rejoin", "member=" + world.Name(guid) + " result=arrived human=1");
                }
                continue;
            }
            if (it == apart.end() || !it->second.human)
            {
                if (it == apart.end())
                {
                    Apart away;
                    away.since = now;
                    it = apart.emplace(guid, away).first;
                }
                Note("run_rejoin", "member=" + world.Name(guid) + " result=away human=1"
                    + (distance >= 0.0f ? " dist=" + Num(distance) : std::string()));
            }
            StopWalk(guid, it->second);
            it->second.human = true;
            it->second.missing = true;
            it->second.cohort = false;
            continue;
        }
        const bool here = runrules::Movable(facts) && Contains(members, guid);
        if (it != apart.end())
            it->second.human = false;
        if (!here)
        {
            if (it == apart.end())
            {
                Apart away;
                away.since = now;
                it = apart.emplace(guid, away).first;
            }
            StopWalk(guid, it->second);
            it->second.missing = true;
            it->second.cohort = false;
            continue;
        }
        if (it != apart.end())
        {
            if (it->second.missing)
            {
                it->second.missing = false;
                it->second.plannedAt = 0;
                Note("run_rejoin", "member=" + world.Name(guid) + " result=back");
            }
            continue;
        }
        Held at;
        if (mode != Mode::Hold && guid != leader && leaderHere && world.Position(guid, at.x, at.y, at.z)
            && runrules::Distance(VecOf(at), VecOf(leaderAt)) > runrules::DISPLACED)
        {
            Apart away;
            away.since = now;
            apart[guid] = away;
            Note("run_rejoin", "member=" + world.Name(guid) + " result=displaced");
        }
    }
    // In lead and released the leader is the party, and never apart from it; on a supply trip's walk out it walks too.
    if (mode != Mode::Hold && !exiting)
    {
        auto self = apart.find(leader);
        if (self != apart.end() && !self->second.missing)
        {
            StopWalk(leader, self->second);
            apart.erase(self);
        }
    }

    // Where the party is: the anchor in hold, set by the leader's place when there was none; in lead and released, the
    // leader's nearest way point.
    Held leaderOnWay;
    if (mode == Mode::Hold && !hasAnchor && leaderHere && world.WayPointNear(leaderAt, leaderOnWay) && world.PlaceClear(leaderAt))
    {
        anchor = leaderAt;
        hasAnchor = true;
        anchorKind = "leader";
        Note("run_anchor", "kind=leader at=" + Num(anchor.x) + "," + Num(anchor.y) + "," + Num(anchor.z));
    }
    if (mode != Mode::Hold && leaderHere)
    {
        Held point;
        if (world.WayPointNear(leaderAt, point))
        {
            leaderWay = point;
            hasLeaderWay = true;
        }
    }
    const bool hasTarget = exiting ? true : mode == Mode::Hold ? hasAnchor : hasLeaderWay;
    const Held target = exiting ? exitPoint : mode == Mode::Hold ? anchor : leaderWay;

    for (auto it = apart.begin(); it != apart.end();)
    {
        const uint32_t guid = it->first;
        Apart& away = it->second;
        Held at;
        if (away.missing || !hasTarget || !world.Position(guid, at.x, at.y, at.z))
        {
            ++it;
            continue;
        }
        const std::string name = world.Name(guid);
        // Arrived: within ARRIVE_NEAR of where the party is, by its walk or already there.
        if (runrules::Distance(VecOf(at), VecOf(target)) <= runrules::ARRIVE_NEAR)
        {
            StopWalk(guid, away);
            if (mode == Mode::Hold)
                held[guid] = at;
            Note("run_rejoin", "member=" + name + " result=arrived at=" + Num(at.x) + "," + Num(at.y) + "," + Num(at.z));
            it = apart.erase(it);
            // The whole party at the exit: the job takes it out for its supplies.
            if (exiting && apart.empty())
            {
                exiting = false;
                exitDone = true;
                Note("supply_trip", "result=at_exit");
            }
            continue;
        }
        // The cohort's minute: not there in time, it stands where it is and walks again later as any member apart.
        if (away.cohort && away.walkSince && now - away.walkSince >= runrules::HOLD_TRAVEL_SECONDS)
        {
            away.cohort = false;
            StopWalk(guid, away);
            away.plannedAt = now;
            away.blocked = "hold_travel_timeout";
            held[guid] = at;
            Note("run_hold_at", "member=" + name + " result=gave_up pos=" + Num(at.x) + "," + Num(at.y) + "," + Num(at.z)
                + " place=" + Num(target.x) + "," + Num(target.y) + "," + Num(target.z)
                + " dist=" + Num(runrules::Distance(VecOf(at), VecOf(target))));
            ++it;
            continue;
        }
        if (away.walking)
        {
            Walk& walk = away.walk;
            // A part's end reached: on to the next, which the member's AI walks once the last one's move is over.
            while (walk.chunk < walk.chunks.size()
                && runrules::Distance(VecOf(at), walk.chunks[walk.chunk].back()) <= runrules::CHUNK_START)
            {
                ++walk.chunk;
                walk.chunkSince = now;
            }
            // A walk to a leader that has gone on is walked to its end first, and then planned again to where the leader is,
            // rather than at every step the leader takes.
            std::string again;
            if (walk.chunk >= walk.chunks.size())
                again = "walked";
            else if (runrules::LinesDistance({ VecOf(at) }, walk.chunks[walk.chunk]) > runrules::WALK_DRIFT)
                again = "drift";
            else if (now - walk.chunkSince >= runrules::WALK_STALL_SECONDS)
                again = "stalled";
            else if (!world.Walking(guid)
                && runrules::Distance(VecOf(at), walk.chunks[walk.chunk].front()) > runrules::CHUNK_START)
                again = "stopped";
            else if (world.Respawned(walk.deadNear))
                again = "respawn";
            else if (world.CreatureNear(walk.chunks[walk.chunk]))
                again = "creature_near";
            if (again.empty())
            {
                ++it;
                continue;
            }
            StopWalk(guid, away);
            away.plannedAt = 0;
            Note("run_rejoin", "member=" + name + " result=replan why=" + again);
        }
        if (now - away.plannedAt < (away.blocked.empty() ? 0 : runrules::REPLAN_SECONDS))
        {
            ++it;
            continue;
        }
        Walk walk;
        std::string why;
        switch (world.PlanWalk(guid, target, walk, why))
        {
            case runrules::PlanResult::Planned:
                walk.id = ++walkIds;
                walk.chunkSince = now;
                away.walk = walk;
                away.walking = true;
                away.plannedAt = now;
                if (!away.walkSince)
                    away.walkSince = now;
                away.blocked.clear();
                Note("run_rejoin", "member=" + name + " result=walking parts=" + std::to_string(walk.chunks.size())
                    + " to=" + Num(target.x) + "," + Num(target.y) + "," + Num(target.z));
                break;
            case runrules::PlanResult::Blocked:
                away.plannedAt = now;
                if (why != away.blocked)
                    Note("run_rejoin", "member=" + name + " result=blocked why=" + why);
                away.blocked = why;
                break;
            default:
                break;
        }
        ++it;
    }

    settled.clear();
    for (uint32_t guid : expected)
    {
        const runrules::Facts facts = world.FactsOf(guid);
        if (!apart.count(guid) && (runrules::Movable(facts) || runrules::Human(facts)) && Contains(members, guid))
            settled.push_back(guid);
    }
}

runrules::Control RunCore::Prepare(RunWorld& world, Generations& generations, bool livingAhead)
{
    hasNext = livingAhead;
    if (mode == Mode::Lead && !livingAhead)
        SetMode(world, generations, Mode::Released, "no_living_pack_ahead");
    return Control();
}

void RunCore::Settle(RunWorld& world, Generations& generations, const runrules::JobReport& report)
{
    job = report;
    if (mode == Mode::Lead && (!report.inside || report.stepDue))
        SetMode(world, generations, Mode::Released, report.stepDue ? "step_due" : "job_not_on_bosses");
    // A recovery is never handed back by the job's report: its obligations end it (CheckRecovery).
    else if (mode == Mode::Released && !recovering && report.inside && !report.stepDue && hasNext)
    {
        // Handed back: to hold when a reason stands, else to lead, and the leader chooses its stop from where it stands.
        if (!holdReasons.empty())
        {
            heldFrom = Mode::Lead;
            held.clear();
            SetMode(world, generations, Mode::Hold, "handed_back," + runrules::Join(holdReasons));
            BeginHold(world, time(nullptr));
            Place(world, expected);
        }
        else
        {
            choose = true;
            SetMode(world, generations, Mode::Lead, "handed_back");
        }
    }
}

void RunCore::TakeBlock(RunWorld& world, Generations& generations, uint32_t id, uint32_t blockGeneration, uint32_t stopKey,
    const std::vector<runrules::Kept>& reasons, const std::string& text, time_t now)
{
    // Once each block, and only one written in this generation in lead: a block of a plan a hold or a new five has since
    // replaced is the old plan's, and the leader gathers again.
    if (mode != Mode::Lead || blockGeneration != generation || reasons.empty() || !runrules::TakeBlock(lastBlock, id))
        return;
    const runrules::BlockOutcome outcome = runrules::CountBlock(blockCounts, stopKey);
    const std::string count = std::to_string(blockCounts[stopKey]) + "/" + std::to_string(runrules::BLOCKS_TO_FAIL);
    if (outcome == runrules::BlockOutcome::Fail)
    {
        Note("run_block", "result=fail stop=" + std::to_string(stopKey) + " count=" + count + " reasons=" + text);
        failed = true;
        End(world, generations, "blocked," + text);
        return;
    }
    Note("run_block", "result=recover stop=" + std::to_string(stopKey) + " count=" + count + " reasons=" + text);
    recovering = true;
    obligations = runrules::Obligations(reasons, int64_t(now));
    recoveryReasons = text;
    exitWanted = std::any_of(reasons.begin(), reasons.end(), [](const runrules::Kept& k) { return k.why == runrules::Keep::Supplies; });
    exiting = exitDone = false;
    exitAsked = 0;
    SetMode(world, generations, Mode::Released, "recovery," + text);
}

void RunCore::PlanExit(RunWorld& world, time_t now)
{
    auto noWay = [&](const std::string& why)
    {
        exitWanted = false;
        runrules::NoSafeWay(obligations, int64_t(now));
        Note("supply_trip", "result=no_safe_way blocker=" + why);
    };
    if (!exitAsked)
        exitAsked = now;
    Held exit;
    if (!world.ExitPoint(exit))
    {
        noWay("no_exit_point");
        return;
    }
    // The leader's way out is checked first; the walk it plans is the one each member is then planned and walks.
    Walk walk;
    std::string why;
    switch (world.PlanWalk(leader, exit, walk, why))
    {
        case runrules::PlanResult::Planned:
        {
            exitWanted = false;
            exiting = true;
            exitPoint = exit;
            std::string names;
            for (uint32_t guid : expected)
            {
                if (!runrules::Movable(world.FactsOf(guid)))
                    continue;
                auto it = apart.find(guid);
                if (it != apart.end())
                    StopWalk(guid, it->second);
                Apart away;
                away.since = now;
                apart[guid] = away;
                names += (names.empty() ? "" : ",") + world.Name(guid);
            }
            Note("supply_trip", "result=walking_out to=" + Num(exit.x) + "," + Num(exit.y) + "," + Num(exit.z) + " members=" + names);
            break;
        }
        case runrules::PlanResult::Blocked:
            noWay(why.empty() ? std::string("blocked") : why);
            break;
        default:
            // The tick's path searches spent: asked again next tick, up to EXIT_PLAN_SECONDS.
            if (int64_t(now - exitAsked) >= runrules::EXIT_PLAN_SECONDS)
                noWay("no_plan_in_time");
            break;
    }
}

void RunCore::CheckRecovery(RunWorld& world, Generations& generations, time_t now)
{
    // Not while the job takes the party out on a supply trip: its obligations are looked at again once it is back.
    if (job.leaving)
        return;
    if (exitWanted)
        PlanExit(world, now);
    std::vector<runrules::Ready> members;
    for (const runrules::Obligation& obligation : obligations)
    {
        runrules::Ready ready;
        ready.guid = obligation.guid;
        ready.facts = world.FactsOf(obligation.guid);
        if (!world.ReadyOf(obligation.guid, leader, ready))
            continue;
        ready.rejoining = apart.count(obligation.guid) > 0;
        members.push_back(ready);
    }
    const runrules::Recovery step = runrules::RecoveryStep(obligations, members, int64_t(now));
    if (step == runrules::Recovery::Going)
        return;
    std::string state;
    for (const runrules::Obligation& obligation : obligations)
    {
        auto it = std::find_if(members.begin(), members.end(), [&](const runrules::Ready& r) { return r.guid == obligation.guid; });
        const bool met = it != members.end() && runrules::Met(obligation, *it);
        state += (state.empty() ? "" : ",") + world.Name(obligation.guid) + ":" + runrules::KeepName(obligation.why)
            + (met ? ":met" : ":unmet");
    }
    Note("run_recovery", std::string("result=") + (step == runrules::Recovery::Done ? "done" : "over") + " obligations=" + state);
    if (exiting)
        ClearApart();
    exitWanted = exiting = exitDone = false;
    recovering = false;
    obligations.clear();
    recoveryReasons.clear();
    // The leader chooses its stop again from where it stands, and gathers with a new budget.
    choose = true;
    SetMode(world, generations, Mode::Lead, step == runrules::Recovery::Done ? "recovered" : "recovery_over");
}

void Failures::Record(uint32_t leader, uint32_t order, const std::string& why)
{
    std::lock_guard<std::mutex> guard(lock);
    failed[leader] = { order, why };
}

bool Failures::Find(uint32_t leader, uint32_t order, std::string& why) const
{
    std::lock_guard<std::mutex> guard(lock);
    auto it = failed.find(leader);
    if (it == failed.end() || it->second.first != order)
        return false;
    why = it->second.second;
    return true;
}

void Failures::Acknowledge(uint32_t leader, uint32_t order)
{
    std::lock_guard<std::mutex> guard(lock);
    auto it = failed.find(leader);
    if (it != failed.end() && it->second.first == order)
        failed.erase(it);
}

uint32_t RunMoves::TokenOf(uint32_t guid) const
{
    std::lock_guard<std::mutex> guard(lock);
    auto it = records.find(guid);
    return it == records.end() ? 0 : it->second.token;
}

bool RunMoves::Find(uint32_t guid, MoveRecord& record) const
{
    std::lock_guard<std::mutex> guard(lock);
    auto it = records.find(guid);
    if (it == records.end())
        return false;
    record = it->second;
    return true;
}

size_t RunMoves::Size() const
{
    std::lock_guard<std::mutex> guard(lock);
    return records.size();
}

void RunMoves::Retire(MoveWorld& world, uint32_t guid, uint32_t token)
{
    std::lock_guard<std::mutex> guard(lock);
    auto it = records.find(guid);
    // A callback may have recorded a new move meanwhile: only this token's record, and only once it is gone.
    if (it != records.end() && it->second.token == token && !world.HasMove(guid, token))
        records.erase(it);
}

void RunMoves::Cancel(MoveWorld& world, uint32_t guid)
{
    const uint32_t token = TokenOf(guid);
    const MoveMember member = world.Member(guid);
    if (!token || !member.visible || !member.inWorld || member.teleporting)
        return;
    if (world.RemoveMove(guid, token) != runrules::Removal::Deferred)
        Retire(world, guid, token);
}

bool RunMoves::Start(MoveWorld& world, uint32_t guid, const runrules::Stamp& stamp, uint32_t leader,
    const std::function<bool()>& current, const std::function<bool(uint32_t token)>& move)
{
    const MoveMember member = world.Member(guid);
    if (!member.visible || !member.inWorld || member.teleporting)
        return false;
    const uint32_t previous = TokenOf(guid);
    // A run move recorded since `previous` that is still in the stack: a removal's callback started it.
    auto replaced = [&]()
    {
        const uint32_t now = TokenOf(guid);
        return now && now != previous && world.HasMove(guid, now);
    };
    std::vector<runrules::Removal> removals;
    if (previous)
    {
        removals.push_back(world.RemoveMove(guid, previous));
        Retire(world, guid, previous);
    }
    if (replaced())
        return false;
    // Any run token no record holds, from a copy of the stack's ids: a removal and its callbacks change the stack.
    const uint32_t kept = TokenOf(guid);
    for (uint32_t id : world.MoveIds(guid))
        if (runrules::IsRunToken(id) && id != previous && id != kept)
            removals.push_back(world.RemoveMove(guid, id));
    if (!runrules::MayIssue(removals) || replaced())
        return false;
    // The removals' callbacks may have moved the member or ended the plan this move was for.
    const MoveMember before = world.Member(guid);
    if (!before.inWorld || before.teleporting || !current())
        return false;

    uint32_t token = 0;
    {
        std::lock_guard<std::mutex> guard(lock);
        token = runrules::NextToken(counter, [&](uint32_t candidate)
        {
            for (auto const& entry : records)
                if (entry.second.token == candidate)
                    return true;
            return world.HasMove(guid, candidate);
        });
    }
    if (!token)
        return false;
    const uint32_t issuedOver = TokenOf(guid);
    const bool moved = move(token);
    // A callback during the move started and recorded a run move of its own that is still in the stack: that one
    // stands, and this one yields. Its removal may run further callbacks, whose records stand too; this call writes no
    // record after a callback, so whatever they leave is the member's run move. A deferred removal goes at the end of the
    // member's update, and its token was never recorded.
    const uint32_t observed = TokenOf(guid);
    if (observed != issuedOver && observed && world.HasMove(guid, observed))
    {
        if (world.HasMove(guid, token))
            world.RemoveMove(guid, token);
        return false;
    }
    // Started only with the token's move on top; a call that moved nothing, or teleported, records nothing.
    const MoveMember after = world.Member(guid);
    if (!moved || !after.inWorld || after.teleporting || world.TopMove(guid) != token)
        return moved;
    // Recorded only over the record this call saw last: nothing between that look and the lock runs a callback, so this
    // only guards the rule.
    std::lock_guard<std::mutex> guard(lock);
    auto it = records.find(guid);
    if ((it == records.end() ? 0 : it->second.token) != observed)
        return false;
    records[guid] = { token, stamp, after.mapId, after.instanceId, leader };
    return true;
}

void RunMoves::Clean(MoveWorld& world, const std::map<uint32_t, RunNow>& runs, uint32_t leader)
{
    std::vector<std::pair<uint32_t, MoveRecord>> snapshot;
    {
        std::lock_guard<std::mutex> guard(lock);
        for (auto const& entry : records)
            if (!leader || entry.second.leader == leader)
                snapshot.push_back(entry);
    }
    for (auto const& [guid, record] : snapshot)
    {
        const MoveMember member = world.Member(guid);
        if (!member.visible)
            continue;
        auto run = runs.find(record.leader);
        runrules::RecordFacts facts;
        facts.runExists = run != runs.end() && run->second.instanceId == record.instanceId
            && run->second.mode != runrules::Mode::Ended;
        facts.stampCurrent = facts.runExists && runrules::Accepts(run->second.mode, run->second.stamp, record.stamp, false);
        facts.memberInRun = facts.runExists && std::find(run->second.expected.begin(), run->second.expected.end(), guid)
            != run->second.expected.end();
        facts.online = member.online;
        facts.engine = member.engine;
        facts.inWorld = member.inWorld;
        facts.teleporting = member.teleporting;
        facts.onRecordInstance = member.inWorld && member.mapId == record.mapId && member.instanceId == record.instanceId;
        facts.tokenInStack = member.online && world.HasMove(guid, record.token);
        facts.reachable = true;
        switch (runrules::RecordStep(facts))
        {
            case runrules::RecordAction::Drop:
            case runrules::RecordAction::Retire:
                Retire(world, guid, record.token);
                break;
            case runrules::RecordAction::Remove:
                if (world.RemoveMove(guid, record.token) != runrules::Removal::Deferred)
                    Retire(world, guid, record.token);
                break;
            default:
                break;
        }
    }
}
