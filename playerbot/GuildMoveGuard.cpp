#include "playerbot/playerbot.h"

#include "playerbot/GuildMoveGuard.h"
#include "playerbot/GuildBotMgr.h"
#include "playerbot/GuildPacks.h"
#include "playerbot/GuildRunState.h"
#include "Entities/Creature.h"
#include "Groups/Group.h"
#include "Maps/Map.h"
#include "Maps/MapPersistentStateMgr.h"
#include "MotionGenerators/MovementGenerator.h"
#include "MotionGenerators/PathFinder.h"
#include "Movement/MoveSpline.h"

#include <limits>
#include <set>
#include <sstream>

namespace
{
    constexpr uint32 WATCH_MS = 250;

    runrules::Vec VecOf(const WorldObject* object)
    {
        return { object->GetPositionX(), object->GetPositionY(), object->GetPositionZ() };
    }

    std::vector<runrules::Vec> Line(const Movement::PointsArray& path)
    {
        std::vector<runrules::Vec> line;
        line.reserve(path.size());
        for (const G3D::Vector3& point : path)
            line.push_back({ point.x, point.y, point.z });
        return line;
    }

    // The moves a bot chooses, which the guard sees; fear, confusion, a fall, a taxi and a distraction are not.
    bool Voluntary(MovementGenerator const* gen)
    {
        if (!gen)
            return true;
        switch (gen->GetMovementGeneratorType())
        {
            case FLEEING_MOTION_TYPE:
            case TIMED_FLEEING_MOTION_TYPE:
            case CONFUSED_MOTION_TYPE:
            case FALL_MOTION_TYPE:
            case TAXI_MOTION_TYPE:
            case DISTRACT_MOTION_TYPE:
            case HOME_MOTION_TYPE:
                return false;
            default:
                return true;
        }
    }

    const char* TypeName(MovementGenerator const* gen)
    {
        if (!gen)
            return "none";
        switch (gen->GetMovementGeneratorType())
        {
            case POINT_MOTION_TYPE: return "point";
            case CHASE_MOTION_TYPE: return "chase";
            case FOLLOW_MOTION_TYPE: return "follow";
            case PATH_MOTION_TYPE: return "path";
            case EFFECT_MOTION_TYPE: return "effect";
            case STAY_MOTION_TYPE: return "stay";
            case RETREAT_MOTION_TYPE: return "retreat";
            default: return "other";
        }
    }

    // A creature's name for a refusal, as A2b names a blocked walk's, with the stop it is of.
    std::string NameOf(const GuildPacks::Route& route, const runguard::GuardThreat& threat)
    {
        return runguard::ThreatName(threat, threat.spawn ? GuildPacks::StopOf(route, threat.spawn) : -1);
    }
}

void GuildMoveGuard::Install()
{
    if (Movement::GetLaunchGuard() != &GuildMoveGuard::OnLaunch)
        Movement::SetLaunchGuard(&GuildMoveGuard::OnLaunch);
}

bool GuildMoveGuard::Guarded(Player* bot, GuildRunView& view) const
{
    if (!bot || !bot->IsInWorld() || bot->IsBeingTeleported() || !bot->IsAlive() || !sGuildBotMgr.IsGuildBot(bot))
        return false;
    PlayerbotAI* ai = bot->GetPlayerbotAI();
    if (!ai)
        return false;
    view = ai->GetAiObjectContext()->GetValue<GuildRunView>("run state")->Get();
    return view.valid && view.mode != GuildRunMode::Ended && view.mapId == bot->GetMapId()
        && view.instanceId == bot->GetInstanceId() && view.packs && view.packs->reach;
}

bool GuildMoveGuard::Guarded(Player* bot) const
{
    GuildRunView view;
    return Guarded(bot, view);
}

std::vector<runguard::GuardThreat> GuildMoveGuard::Threats(Player* bot, const GuildRunView& view,
    const std::vector<runrules::Vec>& path) const
{
    std::vector<runguard::GuardThreat> threats;
    const GuildPacks::Route& route = *view.packs;
    const runguard::ReachIndex& index = *route.reach;
    Map* map = bot->GetMap();
    MapPersistentState* state = map->GetPersistentState();
    const time_t now = time(nullptr);

    // The party, for the current fight: its members and their pets, the player's own among them.
    std::set<ObjectGuid> party = { bot->GetObjectGuid() };
    if (Group* group = bot->GetGroup())
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
            if (Player* member = ref->getSource())
            {
                party.insert(member->GetObjectGuid());
                if (!member->GetPetGuid().IsEmpty())
                    party.insert(member->GetPetGuid());
            }
    if (!bot->GetPetGuid().IsEmpty())
        party.insert(bot->GetPetGuid());
    // In the party's current fight, whoever started it: the run's pull, the job's, a patrol that came, a player's.
    auto inFight = [&party](Creature* creature)
    {
        if (!creature->IsInCombat())
            return false;
        Unit* victim = creature->GetVictim();
        const bool victimMember = victim && party.count(victim->GetObjectGuid());
        bool onList = false;
        for (HostileReference* ref : creature->getThreatManager().getThreatList())
            if (ref && party.count(ref->getUnitGuid()))
            {
                onList = true;
                break;
            }
        return runguard::InCurrentFight(true, onList, victimMember);
    };

    // The plan's own pull: the stop's pack, for the leader in the pull phase, and for a member while the pulled creature is
    // alive, not evading and fighting the party, judged at every check; never the pull's extras or predicted joiners.
    std::set<uint32> pulling;
    {
        const bool inPull = view.mode == GuildRunMode::Lead && view.pull == GuildRunPull::Pull;
        bool pulledFighting = false;
        if (inPull && !view.lead && !view.pulled.IsEmpty())
            if (Creature* pulled = map->GetCreature(view.pulled))
                pulledFighting = pulled->IsAlive() && !pulled->GetCombatManager().IsInEvadeMode() && inFight(pulled);
        if (runguard::PlanExempt(view.lead, inPull, pulledFighting))
            pulling.insert(view.stop.spawns.begin(), view.stop.spawns.end());
    }
    // In released, the leader's job pulls: the pack of its target.
    if (view.lead && view.mode == GuildRunMode::Released)
    {
        Unit* target = bot->GetVictim();
        if (!target)
            if (PlayerbotAI* ai = bot->GetPlayerbotAI())
                target = ai->GetUnit(ai->GetAiObjectContext()->GetValue<ObjectGuid>("current target")->Get());
        if (Creature* creature = target && target->GetTypeId() == TYPEID_UNIT ? static_cast<Creature*>(target) : nullptr)
            if (const runguard::Envelope* own = creature->GetDbGuid() ? index.BySpawn(creature->GetDbGuid()) : nullptr)
            {
                pulling.insert(own->spawn);
                if (own->pack != std::numeric_limits<uint32>::max())
                    for (const runguard::Envelope& e : index.Envelopes())
                        if (e.pack == own->pack)
                            pulling.insert(e.spawn);
            }
    }

    // The route's creatures standing still or wandering whose envelopes reach the path's cells, by the shared reach.
    // Patrols and followers are the live scan's, below, wherever they are.
    for (uint32 i : index.Candidates(path))
    {
        const runguard::Envelope& envelope = index.Envelopes()[i];
        if (runguard::StaticReach(envelope) < 0.0f)
            continue;
        if (state && state->GetCreatureRespawnTime(envelope.spawn) > now)
            continue;
        Creature* creature = map->GetCreature(envelope.spawn);
        runguard::GuardThreat threat;
        if (!creature)
        {
            // Missing from a loaded grid: not up. Not loaded: anywhere in its envelope.
            if (map->IsLoaded(envelope.where[0].x, envelope.where[0].y))
                continue;
            threat = runguard::EnvelopeThreat(envelope, nullptr, 0.0f, 0.0f);
        }
        else
        {
            if (!creature->IsAlive() || creature->GetCombatManager().IsInEvadeMode() || !creature->CanAttackOnSight(bot))
                continue;
            const runrules::Vec at = VecOf(creature);
            // Drawn off its envelope: the live scan has it where it is.
            if (runguard::OffEnvelope(envelope, at))
                continue;
            // The server's aggro radius toward the bot, with ReachPad: the one rule (runguard::PlaceDistance). A wanderer
            // moves even while it pauses; anything with a spline moves now. Out of a fight and on its envelope, it does
            // not leave it: what it reaches is bound by its shared reach. The one model the wait shares (GuardThreatOf).
            threat = GuildPacks::GuardThreatOf(route, creature, bot);
            threat.exempt = inFight(creature) || pulling.count(envelope.spawn);
        }
        if (!threat.where.empty())
            threats.push_back(threat);
    }

    // The live scan: patrols and followers, creatures the model does not have, and modelled ones drawn off their envelopes,
    // each where it is, with its walk over the horizon.
    std::vector<ObjectGuid> found;
    std::vector<runguard::GuardThreat> live = GuildPacks::LiveThreats(route, map, path, bot, bot->GetSpeed(MOVE_RUN), &found);
    for (size_t i = 0; i < live.size(); ++i)
    {
        runguard::GuardThreat& threat = live[i];
        Creature* creature = map->GetCreature(found[i]);
        if (!creature || creature->GetCombatManager().IsInEvadeMode() || !creature->CanAttackOnSight(bot))
            continue;
        threat.exempt = inFight(creature) || (threat.spawn && pulling.count(threat.spawn));
        threats.push_back(threat);
    }
    return threats;
}

bool GuildMoveGuard::Permitted(Player* bot, const runrules::Vec& end) const
{
    std::lock_guard<std::mutex> guard(lock);
    auto it = permits.find(bot->GetGUIDLow());
    if (it == permits.end())
        return false;
    const int64_t now = time(nullptr);
    for (const runguard::Permit& permit : it->second)
        if (runguard::Honours(permit, bot->GetGUIDLow(), end, now))
            return true;
    return false;
}

runguard::GuardResult GuildMoveGuard::Check(Player* bot, const GuildRunView& view, const std::vector<runrules::Vec>& path,
    bool clip, std::string* name, runguard::GuardThreat* blocker) const
{
    runguard::GuardResult result;
    if (path.size() < 2 || Permitted(bot, path.back()))
        return result;
    const std::vector<runguard::GuardThreat> threats = Threats(bot, view, path);
    result = runguard::GuardPath(path, threats, bot->GetSpeed(MOVE_RUN), clip);
    if (result.threat >= 0)
    {
        if (name)
            *name = NameOf(*view.packs, threats[result.threat]);
        if (blocker)
            *blocker = threats[result.threat];
    }
    else if (!bot->IsInCombat() && runguard::Moves(path))
        NoteCheck(bot, path, threats, result);
    return result;
}

bool GuildMoveGuard::PointClear(Player* bot, const GuildRunView& view, const runrules::Vec& at, std::string* name,
    runguard::GuardThreat* blocker) const
{
    if (Permitted(bot, at))
        return true;
    const std::vector<runguard::GuardThreat> threats = Threats(bot, view, { at, at });
    for (const runguard::GuardThreat& threat : threats)
    {
        if (threat.exempt || threat.where.empty())
            continue;
        // As GuardPath judges a point: within its live reach, and within its envelope's shared reach when bound by it.
        const float spread = runguard::SpreadAt(threat, 0.0f, bot->GetSpeed(MOVE_RUN));
        if (spread >= 0.0f && runguard::Reaches(threat, at, spread))
        {
            if (name)
                *name = NameOf(*view.packs, threat);
            if (blocker)
                *blocker = threat;
            return false;
        }
    }
    return true;
}

void GuildMoveGuard::NoteCheck(Player* bot, const std::vector<runrules::Vec>& path,
    const std::vector<runguard::GuardThreat>& threats, const runguard::GuardResult& result) const
{
    // The margin GuardPath judged by and the creature it belongs to, with the server's own margin beside it.
    const std::string text = runguard::CheckText(path, threats, result);
    std::lock_guard<std::mutex> guard(lock);
    LastCheck& last = lastChecks[bot->GetGUIDLow()];
    last.at = time(nullptr);
    last.text = text;
}

std::string GuildMoveGuard::LastCheckOf(uint32 guid, int64_t& at) const
{
    std::lock_guard<std::mutex> guard(lock);
    auto it = lastChecks.find(guid);
    if (it == lastChecks.end())
    {
        at = 0;
        return "";
    }
    at = it->second.at;
    return it->second.text;
}

bool GuildMoveGuard::OnLaunch(Unit& unit, Movement::PointsArray const& path, Movement::LaunchKind kind)
{
    if (!unit.IsPlayer() || path.size() < 2)
        return true;
    Player* bot = static_cast<Player*>(&unit);
    GuildMoveGuard& self = instance();
    MovementGenerator const* calling = bot->GetMotionMaster()->GetCalling();
    Unit* target = calling ? calling->GetCurrentTarget() : nullptr;
    const std::vector<runrules::Vec> line = Line(path);
    const ObjectGuid targetGuid = target ? target->GetObjectGuid() : ObjectGuid();
    std::ostringstream detail;
    detail << (kind == Movement::LaunchKind::Teleport ? "teleport" : "spline") << " gen=" << TypeName(calling);

    // The self-test's own refusal, for a lab character with no run.
    {
        std::function<bool(const std::vector<runrules::Vec>&)> test;
        {
            std::lock_guard<std::mutex> guard(self.lock);
            auto it = self.tests.find(bot->GetGUIDLow());
            if (it != self.tests.end())
                test = it->second;
        }
        if (test)
        {
            if (!test(line))
                return true;
            self.NoteRefusal(bot, targetGuid, &line.back(), detail.str() + " test");
            return false;
        }
    }

    if (!Voluntary(calling))
        return true;
    GuildRunView view;
    if (!self.Guarded(bot, view))
        return true;
    std::string name;
    runguard::GuardThreat blocker;
    bool allowed;
    if (kind == Movement::LaunchKind::Teleport)
        allowed = self.PointClear(bot, view, line.back(), &name, &blocker);
    else
        allowed = self.Check(bot, view, line, false, &name, &blocker).verdict == runguard::Verdict::Allow;
    if (allowed)
    {
        // Only a move that moves ends a refusal: a facing or a fall launches too, and accepts nothing.
        if (!runguard::Moves(line))
            return true;
        std::lock_guard<std::mutex> guard(self.lock);
        auto it = self.records.find(bot->GetGUIDLow());
        if (it != self.records.end())
            runguard::Accepted(it->second.refusal, time(nullptr));
        return true;
    }
    detail << " by=" << name;
    self.NoteRefusal(bot, targetGuid, &line.back(), detail.str(), &blocker);
    return false;
}

uint64 GuildMoveGuard::PlanOf(Player* bot, size_t* stop, bool* lead) const
{
    GuildRunView view;
    const bool guarded = Guarded(bot, view);
    if (stop)
        *stop = guarded ? view.index : SIZE_MAX;
    if (lead)
        *lead = guarded && view.lead;
    return guarded ? runguard::PlanKey(view.stamp.generation, view.stamp.plan) : 0;
}

bool GuildMoveGuard::Refuses(Player* bot, ObjectGuid target, const runrules::Vec* dest) const
{
    const uint64 plan = PlanOf(bot);
    std::lock_guard<std::mutex> guard(lock);
    auto it = records.find(bot->GetGUIDLow());
    return it != records.end() && runguard::Refuses(it->second.refusal, target.GetRawValue(), dest, time(nullptr), plan);
}

void GuildMoveGuard::NoteRefusal(Player* bot, ObjectGuid target, const runrules::Vec* dest, const std::string& detail,
    const runguard::GuardThreat* blocker)
{
    // The run's plan, stop and the bot's part in it first, with no lock held: reading the view may take a run state's.
    size_t stop = SIZE_MAX;
    bool lead = false;
    const uint64 plan = PlanOf(bot, &stop, &lead);
    const uint32 spawn = blocker ? blocker->spawn : 0;
    const bool live = blocker && blocker->live;
    std::ostringstream text;
    text << detail << " who=" << (lead ? "leader" : "member") << " stop=" << (stop == SIZE_MAX ? 0 : stop + 1)
         << " spawn=" << spawn << " live=" << (live ? 1 : 0);
    if (blocker && !blocker->where.empty())
    {
        text.setf(std::ios::fixed);
        text.precision(1);
        text << " at=" << blocker->where.front().x << "," << blocker->where.front().y << "," << blocker->where.front().z;
    }
    std::lock_guard<std::mutex> guard(lock);
    Record& record = records[bot->GetGUIDLow()];
    const bool opening = !record.refusal.open;
    runguard::Note(record.refusal, target.GetRawValue(), dest, spawn, time(nullptr), plan, stop, live);
    if (opening)
        record.detail = text.str();
}

runguard::Refusal GuildMoveGuard::RefusalOf(uint32 guid) const
{
    std::lock_guard<std::mutex> guard(lock);
    auto it = records.find(guid);
    return it == records.end() ? runguard::Refusal() : it->second.refusal;
}

void GuildMoveGuard::Grant(const runguard::Permit& permit, const std::string& detail)
{
    std::lock_guard<std::mutex> guard(lock);
    std::vector<runguard::Permit>& own = permits[permit.subject];
    own.erase(std::remove_if(own.begin(), own.end(), [&](const runguard::Permit& p)
        { return p.kind == permit.kind && p.patient == permit.patient; }), own.end());
    own.push_back(permit);
    pendingEvents.emplace_back(permit.subject, "guard_override", detail);
}

bool GuildMoveGuard::HasPermit(uint32 guid, runguard::PermitKind kind, uint32 patient) const
{
    std::lock_guard<std::mutex> guard(lock);
    auto it = permits.find(guid);
    if (it == permits.end())
        return false;
    const int64_t now = time(nullptr);
    return std::any_of(it->second.begin(), it->second.end(), [&](const runguard::Permit& p)
        { return p.kind == kind && (!patient || p.patient == patient) && now < p.expires; });
}

void GuildMoveGuard::GrantPortal(Player* bot, float x, float y, float z, float radius)
{
    GuildRunView view;
    if (!Guarded(bot, view))
        return;
    Grant(runguard::PortalPermit(bot->GetGUIDLow(), view.stamp.generation, view.stamp.plan, { x, y, z }, radius, time(nullptr)),
        "portal " + std::string(bot->GetName()));
}

void GuildMoveGuard::GrantSurface(Player* bot, float x, float y, float z)
{
    GuildRunView view;
    if (!Guarded(bot, view))
        return;
    Grant(runguard::SurfacePermit(bot->GetGUIDLow(), view.stamp.generation, view.stamp.plan, { x, y, z }, time(nullptr)),
        "surface " + std::string(bot->GetName()));
}

void GuildMoveGuard::SetTestRefusal(uint32 guid, std::function<bool(const std::vector<runrules::Vec>&)> refuse)
{
    std::lock_guard<std::mutex> guard(lock);
    if (refuse)
        tests[guid] = std::move(refuse);
    else
    {
        tests.erase(guid);
        records.erase(guid);
    }
}

std::vector<std::tuple<uint32, std::string, std::string>> GuildMoveGuard::Watch(uint32 elapsed)
{
    std::vector<std::tuple<uint32, std::string, std::string>> events;
    Install();
    sinceWatch += elapsed;
    if (sinceWatch < WATCH_MS)
        return events;
    sinceWatch = 0;
    const int64_t now = time(nullptr);

    for (auto const& [guid, player] : sObjectAccessor.GetPlayers())
    {
        Player* bot = player;
        GuildRunView view;
        if (!Guarded(bot, view))
            continue;
        const uint32 low = bot->GetGUIDLow();

        // Permits that ran out: every move after them is guarded again.
        {
            std::vector<runguard::Permit> own;
            {
                std::lock_guard<std::mutex> guard(lock);
                auto it = permits.find(low);
                if (it != permits.end())
                    own = it->second;
            }
            if (!own.empty())
            {
                std::vector<runguard::Permit> kept;
                for (const runguard::Permit& permit : own)
                {
                    runguard::PermitFacts facts;
                    facts.at = VecOf(bot);
                    facts.generation = view.stamp.generation;
                    facts.revision = view.stamp.plan;
                    facts.now = now;
                    facts.breathing = bot->IsUnderwater();
                    PlayerbotAI* ai = bot->GetPlayerbotAI();
                    facts.fight = ai && bot->IsInCombat() ? uint64(ai->GetCombatStartTime()) : 0;
                    if (permit.patient)
                    {
                        Player* patient = sObjectMgr.GetPlayer(ObjectGuid(HIGHGUID_PLAYER, permit.patient));
                        facts.patientAlive = patient && patient->IsAlive();
                        facts.patientHealth = patient ? uint32(patient->GetHealthPercent()) : 0;
                    }
                    const runguard::PermitEnd end = runguard::Expired(permit, facts);
                    if (end == runguard::PermitEnd::None)
                        kept.push_back(permit);
                }
                std::lock_guard<std::mutex> guard(lock);
                if (kept.empty())
                    permits.erase(low);
                else
                    permits[low] = kept;
            }
        }

        // The running spline, from where the bot is to its end, against what came into its way since it launched.
        MotionMaster* motion = bot->GetMotionMaster();
        MovementGenerator const* running = motion->empty() ? nullptr : motion->GetCurrent();
        if (!bot->movespline->Finalized() && running && Voluntary(running)
            && running->GetMovementGeneratorType() != IDLE_MOTION_TYPE)
        {
            // The points still ahead: from the segment under way's end, raw index GetRawPathIndex() + 1, to the last.
            const auto& points = bot->movespline->GetPathPoints();
            std::vector<runrules::Vec> rest = { VecOf(bot) };
            const int32 from = std::max<int32>(bot->movespline->GetRawPathIndex() + 1, 0);
            for (int32 i = from; i < int32(points.size()); ++i)
                rest.push_back({ points[i].x, points[i].y, points[i].z });
            std::string name;
            runguard::GuardThreat blocker;
            if (rest.size() >= 2 && Check(bot, view, rest, false, &name, &blocker).verdict != runguard::Verdict::Allow)
            {
                Unit* target = running->GetCurrentTarget();
                const ObjectGuid targetGuid = target ? target->GetObjectGuid() : ObjectGuid();
                const std::string detail = std::string("watch gen=") + TypeName(running) + " by=" + name;
                // Outside every lock: the removal finalizes it and resets what it uncovers, which may launch again.
                motion->RemoveGenerator(running);
                if (!bot->movespline->Finalized())
                    bot->InterruptMoving();
                NoteRefusal(bot, targetGuid, &rest.back(), detail, &blocker);
            }
        }
    }

    // The refusals' starts and ends, and the permits granted, as events.
    std::lock_guard<std::mutex> guard(lock);
    for (auto& [guid, record] : records)
        switch (runguard::RecordStep(record.refusal, record.told, now))
        {
            case runguard::RecordEvent::Start:
                events.emplace_back(guid, "guard_refused", "start " + record.detail);
                break;
            case runguard::RecordEvent::End:
                events.emplace_back(guid, "guard_refused", "end");
                break;
            default:
                break;
        }
    for (auto& event : pendingEvents)
        events.push_back(std::move(event));
    pendingEvents.clear();
    return events;
}

std::list<std::string> GuildMoveGuard::Describe(const std::string& args) const
{
    std::istringstream words(args);
    std::string name;
    float x = 0.0f, y = 0.0f, z = 0.0f;
    if (!(words >> name >> x >> y >> z))
        return { "guildbot run guard <character> <x> <y> <z>   the movement guard's verdict on its path there; reads only" };
    Player* bot = ObjectAccessor::FindPlayerByName(name.c_str());
    if (!bot || !bot->IsInWorld())
        return { name + " is not in the world" };
    GuildRunView view;
    if (!Guarded(bot, view))
        return { name + " is not guarded: not a living guild character in its run's instance" };

    // The navmesh path, as a move there would take it. The console may path; the AI never paths for the guard.
    PathFinder finder(bot);
    finder.calculate(x, y, z);
    std::vector<runrules::Vec> path;
    for (const G3D::Vector3& point : finder.getPath())
        path.push_back({ point.x, point.y, point.z });
    if (path.empty())
        return { "no path from " + name + " to there" };
    path.front() = VecOf(bot);

    std::string by;
    const runguard::GuardResult result = Check(bot, view, path, true, &by);
    std::ostringstream out;
    out.setf(std::ios::fixed);
    out.precision(1);
    out << name << ": path of " << path.size() << " points, " << runrules::Length(path) << " yd: ";
    switch (result.verdict)
    {
        case runguard::Verdict::Allow:
            out << "allowed";
            break;
        case runguard::Verdict::Clip:
            out << "clipped at " << result.clipAlong << " yd, by " << by << " at " << result.along << " yd";
            break;
        case runguard::Verdict::Refuse:
            out << "refused by " << by << " at " << result.along << " yd (" << result.at.x << " " << result.at.y << " "
                << result.at.z << ")" << (result.endInReach ? ", its end in reach" : "");
            break;
    }
    return { out.str() };
}
