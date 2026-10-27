#include "playerbot/playerbot.h"
#include "UnstuckAction.h"
#include "playerbot/GuildBotMgr.h"

bool UnstuckAction::Execute(Event& event)
{
    std::string source = event.getSource();
    Player* bot = ai->GetBot();
    Player* master = ai->GetMaster();

    // A guild character with its party only resets: a hearthstone or a release would take it out of the dungeon,
    // or away from the stone, to the other end of the world. The dungeon job sees to a party that cannot go on.
    if (bot->IsAlive() && sGuildBotMgr.StaysWithParty(bot))
    {
        ai->TellDebug(master, "Unstuck: with the party, resetting.", "debug unstuck");
        return ai->DoSpecificAction("reset", event, true);
    }

    // A living guild character is never released: upstream's repop puts it at its race's starting spot,
    // a level 1 zone a continent away. It goes home by hearthstone if that is ready, or resets.
    const bool guildCharacter = sGuildBotMgr.IsGuildBot(bot);
    auto longStuck = [&]()
    {
        if (AI_VALUE2(bool, "action useful", "hearthstone") && bot->IsAlive())
            return ai->DoSpecificAction("hearthstone", event, true);
        if (guildCharacter && bot->IsAlive())
            return ai->DoSpecificAction("reset", event, true);
        return ai->DoSpecificAction("repop", event, true);
    };

    // Default action if no specific source is matched
    if (source.empty())
    {
        ai->TellDebug(master, "Unstuck: No specific source, resetting.", "debug unstuck");
        return ai->DoSpecificAction("reset", event, true);
    }

    // Handle move stuck scenarios
    if (source.find("move stuck") != std::string::npos)
    {
        ai->TellDebug(master, "Unstuck: Move stuck detected, resetting.", "debug unstuck");
        return ai->DoSpecificAction("reset", event, true);
    }

    // Handle long move stuck scenarios
    if (source.find("move long stuck") != std::string::npos)
    {
        ai->TellDebug(master, "Unstuck: Long move stuck detected, attempting hearthstone or repop.", "debug unstuck");
        return longStuck();
    }

    // Handle combat stuck scenarios
    if (source.find("combat stuck") != std::string::npos)
    {
        ai->TellDebug(master, "Unstuck: Combat stuck detected, resetting position.", "debug unstuck");
        return ai->DoSpecificAction("reset", event, true);
    }

    // Handle long combat stuck scenarios
    if (source.find("combat long stuck") != std::string::npos)
    {
        ai->TellDebug(master, "Unstuck: Long combat stuck detected, attempting hearthstone or repop.", "debug unstuck");
        return longStuck();
    }

    // Fallback to reset if no specific condition is met
    ai->TellDebug(master, "Unstuck: Fallback to reset action.", "debug unstuck");
    return ai->DoSpecificAction("reset", event, true);
}
