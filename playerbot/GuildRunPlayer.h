#pragma once

// A person in a guild party's run (docs/plan.md, item 6): who controls each member, a hand-back against a disconnect,
// waiting for a person without counting it against the run, and a run stopped because its person is gone. Pure
// decisions on plain facts, as GuildRunRules.h has them, so that tests/run_lifecycle builds them without the server.

#include "GuildRunRules.h"

#include <cstdint>
#include <string>
#include <vector>

namespace runplayer
{
    // Who plays an expected member: the engine, or a person, for as long as the run knows the member, offline too.
    enum class Controller : uint8_t { Engine, Person };

    // How a person's session ended: a hand-back when the client asked to log out (a logout or /camp, instant or after
    // its countdown) and the session is not offline. A closed socket makes it offline, even mid-countdown; an AFK or a
    // GM kick is not the client's request. Each of those is a disconnect: the character stays the person's.
    inline bool LogoutHandsBack(bool clientLogout, bool sessionOffline)
    {
        return clientLogout && !sessionOffline;
    }

    // Whether the guild manager logs a character in for the engine: enabled, not reserved for a person, and offline long
    // enough. A character handed back is logged in at once, with no relog delay.
    inline bool EngineLogsIn(bool enabled, bool reserved, bool handedBack, int64_t offlineFor, int64_t relogDelay)
    {
        if (!enabled || reserved)
            return false;
        return handedBack || offlineFor >= relogDelay;
    }

    // A person offline: since the disconnect, kept whatever the run does meanwhile, and cleared only when the person is
    // back. At PLAYER_GONE_SECONDS the run stops.
    constexpr int64_t PLAYER_GONE_SECONDS = 1800;

    struct OfflineClock
    {
        int64_t since = 0;

        void Disconnected(int64_t now)
        {
            if (!since)
                since = now;
        }
        void Reconnected() { since = 0; }
        bool Gone(int64_t now) const { return since && now - since >= PLAYER_GONE_SECONDS; }
    };

    // How a run ended, for the manager: failed (cannot_rejoin), or stopped (player_gone), which is neither done nor
    // failed. Neither counts as a run done, and neither starts a repeating order again.
    enum class Outcome : uint8_t { Failed, Stopped };

    inline const char* JobStatus(Outcome outcome)
    {
        return outcome == Outcome::Failed ? "failed" : "stopped";
    }

    inline bool CountsAsDone(Outcome)
    {
        return false;
    }

    // What keeps a pull back, by whose it is. Only a person's: the party waits, with no block counted and the waiting
    // budget frozen. Mixed: the bots' reasons keep their bounded recovery. A person's reason never starts a resupply.
    enum class Wait : uint8_t { None, Person, Bots, Mixed };

    inline Wait WaitOf(size_t personReasons, size_t botReasons)
    {
        if (!personReasons)
            return botReasons ? Wait::Bots : Wait::None;
        return botReasons ? Wait::Mixed : Wait::Person;
    }

    inline bool BudgetRuns(Wait wait)
    {
        return wait == Wait::Bots || wait == Wait::Mixed;
    }

    // The reasons of members the engine plays, which alone may be blocked on.
    inline std::vector<runrules::Kept> BotReasons(const std::vector<runrules::Kept>& kept)
    {
        std::vector<runrules::Kept> bots;
        for (const runrules::Kept& k : kept)
            if (!k.person)
                bots.push_back(k);
        return bots;
    }

    inline Wait WaitOf(const std::vector<runrules::Kept>& kept)
    {
        const size_t bots = BotReasons(kept).size();
        return WaitOf(kept.size() - bots, bots);
    }

    // The waiting budget frozen while only a person keeps the pull back: `personSince` is when that began, or 0. When it
    // ends the budget's start moves on by the time it lasted, so the budget counts only time spent waiting for bots.
    inline void FreezeBudget(runrules::Budget& budget, int64_t& personSince, bool personOnly, int64_t now)
    {
        if (personOnly && !personSince)
            personSince = now;
        else if (!personOnly && personSince)
        {
            if (budget.since)
                budget.since += now - personSince;
            personSince = 0;
        }
    }

    // The budget as a block sees it now: frozen for as long as a person-only wait has lasted so far.
    inline runrules::Budget Frozen(runrules::Budget budget, int64_t personSince, int64_t now)
    {
        if (personSince && budget.since)
            budget.since += now - personSince;
        return budget;
    }

    // What the leader says in party chat it waits for a person for, one clause each, or empty when it waits for no person.
    template<class NameOf>
    std::string WaitLine(const std::vector<runrules::Kept>& kept, NameOf nameOf)
    {
        std::string line;
        for (const runrules::Kept& k : kept)
        {
            if (!k.person)
                continue;
            std::string why;
            switch (k.why)
            {
                case runrules::Keep::Dead: why = "to come back"; break;
                case runrules::Keep::Away: why = "to come to me"; break;
                case runrules::Keep::Fight: why = "to finish the fight"; break;
                case runrules::Keep::Far: why = "to come to me"; break;
                case runrules::Keep::Health: why = "health, need 80%"; break;
                case runrules::Keep::Mana: why = "mana, need 60%"; break;
                case runrules::Keep::Eating: why = "to finish eating or drinking"; break;
                case runrules::Keep::Sight: why = "to see me from within 40 yards"; break;
                default: why = runrules::KeepName(k.why); break;
            }
            const std::string clause = nameOf(k.guid) + " " + why;
            if (line.find(clause) == std::string::npos)
                line += (line.empty() ? "Waiting for " : "; ") + clause;
        }
        return line;
    }

    // Whether the leader says a line now: when it changed, or a minute after it last said the same.
    constexpr int64_t SAY_AGAIN_SECONDS = 60;

    inline bool MaySay(const std::string& line, const std::string& last, int64_t lastAt, int64_t now)
    {
        return !line.empty() && (line != last || now - lastAt >= SAY_AGAIN_SECONDS);
    }

    // Whether a block counts at the budget's end: only when a bot's reason stands.
    inline bool BlockCounts(Wait wait)
    {
        return wait == Wait::Bots || wait == Wait::Mixed;
    }

    // Whether a reason may start a resupply trip: a bot's only.
    inline bool MayResupply(bool personReason)
    {
        return !personReason;
    }

    // A bot's own time apart for cannot_rejoin: it runs only while that bot is apart and nothing else holds the run, so
    // a long wait for a person never uses up a bot's time, and a bot newly apart starts from zero.
    struct BotClock
    {
        int64_t apartFor = 0;
        int64_t last = 0;
        bool wasApart = false;

        // Counted from one tick to the next while the bot was apart at both with nothing holding the run. A reason to hold
        // starts it over, as A2b's deadline does.
        void Tick(int64_t now, bool apart, bool holding)
        {
            if (!apart || holding)
            {
                apartFor = 0;
                wasApart = false;
                last = now;
                return;
            }
            if (wasApart)
                apartFor += now - last;
            wasApart = true;
            last = now;
        }
        bool Overdue(int64_t limit) const { return apartFor >= limit; }
    };

    // A person seated with a food or drink of a cast item still eats or drinks: the pull waits until the aura ends or the
    // person stands, whatever the person's health and mana. A regeneration aura from anything else does not count.
    inline bool Eating(bool seated, bool regenAuraFromItem)
    {
        return seated && regenAuraFromItem;
    }

    // A person healer is ready for a pull only in sight of the tank and within HEAL_REACH of it.
    constexpr float HEAL_REACH = 40.0f;

    inline bool HealerReady(bool healer, bool inSightOfTank, float distanceToTank)
    {
        return !healer || (inSightOfTank && distanceToTank >= 0.0f && distanceToTank <= HEAL_REACH);
    }

    // A fight the leader did not pull: the party fights outside the pull phase, or with a creature that is not the pull's.
    // The planned pull is cancelled, the party defends, and the leader chooses its stop again from what died.
    inline bool UnplannedFight(bool partyInCombat, bool pullPhase, bool fightsThePull)
    {
        return partyInCombat && (!pullPhase || !fightsThePull);
    }

    // The steps of a change of control, in order. To a person: what the bot started goes first, then the run's queued
    // actions for it, then its ownership. Back to the engine: ownership first, then the strategies, then its way back.
    enum class Step : uint8_t { CancelMoves, CancelPull, Revise, Ownership, Strategies, Rejoin };

    inline std::vector<Step> TransferSteps(Controller to)
    {
        if (to == Controller::Person)
            return { Step::CancelMoves, Step::CancelPull, Step::Revise, Step::Ownership };
        return { Step::Revise, Step::Ownership, Step::Strategies, Step::Rejoin };
    }

    // The leader played by a person holds the run; it hands back by the same logout rule as any member.
    inline std::string LeaderHold(Controller leader, bool online)
    {
        if (leader == Controller::Engine)
            return "";
        return online ? "leader_taken_over" : "player_offline";
    }
}
