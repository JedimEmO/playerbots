
#include "playerbot/playerbot.h"
#include "MoltenCoreDungeonTriggers.h"

using namespace ai;
using namespace ai::MoltenCore;

Unit* GolemaggTrigger::Golemagg()
{
    return ai->GetUnit(AI_VALUE2(ObjectGuid, "find target", "golemagg the incinerator"));
}

bool McGolemaggMarkBossTrigger::IsActive()
{
    return PlayerbotAI::IsTank(bot) && Golemagg();
}

bool McGolemaggIsMainTankTrigger::IsActive()
{
    return PlayerbotAI::IsMainTank(bot) && Golemagg();
}

bool McGolemaggIsAssistTankTrigger::IsActive()
{
    return PlayerbotAI::IsAssistTank(bot) && Golemagg();
}

bool McGolemaggIsHealerTrigger::IsActive()
{
    return PlayerbotAI::IsHeal(bot) && Golemagg();
}

bool McGolemaggMagmaSplashTrigger::IsActive()
{
    if (PlayerbotAI::IsTank(bot))
        return false;

    Unit* boss = Golemagg();
    if (!boss || boss->GetHealthPercent() <= GOLEMAGG_BURN_HEALTH_PCT)
        return false;

    SpellAuraHolder* splash = bot->GetSpellAuraHolder(SPELL_MAGMA_SPLASH);
    if (!splash || splash->GetStackAmount() < MAGMA_SPLASH_BACK_OFF_STACKS)
        return false;

    // Only while still in reach; once out, the multiplier keeps the bot from walking back in.
    return bot->GetDistance2d(boss->GetPositionX(), boss->GetPositionY(), DIST_CALC_NONE) < MAGMA_SPLASH_BACK_OFF_DISTANCE;
}