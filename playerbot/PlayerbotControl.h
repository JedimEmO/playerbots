#pragma once

#include "Common.h"

#include <string>

class Player;

// Carries a player's innkeeper request, `.bot ik [@<name>] <request>`, to AiPlayerbot.InnkeeperControlUrl, and the
// sidecar's answer back to that player as addon messages with the prefix IK, which no chat frame shows. No bot is in
// between, so a player can ask with nobody else online. Off while the URL is empty.
//
// Each request is posted on its own detached thread, so no caller waits on the network. The answer waits in a queue
// that Deliver empties on the world thread. docs/innkeeper-control.md describes the request and the answer.
class PlayerbotControl
{
public:
    static bool IsEnabled();

    // Posts `text` for `player`, about `target` when an @<name> named one. Returns at once.
    static void Ask(Player* player, Player* target, const std::string& text);

    // Sends every queued answer to its player, if still in the world. World thread only.
    static void Deliver();

    // Sends `line` to `player` now, as the addon message `IK<TAB>line` on the whisper lane, from the player.
    static void Tell(Player* player, const std::string& line);

    // The request body: the player and the target as the LLM template describes characters, and the text.
    static std::string RequestJson(Player* player, Player* target, const std::string& text);
};
