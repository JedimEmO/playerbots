# Console commands

The `.rndbot` subcommands this fork adds,
for a sidecar such as innkeeper that drives the game over SOAP or remote access.
Each one answers with a single line,
which SOAP returns as the command's result.

They also work as `.bot <subcommand>` in game.
There the speaker commands use the spoofed or current character as the sender,
and `deliver`, `profession`, `teleport` and `autonomous` still need a game master account.

## say, yell and emote

```
.rndbot say <bot> <text>
.rndbot yell <bot> <text>
.rndbot emote <bot> <text>
```

The first word names the sender, an online character, usually a bot.
The rest of the line is the text, spaces included.
It is queued as a chat packet on the sender's session,
so it goes through normal chat handling and nearby bots hear it.

Say and yell use the sender's faction language;
the core switches to universal for game masters and two-side chat.
An emote shows as the sender's name followed by the text,
so `.rndbot emote Dunpriest raises a mug.` shows `Dunpriest raises a mug.`

| Reply | When |
|---|---|
| `Sent say message "<text>" as <bot>` | The say was queued. |
| `Sent yell message "<text>" as <bot>` | The yell was queued. |
| `Sent emote "<text>" as <bot>` | The emote was queued. |
| `No sender found` | No online character has that name. |
| `No message given` | A name and no text. |

## deliver

```
.rndbot deliver <player> <item>:<count>[,<item>:<count>...] <line>
```

Sends a courier with items to an online real player.
`<player>` is the character name; case does not matter.
The item list is `item_template` entries with counts, separated by commas, with no spaces.
A pair without `:<count>` counts one.
Everything after the item list is the courier's spoken line, spaces included;
it may be empty, and then the courier only bows.

```
.rndbot deliver Thrain 4536:5,159:5 Compliments of the house.
```

The courier is the creature `AiPlayerbot.CourierEntry` (default 60200, Dizzlebit),
summoned 6 to 8 yards in front of the player, facing them,
for `AiPlayerbot.CourierLingerSecs` seconds (default 30).
It runs up to about 2 yards from the player.
Once it is within 3 yards, or after 4 seconds, it says the line, bows,
and the items go into the player's bags.
What does not fit is mailed with the courier as the sender,
one stack per letter because Classic mail carries one item,
and the courier adds "Your bags are full; I left it with the postmaster."
The courier has faction 35 and is immune to players and creatures,
so nothing attacks it and it attacks nothing.

The command only checks and queues the delivery;
the player's next update on their map summons the courier.
So the reply comes before the courier arrives.
`Bags full, mailed <n>` is worked out from the bags when the command runs,
and each item is checked on its own,
so several items that each fit but not together are reported as delivered.
At the hand-over the bags are checked again, and whatever does not fit then is mailed.

A player who changes maps before the hand-over gets the items without the courier,
and a system message if anything was mailed.
If the courier cannot be summoned, for instance because the entry has no `creature_template` row,
the items are handed over at once and the server logs an error.
A player who logs out in the seconds before the hand-over does not get the items.

| Reply | When |
|---|---|
| `Delivery to <player>: <n> items` | The delivery is queued and everything fits in the bags. `<n>` is the sum of the counts. |
| `Bags full, mailed <n>` | The delivery is queued and `<n>` items do not fit in the bags now. |
| `No player found` | The player is not online, is a bot, or the name is invalid. |
| `Unknown item <id>` | An entry is not a number or not in `item_template`. `<id>` is the text as given. |
| `Bad count for item <id>` | A count is zero or not a number. |
| `Too many items: at most 16 stacks` | The items fill more than 16 stacks, counted by each item's stack size. |
| `Usage: .rndbot deliver <player> <item>:<count>[,<item>:<count>...] <line>` | The player name or the item list is missing. |
| `You may not send deliveries` | The account is below game master. SOAP runs as the console, which passes. |

The player is checked before the items, and the first bad item stops the check.

## profession

```
.rndbot profession <character> list
.rndbot profession <character> set <skill> <value>
.rndbot profession <character> unlearn <skill>
```

Sets up, removes or lists the professions of an online character, bot or real player.
`<character>` is the character name; case does not matter.
`<skill>` is one of the names below, in any case, or the skill line id.
The replies always use the lower-case name, so a line from `list` reads back as a `set`.

| Name | Skill line | Kind |
|---|---|---|
| `herbalism` | 182 | primary |
| `mining` | 186 | primary |
| `skinning` | 393 | primary |
| `alchemy` | 171 | primary |
| `blacksmithing` | 164 | primary |
| `leatherworking` | 165 | primary |
| `tailoring` | 197 | primary |
| `engineering` | 202 | primary |
| `enchanting` | 333 | primary |
| `cooking` | 185 | secondary |
| `firstaid` | 129 | secondary |
| `fishing` | 356 | secondary |

```
.rndbot profession Dunpriest set herbalism 150
```

`set` teaches the rank spells the value needs, as the trainers would,
and sets the skill to the value with that rank's cap.
Up to 75 is Apprentice, 150 Journeyman, 225 Expert and 300 Artisan,
so the example gives Apprentice and Journeyman Herb Gathering and herbalism 150/150.
Ranks above the one the value needs are unlearned, so `set` also lowers a profession.
Getting the skill teaches what the game gives with it:
Find Herbs for herbalism, Find Minerals, Smelting and Smelt Copper for mining,
Disenchant for enchanting, and each crafting profession's starting recipes.
Other recipes stay with the trainers.

A character has at most two primary professions, as in the game;
cooking, first aid and fishing do not count.
Setting a primary profession the character already has is always allowed.

`unlearn` does what the unlearn button in the skill window does.
The skill goes, and with it the rank spells, every recipe of the profession and the gathering spells.
Unlearning a profession the character does not have still answers `Unlearned`.

`list` gives one line per profession the character has, in the order of the table:

```
herbalism 150/150
cooking 1/75
```

The value is the trained skill, without racial bonuses such as the tauren's herbalism.

`set` and `unlearn` only check and queue the change;
the character's next update on their map makes it, because spells and skills belong to the map's thread.
So the reply is the planned result and comes before the change.
Bots update on every map tick, and real players within a fraction of a second.
A character who is between maps gets the change once they arrive,
and one who logs out before the next update does not get it.
Two `set` commands for new primary professions in the same moment can both answer `Set`
when the character has one primary profession;
the second is then dropped at the update, and the server logs an error.
Random bots may change their professions again when the bot manager randomizes or re-initializes them.

| Reply | When |
|---|---|
| `<name> <value>/<max>` | One line per profession, for `list`. |
| `No professions` | `list` on a character without any of the twelve professions. |
| `Set <skill> to <value> for <character>` | The change is queued. |
| `Unlearned <skill> for <character>` | The unlearning is queued. |
| `No player found` | No online character has that name, or the name is invalid. |
| `Unknown skill <text>` | The skill is not one of the twelve names or ids. `<text>` is as given. |
| `Bad value <text>` | The value is not a number from 1 to 300. |
| `Already has two primary professions` | `set` of a primary profession the character lacks while they have two. |
| `Usage: .rndbot profession <character> list\|set <skill> <value>\|unlearn <skill>` | Missing or extra words, or an unknown action. |
| `You may not change professions` | The account is below game master. SOAP runs as the console, which passes. |

The character is checked first, then the skill, then the value.

## teleport

```
.rndbot teleport <bot> <player>
```

Teleports an online bot next to an online real player, on the same map or another one.
Both names are character names; case does not matter.

```
.rndbot teleport Dunpriest Thrain
```

The bot lands two yards to the player's right, facing them.
A bot on a flight path leaves it, and its AI is reset,
so it does not walk straight back to what it was doing.
A dead bot is teleported as it is; its corpse stays where it died.

The teleport is made when the command runs, on the world thread while the maps wait,
as the core's own `.namego` and `.goname` do.
So `Teleported` means the core accepted it.
On another map the bot arrives once the map change completes, a moment later.

The core decides the instance from the bot's own bind or its group's,
and makes a new one when there is neither.
So a player in a dungeon or raid only gets the bot
when it is in their group and that bind is the player's instance;
the usual case is a group that entered together.
Otherwise the bot would arrive alone in another copy of the dungeon, and the command refuses.
It refuses a player in a battleground,
a player on a ship or zeppelin, because the spot beside them is in the water,
and either character in the middle of a teleport.

With exactly one name, `.rndbot teleport <name>` keeps its old meaning:
random bots whose names start with `<name>` go to a grinding spot for their level.
Two or more names always come to this command, so more than two answer the usage
and never reach that prefix match.

| Reply | When |
|---|---|
| `Teleported <bot> to <player>` | The core accepted the teleport. |
| `No bot found` | No online bot has that name; a real player's character is not a bot. |
| `No player found` | No online real player has that name. |
| `Could not teleport <bot> to <player>` | One of the refusals above, or the core refused the teleport. |
| `Usage: .rndbot teleport <bot> <player>` | Not exactly two names. |
| `You may not teleport bots` | The account is below game master. SOAP runs as the console, which passes. |

The bot is checked before the player.

## autonomous

```
.rndbot autonomous
```

Lists the autonomous bots, the characters on the `AiPlayerbot.AutonomousAccounts` accounts,
one line each, `<name> online` or `<name> offline`, sorted by name.
`No autonomous bots` means the setting is empty or its accounts have no characters.
[Autonomous bots](autonomous-bots.md) describes them and every reply.
