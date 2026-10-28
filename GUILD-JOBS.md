# The guild's jobs

Three jobs for the competition (`docs/competition.md`): `pay`, `reset` and `craft`.
They are in `playerbot/GuildJobs.cpp`; `GuildBotMgr` holds the orders and reports them as it does for the others.
This page is to be folded into `docs/engine.md`.

Each one acts only while the character is alive, in the world, settled and out of combat;
until then its status stays `working`, and the reason says what it waits for,
such as `waiting: dead`, `waiting: in combat`, `waiting: between places` or `waiting: on a flight`.
`reset` needs only the character in the world.
The character stands where it is on all three, as on `idle`,
but for a `pay` order with `at`, which travels to a counter first; see below.

## Jobs

| Job | Arguments | What the character does |
|---|---|---|
| `pay` | `ref=<number>`, `gold=<copper>` (optional), `items=<entry>:<count>,<entry>:<count>` (optional), `at=guild_master` or `at=emissary` (optional) | Hands in the gold and the items from its bags, which leave the game. All or nothing: if it lacks the gold or any item, it takes nothing and fails. With `at`, it travels to the nearest counter of that kind and pays there. Done when paid |
| `reset` | `map=<raid map id>` | Clears the lockout to one raid for every character on its account, so the guild's next entry opens a fresh instance. Refuses while any of them is inside. Done when cleared |
| `craft` | `item=<entry made>` or `spell=<recipe spell id>`, `amount=<n>` (optional, 1 by default) | Makes the item with a recipe it knows, from the reagents in its bags, where it stands. Done when it has made the amount. The reason reports the count |

A `pay` order needs `ref` and at least one of `gold` and `items`.
The ref is the guild service's number for the payment, and it comes back in the event.
A ref is paid once per character:
an order whose ref the character already paid is done at once, with the reason `already paid ref=17`, and takes nothing.
That is what keeps a restart from paying twice. The character is saved before the `paid` event is written,
in the same queue, so the event is never there without the payment.
Items are counted in the backpack, the keyring and the bags; equipped items and items in a trade do not count.

## Paying at a counter

The guild service sends every purchase with `at`, such as `pay at=guild_master gold=50000 ref=7`
or `pay at=emissary items=200001:3 ref=9`, so a purchase is paid in a capital, not wherever the character stands.
Without `at`, `pay` pays where it stands.

The counters are the capitals' Guild Masters, the creatures with the guild charter flag spawned on a continent:
Aldwin Laughlin in Stormwind, Jondor Steelbrow in Ironforge and Lysheana in Darnassus for the Alliance,
Urtrun Clanbringer in Orgrimmar, Christopher Drakul in Undercity and Krumn in Thunder Bluff for the Horde.
A counter serves the characters its Guild Master's faction is friendly to.
The emissary of the Bronze Flight, creature 200100, stands beside each Guild Master;
each city's emissary is the spawn of it nearest its Guild Master, within 40 yards.
A city with no emissary spawned yet has its emissary's counter at the Guild Master's spot,
so `at=emissary` works before the emissary is in the world database.
The spawns are read once, at the first such order, and the world database is read at startup only.

At its first check the order picks the counter of its kind with the shortest route on the travel map,
from where the character stands, among those of its faction.
With no route to any, it takes the nearest on the character's map, then the nearest by way of the map transfers.
Every faction has a capital on both continents, so a character on a continent goes to one on its own,
unless the travel map's route to one across the sea is shorter.
The choice holds for the order; a restart chooses again.

The money and items are checked before it sets out, and again at every check, so it does not travel to a payment
it cannot make. On the way it has the travel strategy and none of the town services:
no trips to sell or repair, which could spend the gold it carries.
The travel named `guild job` takes it there, by `GuildJobs::ShouldTravel` and `GuildJobs::Destination`.
It pays once it is within 8 yards of the counter's spawn, talking range;
travel counts it as arrived at 5. Bots pay without a gossip window.

A character that comes no 10 yards nearer to its counter for 20 minutes while it can move gives the order up,
with the counter and the nearest it came, such as
`could not reach the Guild Master in Ironforge: came no nearer than 340 yards in 20 minutes`.
Moving on to another map counts as progress, and waiting dead, in combat, between places or on a flight
starts the clock again.

A `reset` order takes only a raid's map, such as 409 for Molten Core.
Online characters are unbound through the core, as `.instance unbind` does,
and offline ones by deleting their binds to that map from `character_instance`.
A group that any of the account's characters is in loses its bind to the raid too,
since a group's bind would send its members back to the old instance.
It refuses while any character of the account is inside the raid,
an offline one included when it logged out there, and while any member of such a group is inside.
While one of them is between maps it waits.
The items a reset costs are handed in first, by a `pay` order; `reset` itself only unbinds.

A `craft` order with `item` uses the first recipe the character knows that makes the item.
Before each cast it checks the recipe's cooldown, its tools, such as a Blacksmith Hammer,
the object it needs nearby, such as an anvil or a forge, its reagents and room in the bags.
It counts what it made by the item appearing in its bags, and keeps the count in `guildbot_job_progress`,
so a restart carries on with the same order instead of starting it over.

## Statuses and reasons

| Job | Working | Done | Failed |
|---|---|---|---|
| `pay` | `on its way to the Guild Master in Ironforge`, `on its way to the emissary of the Bronze Flight in Orgrimmar`; `waiting: in combat` and the like | `paid 20g, 2 Thorium Widget (ref 17)`, with ` to the Guild Master in Ironforge` at a counter; `already paid ref=17` | `has 12g, needs 20g`; `has 3 of 5 Thorium Widget`; `could not reach the Guild Master in Ironforge: came no nearer than 340 yards in 20 minutes`; `no Guild Master known for its faction`; a malformed order, such as `pay needs ref=<number>, and gold=<copper> or items=<entry>:<count>,...`, `pay: no item 99999` or `pay: at must be guild_master or emissary` |
| `reset` | `waiting for Gbwar to arrive` | `reset Molten Core: 12 characters and 1 groups unbound` | `map 36 is not a raid`; `Gbwar is inside Molten Core`; `Gbwar is inside Molten Core, offline`; `Gbmage, in Gbwar's group, is inside Molten Core` |
| `craft` | `made 2 of 5 Thorium Widget`, with `; waiting: in combat` and the like | `made 5 of 5 Thorium Widget` | The count, then why: `out of Thorium Bar, has 1, needs 3`; `needs a Blacksmith Hammer`; `needs an anvil nearby`; `Transmute: Arcanite is on cooldown`; `bags full`; `could not cast Thorium Widget`. Before any count: `knows no recipe for Thorium Widget`, `does not know <recipe>`, `<recipe> makes no item` |

A craft cast that does not start six checks in a row, about half a minute, fails with `could not cast`.
A cooldown fails the order rather than waiting for it, which may be a day for a transmute;
the guild service gives the order again once the cooldown is over.

## Events

| Kind | When |
|---|---|
| `paid` | A `pay` order was paid; the detail is the ref, the gold in copper and the items, such as `ref=17 gold=200000 items=12655:2` |
| `raid_reset` | A `reset` order cleared a raid; the detail is the map and what was unbound, such as `map=409 Molten Core characters=12 groups=1` |
| `crafted` | A `craft` order made its amount; the detail is the item, the recipe and the count, such as `item=15994 spell=19791 made=5` |

The usual `job`, `job_done` and `job_failed` come with them,
the last two with the job's name and the reason, such as `pay has 12g, needs 20g`.

## Items guild characters keep

Every guild character keeps a short list of items, whatever its job:
it loots them, rolls need on them, and never sells, banks or destroys them,
as it does with the item its farm job collects.
They are the raid reset currency, 200001 Sands of Renewal,
and the crafts the guild's features and budget increases cost:
4382 Bronze Framework, 4389 Gyrochronatom, 10560 Unstable Trigger, 10561 Mithril Casing, 12360 Arcanite Bar,
12655 Enchanted Thorium Bar, 14342 Mooncloth, 15994 Thorium Widget and 18631 Truesilver Transformer.
The list is `GuildJobs::AlwaysKeep`; `ItemUsageValue` gives them the guild task usage.

## Gathering

There is no `gather` job, since `farm` already gathers.
With `item=` of a herb or an ore, and the gathering skill, it travels to the nodes the item comes from and gathers there.
Skinning is the gap: the drop list the farm order reads has only creature corpse loot and object loot,
not skinning loot, so a farm order for a leather finds nowhere to go.
