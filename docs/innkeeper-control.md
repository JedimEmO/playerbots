# Innkeeper requests from players

How a player asks a sidecar such as innkeeper for something, with no bot in between.
This page is for whoever writes the sidecar or a client addon that talks to it.

## The problem

A request used to travel as a whisper to a bot, which the LLM hook handed to the sidecar,
and the answer came back as whispers from that bot.
So every request needed some bot to be online, and the addon had to know which.
With every character of the player's logged out there was nobody to ask,
not even to log one of them back in.

## The command

```text
.bot ik <request>
.bot ik @<name> <request>
```

The server reads a chat line that starts with `.` as a command and shows it to nobody,
so an addon can send it with `SendChatMessage(text, "SAY")`.
Only a player in the game can ask; the console cannot.
A request about one character names it first with `@<name>`, and that character must be in the world.

The answer to the command itself is empty.
The fork posts the request to `AiPlayerbot.InnkeeperControlUrl` on a thread of its own,
waits up to `AiPlayerbot.InnkeeperControlTimeout` seconds,
and sends every line of the answer to the player as an addon message:
`IK`, a tab, the line, on the whisper lane, with the player as the sender.
The client fires `CHAT_MSG_ADDON` with the prefix `IK` for each line, and no chat frame shows it.
The lines go out on the world thread's next update after the answer arrives, in order.

## The request

A POST with a JSON body. The characters have the shape of the LLM template's:

```json
{"player":{"name":"Mathias","race":"Human","class":"Warrior","gender":"male","level":"18","zone":"Westfall","subzone":"Moonbrook"},
 "account":205,
 "target":{"name":"Pia","race":"Dwarf","class":"Priest","gender":"female","level":"31","zone":"Dun Morogh","subzone":"Kharanos"},
 "text":"ik persona get"}
```

`target` is there only when an `@<name>` named a character.
`text` is `ik`, a space, and the rest of the command as typed.

## The answer

Plain text, one reply line per line.
Empty lines are dropped.
The fork reads everything after the first blank line of the response, as it does for the LLM hook,
so the sidecar must close the connection or send a `Content-Length`.

## Replies the fork writes itself

| Line | When |
|---|---|
| `ik: error <name> is not online` | The `@<name>` named nobody in the world. Nothing is posted. |
| `ik: error innkeeper is not set up` | `AiPlayerbot.InnkeeperControlUrl` is empty. |
| `ik: error innkeeper is busy, try again` | Eight requests are already waiting on the sidecar. |
| `ik: error innkeeper did not answer` | The post failed, or the answer had no lines, within the timeout. |

## The settings

```text
AiPlayerbot.InnkeeperControlUrl = http://innkeeper:8787/v1/control
AiPlayerbot.InnkeeperControlTimeout = 90
```

A party fill brings and prepares up to four bots before it answers, which can take a minute,
so the timeout is generous.
A player who logs out before the answer arrives does not get it.
