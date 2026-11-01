#pragma once

#include "Common.h"
#include "Entities/ObjectGuid.h"
#include "Movement/LaunchGuard.h"
#include "playerbot/GuildRunGuard.h"

#include <functional>
#include <list>
#include <map>
#include <mutex>
#include <string>
#include <tuple>
#include <vector>

class Player;
class Unit;
struct GuildRunView;

// The movement guard of a party's run (plan item 4a; the rules are runguard's, in GuildRunGuard.h). A guild bot the engine
// plays, alive in its run's instance while the run is not ended, never starts or keeps a move whose executed path or end
// enters the reach of a living creature outside the current fight.
//
// The core asks it about every player's spline as it launches and every near teleport (Movement::SetLaunchGuard): a
// refusal launches nothing, and the core removes the generator that asked. The AI asks it earlier and more cheaply, in
// MoveTo2 on the path the move resolved, and before a chase or a follow. The guild tick asks it again every 250 ms for each
// running spline, for creatures that walk into a move already under way. A creature fighting the party, whoever started the
// fight, is never in the way (runguard::InCurrentFight); nor the pack the plan is pulling, for the leader in the pull
// phase and for a member while the pulled creature fights the party (runguard::PlanExempt); nor, for the leader in
// released, the pack of its job's target.
//
// One rule decides whether a creature reaches a point, for the route and the guard alike (runguard::PlaceDistance): the 3D
// distance between centres against the server's aggro radius, both bounding radii and THREAT_MARGIN, with a creature's
// movement (a wanderer's range, a live mover's walk over the first 20 yards of the path) spreading its places across
// only, never up or down. A creature standing still or wandering is judged by its envelope's shared reach
// (runguard::EnvelopeThreat), the one the route plans its spots and stands by, so a place the route certified is never
// refused by one on its envelope. A patrol and a follower are judged where they stand, bound by their loop, so a place
// the route planned clear of the whole loop is passed too; a creature drawn off its envelope or not in the model is
// judged where it stands, unbound. A refusal by any of these is `live`, and readiness waits for it as guarded.
//
// It reads the bot's run view (the AI's cached `run state`), the route's reach index and the bot's own map, so it runs on
// the bot's map thread or on the world thread between map updates. Its records and permits have their own lock, held
// only to copy. Lock order: the guard's lock is never taken while a run state's lock is held, and nothing done under the
// guard's lock reads `run state` or takes any other lock; readiness copies a record (RefusalOf) with no run state lock held.
class GuildMoveGuard
{
public:
    static GuildMoveGuard& instance()
    {
        static GuildMoveGuard guard;
        return guard;
    }

    // Sets the core's launch guard, once.
    void Install();

    // Whether a bot's moves are guarded, with its run view.
    bool Guarded(Player* bot, GuildRunView& view) const;
    bool Guarded(Player* bot) const;

    // A path's verdict for a guarded bot, its first point the bot's place: the route's envelopes with live facts, the live
    // scan's creatures, the bot's exemptions, and its permits. `name` gets the creature that refuses or clips it, and
    // `blocker` the creature itself, as the guard judged it.
    runguard::GuardResult Check(Player* bot, const GuildRunView& view, const std::vector<runrules::Vec>& path, bool clip,
        std::string* name = nullptr, runguard::GuardThreat* blocker = nullptr) const;
    // Whether a place is out of every reach, bound by the envelope as a path is: for a teleport's landing, a chase's end
    // and a healer's spot. `name` and `blocker` as Check's.
    bool PointClear(Player* bot, const GuildRunView& view, const runrules::Vec& at, std::string* name = nullptr,
        runguard::GuardThreat* blocker = nullptr) const;

    // The AI's gates: whether this target or destination was refused in the last few seconds.
    bool Refuses(Player* bot, ObjectGuid target, const runrules::Vec* dest) const;
    // A refusal the AI or the watcher made, kept for the AI's gates, readiness and the triggers' holding, and told by the
    // tick. Its plan and stop are the bot's run's; `blocker` names the creature and whether it is live.
    void NoteRefusal(Player* bot, ObjectGuid target, const runrules::Vec* dest, const std::string& detail,
        const runguard::GuardThreat* blocker = nullptr);
    // A copy of a bot's refusal record, under the guard's lock only, for readiness, the leader's budget and the triggers'
    // holding. Empty for a bot never refused.
    runguard::Refusal RefusalOf(uint32 guid) const;
    // The bot's last guard check made out of a fight, for a fight it walked into: when, and what it found nearest, with that
    // creature's place and margin. Empty when there is none.
    std::string LastCheckOf(uint32 guid, int64_t& at) const;

    // Whether one of the bot's permits lets a move ending there through.
    bool Permitted(Player* bot, const runrules::Vec& end) const;
    // A scoped exception: a healer's reach to its patient, the way out through a portal, surfacing for breath.
    void Grant(const runguard::Permit& permit, const std::string& detail);
    bool HasPermit(uint32 guid, runguard::PermitKind kind, uint32 patient = 0) const;
    // A portal's permit, to an area trigger, and surfacing's, straight up: for a guarded bot, under its run's stamp.
    void GrantPortal(Player* bot, float x, float y, float z, float radius);
    void GrantSurface(Player* bot, float x, float y, float z);

    // The guild tick, on the world thread between map updates: every 250 ms each guarded bot's running spline, against
    // creatures that came into its way since it launched; permits that ran out; the refusals' starts and ends as events.
    std::vector<std::tuple<uint32, std::string, std::string>> Watch(uint32 elapsed);

    // `guildbot run guard <name> <x> <y> <z>`: the verdict on the named bot's path there, by the navmesh. Reads only.
    std::list<std::string> Describe(const std::string& args) const;

    // For `run selftest`: a check of the test's own that refuses a lab character's moves, whether or not it is in a run.
    void SetTestRefusal(uint32 guid, std::function<bool(const std::vector<runrules::Vec>&)> refuse);

private:
    GuildMoveGuard() = default;

    static bool OnLaunch(Unit& unit, Movement::PointsArray const& path, Movement::LaunchKind kind);
    // The bot's run generation and plan revision, for its refusals; 0 outside a run. `stop` gets the run's stop, SIZE_MAX
    // outside a run, and `lead` whether it leads.
    uint64 PlanOf(Player* bot, size_t* stop = nullptr, bool* lead = nullptr) const;
    // Keeps the margin an allowed check out of a fight was judged by, and its creature (runguard::CheckText), for LastCheckOf.
    void NoteCheck(Player* bot, const std::vector<runrules::Vec>& path, const std::vector<runguard::GuardThreat>& threats,
        const runguard::GuardResult& result) const;
    // The threats in a path's way for this bot, from the route's index and the live scan, each marked exempt or not.
    std::vector<runguard::GuardThreat> Threats(Player* bot, const GuildRunView& view, const std::vector<runrules::Vec>& path)
        const;

    struct Record
    {
        runguard::Refusal refusal;
        uint32 told = 0;
        std::string detail;
    };

    struct LastCheck
    {
        int64_t at = 0;
        std::string text;
    };

    mutable std::mutex lock;
    std::map<uint32, Record> records;
    mutable std::map<uint32, LastCheck> lastChecks;
    std::map<uint32, std::vector<runguard::Permit>> permits;
    std::map<uint32, std::function<bool(const std::vector<runrules::Vec>&)>> tests;
    std::vector<std::tuple<uint32, std::string, std::string>> pendingEvents;  // permits granted, told on the next watch
    uint32 sinceWatch = 0;
};

#define sGuildMoveGuard GuildMoveGuard::instance()
