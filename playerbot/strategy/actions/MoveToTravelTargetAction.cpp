
#include "playerbot/playerbot.h"
#include "MoveToTravelTargetAction.h"
#include "playerbot/PlayerbotAIConfig.h"
#include "playerbot/ServerFacade.h"
#include "playerbot/LootObjectStack.h"
#include "MotionGenerators/PathFinder.h"
#include "playerbot/TravelMgr.h"
#include "playerbot/strategy/values/FreeMoveValues.h"
#include "playerbot/strategy/values/FarmOrderValues.h"
#include <iomanip>

using namespace ai;

bool MoveToTravelTargetAction::Execute(Event& event)
{
    TravelTarget* target = AI_VALUE(TravelTarget*, "travel target");

    if (target->GetStatus() == TravelStatus::TRAVEL_STATUS_READY)
    {
        ai->TellDebug(ai->GetMaster(), "The target is ready to travel start now.", "debug travel");
        target->SetStatus(TravelStatus::TRAVEL_STATUS_TRAVEL);
    }

    target->CheckStatus();

    // wow-playground: working a grind spot with nothing to fight, walk to one of its spawns, a living creature's if
    // the bot knows of one, so it finds the next pull. Like mod-playerbots' MoveRandomNear, a few points are tried
    // until one has a path.
    // wow-playground: see FarmOrders. Working a farm order's gather trip with no node to gather, walk to the nearest
    // spawn point not seen lately; most are empty, since nodes are pooled. With none left in range the trip ends, and
    // the next farm order trip goes to the nearest point not seen. Forced trips never end on their own.
    // A node of the item in sight comes first, working the trip or on the way: walk up to it, and the loot actions
    // gather it; they take loot within AiPlayerbot.LootDistance only, and the loot scan skips one more than 5 yards
    // above or below. Too long on the way, as for a node behind a wall, and the loot stack gives up on it.
    if (FarmOrders::WorkedGather(ai, true))
    {
        if (GameObject* node = FarmOrders::NodeInSight(bot))
        {
            LootObjectStack* stack = AI_VALUE(LootObjectStack*, "available loot");
            if (bot->GetDistance(node) > INTERACTION_DISTANCE && stack->NoteApproach(node->GetObjectGuid()) &&
                MoveTo(node->GetMapId(), node->GetPositionX(), node->GetPositionY(), node->GetPositionZ(), false, false))
            {
                if (ai->HasStrategy("debug move", BotState::BOT_STATE_NON_COMBAT))
                    ai->TellPlayerNoFacing(GetMaster(), "Walking " + std::to_string(uint32(bot->GetDistance(node))) + "y to " + chat->formatWorldobject(node));
                return true;
            }
        }
    }

    if (GatherTravelDestination* gather = FarmOrders::WorkedGather(ai))
    {
        for (uint32 attempt = 0; attempt < 5; attempt++)
        {
            const WorldPosition* roamPoint = FarmOrders::RoamPoint(bot, gather);
            if (!roamPoint)
            {
                ai->TellDebug(ai->GetMaster(), "Every spawn point of " + gather->GetTitle() + " within " +
                    std::to_string(uint32(FarmOrders::ROAM_DISTANCE)) + "y seen, leaving it.", "debug travel");
                target->SetForced(false);
                target->SetStatus(TravelStatus::TRAVEL_STATUS_EXPIRED);
                context->ClearValues("no active travel destinations");
                return false;
            }

            // Seen once walked to, or skipped for a while when there is no path.
            FarmOrders::MarkVisited(bot, roamPoint);
            if (MoveTo(roamPoint->getMapId(), roamPoint->getX(), roamPoint->getY(), roamPoint->getZ(), false, false))
            {
                if (ai->HasStrategy("debug move", BotState::BOT_STATE_NON_COMBAT))
                    ai->TellPlayerNoFacing(GetMaster(), "Roaming " + std::to_string(uint32(roamPoint->distance(bot))) + "y to a spawn point of " + gather->GetTitle());
                return true;
            }
        }

        return false;
    }

    if (target->GetStatus() == TravelStatus::TRAVEL_STATUS_WORK)
    {
        GrindSpotTravelDestination* spot = dynamic_cast<GrindSpotTravelDestination*>(target->GetDestination());
        if (!spot)
            return true;

        for (uint32 attempt = 0; attempt < 5; attempt++)
        {
            const GuidPosition* roamPoint = spot->GetRoamPoint(bot);
            if (!roamPoint)
                return false;

            if (MoveTo(roamPoint->getMapId(), roamPoint->getX(), roamPoint->getY(), roamPoint->getZ(), false, false))
            {
                if (ai->HasStrategy("debug move", BotState::BOT_STATE_NON_COMBAT))
                    ai->TellPlayerNoFacing(GetMaster(), "Roaming " + std::to_string(uint32(roamPoint->distance(bot))) + "y in " + spot->GetTitle());
                return true;
            }
        }

        return false;
    }

    if (target->GetStatus() != TravelStatus::TRAVEL_STATUS_TRAVEL)
        return true;

    WorldPosition botLocation(bot);
    WorldPosition location = *target->GetPosition();
    
    Group* group = bot->GetGroup();
    if (ai->IsGroupLeader() && !urand(0, 1) && !bot->IsInCombat())
    {        
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player* member = ref->getSource();
            if (member == bot)
                continue;

            if (!member->IsAlive())
                continue;

            if (!member->IsMoving())
                continue;

            if (member->GetPlayerbotAI() &&
                !(member->GetPlayerbotAI()->HasStrategy("follow", BotState::BOT_STATE_NON_COMBAT) || member->GetPlayerbotAI()->HasStrategy("wander", BotState::BOT_STATE_NON_COMBAT)))
                continue;

            WorldPosition memberPos(member);
            WorldPosition targetPos = *target->GetPosition();

            float memberDistance = std::min(botLocation.distance(memberPos), location.distance(memberPos));

            if (memberDistance < 50.0f)
                continue;
            if (memberDistance > sPlayerbotAIConfig.reactDistance * 20)
                continue;

           // float memberAngle = botLocation.getAngleBetween(targetPos, memberPos);

           // if (botLocation.getMapId() == targetPos.getMapId() && botLocation.getMapId() == memberPos.getMapId() && memberAngle < M_PI_F / 2) //We are heading that direction anyway.
           //     continue;

            if (!urand(0, 5))
            {
                std::ostringstream out;
                if ((ai->GetMaster() && !bot->GetGroup()->IsMember(ai->GetMaster()->GetObjectGuid())) || !ai->HasActivePlayerMaster())
                    out << "Waiting a bit for ";
                else
                    out << "Please hurry up ";

                out << member->GetName();

                if (bot->GetPlayerbotAI() && !ai->HasActivePlayerMaster())
                {
                    out << " who is " << round(memberDistance) << "y away";
                    if (!memberPos.getAreaName().empty())
                        out << " in " << memberPos.getAreaName();
                }

                ai->TellPlayerNoFacing(GetMaster(), out, PlayerbotSecurityLevel::PLAYERBOT_SECURITY_ALLOW_ALL, false);
            }

            // Introduce a random delay between 80% and 120% of maxWaitForMove to make waiting more natural
            uint32 randomDelay = sPlayerbotAIConfig.maxWaitForMove * (urand(80, 120) / 100.0f);
            target->SetExpireIn(target->GetTimeLeft() + randomDelay);

            SetDuration(randomDelay);

            // Occasionally face the member and perform an emote
            if (urand(0, 3) == 0) { // 25% chance to emote
                bot->SetFacingToObject(member);
                uint32 emoteChoice = urand(0, 2);
                switch (emoteChoice) {
                    case 0:
                        bot->HandleEmoteCommand(EMOTE_ONESHOT_POINT);
                        break;
                    case 1:
                        bot->HandleEmoteCommand(EMOTE_ONESHOT_TALK);
                        break;
                    case 2:
                        bot->HandleEmoteCommand(EMOTE_ONESHOT_EXCLAMATION);
                        break;
                }
            }

            return true;
        }
    }

    float x = location.getX();
    float y = location.getY();
    float z = location.getZ();
    float mapId = location.getMapId();

    if (botLocation.getMapId() == location.getMapId() && botLocation.sqDistance2d(location) < 10000.0f)
    {
        float maxDistance = target->GetDestination()->GetRadiusMin();

        float angle = 2 * M_PI * urand(0, 100) / 100.0;
        float mod = urand(50, 100) / 100.0;

        x += cos(angle) * maxDistance * mod;
        y += sin(angle) * maxDistance * mod;

        if (ai->HasStrategy("debug move", BotState::BOT_STATE_NON_COMBAT))
        {
            std::ostringstream out;
            out << "Moving to ";
            out << target->GetDestination()->GetTitle();
            if (!(*target->GetPosition() == WorldPosition()))
            {
                out << " at " << uint32(target->GetPosition()->distance(bot)) << "y";
            }
            if (target->GetStatus() != TravelStatus::TRAVEL_STATUS_EXPIRED)
                out << " for " << (target->GetTimeLeft() / 1000) << "s";
            if (target->GetRetryCount(true))
                out << " (move retry: " << target->GetRetryCount(true) << ")";
            else if (target->GetRetryCount(false))
                out << " (retry: " << target->GetRetryCount(false) << ")";
            ai->TellPlayerNoFacing(GetMaster(), out);
        }
    }

    bool canMove = MoveTo(mapId, x, y, z, false, false);

    if (!canMove)
    {
        target->IncRetry(true);

        if (target->IsMaxRetry(true))
        {
            // wow-playground: a grind spot the bot cannot get to is not picked again for a while.
            if (GrindSpotTravelDestination* spot = dynamic_cast<GrindSpotTravelDestination*>(target->GetDestination()))
                spot->Leave(bot, "no way there");

            ai->TellDebug(ai->GetMaster(), "The target is cooling down because we failed to move to it a few times in a row.", "debug travel");
            target->SetStatus(TravelStatus::TRAVEL_STATUS_COOLDOWN);      
            target->SetForced(false);
        }
    }
    else
        target->DecRetry(true);

    if (ai->HasStrategy("debug move", BotState::BOT_STATE_NON_COMBAT))
    {
        WorldPosition* pos = target->GetPosition();
        GuidPosition* guidP = dynamic_cast<GuidPosition*>(pos);

        std::string name = (guidP && guidP->GetWorldObject(bot->GetInstanceId())) ? chat->formatWorldobject(guidP->GetWorldObject(bot->GetInstanceId())) : "travel target";

        if (mapId == bot->GetMapId())
        {
            ai->Poi(x, y, name);
        }
        else
        {
            LastMovement& lastMove = *context->GetValue<LastMovement&>("last movement");
            if (!lastMove.lastPath.empty() && lastMove.lastPath.getBack().distance(location) < 20.0f)
            {
                for (auto& p : lastMove.lastPath.getPointPath())
                {
                    if (p.getMapId() == bot->GetMapId())
                        ai->Poi(p.getX(), p.getY(), name);
                }
            }
        }
    }
     
    return canMove;
}

bool MoveToTravelTargetAction::isUseful()
{
    if (!ai->AllowActivity(TRAVEL_ACTIVITY))
        return false;

    // wow-playground: also while working a grind spot, to walk between its spawns (see Execute). Only when the bot
    // stands, so a walk ends and eating or drinking is not cut short.
    TravelTarget* currentTarget = AI_VALUE(TravelTarget*, "travel target");
    bool roaming = currentTarget->GetStatus() == TravelStatus::TRAVEL_STATUS_WORK && dynamic_cast<GrindSpotTravelDestination*>(currentTarget->GetDestination());
    // wow-playground: and between the spawn points of a farm order's gather trip (see Execute).
    roaming = roaming || FarmOrders::WorkedGather(ai);
    // wow-playground: a bot "moving" without getting anywhere for ten seconds may go on, as the travel check below
    // allows. The travel action skips a moving bot, so one stuck that way at a vendor in town never checked its grind
    // spot, and worked it for half an hour with no kill; Execute checks the spot first.
    if (roaming && ((bot->IsMoving() && MEM_AI_VALUE(WorldPosition, "current position")->LastChangeDelay() < 10) || bot->IsSitState() || bot->IsInCombat()))
        return false;

    if (!roaming && !AI_VALUE(bool, "travel target traveling") && AI_VALUE(TravelTarget*, "travel target")->GetStatus() != TravelStatus::TRAVEL_STATUS_READY)
        return false;

    if (bot->IsTaxiFlying())
        return false;

    if (MEM_AI_VALUE(WorldPosition, "current position")->LastChangeDelay() < 10)
#ifndef MANGOSBOT_ZERO
        if (bot->IsMovingIgnoreFlying())
            return false;
#else
        if (bot->IsMoving())
            return false;
#endif

    if (!AI_VALUE(bool, "can move around"))
        return false;

    TravelTarget* travelTarget = AI_VALUE(TravelTarget*, "travel target");

    if (ai->HasStrategy("follow", BotState::BOT_STATE_NON_COMBAT) || ai->HasStrategy("wander", BotState::BOT_STATE_NON_COMBAT))
    {
        auto conditions = travelTarget->GetConditions();
        for (auto& cond : conditions)
        {
            if (cond == "should travel named::guild order" || cond == "should travel named::farm order")
                return false;
        }
    }

    if (bot->GetGroup() && !bot->GetGroup()->IsLeader(bot->GetObjectGuid()))
        if (ai->HasStrategy("follow", BotState::BOT_STATE_NON_COMBAT) ||
            ai->HasStrategy("stay", BotState::BOT_STATE_NON_COMBAT) ||
            ai->HasStrategy("guard", BotState::BOT_STATE_NON_COMBAT))
            if (!travelTarget->IsForced())
                return false;

    WorldPosition travelPos(*travelTarget->GetPosition());

    if (travelPos.isDungeon() && bot->GetGroup() && bot->GetGroup()->IsLeader(bot->GetObjectGuid()) && sTravelMgr.MapTransDistance(bot, travelPos, true) < sPlayerbotAIConfig.sightDistance && !AI_VALUE2(bool, "group and", "near leader"))
        return false;
     
    //Should the bot still loot something?
    if (AI_VALUE(bool, "has available loot"))
    {
        LootObject lootObject = AI_VALUE(LootObjectStack*, "available loot")->GetLoot(sPlayerbotAIConfig.lootDistance);
        if (lootObject.IsLootPossible(bot))
            return false;
    }

    //Is the bot still going to loot something?
    LootObject loot = AI_VALUE(LootObject, "loot target");

    if (loot.IsLootPossible(bot))
        return false;

    if (!travelTarget->IsForced())
        if (!CanFreeMoveValue::CanFreeMoveTo(ai, *travelTarget->GetPosition()))
            return false;

    return true;
}

