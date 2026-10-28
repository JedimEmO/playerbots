#pragma once

#include "Common.h"

#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

class Player;
namespace ai { class TravelDestination; }

// The guild's own jobs for the competition (docs/competition.md): paying for slots and features,
// resetting a raid's lockout, crafting the items the features cost, and kitting a character from the guild's vault.
// GuildBotMgr holds the orders and reports; these jobs only say, at each check, how the order stands. See GUILD-JOBS.md.
//
//   pay     gold=<copper> items=<entry>:<count>,... ref=<number>   hands gold and items in; they leave the game
//           at=guild_master|emissary                               at a counter: travels to the nearest one first
//   reset   map=<raid map id>                                      unbinds the account's characters from one raid
//   craft   item=<entry> | spell=<recipe>, amount=<n>              makes items with a recipe it knows, where it stands
//   kit     loadout=<name> items=<entry>:<count>,...               brings what it carries up to the counts: from the
//                                                                  vault, then from vendors near it, with its own gold
//   deposit items=<entry>:<count>,... (0 for all it carries)       puts items in the guild's vault
//
// Each one acts only alive, in the world and out of combat. The character stands where it is meanwhile,
// but for a pay order with at=, which travels to its counter: a capital's Guild Master, or the emissary beside one;
// and kit and deposit, which travel to the vaultkeeper beside a Guild Master, and kit to the vendors it buys from.
// Without the core's guild vault (Guilds/GuildVault.h), kit buys from vendors only and deposit fails.
class GuildJobs
{
public:
    static GuildJobs& instance()
    {
        static GuildJobs instance;
        return instance;
    }

    // True for the jobs this file runs.
    static bool IsJob(const std::string& job);

    // Items every guild character keeps, whatever its job: it loots them, and never sells, banks or destroys them.
    // The reset currency, and the crafts the guild's features and budget increases cost.
    static bool AlwaysKeep(uint32 itemId);

    // How the order stands after this check: "working", "done" or "failed", with the reason,
    // and the events to record for the character.
    struct Update
    {
        std::string status = "working";
        std::string reason;
        std::vector<std::pair<std::string, std::string>> events;
    };

    // One check of a guild character the engine plays, in the world and not between maps, on one of these jobs.
    Update Tick(Player* bot, const std::string& job, const std::string& args, uint32 jobSeq);

    // An order that goes somewhere, a pay order at a counter (at=guild_master or at=emissary), kit or deposit:
    // the character travels there, through the travel named "guild job".
    static bool Travels(const std::string& job, const std::string& args);
    // The items a kit or deposit order names: the character keeps them while the order runs, as AlwaysKeep's.
    static bool KeepsForJob(const std::string& job, const std::string& args, uint32 itemId);
    // For the travel named "guild job": true while the character's order has a place it is not yet within talking
    // range of: a pay counter, a vaultkeeper or a vendor. The order chooses the place at its checks.
    static bool ShouldTravel(Player* bot);
    // That place, at its spawn, living as long as the server; null when there is none.
    static ai::TravelDestination* Destination(Player* bot);

private:
    GuildJobs() = default;

    // What a character's order has done so far, for the order it was counted for.
    struct Progress
    {
        uint32 seq = 0;
        bool started = false;      // the first check of this order has run
        uint32 made = 0;           // craft: items made on this order
        uint32 lastCount = 0;      // craft: the made item in its bags at the last check
        uint32 castFailures = 0;   // craft: casts in a row that did not start
        float nearest = 0.0f;      // pay at a counter: the nearest it came to it on its map, 0 before any
        uint32 nearestMap = 0;     // the map that was on
        time_t nearestAt = 0;      // when it came that near or moved on to another map; 0 to start again
        // kit and deposit
        uint8 stage = 0;                    // kit: where it is in the order, a KitStage
        std::string guild;                  // the guild key of its vault, empty for none
        std::map<uint32, uint32> took;      // kit: taken from the vault, by item
        std::map<uint32, uint32> bought;    // kit: bought from vendors, by item
        uint32 spent = 0;                   // kit: copper spent on them
        int vendor = -1;                    // kit: the vendor it is going to, -1 for none
        std::set<int> visited;              // kit: the vendors it bought from or gave up on
        uint32 approaches = 0;              // kit: checks spent stepping up to the vendor
        bool bagsFull = false;              // kit: the bags took no more
        bool noGold = false;                // kit: it could not pay for one more of what a vendor near it sells
        std::string note;                   // kit: a place it could not reach
    };

    Update Pay(Player* bot, const std::string& args, Progress& p);
    Update Reset(Player* bot, const std::string& args);
    Update Craft(Player* bot, const std::string& args, Progress& p);
    Update Kit(Player* bot, const std::string& args, Progress& p);
    Update Deposit(Player* bot, const std::string& args, Progress& p);

    void EnsureTable();
    uint32 LoadMade(uint32 guid, uint32 seq);
    void SaveMade(uint32 guid, uint32 seq, uint32 made);

    std::map<uint32, Progress> progress;   // per character
    bool tableReady = false;
};

#define sGuildJobs GuildJobs::instance()
