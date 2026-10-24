# Autonomous bots

Characters that the server plays on its own, like random bots,
but that keep the character they have, like a player's alts.
This page is for whoever runs the server or writes a sidecar that talks to these characters;
it describes the setting, what an autonomous bot does, and every place it differs from a random bot.

## The setting

```
AiPlayerbot.AutonomousAccounts = GUILD
```

The value is a comma-separated list of account names; case does not matter.
Empty, the default, turns autonomous bots off.
Every character on a listed account is an autonomous bot,
including characters created while the server runs.

An account whose name starts with `AiPlayerbot.RandomBotAccountPrefix`,
or that is in the random bot account list, is ignored with an error in the log,
because the random bot manager owns its characters and randomizes them.
An account that does not exist yet is looked up again every minute.

## Logging in and staying online

The random bot manager logs every autonomous character in at startup,
whether or not `AiPlayerbot.RandomBotAutologin` is on and whether or not a real player is online.
It checks every update.
A character that is not in the world is sent to log in again,
at most five per update and at most once every 30 seconds per character.
So a bot that logged out, was told to log out, or crashed out comes back within half a minute.
The list of characters is re-read from the database every minute.

While someone plays on an autonomous account from a client,
the manager logs in none of its characters,
and the character that person plays is theirs, not an autonomous bot.

A real player logging out changes nothing for an autonomous bot,
except that a bot whose master was that player loses its master and resets its strategies,
as a random bot does.

Autonomous bots are held by the random bot manager,
so they count towards the online bot total the manager compares with `AiPlayerbot.MaxRandomBots`.

## How they play

An autonomous bot is a free bot, the same group as random bots and free alt bots.
So it gets the random bot strategy set:
grind, travel, rpg, quests, loot, gathering,
and `AiPlayerbot.RandomBotCombatStrategies`, `AiPlayerbot.RandomBotNonCombatStrategies`,
`AiPlayerbot.RandomBotDeadStrategies` and `AiPlayerbot.RandomBotReactStrategies` on top.
From level 10 it joins the World channel unless it plays solo,
and it earns experience at `AiPlayerbot.XPRate`, as other free bots do.

It is always active, as a free alt bot is,
so the activity scaling that slows random bots far from players never pauses it.

It keeps `ai chat` when `AiPlayerbot.NonCombatStrategies`, the list for bots that belong to players, gives it,
even when the random bot list removes it.
That keeps it eligible for the LLM chat hook,
which answers bots with the `ai chat` strategy, or every bot when `AiPlayerbot.LLMEnabled` is 3.
`AiPlayerbot.LLMEnabled = 2` gives `ai chat` to every bot as before.

Its strategies always come from that set.
A strategy set saved for it, for instance while it was grouped with a player, is not loaded back.
A strategy changed by chat, such as `nc +ai chat`, lasts until the next reset;
for a permanent change, edit the config lists.

## Grouping with a player

An autonomous bot accepts a group invitation from a real player on the same terms as a random bot:
the player must be of its faction, or two-side groups must be allowed,
and not too far below its level or gear score.
The player who invites it becomes its master,
and while grouped it takes the normal chat commands from group members.
Leaving the group, it plays on its own again.
[Following a real player](#following-a-real-player) describes the strategies it plays meanwhile.

Its level never follows a master.
`AiPlayerbot.SyncAltLevelToMaster` does not apply to it,
and neither does `AiPlayerbot.SyncLevelWithPlayers`, which only works through randomizing.
A player who groups with a level 12 autonomous bot leaves it at level 12.

`.bot add` on a character from the player's own account works as before.
`.bot` commands that name an autonomous bot answer `Not allowed on autonomous bots`
unless they come from a game master or the console.

## Following a real player

```
AiPlayerbot.FreeBotsFollowRealMaster = 1
```

A free bot whose master is a real player in its group plays as that player's companion.
This covers random bots and free alt bots as well as autonomous bots,
and it is on by default.
The problem it solves is a random bot that a player invites
and that keeps roaming: it walks off to a trainer or a quest hub,
or stands still with `passive` and `stay` while its party fights.

While it follows a real master, the bot gets the strategies an alt bot on the player's own account gets,
with one difference in movement.
In the non-combat engine that is the role and spec strategies for its class,
`follow`, `loot`, `quest`, `gather`, `food`, `buff`, `mount` and the rest of the base list,
with `AiPlayerbot.NonCombatStrategies` on top.
The combat, dead and reaction engines take `AiPlayerbot.CombatStrategies`, `AiPlayerbot.DeadStrategies`
and `AiPlayerbot.ReactStrategies` instead of the random bot lists.
There is no `grind`, `travel`, `rpg`, `lfg`, `maintenance` or `wander`.
`quest` stays because it only reacts to the player:
it accepts shared quests and talks to the quest giver the player talks to.

The difference is that it always gets `follow`, even with `AiPlayerbot.UseWanderAsDefaultFollowStrategy` on,
which is the default.
Alt bots still get `wander` under that setting;
set it to `0` for them to follow closely as well.

Two other random bot rules stand aside while it follows a real master.
A saved strategy set is not loaded back on reset,
since any `nc` or `co` command saves all four engines
and the set may come from a time the bot roamed.
The loot rule that adds `passive` and `stay` to both engines,
under free for all, master loot or a threshold above uncommon
when the bot is not in the player's guild, does not run;
alt bots never had it.

The strategies are rebuilt whenever the master changes:
when the bot accepts the invitation,
when it picks a real player in its group as master,
when that player logs in while grouped with it, and when the master goes away.
The master goes away when the bot leaves the group, when the group disbands or the player kicks it,
when the player leaves a group that still has other members, and when the player logs out.
The bot then plays the free bot strategies again;
when another bot leads what is left of the group, that bot becomes its master.

With the setting at `0` a free bot keeps the random bot strategies while grouped with a real player.
In a battleground the battleground strategies apply either way.

## What never happens to them

`IsRandomBot` is false for an autonomous bot,
so every random bot rule passes it by.
In particular, nothing randomizes it:
no `Randomize`, `RandomizeFirst` or `InstaRandomize`,
no level re-roll, no gear progression re-roll while grouped with a player,
no random teleport or strategy change on the random bot timers, and no timed logout.
The manager's random bot update loop only walks random bots.

`AiPlayerbot.DeleteRandomBotGuilds` disbands guilds whose leader is a random bot,
so a guild led by an autonomous bot or a real player is safe.
A guild with a random bot leader is not, whoever else is in it.

Console commands follow the same line.
`.rndbot init`, `upgrade`, `refresh` and `remove` skip autonomous bots,
even when their names match the prefix given.
Through `.bot` or `.rndbot` with a character name,
game masters and the console may still use the commands that help a bot along,
such as `train`, `food` or `do`,
but `add`, `login`, `remove`, `logout`, `rm`, `always`, `init`, `gear`, `equip`, `levelup`, `level` and `random`
answer `Not allowed on autonomous bots`.

## Where they differ from random bots

| Rule | Random bot | Autonomous bot |
|---|---|---|
| Logged in and kept online | Random bot manager, by count and timers | Random bot manager, always |
| Strategy set | Random bot set; alt bot set while it follows a real master | Random bot set, plus `ai chat` from `NonCombatStrategies`; alt bot set while it follows a real master |
| Activity | Scaled by priority | Always active |
| Randomized, levels and gear re-rolled | Yes | Never |
| Level synced to master or players | Players, when randomized | Never |
| Saved strategies loaded on reset | When it has a player relation, unless it follows a real master | Never |
| Cheats | `AiPlayerbot.RndBotCheats` | `AiPlayerbot.BotCheats` |
| Instance reset limit on entering a dungeon | Checked | Not checked |
| Trades | Sells, with discounts | Trades as a player's bot does |
| Hired by a player | Possible | Never |
| WTS adverts and activity suggestions in chat | Yes | No |
| `level_up` event to the webhook | Never | When its master is a real player |

Equipping looted upgrades and picking talents follow `AiPlayerbot.AutoEquipUpgradeLoot` and `AiPlayerbot.AutoPickTalents`,
as for a player's bots.
With the defaults, `0` and `no`, an autonomous bot equips nothing it loots and spends no talent points,
so set them to `1` and `full`.

## Guild ranks

The `guild` strategy, part of the random bot set, lets a bot manage its guild:
invite nearby players and bots, promote and demote members by chance, and work guild craft orders.
It only does what its guild rank allows.
Give autonomous bots a rank without the invite, promote and demote rights
unless they should recruit and manage members the way random bot officers do.

## Listing them

```
.rndbot autonomous
```

Answers one line per autonomous character, sorted by name:

```
Ana online
Bob offline
```

| Reply | When |
|---|---|
| `<name> online` | The character is in the world. |
| `<name> offline` | The character is not in the world; the manager will log it in. |
| `No autonomous bots` | The setting is empty, or its accounts have no characters. |
| `You may not list autonomous bots` | The account is below game master. SOAP runs as the console, which passes. |

The command re-reads the characters first, so one created a moment ago is listed.
It also works as `.bot autonomous` in game.
