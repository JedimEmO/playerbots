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
