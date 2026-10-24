
#include "playerbot/playerbot.h"
#include "playerbot/PlayerbotAIConfig.h"
#include "Entities/Player.h"

using namespace ai;

PlayerbotAIBase::PlayerbotAIBase() : aiInternalUpdateDelay(0)
{
}

void PlayerbotAIBase::UpdateAIInternal(uint32 elapsed, bool minimal)
{
}

void PlayerbotAIBase::UpdateAI(uint32 elapsed)
{
    totalPmo.reset();
    totalPmo = sPerformanceMonitor.start(PERF_MON_TOTAL, "PlayerbotAIBase::FullTick");
    
    if (aiInternalUpdateDelay > elapsed)
        aiInternalUpdateDelay -= elapsed;
    else
        aiInternalUpdateDelay = 0;

    if (!CanUpdateAIInternal())
        return;

    UpdateAIInternal(elapsed);
    YieldAIInternalThread();
}

void PlayerbotAIBase::SetAIInternalUpdateDelay(const uint32 delay)
{
    if (aiInternalUpdateDelay < delay)
        sLog.outDebug("Setting lesser ai internal update delay %d -> %d", aiInternalUpdateDelay, delay);

    aiInternalUpdateDelay = delay;

    if (aiInternalUpdateDelay > sPlayerbotAIConfig.globalCoolDown)
        sLog.outDebug( "Set ai internal update delay: %d", aiInternalUpdateDelay);
}

void PlayerbotAIBase::IncreaseAIInternalUpdateDelay(uint32 delay)
{
    aiInternalUpdateDelay += delay;

    if (aiInternalUpdateDelay > sPlayerbotAIConfig.globalCoolDown)
        sLog.outDebug( "Increase ai internal update delay: %d", aiInternalUpdateDelay);
}

void PlayerbotAIBase::YieldAIInternalThread(bool minimal)
{
    if (aiInternalUpdateDelay < sPlayerbotAIConfig.reactDelay)
        aiInternalUpdateDelay = minimal ? sPlayerbotAIConfig.reactDelay * 10 : sPlayerbotAIConfig.reactDelay;
}

void PlayerbotAIBase::QueueMapTask(std::function<void(Player*)> task)
{
    std::lock_guard<std::mutex> guard(mapTaskMutex);
    mapTasks.push_back(std::move(task));
    hasMapTasks = true;
}

void PlayerbotAIBase::RunMapTasks(Player* player)
{
    if (!hasMapTasks || !player)
        return;

    // During a teleport the player is between maps; wait until they arrive.
    if (!player->IsInWorld() || player->IsBeingTeleported())
        return;

    std::vector<std::function<void(Player*)>> tasks;
    {
        std::lock_guard<std::mutex> guard(mapTaskMutex);
        tasks.swap(mapTasks);
        hasMapTasks = false;
    }

    for (auto& task : tasks)
        task(player);
}

bool PlayerbotAIBase::IsActive() const
{
    return (int)aiInternalUpdateDelay < (int)sPlayerbotAIConfig.maxWaitForMove;
}
