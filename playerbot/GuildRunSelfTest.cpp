#include "playerbot/playerbot.h"

#include "playerbot/GuildRunSelfTest.h"
#include "playerbot/GuildBotMgr.h"
#include "playerbot/GuildRunState.h"
#include "playerbot/GuildMoveGuard.h"
#include "Movement/MoveSplineInit.h"
#include "Accounts/AccountMgr.h"
#include "MotionGenerators/MovementGenerator.h"
#include "Movement/MoveSpline.h"
#include "Entities/Creature.h"

#include <algorithm>
#include <cstdlib>
#include <functional>
#include <iterator>
#include <memory>
#include <sstream>

namespace
{
    // What happened to a test generator, kept apart from it, so that it can be read after the generator is deleted.
    struct Trace
    {
        int resets = 0;
        int finalizes = 0;
        int interrupts = 0;
        int updates = 0;
        bool deleted = false;
    };

    // A generator of the test's own: a point move by type, with an id, and what it does on update and finalize.
    class TestMove : public MovementGenerator
    {
    public:
        TestMove(uint32 id, std::shared_ptr<Trace> trace) : id(id), trace(std::move(trace)) {}
        ~TestMove() override { trace->deleted = true; }

        void Initialize(Unit& unit) override
        {
            if (onInitialize)
                onInitialize(unit);
        }
        void Finalize(Unit& unit) override
        {
            ++trace->finalizes;
            if (onFinalize)
                onFinalize(unit);
        }
        void Interrupt(Unit&) override { ++trace->interrupts; }
        void Reset(Unit& unit) override
        {
            ++trace->resets;
            if (onReset)
                onReset(unit);
        }
        bool Update(Unit& unit, const uint32&) override
        {
            ++trace->updates;
            return onUpdate ? onUpdate(unit) : true;
        }
        MovementGeneratorType GetMovementGeneratorType() const override { return POINT_MOTION_TYPE; }
        uint32 GetMoveId() const override { return id; }

        std::function<bool(Unit&)> onUpdate;
        std::function<void(Unit&)> onFinalize;
        std::function<void(Unit&)> onInitialize;
        std::function<void(Unit&)> onReset;

    private:
        uint32 id;
        std::shared_ptr<Trace> trace;
    };

    // Ids for the test's own moves, in the run's namespace and far from the counter's start.
    constexpr uint32 T1 = runrules::RUN_TOKEN_BIT | 0x7FFFFF01u;
    constexpr uint32 T2 = runrules::RUN_TOKEN_BIT | 0x7FFFFF02u;
    constexpr uint32 T3 = runrules::RUN_TOKEN_BIT | 0x7FFFFF03u;
    constexpr uint32 T5 = runrules::RUN_TOKEN_BIT | 0x7FFFFF05u;
    constexpr uint32 T9 = runrules::RUN_TOKEN_BIT | 0x7FFFFF09u;

    std::string Stack(Player* bot)
    {
        std::ostringstream out;
        bool first = true;
        for (MovementGenerator const* generator : *bot->GetMotionMaster())
        {
            out << (first ? "" : ",") << uint32(generator->GetMovementGeneratorType());
            if (generator->GetMoveId())
                out << ":" << std::hex << generator->GetMoveId() << std::dec;
            first = false;
        }
        return out.str();
    }

    bool LabAccount(Player* player)
    {
        std::string account;
        return sAccountMgr.GetName(sObjectMgr.GetPlayerAccountIdByGUID(player->GetObjectGuid()), account)
            && (account.rfind("LAB", 0) == 0 || account.rfind("GBENCH", 0) == 0) && account != "LABGM";
    }

    std::string Refusal(Player* player, const std::string& name)
    {
        if (!player)
            return name + " is not online";
        if (!LabAccount(player))
            return name + " is not on a lab or bench account";
        if (sGuildBotMgr.GetPlayerBot(player->GetGUIDLow()) != player)
            return name + " is not played by the engine";
        if (!player->IsInWorld() || player->IsBeingTeleported() || !player->IsAlive() || player->IsInCombat())
            return name + " is not alive, settled and out of a fight";
        // A rooted or stunned character's motion is not updated, and the cases that update it would run nothing.
        if (player->hasUnitState(UNIT_STAT_CAN_NOT_MOVE))
            return name + " cannot move";
        return "";
    }
}

std::list<std::string> GuildRunSelfTest(const std::string& args)
{
    std::istringstream words(args);
    std::string name, targetName;
    words >> name >> targetName;
    // patrol=<spawn>: a patrol of the character's run, for the live guard's case.
    uint32 patrolSpawn = 0;
    for (std::string word; words >> word;)
        if (word.rfind("patrol=", 0) == 0)
            patrolSpawn = uint32(std::strtoul(word.c_str() + 7, nullptr, 10));
    if (name.empty() || targetName.empty())
        return { "guildbot run selftest <character> <second character> [patrol=<spawn>]   lab or bench characters only; the "
            "second is chased; with patrol=, the first in a run in lead, near that patrol of its route" };
    Player* bot = ObjectAccessor::FindPlayerByName(name.c_str());
    Player* target = ObjectAccessor::FindPlayerByName(targetName.c_str());
    for (const std::string& why : { Refusal(bot, name), Refusal(target, targetName) })
        if (!why.empty())
            return { why };
    if (bot == target || bot->GetMap() != target->GetMap() || bot->GetDistance(target) > 30.0f)
        return { name + " and " + targetName + " must be two characters within 30 yards of each other" };

    MotionMaster* motion = bot->GetMotionMaster();
    std::list<std::string> out;
    uint32 passed = 0, failed = 0;

    // A lab character in a live run is guarded, and the live guard would refuse the test's own moves wherever a pack
    // reaches them. For the whole self-test its moves are judged by the test's own check instead, which allows everything
    // but a case's own refusal, and the live guard is back on every way out.
    const uint32 low = bot->GetGUIDLow();
    const std::function<bool(const std::vector<runrules::Vec>&)> allowAll = [](const std::vector<runrules::Vec>&) { return false; };
    sGuildMoveGuard.Install();
    sGuildMoveGuard.SetTestRefusal(low, allowAll);
    struct Restore
    {
        uint32 guid;
        ~Restore() { sGuildMoveGuard.SetTestRefusal(guid, nullptr); }
    } restore{ low };
    auto check = [&](const std::string& what, bool ok, const std::string& before)
    {
        ok ? ++passed : ++failed;
        out.push_back(std::string(ok ? "pass " : "fail ") + what + "  before=" + before + "  after=" + Stack(bot));
    };
    // Before each case, and at its end, while the locals its test moves' callbacks reach are still there: a move that
    // outlives its case would call into them from the next one.
    auto fresh = [&]()
    {
        motion->Initialize();
    };
    auto near = [&](float yards, float& x, float& y, float& z)
    {
        x = bot->GetPositionX() + yards;
        y = bot->GetPositionY();
        z = bot->GetPositionZ();
    };
    auto pushTest = [&](uint32 id, std::shared_ptr<Trace> trace) -> TestMove*
    {
        TestMove* move = new TestMove(id, trace);
        motion->MoveGenerator(move);
        return move;
    };
    auto topId = [&]() { return motion->empty() ? 0u : motion->GetCurrent()->GetMoveId(); };
    auto topType = [&]() { return motion->empty() ? MovementGeneratorType(0) : motion->GetCurrentMovementGeneratorType(); };
    float x, y, z;
    near(5.0f, x, y, z);
    // The self-test's characters have no run: a run move's check of its run always passes here.
    const std::function<bool()> always = []() { return true; };

    // 1. A run move on top is removed, and the stack is as before it.
    {
        fresh();
        motion->MovePoint(T1, x, y, z, FORCED_MOVEMENT_RUN, false);
        const std::string before = Stack(bot);
        const bool removed = motion->RemoveMove(T1) == MotionMaster::RemoveResult::Removed;
        check("on_top", removed && !motion->HasMove(T1) && topType() == IDLE_MOTION_TYPE, before);
        fresh();
    }
    // 2. Beneath a chase: removed, the chase on top, its spline untouched.
    {
        fresh();
        motion->MovePoint(T1, x, y, z, FORCED_MOVEMENT_RUN, false);
        motion->MoveChase(target);
        const uint32 spline = bot->movespline->GetId();
        const std::string before = Stack(bot);
        const bool removed = motion->RemoveMove(T1) == MotionMaster::RemoveResult::Removed;
        check("beneath_chase", removed && !motion->HasMove(T1) && topType() == CHASE_MOTION_TYPE
            && bot->movespline->GetId() == spline, before);
        fresh();
    }
    // 3. Beneath another run move: removed, the other untouched.
    {
        fresh();
        motion->MovePoint(T1, x, y, z, FORCED_MOVEMENT_RUN, false);
        motion->MovePoint(T2, x, y + 3.0f, z, FORCED_MOVEMENT_RUN, false);
        const uint32 spline = bot->movespline->GetId();
        const std::string before = Stack(bot);
        const bool removed = motion->RemoveMove(T1) == MotionMaster::RemoveResult::Removed;
        check("beneath_point", removed && !motion->HasMove(T1) && topId() == T2 && bot->movespline->GetId() == spline, before);
        fresh();
    }
    // 4. Not one more: the chase beneath a removed top survives, where MovementExpired would take it too.
    {
        fresh();
        motion->MoveChase(target);
        motion->MovePoint(T1, x, y, z, FORCED_MOVEMENT_RUN, false);
        const std::string before = Stack(bot);
        const bool removed = motion->RemoveMove(T1) == MotionMaster::RemoveResult::Removed;
        check("chase_survives", removed && !motion->HasMove(T1) && topType() == CHASE_MOTION_TYPE, before);
        fresh();
    }
    // 5. Reentrant, deferred: the top's update removes a buried token and expires; the token goes before it can be
    //    uncovered and reset.
    {
        fresh();
        auto stale = std::make_shared<Trace>();
        auto updating = std::make_shared<Trace>();
        pushTest(T1, stale);
        TestMove* top = pushTest(T9, updating);
        MotionMaster::RemoveResult inside = MotionMaster::RemoveResult::NotFound;
        top->onUpdate = [&](Unit&) { inside = motion->RemoveMove(T1); return false; };
        const std::string before = Stack(bot);
        motion->UpdateMotion(0);
        check("deferred_in_update", inside == MotionMaster::RemoveResult::Deferred && stale->deleted && stale->resets == 0
            && updating->deleted && topType() == IDLE_MOTION_TYPE, before);
        fresh();
    }
    // 6. A callback that pushes: removing a top whose Finalize pushes another generator; the pushed one stays, not reset.
    {
        fresh();
        auto removedTrace = std::make_shared<Trace>();
        auto pushedTrace = std::make_shared<Trace>();
        TestMove* top = pushTest(T1, removedTrace);
        top->onFinalize = [&pushTest, pushedTrace](Unit&) { pushTest(T2, pushedTrace); };
        const std::string before = Stack(bot);
        const bool removed = motion->RemoveMove(T1) == MotionMaster::RemoveResult::Removed;
        check("callback_push", removed && removedTrace->deleted && topId() == T2 && pushedTrace->resets == 0, before);
        fresh();
    }
    // 7. The updating move removes itself and returns false: removed once, the one beneath not expired.
    {
        fresh();
        auto beneath = std::make_shared<Trace>();
        auto self = std::make_shared<Trace>();
        pushTest(T5, beneath);
        TestMove* top = pushTest(T1, self);
        top->onUpdate = [&](Unit&) { motion->RemoveMove(T1); return false; };
        const std::string before = Stack(bot);
        motion->UpdateMotion(0);
        check("self_removed", self->deleted && self->finalizes == 1 && !beneath->deleted && topId() == T5, before);
        fresh();
    }
    // 8. Several pending tokens: two buried tokens removed from one update; the top unchanged and not reset.
    {
        fresh();
        auto a = std::make_shared<Trace>();
        auto b = std::make_shared<Trace>();
        auto updating = std::make_shared<Trace>();
        pushTest(T1, a);
        pushTest(T2, b);
        TestMove* top = pushTest(T9, updating);
        top->onUpdate = [&](Unit&) { motion->RemoveMove(T1); motion->RemoveMove(T2); return true; };
        const std::string before = Stack(bot);
        motion->UpdateMotion(0);
        check("several_pending", a->deleted && b->deleted && topId() == T9 && updating->resets == 0, before);
        fresh();
    }
    // 9. Delayed expiry: the update removes the token beneath it and expires itself the delayed way; the token goes
    //    unreset, and the deferred reset lands on what was beneath it.
    {
        fresh();
        auto base = std::make_shared<Trace>();
        auto stale = std::make_shared<Trace>();
        auto updating = std::make_shared<Trace>();
        pushTest(T5, base);
        pushTest(T1, stale);
        TestMove* top = pushTest(T9, updating);
        top->onUpdate = [&](Unit&) { motion->RemoveMove(T1); motion->MovementExpired(); return true; };
        const std::string before = Stack(bot);
        motion->UpdateMotion(0);
        check("delayed_expiry", stale->deleted && stale->resets == 0 && updating->deleted && topId() == T5 && base->resets == 1,
            before);
        fresh();
    }
    // 10. Partial clear: a pending token and a Clear in the same update; the bottom stays, and nothing pending is left
    //     for a move no longer there.
    {
        fresh();
        auto stale = std::make_shared<Trace>();
        auto updating = std::make_shared<Trace>();
        pushTest(T1, stale);
        TestMove* top = pushTest(T9, updating);
        top->onUpdate = [&](Unit&) { motion->RemoveMove(T1); motion->Clear(); return true; };
        const std::string before = Stack(bot);
        motion->UpdateMotion(0);
        check("partial_clear", stale->deleted && updating->deleted && !motion->HasPendingRemoval(T1) && !motion->empty()
            && topType() == IDLE_MOTION_TYPE, before);
        fresh();
    }
    // 10b. Partial clear over a pending bottom: the clear keeps the bottom, owed its reset; the pending removal takes it
    //      unreset, and the stack is idle with nothing pending.
    {
        fresh();
        motion->Clear(false, true);
        auto bottom = std::make_shared<Trace>();
        auto updating = std::make_shared<Trace>();
        pushTest(T5, bottom);
        TestMove* top = pushTest(T9, updating);
        top->onUpdate = [&](Unit&) { motion->RemoveMove(T5); motion->Clear(); return true; };
        const std::string before = Stack(bot);
        motion->UpdateMotion(0);
        check("partial_clear_pending_bottom", bottom->deleted && bottom->resets == 0 && bottom->finalizes == 1
            && updating->deleted && !motion->HasPendingRemoval(T5) && std::distance(motion->begin(), motion->end()) == 1
            && topType() == IDLE_MOTION_TYPE, before);
        fresh();
    }
    // 11. Deferred replacement: a run move issued from inside an update, its previous token's removal deferred, issues
    //     nothing and keeps the previous record.
    {
        fresh();
        GuildRunView view;
        view.valid = true;
        view.mode = GuildRunMode::Lead;
        view.stamp = { 1, 1 };
        view.leader = bot->GetGUIDLow();
        uint32 previous = 0;
        sGuildRunStates.StartRunMove(bot, view, always, [&](uint32 token)
        {
            previous = token;
            motion->MovePoint(token, x, y, z, FORCED_MOVEMENT_RUN, false);
            return true;
        });
        auto updating = std::make_shared<Trace>();
        TestMove* top = pushTest(T9, updating);
        bool issued = false, started = true;
        top->onUpdate = [&](Unit&)
        {
            started = sGuildRunStates.StartRunMove(bot, view, always, [&](uint32) { issued = true; return true; });
            return true;
        };
        const std::string before = Stack(bot);
        motion->UpdateMotion(0);
        GuildRunStamp stamp;
        const uint32 recorded = sGuildRunStates.RunMoveOf(bot->GetGUIDLow(), stamp);
        check("deferred_replacement", previous && !started && !issued && recorded == previous && !motion->HasMove(previous), before);
        // The record, its move gone, is retired by the next cleanup.
        fresh();
        sGuildRunStates.CleanMoves();
    }
    // 12. Top first: the updating move queues itself and then a buried token. Removing the top uncovers the buried one
    //     while it is still queued: it goes unreset, and only the base is reset, once.
    {
        fresh();
        auto base = std::make_shared<Trace>();
        auto buried = std::make_shared<Trace>();
        auto self = std::make_shared<Trace>();
        pushTest(T5, base);
        pushTest(T2, buried);
        TestMove* top = pushTest(T1, self);
        top->onUpdate = [&](Unit&) { motion->RemoveMove(T1); motion->RemoveMove(T2); return true; };
        const std::string before = Stack(bot);
        motion->UpdateMotion(0);
        check("top_first_pending", self->deleted && self->finalizes == 1 && buried->deleted && buried->resets == 0
            && base->resets == 1 && !base->deleted && topId() == T5 && !motion->HasPendingRemoval(T2), before);
        fresh();
    }
    // 13. Delayed expiry whose Finalize pushes: the move pushed is not reset, nor the one it covers.
    {
        fresh();
        auto base = std::make_shared<Trace>();
        auto updating = std::make_shared<Trace>();
        auto pushed = std::make_shared<Trace>();
        pushTest(T5, base);
        TestMove* top = pushTest(T9, updating);
        top->onUpdate = [&](Unit&) { motion->MovementExpired(); return true; };
        top->onFinalize = [&pushTest, pushed](Unit&) { pushTest(T2, pushed); };
        const std::string before = Stack(bot);
        motion->UpdateMotion(0);
        check("delayed_expiry_push", updating->deleted && updating->finalizes == 1 && topId() == T2 && pushed->resets == 0
            && base->resets == 0 && !base->deleted, before);
        fresh();
    }
    // 14. A replacement made by a callback: the previous run move's Finalize starts and records a run move of its own.
    //     The outer call keeps it, issues nothing, and the record is the callback's.
    {
        fresh();
        GuildRunView view;
        view.valid = true;
        view.mode = GuildRunMode::Lead;
        view.stamp = { 1, 1 };
        view.leader = bot->GetGUIDLow();
        auto previousTrace = std::make_shared<Trace>();
        TestMove* previousMove = nullptr;
        sGuildRunStates.StartRunMove(bot, view, always, [&](uint32 token)
        {
            previousMove = pushTest(token, previousTrace);
            return true;
        });
        GuildRunStamp stamp;
        const uint32 previous = sGuildRunStates.RunMoveOf(bot->GetGUIDLow(), stamp);
        uint32 replacement = 0;
        bool inner = false;
        if (previousMove)
            previousMove->onFinalize = [&](Unit&)
            {
                inner = sGuildRunStates.StartRunMove(bot, view, always, [&](uint32 token)
                {
                    replacement = token;
                    motion->MovePoint(token, x, y, z, FORCED_MOVEMENT_RUN, false);
                    return true;
                });
            };
        bool issued = false;
        const std::string before = Stack(bot);
        const bool outer = sGuildRunStates.StartRunMove(bot, view, always, [&](uint32) { issued = true; return true; });
        const uint32 recorded = sGuildRunStates.RunMoveOf(bot->GetGUIDLow(), stamp);
        check("callback_replacement", previous && previousTrace->deleted && inner && replacement && !outer && !issued
            && recorded == replacement && topId() == replacement, before);
        fresh();
        sGuildRunStates.CleanMoves();
    }

    // The movement guard's core hook (plan item 4a), with a refusal of the test's own in place of a run's: every launch,
    // whichever generator asks, and the generator that asked removed by identity.
    {
        // Refuses any final path passing within a yard of the point 2.5 yards ahead, between its two ends.
        const runrules::Vec mid = { bot->GetPositionX() + 2.5f, bot->GetPositionY(), bot->GetPositionZ() };
        auto across = [mid](const std::vector<runrules::Vec>& path)
        {
            const float d = runrules::LinesDistance(path, { mid });
            return d >= 0.0f && d <= 1.0f;
        };
        auto refuseAll = [](const std::vector<runrules::Vec>&) { return true; };
        auto launch = [x, y, z](Unit& unit)
        {
            Movement::MoveSplineInit init(unit);
            init.MoveTo(x, y, z, false);
            init.Launch();
        };

        // G1. The final path of a point move, though both its ends are clear: refused as it launches, never pushed.
        {
            fresh();
            sGuildMoveGuard.SetTestRefusal(low, across);
            const std::string before = Stack(bot);
            motion->MovePoint(T3, x, y, z, FORCED_MOVEMENT_RUN, false);
            check("guard_final_path", !motion->HasMove(T3) && topType() == IDLE_MOTION_TYPE && bot->movespline->Finalized(),
                before);
            sGuildMoveGuard.SetTestRefusal(low, allowAll);
            fresh();
        }
        // G2. A generator's own launch from its Initialize: not pushed, deleted, what it covered left on top.
        {
            fresh();
            motion->MovePoint(T1, x, y + 3.0f, z, FORCED_MOVEMENT_RUN, false);
            sGuildMoveGuard.SetTestRefusal(low, across);
            auto trace = std::make_shared<Trace>();
            TestMove* move = new TestMove(T2, trace);
            move->onInitialize = launch;
            const std::string before = Stack(bot);
            motion->MoveGenerator(move);
            check("guard_initialize", trace->deleted && !motion->HasMove(T2) && topId() == T1, before);
            sGuildMoveGuard.SetTestRefusal(low, allowAll);
            fresh();
        }
        // G3. A relaunch from its Update, as a chase's replan: removed after the update, and not expired twice.
        {
            fresh();
            auto beneath = std::make_shared<Trace>();
            pushTest(T1, beneath);
            auto trace = std::make_shared<Trace>();
            TestMove* move = pushTest(T2, trace);
            move->onUpdate = [&](Unit& unit) { launch(unit); return true; };
            sGuildMoveGuard.SetTestRefusal(low, refuseAll);
            const std::string before = Stack(bot);
            motion->UpdateMotion(0);
            check("guard_update", trace->deleted && trace->finalizes == 1 && !motion->HasMove(T2) && topId() == T1
                && !beneath->deleted, before);
            sGuildMoveGuard.SetTestRefusal(low, allowAll);
            fresh();
        }
        // G4. A buried generator that relaunches when uncovered: removed then, never restored.
        {
            fresh();
            auto buried = std::make_shared<Trace>();
            TestMove* under = pushTest(T1, buried);
            under->onReset = launch;
            auto top = std::make_shared<Trace>();
            pushTest(T2, top);
            sGuildMoveGuard.SetTestRefusal(low, refuseAll);
            const std::string before = Stack(bot);
            const bool removed = motion->RemoveMove(T2) == MotionMaster::RemoveResult::Removed;
            check("guard_buried", removed && top->deleted && buried->resets == 1 && buried->deleted && !motion->HasMove(T1)
                && topType() == IDLE_MOTION_TYPE, before);
            sGuildMoveGuard.SetTestRefusal(low, allowAll);
            fresh();
        }
        // G5. A chase refused as it launches, from its initialize or its first update: no chase on the stack, and the AI's
        //     gate refuses its target for a while. A chase whose target already stands in reach launches nothing, so
        //     there is nothing to refuse, and the case says so instead of passing or failing.
        {
            fresh();
            sGuildMoveGuard.SetTestRefusal(low, refuseAll);
            const std::string before = Stack(bot);
            motion->MoveChase(target);
            if (topType() == CHASE_MOTION_TYPE)
                motion->UpdateMotion(0);
            const bool gated = sGuildMoveGuard.Refuses(bot, target->GetObjectGuid(), nullptr);
            if (topType() == CHASE_MOTION_TYPE && !gated && bot->GetDistance(target) <= 8.0f)
                out.push_back("skip guard_chase  " + targetName + " stands within reach, so the chase launches nothing; move them 10 yards apart");
            else
                check("guard_chase", topType() != CHASE_MOTION_TYPE && gated, before);
            sGuildMoveGuard.SetTestRefusal(low, allowAll);
            fresh();
        }
        // G6. A near teleport, as Blink's or a follow's unstuck jump: not made.
        {
            fresh();
            sGuildMoveGuard.SetTestRefusal(low, refuseAll);
            const std::string before = Stack(bot);
            const float x0 = bot->GetPositionX(), y0 = bot->GetPositionY();
            bot->NearTeleportTo(x, y, z, bot->GetOrientation());
            check("guard_teleport", !bot->IsBeingTeleportedNear() && bot->GetPositionX() == x0 && bot->GetPositionY() == y0,
                before);
            sGuildMoveGuard.SetTestRefusal(low, allowAll);
            fresh();
        }
        // G7. With the refusal gone, the same point move launches: the guard refuses only what it is asked to.
        {
            fresh();
            const std::string before = Stack(bot);
            motion->MovePoint(T3, x, y, z, FORCED_MOVEMENT_RUN, false);
            check("guard_allows", motion->HasMove(T3) && !bot->movespline->Finalized(), before);
            fresh();
        }
        // G8. A refusal's record, copied out under the guard's lock alone, keeps the run's plan and stop, the blocker and
        //     whether it is live; the run states read in the same tick, since no run state's lock is held with the guard's.
        {
            fresh();
            const std::string before = Stack(bot);
            runguard::GuardThreat blocker;
            blocker.spawn = 4242;
            blocker.live = true;
            blocker.where = { { x, y, z } };
            const runrules::Vec dest = { x, y, z };
            sGuildMoveGuard.NoteRefusal(bot, ObjectGuid(), &dest, "selftest", &blocker);
            const runguard::Refusal copy = sGuildMoveGuard.RefusalOf(low);
            GuildRunView view;
            const bool guarded = sGuildMoveGuard.Guarded(bot, view);
            const uint64 plan = guarded ? runguard::PlanKey(view.stamp.generation, view.stamp.plan) : 0;
            const size_t stop = guarded ? view.index : SIZE_MAX;
            const bool reads = !sGuildRunStates.Describe("").empty();
            check("guard_refusal_copy", copy.open && copy.spawn == 4242 && copy.live && copy.plan == plan && copy.stop == stop
                && reads, before);
            fresh();
        }
        // G9. A patrol is judged live, where it stands and with its walk across over the horizon, bound by its loop and
        //     never reaching from all of it: a line to a point of its loop beyond that is not refused by it, and a line to
        //     where it stands is, as live.
        //     Only with patrol=<spawn>, on a character whose run is in lead, with that patrol alive and out of a fight.
        if (!patrolSpawn)
            out.push_back("skip guard_patrol_live  give patrol=<spawn>, a patrol of the first character's run, as the foundry's 3600054");
        else
        {
            GuildRunView view;
            Creature* patrol = bot->GetMap()->GetCreature(patrolSpawn);
            const runguard::Envelope* envelope = nullptr;
            const bool guarded = sGuildMoveGuard.Guarded(bot, view) && view.mode == GuildRunMode::Lead;
            if (guarded && view.packs && view.packs->reach)
                envelope = view.packs->reach->BySpawn(patrolSpawn);
            if (!guarded)
                out.push_back("skip guard_patrol_live  " + name + " is not guarded in a run in lead");
            else if (!patrol || !patrol->IsAlive() || patrol->IsInCombat() || !envelope
                || envelope->kind != runguard::Kind::Patrol)
                out.push_back("skip guard_patrol_live  spawn " + std::to_string(patrolSpawn)
                    + " is no living patrol of the route out of a fight here");
            else
            {
                const std::string before = Stack(bot);
                const runrules::Vec here = { bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ() };
                const runrules::Vec live = { patrol->GetPositionX(), patrol->GetPositionY(), patrol->GetPositionZ() };
                const float reach = patrol->GetAttackDistance(bot)
                    + runguard::ReachPad(patrol->GetObjectBoundingRadius(), bot->GetObjectBoundingRadius());
                // Its reach and its walk across over the horizon: the 3D distance beyond this is beyond its live reach.
                const float most = reach + runguard::MaxSpread(GuildPacks::LiveSpeed(patrol, true), bot->GetSpeed(MOVE_RUN));
                // The point of its loop farthest from where it stands, beyond its most reach.
                const runrules::Vec* far = nullptr;
                for (const runrules::Vec& point : envelope->where)
                    if (runrules::Distance(point, live) > most + 3.0f
                        && (!far || runrules::Distance(point, live) > runrules::Distance(*far, live)))
                        far = &point;
                if (!far)
                    out.push_back("skip guard_patrol_live  no point of its loop lies beyond its reach and walk of where it stands");
                else
                {
                    // Short lines, so that the first creature in the way is the one asked about: 3 yards up to the loop's
                    // point, from its side away from the patrol, and from just outside its reach to where it stands.
                    // The point `yards` from `from` on the line toward `to`, or beyond `from` away from it when negative.
                    auto along = [](const runrules::Vec& from, const runrules::Vec& to, float yards)
                    {
                        const float d = std::max(0.01f, runrules::Distance(from, to));
                        const float t = yards / d;
                        return runrules::Vec{ from.x + (to.x - from.x) * t, from.y + (to.y - from.y) * t, from.z + (to.z - from.z) * t };
                    };
                    std::string by;
                    runguard::GuardThreat farBlocker, nearBlocker;
                    const runrules::Vec farFrom = along(*far, live, -3.0f);
                    const runguard::GuardResult farVerdict = sGuildMoveGuard.Check(bot, view, { farFrom, *far }, false, &by, &farBlocker);
                    const runrules::Vec nearFrom = along(live, here, most + 2.0f);
                    const runguard::GuardResult nearVerdict = sGuildMoveGuard.Check(bot, view, { nearFrom, live }, false, &by,
                        &nearBlocker);
                    const bool farPasses = farVerdict.verdict == runguard::Verdict::Allow || farBlocker.spawn != patrolSpawn;
                    const bool nearRefused = nearVerdict.verdict == runguard::Verdict::Refuse && nearBlocker.spawn == patrolSpawn
                        && nearBlocker.live;
                    check("guard_patrol_live", farPasses && nearRefused, before + " far_by=" + std::to_string(farBlocker.spawn)
                        + " near_by=" + std::to_string(nearBlocker.spawn));
                }
            }
        }
        // G10. The stop's stands, as the route certified them: none within reach of a living patrol of another pack that
        //      paces within its reach of the stand, as the foundry's Goblin Engineers do, which the live guard refuses at
        //      every look. The route certifies every yard of each stand's path against such movers; this looks at the
        //      stands themselves, on a character whose run is in lead at a stop.
        {
            GuildRunView view;
            const bool guarded = sGuildMoveGuard.Guarded(bot, view) && view.mode == GuildRunMode::Lead;
            if (!guarded || !view.packs || !view.packs->reach || view.index >= view.packs->stops.size())
                out.push_back("skip guard_stand_pacing  " + name + " is not guarded at a stop of a run in lead");
            else
            {
                const GuildPacks::Stop& stop = view.packs->stops[view.index];
                std::string by;
                for (const G3D::Vector3& stand : stop.stand)
                {
                    const runrules::Vec at = { stand.x, stand.y, stand.z };
                    for (const runguard::Envelope& e : view.packs->reach->Envelopes())
                    {
                        if (e.kind != runguard::Kind::Patrol || !e.aggroes || e.pack == stop.pack || e.where.empty())
                            continue;
                        Creature* creature = bot->GetMap()->GetCreature(e.spawn);
                        if (!creature || !creature->IsAlive())
                            continue;
                        if (std::all_of(e.where.begin(), e.where.end(),
                            [&](const runrules::Vec& point) { return runrules::Distance(point, at) <= e.reach; }))
                            by += (by.empty() ? "" : ",") + std::to_string(e.spawn);
                    }
                }
                check("guard_stand_pacing", by.empty(), "stop=" + std::to_string(view.index + 1) + " stands="
                    + std::to_string(stop.stand.size()) + (by.empty() ? std::string() : " paced_by=" + by));
            }
        }
    }

    fresh();
    out.push_back(std::to_string(passed) + " passed, " + std::to_string(failed) + " failed; " + name + " left idle");
    return out;
}
