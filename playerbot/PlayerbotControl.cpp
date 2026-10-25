#include "playerbot/PlayerbotControl.h"

#include "playerbot/playerbot.h"
#include "playerbot/PlayerbotAIConfig.h"
#include "playerbot/PlayerbotLLMInterface.h"
#include "playerbot/ChatHelper.h"
#include "playerbot/WorldPosition.h"
#include "Chat/Chat.h"
#include "Entities/Player.h"
#include "Globals/ObjectMgr.h"

#include <atomic>
#include <mutex>
#include <sstream>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace
{
    // Requests waiting on the sidecar. A party fill can take a minute, so a few may overlap.
    std::atomic<int> requestsInFlight{0};
    const int MaxRequestsInFlight = 8;

    std::mutex answersMutex;
    std::vector<std::pair<ObjectGuid, std::vector<std::string>>> answers;

    void QueueAnswer(ObjectGuid player, std::vector<std::string> lines)
    {
        std::lock_guard<std::mutex> guard(answersMutex);
        answers.emplace_back(player, std::move(lines));
    }

    void WriteCharacter(std::ostringstream& out, Player* who)
    {
        ai::WorldPosition pos(who);
        out << "{\"name\":\"" << PlayerbotLLMInterface::SanitizeForJson(who->GetName()) << "\""
            << ",\"race\":\"" << PlayerbotLLMInterface::SanitizeForJson(ai::ChatHelper::formatRace(who->getRace())) << "\""
            << ",\"class\":\"" << PlayerbotLLMInterface::SanitizeForJson(ai::ChatHelper::formatClass(who->getClass())) << "\""
            << ",\"gender\":\"" << (who->getGender() == GENDER_MALE ? "male" : "female") << "\""
            << ",\"level\":\"" << who->GetLevel() << "\""
            << ",\"zone\":\"" << PlayerbotLLMInterface::SanitizeForJson(pos.getAreaName()) << "\""
            << ",\"subzone\":\"" << PlayerbotLLMInterface::SanitizeForJson(pos.getAreaOverride()) << "\"}";
    }

    // The lines of an HTTP response's body: everything after the first blank line, split at line ends, empty lines
    // dropped.
    std::vector<std::string> BodyLines(const std::string& response)
    {
        std::vector<std::string> lines;
        size_t start = response.find("\r\n\r\n");
        if (start == std::string::npos)
            return lines;

        std::istringstream body(response.substr(start + 4));
        std::string line;
        while (std::getline(body, line))
        {
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            if (!line.empty())
                lines.push_back(line);
        }
        return lines;
    }
}

bool PlayerbotControl::IsEnabled()
{
    return sPlayerbotAIConfig.innkeeperControlEnabled;
}

std::string PlayerbotControl::RequestJson(Player* player, Player* target, const std::string& text)
{
    std::ostringstream out;
    out << "{\"player\":";
    WriteCharacter(out, player);
    out << ",\"account\":" << player->GetSession()->GetAccountId();
    if (target)
    {
        out << ",\"target\":";
        WriteCharacter(out, target);
    }
    out << ",\"text\":\"" << PlayerbotLLMInterface::SanitizeForJson(text) << "\"}";
    return out.str();
}

void PlayerbotControl::Ask(Player* player, Player* target, const std::string& text)
{
    if (!player)
        return;

    if (!IsEnabled())
    {
        Tell(player, "ik: error innkeeper is not set up");
        return;
    }

    if (requestsInFlight.fetch_add(1) >= MaxRequestsInFlight)
    {
        requestsInFlight--;
        Tell(player, "ik: error innkeeper is busy, try again");
        return;
    }

    // Built here, on the thread that owns the characters; the request thread only sees strings.
    std::string body = RequestJson(player, target, text);
    ObjectGuid asker = player->GetObjectGuid();
    ParsedUrl url = sPlayerbotAIConfig.innkeeperControlEndPointUrl;
    int timeOutSeconds = sPlayerbotAIConfig.innkeeperControlTimeout;

    try
    {
        std::thread([url, body, asker, timeOutSeconds]()
        {
            std::string response;
            std::vector<std::string> debugLines;
            std::vector<std::string> lines;
            if (PlayerbotLLMInterface::HttpPost(url, body, timeOutSeconds, "", response, debugLines))
                lines = BodyLines(response);

            if (lines.empty())
            {
                sLog.outError("PlayerbotControl: no answer from %s:%d", url.hostname.c_str(), url.port);
                lines.push_back("ik: error innkeeper did not answer");
            }

            QueueAnswer(asker, std::move(lines));
            requestsInFlight--;
        }).detach();
    }
    catch (const std::system_error& e)
    {
        requestsInFlight--;
        sLog.outError("PlayerbotControl: could not start a thread for a request: %s", e.what());
        Tell(player, "ik: error innkeeper is busy, try again");
    }
}

void PlayerbotControl::Deliver()
{
    std::vector<std::pair<ObjectGuid, std::vector<std::string>>> ready;
    {
        std::lock_guard<std::mutex> guard(answersMutex);
        if (answers.empty())
            return;
        ready.swap(answers);
    }

    for (auto& [guid, lines] : ready)
    {
        Player* player = sObjectMgr.GetPlayer(guid);
        if (!player || !player->IsInWorld() || !player->GetSession())
            continue;

        for (const std::string& line : lines)
            Tell(player, line);
    }
}

void PlayerbotControl::Tell(Player* player, const std::string& line)
{
    if (!player || !player->GetSession())
        return;

    std::string text = "IK\t" + line;
    WorldPacket data;
    ChatHandler::BuildChatPacket(data, CHAT_MSG_WHISPER, text.c_str(), LANG_ADDON, CHAT_TAG_NONE,
        player->GetObjectGuid(), player->GetName());
    player->GetSession()->SendPacket(data);
}
