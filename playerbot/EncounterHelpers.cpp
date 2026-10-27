#include "playerbot/playerbot.h"
#include "playerbot/EncounterHelpers.h"
#include "playerbot/strategy/values/RtiTargetValue.h"
#include "Grids/GridNotifiers.h"
#include "Grids/GridNotifiersImpl.h"
#include "Grids/CellImpl.h"

#include <algorithm>

namespace
{
    class DynamicObjectsInRangeCheck
    {
    public:
        DynamicObjectsInRangeCheck(WorldObject const* obj, float range) : obj(obj), range(range) {}
        WorldObject const& GetFocusObject() const { return *obj; }
        bool operator()(DynamicObject* dynObj) { return dynObj && obj->IsWithinDistInMap(dynObj, range); }

    private:
        WorldObject const* obj;
        float range;
    };
}

namespace EncounterHelpers
{
    bool IsEncounterInProgress(Player* bot, uint32 mapId)
    {
        if (bot->GetMapId() != mapId)
            return false;

        InstanceData* instance = bot->GetInstanceData();
        return instance && instance->IsEncounterInProgress();
    }

    bool MarkTargetWithIcon(Player* bot, Unit* target, uint8 iconId)
    {
        Group* group = bot->GetGroup();
        if (!target || !group || group->GetTargetIcon(iconId) == target->GetObjectGuid())
            return false;

#ifndef MANGOSBOT_TWO
        group->SetTargetIcon(iconId, target->GetObjectGuid());
#else
        group->SetTargetIcon(iconId, bot->GetObjectGuid(), target->GetObjectGuid());
#endif
        return true;
    }

    bool MarkTargetWithSkull(Player* bot, Unit* target) { return MarkTargetWithIcon(bot, target, RtiTargetValue::GetRtiIndex("skull")); }
    bool MarkTargetWithSquare(Player* bot, Unit* target) { return MarkTargetWithIcon(bot, target, RtiTargetValue::GetRtiIndex("square")); }
    bool MarkTargetWithStar(Player* bot, Unit* target) { return MarkTargetWithIcon(bot, target, RtiTargetValue::GetRtiIndex("star")); }
    bool MarkTargetWithCircle(Player* bot, Unit* target) { return MarkTargetWithIcon(bot, target, RtiTargetValue::GetRtiIndex("circle")); }
    bool MarkTargetWithDiamond(Player* bot, Unit* target) { return MarkTargetWithIcon(bot, target, RtiTargetValue::GetRtiIndex("diamond")); }
    bool MarkTargetWithTriangle(Player* bot, Unit* target) { return MarkTargetWithIcon(bot, target, RtiTargetValue::GetRtiIndex("triangle")); }
    bool MarkTargetWithCross(Player* bot, Unit* target) { return MarkTargetWithIcon(bot, target, RtiTargetValue::GetRtiIndex("cross")); }
    bool MarkTargetWithMoon(Player* bot, Unit* target) { return MarkTargetWithIcon(bot, target, RtiTargetValue::GetRtiIndex("moon")); }

    bool ClearTargetIcon(Player* bot, uint8 iconId)
    {
        Group* group = bot->GetGroup();
        if (!group || !group->GetTargetIcon(iconId))
            return false;

#ifndef MANGOSBOT_TWO
        group->SetTargetIcon(iconId, ObjectGuid());
#else
        group->SetTargetIcon(iconId, bot->GetObjectGuid(), ObjectGuid());
#endif
        return true;
    }

    bool ClearStrategyTargetIcon(Player* bot, uint8 iconId, std::initializer_list<uint32> entries)
    {
        Group* group = bot->GetGroup();
        if (!group)
            return false;

        uint32 entry = group->GetTargetIcon(iconId).GetEntry();
        if (std::find(entries.begin(), entries.end(), entry) == entries.end())
            return false;

        return ClearTargetIcon(bot, iconId);
    }

    void SetRtiTarget(PlayerbotAI* ai, const std::string& rtiName)
    {
        ai::Value<std::string>* rti = ai->GetAiObjectContext()->GetValue<std::string>("rti");
        if (rti->Get() != rtiName)
            rti->Set(rtiName);
    }

    bool IsMechanicTrackerBot(Player* bot, uint32 mapId)
    {
        Group* group = bot->GetGroup();
        if (!group)
            return false;

        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player* member = ref->getSource();
            if (!member || !member->IsAlive() || member->GetMapId() != mapId || !member->GetPlayerbotAI())
                continue;

            return member == bot;
        }

        return false;
    }

    Player* GetGroupMainTank(Player* bot)
    {
        Group* group = bot->GetGroup();
        if (!group)
            return nullptr;

        Player* mainTank = sObjectMgr.GetPlayer(PlayerbotAI::GetMainTankGuid(group));
        return mainTank && mainTank->IsAlive() ? mainTank : nullptr;
    }

    Player* GetGroupAssistTank(Player* bot, uint8 index)
    {
        Group* group = bot->GetGroup();
        if (!group)
            return nullptr;

        for (Player* tank : PlayerbotAI::GetGroupTanks(group))
        {
            if (PlayerbotAI::IsAssistTankOfIndex(tank, index, true))
                return tank;
        }

        return nullptr;
    }

    Player* GetNearestPlayerInRadius(Player* bot, float radius)
    {
        Group* group = bot->GetGroup();
        if (!group)
            return nullptr;

        Player* nearest = nullptr;
        float nearestDistance = radius;
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player* member = ref->getSource();
            if (!member || member == bot || !member->IsAlive() || member->GetMapId() != bot->GetMapId())
                continue;

            float distance = bot->GetDistance2d(member->GetPositionX(), member->GetPositionY(), DIST_CALC_NONE);
            if (distance < nearestDistance)
            {
                nearestDistance = distance;
                nearest = member;
            }
        }

        return nearest;
    }

    std::vector<WorldPosition> GetDynamicObjectPositions(Player* bot, float searchRadius, uint32 spellId)
    {
        std::list<DynamicObject*> dynObjs;
        DynamicObjectsInRangeCheck check(bot, searchRadius);
        // The wotlk core lacks this searcher, as NearestDynamicObjects notes.
#ifndef MANGOSBOT_TWO
        MaNGOS::DynamicObjectListSearcher<DynamicObjectsInRangeCheck> searcher(dynObjs, check);
        Cell::VisitAllObjects((const WorldObject*)bot, searcher, searchRadius);
#endif

        std::vector<WorldPosition> positions;
        for (DynamicObject* dynObj : dynObjs)
        {
            if (dynObj->GetSpellId() == spellId)
                positions.push_back(WorldPosition(dynObj));
        }

        return positions;
    }
}
