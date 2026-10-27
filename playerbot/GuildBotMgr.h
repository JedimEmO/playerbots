#pragma once

#include "PlayerbotMgr.h"
#include "ChatHelper.h"

#include <map>
#include <set>
#include <vector>
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
//   guildbot_metric        per character, job and hour: seconds played, seconds earning experience, experience, deaths,
//                          and the seconds played split by activity: fighting, looking for a target, and so on.
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
    // A guild character on its own: in no group, or in one with no other member alive near it.
    // Its strategies are then a solo character's, so a healer cut off from its party fights for itself.
    bool IsAlone(Player* bot) const;
    // The farm job's item and amount (0 for no limit), for the guild order value. False when not farming.
    bool GetFarmOrder(uint32 guid, uint32& itemId, uint32& amount) const;
    // True for the item the character's job is collecting, which it must keep.
    bool IsJobItem(uint32 guid, uint32 itemId) const;
    // Whether the character may learn from a profession trainer for this skill: always for a bot that is not a
    // guild character; for one, only ranks of a profession it has, or the profession its learn job names.
    bool MayLearnSkill(Player* bot, uint32 skill) const;
    // The skill a profession name stands for, such as herbalism, or 0.
    static uint32 ProfessionSkill(const std::string& name);
    // The tank order the guild service assigned: 1 is the main tank, 2 the first assist tank, and so on;
    // 0 for none. Any character, the player's own included; read from guildbot_assignment with the orders.
    uint8 GetTankOrder(uint32 guid) const;
    // A creature the character gave up on as out of its reach, for a while; target picking passes over it.
    bool IsIgnoredTarget(Player* bot, ObjectGuid target) const;  // also loot it gave up on
    // Travel destinations, by entry, the character left because it could not work them; set aside for a while.
    std::set<int32> LeftDestinations(Player* bot) const;

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

    // What a character is doing at a check, for the measure; the names are guildbot_metric's columns.
    enum Activity
    {
        ACTIVITY_FIGHT,   // in combat
        ACTIVITY_SEEK,    // at a grind spot, out of combat
        ACTIVITY_TRAVEL,  // on the way to a grind spot
        ACTIVITY_ERRAND,  // to or at a vendor, trainer, repairer, bank, mailbox or the like
        ACTIVITY_REST,    // sitting, to eat or drink
        ACTIVITY_DEAD,
        ACTIVITY_OTHER,
        ACTIVITIES
    };
    static constexpr const char* ACTIVITY_COLUMNS[ACTIVITIES] = { "fight", "seek", "travel", "errand", "rest", "dead", "other" };
    // The activity, and the grind spot it is at or on its way to: the creature entry, or 0.
    static Activity ActivityOf(Player* bot, int32& spot);

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
        time_t recoveredAt = 0;     // when the engine last reset a stalled character
        bool dead = false;
        bool alone = false;         // in a group but with no member near, as its strategies were last built
        std::string partyLeader;    // the leader it joined for its party job, to leave when the job ends
        time_t invitedAt = 0;       // when the dungeon leader last invited its companion

        // The hourly measure, counted every check and written every few minutes; see guildbot_metric.
        uint32 xpLevel = 0, xpValue = 0;  // level and experience at the last check
        time_t xpAt = 0;                  // when it last gained experience
        time_t measuredAt = 0;            // the last check counted
        time_t metricHour = 0;            // the hour the counts below belong to
        std::string metricJob;            // and the job
        uint32 seconds = 0, earning = 0, experience = 0, deaths = 0;
        uint32 activity[ACTIVITIES] = {};  // seconds by Activity
        // The same counts by grind spot, for guildbot_spot: what each spot cost in walking and gave back.
        struct SpotCount { uint32 travel = 0, work = 0, experience = 0, deaths = 0, money = 0, visits = 0; };
        std::map<int32, SpotCount> spots;
        int32 lastSpot = 0;               // the spot at the last check, to count visits
        uint32 lastMoney = 0;             // money at the last check, for the coin looted at a spot
    };

    void EnsureTables();
    void LoadOrders();
    void LoginLogout();
    void ApplyOrders();
    void CheckJobs();
    void WriteState();
    void WriteState(const Character& c, Player* bot);
    void ApplyRole(Character& c, Player* bot);
    void CheckChase(Character& c, Player* bot);
    void CheckLoot(Character& c, Player* bot);
    // Count this check into the hourly measure, and write the counts when the hour or job changes, or every few minutes.
    void Measure(Character& c, Player* bot, bool died);
    void WriteMetric(Character& c);
    std::list<std::string> HandleMetrics(const std::string& args);
    std::list<std::string> HandleSpots(const std::string& args);
    // The party job: be in the leader's group. Returns why not, or empty when it is.
    std::string KeepParty(Character& c, Player* bot);
    // The dungeon leader's companion, `with=<name>`: one of the player's characters, often the player's own,
    // whom it invites to its group the way a client does. Empty, or why it is not in the group yet.
    std::string InviteCompanion(Character& c, Player* bot);
    void LeaveParty(Character& c, Player* bot);

    void Event(uint32 guid, const std::string& kind, const std::string& detail = "");
    Character* Find(const std::string& name);
    static std::string Escape(std::string text);
    std::string ArgValue(const Character& c, const std::string& key) const;

    std::list<std::string> HandleAdd(const std::string& args);
    std::list<std::string> HandleCreate(const std::string& args);
    std::list<std::string> HandleList();
    std::list<std::string> HandleState(const std::string& args);
    std::list<std::string> HandleDebug(const std::string& args);
    std::list<std::string> HandleOrder(const std::string& sub, const std::string& args);
    std::list<std::string> HandleTank(const std::string& args);
    void LoadAssignments();

    std::map<uint32, Character> characters;
    std::map<uint32, uint8> tankOrders;
    struct Chase { ObjectGuid target; uint32 health = 0; time_t since = 0; };
    std::map<uint32, Chase> chases;                             // per character: the target it is after
    struct LootChase { ObjectGuid target; time_t since = 0; };
    std::map<uint32, LootChase> lootChases;                     // per character: the loot it is after
    std::map<uint32, std::map<ObjectGuid, time_t>> ignoredTargets; // per character: given up, until
    std::map<uint32, std::vector<time_t>> giveUps;              // per character: recent give-ups
    std::map<uint32, std::map<int32, time_t>> leftDestinations;  // per character: destination entry, until
    bool assignmentsLoaded = false;   // the first load after a start reports nothing; it is not a change
    bool tablesReady = false;
    uint32 sinceOrders = 0;
    uint32 sinceState = 0;
    uint32 createAccount = 0;
};

#define sGuildBotMgr GuildBotMgr::instance()
