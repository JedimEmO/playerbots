#pragma once
#include "playerbot.h"

namespace ai
{
    class ItemQualifier;

    class LootObject
    {
    public:
        LootObject() : skillId(0), reqSkillValue(0), reqItem(0) {}
        LootObject(Player* bot, ObjectGuid guid);
        LootObject(const LootObject& other);

    public:
        bool IsEmpty() { return !guid; }
        bool IsLootPossible(Player* bot);
        void Refresh(Player* bot, ObjectGuid guid, bool debug = false);
        WorldObject* GetWorldObject(Player* bot);
        ObjectGuid guid;

        uint32 skillId;
        uint32 reqSkillValue;
        uint32 reqItem;
    };

    class LootTarget
    {
    public:
        LootTarget(ObjectGuid guid);
        LootTarget(LootTarget const& other);

    public:
        LootTarget& operator=(LootTarget const& other);
        bool operator< (const LootTarget& other) const;

    public:
        ObjectGuid guid;
        time_t asOfTime;
    };

    class LootTargetList : public std::set<LootTarget>
    {
    public:
        void shrink(time_t fromTime);
    };

    class LootObjectStack
    {
    public:
        LootObjectStack(Player* bot) : bot(bot) {}

    public:
        bool Add(ObjectGuid guid);
        void Remove(ObjectGuid guid);
        void Clear();
        bool CanLoot(float maxDistance);
        LootObject GetLoot(float maxDistance = 0);

        // wow-playground: call on each step towards a loot object. Returns false, and leaves that object alone for
        // a while, once the bot has spent too long without getting there (a crate behind a wall, which the loot scan
        // finds again every few seconds because it ignores line of sight).
        bool NoteApproach(ObjectGuid guid);
        bool GaveUpOn(ObjectGuid guid);

    private:
        std::vector<LootObject> OrderByDistance(float maxDistance = 0);

    private:
        Player* bot;
        LootTargetList availableLoot;
        ObjectGuid approachGuid;
        time_t approachSince = 0;
        time_t approachLast = 0;
        std::map<ObjectGuid, time_t> givenUpUntil;
    };

};
