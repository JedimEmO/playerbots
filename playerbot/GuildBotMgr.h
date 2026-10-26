#pragma once

#include "PlayerbotMgr.h"
#include "ChatHelper.h"

#include <map>
#include <string>

namespace ai { class Engine; }

// Guild characters: a player's own characters, which the server plays while the player is not
// playing them, including while the player is offline. They are not random bots: nothing about them
// is randomised, and they have none of the random bots' habits. Each one has a job, an order the guild
// service gives it, and reports what it is doing as data.
//
// The guild service talks to the engine through three tables in the characters database, which this
// manager creates:
//   guildbot_character     the orders: which characters are guild characters, whether they are switched on,
//                          and their job. A new order is a new job_seq.
//   guildbot_state         what each one is doing now, written every few seconds.
//   guildbot_event         things that happened, appended: logins, takeovers, levels, deaths, jobs done.
class GuildBotMgr : public PlayerbotHolder
{
public:
    static GuildBotMgr& instance()
    {
        static GuildBotMgr instance;
        return instance;
    }

    // True while the engine plays this character: it is a switched-on guild character and no player is on it.
    bool IsGuildBot(Player* player) const;
    bool IsGuildBot(uint32 guid) const;
    // A guild character, whoever plays it right now.
    bool IsGuildCharacter(uint32 guid) const { return characters.find(guid) != characters.end(); }

    // The job the character should be doing: its order, or "idle" once that order is done or failed.
    std::string GetActiveJob(uint32 guid) const;
    // The role the player gave the character, which its talents follow; none if not a guild character.
    BotRoles GetRole(uint32 guid) const;
    // The farm job's item and amount (0 for no limit), for the guild order value. False when not farming.
    bool GetFarmOrder(uint32 guid, uint32& itemId, uint32& amount) const;
    // True for the item the character's job is collecting, which it must keep.
    bool IsJobItem(uint32 guid, uint32 itemId) const;

    // Called from AiFactory: the non-combat strategies of a guild character, from its job.
    void AddNonCombatStrategies(Player* player, ai::Engine* engine) const;

    // Ticks, driven from RandomPlayerbotMgr, since the core only ticks that holder.
    void Tick(uint32 elapsed);

    // A player logged in to a character the engine was playing; it is theirs until they log out.
    void OnPlayerTakeover(Player* player);
    // True when OnBotLogin must not give this character an AI: a player is on it.
    bool IsPlayerControlled(Player* player) const;
    void OnPlayerLogout(Player* player);

    std::list<std::string> HandleCommand(std::string args);

    void LogoutAll();

protected:
    void OnBotLoginInternal(Player* const bot) override;
    uint32 GetOrCreateAccount(Player* master, std::string& error) override;

private:
    GuildBotMgr() = default;

    struct Character
    {
        uint32 guid = 0;
        uint32 account = 0;
        std::string name;
        bool enabled = false;
        std::string job = "idle";
        std::string jobArgs;
        uint32 jobSeq = 0;
        std::string role;           // tank, healer, dps, or empty for the class default

        // Engine side.
        uint32 appliedSeq = 0;      // the order the AI's strategies were built for
        std::string status = "working";
        std::string reason;
        bool playerControlled = false;
        time_t offlineSince = 0;    // when it went offline, for the relogin delay
        time_t loginRequested = 0;
        uint32 lastLevel = 0;
        bool roleChanged = false;   // the role column changed since the talents were last steered by it
        std::string roleStatus;     // empty, or "respec needed" when talents are spent in another role's tree
        uint64 lastProgress = 0;     // level and experience, as one number
        time_t progressAt = 0;       // when it last changed
        bool stalled = false;
        bool dead = false;
        std::string partyLeader;    // the leader it joined for its party job, to leave when the job ends
    };

    void EnsureTables();
    void LoadOrders();
    void LoginLogout();
    void ApplyOrders();
    void CheckJobs();
    void WriteState();
    void WriteState(const Character& c, Player* bot);
    void ApplyRole(Character& c, Player* bot);
    // The party job: be in the leader's group. Returns why not, or empty when it is.
    std::string KeepParty(Character& c, Player* bot);
    void LeaveParty(Character& c, Player* bot);

    void Event(uint32 guid, const std::string& kind, const std::string& detail = "");
    Character* Find(const std::string& name);
    static std::string Escape(std::string text);
    std::string ArgValue(const Character& c, const std::string& key) const;

    std::list<std::string> HandleAdd(const std::string& args);
    std::list<std::string> HandleCreate(const std::string& args);
    std::list<std::string> HandleList();
    std::list<std::string> HandleState(const std::string& args);
    std::list<std::string> HandleOrder(const std::string& sub, const std::string& args);

    std::map<uint32, Character> characters;
    bool tablesReady = false;
    uint32 sinceOrders = 0;
    uint32 sinceState = 0;
    uint32 createAccount = 0;
};

#define sGuildBotMgr GuildBotMgr::instance()
