#pragma once

class Player;
class PlayerbotMgr;
class ChatHandler;
class PerformanceMonitorOperation;

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

class PlayerbotAIBase
{
public:
    PlayerbotAIBase();

public:
    bool IsActive() const;
    virtual void UpdateAI(uint32 elapsed);
    
    uint32 GetAIInternalUpdateDelay() const { return aiInternalUpdateDelay; }

    // Queues work on the character this AI or manager belongs to, to run on that character's map thread at its next
    // AI tick (RunMapTasks). Thread-safe, so console commands on the world thread can hand work to a map.
    void QueueMapTask(std::function<void(Player*)> task);

protected:
    virtual void UpdateAIInternal(uint32 elapsed, bool minimal = false);
    bool CanUpdateAIInternal() const { return aiInternalUpdateDelay < 100U; }
    void SetAIInternalUpdateDelay(const uint32 delay);
    void ResetAIInternalUpdateDelay() { aiInternalUpdateDelay = 0U; }
    void IncreaseAIInternalUpdateDelay(uint32 delay);
    void YieldAIInternalThread(bool minimal = false);

    // Runs the tasks QueueMapTask queued, with player, once player is in the world and not between maps; until then
    // they stay queued. Call it only from the player's AI tick, which runs on the player's map thread.
    void RunMapTasks(Player* player);
    
protected:
	uint32 aiInternalUpdateDelay;

    std::unique_ptr<PerformanceMonitorOperation> totalPmo;

private:
    std::mutex mapTaskMutex;
    std::vector<std::function<void(Player*)>> mapTasks;  // guarded by mapTaskMutex
    std::atomic<bool> hasMapTasks{false};                 // lets RunMapTasks skip the lock when nothing is queued
};
