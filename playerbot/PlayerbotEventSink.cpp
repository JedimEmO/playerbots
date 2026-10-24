#include "playerbot/playerbot.h"
#include "PlayerbotEventSink.h"
#include "PlayerbotLLMInterface.h"
#include "playerbot/PlayerbotAIConfig.h"
#include "Groups/Group.h"
#include "Log/Log.h"
#include "Entities/Creature.h"
#include "Globals/ObjectMgr.h"
#include "Maps/Map.h"
#include "Server/DBCStructure.h"

#include <atomic>
#include <ctime>
#include <map>
#include <mutex>
#include <sstream>
#include <system_error>
#include <thread>
#include <utility>

namespace
{
    // Posts that may be waiting on the network at the same time. A webhook that stops answering
    // holds at most this many threads; events beyond it are dropped until some finish.
    constexpr int MaxPostsInFlight = 16;
    std::atomic<int> postsInFlight{0};

    // A boss kill of the same entry in the same instance within this many seconds of a posted one is not posted again.
    // Every player and bot near the kill is sent the packet, and the maps of different instances update on different
    // threads, so the record of recent kills takes a lock.
    constexpr time_t BossKillRepeatSeconds = 60;
    std::mutex recentBossKillsLock;
    std::map<std::pair<uint32, uint32>, time_t> recentBossKills; // (instance id, entry) -> when it was posted

    // True when no kill of this entry in this instance was posted in the last BossKillRepeatSeconds; records this one.
    bool ClaimBossKill(uint32 instanceId, uint32 entry)
    {
        const time_t now = time(nullptr);
        std::lock_guard<std::mutex> guard(recentBossKillsLock);

        for (auto itr = recentBossKills.begin(); itr != recentBossKills.end();)
        {
            if (now - itr->second >= BossKillRepeatSeconds)
                itr = recentBossKills.erase(itr);
            else
                ++itr;
        }

        return recentBossKills.emplace(std::make_pair(instanceId, entry), now).second;
    }

    // A world boss (creature_template.Rank 3) anywhere, or the kill credit of a dungeon encounter on the encounter's own map:
    // a row of instance_encounters with creditType 0 whose creditEntry is the creature's entry.
    bool IsBoss(Creature const* creature)
    {
        if (creature->IsWorldBoss())
            return true;

        DungeonEncounterMapBounds bounds = sObjectMgr.GetDungeonEncounterBounds(creature->GetEntry());
        for (DungeonEncounterMap::const_iterator itr = bounds.first; itr != bounds.second; ++itr)
        {
            DungeonEncounter const& encounter = itr->second;
            if (encounter.creditType == ENCOUNTER_CREDIT_KILL_CREATURE && encounter.dbcEntry && encounter.dbcEntry->mapId == creature->GetMapId())
                return true;
        }

        return false;
    }

    void WriteNames(std::ostringstream& out, const std::vector<std::string>& names)
    {
        out << "[";
        for (size_t i = 0; i < names.size(); ++i)
        {
            if (i)
                out << ",";
            out << "\"" << PlayerbotLLMInterface::SanitizeForJson(names[i]) << "\"";
        }
        out << "]";
    }
}

bool PlayerbotEventSink::IsEnabled()
{
    return sPlayerbotAIConfig.eventWebhookEnabled;
}

void PlayerbotEventSink::Post(const std::string& jsonBody)
{
    if (!IsEnabled())
        return;

    if (postsInFlight.fetch_add(1) >= MaxPostsInFlight)
    {
        postsInFlight--;
        sLog.outError("PlayerbotEventSink: %d posts are still waiting on the webhook, dropping an event", MaxPostsInFlight);
        return;
    }

    // Copy the settings now, so a config reload cannot change them under the request.
    ParsedUrl url = sPlayerbotAIConfig.eventWebhookEndPointUrl;
    int timeOutSeconds = sPlayerbotAIConfig.eventWebhookTimeout;

    try
    {
        std::thread([url, jsonBody, timeOutSeconds]()
        {
            std::string response;
            std::vector<std::string> debugLines;
            if (!PlayerbotLLMInterface::HttpPost(url, jsonBody, timeOutSeconds, "", response, debugLines))
                sLog.outError("PlayerbotEventSink: could not post an event to %s:%d", url.hostname.c_str(), url.port);

            postsInFlight--;
        }).detach();
    }
    catch (const std::system_error& e)
    {
        postsInFlight--;
        sLog.outError("PlayerbotEventSink: could not start a thread for an event: %s", e.what());
    }
}

void PlayerbotEventSink::PostLevelUp(PlayerbotAI* ai, uint32 level)
{
    if (!IsEnabled() || !ai)
        return;

    Player* bot = ai->GetBot();
    Player* master = ai->GetMaster();

    if (!bot || !master || master == bot || !master->isRealPlayer() || sRandomPlayerbotMgr.IsRandomBot(bot))
        return;

    std::vector<std::string> witnesses;
    witnesses.push_back(master->GetName());

    if (Group* group = bot->GetGroup())
    {
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player* member = ref->getSource();
            if (!member || member == bot || member == master)
                continue;

            witnesses.push_back(member->GetName());
        }
    }

    Post(LevelUpJson(bot->GetName(), level, witnesses));
}

std::string PlayerbotEventSink::LevelUpJson(const std::string& who, uint32 level, const std::vector<std::string>& witnesses)
{
    std::ostringstream out;
    out << "{\"type\":\"level_up\",\"who\":\"" << PlayerbotLLMInterface::SanitizeForJson(who) << "\",\"level\":" << level << ",\"witnesses\":";
    WriteNames(out, witnesses);
    out << "}";
    return out.str();
}

void PlayerbotEventSink::PostBossKilled(Player* receiver, const WorldPacket& partyKillLog)
{
    if (!IsEnabled() || !receiver || !receiver->IsInWorld())
        return;

    // Unit::Kill writes the guid of the player with the killing blow, then the victim's, both as full 8-byte guids.
    if (partyKillLog.size() < 16)
        return;

    WorldPacket p(partyKillLog);
    p.rpos(0);
    ObjectGuid killerGuid, victimGuid;
    p >> killerGuid >> victimGuid;

    if (!victimGuid.IsCreature())
        return;

    // The packet only goes to players near the victim, so the victim is on the receiver's map,
    // and Unit::Kill sends it before the creature dies, so the creature is still there.
    Creature* boss = receiver->GetMap()->GetCreature(victimGuid);
    if (!boss || !IsBoss(boss))
        return;

    // The witnesses are the members the core would credit with the kill: Player::IsAtGroupRewardDistance,
    // which is anywhere in the same dungeon instance, or within group experience distance elsewhere. Real players first.
    std::vector<std::string> witnesses;
    std::vector<std::string> botWitnesses;
    auto addWitness = [&](Player* member)
    {
        if (!member || !member->IsAtGroupRewardDistance(boss))
            return;

        if (member->isRealPlayer())
            witnesses.push_back(member->GetName());
        else
            botWitnesses.push_back(member->GetName());
    };

    if (Group* group = receiver->GetGroup())
    {
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
            addWitness(ref->getSource());
    }
    else
        addWitness(receiver);

    // Bots killing a boss with no real player there is not news.
    if (witnesses.empty())
        return;

    witnesses.insert(witnesses.end(), botWitnesses.begin(), botWitnesses.end());

    if (!ClaimBossKill(boss->GetInstanceId(), boss->GetEntry()))
        return;

    Post(BossKilledJson(boss->GetName(), boss->GetEntry(), boss->GetMapId(), boss->GetInstanceId(), witnesses));
}

std::string PlayerbotEventSink::BossKilledJson(const std::string& creature, uint32 entry, uint32 map, uint32 instance, const std::vector<std::string>& witnesses)
{
    std::ostringstream out;
    out << "{\"type\":\"boss_killed\",\"creature\":\"" << PlayerbotLLMInterface::SanitizeForJson(creature) << "\",\"entry\":" << entry
        << ",\"map\":" << map << ",\"instance\":" << instance << ",\"witnesses\":";
    WriteNames(out, witnesses);
    out << "}";
    return out.str();
}
