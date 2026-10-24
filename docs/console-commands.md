# Console commands

The `.rndbot` subcommands this fork adds,
for a sidecar such as innkeeper that drives the game over SOAP or remote access.
Each one answers with a single line,
which SOAP returns as the command's result.

All four also work as `.bot <subcommand>` in game.
There the speaker commands use the spoofed or current character as the sender,
and `deliver` still needs a game master account.

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
