#pragma once

#include "Common.h"
#include "Entities/ObjectGuid.h"
#include "playerbot/GuildRunState.h"
#include "playerbot/GuildRunTactics.h"

#include <cstdint>
#include <list>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <tuple>
#include <vector>

class Creature;
class Group;
class Player;
class Spell;
class Unit;
struct SpellEntry;

// A run's tactics in the server (docs/plan.md, item 5, the first experiment: kill order and interrupts): the facts of
// runtactics filled from the creatures' spells and AI data and from what the party sees them do, the leader's plan for
// the fight, the marks it sets, the interrupts it routes to members, and their outcomes as run_tactic events.
//
// Who writes what (docs/run-contract.md): the leader's AI owns one plan for its run, on its instance's map thread and only
// in the mode lead, under the run's stamp when it decided. A new generation or plan revision makes the plan stale, and a
// member's interrupt under a plan of another stamp does nothing. Members only read the plan, and say what their own
// interrupt came to. The guild tick, on the world thread between map updates, lets go of the plans of runs no longer in
// lead: it clears the marks the plan set, where they still stand, and the plan with them.
// One lock for every plan, held only to copy plain values in or out; never across a cast, a mark or another lock.
class GuildTactics
{
public:
    static GuildTactics& instance()
    {
        static GuildTactics tactics;
        return tactics;
    }

    // The leader's AI, in lead, each look while the party fights or pulls: what it sees of the creatures since the last look
    // (casts begun and ended, flees, summons and whose), the cast events, and whether to decide again (runtactics::Due):
    // no plan yet for this stamp, or the plan stale, or a cast with nobody on it after the retry time. `why` says which.
    // Out of a fight with no pull coming, due only to clear a mark the plan set on a creature not in a fight (`clear`).
    bool Look(Player* leader, const GuildRunView& view, std::string& why);
    // The leader decides now, from the creatures as they are: the kill order, the marks Skull and Cross set on the group,
    // the tank's target and the interrupts. Under the view's stamp, which the plan keeps. Writes run_tactic when the marks
    // change. `pulled`, at the pull, is the creature pulled, a foe once in the fight, as the rest of its pack.
    // True only when it changed something the party acts on: a mark set or cleared, what it tells, or the interrupts;
    // else the leader's engine goes on to its next action in the same tick.
    bool Decide(Player* leader, const GuildRunView& view, const std::string& why, Creature* pulled = nullptr);
    // A pull that started nothing, or failed: when the plan has marks set, it decides again on the creatures in a fight
    // now, none when nobody fights, so a mark on the creature where it stands is cleared.
    bool Clear(Player* leader, const GuildRunView& view, const std::string& why);

    // Whether any plan in the bot's instance has it interrupt something: a cheap look, before a fresh view of the run.
    bool Assigned(Player* bot);
    // A member's AI, in lead: the creature the plan has it interrupt now, casting a cast its interrupt can stop, and the
    // known interrupt it can cast at it now. Under the view's stamp only (runtactics::PlanCurrent).
    bool InterruptDue(Player* bot, const GuildRunView& view, Unit*& foe, uint32& spell);
    // The member's interrupt, its outcome judged right after (runtactics::Judge), told to the plan as a run_tactic event.
    // Nothing when the action's stamp or the plan's is not the run's now.
    bool Interrupt(Player* bot, const GuildRunView& view, const GuildRunStamp& stamp);

    // The plan's tank target for a tank of a run in lead (runtactics::TankPick): the first creature of the kept plan's
    // kill order on a healer of the party; else the tank's last pick while it lives in the order on the tank or a member;
    // else the first on a member who is not a tank, the skull left out, which the tank reaches through the mark. Empty
    // otherwise, and the tank picks as it does (TankTargetValue): the mark, then the creatures it does not hold. A creature
    // under crowd control that damage breaks is never picked. A change of pick is told as run_tactic tank_pick=, under the
    // leader.
    ObjectGuid TankTarget(Player* bot, const GuildRunView& view);

    // The guild tick, on the world thread between map updates: the plans of runs no longer in lead, or of another
    // generation, are let go, their marks cleared where they still stand. Returns its events, (leader, kind, detail).
    std::vector<std::tuple<uint32, std::string, std::string>> Sweep();

    // `run tactics [leader]`: every plan kept; with a leader, its fight's facts, the members' interrupts, a fresh decision
    // beside the kept plan, and checks of the marks and the plan's stamp. Reads only. World thread.
    std::list<std::string> Describe(const std::string& leaderName);

    // What a spell does (runtactics::Kind), from its effects and one level of the spells it triggers.
    static uint8 SpellKind(uint32 spellId);
    // What a creature can do by its spell list and its AI's data: summon, heal, flee at low health. `from` says where.
    struct Abilities
    {
        bool summons = false;
        bool heals = false;
        bool flees = false;
        std::string from;
    };
    static Abilities AbilitiesOf(Creature* creature);
    // The interrupts a character knows: spells with an interrupt effect in its spell book, the highest rank of each.
    static std::vector<uint32> InterruptsOf(Player* player);
    // The current cast of a creature that summons or heals: the spell, or null.
    static Spell* CastOf(Unit* unit, uint8& kind);

private:
    GuildTactics() = default;

    // A creature as one plan knows it: its id in the plan, what it can do, what it was seen do, and its cast under way.
    struct Known
    {
        uint32 id = 0;
        Abilities abilities;
        bool seenSummoning = false;
        bool seenHealing = false;
        bool seenFleeing = false;
        uint32 castSpell = 0;        // the summon or heal it is casting, as last seen, or 0
        uint8 castKind = 0;
        uint32 castStartMs = 0;
        uint32 castMs = 0;            // how long it takes, from its start
        bool castInterruptible = false;
        uint32 tried = 0;             // interrupts tried on it
        bool interrupted = false;     // and one stopped it
    };

    struct Live
    {
        uint32 leader = 0;
        uint32 group = 0;
        uint32 mapId = 0;
        uint32 instanceId = 0;
        uint64 start = 0;             // the run's server start, for the events' tag
        GuildRunStamp stamp;          // the run's stamp it was decided under; generation 0 for none yet
        runtactics::Plan plan;
        std::vector<uint32> planned;  // the foes it was decided on
        std::map<ObjectGuid, Known> known;
        std::map<uint32, ObjectGuid> guids; // by id
        uint32 nextId = 0;
        ObjectGuid skull, cross;      // the marks it set
        std::string marks;            // as last told
        std::set<std::string> notCast;// member:foe:spell said once as not cast
        uint32 decided = 0;           // decisions under this generation
        std::map<uint32, uint32> tankPicks; // each tank's last pick, by its id in the plan, as last told
    };

    // TankTarget's pick, read only: the creature, its id in the plan and whom it is on; empty, 0 and None for none.
    ObjectGuid TankPickOf(Player* bot, const GuildRunView& view, uint32& id, runtactics::Victim& on);

    // The creatures of the party's fight, through the leader's map: what attacks a member, and what a member attacks, the
    // creature pulled and the stop's pack while the pull comes, each once in a fight. Alive in the world only; never one not
    // yet in the fight, which the party would go and strike where it stands.
    static std::vector<Creature*> FightCreatures(Player* leader, const GuildRunView& view, Creature* pulled);
    // The members as runtactics sees them, those that deal damage first; with `casting`, each one's interrupt checked
    // against each caster (castable now, in range and sight), else none asked.
    std::vector<runtactics::Member> Members(Player* leader, const GuildRunView& view, const std::vector<std::pair<uint32, Unit*>>& casting,
        std::map<uint32, std::string>& names);
    // The facts of one creature for the plan, with its id, updating what the plan has seen of it.
    runtactics::Foe FoeOf(Live& live, Creature* creature, const std::vector<Player*>& healers, uint32 nowMs,
        std::vector<std::string>& events);
    Live& LiveOf(Player* leader, const GuildRunView& view);
    static std::string Tag(const Live& live);
    static std::string FoeText(const Live& live, uint32 id);
    // The cast a creature was seen casting has ended: told once, as run_tactic cast=.
    static std::string CastEnded(const Live& live, const Known& known, const char* how, uint32 nowMs);

    std::mutex lock;
    std::map<uint32, Live> lives;     // by instance id
    // Spells' kinds and characters' interrupts, from static data, kept once looked up.
    static std::mutex cacheLock;
    static std::map<uint32, uint8> spellKinds;
};

#define sGuildTactics GuildTactics::instance()
