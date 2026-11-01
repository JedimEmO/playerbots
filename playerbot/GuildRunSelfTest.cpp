#include "playerbot/playerbot.h"

#include "playerbot/GuildRunSelfTest.h"
#include "playerbot/GuildBotMgr.h"
#include "playerbot/GuildRunState.h"
#include "Accounts/AccountMgr.h"
#include "MotionGenerators/MovementGenerator.h"
#include "Movement/MoveSpline.h"

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

        void Initialize(Unit&) override {}
        void Finalize(Unit& unit) override
        {
            ++trace->finalizes;
            if (onFinalize)
                onFinalize(unit);
        }
        void Interrupt(Unit&) override { ++trace->interrupts; }
        void Reset(Unit&) override { ++trace->resets; }
        bool Update(Unit& unit, const uint32&) override
        {
            ++trace->updates;
            return onUpdate ? onUpdate(unit) : true;
        }
        MovementGeneratorType GetMovementGeneratorType() const override { return POINT_MOTION_TYPE; }
        uint32 GetMoveId() const override { return id; }

        std::function<bool(Unit&)> onUpdate;
        std::function<void(Unit&)> onFinalize;

    private:
        uint32 id;
        std::shared_ptr<Trace> trace;
    };

    // Ids for the test's own moves, in the run's namespace and far from the counter's start.
    constexpr uint32 T1 = runrules::RUN_TOKEN_BIT | 0x7FFFFF01u;
    constexpr uint32 T2 = runrules::RUN_TOKEN_BIT | 0x7FFFFF02u;
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
    if (name.empty() || targetName.empty())
        return { "guildbot run selftest <character> <second character>   lab or bench characters only; the second is chased" };
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

    fresh();
    out.push_back(std::to_string(passed) + " passed, " + std::to_string(failed) + " failed; " + name + " left idle");
    return out;
}
