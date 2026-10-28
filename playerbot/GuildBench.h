#pragma once

#include "Common.h"

#include <list>
#include <string>

class Player;

// A test bench for game masters: it makes guild characters ready for a raid test in seconds.
// It raises a character's level with its class spells and talents for its role,
// equips level-appropriate gear, and hands out food, reagents, ammunition, bags and a hunter's pet.
//
// This is not how a guild character gets anything in play. Guild characters have no cheats:
// they earn their levels, buy their spells and carry what they collected. Everything here comes
// from nothing, through upstream's random bot factory, so the tool is for GM test benches only,
// reached through the GM console command `.rndbot guildbot bench`, and never by the guild service.
class GuildBench
{
public:
    // The `bench` command; args is what follows `bench`. Returns the lines to print.
    static std::list<std::string> HandleCommand(const std::string& args);

private:
    static std::list<std::string> Help();
    static std::list<std::string> Level(Player* bot, const std::string& name, uint32 level);
    static std::list<std::string> Gear(Player* bot, const std::string& quality);
    static std::list<std::string> Consumables(Player* bot);
    static std::list<std::string> Bags(Player* bot);
    static std::list<std::string> Pet(Player* bot);
    // The keys for the dungeon named, or with none named, the one the character's order names.
    static std::list<std::string> Keys(Player* bot, const std::string& dungeonName = "");
};
