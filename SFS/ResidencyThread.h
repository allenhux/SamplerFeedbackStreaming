#pragma once

#include <vector>
#include <thread>
#include <set>
#include "Streaming.h"

//=============================================================================
// a thread to update residency maps as a result of feedback
//=============================================================================
namespace SFS
{
    class ManagerBase;
    class ResourceBase;

    class ResidencyThread
    {
    public:
        ResidencyThread(ManagerBase* in_pSFSManager, int in_threadPriority);

        void Start();
        void Stop();

        void Wake() { m_residencyChangedFlag.Set(); }

        // blocking lock
        void ShareNewResources(const std::vector<ResourceBase*>& in_resources);
        void RemoveResources(const std::set<ResourceBase*>& in_resources);
    private:
        ManagerBase* const m_pSFSManager;
        const int m_threadPriority;

        std::thread m_thread;

        // working set of streaming resources
        std::vector<ResourceBase*> m_streamingResources;

        std::atomic<bool> m_threadRunning{ false };
        SFS::SynchronizationFlag m_residencyChangedFlag;

        // staging area for new resources
        std::vector<ResourceBase*> m_newResourcesStaging;
        // lock around staging area for new resources
        Lock m_newResourcesLock;
    };
}
