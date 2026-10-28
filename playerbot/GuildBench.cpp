#include "playerbot/playerbot.h"
#include "playerbot/GuildBench.h"
#include "playerbot/GuildBotMgr.h"
#include "playerbot/GuildDungeon.h"
#include "playerbot/PlayerbotFactory.h"
#include "playerbot/Talentspec.h"
#include "playerbot/strategy/actions/ChangeTalentsAction.h"

#include <sstream>

using namespace ai;

// Everything below gives a character what a guild character never gets in play: levels, spells, items.
// It is for game masters building a test bench; see GuildBench.h.

namespace
{
    std::string Trim(std::string text)
    {
        size_t start = text.find_first_not_of(" \t");
        size_t end = text.find_last_not_of(" \t");
        return start == std::string::npos ? "" : text.substr(start, end - start + 1);
    }

    // 0 lets the factory choose by level, as it does for random bots; -1 for a word it does not know.
    int ParseQuality(const std::string& word)
    {
        if (word.empty())
            return 0;
        if (word == "green" || word == "uncommon")
            return ITEM_QUALITY_UNCOMMON;
        if (word == "blue" || word == "rare")
            return ITEM_QUALITY_RARE;
        if (word == "epic" || word == "purple")
            return ITEM_QUALITY_EPIC;
        return -1;
    }
}

std::list<std::string> GuildBench::Help()
{
    return {
        "guildbot bench: a test bench for game masters. It gives a guild character levels, spells, gear and items",
        "from nothing, which guild characters never get in play (they have no cheats). Test benches only.",
        "  bench level <name> <level>                 raise to a level, with class spells, skills and talents for its role",
        "  bench gear <name> [green|blue|epic]        equip gear for its level and talent tree; a worn item it rates lower is destroyed",
        "  bench consumables <name>                   food, drink, reagents, ammunition, oils and potions for its class",
        "  bench bags <name>                          a Traveler's Backpack in every empty bag slot",
        "  bench pet <name>                           a hunter's pet with its spells",
        "  bench keys <name>                          the keys to the locked doors of the dungeon its order names",
        "  bench setup <name> <level> [quality]       all of the above, in that order",
        "All but level need the character online and played by the engine; level works offline as a level change only.",
    };
}

std::list<std::string> GuildBench::HandleCommand(const std::string& args)
{
    std::istringstream in(args);
    std::string sub, name;
    in >> sub >> name;
    std::string rest;
    std::getline(in, rest);
    rest = Trim(rest);

    if (sub.empty() || sub == "help" || name.empty())
        return Help();

    if (!normalizePlayerName(name))
        return { "no character named " + name };

    ObjectGuid guid = sObjectMgr.GetPlayerGuidByName(name);
    if (!guid)
        return { "no character named " + name };

    if (!sGuildBotMgr.IsGuildCharacter(guid.GetCounter()))
        return { name + " is not a guild character; the bench works only on guild characters" };

    Player* bot = sObjectMgr.GetPlayer(guid);
    bool online = bot && bot->IsInWorld();

    std::istringstream restIn(rest);
    std::string first, second;
    restIn >> first >> second;

    if (sub == "level" || sub == "setup")
    {
        uint32 level = uint32(atoi(first.c_str()));
        uint32 maxLevel = sWorld.getConfig(CONFIG_UINT32_MAX_PLAYER_LEVEL);
        if (!level || level > maxLevel)
            return { "usage: guildbot bench " + sub + " <name> <level 1 to " + std::to_string(maxLevel) + ">" + (sub == "setup" ? " [quality]" : "") };

        if (!online && sub == "level")
        {
            // As the core's .character level does for a character that is not in the world.
            auto result = CharacterDatabase.PQuery("SELECT level FROM characters WHERE guid = %u", guid.GetCounter());
            uint32 oldLevel = result ? result->Fetch()[0].GetUInt32() : 0;
            if (level <= oldLevel)
                return { "the bench only raises levels; " + name + " is already level " + std::to_string(oldLevel) };

            CharacterDatabase.DirectPExecute("UPDATE characters SET level = %u, xp = 0 WHERE guid = %u", level, guid.GetCounter());
            return { name + " is offline: its level is now " + std::to_string(level) + ", without spells or talents;",
                     "run bench level again once the engine has logged it in" };
        }
    }

    if (!online)
        return { name + " is offline; switch it on and wait for the engine to log it in" };

    // The factory needs the bot's AI, and a player on the character is the player's business.
    if (!bot->GetPlayerbotAI())
        return { "a player is playing " + name + "; the bench works only while the engine plays it" };

    if (sub == "level")
        return Level(bot, name, uint32(atoi(first.c_str())));
    if (sub == "gear")
        return Gear(bot, first);
    if (sub == "consumables")
        return Consumables(bot);
    if (sub == "bags")
        return Bags(bot);
    if (sub == "pet")
        return Pet(bot);
    if (sub == "keys")
        return Keys(bot);

    if (sub == "setup")
    {
        if (ParseQuality(second) < 0)
            return { "unknown quality " + second + "; use green, blue or epic" };

        // Gear follows the talent tree and needs bag space for what it replaces, so the order matters.
        std::list<std::string> out = Level(bot, name, uint32(atoi(first.c_str())));
        if (bot->GetLevel() != uint32(atoi(first.c_str())))
            return out;
        out.splice(out.end(), Bags(bot));
        out.splice(out.end(), Gear(bot, second));
        out.splice(out.end(), Consumables(bot));
        out.splice(out.end(), Pet(bot));
        out.splice(out.end(), Keys(bot));
        return out;
    }

    return Help();
}

std::list<std::string> GuildBench::Level(Player* bot, const std::string& name, uint32 level)
{
    uint32 oldLevel = bot->GetLevel();
    if (level < oldLevel)
        return { "the bench only raises levels; " + name + " is already level " + std::to_string(oldLevel) };

    // As the core's .character level does for a character in the world.
    if (level > oldLevel)
    {
        bot->GiveLevel(level);
        bot->SetUInt32Value(PLAYER_XP, 0);
    }
    // The talent points for the level, also for a character raised some other way, which may have too few.
    bot->InitTalentForLevel();

    // A bench character is built for its role: talents spent in a tree that does not suit it are reset, for free,
    // and the path picked earlier is forgotten, so the picks below follow the role.
    BotRoles role = sGuildBotMgr.GetRole(bot->GetGUIDLow());
    std::set<int> trees = ChangeTalentsAction::RoleTrees(bot->getClass(), role);
    TalentSpec spent(bot);
    if (spent.GetTalentPoints() > 0 && !trees.empty() && !trees.count(spent.highestTree()))
    {
        bot->resetTalents(true);
        sRandomPlayerbotMgr.SetValue(bot->GetGUIDLow(), "specNo", 0);
        sRandomPlayerbotMgr.SetValue(bot->GetGUIDLow(), "specLink", 0, "");
    }

    // What a class trainer would teach by this level, without the cost; and weapon skills at their maximum.
    bot->learnDefaultSpells();
#ifndef MANGOSBOT_ONE
    bot->learnClassLevelSpells(true);
#endif
    bot->UpdateSkillsForLevel(true);

    // Talents along the role's premade path, as the engine picks them for a guild character.
    std::ostringstream talentOut;
    ChangeTalentsAction::AutoSelectTalents(bot, &talentOut, role);

    // Combat strategies follow the talent tree.
    bot->GetPlayerbotAI()->ResetStrategies();
    bot->SaveToDB();

    std::ostringstream out;
    out << name << " is level " << bot->GetLevel() << ", with its class spells and " << TalentSpec(bot).GetTalentPoints()
        << " talent points spent";
    if (uint32 free = bot->GetFreeTalentPoints())
        out << " (" << free << " unspent: no premade path for its role and class)";
    return { out.str() };
}

std::list<std::string> GuildBench::Gear(Player* bot, const std::string& qualityWord)
{
    int quality = ParseQuality(qualityWord);
    if (quality < 0)
        return { "unknown quality " + qualityWord + "; use green, blue or epic" };

    PlayerbotAI* ai = bot->GetPlayerbotAI();
    uint32 before = ai->GetEquipGearScore(bot, false, false);

    // Upstream's random bot gear, for the bot's level and the talent tree it has spent most in.
    // Incremental: an item already worn stays unless the factory rates the new one higher,
    // and then the old one is destroyed, not put in the bags. Mind that on a character with earned gear.
    PlayerbotFactory factory(bot, bot->GetLevel(), uint32(quality));
    factory.UpgradeGear(false);
    bot->SaveToDB();

    uint32 after = ai->GetEquipGearScore(bot, false, false);
    return { std::string(bot->GetName()) + " is geared: average item level " + std::to_string(before) + " before, " + std::to_string(after) + " now" };
}

std::list<std::string> GuildBench::Consumables(Player* bot)
{
    PlayerbotFactory factory(bot, bot->GetLevel());
    factory.AddFood();
    factory.AddReagents();
    factory.AddPotions();
    factory.InitAmmo();
    factory.AddConsumes();
    bot->SaveToDB();
    return { std::string(bot->GetName()) + " has food, drink, reagents, ammunition, oils and potions for its class and level, as far as its bags hold them" };
}

std::list<std::string> GuildBench::Bags(Player* bot)
{
    // As the factory's InitBags, which is not public.
    uint32 added = 0;
    for (uint8 slot = INVENTORY_SLOT_BAG_START; slot < INVENTORY_SLOT_BAG_END; ++slot)
    {
        if (!bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot) && bot->StoreNewItemInBestSlots(4500, 1))
            ++added;
    }

    bot->SaveToDB();
    return { std::string(bot->GetName()) + " got " + std::to_string(added) + " Traveler's Backpacks" };
}

std::list<std::string> GuildBench::Keys(Player* bot)
{
    // The keys a player brings for the dungeon: the bench has no quest chain to earn them by.
    const std::string dungeon = sGuildBotMgr.GetDungeonOrder(bot->GetGUIDLow());
    if (dungeon.empty())
        return { std::string(bot->GetName()) + " has no dungeon order; no keys" };
    std::vector<uint32> keys = GuildDungeon::DungeonKeys(dungeon);
    if (keys.empty())
        return { std::string(bot->GetName()) + " needs no keys for " + dungeon };
    uint32 added = 0;
    for (uint32 key : keys)
        if (!bot->HasItemCount(key, 1, true) && bot->StoreNewItemInBestSlots(key, 1))
            ++added;
    bot->SaveToDB();
    return { std::string(bot->GetName()) + " got " + std::to_string(added) + " of the " + std::to_string(keys.size()) + " keys for " + dungeon };
}

std::list<std::string> GuildBench::Pet(Player* bot)
{
    if (bot->getClass() != CLASS_HUNTER)
        return { std::string(bot->GetName()) + " is not a hunter; only hunters get a pet here, and warlocks summon their own" };

    PlayerbotFactory factory(bot, bot->GetLevel());
    factory.InitPet();
    factory.InitPetSpells();
    bot->SaveToDB();
    return { std::string(bot->GetName()) + (bot->GetPet() ? " has a pet: " + std::string(bot->GetPet()->GetName()) : " has no pet; no tameable creature fits its level") };
}
