
#include "playerbot/playerbot.h"
#include "ConserveManaStrategy.h"
#include "playerbot/PlayerbotAIConfig.h"
#include "playerbot/strategy/actions/GenericSpellActions.h"
#include "playerbot/strategy/values/LastSpellCastValue.h"
#include "playerbot/ServerFacade.h"

using namespace ai;

float ConserveManaMultiplier::GetValue(Action* action)
{
    if (action == NULL) return 1.0f;

    uint8 health = AI_VALUE2(uint8, "health", "self target");
    uint8 targetHealth = AI_VALUE2(uint8, "health", "current target");
    uint8 mana = AI_VALUE2(uint8, "mana", "self target");
    bool hasMana = AI_VALUE2(bool, "has mana", "self target");
    bool mediumMana = hasMana && mana < sPlayerbotAIConfig.mediumMana;

    std::string name = action->getName();

    if (health < sPlayerbotAIConfig.lowHealth)
        return 1.0f;

    Unit* target = ai->GetUnit(AI_VALUE(ObjectGuid, "current target"));
    if (action->GetTarget() != target)
        return 1.0f;

    CastSpellAction* spellAction = dynamic_cast<CastSpellAction*>(action);
    if (!spellAction)
        return 1.0f;

    std::string spell = spellAction->getName();
    uint32 spellId = AI_VALUE2(uint32, "spell id", spell);
    const SpellEntry* const spellInfo = sServerFacade.LookupSpellInfo(spellId);
    if (!spellInfo || spellInfo->powerType != POWER_MANA)
        return 1.0f;

    if (mediumMana && dynamic_cast<CastBuffSpellAction*>(action))
        return 0.0f;

    if (target)
    {
        if (((int)target->GetLevel() - (int)bot->GetLevel()) >= 0)
            return 1.0f;
    }

    return 1.0f;
}

float SaveManaMultiplier::GetValue(Action* action)
{
    if (action == NULL)
        return 1.0f;

    if (action->GetTarget() != ai->GetUnit(AI_VALUE(ObjectGuid, "current target")))
        return 1.0f;

    double saveLevel = AI_VALUE(double, "mana save level");
    if (saveLevel <= 1.0)
        return 1.0f;

    CastSpellAction* spellAction = dynamic_cast<CastSpellAction*>(action);
    if (!spellAction)
        return 1.0f;

    std::string spell = spellAction->getName();
    uint32 spellId = AI_VALUE2(uint32, "spell id", spell);
    const SpellEntry* const spellInfo = sServerFacade.LookupSpellInfo(spellId);
    if (!spellInfo || spellInfo->powerType != POWER_MANA)
        return 1.0f;

    int32 cost = spellInfo->manaCost;
    if (!cost)
        return 1.0f;

    time_t lastCastTime = AI_VALUE2(time_t, "last spell cast time", spell);
    if (!lastCastTime)
        return 1.0f;

    time_t elapsed = time(0) - lastCastTime;
    if ((double)elapsed < 10 * saveLevel)
        return 0.0f;

    return 1.0f;
}

void ConserveManaStrategy::InitCombatMultipliers(std::list<Multiplier*> &multipliers)
{
    multipliers.push_back(new ConserveManaMultiplier(ai));
}

// The mana at which a healer starts saving, AzerothCore's Playerbots.SaveManaThreshold.
static constexpr float SAVE_MANA_THRESHOLD = 60.0f;

float HealerAutoSaveManaMultiplier::GetValue(Action* action)
{
    CastHealingSpellAction* heal = dynamic_cast<CastHealingSpellAction*>(action);
    if (!heal)
        return 1.0f;

    if (!bot->GetMaxPower(POWER_MANA) || bot->GetPowerPercent(POWER_MANA) > SAVE_MANA_THRESHOLD)
        return 1.0f;

    Unit* target = action->GetTarget();
    if (!target || !target->IsAlive())
        return 1.0f;

    // A member under low health is in danger and gets whatever heal comes. AzerothCore still passes over the least
    // efficient heals for one that is not a tank; here nobody in danger waits on a heal.
    const float health = target->GetHealthPercent();
    if (health < sPlayerbotAIConfig.lowHealth)
        return 1.0f;

    // A shield, or a heal the core scripts, has no estimate and is left alone.
    const uint32 estimate = heal->EstimateHeal(target);
    if (!estimate)
        return 1.0f;

    // Overhealing. AzerothCore divides a tank's estimate by 1.5, as its estimates are shares of an average member's
    // health and a tank has more. These are in health points, so the factor stays only as leeway for the member whose
    // health drops fastest: a tank's heal may be half as large again as its missing health.
    const bool tank = target->IsPlayer() && PlayerbotAI::IsTank(static_cast<Player*>(target));
    const float missing = float(target->GetMaxHealth() - target->GetHealth());
    if ((tank ? estimate / 1.5f : float(estimate)) > missing)
        return 0.0f;

    // A costly heal. Above medium health only the efficient heals go out, below it all but the least efficient.
    const HealingManaEfficiency efficiency = heal->GetManaEfficiency();
    if (health >= sPlayerbotAIConfig.mediumHealth ? efficiency <= HealingManaEfficiency::MEDIUM : efficiency <= HealingManaEfficiency::LOW)
        return 0.0f;

    return 1.0f;
}

void HealerAutoSaveManaStrategy::InitCombatMultipliers(std::list<Multiplier*> &multipliers)
{
    multipliers.push_back(new HealerAutoSaveManaMultiplier(ai));
}

void HealerAutoSaveManaStrategy::InitNonCombatMultipliers(std::list<Multiplier*> &multipliers)
{
    multipliers.push_back(new HealerAutoSaveManaMultiplier(ai));
}
