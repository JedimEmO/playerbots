#include "playerbot/playerbot.h"
#include "playerbot/GuildJobs.h"
#include "playerbot/ServerFacade.h"
#include "Entities/Bag.h"
#include "Groups/Group.h"
#include "Grids/GridNotifiers.h"
#include "Grids/GridNotifiersImpl.h"
#include "Grids/CellImpl.h"

#include <algorithm>
#include <cctype>
#include <set>
#include <sstream>

namespace
{
    // A cast that does not start this many checks in a row, about half a minute, is not going to.
    constexpr uint32 CRAFT_CAST_TRIES = 6;
    // At most this many items on one craft order.
    constexpr uint64 CRAFT_MAX_AMOUNT = 1000;

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

    // How many of the item are in its backpack, keyring and bags, not equipped, not in a bag slot and not in a trade:
    // the ones Player::DestroyItemCount takes before it reaches the equipment.
    uint32 CountInBags(Player* bot, uint32 entry)
    {
        uint32 count = 0;
        auto add = [&](Item* item)
        {
            if (item && item->GetEntry() == entry && !item->IsInTrade())
                count += item->GetCount();
        };
        for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
            add(bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot));
        for (uint8 slot = KEYRING_SLOT_START; slot < KEYRING_SLOT_END; ++slot)
            add(bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot));
        for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
            if (Bag* bag = dynamic_cast<Bag*>(bot->GetItemByPos(INVENTORY_SLOT_BAG_0, bagSlot)))
                for (uint32 slot = 0; slot < bag->GetBagSize(); ++slot)
                    add(bag->GetItemByPos(uint8(slot)));
        return count;
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
    return job == "pay" || job == "reset" || job == "craft";
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
        p = Progress();
        p.seq = jobSeq;
    }

    if (job == "pay")
        return Pay(bot, args, p);
    if (job == "reset")
        return Reset(bot, args);
    return Craft(bot, args, p);
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
        std::istringstream in(itemsArg);
        std::string part;
        while (std::getline(in, part, ','))
        {
            size_t colon = part.find(':');
            uint64 entry = 0, count = 0;
            if (colon == std::string::npos || !ParseNumber(part.substr(0, colon), entry) || !ParseNumber(part.substr(colon + 1), count) || !count || count > 10000)
                return fail("pay: items must be <entry>:<count>,<entry>:<count>");
            if (!sObjectMgr.GetItemPrototype(uint32(entry)))
                return fail("pay: no item " + std::to_string(entry));
            auto it = std::find_if(items.begin(), items.end(), [entry](auto const& i) { return i.first == entry; });
            if (it != items.end())
                it->second += uint32(count);
            else
                items.push_back({ uint32(entry), uint32(count) });
        }
    }
    if (!gold && items.empty())
        return fail(usage);

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
    }

    u.reason = NotReady(bot);
    if (!u.reason.empty())
        return u;

    // All or nothing: every check first, then everything taken at once.
    if (bot->GetMoney() < gold)
        return fail("has " + Money(bot->GetMoney()) + ", needs " + Money(uint32(gold)));
    for (auto const& [entry, count] : items)
    {
        uint32 have = CountInBags(bot, entry);
        if (have < count)
            return fail("has " + std::to_string(have) + " of " + std::to_string(count) + " " + ItemName(entry));
    }

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
    u.reason = "paid " + what + " (ref " + refArg + ")";
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
