#pragma once

#include "Common.h"

#include <string>
#include <vector>

class Player;
class PlayerbotAI;
class WorldPacket;

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

    // Posts a boss_killed event if partyKillLog, an SMSG_PARTYKILLLOG the receiver was sent, names a boss as the victim
    // and a real player is among the witnesses. A kill of the same entry in the same instance posts once per 60 seconds.
    static void PostBossKilled(Player* receiver, const WorldPacket& partyKillLog);

    static std::string BossKilledJson(const std::string& creature, uint32 entry, uint32 map, uint32 instance, const std::vector<std::string>& witnesses);
};
