# The guild's jobs

Three jobs for the competition (`docs/competition.md`): `pay`, `reset` and `craft`.
They are in `playerbot/GuildJobs.cpp`; `GuildBotMgr` holds the orders and reports them as it does for the others.
This page is to be folded into `docs/engine.md`.

Each one acts only while the character is alive, in the world, settled and out of combat;
until then its status stays `working`, and the reason says what it waits for,
such as `waiting: dead`, `waiting: in combat`, `waiting: between places` or `waiting: on a flight`.
`reset` needs only the character in the world.
The character stands where it is on all three, as on `idle`.
None of them travels; the character must already be where the job can be done.

## Jobs

| Job | Arguments | What the character does |
|---|---|---|
| `pay` | `ref=<number>`, `gold=<copper>` (optional), `items=<entry>:<count>,<entry>:<count>` (optional) | Hands in the gold and the items from its bags, which leave the game. All or nothing: if it lacks the gold or any item, it takes nothing and fails. Done when paid |
| `reset` | `map=<raid map id>` | Clears the lockout to one raid for every character on its account, so the guild's next entry opens a fresh instance. Refuses while any of them is inside. Done when cleared |
| `craft` | `item=<entry made>` or `spell=<recipe spell id>`, `amount=<n>` (optional, 1 by default) | Makes the item with a recipe it knows, from the reagents in its bags, where it stands. Done when it has made the amount. The reason reports the count |

A `pay` order needs `ref` and at least one of `gold` and `items`.
The ref is the guild service's number for the payment, and it comes back in the event.
A ref is paid once per character:
an order whose ref the character already paid is done at once, with the reason `already paid ref=17`, and takes nothing.
That is what keeps a restart from paying twice. The character is saved before the `paid` event is written,
in the same queue, so the event is never there without the payment.
Items are counted in the backpack, the keyring and the bags; equipped items and items in a trade do not count.

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
| `pay` | `waiting: in combat` and the like | `paid 20g, 2 Thorium Widget (ref 17)`; `already paid ref=17` | `has 12g, needs 20g`; `has 3 of 5 Thorium Widget`; a malformed order, such as `pay needs ref=<number>, and gold=<copper> or items=<entry>:<count>,...` or `pay: no item 99999` |
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
