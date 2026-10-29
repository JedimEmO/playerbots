
#include "playerbot/playerbot.h"
#include "ReviveFromCorpseAction.h"
#include "playerbot/PlayerbotFactory.h"
#include "playerbot/PlayerbotAIConfig.h"
#include "playerbot/FleeManager.h"
#include "playerbot/TravelMgr.h"
#include "playerbot/ServerFacade.h"
#include "playerbot/strategy/values/DeadValues.h"
#include "playerbot/GuildDungeon.h"

using namespace ai;

bool ReviveFromCorpseAction::Execute(Event& event)
{
    Player* requester = event.getOwner() ? event.getOwner() : GetMaster();
    Player* master = ai->GetGroupMaster();
    Corpse* corpse = bot->GetCorpse();

    // follow master when master revives
    WorldPacket& p = event.getPacket();
    if (!p.empty() && p.GetOpcode() == CMSG_RECLAIM_CORPSE && master && !corpse && sServerFacade.IsAlive(bot))
    {
        if (sServerFacade.IsDistanceLessThan(AI_VALUE2(float, "distance", "master target"), sPlayerbotAIConfig.farDistance))
        {
            std::string defaultMovementStrategy = ai->GetDefaultMovementStrategy();

            if (!ai->HasStrategy(defaultMovementStrategy, BotState::BOT_STATE_NON_COMBAT))
            {
                ai->TellPlayerNoFacing(requester, "Welcome back!", PlayerbotSecurityLevel::PLAYERBOT_SECURITY_ALLOW_ALL, false);
                ai->ChangeStrategy("+" + defaultMovementStrategy + ",-stay", BotState::BOT_STATE_NON_COMBAT);
                return true;
            }
        }
    }

    if (!corpse)
        return false;

    if (corpse->GetGhostTime() + bot->GetCorpseReclaimDelay(corpse->GetType() == CORPSE_RESURRECTABLE_PVP) > time(nullptr))
        return false;

    if (master)
    {
        //Revive with master.
        if (bot != master && sServerFacade.UnitIsDead(master) && master->GetCorpse() && sServerFacade.IsDistanceLessThan(AI_VALUE2(float, "distance", "master target"), sPlayerbotAIConfig.farDistance))
            return false;
    }

    sLog.outDetail("Bot #%d %s:%d <%s> revives at body", bot->GetGUIDLow(), bot->GetTeam() == ALLIANCE ? "A" : "H", bot->GetLevel(), bot->GetName());

    ai->StopMoving();
    WorldPacket packet(CMSG_RECLAIM_CORPSE);
    packet << bot->GetObjectGuid();
    bot->GetSession()->HandleReclaimCorpseOpcode(packet);

    sPlayerbotAIConfig.logEvent(ai, "ReviveFromCorpseAction");

    return true;
}

bool FindCorpseAction::Execute(Event& event)
{
    if (bot->InBattleGround())
        return false;

    Corpse* corpse = bot->GetCorpse();
    if (!corpse)
        return false;

    // A guild character's corpse in a dungeon, the ghost outside: it walks to the entrance and takes it,
    // which revives it inside, as it does a player; the trigger's guard has it go after its leader.
    WorldLocation entrance;
    uint32 triggerId = 0;
    float reach = 0.0f;
    if (GuildDungeon::CorpseEntrance(bot, entrance, triggerId, reach))
    {
        if (!bot->IsInWorld() || bot->IsBeingTeleported())
            return true;
        WorldPosition at(entrance.mapid, entrance.coord_x, entrance.coord_y, entrance.coord_z);
        // In the trigger as the core checks it, with a margin inside the core's: a sphere's centre can lie
        // under the walkable ground, as Blackfathom Deeps' does under water.
        AreaTriggerEntry const* atEntry = sAreaTriggerStore.LookupEntry(triggerId);
        if (!atEntry || !IsPointInAreaTriggerZone(atEntry, bot->GetMapId(), bot->GetPositionX(), bot->GetPositionY(),
            bot->GetPositionZ(), 2.0f))
        {
            const float distance = WorldPosition(bot).distance(at);
            // Unwatched, a ghost that has run as long as the way should take is put at the entrance, as upstream puts an
            // unwatched bot's ghost at its corpse below. Ghosts that ran the whole corpse-run limit and then took the
            // spirit healer outside were found at eight of the dungeons: short of entrances the travel map has no way
            // into, and pacing by the Dire Maul East gate.
            const time_t ran = time(nullptr) - corpse->GetGhostTime();
            const time_t way = time_t(1.5f * distance / std::max(bot->GetSpeed(MOVE_RUN), 1.0f));
            if (ran > std::max(time_t(3 * MINUTE), way) && !ai->HasActivePlayerMaster()
                && !ai->HasPlayerNearby(at, 0.0f) && !ai->HasPlayerNearby(WorldPosition(bot), 0.0f))
            {
                sLog.outString("Guild dungeon: ghost %s put at the entrance of map %u after %u s, %.0f yards short",
                    bot->GetName(), corpse->GetMapId(), uint32(ran), distance);
                bot->GetMotionMaster()->Clear();
                bot->TeleportTo(entrance.mapid, entrance.coord_x, entrance.coord_y, entrance.coord_z, bot->GetOrientation());
                return true;
            }
            if (distance > reach && MoveTo(entrance.mapid, entrance.coord_x, entrance.coord_y, entrance.coord_z, false, false))
                return true;
            // No route the travel map knows, or near it and not in it: straight at it, as the area trigger action goes
            // into a trigger.
            bot->GetMotionMaster()->MovePoint(entrance.mapid, entrance.coord_x, entrance.coord_y, entrance.coord_z, FORCED_MOVEMENT_RUN);
            return true;
        }
        if (!GuildDungeon::MayEnter(bot, triggerId))
            return false;
        WorldPacket packet(CMSG_AREATRIGGER);
        packet << triggerId;
        packet.rpos(0);
        bot->GetSession()->HandleAreaTriggerOpcode(packet);
        return true;
    }

    Player* master = ai->GetGroupMaster();
    if (master)
    {
        if (!master->GetPlayerbotAI() && sServerFacade.IsDistanceLessThan(AI_VALUE2(float, "distance", "master target"), sPlayerbotAIConfig.farDistance))
        {
            return false;
        }
    }

    WorldPosition botPos(bot), corpsePos(corpse), moveToPos = corpsePos, masterPos(master);
    float reclaimDist = CORPSE_RECLAIM_RADIUS - 5.0f;
    float corpseDist = botPos.distance(corpsePos);

    if (!corpsePos) //Corpse is at 0,0,0,0
        return false;

    //If player fell through terrain move corpse to player position.
    if (bot->isRealPlayer() && botPos.getMapId() == moveToPos.getMapId())
    {
        //Try to correct the position upward.
        if (!moveToPos.ClosestCorrectPoint(5.0f, 500.0f, bot->GetInstanceId()))
        {
            //Revive in place.
            corpse->Relocate(botPos.getX(), botPos.getY(), botPos.getZ());
            corpsePos = corpse;
            corpseDist = botPos.distance(corpsePos);
        }
        else
        {
            corpse->Relocate(moveToPos.getX(), moveToPos.getY(), moveToPos.getZ());
            corpsePos = corpse;
            corpseDist = botPos.distance(corpsePos);
        }
    }

    int64 deadTime = time(nullptr) - corpse->GetGhostTime();

    bool moveToMaster = master && master != bot && masterPos.fDist(corpsePos) < reclaimDist;

    //Should we ressurect? If so, return false.
    if (corpseDist < reclaimDist)
    {
        if (moveToMaster) //We are near master.
        {
            if (botPos.fDist(masterPos) < sPlayerbotAIConfig.spellDistance)
                return false;
        }
        else if (deadTime > 8 * MINUTE) //We have walked too long already.
            return false;
        else
        {
            std::list<ObjectGuid> units = AI_VALUE(std::list<ObjectGuid>, "possible targets no los");

            if (botPos.getUnitsAggro(units, bot) == 0) //There are no mobs near.
                return false;
        }
    }

    //If we are getting close move to a save ressurrection spot instead of just the corpse.
    if (corpseDist < sPlayerbotAIConfig.reactDistance)
    {
        if (moveToMaster)
        {
            if (ai->HasStrategy("debug move", BotState::BOT_STATE_NON_COMBAT))
            {
                std::ostringstream out;
                out << "Moving to revive near master.";
                ai->TellPlayerNoFacing(GetMaster(), out);
            }
            moveToPos = masterPos;
        }
        else
        {
            FleeManager manager(bot, reclaimDist, 0.0, urand(0, 1), moveToPos);

            if (manager.isUseful())
            {
                float rx, ry, rz;
                if (manager.CalculateDestination(&rx, &ry, &rz))
                {
                    if (ai->HasStrategy("debug move", BotState::BOT_STATE_NON_COMBAT))
                    {
                        std::ostringstream out;
                        out << "Moving to revive some where safe.";
                        ai->TellPlayerNoFacing(GetMaster(), out);
                    }
                    moveToPos = WorldPosition(moveToPos.getMapId(), rx, ry, rz, 0.0);
                }
                else if (!moveToPos.GetReachableRandomPointOnGround(bot, reclaimDist, urand(0, 1)))
                {
                    if (ai->HasStrategy("debug move", BotState::BOT_STATE_NON_COMBAT))
                    {
                        std::ostringstream out;
                        out << "Moving to revive at corpse.";
                        ai->TellPlayerNoFacing(GetMaster(), out);
                    }
                    moveToPos = corpsePos;
                }
            }
        }
    }
    else
    {
        if (ai->HasStrategy("debug move", BotState::BOT_STATE_NON_COMBAT))
        {
            std::ostringstream out;
            out << "Moving towards corpse.";
            ai->TellPlayerNoFacing(GetMaster(), out);
        }
    }

    //Actual mobing part.
    bool moved = false;

    if (!ai->AllowActivity(DETAILED_MOVE_ACTIVITY) && !ai->HasPlayerNearby(moveToPos))
    {
        uint32 delay = sServerFacade.GetDistance2d(bot, corpse) / bot->GetSpeed(MOVE_RUN); //Time a bot would take to travel to it's corpse.
        delay = std::min(delay, uint32(10 * MINUTE)); //Cap time to get to corpse at 10 minutes.

        if (deadTime > delay)
        {
            bot->GetMotionMaster()->Clear();
            bot->TeleportTo(moveToPos.getMapId(), moveToPos.getX(), moveToPos.getY(), moveToPos.getZ(), 0);
            if (bot->isRealPlayer())
                bot->SendHeartBeat();
        }

        moved = true;
    }
    else
    {
#ifndef MANGOSBOT_ZERO
        if (bot->IsMovingIgnoreFlying())
            moved = true;
#else
        if (bot->IsMoving())
            moved = true;
#endif
        else
        {

            moved = MoveTo(moveToPos.getMapId(), moveToPos.getX(), moveToPos.getY(), moveToPos.getZ(), false, false);

            // A guild character keeps to its corpse in a dungeon; it never takes the spirit healer instead.
            if (!moved && !ai->HasActivePlayerMaster() && !GuildDungeon::KeepsToCorpse(bot)) //We could not move to coprse. Try spirithealer instead.
            {
                moved = ai->DoSpecificAction("spirit healer", Event(), true);
            }
        }
    }

    return moved;
}

bool FindCorpseAction::isUseful()
{
    if (bot->InBattleGround())
        return false;

    return bot->GetCorpse();
}

bool SpiritHealerAction::Execute(Event& event)
{
    Player* requester = event.getOwner() ? event.getOwner() : GetMaster();
    Corpse* corpse = bot->GetCorpse();
    if (!corpse)
    {
        ai->TellPlayerNoFacing(requester, "I am not a spirit");
        return false;
    }

    uint32 dCount = AI_VALUE(uint32, "death count");
    GuidPosition grave = AI_VALUE(GuidPosition, "best graveyard");
    // A guild ghost whose corpse lies in a dungeon, back too late from its corpse run: the graveyard by that dungeon's
    // entrance, so it revives next to its party's way in rather than at whichever graveyard upstream ranks best.
    WorldLocation dungeonGrave;
    if (GuildDungeon::CorpseGraveyard(bot, dungeonGrave))
        grave = GuidPosition(ObjectGuid(), WorldPosition(dungeonGrave.mapid, dungeonGrave.coord_x, dungeonGrave.coord_y, dungeonGrave.coord_z));

    //something went wrong
    if (!grave)
    {
        //prevent doing weird stuff OR GOING TO 0,0,0
        sLog.outDetail(
            "ERROR: no graveyard in SpiritHealerAction for bot #%d %s:%d <%s>, evacuating to prevent weird behavior",
            bot->GetGUIDLow(),
            bot->GetTeam() == ALLIANCE ? "A" : "H",
            bot->GetLevel(),
            bot->GetName()
        );
        ai->DoSpecificAction("repop");
        return false;
    }

    if (grave && grave.fDist(bot) < sPlayerbotAIConfig.sightDistance)
    {
        bool foundSpiritHealer = false;
        std::list<ObjectGuid> npcs = AI_VALUE(std::list<ObjectGuid>, "nearest npcs");
        for (std::list<ObjectGuid>::iterator i = npcs.begin(); i != npcs.end(); i++)
        {
            Unit* unit = ai->GetUnit(*i);
            if (unit && unit->HasFlag(UNIT_NPC_FLAGS, UNIT_NPC_FLAG_SPIRITHEALER))
            {
                foundSpiritHealer = true;
                break;
            }
        }

        if (!foundSpiritHealer)
        {
            sLog.outDetail("Bot #%d %s:%d <%s> can't find a spirit healer", bot->GetGUIDLow(), bot->GetTeam() == ALLIANCE ? "A" : "H", bot->GetLevel(), bot->GetName());
            ai->TellPlayerNoFacing(requester, "Cannot find any spirit healer nearby");
        }


        sLog.outDetail("Bot #%d %s:%d <%s> revives at spirit healer", bot->GetGUIDLow(), bot->GetTeam() == ALLIANCE ? "A" : "H", bot->GetLevel(), bot->GetName());
        PlayerbotChatHandler ch(bot);
        bot->ResurrectPlayer(0.5f, !ai->HasCheat(BotCheatMask::repair));
        bot->DurabilityLossAll(0.25f, true);

        bot->SpawnCorpseBones();
        bot->SaveToDB();
        context->GetValue<ObjectGuid>("current target")->Set(ObjectGuid());
        bot->SetSelectionGuid(ObjectGuid());
        ai->TellPlayer(requester, BOT_TEXT("hello"), PlayerbotSecurityLevel::PLAYERBOT_SECURITY_ALLOW_ALL, false);
        sPlayerbotAIConfig.logEvent(ai, "ReviveFromSpiritHealerAction");

        return true;
    }

    bool shouldTeleportToGY = false;

    const int64 deadTime = time(nullptr) - corpse->GetGhostTime();

    // Prevent taking too long to go to corpse (10 mins)
    // no need to wait longer, because bot is probably stuck in navigating issues
    shouldTeleportToGY = deadTime > uint32(10 * MINUTE);

    // Check if we can teleport to the graveyard when nobody is looking
    if (!shouldTeleportToGY && !ai->AllowActivity(DETAILED_MOVE_ACTIVITY) && !ai->HasPlayerNearby(WorldPosition(grave)))
    {
        //Time a bot would take to travel to it's corpse.
        uint32 delay = sServerFacade.GetDistance2d(bot, corpse) / bot->GetSpeed(MOVE_RUN);
        //Cap time to get to corpse at 10 minutes.
        delay = std::min(delay, uint32(10 * MINUTE));

        shouldTeleportToGY = deadTime > delay;
    }

    if (ai->HasStrategy("debug move", BotState::BOT_STATE_NON_COMBAT))
    {
        std::ostringstream out;
        out << "Moving towards graveyard.";
        ai->TellPlayerNoFacing(GetMaster(), out);
    }

    if (shouldTeleportToGY)
    {
        bot->GetMotionMaster()->Clear();
        bot->TeleportTo(grave.getMapId(), grave.getX(), grave.getY(), grave.getZ(), 0);
        if (bot->isRealPlayer())
            bot->SendHeartBeat();
        return true;
    }
    else
    {
        return MoveTo(grave.getMapId(), grave.getX(), grave.getY(), grave.getZ(), false, false);
    }
}

bool SpiritHealerAction::isUseful()
{
    if (bot->InBattleGround())
        return false;

    // A guild character whose corpse lies in a dungeon goes back to it.
    if (GuildDungeon::KeepsToCorpse(bot))
        return false;

    return bot->HasFlag(PLAYER_FLAGS, PLAYER_FLAGS_GHOST);
}
