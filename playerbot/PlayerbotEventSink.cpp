#include "playerbot/playerbot.h"
#include "PlayerbotEventSink.h"
#include "PlayerbotLLMInterface.h"
#include "playerbot/PlayerbotAIConfig.h"
#include "Groups/Group.h"
#include "Log/Log.h"

#include <atomic>
#include <sstream>
#include <system_error>
#include <thread>

namespace
{
    // Posts that may be waiting on the network at the same time. A webhook that stops answering
    // holds at most this many threads; events beyond it are dropped until some finish.
    constexpr int MaxPostsInFlight = 16;
    std::atomic<int> postsInFlight{0};
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
    out << "{\"type\":\"level_up\",\"who\":\"" << PlayerbotLLMInterface::SanitizeForJson(who) << "\",\"level\":" << level << ",\"witnesses\":[";

    for (size_t i = 0; i < witnesses.size(); ++i)
    {
        if (i)
            out << ",";
        out << "\"" << PlayerbotLLMInterface::SanitizeForJson(witnesses[i]) << "\"";
    }

    out << "]}";
    return out.str();
}
