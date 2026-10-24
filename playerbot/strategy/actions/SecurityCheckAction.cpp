
#include "playerbot/playerbot.h"
#include "playerbot/RandomPlayerbotMgr.h"
#include "playerbot/PlayerbotAIConfig.h"
#include "SecurityCheckAction.h"

using namespace ai;

bool SecurityCheckAction::isUseful()
{
    // A random bot following a real master plays as an alt bot would (AiPlayerbot.FreeBotsFollowRealMaster), and alt
    // bots have no loot rule. Without this the rule adds 'passive' and 'stay' to both engines under free for all,
    // master loot or a threshold above uncommon unless the bot is in the player's guild, and the bot stops fighting.
    // AiPlayerbot.RealPlayersCommandFreeBots turns the rule off for every free bot.
    if (sPlayerbotAIConfig.realPlayersCommandFreeBots || ai->IsFreeBotFollowingRealMaster())
        return false;

    return sRandomPlayerbotMgr.IsRandomBot(bot) && ai->GetMaster() && ai->GetMaster()->GetSession()->GetSecurity() < SEC_GAMEMASTER && !ai->GetMaster()->GetPlayerbotAI();
}

bool SecurityCheckAction::Execute(Event& event)
{
    Player* requester = event.getOwner() ? event.getOwner() : GetMaster();
    Group* group = bot->GetGroup();
    if (group)
    {
        LootMethod method = group->GetLootMethod();
        ItemQualities threshold = group->GetLootThreshold();
        if (method == MASTER_LOOT || method == FREE_FOR_ALL || threshold > ITEM_QUALITY_UNCOMMON)
        {
            if ((ai->GetGroupMaster()->GetSession()->GetSecurity() == SEC_PLAYER) && (!bot->GetGuildId() || bot->GetGuildId() != ai->GetGroupMaster()->GetGuildId()))
            {
                ai->TellError(requester, "I will play with this loot type only if I'm in your guild :/");
                ai->ChangeStrategy("+passive,+stay", BotState::BOT_STATE_NON_COMBAT);
                ai->ChangeStrategy("+passive,+stay", BotState::BOT_STATE_COMBAT);
            }
            return true;
        }
    }
    return false;
}
