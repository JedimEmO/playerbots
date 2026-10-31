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

void RunCore::Cancel(RunWorld& world)
{
    for (uint32_t guid : expected)
        if (runrules::Movable(world.FactsOf(guid)))
            world.CancelMotion(guid);
}

void RunCore::SetMode(RunWorld& world, Generations& generations, Mode to, const std::string& why)
{
    // Every change of mode is a new generation, and stops what the old one started before anything of the new one moves.
    mode = to;
    reason = why;
    generation = generations.Next();
    Cancel(world);
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
    choose = false;
    endSeen = false;
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

    // A new five: the old roster's moves stop first, the departed let go of their places, then the five are expected.
    const runrules::Roster roster = runrules::RosterChange(expected, members);
    if (roster == runrules::Roster::Replace)
    {
        Cancel(world);
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
    // The job's departure on purpose is exempt only while the job has the party for it, or the hold began there.
    const bool leaving = job.leaving && (mode == Mode::Released || (mode == Mode::Hold && heldFrom == Mode::Released));
    holdReasons = runrules::HoldReasons(world.FactsOf(leader), facts, leaving);

    if (!holdReasons.empty())
    {
        leaderBackAt = 0;
        const std::string why = runrules::Join(holdReasons);
        if (mode != Mode::Hold)
        {
            heldFrom = mode;
            held.clear();
            choose = false;
            SetMode(world, generations, Mode::Hold, why);
        }
        else if (reason != why)
        {
            reason = why;
            Note("run_mode", "mode=hold reason=" + why);
        }
        Place(world, members);
        return true;
    }
    if (mode != Mode::Hold)
        return true;

    // Every reason gone: the leader alive inside and the engine's, the group the expected five. The run resumes once the
    // leader is near a member's place, or a while after it came back.
    Place(world, members);
    if (!leaderBackAt)
        leaderBackAt = now;
    float nearest = -1.0f;
    float lx, ly, lz;
    if (world.Position(leader, lx, ly, lz))
        for (auto const& [guid, place] : held)
        {
            if (guid == leader)
                continue;
            const float dx = place.x - lx, dy = place.y - ly, dz = place.z - lz;
            const float distance = float(std::sqrt(dx * dx + dy * dy + dz * dz));
            if (nearest < 0.0f || distance < nearest)
                nearest = distance;
        }
    if (!runrules::Resumes(true, nearest, int64_t(now - leaderBackAt)))
        return true;
    held.clear();
    leaderBackAt = 0;
    const Mode to = heldFrom == Mode::Released ? Mode::Released : Mode::Lead;
    choose = to == Mode::Lead;
    SetMode(world, generations, to, nearest >= 0.0f && nearest <= runrules::RESUME_NEAR ? "resume_near" : "resume_waited");
    return true;
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
    else if (mode == Mode::Released && report.inside && !report.stepDue && hasNext)
    {
        // Handed back: to hold when a reason stands, else to lead, and the leader chooses its stop from where it stands.
        if (!holdReasons.empty())
        {
            heldFrom = Mode::Lead;
            held.clear();
            SetMode(world, generations, Mode::Hold, "handed_back," + runrules::Join(holdReasons));
            Place(world, expected);
        }
        else
        {
            choose = true;
            SetMode(world, generations, Mode::Lead, "handed_back");
        }
    }
}
