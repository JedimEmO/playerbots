#pragma once

#include "Common.h"

#include <string>
#include <vector>

class PlayerbotAI;

// Posts game events as JSON to AiPlayerbot.EventWebhookUrl, for a sidecar such as innkeeper.
// Off while the URL is empty. Every post runs on its own detached thread, so no caller waits on the network.
// docs/innkeeper-events.md describes the bodies.
class PlayerbotEventSink
{
public:
    static bool IsEnabled();

    // Sends jsonBody to the webhook in a POST. Returns at once.
    static void Post(const std::string& jsonBody);

    // Posts a level_up event if the bot is an alt bot: not a random bot, with a real player as master.
    static void PostLevelUp(PlayerbotAI* ai, uint32 level);

    static std::string LevelUpJson(const std::string& who, uint32 level, const std::vector<std::string>& witnesses);
};
