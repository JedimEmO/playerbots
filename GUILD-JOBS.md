# The guild's jobs

Five jobs for the competition (`docs/competition.md`): `pay`, `reset`, `craft`, `kit` and `deposit`.
They are in `playerbot/GuildJobs.cpp`; `GuildBotMgr` holds the orders and reports them as it does for the others.
This page is to be folded into `docs/engine.md`.

Each one acts only while the character is alive, in the world, settled and out of combat;
until then its status stays `working`, and the reason says what it waits for,
such as `waiting: dead`, `waiting: in combat`, `waiting: between places` or `waiting: on a flight`.
`reset` needs only the character in the world.
The character stands where it is on `reset`, `craft` and `pay`, as on `idle`,
but for a `pay` order with `at`, which travels to a counter first,
and `kit` and `deposit`, which travel to the vaultkeeper and to vendors; see below.

## Jobs

| Job | Arguments | What the character does |
|---|---|---|
| `pay` | `ref=<number>`, `gold=<copper>` (optional), `items=<entry>:<count>,<entry>:<count>` (optional), `at=guild_master` or `at=emissary` (optional) | Hands in the gold and the items from its bags, which leave the game. All or nothing: if it lacks the gold or any item, it takes nothing and fails. With `at`, it travels to the nearest counter of that kind and pays there. Done when paid |
| `reset` | `map=<raid map id>` | Clears the lockout to one raid for every character on its account, so the guild's next entry opens a fresh instance. Refuses while any of them is inside. Done when cleared |
| `craft` | `item=<entry made>` or `spell=<recipe spell id>`, `amount=<n>` (optional, 1 by default) | Makes the item with a recipe it knows, from the reagents in its bags, where it stands. Done when it has made the amount. The reason reports the count |
| `kit` | `loadout=<name>` (optional, for the reason and the event), `items=<entry>:<count>,<entry>:<count>` | Brings what it carries of each item up to at least the count: takes what the guild's vault has, then buys from vendors near it with its own gold. Done when it holds the counts or has got all it could, with what is short |
| `deposit` | `items=<entry>:<count>,<entry>:<count>`, a count of 0 for all it carries of one | Travels to the nearest vaultkeeper and puts the items in the guild's vault. Done with what went in, what the vault refused and why |

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

## Kit and deposit

A guild keeps a stockpile in its vault, the core's `GuildVault`, kept by a vaultkeeper (creature 200101) beside each capital's Guild Master.
The guild service works out a loadout's counts for one character, such as `kit loadout=molten_core items=6049:5,13444:10`,
and the engine only brings what the character carries up to them.
Counts are of the backpack, the keyring and the bags, as for `pay`.

At its first check, `kit` works out what it lacks.
It goes to the vault only if the vault has some of that:
to the nearest vaultkeeper of its faction, chosen as a pay counter is, by the travel named `guild job`.
Each city's vaultkeeper is the spawn of 200101 nearest its Guild Master, within 40 yards;
a city with none spawned yet has its vaultkeeper's place at the Guild Master's spot, as the emissary has.
Within 8 yards it takes, with `GuildVault::Withdraw`, as much of each as the vault holds and the bags take.
Another character of the guild may have taken some on the way; it takes what is left.
A vaultkeeper it comes no 10 yards nearer to in 20 minutes is given up, and the order goes on to the vendors.

Then it buys what it still lacks from vendors, with its own gold.
It goes to the nearest vendor on its map, within 400 yards, that sells something it lacks for no more than its gold,
and buys there everything that vendor sells of what it lacks, a stack at a time, as its gold and bags allow.
Then the next nearest, up to six vendors an order.
After the vault that is the capital it stands in; without a vault, the town or the field around it.
The vendors are read once, at the first `kit` order, from the world database's spawns and vendor lists:
those on a continent whose faction is friendly to the character,
and not one with three creatures hostile to its team within 30 yards, such as the Defias Profiteer in Moonbrook.
Travel stops within 5 yards of the vendor's spawn; if it cannot trade from there, it steps up to the vendor,
and gives the vendor up after six checks, or after 5 minutes without coming 10 yards nearer.
It buys through the core's `Player::BuyItemFromVendor`, as the playerbots' `buy` action does,
but not through that action: its `vendor` mode buys by item usage, and its item mode tells a master.
A vendor sold out, or an item that needs a reputation it lacks, leaves that item short.

`kit` is done when it holds every count, or when no vault, vendor, gold or room in its bags gets it more.
Being short is done, not failed, so the job queued after it, such as a raid, still goes.
It fails only on a malformed order, or when its bags take nothing at all of what it lacks,
before it has taken or bought anything.
A restart starts the order over from what it carries, which takes and buys nothing twice.

`deposit` checks before it sets out that the account has a vault and that the vault would take something,
by `GuildVault::IsFungible`; with nothing to put in it is done where it stands.
At the vaultkeeper it puts each stack in with `GuildVault::Deposit`, up to the count, or every stack for 0.
A deposit is put in once: after a restart that lost the done status,
the `deposited` event of the order, written after the vault's change in the same queue,
says it was put in already. The event names the order's `job_seq` for that.
A vaultkeeper it comes no 10 yards nearer to in 20 minutes fails the order, as a pay counter does.

While either order runs, the character keeps the items it names, as the ones below:
it loots them, rolls need on them, and never sells, banks or destroys them.
On the way it has the travel strategy and none of the town services, as `pay` at a counter has.

The vault is on the core's `competition` branch until it is merged.
The engine builds without it, by `__has_include("Guilds/GuildVault.h")`:
then `kit` buys from vendors only, and `deposit` fails with `this server has no guild vault`.

## Statuses and reasons

| Job | Working | Done | Failed |
|---|---|---|---|
| `pay` | `on its way to the Guild Master in Ironforge`, `on its way to the emissary of the Bronze Flight in Orgrimmar`; `waiting: in combat` and the like | `paid 20g, 2 Thorium Widget (ref 17)`, with ` to the Guild Master in Ironforge` at a counter; `already paid ref=17` | `has 12g, needs 20g`; `has 3 of 5 Thorium Widget`; `could not reach the Guild Master in Ironforge: came no nearer than 340 yards in 20 minutes`; `no Guild Master known for its faction`; a malformed order, such as `pay needs ref=<number>, and gold=<copper> or items=<entry>:<count>,...`, `pay: no item 99999` or `pay: at must be guild_master or emissary` |
| `reset` | `waiting for Gbwar to arrive` | `reset Molten Core: 12 characters and 1 groups unbound` | `map 36 is not a raid`; `Gbwar is inside Molten Core`; `Gbwar is inside Molten Core, offline`; `Gbmage, in Gbwar's group, is inside Molten Core` |
| `craft` | `made 2 of 5 Thorium Widget`, with `; waiting: in combat` and the like | `made 5 of 5 Thorium Widget` | The count, then why: `out of Thorium Bar, has 1, needs 3`; `needs a Blacksmith Hammer`; `needs an anvil nearby`; `Transmute: Arcanite is on cooldown`; `bags full`; `could not cast Thorium Widget`. Before any count: `knows no recipe for Thorium Widget`, `does not know <recipe>`, `<recipe> makes no item` |
| `kit` | `on its way to the vaultkeeper in Stormwind`; `on its way to buy Sacred Candle from Keldric Boucher`; `on its way to buy from Keldric Boucher`; `stepping up to Keldric Boucher`; `waiting: in combat` and the like | `kitted molten_core`; `kitted molten_core; short 4 Major Mana Potion`, with `; bags full`, `; out of gold` or `; could not reach the vaultkeeper in Stormwind: came no nearer than 340 yards in 20 minutes` when one of those stopped it | `bags full; short 4 Major Mana Potion`; a malformed order, such as `kit needs items=<entry>:<count>,...` or `kit: no item 99999` |
| `deposit` | `on its way to the vaultkeeper in Orgrimmar`; `waiting: dead` and the like | `deposited 20 Major Mana Potion`, with `; refused 1 Hearthstone (Soulbound items cannot go in the vault)` for each refusal and `; carries 6 of 10 Thorium Bar, none of Arcanite Bar` for what it had less of; `deposited nothing; ...` when the vault would take none, without travelling; `already deposited this order` | `this server has no guild vault`; `has no guild vault`; `no vaultkeeper known for its faction`; `could not reach the vaultkeeper in Orgrimmar: ...`; a malformed order, such as `deposit: items must be <entry>:<count>,<entry>:<count>` |

A craft cast that does not start six checks in a row, about half a minute, fails with `could not cast`.
A cooldown fails the order rather than waiting for it, which may be a day for a transmute;
the guild service gives the order again once the cooldown is over.

## Events

| Kind | When |
|---|---|
| `paid` | A `pay` order was paid; the detail is the ref, the gold in copper and the items, such as `ref=17 gold=200000 items=12655:2` |
| `raid_reset` | A `reset` order cleared a raid; the detail is the map and what was unbound, such as `map=409 Molten Core characters=12 groups=1` |
| `crafted` | A `craft` order made its amount; the detail is the item, the recipe and the count, such as `item=15994 spell=19791 made=5` |
| `kitted` | A `kit` order is done; the detail is the loadout, what it took from the vault, what it bought and the copper that cost, and what is short, each part only when there is one, such as `loadout=molten_core vault=13444:6 bought=17029:10 spent=10000 short=13444:4` |
| `deposited` | A `deposit` order put something in; the detail is the order's `job_seq` and what went in, such as `seq=12 items=13444:20,12359:5` |

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
