#pragma once

// A pull's tactics (docs/plan.md, item 5): kill order, crowd control, interrupts and snares, as pure functions of plain
// facts about the creatures and the party, with no server types, so that tests/run_lifecycle builds them without the
// server. The facts are generic: what a creature can do comes from its spells and its AI's data, or from what it was seen
// doing, never from its entry, name or dungeon. The server fills them and carries the plan out (GuildTactics).

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace runtactics
{
    enum class Type : uint8_t { Humanoid, Beast, Undead, Elemental, Mechanical, Demon, Dragonkin, Giant, Other };

    // Crowd control the first version assigns. A hunter's trap is a follow-up: upstream's trapping walks up to the target
    // and feigns death, and nothing yet places a trap at the pull spot.
    enum class Cc : uint8_t { Polymorph, Shackle, Sap };

    // Whether a crowd control fits a creature's type at all; Sap only before the creature is in a fight.
    inline bool Fits(Cc cc, Type type, bool inFight)
    {
        switch (cc)
        {
            case Cc::Polymorph: return type == Type::Humanoid || type == Type::Beast;
            case Cc::Shackle: return type == Type::Undead;
            default: return type == Type::Humanoid && !inFight;
        }
    }

    // What a spell does, for the facts: it summons a creature, or it heals. Classified from its effects alone, by the
    // core's effect and aura numbers (SpellEffectDefines.h, SpellAuraDefines.h; GuildTactics.cpp checks each against them).
    namespace effect
    {
        constexpr uint32_t APPLY_AURA = 6;
        constexpr uint32_t HEAL = 10;
        constexpr uint32_t SUMMON = 28;
        constexpr uint32_t TRIGGER_MISSILE = 32;
        constexpr uint32_t SUMMON_WILD = 41;
        constexpr uint32_t SUMMON_GUARDIAN = 42;
        constexpr uint32_t TRIGGER_SPELL = 64;
        constexpr uint32_t HEAL_MAX_HEALTH = 67;
        constexpr uint32_t INTERRUPT_CAST = 68;
        constexpr uint32_t SUMMON_POSSESSED = 73;
        constexpr uint32_t SUMMON_TOTEM = 74;
        constexpr uint32_t HEAL_MECHANICAL = 75;
        constexpr uint32_t SUMMON_TOTEM_SLOT1 = 87;
        constexpr uint32_t SUMMON_TOTEM_SLOT4 = 90;
        constexpr uint32_t SUMMON_PHANTASM = 93;
        constexpr uint32_t SUMMON_DEMON = 112;
        constexpr uint32_t AURA_PERIODIC_HEAL = 8;
    }

    enum Kind : uint8_t { KIND_NONE = 0, KIND_SUMMON = 1, KIND_HEAL = 2 };

    // One effect of a spell: whether it summons a creature or heals. A critter, an object, an item or a player summoned is
    // neither: none of them joins a fight. A trigger is followed by the caller, one level down.
    inline uint8_t EffectKind(uint32_t effectId, uint32_t aura)
    {
        using namespace effect;
        switch (effectId)
        {
            case SUMMON: case SUMMON_WILD: case SUMMON_GUARDIAN: case SUMMON_POSSESSED: case SUMMON_TOTEM:
            case SUMMON_PHANTASM: case SUMMON_DEMON:
                return KIND_SUMMON;
            case HEAL: case HEAL_MAX_HEALTH: case HEAL_MECHANICAL:
                return KIND_HEAL;
            case APPLY_AURA:
                return aura == AURA_PERIODIC_HEAL ? KIND_HEAL : KIND_NONE;
            default:
                return effectId >= SUMMON_TOTEM_SLOT1 && effectId <= SUMMON_TOTEM_SLOT4 ? KIND_SUMMON : KIND_NONE;
        }
    }

    // Whether an effect hands on to another spell, whose kind then counts as this one's.
    inline bool Triggers(uint32_t effectId)
    {
        return effectId == effect::TRIGGER_SPELL || effectId == effect::TRIGGER_MISSILE;
    }

    struct Foe
    {
        uint32_t guid = 0;
        Type type = Type::Other;
        bool elite = false;
        uint32_t health = 100;        // percent
        bool summons = false;         // it has a spell that summons a creature, or was seen summoning
        bool heals = false;           // it has a spell that heals, or was seen casting one
        bool flees = false;           // its AI's data flees at low health, or it has been seen fleeing
        bool fleeing = false;         // fleeing now
        bool casting = false;         // casting now a summon or a heal
        bool interruptible = false;   // that cast can be interrupted
        bool onHealer = false;        // its target is the party's healer
        bool inFight = false;
        uint8_t immune = 0;           // a bit for each Cc it is immune to: 1 << Cc
    };

    // A member as tactics see it: what it can do now, each checked by the server against a known spell, its resources,
    // its cooldown and range. A person's character gets no assignment: the run never commands a human's abilities.
    // The order members are given in is the order they are asked in: the server puts those that deal damage first.
    struct Member
    {
        uint32_t guid = 0;
        bool engine = true;
        bool alive = true;
        std::vector<Cc> cc;           // crowd control it can cast now
        bool interrupt = false;       // an interrupt ready now
        std::vector<uint32_t> beyond; // foes that interrupt does not reach now, out of its range or sight
        bool snare = false;           // a snare or stun ready now
    };

    struct Assignment
    {
        uint32_t member = 0;
        uint32_t foe = 0;
        Cc cc = Cc::Polymorph;
    };

    struct Pair
    {
        uint32_t member = 0;
        uint32_t foe = 0;
    };

    struct Plan
    {
        uint32_t skull = 0;           // the kill target
        uint32_t cross = 0;           // the next
        uint32_t tank = 0;            // the tank's target: a creature on the healer first, else the skull
        std::vector<uint32_t> order;  // the kill order, crowd-controlled creatures left out
        std::vector<Assignment> cc;
        std::vector<Pair> interrupts;
        std::vector<Pair> snares;
        // What the plan was decided on, so that the same facts never make it stale again: the casts to interrupt, the
        // creatures about to flee and those on the healer, whether or not a member could take them.
        std::vector<uint32_t> casts;
        std::vector<uint32_t> fleeing;
        std::vector<uint32_t> onHealer;
        int64_t at = 0;               // when it was decided, in the caller's milliseconds
    };

    // Crowd control comes in only for a pull of three creatures or more, or of two elites or more.
    inline bool BigPull(const std::vector<Foe>& foes)
    {
        const auto elites = std::count_if(foes.begin(), foes.end(), [](const Foe& f) { return f.elite; });
        return foes.size() >= 3 || elites >= 2;
    }

    // A creature about to run: one that flees, under this health, or running already, is snared before it brings friends.
    constexpr uint32_t FLEE_HEALTH = 25;

    inline bool AboutToFlee(const Foe& foe)
    {
        return foe.fleeing || (foe.flees && foe.health < FLEE_HEALTH);
    }

    inline bool ToInterrupt(const Foe& foe)
    {
        return foe.casting && foe.interruptible;
    }

    inline bool Has(const std::vector<uint32_t>& list, uint32_t guid)
    {
        return std::find(list.begin(), list.end(), guid) != list.end();
    }

    inline bool Reaches(const Member& member, uint32_t foe)
    {
        return member.interrupt && !Has(member.beyond, foe);
    }

    // The kill order: a creature that summons or heals first, then one on the healer, then one about to flee, then the
    // lowest health; the guid last, so the order never depends on the input's.
    inline std::vector<uint32_t> KillOrder(std::vector<Foe> foes)
    {
        std::sort(foes.begin(), foes.end(), [](const Foe& a, const Foe& b)
        {
            const int ac = a.summons || a.heals ? 0 : 1, bc = b.summons || b.heals ? 0 : 1;
            if (ac != bc)
                return ac < bc;
            if (a.onHealer != b.onHealer)
                return a.onHealer;
            if (AboutToFlee(a) != AboutToFlee(b))
                return AboutToFlee(a);
            if (a.health != b.health)
                return a.health < b.health;
            return a.guid < b.guid;
        });
        std::vector<uint32_t> order;
        for (const Foe& foe : foes)
            order.push_back(foe.guid);
        return order;
    }

    inline const Foe* FoeOf(const std::vector<Foe>& foes, uint32_t guid)
    {
        auto it = std::find_if(foes.begin(), foes.end(), [guid](const Foe& f) { return f.guid == guid; });
        return it == foes.end() ? nullptr : &*it;
    }

    // The plan for the creatures as they are now. Crowd control falls on the last of the kill order, never on the skull
    // or the cross, one creature for each member that can, by the first crowd control of its own that fits and the
    // creature is not immune to. Interrupts go only to casts that summon or heal and can be interrupted, one member each,
    // the first free member whose interrupt reaches the caster. Snares go to creatures about to flee.
    inline Plan Decide(const std::vector<Foe>& foes, const std::vector<Member>& members)
    {
        Plan plan;
        std::vector<uint32_t> order = KillOrder(foes);
        std::vector<uint32_t> busy;
        auto free = [&](const Member& m) { return m.engine && m.alive && !Has(busy, m.guid); };

        if (BigPull(foes) && order.size() > 2)
        {
            for (size_t i = order.size(); i-- > 2;)
            {
                const Foe* foe = FoeOf(foes, order[i]);
                if (!foe || foe->summons || foe->heals)
                    continue;
                for (const Member& member : members)
                {
                    if (!free(member))
                        continue;
                    auto cc = std::find_if(member.cc.begin(), member.cc.end(), [&](Cc c)
                        { return Fits(c, foe->type, foe->inFight) && !(foe->immune & (1u << uint8_t(c))); });
                    if (cc == member.cc.end())
                        continue;
                    plan.cc.push_back({ member.guid, foe->guid, *cc });
                    busy.push_back(member.guid);
                    break;
                }
            }
        }
        // The kill order without the crowd-controlled.
        for (uint32_t guid : order)
            if (std::none_of(plan.cc.begin(), plan.cc.end(), [guid](const Assignment& a) { return a.foe == guid; }))
                plan.order.push_back(guid);
        plan.skull = plan.order.size() > 0 ? plan.order[0] : 0;
        plan.cross = plan.order.size() > 1 ? plan.order[1] : 0;
        plan.tank = plan.skull;
        for (uint32_t guid : plan.order)
            if (const Foe* foe = FoeOf(foes, guid))
                if (foe->onHealer)
                {
                    plan.tank = guid;
                    break;
                }

        for (uint32_t guid : plan.order)
        {
            const Foe* foe = FoeOf(foes, guid);
            if (!foe || !ToInterrupt(*foe))
                continue;
            plan.casts.push_back(guid);
            for (const Member& member : members)
                if (free(member) && Reaches(member, guid))
                {
                    plan.interrupts.push_back({ member.guid, guid });
                    busy.push_back(member.guid);
                    break;
                }
        }
        for (uint32_t guid : plan.order)
        {
            const Foe* foe = FoeOf(foes, guid);
            if (!foe || !AboutToFlee(*foe))
                continue;
            plan.fleeing.push_back(guid);
            for (const Member& member : members)
                if (free(member) && member.snare)
                {
                    plan.snares.push_back({ member.guid, guid });
                    busy.push_back(member.guid);
                    break;
                }
        }
        for (const Foe& foe : foes)
            if (foe.onHealer)
                plan.onHealer.push_back(foe.guid);
        return plan;
    }

    // Whether a plan is stale for the creatures now: a marked or assigned creature gone or dead, a new one in the fight,
    // as a summon, a new cast to interrupt, a creature newly about to flee or newly on the healer, or a crowd control
    // that failed. What the plan was decided on never makes it stale again, taken or not, so a cast no member can take
    // does not decide again on every look; Due retries those.
    inline bool Stale(const Plan& plan, const std::vector<Foe>& foes, const std::vector<uint32_t>& planned, bool ccFailed)
    {
        if (ccFailed)
            return true;
        for (uint32_t guid : { plan.skull, plan.cross })
            if (guid && !FoeOf(foes, guid))
                return true;
        for (const Assignment& a : plan.cc)
            if (!FoeOf(foes, a.foe))
                return true;
        for (const Foe& foe : foes)
        {
            if (!Has(planned, foe.guid))
                return true;
            if ((ToInterrupt(foe) && !Has(plan.casts, foe.guid)) || (AboutToFlee(foe) && !Has(plan.fleeing, foe.guid))
                || (foe.onHealer && !Has(plan.onHealer, foe.guid)))
                return true;
        }
        return false;
    }

    // How soon a cast the plan has no interrupt for is looked at again: a member's interrupt may be ready by then.
    constexpr int64_t RETRY_MS = 300;

    // Whether to decide again now: the plan is stale, or a cast it took note of is still going with no member on it,
    // RETRY_MS after the plan was decided.
    inline bool Due(const Plan& plan, const std::vector<Foe>& foes, const std::vector<uint32_t>& planned, int64_t now)
    {
        if (Stale(plan, foes, planned, false))
            return true;
        if (now - plan.at < RETRY_MS)
            return false;
        for (uint32_t guid : plan.casts)
        {
            const Foe* foe = FoeOf(foes, guid);
            const bool taken = std::any_of(plan.interrupts.begin(), plan.interrupts.end(), [guid](const Pair& p) { return p.foe == guid; });
            if (foe && ToInterrupt(*foe) && !taken)
                return true;
        }
        return false;
    }

    // A member tried its interrupt on the foe, landed or not: the pair is done, and a cast still going is retried by Due
    // with the members whose interrupt is still ready.
    inline void Spent(Plan& plan, uint32_t member, uint32_t foe)
    {
        plan.interrupts.erase(std::remove_if(plan.interrupts.begin(), plan.interrupts.end(),
            [&](const Pair& p) { return p.member == member && p.foe == foe; }), plan.interrupts.end());
    }

    // The foe's cast ended, by an interrupt, its completion or its death: its next cast is a new one, and makes the plan
    // stale.
    inline void Ended(Plan& plan, uint32_t foe)
    {
        plan.casts.erase(std::remove(plan.casts.begin(), plan.casts.end(), foe), plan.casts.end());
        plan.interrupts.erase(std::remove_if(plan.interrupts.begin(), plan.interrupts.end(),
            [foe](const Pair& p) { return p.foe == foe; }), plan.interrupts.end());
    }

    // The member the plan has interrupting the foe, or 0.
    inline uint32_t InterrupterOf(const Plan& plan, uint32_t foe)
    {
        for (const Pair& p : plan.interrupts)
            if (p.foe == foe)
                return p.member;
        return 0;
    }

    // The foe the plan has this member interrupt, or 0.
    inline uint32_t InterruptFor(const Plan& plan, uint32_t member)
    {
        for (const Pair& p : plan.interrupts)
            if (p.member == member)
                return p.foe;
        return 0;
    }

    // A crowd control that did not land, retried a bounded number of times; then the assignment is cleared, so that the
    // creature is not left out of the kill order for good.
    constexpr uint32_t CC_TRIES = 2;

    inline bool KeepAssignment(bool applied, uint32_t tries)
    {
        return applied || tries < CC_TRIES;
    }

    // What an interrupt attempt came to: the cast stopped, the cast went on (a miss, a dodge, a resist, or not landed
    // yet), or the interrupt was not cast at all.
    enum class Outcome : uint8_t { Interrupted, Missed, NotCast };

    // Judged right after the cast call, from the foe's own current cast: the class interrupts are instant and with no
    // travel time, so an interrupt that landed has stopped the cast by then.
    inline Outcome Judge(bool cast, bool stillCastingSame)
    {
        if (!cast)
            return Outcome::NotCast;
        return stillCastingSame ? Outcome::Missed : Outcome::Interrupted;
    }

    inline const char* OutcomeName(Outcome outcome)
    {
        switch (outcome)
        {
            case Outcome::Interrupted: return "interrupted";
            case Outcome::Missed: return "missed";
            default: return "not_cast";
        }
    }

    // Whether a stamped run action may act on a plan: the plan's run stamp is the run's stamp now, and the action's.
    // A plan of an older generation or plan revision does nothing, as every run action.
    inline bool PlanCurrent(uint32_t planGeneration, uint32_t planRevision, uint32_t runGeneration, uint32_t runRevision,
        uint32_t actionGeneration, uint32_t actionRevision)
    {
        return planGeneration && planGeneration == runGeneration && planRevision == runRevision
            && actionGeneration == runGeneration && actionRevision == runRevision;
    }

    // The order members are asked in for an interrupt: those that deal damage first, then the tank, then the healer last,
    // whose mana the party needs.
    enum class Role : uint8_t { Dps, Tank, Healer };

    inline int RoleRank(Role role)
    {
        return role == Role::Dps ? 0 : role == Role::Tank ? 1 : 2;
    }
}
