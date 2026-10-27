
#include "playerbot/playerbot.h"
#include "DpsTargetValue.h"
#include "LeastHpTargetValue.h"

#include "playerbot/PlayerbotAIConfig.h"

using namespace ai;

namespace
{
    // AzerothCore's dps target: attackers judged by how long each will live under the group's damage.
    // Players are passed over, as by the least health target, so pvp keeps its own targeting.
    class LifetimeTargetStrategy : public FindNonCcTargetStrategy
    {
    public:
        LifetimeTargetStrategy(PlayerbotAI* ai, float dps) : FindNonCcTargetStrategy(ai), dps(std::max(dps, 1.0f))
        {
            currentTarget = ai->GetUnit(ai->GetAiObjectContext()->GetValue<ObjectGuid>("current target")->Get());
        }

        void CheckAttacker(Unit* attacker, ThreatManager* threatManager) override
        {
            if (attacker->IsPlayer() || !attacker->IsAlive() || IsCcTarget(attacker))
                return;

            if (!result || IsBetter(attacker, result))
                result = attacker;
        }

    protected:
        virtual bool IsBetter(Unit* candidate, Unit* best) = 0;

        float Lifetime(Unit* unit) const { return unit->GetHealth() / dps; }

        bool InRange(Unit* unit) const
        {
            Player* bot = ai->GetBot();
            float range = ai->IsRanged(bot) ? sPlayerbotAIConfig.spellDistance : sPlayerbotAIConfig.meleeDistance;
            return bot->GetDistance(unit) < range + 5.0f;
        }

        bool Nearer(Unit* candidate, Unit* best) const
        {
            Player* bot = ai->GetBot();
            return bot->GetDistance(candidate) < bot->GetDistance(best);
        }

        float dps;
        Unit* currentTarget = nullptr;
    };

    // In range before out of it; in range, the one that dies soonest; out of range, the nearest.
    class GeneralLifetimeStrategy : public LifetimeTargetStrategy
    {
    public:
        using LifetimeTargetStrategy::LifetimeTargetStrategy;

    protected:
        bool IsBetter(Unit* candidate, Unit* best) override
        {
            bool candidateInRange = InRange(candidate);
            if (candidateInRange != InRange(best))
                return candidateInRange;

            return candidateInRange ? Lifetime(candidate) < Lifetime(best) : Nearer(candidate, best);
        }
    };

    // Casters, in range before out of it, prefer a target with 5 to 30 seconds to live, then one about to die,
    // then one that will live long, so a long cast does not open on a target about to die.
    // Among targets about to die they keep the current one, else take the one that lives longest.
    class CasterLifetimeStrategy : public LifetimeTargetStrategy
    {
    public:
        using LifetimeTargetStrategy::LifetimeTargetStrategy;

    protected:
        bool IsBetter(Unit* candidate, Unit* best) override
        {
            int candidateLevel = Level(candidate);
            int bestLevel = Level(best);
            if (candidateLevel != bestLevel)
                return candidateLevel > bestLevel;

            if (candidateLevel % 10 != 1)
                return Lifetime(candidate) < Lifetime(best);

            if (best == currentTarget)
                return false;
            if (candidate == currentTarget)
                return true;

            return Lifetime(candidate) > Lifetime(best);
        }

        int Level(Unit* unit) const
        {
            int level = InRange(unit) ? 10 : 0;
            float lifetime = Lifetime(unit);
            if (lifetime >= 5.0f && lifetime <= 30.0f)
                return level + 2;
            if (lifetime > 30.0f)
                return level;
            return level + 1;
        }
    };

    // Rogues and cat druids, in range before out of it, keep their combo target, else take the one that dies soonest;
    // out of range, the nearest.
    class ComboLifetimeStrategy : public LifetimeTargetStrategy
    {
    public:
        using LifetimeTargetStrategy::LifetimeTargetStrategy;

    protected:
        bool IsBetter(Unit* candidate, Unit* best) override
        {
            bool candidateInRange = InRange(candidate);
            if (candidateInRange != InRange(best))
                return candidateInRange;

            if (!candidateInRange)
                return Nearer(candidate, best);

            ObjectGuid comboTarget = ai->GetBot()->GetComboTargetGuid();
            if (best->GetObjectGuid() == comboTarget)
                return false;
            if (candidate->GetObjectGuid() == comboTarget)
                return true;

            return Lifetime(candidate) < Lifetime(best);
        }
    };

    // The group members, the bot included, on its map and within sight of it.
    uint32 NearGroupMemberCount(Player* bot)
    {
        Group* group = bot->GetGroup();
        if (!group)
            return 1;

        uint32 count = 0;
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player* member = ref->getSource();
            if (member && member->IsInWorld() && member->GetMapId() == bot->GetMapId() &&
                (member == bot || member->GetDistance(bot) <= sPlayerbotAIConfig.sightDistance))
                ++count;
        }

        return count;
    }
}

ObjectGuid DpsTargetValue::Calculate()
{
    GuidSet exclusions = GatherStrategyTargetExclusions(ai, TargetValueExclusionType::DpsTarget);
    ObjectGuid rti = RtiTargetValue::Calculate();
    if (rti && !exclusions.count(rti)) return rti;

    float dps = AI_VALUE(float, "estimated group dps");
    Unit* target = nullptr;

    // In a group of more than three, casters and combo classes choose by their own rules.
    if (NearGroupMemberCount(bot) > 3 && ai->IsCaster(bot))
    {
        CasterLifetimeStrategy strategy(ai, dps);
        target = TargetValue::FindTarget(&strategy, exclusions);
    }
    else if (NearGroupMemberCount(bot) > 3 && PlayerbotAI::IsCombo(bot))
    {
        ComboLifetimeStrategy strategy(ai, dps);
        target = TargetValue::FindTarget(&strategy, exclusions);
    }
    else
    {
        GeneralLifetimeStrategy strategy(ai, dps);
        target = TargetValue::FindTarget(&strategy, exclusions);
    }

    return target ? target->GetObjectGuid() : ObjectGuid();
}

class FindMaxHpTargetStrategy : public FindTargetStrategy
{
public:
    FindMaxHpTargetStrategy(PlayerbotAI* ai) : FindTargetStrategy(ai)
    {
        maxHealth = 0;
    }

public:
    virtual void CheckAttacker(Unit* attacker, ThreatManager* threatManager) override
    {
        if (!result || result->GetHealth() < attacker->GetHealth())
            result = attacker;
    }

protected:
    float maxHealth;
};

ObjectGuid DpsAoeTargetValue::Calculate()
{
    GuidSet exclusions = GatherStrategyTargetExclusions(ai, TargetValueExclusionType::DpsTarget);
    ObjectGuid rti = RtiTargetValue::Calculate();
    if (rti && !exclusions.count(rti)) return rti;

    FindMaxHpTargetStrategy strategy(ai);
    Unit* target = TargetValue::FindTarget(&strategy, exclusions);
    return target ? target->GetObjectGuid() : ObjectGuid();
}
