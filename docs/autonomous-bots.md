# Autonomous bots

Characters that the server plays on its own, like random bots,
but that keep the character they have, like a player's alts.
This page is for whoever runs the server or writes a sidecar that talks to these characters;
it describes the setting, what an autonomous bot does, and every place it differs from a random bot.

## The settings

```
AiPlayerbot.AutonomousAccounts = ALICE,BOB
AiPlayerbot.SharedAutonomousAccounts = GUILD
```

Each value is a comma-separated list of account names; case does not matter.
Both empty, the default, turns autonomous bots off.
Every character on a listed account is an autonomous bot,
including characters created while the server runs.

The two lists say who the characters belong to.
`AutonomousAccounts` names players' accounts:
a character there belongs to whoever plays that account,
plays only while its switch is on, and takes commands only from its own account.
`SharedAutonomousAccounts` names accounts the server owns:
a character there plays unless switched off, and any real player may command it.
An account on both lists is shared.

An account whose name starts with `AiPlayerbot.RandomBotAccountPrefix`,
or that is in the random bot account list, is ignored with an error in the log,
because the random bot manager owns its characters and randomizes them.
An account that does not exist yet is looked up again every minute.

## The switch

Each autonomous character has a switch, on or off, set with `.rndbot autonomous <name> on|off`
and stored in `ai_playerbot_random_bots` as the event `autonomy`, so it survives a restart.
`.rndbot autonomous <name> default` removes it,
and the character follows its account again: off on a player's account, on on a shared one.

A character switched off and not in a group is logged out within an update.
One in a group stays until it leaves, since it plays for its group meanwhile.
A character a client plays, or one a player logged in with `.bot add`, is not the manager's to log out.

## Logging in and staying online

The random bot manager logs every autonomous character that is switched on in at startup,
whether or not `AiPlayerbot.RandomBotAutologin` is on and whether or not a real player is online.
It checks every update.
A character that is switched on and not in the world is sent to log in again,
at most five per update and at most once every 30 seconds per character.
So a bot that logged out, was told to log out, or crashed out comes back within half a minute.
The list of characters, with each switch and cap, is re-read from the database every minute.

A client on an autonomous account changes this for that account only while it has no character in the world:
at the character screen, or loading a character, the manager logs in none of the account's characters,
because the one the client is about to play could be among them.
Once the client is in the world, the account's other characters log in as usual,
and the one it plays is the client's.

## Taking a character over from its bot

A client may log in to a character its bot is playing.
The core's `HandlePlayerLoginOpcode` hands the client's session the character where it stands, with no reload,
and calls `RandomPlayerbotMgr::MovePlayerBot`.
For an autonomous character the manager then stops the bot's AI, saves the character and lets it go,
so the client plays it from that moment.
It does not log the character out.

When the client logs out and leaves, the character is out of the world,
and the manager logs it in again as a bot if its switch is on.

A player who wants the AI to keep playing their character while they watch can say so with `.bot self login`,
which the takeover then leaves alone.
That is upstream's self-bot, with its warnings.

## The level cap

```
.rndbot autonomous <name> cap 30
```

An autonomous character with a cap gains no experience at that level while a bot plays it,
grouped with a player or on its own;
experience that would carry it past the cap is dropped.
`.rndbot autonomous <name> cap off` removes it.
The cap is stored as the event `xpcap` and survives a restart.

A client playing the character has no cap.
Commands that set a level, such as `.character level`, ignore it.

The cap needs a change in the core's `Player::GiveXP`, which asks `RandomPlayerbotMgr::GetXpCap`;
in our server image it is the patch `autonomous-level-cap.patch`.
Without it the cap is stored and listed, and has no effect.

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

An autonomous bot on a shared account accepts a group invitation from any real player
while `AiPlayerbot.RealPlayersCommandFreeBots` is on, the default;
the core still keeps the factions apart unless two-side groups are allowed.
With the setting off it accepts on the same terms as a random bot:
the player must be of its faction, or two-side groups must be allowed,
and not too far below its level or gear score.
One on a player's account accepts invitations only from its own account,
and refuses everyone else with `I have a master already`, whatever the setting.
The player who invites it becomes its master,
and while grouped it takes the normal chat commands from group members it would take commands from.
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

A bot that queued for a dungeon on its own leaves the LFG queue
when a real player becomes its master,
whatever the settings.
It would otherwise stay queued, since the bot's own rule for leaving the queue
waits while its master is a real player.

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

## Commanding a free bot

```
AiPlayerbot.RealPlayersCommandFreeBots = 1
```

A real player who is a free bot's master, or in its group, may give it every command.
An autonomous bot on a shared account goes further and takes every command from any real player, grouped or not,
because it plays for the server's players.
A whisper to it is then a command or chat for the LLM hook,
never `Invite me to your group first`.
Random bots and free alt bots still need the player in their group.

An autonomous bot on a player's account takes every command from its own account and none from anyone else,
grouped or not and whatever this setting says.
Chat for the LLM hook is not a command, so anyone can still talk to it.

The security checks that would otherwise answer with a refusal or silence are skipped:
the level check, the gear score check,
and the checks for a bot in a battleground queue or the LFG queue.
The setting also turns the loot rule above off for every free bot,
grouped with a real player or not.
It is on by default; `0` restores the upstream security levels.

Group members already passed these checks for chat commands,
but not for guild invitations and petitions, which ignore the group;
with the setting on, a real player in the bot's group passes those too.

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
| Logged in and kept online | Random bot manager, by count and timers | Random bot manager, while switched on |
| Experience | Free | Stops at its cap, when it has one |
| Strategy set | Random bot set; alt bot set while it follows a real master | Random bot set, plus `ai chat` from `NonCombatStrategies`; alt bot set while it follows a real master |
| Activity | Scaled by priority | Always active |
| Randomized, levels and gear re-rolled | Yes | Never |
| Level synced to master or players | Players, when randomized | Never |
| Saved strategies loaded on reset | When it has a player relation, unless it follows a real master | Never |
| Cheats | `AiPlayerbot.RndBotCheats` | `AiPlayerbot.BotCheats` |
| Instance reset limit on entering a dungeon | Checked | Not checked |
| Trades | Sells, with discounts | Trades as a player's bot does |
| Hired by a player | Possible | Never |
| Full commands from a real player | Master or group member | Shared account: any real player, grouped or not. Player's account: its own account only |
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

## Listing and switching them

```
.rndbot autonomous
.rndbot autonomous <name>
.rndbot autonomous <name> on|off|default
.rndbot autonomous <name> cap <level>|off
```

Without a name it answers one line per autonomous character, sorted by name;
with one, that character's line, after any change:

```
Ana online on
Bob offline off
Cid online on cap 30
```

Each line is the name, whether the character is in the world, its switch, and its cap when it has one.
An `offline on` character is about to be logged in; an `online off` one, out of a group, about to be logged out.

| Reply | When |
|---|---|
| `<name> online\|offline on\|off`, then ` cap <level>` | As above. |
| `No autonomous bots` | The settings are empty, or their accounts have no characters. |
| `No autonomous character named <name>` | No character of that name is on an autonomous account. |
| `Cap must be a level from 1 to <max>, or off` | The cap is not a level the server allows. |
| `Usage: .rndbot autonomous [<name> [on\|off\|default\|cap <level>\|cap off]]` | Anything else. |
| `You may not list autonomous bots` | The account is below game master. SOAP runs as the console, which passes. |

The command re-reads the characters first, so one created a moment ago is found.
It also works as `.bot autonomous` in game.
