#include "playerbot/playerbot.h"
#include "FarmOrderValues.h"
#include "LootValues.h"
#include "SharedValueContext.h"
#include "playerbot/RandomPlayerbotMgr.h"
#include "playerbot/LootObjectStack.h"
#include "playerbot/ServerFacade.h"

using namespace ai;

namespace
{
    bool LootHolds(LootTemplateAccess const* loot, uint32 itemId)
    {
        // References (mincountOrRef below 0) name another template, not an item; the drop map leaves them out too.
        auto holds = [itemId](const LootStoreItemList& items)
        {
            for (const LootStoreItem& item : items)
                if (item.itemid == itemId && item.mincountOrRef > 0)
                    return true;
            return false;
        };

        if (holds(loot->Entries))
            return true;

        for (const LootLootGroupAccess& group : loot->Groups)
            if (holds(group.ExplicitlyChanced) || holds(group.EqualChanced))
                return true;

        return false;
    }

    // The creatures to skin for each item, built once: the drop map holds corpse loot only.
    const std::unordered_multimap<uint32, uint32>& SkinMap()
    {
        static std::unordered_multimap<uint32, uint32> skins;
        static std::once_flag built;
        std::call_once(built, []()
        {
            for (uint32 entry = 0; entry < sCreatureStorage.GetMaxEntry(); ++entry)
            {
                CreatureInfo const* info = sCreatureStorage.LookupEntry<CreatureInfo>(entry);
                if (!info || !info->SkinningLootId || info->GetRequiredLootSkill() != SKILL_SKINNING)
                    continue;

                LootTemplateAccess const* loot = DropMapValue::GetLootTemplate(ObjectGuid(HIGHGUID_UNIT, entry, uint32(1)), LOOT_SKINNING);
                if (!loot)
                    continue;

                std::set<uint32> items;
                auto add = [&items](const LootStoreItemList& list)
                {
                    for (const LootStoreItem& item : list)
                        if (item.mincountOrRef > 0)
                            items.insert(item.itemid);
                };
                add(loot->Entries);
                for (const LootLootGroupAccess& group : loot->Groups)
                {
                    add(group.ExplicitlyChanced);
                    add(group.EqualChanced);
                }

                for (uint32 itemId : items)
                    skins.emplace(itemId, entry);
            }
        });
        return skins;
    }
}

std::vector<FarmSource> FarmOrders::Sources(uint32 itemId)
{
    static std::mutex mutex;
    static std::unordered_map<uint32, std::vector<FarmSource>> cache;

    {
        std::lock_guard<std::mutex> guard(mutex);
        auto it = cache.find(itemId);
        if (it != cache.end())
            return it->second;
    }

    std::vector<FarmSource> sources;

    // Herbs and ore: the nodes in the drop map, with the skill their lock asks for, as GatherTravelDestination reads it.
    for (int32 entry : GAI_VALUE2(std::list<int32>, "item drop list", itemId))
    {
        if (entry >= 0)
            continue;

        GameObjectInfo const* info = ObjectMgr::GetGameObjectInfo(uint32(-entry));
        if (!info)
            continue;

        LockEntry const* lock = sLockStore.LookupEntry(info->GetLockId());
        if (!lock)
            continue;

        for (int i = 0; i < 8; ++i)
        {
            if (lock->Type[i] != LOCK_KEY_SKILL)
                continue;

            SkillType skill = SkillType(SkillByLockType(LockType(lock->Index[i])));
            if (skill == SKILL_HERBALISM || skill == SKILL_MINING)
                sources.push_back({ entry, skill, std::max<uint32>(1, lock->Skill[i]) });
            break;
        }
    }

    // Skins: the skill a creature asks for, by its highest level, as GatherTravelDestination::IsPossible reads it.
    auto range = SkinMap().equal_range(itemId);
    for (auto it = range.first; it != range.second; ++it)
    {
        CreatureInfo const* info = sCreatureStorage.LookupEntry<CreatureInfo>(it->second);
        if (!info)
            continue;

        uint32 level = info->MaxLevel;
        uint32 reqSkill = level < 10 ? 1 : level < 20 ? (level - 10) * 10 : level * 5;
        sources.push_back({ int32(it->second), SKILL_SKINNING, reqSkill });
    }

    std::lock_guard<std::mutex> guard(mutex);
    cache[itemId] = sources;
    return sources;
}

bool FarmOrders::IsSource(uint32 itemId, int32 entry)
{
    for (const FarmSource& source : Sources(itemId))
        if (source.entry == entry)
            return true;
    return false;
}

uint32 FarmOrders::FindItem(const std::string& text, std::string& error)
{
    if (Qualified::isValidNumberString(text))
    {
        uint32 itemId = text.size() <= 6 ? uint32(std::stoul(text)) : 0;
        if (itemId && sItemStorage.LookupEntry<ItemPrototype>(itemId) && !Sources(itemId).empty())
            return itemId;

        error = "No herb, ore or skin with item id " + text;
        return 0;
    }

    auto lower = [](std::string s)
    {
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
        return s;
    };
    std::string wanted = lower(text);

    std::vector<uint32> exact, partial;
    for (uint32 itemId = 0; itemId < sItemStorage.GetMaxEntry(); ++itemId)
    {
        ItemPrototype const* proto = sItemStorage.LookupEntry<ItemPrototype>(itemId);
        if (!proto || proto->Class != ITEM_CLASS_TRADE_GOODS)
            continue;

        std::string name = lower(proto->Name1);
        if (name == wanted)
            exact.push_back(itemId);
        else if (name.find(wanted) != std::string::npos)
            partial.push_back(itemId);
    }

    for (uint32 itemId : exact)
        if (!Sources(itemId).empty())
            return itemId;

    std::vector<uint32> found;
    for (uint32 itemId : partial)
        if (!Sources(itemId).empty())
            found.push_back(itemId);

    if (found.size() == 1)
        return found.front();

    if (found.empty())
    {
        error = "No herb, ore or skin named " + text;
        return 0;
    }

    std::string names;
    for (size_t i = 0; i < found.size() && i < 5; ++i)
        names += std::string(i ? ", " : "") + sItemStorage.LookupEntry<ItemPrototype>(found[i])->Name1;
    if (found.size() > 5)
        names += ", ...";

    error = "Several herbs, ores and skins match " + text + ": " + names;
    return 0;
}

uint32 FarmOrders::Held(Player* bot, uint32 itemId)
{
    uint32 count = 0;
    auto add = [&count, bot, itemId](Item* item)
    {
        if (item && item->GetEntry() == itemId && !sRandomPlayerbotMgr.IsOwnersItem(bot, item))
            count += item->GetCount();
    };

    for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
        add(bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot));

    for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
    {
        Bag* bag = dynamic_cast<Bag*>(bot->GetItemByPos(INVENTORY_SLOT_BAG_0, bagSlot));
        if (!bag)
            continue;

        for (uint32 slot = 0; slot < bag->GetBagSize(); ++slot)
            add(bag->GetItemByPos(uint8(slot)));
    }

    return count;
}

uint32 FarmOrders::Gathered(Player* bot, uint32 itemId, uint32 mailed)
{
    return mailed + (bot ? Held(bot, itemId) : 0);
}

std::string FarmOrders::SkillName(SkillType skill)
{
    switch (skill)
    {
    case SKILL_HERBALISM: return "Herbalism";
    case SKILL_MINING: return "Mining";
    case SKILL_SKINNING: return "Skinning";
    default: return "skill " + std::to_string(uint32(skill));
    }
}

namespace
{
    // The spawn points each bot saw, by bot guid, with when, and the point it walks to. Bots on different maps ask
    // from their own threads.
    struct Roaming
    {
        std::unordered_map<const WorldPosition*, time_t> seen;
        const WorldPosition* current = nullptr;
        time_t since = 0;
    };
    std::mutex visitedMutex;
    std::unordered_map<uint32, Roaming> roaming;

    // With visitedMutex held: the bot's seen points, those older than VISIT_SECONDS dropped.
    std::unordered_map<const WorldPosition*, time_t>& VisitedOf(uint32 guid, time_t now)
    {
        auto& points = roaming[guid].seen;
        for (auto it = points.begin(); it != points.end();)
            it = it->second + FarmOrders::VISIT_SECONDS < now ? points.erase(it) : std::next(it);
        return points;
    }
}

GameObject* FarmOrders::NodeInSight(Player* bot)
{
    RandomPlayerbotMgr::FarmOrder order;
    if (!sRandomPlayerbotMgr.GetFarmOrder(bot, order))
        return nullptr;

    PlayerbotAI* ai = bot->GetPlayerbotAI();
    AiObjectContext* context = ai->GetAiObjectContext();
    LootObjectStack* stack = AI_VALUE(LootObjectStack*, "available loot");

    GameObject* nearest = nullptr;
    for (ObjectGuid guid : AI_VALUE(std::list<ObjectGuid>, "nearest game objects no los"))
    {
        if (!guid.IsGameObject() || !IsSource(order.item, -int32(guid.GetEntry())) || stack->GaveUpOn(guid))
            continue;

        GameObject* go = ai->GetGameObject(guid);
        if (!go || !sServerFacade.isSpawned(go))
            continue;

        LootObject loot(bot, guid);
        if (!loot.IsLootPossible(bot))
            continue;

        if (!nearest || bot->GetDistance(go) < bot->GetDistance(nearest))
            nearest = go;
    }

    return nearest;
}

GatherTravelDestination* FarmOrders::WorkedGather(PlayerbotAI* ai, bool travelling)
{
    TravelTarget* target = ai->GetAiObjectContext()->GetValue<TravelTarget*>("travel target")->Get();
    if (!target)
        return nullptr;

    TravelStatus status = target->GetStatus();
    if (status != TravelStatus::TRAVEL_STATUS_WORK && !(travelling && status == TravelStatus::TRAVEL_STATUS_TRAVEL))
        return nullptr;

    GatherTravelDestination* gather = dynamic_cast<GatherTravelDestination*>(target->GetDestination());
    if (!gather || gather->GetPurpose() == TravelDestinationPurpose::GatherFishing)
        return nullptr;

    for (auto& condition : target->GetConditions())
        if (condition == "should travel named::farm order")
            return gather;

    return nullptr;
}

const WorldPosition* FarmOrders::RoamPoint(Player* bot, const TravelDestination* destination)
{
    WorldPosition botPos(bot);
    time_t now = time(nullptr);

    std::lock_guard<std::mutex> guard(visitedMutex);
    auto& visited = VisitedOf(bot->GetGUIDLow(), now);
    Roaming& state = roaming[bot->GetGUIDLow()];

    if (state.current && now - state.since >= ROAM_SECONDS)
        visited[state.current] = now;

    const WorldPosition* nearest = nullptr;
    float nearestDistance = ROAM_DISTANCE;
    for (WorldPosition* point : destination->GetPoints())
    {
        if (point->getMapId() != botPos.getMapId())
            continue;

        float distance = point->distance(botPos);
        if (distance <= SEEN_DISTANCE)
        {
            visited[point] = now;
            continue;
        }

        if (distance < nearestDistance && visited.find(point) == visited.end())
        {
            nearest = point;
            nearestDistance = distance;
        }
    }

    // Still on the way to the point it chose, and it is in range and not seen: keep going there.
    if (state.current && visited.find(state.current) == visited.end() && state.current->getMapId() == botPos.getMapId() &&
        state.current->distance(botPos) < ROAM_DISTANCE)
        return state.current;

    state.current = nearest;
    state.since = now;
    return nearest;
}

void FarmOrders::MarkVisited(Player* bot, const WorldPosition* point)
{
    std::lock_guard<std::mutex> guard(visitedMutex);
    roaming[bot->GetGUIDLow()].seen[point] = time(nullptr);
}

std::set<const WorldPosition*> FarmOrders::Visited(Player* bot)
{
    std::lock_guard<std::mutex> guard(visitedMutex);
    std::set<const WorldPosition*> points;
    for (auto& [point, when] : VisitedOf(bot->GetGUIDLow(), time(nullptr)))
        points.insert(point);
    return points;
}

bool HasFarmOrderValue::Calculate()
{
    RandomPlayerbotMgr::FarmOrder order;
    if (!sRandomPlayerbotMgr.GetFarmOrder(bot, order))
        return false;

    return !order.count || FarmOrders::Gathered(bot, order.item, order.mailed) < order.count;
}
