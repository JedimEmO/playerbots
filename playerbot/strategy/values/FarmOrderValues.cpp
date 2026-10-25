#include "playerbot/playerbot.h"
#include "FarmOrderValues.h"
#include "LootValues.h"
#include "SharedValueContext.h"
#include "playerbot/RandomPlayerbotMgr.h"

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

bool HasFarmOrderValue::Calculate()
{
    RandomPlayerbotMgr::FarmOrder order;
    if (!sRandomPlayerbotMgr.GetFarmOrder(bot, order))
        return false;

    return !order.count || FarmOrders::Gathered(bot, order.item, order.mailed) < order.count;
}
