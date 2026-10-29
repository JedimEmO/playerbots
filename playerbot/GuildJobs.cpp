#include "playerbot/playerbot.h"
#include "playerbot/GuildJobs.h"
#include "playerbot/GuildBotMgr.h"
#include "playerbot/ServerFacade.h"
#include "playerbot/TravelMgr.h"
#include "playerbot/TravelNode.h"
#include "Entities/Bag.h"
#include "Groups/Group.h"
#include "Grids/GridNotifiers.h"
#include "Grids/GridNotifiersImpl.h"
#include "Grids/CellImpl.h"

// The guild vault is the core's, on its competition branch until it is merged. Without it, kit buys from vendors only
// and deposit fails.
#if __has_include("Guilds/GuildVault.h")
#include "Guilds/GuildVault.h"
#define GUILD_JOBS_VAULT 1
#else
#define GUILD_JOBS_VAULT 0
#endif

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <memory>
#include <mutex>
#include <set>
#include <tuple>
#include <sstream>
#include <tuple>

using namespace ai;

namespace
{
    // A cast that does not start this many checks in a row, about half a minute, is not going to.
    constexpr uint32 CRAFT_CAST_TRIES = 6;
    // At most this many items on one craft order.
    constexpr uint64 CRAFT_MAX_AMOUNT = 1000;

    // The emissary of the Bronze Flight, who stands beside each capital's Guild Master.
    constexpr uint32 EMISSARY_ENTRY = 200100;
    // The Steamwheedle Vaultkeeper, who keeps the guild's vault beside each capital's Guild Master;
    // GuildVault::VAULTKEEPER_ENTRY, here for a core without the vault too.
    constexpr uint32 VAULTKEEPER_ENTRY = 200101;
    // An emissary or vaultkeeper spawn this near a Guild Master is that city's.
    constexpr float EMISSARY_NEAR = 40.0f;
    // Talking range: a character this near its counter pays. A little more than the 5 yards travel counts as
    // arrived, since it walks to a point up to 5 yards from the counter's spawn.
    constexpr float COUNTER_REACH = 8.0f;
    // On the way to a counter, a character that comes no 10 yards nearer to it in 20 minutes, while able to move,
    // gives the order up.
    constexpr float COUNTER_PROGRESS = 10.0f;
    constexpr time_t COUNTER_STALL = 20 * MINUTE;

    // kit buys from vendors this near it, on its map: in practice the capital it kitted from the vault in.
    constexpr float VENDOR_RANGE = 400.0f;
    // A vendor with this many creatures hostile to the character within 30 yards stands in a hostile camp, as the
    // Defias Profiteer in Moonbrook does, and is not gone to (docs/lessons.md).
    constexpr float VENDOR_CAMP_RADIUS = 30.0f;
    constexpr uint32 VENDOR_CAMP_HOSTILES = 3;
    // A vendor it comes no 10 yards nearer to in 5 minutes is given up, and so is one it cannot step up to and
    // trade with in six checks, about half a minute.
    constexpr time_t VENDOR_STALL = 5 * MINUTE;
    constexpr uint32 VENDOR_APPROACHES = 6;
    // At most this many vendors on one kit order.
    constexpr size_t KIT_MAX_VENDORS = 6;
    // At most this many of an item on a pay, kit or deposit order.
    constexpr uint64 ORDER_MAX_COUNT = 10000;

    enum class CounterKind : uint8 { GuildMaster, Emissary, Vaultkeeper };

    // A place a pay order is handed in at, or the vault is reached from: a capital's Guild Master,
    // or the emissary or the vaultkeeper beside one.
    struct Counter
    {
        CounterKind kind = CounterKind::GuildMaster;
        uint32 entry = 0;            // the creature
        uint32 spawn = 0;            // its spawn, 0 for a place taken from its Guild Master's
        uint32 faction = 0;          // the Guild Master's faction template: whom the counter serves
        WorldPosition pos;
        std::string where;           // "the Guild Master in Ironforge"
        std::unique_ptr<PlaceTravelDestination> place;
    };

    // Indexed once from the world database's spawns, and never cleared: travel targets hold their places.
    std::mutex counterLock;
    bool countersIndexed = false;
    std::vector<Counter> counters;

    // Where a character's order goes now: a counter or a vendor, by character. Read by the travel values off the
    // world thread, so under counterLock.
    struct Trip
    {
        WorldPosition pos;
        TravelDestination* place = nullptr;
        std::string where;
    };
    std::map<uint32, Trip> trips;

    // A vendor spawn kit may buy from, on a continent, with what it sells.
    struct Vendor
    {
        uint32 entry = 0;
        uint32 faction = 0;
        WorldPosition pos;
        std::string name;
        uint32 hostileToAlliance = 0;   // creatures within VENDOR_CAMP_RADIUS hostile to each team
        uint32 hostileToHorde = 0;
        std::unique_ptr<PlaceTravelDestination> place;   // made when first gone to
    };

    // Indexed once at the first kit order and used on the world thread only, where the jobs run.
    bool vendorsIndexed = false;
    std::vector<Vendor> vendors;
    std::map<uint32, std::vector<size_t>> vendorsOf;    // by item sold

    // The stages of a kit order.
    enum KitStage : uint8 { KIT_PLAN = 0, KIT_VAULT, KIT_VENDORS };

    // The capitals' Guild Masters, by entry, for their city's name.
    std::string CityOf(uint32 entry, CreatureData const& data)
    {
        switch (entry)
        {
            case 4974: return "Stormwind";       // Aldwin Laughlin
            case 5130: return "Ironforge";       // Jondor Steelbrow
            case 4161: return "Darnassus";       // Lysheana
            case 3370: return "Orgrimmar";       // Urtrun Clanbringer
            case 4613: return "Undercity";       // Christopher Drakul
            case 5054: return "Thunder Bluff";   // Krumn
            default: break;
        }
        if (AreaTableEntry const* zone = GetAreaEntryByAreaID(data.OriginalZoneId))
            if (const char* name = zone->area_name[sWorld.GetDefaultDbcLocale()])
                return name;
        return "";
    }

    // Called with counterLock held.
    void IndexCounters()
    {
        if (countersIndexed)
            return;
        countersIndexed = true;

        // Guild Masters, the creatures with the guild charter flag, and the emissaries and vaultkeepers, spawned on a continent.
        std::vector<std::pair<uint32, CreatureData>> masters, emissaries, vaultkeepers;
        auto index = [&](auto const& pair)
        {
            CreatureData const& data = pair.second;
            MapEntry const* map = sMapStore.LookupEntry(data.mapid);
            if (!map || !map->IsContinent() || data.gameEvent)
                return false;
            if (data.id == EMISSARY_ENTRY)
                emissaries.push_back({ pair.first, data });
            else if (data.id == VAULTKEEPER_ENTRY)
                vaultkeepers.push_back({ pair.first, data });
            else if (CreatureInfo const* info = ObjectMgr::GetCreatureTemplate(data.id); info && (info->NpcFlags & UNIT_NPC_FLAG_PETITIONER))
                masters.push_back({ pair.first, data });
            return false;
        };
        sObjectMgr.DoCreatureData(index);

        for (auto const& [guid, data] : masters)
        {
            CreatureInfo const* info = ObjectMgr::GetCreatureTemplate(data.id);
            const std::string city = CityOf(data.id, data);
            const WorldPosition at(data.mapid, data.posX, data.posY, data.posZ, data.orientation);

            Counter master;
            master.kind = CounterKind::GuildMaster;
            master.entry = data.id;
            master.spawn = guid;
            master.faction = info->Faction;
            master.pos = at;
            master.where = city.empty() ? std::string("the Guild Master ") + info->Name : "the Guild Master in " + city;
            master.place = std::make_unique<PlaceTravelDestination>(at, master.where);
            counters.push_back(std::move(master));

            // The emissary and the vaultkeeper nearest this Guild Master, or, until one is spawned, the Guild Master's own spot.
            auto beside = [&](CounterKind kind, uint32 entry, std::vector<std::pair<uint32, CreatureData>> const& spawns, const std::string& where)
            {
                Counter c;
                c.kind = kind;
                c.entry = entry;
                c.faction = info->Faction;
                c.pos = at;
                float best = EMISSARY_NEAR;
                for (auto const& [spawnGuid, spawn] : spawns)
                {
                    const WorldPosition there(spawn.mapid, spawn.posX, spawn.posY, spawn.posZ, spawn.orientation);
                    if (there.getMapId() == at.getMapId() && there.distance(at) <= best)
                    {
                        best = there.distance(at);
                        c.spawn = spawnGuid;
                        c.pos = there;
                    }
                }
                c.where = where + (city.empty() ? "" : " in " + city);
                c.place = std::make_unique<PlaceTravelDestination>(c.pos, c.where);
                counters.push_back(std::move(c));
            };
            beside(CounterKind::Emissary, EMISSARY_ENTRY, emissaries, "the emissary of the Bronze Flight");
            beside(CounterKind::Vaultkeeper, VAULTKEEPER_ENTRY, vaultkeepers, "the vaultkeeper");
        }
        sLog.outString("GuildJobs: %u Guild Masters, %u emissary and %u vaultkeeper spawns for counters",
            uint32(masters.size()), uint32(emissaries.size()), uint32(vaultkeepers.size()));
    }

    // How far from a place the travel map's nearest node may be for a route to start or end there. The capitals'
    // nodes are sparse: the nearest to the Guild Master in Ironforge is 150 yards off, in Orgrimmar 89.
    constexpr float ROUTE_NODE_REACH = 200.0f;

    // The length of the travel map's route between two places for the character, or -1 for none. The search is the
    // one its travel makes, so a route takes no lift a guild character may not ride, and may start with its
    // hearthstone or a teleport spell. Those steps, and boats and zeppelins, count as no distance, so a route across
    // the sea can measure short.
    float RouteLength(Player* bot, const WorldPosition& from, const WorldPosition& to)
    {
        std::shared_lock<std::shared_timed_mutex> guard(sTravelNodeMap.m_nMapMtx);
        std::vector<TravelNode*> starts = sTravelNodeMap.getNodes(from, ROUTE_NODE_REACH);
        std::vector<TravelNode*> goals = sTravelNodeMap.getNodes(to, ROUTE_NODE_REACH);
        if (starts.empty() || goals.empty())
            return -1.0f;
        TravelNode* start = starts.front();
        TravelNode* goal = goals.front();
        float length = from.distance(*start->getPosition()) + goal->getPosition()->distance(to);
        if (start == goal)
            return length;
        TravelNodeRoute route = sTravelNodeMap.getRoute(start, goal, bot);
        std::vector<TravelNode*>& nodes = route.getNodes();
        if (nodes.size() < 2)
            length = -1.0f;
        // A step by spell is a path but no link, so paths are what is measured.
        for (size_t i = 0; length >= 0.0f && i + 1 < nodes.size(); ++i)
        {
            if (!nodes[i]->hasPathTo(nodes[i + 1]))
                length = -1.0f;
            else
                length += nodes[i]->linkDistanceTo(nodes[i + 1]);
        }
        route.cleanTempNodes();
        return length;
    }

    // The counter of the kind nearest the character among those of its faction. One on the character's own map comes
    // first, since a boat or zeppelin measures as no distance but can take longer than a walk; among them the shortest
    // route on the travel map, then with no route the nearest in a line. Off its map, the same by way of the map
    // transfers. -1 for none. Called once the counters are indexed.
    int NearestCounter(Player* bot, CounterKind kind)
    {
        FactionTemplateEntry const* own = bot->GetFactionTemplateEntry();
        if (!own)
            return -1;
        const WorldPosition from(bot);
        int best = -1;
        std::tuple<bool, int, float> bestKey(true, 2, FLT_MAX);
        for (size_t i = 0; i < counters.size(); ++i)
        {
            Counter const& c = counters[i];
            if (c.kind != kind)
                continue;
            FactionTemplateEntry const* theirs = sFactionTemplateStore.LookupEntry(c.faction);
            if (!theirs || !theirs->IsFriendlyTo(*own) || theirs->IsHostileTo(*own))
                continue;
            const float route = RouteLength(bot, from, c.pos);
            const float straight = from.getMapId() == c.pos.getMapId() ? from.distance(c.pos) : sTravelMgr.MapTransDistance(from, c.pos);
            const bool otherMap = from.getMapId() != c.pos.getMapId();
            const std::tuple<bool, int, float> key = route >= 0.0f ? std::make_tuple(otherMap, 0, route) : std::make_tuple(otherMap, 1, straight);
            if (best < 0 || key < bestKey)
            {
                best = int(i);
                bestKey = key;
            }
        }
        return best;
    }

    bool InReach(Player* bot, const WorldPosition& counter)
    {
        return bot->GetMapId() == counter.getMapId() && WorldPosition(bot).distance(counter) <= COUNTER_REACH;
    }

    void EndTrip(uint32 guid)
    {
        std::lock_guard<std::mutex> guard(counterLock);
        trips.erase(guid);
    }

    void StartTrip(uint32 guid, const WorldPosition& pos, TravelDestination* place, const std::string& where)
    {
        std::lock_guard<std::mutex> guard(counterLock);
        trips[guid] = Trip{ pos, place, where };
    }

    // The trip a character's order is on, if any.
    bool CurrentTrip(uint32 guid, Trip& out)
    {
        std::lock_guard<std::mutex> guard(counterLock);
        auto it = trips.find(guid);
        if (it == trips.end())
            return false;
        out = it->second;
        return true;
    }

    // The counter of the kind nearest the character, as a trip; false when its faction has none.
    bool CounterTrip(Player* bot, CounterKind kind, Trip& out)
    {
        {
            std::lock_guard<std::mutex> guard(counterLock);
            IndexCounters();
        }
        // The counters do not change once indexed, so the routes are measured without the lock,
        // which the travel values of characters on other maps take meanwhile.
        const int chosen = NearestCounter(bot, kind);
        if (chosen < 0)
            return false;
        out = Trip{ counters[chosen].pos, counters[chosen].place.get(), counters[chosen].where };
        return true;
    }

    // The value of key=value in a job's arguments, or empty.
    std::string Arg(const std::string& args, const std::string& key)
    {
        std::istringstream in(args);
        std::string word;
        while (in >> word)
        {
            size_t eq = word.find('=');
            if (eq != std::string::npos && word.substr(0, eq) == key)
                return word.substr(eq + 1);
        }
        return "";
    }

    // A whole number of digits only, up to ten of them.
    bool ParseNumber(const std::string& text, uint64& out)
    {
        if (text.empty() || text.size() > 10 || !std::all_of(text.begin(), text.end(), [](unsigned char ch) { return std::isdigit(ch); }))
            return false;
        out = std::stoull(text);
        return true;
    }

    // Copper as the game shows it: 12g 3s 40c, without the parts that are nothing.
    std::string Money(uint32 copper)
    {
        std::string out;
        if (copper >= 10000)
            out += std::to_string(copper / 10000) + "g";
        if ((copper / 100) % 100)
            out += (out.empty() ? "" : " ") + std::to_string((copper / 100) % 100) + "s";
        if (copper % 100 || out.empty())
            out += (out.empty() ? "" : " ") + std::to_string(copper % 100) + "c";
        return out;
    }

    std::string ItemName(uint32 entry)
    {
        ItemPrototype const* proto = sObjectMgr.GetItemPrototype(entry);
        return proto ? proto->Name1 : "item " + std::to_string(entry);
    }

    // The stacks of the item in its backpack, keyring and bags, not equipped, not in a bag slot and not in a trade:
    // the ones Player::DestroyItemCount takes before it reaches the equipment, and the ones the vault takes.
    std::vector<Item*> StacksInBags(Player* bot, uint32 entry)
    {
        std::vector<Item*> stacks;
        auto add = [&](Item* item)
        {
            if (item && item->GetEntry() == entry && !item->IsInTrade())
                stacks.push_back(item);
        };
        for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
            add(bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot));
        for (uint8 slot = KEYRING_SLOT_START; slot < KEYRING_SLOT_END; ++slot)
            add(bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot));
        for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
            if (Bag* bag = dynamic_cast<Bag*>(bot->GetItemByPos(INVENTORY_SLOT_BAG_0, bagSlot)))
                for (uint32 slot = 0; slot < bag->GetBagSize(); ++slot)
                    add(bag->GetItemByPos(uint8(slot)));
        return stacks;
    }

    // How many of the item those stacks hold.
    uint32 CountInBags(Player* bot, uint32 entry)
    {
        uint32 count = 0;
        for (Item* item : StacksInBags(bot, entry))
            count += item->GetCount();
        return count;
    }

    // <entry>:<count>,... into out, in the order given, each entry once: the counts of an entry given twice add up,
    // and with allowAll a count of 0, all the character carries, stays 0. Empty when it parsed, else what is wrong.
    std::string ParseItems(const std::string& text, const std::string& job, bool allowAll, std::vector<std::pair<uint32, uint32>>& out)
    {
        std::istringstream in(text);
        std::string part;
        while (std::getline(in, part, ','))
        {
            size_t colon = part.find(':');
            uint64 entry = 0, count = 0;
            if (colon == std::string::npos || !ParseNumber(part.substr(0, colon), entry) || !ParseNumber(part.substr(colon + 1), count) ||
                (!count && !allowAll) || count > ORDER_MAX_COUNT)
                return job + ": items must be <entry>:<count>,<entry>:<count>";
            if (!sObjectMgr.GetItemPrototype(uint32(entry)))
                return job + ": no item " + std::to_string(entry);
            auto it = std::find_if(out.begin(), out.end(), [entry](auto const& i) { return i.first == entry; });
            if (it == out.end())
                out.push_back({ uint32(entry), uint32(count) });
            else
                it->second = it->second && count ? it->second + uint32(count) : 0;
        }
        return out.empty() ? job + ": items must be <entry>:<count>,<entry>:<count>" : "";
    }

    // Whether an items= list names the entry, without checking the rest of it.
    bool ListsItem(const std::string& text, uint32 entry)
    {
        std::istringstream in(text);
        std::string part;
        while (std::getline(in, part, ','))
        {
            uint64 listed = 0;
            if (ParseNumber(part.substr(0, part.find(':')), listed) && listed == entry)
                return true;
        }
        return false;
    }

    // What it lacks of each item to have the count in its bags, in the order given.
    std::vector<std::pair<uint32, uint32>> Lacking(Player* bot, std::vector<std::pair<uint32, uint32>> const& items)
    {
        std::vector<std::pair<uint32, uint32>> lacking;
        for (auto const& [entry, count] : items)
        {
            const uint32 have = CountInBags(bot, entry);
            if (have < count)
                lacking.push_back({ entry, count - have });
        }
        return lacking;
    }

    // Room in its bags for one of anything it lacks.
    bool RoomForAny(Player* bot, std::vector<std::pair<uint32, uint32>> const& lacking)
    {
        for (auto const& [entry, count] : lacking)
        {
            ItemPosCountVec dest;
            if (bot->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, entry, 1) == EQUIP_ERR_OK)
                return true;
        }
        return false;
    }

    // Items for a reason: "4 Major Mana Potion, 10 Sacred Candle".
    template<class List>
    std::string Names(List const& items)
    {
        std::string out;
        for (auto const& [entry, count] : items)
            out += (out.empty() ? "" : ", ") + std::to_string(count) + " " + ItemName(entry);
        return out;
    }

    // Items for an event's detail: "13444:4,17029:10".
    template<class List>
    std::string Entries(List const& items)
    {
        std::string out;
        for (auto const& [entry, count] : items)
            out += (out.empty() ? "" : ",") + std::to_string(entry) + ":" + std::to_string(count);
        return out;
    }

    // On the way to a place, at a check while it can move: true once it has come no COUNTER_PROGRESS yards nearer
    // in `stall`, with why, such as "came no nearer than 340 yards in 20 minutes". Coming 10 yards nearer on the
    // place's map, or moving on to another map, is progress. nearestAt 0 starts the clock again.
    bool Stalled(Player* bot, const WorldPosition& to, time_t stall, float& nearest, uint32& nearestMap, time_t& nearestAt, std::string& why)
    {
        const time_t now = time(nullptr);
        const float distance = bot->GetMapId() == to.getMapId() ? WorldPosition(bot).distance(to) : FLT_MAX;
        if (!nearestAt || bot->GetMapId() != nearestMap || (distance < FLT_MAX && distance + COUNTER_PROGRESS <= nearest))
        {
            nearest = distance;
            nearestMap = bot->GetMapId();
            nearestAt = now;
            return false;
        }
        if (now - nearestAt < stall)
            return false;
        if (distance < FLT_MAX)
            why = "came no nearer than " + std::to_string(uint32(nearest)) + " yards in " + std::to_string(stall / MINUTE) + " minutes";
        else
            why = "did not reach its map in " + std::to_string(stall / MINUTE) + " minutes";
        return true;
    }

    // The vendors on a continent with anything for sale, from the world database's spawns and vendor lists,
    // and how many creatures hostile to each team stand around each.
    void IndexVendors()
    {
        if (vendorsIndexed)
            return;
        vendorsIndexed = true;

        auto onContinent = [](CreatureData const& data)
        {
            MapEntry const* map = sMapStore.LookupEntry(data.mapid);
            return map && map->IsContinent() && !data.gameEvent;
        };
        auto at = [](CreatureData const& data) { return WorldPosition(data.mapid, data.posX, data.posY, data.posZ, data.orientation); };

        // What each vendor entry sells: its own list and its vendor template's.
        std::map<uint32, std::set<uint32>> soldBy;
        auto sells = [](CreatureInfo const* info, std::set<uint32>& items)
        {
            auto add = [&items](VendorItemData const* list)
            {
                if (list)
                    for (VendorItem const* sold : list->m_items)
                        if (sold && sObjectMgr.GetItemPrototype(sold->item))
                            items.insert(sold->item);
            };
            add(sObjectMgr.GetNpcVendorItemList(info->Entry));
            if (info->VendorTemplateId)
                add(sObjectMgr.GetNpcVendorTemplateItemList(info->VendorTemplateId));
        };
        auto index = [&](auto const& pair)
        {
            CreatureData const& data = pair.second;
            if (!onContinent(data))
                return false;
            CreatureInfo const* info = ObjectMgr::GetCreatureTemplate(data.id);
            if (!info || !(info->NpcFlags & UNIT_NPC_FLAG_VENDOR))
                return false;
            auto [sold, fresh] = soldBy.try_emplace(data.id);
            if (fresh)
                sells(info, sold->second);
            if (sold->second.empty())
                return false;
            Vendor v;
            v.entry = data.id;
            v.faction = info->Faction;
            v.pos = at(data);
            v.name = info->Name;
            for (uint32 item : sold->second)
                vendorsOf[item].push_back(vendors.size());
            vendors.push_back(std::move(v));
            return false;
        };
        sObjectMgr.DoCreatureData(index);

        // The hostile creatures around each vendor, by a grid of cells the camp radius wide.
        typedef std::tuple<uint32, int32, int32> GridCell;
        auto cellOf = [](const WorldPosition& pos, int32 dx, int32 dy)
        {
            return GridCell(pos.getMapId(), int32(std::floor(pos.getX() / VENDOR_CAMP_RADIUS)) + dx, int32(std::floor(pos.getY() / VENDOR_CAMP_RADIUS)) + dy);
        };
        std::map<GridCell, std::vector<size_t>> cells;
        for (size_t i = 0; i < vendors.size(); ++i)
            cells[cellOf(vendors[i].pos, 0, 0)].push_back(i);
        FactionTemplateEntry const* alliance = sFactionTemplateStore.LookupEntry(1);   // a human's
        FactionTemplateEntry const* horde = sFactionTemplateStore.LookupEntry(2);      // an orc's
        std::map<uint32, std::pair<bool, bool>> hostility;                              // by faction template
        auto camps = [&](auto const& pair)
        {
            CreatureData const& data = pair.second;
            if (!onContinent(data))
                return false;
            CreatureInfo const* info = ObjectMgr::GetCreatureTemplate(data.id);
            if (!info)
                return false;
            auto [hostile, fresh] = hostility.try_emplace(info->Faction);
            if (fresh)
            {
                FactionTemplateEntry const* theirs = sFactionTemplateStore.LookupEntry(info->Faction);
                hostile->second = { theirs && alliance && theirs->IsHostileTo(*alliance), theirs && horde && theirs->IsHostileTo(*horde) };
            }
            if (!hostile->second.first && !hostile->second.second)
                return false;
            const WorldPosition pos = at(data);
            for (int32 dx = -1; dx <= 1; ++dx)
                for (int32 dy = -1; dy <= 1; ++dy)
                    if (auto near = cells.find(cellOf(pos, dx, dy)); near != cells.end())
                        for (size_t i : near->second)
                            if (vendors[i].pos.distance(pos) <= VENDOR_CAMP_RADIUS)
                            {
                                vendors[i].hostileToAlliance += hostile->second.first ? 1 : 0;
                                vendors[i].hostileToHorde += hostile->second.second ? 1 : 0;
                            }
            return false;
        };
        sObjectMgr.DoCreatureData(camps);

        sLog.outString("GuildJobs: %u vendor spawns selling %u items for kit orders", uint32(vendors.size()), uint32(vendorsOf.size()));
    }

    bool Sells(size_t vendor, uint32 entry)
    {
        auto it = vendorsOf.find(entry);
        return it != vendorsOf.end() && std::find(it->second.begin(), it->second.end(), vendor) != it->second.end();
    }

    // The nearest vendor on the character's map, within VENDOR_RANGE, of a faction friendly to it and not in a hostile
    // camp, that sells something it lacks for no more than its gold, passing over those it visited; -1 for none.
    // sells is the first such item; noGold is true when there is such a vendor, but only for more than its gold.
    int NearestVendor(Player* bot, std::vector<std::pair<uint32, uint32>> const& lacking, std::set<int> const& visited, uint32& sells, bool& noGold)
    {
        noGold = false;
        FactionTemplateEntry const* own = bot->GetFactionTemplateEntry();
        if (!own)
            return -1;
        const WorldPosition from(bot);
        const bool alliance = bot->GetTeam() == ALLIANCE;
        int best = -1;
        float bestDistance = VENDOR_RANGE;
        for (auto const& [entry, count] : lacking)
        {
            auto it = vendorsOf.find(entry);
            ItemPrototype const* proto = sObjectMgr.GetItemPrototype(entry);
            if (it == vendorsOf.end() || !proto)
                continue;
            for (size_t i : it->second)
            {
                Vendor const& v = vendors[i];
                if (visited.count(int(i)) || v.pos.getMapId() != from.getMapId() || v.pos.distance(from) > bestDistance)
                    continue;
                FactionTemplateEntry const* theirs = sFactionTemplateStore.LookupEntry(v.faction);
                if (!theirs || !theirs->IsFriendlyTo(*own) || theirs->IsHostileTo(*own))
                    continue;
                if ((alliance ? v.hostileToAlliance : v.hostileToHorde) >= VENDOR_CAMP_HOSTILES)
                    continue;
                if (proto->BuyPrice > bot->GetMoney())
                {
                    noGold = true;
                    continue;
                }
                best = int(i);
                bestDistance = v.pos.distance(from);
                sells = entry;
            }
        }
        if (best >= 0)
            noGold = false;
        return best;
    }

    // The live vendor of the entry nearest the character, within a little more than its spawn's reach.
    Creature* FindVendor(Player* bot, uint32 entry)
    {
        Creature* found = nullptr;
        MaNGOS::NearestCreatureEntryWithLiveStateInObjectRangeCheck check(*bot, entry, true, false, 30.0f);
        MaNGOS::CreatureLastSearcher<MaNGOS::NearestCreatureEntryWithLiveStateInObjectRangeCheck> searcher(found, check);
        Cell::VisitGridObjects(bot, searcher, 30.0f);
        return found;
    }

    // Buys up to want of the item from the vendor, which it can trade with, as its gold and bags allow, a stack at a
    // time; how many it got. bagsFull and noGold say what stopped it, if that did; a vendor sold out, or an item
    // that needs a reputation or a condition it lacks, stops it too.
    uint32 BuyFrom(Player* bot, Creature* vendor, uint32 entry, uint32 want, uint32& spent, bool& bagsFull, bool& noGold)
    {
        ItemPrototype const* proto = sObjectMgr.GetItemPrototype(entry);
        if (!proto)
            return 0;
        const uint32 perBuy = std::max<uint32>(1, proto->BuyCount);
        const uint32 buysPerStack = std::min<uint32>(255, std::max<uint32>(1, proto->GetMaxStackSize() / perBuy));
        const float discount = bot->GetReputationPriceDiscount(vendor);
        uint32 got = 0;
        while (got < want)
        {
            uint32 buys = std::min<uint32>((want - got + perBuy - 1) / perBuy, buysPerStack);
            ItemPosCountVec dest;
            InventoryResult room = bot->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, entry, buys * perBuy);
            while (room != EQUIP_ERR_OK && buys > 1)
            {
                buys /= 2;
                dest.clear();
                room = bot->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, entry, buys * perBuy);
            }
            if (room != EQUIP_ERR_OK)
            {
                bagsFull = bagsFull || room != EQUIP_ERR_CANT_CARRY_MORE_OF_THIS;
                break;
            }
            const uint32 each = uint32(std::floor(proto->BuyPrice * discount));
            if (each && bot->GetMoney() < uint64(each) * buys)
                buys = std::min<uint32>(buys, bot->GetMoney() / each);
            if (!buys)
            {
                noGold = true;
                break;
            }
            const uint32 before = CountInBags(bot, entry);
            const uint32 money = bot->GetMoney();
            bot->BuyItemFromVendor(vendor->GetObjectGuid(), entry, uint8(buys), NULL_BAG, NULL_SLOT);
            const uint32 after = CountInBags(bot, entry);
            if (after <= before)
                break;
            got += after - before;
            spent += money > bot->GetMoney() ? money - bot->GetMoney() : 0;
        }
        return got;
    }

    // Why the character cannot act now, or empty: pay and craft wait for it to be alive, settled and out of a fight.
    std::string NotReady(Player* bot)
    {
        if (sServerFacade.UnitIsDead(bot))
            return "waiting: dead";
        if (sServerFacade.IsInCombat(bot))
            return "waiting: in combat";
        if (bot->IsBeingTeleported())
            return "waiting: between places";
        if (bot->IsTaxiFlying())
            return "waiting: on a flight";
        return "";
    }

    // The item a recipe makes, or 0.
    uint32 CreatedItem(SpellEntry const* spell)
    {
        for (uint8 i = 0; i < MAX_EFFECT_INDEX; ++i)
            if (spell->Effect[i] == SPELL_EFFECT_CREATE_ITEM && spell->EffectItemType[i])
                return spell->EffectItemType[i];
        return 0;
    }

    // The most items one cast of the recipe makes.
    uint32 MostPerCast(SpellEntry const* spell)
    {
        for (uint8 i = 0; i < MAX_EFFECT_INDEX; ++i)
            if (spell->Effect[i] == SPELL_EFFECT_CREATE_ITEM && spell->EffectItemType[i])
                return uint32(std::max(1, spell->EffectBasePoints[i] + std::max(1, spell->EffectDieSides[i])));
        return 1;
    }

    // A recipe the character knows that makes the item, or 0.
    uint32 KnownRecipeFor(Player* bot, uint32 item)
    {
        for (auto const& [spellId, known] : bot->GetSpellMap())
        {
            if (known.state == PLAYERSPELL_REMOVED || known.disabled)
                continue;
            SpellEntry const* spell = sServerFacade.LookupSpellInfo(spellId);
            if (spell && CreatedItem(spell) == item)
                return spellId;
        }
        return 0;
    }

    std::string FocusName(uint32 focus)
    {
        switch (focus)
        {
            case 1: return "an anvil";
            case 2: return "a loom";
            case 3: return "a forge";
            case 4: return "a cooking fire";
            case 543: return "the Black Forge";
            default: return "spell focus " + std::to_string(focus);
        }
    }

    // The spell focus object the recipe needs within its reach, as Spell::CheckCast looks for it.
    bool NearFocus(Player* bot, uint32 focus)
    {
        GameObject* found = nullptr;
        MaNGOS::GameObjectFocusCheck check(bot, focus);
        MaNGOS::GameObjectSearcher<MaNGOS::GameObjectFocusCheck> searcher(found, check);
        Cell::VisitGridObjects(bot, searcher, bot->GetMap()->GetVisibilityDistance());
        return found != nullptr;
    }
}

bool GuildJobs::IsJob(const std::string& job)
{
    return job == "pay" || job == "reset" || job == "craft" || job == "kit" || job == "deposit" || job == "withdraw";
}

bool GuildJobs::AlwaysKeep(uint32 itemId)
{
    static const std::set<uint32> items = {
        200001, // Sands of Renewal, the raid reset currency, from the level 60 dungeons' bosses
        4382,   // Bronze Framework
        4389,   // Gyrochronatom
        10560,  // Unstable Trigger
        10561,  // Mithril Casing
        12360,  // Arcanite Bar
        12655,  // Enchanted Thorium Bar
        14342,  // Mooncloth
        15994,  // Thorium Widget
        18631,  // Truesilver Transformer
    };
    return items.count(itemId) != 0;
}

GuildJobs::Update GuildJobs::Tick(Player* bot, const std::string& job, const std::string& args, uint32 jobSeq)
{
    Progress& p = progress[bot->GetGUIDLow()];
    if (p.seq != jobSeq)
    {
        // A new order: whatever place the last one went to is no longer the way.
        p = Progress();
        p.seq = jobSeq;
        EndTrip(bot->GetGUIDLow());
    }

    if (job == "pay" || job == "kit" || job == "deposit" || job == "withdraw")
    {
        Update u = job == "pay" ? Pay(bot, args, p) : job == "kit" ? Kit(bot, args, p)
            : job == "deposit" ? Deposit(bot, args, p) : Withdraw(bot, args, p);
        if (u.status != "working")
            EndTrip(bot->GetGUIDLow());
        return u;
    }
    EndTrip(bot->GetGUIDLow());
    if (job == "reset")
        return Reset(bot, args);
    return Craft(bot, args, p);
}

bool GuildJobs::Travels(const std::string& job, const std::string& args)
{
    if (job == "kit" || job == "deposit" || job == "withdraw")
        return true;
    if (job != "pay")
        return false;
    const std::string at = Arg(args, "at");
    return at == "guild_master" || at == "emissary";
}

bool GuildJobs::KeepsForJob(const std::string& job, const std::string& args, uint32 itemId)
{
    return (job == "kit" || job == "deposit" || job == "withdraw") && ListsItem(Arg(args, "items"), itemId);
}

bool GuildJobs::ShouldTravel(Player* bot)
{
    if (!bot || !bot->IsInWorld() || bot->IsBeingTeleported())
        return false;
    const std::string job = sGuildBotMgr.GetActiveJob(bot->GetGUIDLow());
    if (job != "pay" && job != "kit" && job != "deposit" && job != "withdraw")
        return false;
    Trip trip;
    return CurrentTrip(bot->GetGUIDLow(), trip) && !InReach(bot, trip.pos);
}

TravelDestination* GuildJobs::Destination(Player* bot)
{
    Trip trip;
    return bot && CurrentTrip(bot->GetGUIDLow(), trip) ? trip.place : nullptr;
}

GuildJobs::Update GuildJobs::Pay(Player* bot, const std::string& args, Progress& p)
{
    Update u;
    auto fail = [&u](const std::string& reason) { u.status = "failed"; u.reason = reason; return u; };

    const std::string usage = "pay needs ref=<number>, and gold=<copper> or items=<entry>:<count>,...";
    const std::string refArg = Arg(args, "ref");
    uint64 ref = 0, gold = 0;
    if (!ParseNumber(refArg, ref))
        return fail(usage);
    const std::string goldArg = Arg(args, "gold");
    if (!goldArg.empty() && (!ParseNumber(goldArg, gold) || gold > MAX_MONEY_AMOUNT))
        return fail("pay: gold must be copper, up to " + std::to_string(MAX_MONEY_AMOUNT));

    // The items in the order given, each entry once.
    std::vector<std::pair<uint32, uint32>> items;
    const std::string itemsArg = Arg(args, "items");
    if (!itemsArg.empty())
    {
        const std::string error = ParseItems(itemsArg, "pay", false, items);
        if (!error.empty())
            return fail(error);
    }
    if (!gold && items.empty())
        return fail(usage);
    const std::string at = Arg(args, "at");
    if (!at.empty() && at != "guild_master" && at != "emissary")
        return fail("pay: at must be guild_master or emissary");

    std::ostringstream paid;
    paid << "ref=" << ref;
    if (gold)
        paid << " gold=" << gold;
    if (!items.empty())
    {
        paid << " items=";
        for (size_t i = 0; i < items.size(); ++i)
            paid << (i ? "," : "") << items[i].first << ":" << items[i].second;
    }

    // A ref is paid once per character. After a restart that lost the done status, the paid event,
    // written after the character's save, says it was paid already.
    if (!p.started)
    {
        p.started = true;
        auto result = CharacterDatabase.PQuery(
            "SELECT 1 FROM guildbot_event WHERE guid = %u AND kind = 'paid' AND (detail = 'ref=%s' OR detail LIKE 'ref=%s %%') LIMIT 1",
            bot->GetGUIDLow(), refArg.c_str(), refArg.c_str());
        if (result)
        {
            u.status = "done";
            u.reason = "already paid ref=" + refArg;
            return u;
        }
#if GUILD_JOBS_VAULT
        if (!at.empty())
            p.guild = sGuildVault.GuildOf(bot->GetSession()->GetAccountId());
#endif
    }

    // At a counter: the nearest of its faction, chosen once per order, which travel takes it to.
    WorldPosition counter;
    std::string where;
    if (!at.empty())
    {
        Trip trip;
        if (!CurrentTrip(bot->GetGUIDLow(), trip))
        {
            if (!CounterTrip(bot, at == "emissary" ? CounterKind::Emissary : CounterKind::GuildMaster, trip))
                return fail(at == "emissary" ? "no emissary known for its faction" : "no Guild Master known for its faction");
            StartTrip(bot->GetGUIDLow(), trip.pos, trip.place, trip.where);
        }
        counter = trip.pos;
        where = trip.where;
    }

    u.reason = NotReady(bot);
    if (!u.reason.empty())
    {
        // Waiting is no lack of progress towards the counter: the clock starts again once it can move.
        p.nearestAt = 0;
        return u;
    }

    // All or nothing: every check first, then everything taken at once.
    // The checks come before the way to a counter too, so it does not set out for a payment it cannot make.
    // Short of gold at a counter, it takes the rest from the guild's vault: a vaultkeeper stands beside every counter.
    const uint32 shortfall = bot->GetMoney() < gold ? uint32(gold) - bot->GetMoney() : 0;
    if (shortfall)
    {
        uint64 inVault = 0;
#if GUILD_JOBS_VAULT
        if (!at.empty() && !p.guild.empty())
            inVault = sGuildVault.Gold(p.guild);
#endif
        if (inVault < shortfall)
            return fail("has " + Money(bot->GetMoney()) + ", needs " + Money(uint32(gold))
                + (at.empty() || p.guild.empty() ? "" : "; the vault holds " + Money(uint32(inVault))));
    }
    for (auto const& [entry, count] : items)
    {
        uint32 have = CountInBags(bot, entry);
        if (have < count)
            return fail("has " + std::to_string(have) + " of " + std::to_string(count) + " " + ItemName(entry));
    }

    if (!at.empty() && !InReach(bot, counter))
    {
        std::string why;
        if (Stalled(bot, counter, COUNTER_STALL, p.nearest, p.nearestMap, p.nearestAt, why))
            return fail("could not reach " + where + ": " + why);
        u.reason = "on its way to " + where;
        return u;
    }

#if GUILD_JOBS_VAULT
    if (shortfall)
    {
        // The vault's gold is saved with the character's in the vault's own transaction, before the payment below.
        std::string why;
        p.fromVault += sGuildVault.WithdrawGold(bot, shortfall, why);
        if (bot->GetMoney() < gold)
            return fail("has " + Money(bot->GetMoney()) + ", needs " + Money(uint32(gold)) + (why.empty() ? "" : "; " + why));
    }
#endif

    if (gold)
        bot->ModifyMoney(-int32(gold));
    for (auto const& [entry, count] : items)
        bot->DestroyItemCount(entry, count, true);
    // Saved now, so the payment is in the database before its event: the event is queued after the save.
    bot->SaveToDB();

    std::string what = gold ? Money(uint32(gold)) : "";
    for (auto const& [entry, count] : items)
        what += (what.empty() ? "" : ", ") + std::to_string(count) + " " + ItemName(entry);
    u.status = "done";
    u.reason = "paid " + what + " (ref " + refArg + ")" + (where.empty() ? "" : " to " + where)
        + (p.fromVault ? ", " + Money(p.fromVault) + " of it from the vault" : "");
    u.events.push_back({ "paid", paid.str() });
    return u;
}

GuildJobs::Update GuildJobs::Reset(Player* bot, const std::string& args)
{
    Update u;
    auto fail = [&u](const std::string& reason) { u.status = "failed"; u.reason = reason; return u; };

    uint64 mapArg = 0;
    if (!ParseNumber(Arg(args, "map"), mapArg))
        return fail("reset needs map=<raid map id>");
    const uint32 mapId = uint32(mapArg);
    MapEntry const* mapEntry = sMapStore.LookupEntry(mapId);
    if (!mapEntry || !mapEntry->IsRaid())
        return fail("map " + std::to_string(mapId) + " is not a raid");
    const std::string mapName = mapEntry->name[0];

    // Every character on the account: the guild. Online ones are unbound through the core, offline ones in the database.
    struct Member { uint32 guid; std::string name; Player* online; };
    std::vector<Member> members;
    std::set<uint32> guids;
    std::vector<uint32> offline;
    auto result = CharacterDatabase.PQuery("SELECT guid, name, map FROM characters WHERE account = %u", bot->GetSession()->GetAccountId());
    if (result)
    {
        do
        {
            Field* f = result->Fetch();
            Member m{ f[0].GetUInt32(), f[1].GetCppString(), sObjectMgr.GetPlayer(ObjectGuid(HIGHGUID_PLAYER, f[0].GetUInt32()), false) };
            if (m.online)
            {
                // Between maps it has no map to ask; wait until it arrives.
                if (!m.online->IsInWorld() || m.online->IsBeingTeleported())
                {
                    u.reason = "waiting for " + m.name + " to arrive";
                    return u;
                }
                if (m.online->GetMapId() == mapId)
                    return fail(m.name + " is inside " + mapName);
            }
            else
            {
                if (f[2].GetUInt32() == mapId)
                    return fail(m.name + " is inside " + mapName + ", offline");
                offline.push_back(m.guid);
            }
            guids.insert(m.guid);
            members.push_back(m);
        } while (result->NextRow());
    }

    // The groups any of them is in that are bound to the raid: a group's bind sends its members back to the old
    // instance, so it goes too, but not while anyone of the group is inside.
    std::vector<Group*> groups;
    for (auto it = sObjectMgr.GetGroupMapBegin(); it != sObjectMgr.GetGroupMapEnd(); ++it)
    {
        Group* group = it->second;
        if (!group || !group->GetBoundInstance(mapId))
            continue;
        bool ours = false;
        for (auto const& slot : group->GetMemberSlots())
            ours = ours || guids.count(slot.guid.GetCounter());
        if (!ours)
            continue;
        for (auto const& slot : group->GetMemberSlots())
        {
            Player* member = sObjectMgr.GetPlayer(slot.guid, false);
            if (member && (!member->IsInWorld() || member->IsBeingTeleported()))
            {
                u.reason = "waiting for " + slot.name + " to arrive";
                return u;
            }
            if (member && member->GetMapId() == mapId)
                return fail(slot.name + ", in " + std::string(group->GetLeaderName()) + "'s group, is inside " + mapName);
        }
        groups.push_back(group);
    }

    uint32 characters = 0;
    for (Member const& m : members)
    {
        if (m.online && m.online->GetBoundInstance(mapId))
        {
            m.online->UnbindInstance(mapId);
            ++characters;
        }
    }

    if (!offline.empty())
    {
        std::string list;
        for (uint32 guid : offline)
            list += (list.empty() ? "" : ",") + std::to_string(guid);
        auto bound = CharacterDatabase.PQuery(
            "SELECT DISTINCT ci.guid FROM character_instance ci JOIN instance i ON i.id = ci.instance WHERE i.map = %u AND ci.guid IN (%s)",
            mapId, list.c_str());
        if (bound)
        {
            do
            {
                ++characters;
            } while (bound->NextRow());
            CharacterDatabase.PExecute(
                "DELETE ci FROM character_instance ci JOIN instance i ON i.id = ci.instance WHERE i.map = %u AND ci.guid IN (%s)",
                mapId, list.c_str());
        }
    }

    for (Group* group : groups)
        group->UnbindInstance(mapId);

    u.status = "done";
    u.reason = "reset " + mapName + ": " + std::to_string(characters) + " characters and " + std::to_string(groups.size()) + " groups unbound";
    u.events.push_back({ "raid_reset", "map=" + std::to_string(mapId) + " " + mapName + " characters=" + std::to_string(characters) +
        " groups=" + std::to_string(groups.size()) });
    return u;
}

GuildJobs::Update GuildJobs::Craft(Player* bot, const std::string& args, Progress& p)
{
    Update u;
    auto fail = [&u](const std::string& reason) { u.status = "failed"; u.reason = reason; return u; };

    // The recipe: spell= names it; item= finds one the character knows that makes the item.
    uint64 spellArg = 0, itemArg = 0, amount = 1;
    const std::string spellText = Arg(args, "spell"), itemText = Arg(args, "item"), amountText = Arg(args, "amount");
    if ((spellText.empty() && itemText.empty()) || (!spellText.empty() && !ParseNumber(spellText, spellArg)) ||
        (!itemText.empty() && !ParseNumber(itemText, itemArg)))
        return fail("craft needs item=<item made> or spell=<recipe>, and amount=<n>");
    if (!amountText.empty() && (!ParseNumber(amountText, amount) || !amount || amount > CRAFT_MAX_AMOUNT))
        return fail("craft: amount must be 1 to " + std::to_string(CRAFT_MAX_AMOUNT));

    uint32 spellId = uint32(spellArg);
    if (!spellId)
    {
        if (!sObjectMgr.GetItemPrototype(uint32(itemArg)))
            return fail("craft: no item " + std::to_string(itemArg));
        spellId = KnownRecipeFor(bot, uint32(itemArg));
        if (!spellId)
            return fail("knows no recipe for " + ItemName(uint32(itemArg)));
    }
    SpellEntry const* spell = sServerFacade.LookupSpellInfo(spellId);
    if (!spell)
        return fail("craft: no spell " + std::to_string(spellId));
    const std::string spellName = spell->SpellName[0];
    if (!bot->HasSpell(spellId))
        return fail("does not know " + spellName);
    const uint32 item = CreatedItem(spell);
    if (!item || !sObjectMgr.GetItemPrototype(item))
        return fail(spellName + " makes no item");

    const uint32 guid = bot->GetGUIDLow();
    const uint32 have = CountInBags(bot, item);
    if (!p.started)
    {
        // What it made on this order before a restart, and what it holds now, so only new items count.
        p.started = true;
        p.made = LoadMade(guid, p.seq);
        p.lastCount = have;
    }
    if (have > p.lastCount)
    {
        p.made += have - p.lastCount;
        SaveMade(guid, p.seq, p.made);
    }
    p.lastCount = have;

    const std::string made = "made " + std::to_string(p.made) + " of " + std::to_string(amount) + " " + ItemName(item);
    u.reason = made;
    if (p.made >= amount)
    {
        u.status = "done";
        u.events.push_back({ "crafted", "item=" + std::to_string(item) + " spell=" + std::to_string(spellId) + " made=" + std::to_string(p.made) });
        return u;
    }

    if (bot->IsNonMeleeSpellCasted(false))
        return u;

    const std::string waiting = NotReady(bot);
    if (!waiting.empty())
    {
        u.reason = made + "; " + waiting;
        return u;
    }

    if (!bot->IsSpellReady(*spell))
        return fail(made + "; " + spellName + " is on cooldown");
    for (uint32 tool : spell->Totem)
        if (tool && !bot->HasItemCount(tool, 1))
            return fail(made + "; needs a " + ItemName(tool));
    if (spell->RequiresSpellFocus && !NearFocus(bot, spell->RequiresSpellFocus))
        return fail(made + "; needs " + FocusName(spell->RequiresSpellFocus) + " nearby");
    for (uint8 i = 0; i < MAX_SPELL_REAGENTS; ++i)
    {
        if (spell->Reagent[i] <= 0 || !spell->ReagentCount[i])
            continue;
        const uint32 reagent = uint32(spell->Reagent[i]);
        if (!bot->HasItemCount(reagent, spell->ReagentCount[i]))
            return fail(made + "; out of " + ItemName(reagent) + ", has " + std::to_string(bot->GetItemCount(reagent)) +
                ", needs " + std::to_string(spell->ReagentCount[i]));
    }
    ItemPosCountVec dest;
    if (bot->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, item, MostPerCast(spell)) != EQUIP_ERR_OK)
        return fail(made + "; bags full");

    PlayerbotAI* ai = bot->GetPlayerbotAI();
    if (ai && ai->CastSpell(spellId, bot, nullptr, true))
    {
        p.castFailures = 0;
        return u;
    }
    if (++p.castFailures >= CRAFT_CAST_TRIES)
        return fail(made + "; could not cast " + spellName);
    return u;
}

GuildJobs::Update GuildJobs::Kit(Player* bot, const std::string& args, Progress& p)
{
    Update u;
    auto fail = [&u](const std::string& reason) { u.status = "failed"; u.reason = reason; return u; };

    std::vector<std::pair<uint32, uint32>> items;
    const std::string itemsArg = Arg(args, "items");
    if (itemsArg.empty())
        return fail("kit needs items=<entry>:<count>,...");
    const std::string error = ParseItems(itemsArg, "kit", false, items);
    if (!error.empty())
        return fail(error);
    const std::string loadout = Arg(args, "loadout");
    const uint32 guid = bot->GetGUIDLow();

    u.reason = NotReady(bot);
    if (!u.reason.empty())
    {
        // Waiting is no lack of progress towards a place: the clock starts again once it can move.
        p.nearestAt = 0;
        return u;
    }

    std::vector<std::pair<uint32, uint32>> lacking = Lacking(bot, items);

    // Done once it holds the counts or has got all it could, with what is short: the order queued after it still goes.
    // Failed only when its bags took nothing at all.
    auto finish = [&]()
    {
        lacking = Lacking(bot, items);
        std::string reason = loadout.empty() ? "kitted" : "kitted " + loadout;
        if (!lacking.empty())
        {
            reason += "; short " + Names(lacking);
            if (p.bagsFull)
                reason += "; bags full";
            if (p.noGold)
                reason += "; out of gold";
            if (!p.note.empty())
                reason += "; " + p.note;
            if (p.bagsFull && p.took.empty() && p.bought.empty())
                return fail("bags full; short " + Names(lacking));
        }
        std::string detail = loadout.empty() ? "" : "loadout=" + loadout;
        auto add = [&detail](const std::string& key, const std::string& value)
        {
            if (!value.empty())
                detail += (detail.empty() ? "" : " ") + key + "=" + value;
        };
        add("vault", Entries(p.took));
        add("bought", Entries(p.bought));
        add("spent", p.spent ? std::to_string(p.spent) : "");
        add("short", Entries(lacking));
        u.status = "done";
        u.reason = reason;
        u.events.push_back({ "kitted", detail });
        return u;
    };

    if (lacking.empty())
        return finish();

    if (p.stage == KIT_PLAN)
    {
        if (!RoomForAny(bot, lacking))
        {
            p.bagsFull = true;
            return finish();
        }
        p.stage = KIT_VENDORS;
#if GUILD_JOBS_VAULT
        // To the vault only if it has some of what the character lacks.
        p.guild = sGuildVault.GuildOf(bot->GetSession()->GetAccountId());
        bool inVault = false;
        for (auto const& [entry, count] : lacking)
            inVault = inVault || (!p.guild.empty() && sGuildVault.Count(p.guild, entry) > 0);
        Trip trip;
        if (inVault && CounterTrip(bot, CounterKind::Vaultkeeper, trip))
        {
            StartTrip(guid, trip.pos, trip.place, trip.where);
            p.stage = KIT_VAULT;
            p.nearestAt = 0;
        }
#endif
    }

#if GUILD_JOBS_VAULT
    if (p.stage == KIT_VAULT)
    {
        Trip trip;
        if (!CurrentTrip(guid, trip))
            p.stage = KIT_VENDORS;
        else if (!InReach(bot, trip.pos))
        {
            std::string why;
            if (!Stalled(bot, trip.pos, COUNTER_STALL, p.nearest, p.nearestMap, p.nearestAt, why))
            {
                u.reason = "on its way to " + trip.where;
                return u;
            }
            // Short of the vault, it still buys what it can.
            p.note = "could not reach " + trip.where + ": " + why;
            EndTrip(guid);
            p.stage = KIT_VENDORS;
        }
        else
        {
            // As much of each as the vault holds and the bags take; another character of the guild may have taken
            // some on the way.
            for (auto const& [entry, count] : lacking)
            {
                std::string reason;
                InventoryResult bagError = EQUIP_ERR_OK;
                if (const uint32 taken = sGuildVault.Withdraw(bot, entry, count, reason, &bagError))
                    p.took[entry] += taken;
                if (bagError != EQUIP_ERR_OK && bagError != EQUIP_ERR_CANT_CARRY_MORE_OF_THIS)
                    p.bagsFull = true;
            }
            EndTrip(guid);
            p.stage = KIT_VENDORS;
            lacking = Lacking(bot, items);
            if (lacking.empty())
                return finish();
        }
    }
#endif

    // At the vendor it went to: step up to it, and buy what it sells of what the character lacks.
    if (p.vendor >= 0)
    {
        Vendor const& v = vendors[p.vendor];
        if (!InReach(bot, v.pos))
        {
            std::string why;
            if (!Stalled(bot, v.pos, VENDOR_STALL, p.nearest, p.nearestMap, p.nearestAt, why))
            {
                u.reason = "on its way to buy from " + v.name;
                return u;
            }
        }
        else if (Creature* vendor = FindVendor(bot, v.entry))
        {
            if (!bot->GetNPCIfCanInteractWith(vendor->GetObjectGuid(), UNIT_NPC_FLAG_VENDOR))
            {
                if (++p.approaches <= VENDOR_APPROACHES)
                {
                    // Travel stops within 5 yards of the vendor's spawn; trading needs a little nearer, to where it
                    // stands now. Never a move for a character on its way to another map.
                    if (bot->IsInWorld() && !bot->IsBeingTeleported())
                    {
                        const float angle = vendor->GetAngle(bot);
                        bot->GetMotionMaster()->MovePoint(0, vendor->GetPositionX() + 2.0f * std::cos(angle),
                            vendor->GetPositionY() + 2.0f * std::sin(angle), vendor->GetPositionZ(), FORCED_MOVEMENT_RUN);
                    }
                    u.reason = "stepping up to " + v.name;
                    return u;
                }
            }
            else
            {
                for (auto const& [entry, count] : lacking)
                {
                    if (!Sells(size_t(p.vendor), entry))
                        continue;
                    if (const uint32 got = BuyFrom(bot, vendor, entry, count, p.spent, p.bagsFull, p.noGold))
                        p.bought[entry] += got;
                    if (p.bagsFull)
                        break;
                }
            }
        }
        // Bought from, or given up on: on to the next.
        p.visited.insert(p.vendor);
        p.vendor = -1;
        EndTrip(guid);
        lacking = Lacking(bot, items);
        if (lacking.empty())
            return finish();
    }

    // The nearest vendor that sells something it still lacks, which travel takes it to.
    if (!p.bagsFull && p.visited.size() < KIT_MAX_VENDORS)
    {
        if (!RoomForAny(bot, lacking))
            p.bagsFull = true;
        else
        {
            IndexVendors();
            uint32 sells = 0;
            bool noGold = false;
            const int next = NearestVendor(bot, lacking, p.visited, sells, noGold);
            if (next >= 0)
            {
                Vendor& v = vendors[next];
                if (!v.place)
                    v.place = std::make_unique<PlaceTravelDestination>(v.pos, v.name);
                StartTrip(guid, v.pos, v.place.get(), v.name);
                p.vendor = next;
                p.approaches = 0;
                p.nearestAt = 0;
                u.reason = "on its way to buy " + ItemName(sells) + " from " + v.name;
                return u;
            }
            p.noGold = p.noGold || noGold;
        }
    }
    return finish();
}

GuildJobs::Update GuildJobs::Deposit(Player* bot, const std::string& args, Progress& p)
{
    Update u;
    auto fail = [&u](const std::string& reason) { u.status = "failed"; u.reason = reason; return u; };

#if !GUILD_JOBS_VAULT
    (void)bot;
    (void)args;
    (void)p;
    return fail("this server has no guild vault");
#else
    std::vector<std::pair<uint32, uint32>> items;
    const std::string itemsArg = Arg(args, "items");
    const std::string goldArg = Arg(args, "gold");
    const std::string keepArg = Arg(args, "keep");
    if (itemsArg.empty() && goldArg.empty())
        return fail("deposit needs items=<entry>:<count>,... (a count of 0 for all it carries) or gold=<copper> (0 for all)");
    if (!itemsArg.empty())
    {
        const std::string error = ParseItems(itemsArg, "deposit", true, items);
        if (!error.empty())
            return fail(error);
    }
    // gold=0 is all it carries; keep is what it keeps back of that.
    const bool withGold = !goldArg.empty();
    uint64 gold = 0, keep = 0;
    if (withGold && (!ParseNumber(goldArg, gold) || gold > MAX_MONEY_AMOUNT))
        return fail("deposit: gold must be copper, up to " + std::to_string(MAX_MONEY_AMOUNT));
    if (!keepArg.empty() && (!withGold || !ParseNumber(keepArg, keep) || keep > MAX_MONEY_AMOUNT))
        return fail("deposit: keep=<copper> goes with gold=, up to " + std::to_string(MAX_MONEY_AMOUNT));
    const uint32 guid = bot->GetGUIDLow();

    // The gold to put in now: what it carries above keep, up to gold.
    auto spareGold = [&]() -> uint32
    {
        if (!withGold)
            return 0;
        const uint32 money = bot->GetMoney();
        const uint32 spare = money > keep ? money - uint32(keep) : 0;
        return gold ? std::min(uint32(gold), spare) : spare;
    };

    // An order is put in once. After a restart that lost the done status, the deposited event of this order,
    // written after the vault's change in the same queue, says it was put in already. The order's events come
    // after the last job event, which a new order writes.
    if (!p.started)
    {
        p.started = true;
        auto result = CharacterDatabase.PQuery(
            "SELECT 1 FROM guildbot_event WHERE guid = %u AND kind = 'deposited' AND (detail = 'seq=%u' OR detail LIKE 'seq=%u %%') "
            "AND id > (SELECT COALESCE(MAX(id), 0) FROM guildbot_event WHERE guid = %u AND kind = 'job') LIMIT 1",
            guid, p.seq, p.seq, guid);
        if (result)
        {
            u.status = "done";
            u.reason = "already deposited this order";
            return u;
        }
        p.guild = sGuildVault.GuildOf(bot->GetSession()->GetAccountId());
    }

    u.reason = NotReady(bot);
    if (!u.reason.empty())
    {
        p.nearestAt = 0;
        return u;
    }
    if (p.guild.empty())
        return fail("has no guild vault");

    // Each stack of each item, up to its count: put in, or with forReal false only asked whether the vault would take it.
    // The reason lists what went in, what the vault refused and why, and what it carried less of than the order said.
    std::vector<std::pair<uint32, uint32>> deposited;
    uint32 goldIn = 0;
    auto putIn = [&](bool forReal)
    {
        deposited.clear();
        goldIn = 0;
        std::vector<std::tuple<uint32, uint32, std::string>> refused;
        std::vector<std::string> missing;
        for (auto const& [entry, count] : items)
        {
            uint32 put = 0, carried = 0;
            for (Item* item : StacksInBags(bot, entry))
            {
                const uint32 stack = item->GetCount();
                carried += stack;
                if (count && put >= count)
                    continue;
                const uint32 n = count ? std::min(stack, count - put) : stack;
                std::string why;
                // The item may be gone after a deposit: only its count, read before, is used.
                if (forReal ? sGuildVault.Deposit(bot, item, n == stack ? 0 : n, why) : GuildVault::IsFungible(item, why))
                    put += n;
                else
                {
                    if (!why.empty() && why.back() == '.')
                        why.pop_back();
                    auto it = std::find_if(refused.begin(), refused.end(), [&](auto const& r) { return std::get<0>(r) == entry && std::get<2>(r) == why; });
                    if (it == refused.end())
                        refused.emplace_back(entry, n, why);
                    else
                        std::get<1>(*it) += n;
                }
            }
            if (put)
                deposited.push_back({ entry, put });
            if (!carried)
                missing.push_back("none of " + ItemName(entry));
            else if (count && carried < count)
                missing.push_back(std::to_string(carried) + " of " + std::to_string(count) + " " + ItemName(entry));
        }
        std::string goldRefused;
        if (const uint32 spare = spareGold())
        {
            if (!forReal || sGuildVault.DepositGold(bot, spare, goldRefused))
                goldIn = spare;
            else if (!goldRefused.empty() && goldRefused.back() == '.')
                goldRefused.pop_back();
        }
        std::string reason;
        if (deposited.empty() && !goldIn)
            reason = "deposited nothing";
        else
            reason = "deposited " + (goldIn ? Money(goldIn) + (deposited.empty() ? "" : " and ") : "") + (deposited.empty() ? "" : Names(deposited));
        if (!goldRefused.empty())
            reason += "; refused the gold (" + goldRefused + ")";
        else if (withGold && gold && goldIn < gold)
            reason += "; had " + Money(goldIn) + " of " + Money(uint32(gold)) + " to spare";
        for (auto const& [entry, n, why] : refused)
            reason += "; refused " + std::to_string(n) + " " + ItemName(entry) + " (" + why + ")";
        if (!missing.empty())
        {
            reason += "; carries ";
            for (size_t i = 0; i < missing.size(); ++i)
                reason += (i ? ", " : "") + missing[i];
        }
        return reason;
    };

    // Before it sets out: to the vault only with something the vault would take.
    Trip trip;
    if (!CurrentTrip(guid, trip))
    {
        const std::string plan = putIn(false);
        if (deposited.empty() && !goldIn)
        {
            u.status = "done";
            u.reason = plan;
            return u;
        }
        if (!CounterTrip(bot, CounterKind::Vaultkeeper, trip))
            return fail("no vaultkeeper known for its faction");
        StartTrip(guid, trip.pos, trip.place, trip.where);
        p.nearestAt = 0;
    }

    if (!InReach(bot, trip.pos))
    {
        std::string why;
        if (Stalled(bot, trip.pos, COUNTER_STALL, p.nearest, p.nearestMap, p.nearestAt, why))
            return fail("could not reach " + trip.where + ": " + why);
        u.reason = "on its way to " + trip.where;
        return u;
    }

    u.status = "done";
    u.reason = putIn(true);
    if (!deposited.empty() || goldIn)
    {
        std::string detail = "seq=" + std::to_string(p.seq);
        if (!deposited.empty())
            detail += " items=" + Entries(deposited);
        if (goldIn)
            detail += " gold=" + std::to_string(goldIn);
        u.events.push_back({ "deposited", detail });
    }
    return u;
#endif
}

GuildJobs::Update GuildJobs::Withdraw(Player* bot, const std::string& args, Progress& p)
{
    Update u;
    auto fail = [&u](const std::string& reason) { u.status = "failed"; u.reason = reason; return u; };

#if !GUILD_JOBS_VAULT
    (void)bot;
    (void)args;
    (void)p;
    return fail("this server has no guild vault");
#else
    std::vector<std::pair<uint32, uint32>> items;
    const std::string itemsArg = Arg(args, "items");
    const std::string goldArg = Arg(args, "gold");
    if (itemsArg.empty() && goldArg.empty())
        return fail("withdraw needs items=<entry>:<count>,... (a count of 0 for all the vault holds) or gold=<copper>");
    if (!itemsArg.empty())
    {
        const std::string error = ParseItems(itemsArg, "withdraw", true, items);
        if (!error.empty())
            return fail(error);
    }
    uint64 gold = 0;
    if (!goldArg.empty() && (!ParseNumber(goldArg, gold) || !gold || gold > MAX_MONEY_AMOUNT))
        return fail("withdraw: gold must be copper, from 1 up to " + std::to_string(MAX_MONEY_AMOUNT));
    const uint32 guid = bot->GetGUIDLow();

    // An order is taken out once, as a deposit is put in once: after a restart that lost the done status,
    // the withdrew event of this order says it was taken already.
    if (!p.started)
    {
        p.started = true;
        auto result = CharacterDatabase.PQuery(
            "SELECT 1 FROM guildbot_event WHERE guid = %u AND kind = 'withdrew' AND (detail = 'seq=%u' OR detail LIKE 'seq=%u %%') "
            "AND id > (SELECT COALESCE(MAX(id), 0) FROM guildbot_event WHERE guid = %u AND kind = 'job') LIMIT 1",
            guid, p.seq, p.seq, guid);
        if (result)
        {
            u.status = "done";
            u.reason = "already withdrew this order";
            return u;
        }
        p.guild = sGuildVault.GuildOf(bot->GetSession()->GetAccountId());
    }

    u.reason = NotReady(bot);
    if (!u.reason.empty())
    {
        p.nearestAt = 0;
        return u;
    }
    if (p.guild.empty())
        return fail("has no guild vault");

    // What the vault holds of the order: each item, and the gold.
    auto inVault = [&]()
    {
        bool any = gold && sGuildVault.Gold(p.guild) > 0;
        for (auto const& [entry, count] : items)
            any = any || sGuildVault.Count(p.guild, entry) > 0;
        return any;
    };

    // Before it sets out: to the vault only if it holds some of the order.
    Trip trip;
    if (!CurrentTrip(guid, trip))
    {
        if (!inVault())
            return fail("the vault holds none of it");
        if (!CounterTrip(bot, CounterKind::Vaultkeeper, trip))
            return fail("no vaultkeeper known for its faction");
        StartTrip(guid, trip.pos, trip.place, trip.where);
        p.nearestAt = 0;
    }

    if (!InReach(bot, trip.pos))
    {
        std::string why;
        if (Stalled(bot, trip.pos, COUNTER_STALL, p.nearest, p.nearestMap, p.nearestAt, why))
            return fail("could not reach " + trip.where + ": " + why);
        u.reason = "on its way to " + trip.where;
        return u;
    }

    // As much as the vault holds and the bags and purse take; another character of the guild may have taken some
    // on the way.
    std::vector<std::pair<uint32, uint32>> took, shortOf;
    std::string bagNote;
    for (auto const& [entry, count] : items)
    {
        const uint32 held = sGuildVault.Count(p.guild, entry);
        const uint32 want = count ? count : held;
        uint32 got = 0;
        if (want)
        {
            std::string why;
            InventoryResult bagError = EQUIP_ERR_OK;
            got = sGuildVault.Withdraw(bot, entry, want, why, &bagError);
            if (bagError != EQUIP_ERR_OK && bagNote.empty())
                bagNote = "bags full";
        }
        if (got)
            took.push_back({ entry, got });
        if (count && got < count)
            shortOf.push_back({ entry, count - got });
        else if (!count && !held)
            shortOf.push_back({ entry, 0 });
    }
    uint32 goldOut = 0;
    std::string goldNote;
    if (gold)
    {
        std::string why;
        goldOut = sGuildVault.WithdrawGold(bot, uint32(gold), why);
        if (goldOut < gold)
        {
            if (!why.empty() && why.back() == '.')
                why.pop_back();
            goldNote = goldOut ? "short " + Money(uint32(gold) - goldOut) + " of gold" : why;
        }
    }

    if (took.empty() && !goldOut)
        return fail("took nothing" + (bagNote.empty() ? "" : "; " + bagNote) + (goldNote.empty() ? "" : "; " + goldNote));

    std::string reason = "took " + (goldOut ? Money(goldOut) + (took.empty() ? "" : " and ") : "") + (took.empty() ? "" : Names(took));
    if (!shortOf.empty())
    {
        reason += "; the vault was short of ";
        for (size_t i = 0; i < shortOf.size(); ++i)
            reason += (i ? ", " : "") + (shortOf[i].second ? std::to_string(shortOf[i].second) + " " : "") + ItemName(shortOf[i].first);
    }
    if (!bagNote.empty())
        reason += "; " + bagNote;
    if (!goldNote.empty())
        reason += "; " + goldNote;

    std::string detail = "seq=" + std::to_string(p.seq);
    if (!took.empty())
        detail += " items=" + Entries(took);
    if (goldOut)
        detail += " gold=" + std::to_string(goldOut);
    u.status = "done";
    u.reason = reason;
    u.events.push_back({ "withdrew", detail });
    return u;
#endif
}

void GuildJobs::EnsureTable()
{
    if (tableReady)
        return;
    // The craft job's count, kept so a restart does not start the order over.
    CharacterDatabase.DirectExecute(
        "CREATE TABLE IF NOT EXISTS guildbot_job_progress ("
        " guid INT UNSIGNED NOT NULL PRIMARY KEY,"
        " job_seq INT UNSIGNED NOT NULL,"
        " made INT UNSIGNED NOT NULL DEFAULT 0,"
        " updated_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP"
        ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4");
    tableReady = true;
}

uint32 GuildJobs::LoadMade(uint32 guid, uint32 seq)
{
    EnsureTable();
    auto result = CharacterDatabase.PQuery("SELECT made FROM guildbot_job_progress WHERE guid = %u AND job_seq = %u", guid, seq);
    return result ? result->Fetch()[0].GetUInt32() : 0;
}

void GuildJobs::SaveMade(uint32 guid, uint32 seq, uint32 made)
{
    EnsureTable();
    CharacterDatabase.PExecute("REPLACE INTO guildbot_job_progress (guid, job_seq, made) VALUES (%u, %u, %u)", guid, seq, made);
}
