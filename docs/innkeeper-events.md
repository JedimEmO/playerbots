# Innkeeper events

The game events this fork posts to an HTTP webhook,
for a sidecar such as innkeeper that wants to hear the game without polling the console.
`PlayerbotEventSink` sends them; this page describes the settings, the request and every event body.

## Settings

Both keys live in `aiplayerbot.conf`.

| Key | Meaning | Default |
|---|---|---|
| `AiPlayerbot.EventWebhookUrl` | Where events go, as `http://host:port/path` or `https://...`. Empty turns events off. | empty |
| `AiPlayerbot.EventWebhookTimeout` | Seconds to wait for the response before closing the connection. | `2` |

A URL the server cannot parse logs `Unable to parse EventWebhookUrl url` at startup
and leaves events off.

## The request

Each event is one `POST` to the URL, on its own detached thread,
so the world thread never waits on the webhook.
The client is the same raw socket client the LLM chat uses.
It sends HTTP/1.1 with `Content-Type: application/json`, `Content-Length` and `Connection: close`,
and no `Authorization` header.

The body is one JSON object on one line.
Every string goes through `PlayerbotLLMInterface::SanitizeForJson`,
which escapes it and converts text that is not valid UTF-8 from Windows-1251.

The server ignores the response, its status included.
It reads until the webhook closes the connection or the timeout runs out,
so answer at once with a fixed-length body and close the connection.

## Delivery

Delivery is best effort.
A failed request is logged as `PlayerbotEventSink: could not post an event` and never retried.
At most 16 requests wait on the webhook at the same time;
an event beyond that is dropped and logged.

The timeout covers the response only.
Connecting uses the operating system's own timeout,
so a webhook host that drops packets holds a thread for as long as the system keeps trying.
A refused connection fails at once.

## Events

Every body has a `type` field that names the event.

### `level_up`

An alt bot gained a level.

```json
{"type":"level_up","who":"Ana","level":13,"witnesses":["Mathias","Bob"]}
```

| Field | Type | Meaning |
|---|---|---|
| `type` | string | `level_up` |
| `who` | string | The bot's name. |
| `level` | number | The level the bot reached. |
| `witnesses` | array of strings | The bot's master first, then every other online member of the bot's group. |

An alt bot here is a bot that is not a random bot and whose master is a real player,
so a bot whose master is offline or another bot sends nothing.
The witnesses are the people the bot belongs to, not the people standing near it;
the master is listed even when not grouped with the bot, and group members in another zone are listed too.

The event fires when the server sends the bot `SMSG_LEVELUP_INFO`.
Experience sends it once per level, so a bot that gains two levels at once posts two events.
`.character level` sends it once with the final level.

### `boss_killed`

A boss died with a real player there.

```json
{"type":"boss_killed","creature":"Edwin VanCleef","entry":639,"map":36,"instance":7,"witnesses":["Mathias","Ana","Bob"]}
```

| Field | Type | Meaning |
|---|---|---|
| `type` | string | `boss_killed` |
| `creature` | string | The boss's name. |
| `entry` | number | The boss's `creature_template` entry. |
| `map` | number | The map it died on. |
| `instance` | number | That map's instance id, which tells two runs of the same dungeon apart. `0` outside instances. |
| `witnesses` | array of strings | The real players present, then the bots present. |

A boss is a creature with `Rank` 3 in `creature_template`, a world boss, wherever it dies,
or the kill credit of a dungeon encounter on that encounter's own map:
a row of `instance_encounters` with `creditType` 0 whose `creditEntry` is the creature's entry.
Rank alone does not tell bosses from trash in vanilla dungeons;
Edwin VanCleef is rank 2 and most dungeon bosses are rank 1, like the elites around them.

The witnesses are the members of the group the core would credit with the kill,
by `Player::IsAtGroupRewardDistance`:
anyone in the same instance inside a dungeon or raid, dead or alive,
and anyone within group experience distance elsewhere.
A member who released their spirit and ran back outside is not listed.
Without a group, the only witness is the player or bot that was sent the kill.
A kill with no real player among the witnesses sends nothing.

The event fires when a real player or a bot is sent `SMSG_PARTYKILLLOG` and the victim is a boss.
The core sends that packet when a player, or a player's pet or totem, lands the killing blow on a creature
that player or their group tapped.
It goes to the killer and to the members of the killer's raid subgroup within group reward distance,
so the master's own kills count without bots,
and a raid kill the master did not get the packet for is still seen through a bot.
A boss killed by something that is not a player, such as an escort or a scripted instant kill, sends nothing.

Everyone near the kill is sent the packet, so the fork posts each kill once.
A kill is posted unless a kill with the same instance id and entry was posted less than 60 seconds earlier;
the repeats inside that window are dropped, and they do not extend it.
Outside instances the instance id is `0`, so two kills of the same world boss within 60 seconds post once.
