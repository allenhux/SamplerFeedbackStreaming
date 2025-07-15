//==============================================================
// Copyright © Intel Corporation
//
// SPDX-License-Identifier: MIT
// =============================================================

//=============================================================================
// a thread to update residency maps as a result of feedback
//=============================================================================

#pragma once

#include <vector>
#include <thread>
#include <set>
#include <atomic>
#include "Streaming.h"
#include "SynchronizationUtils.h"

namespace SFS
{
    class ManagerRT;
    class ResourceBase;

    class ResidencyThread
    {
    public:
        ResidencyThread(ManagerRT* in_pSFSManager, class GroupRemoveResources& in_grr,int in_threadPriority);

        void Start();
        void Stop();

        void Wake() { m_residencyChangedFlag.Set(); }

        // blocking lock
        void SharePendingResourcesRT(std::set<ResourceBase*> in_resources);
    private:
        ManagerRT* const m_pSFSManager;
        const int m_threadPriority;

        std::thread m_thread;

        std::atomic<bool> m_threadRunning{ false };
        SFS::SynchronizationFlag m_residencyChangedFlag;

        LockedContainer<std::set<ResourceBase*>> m_resourcesStaging;

        // working set of streaming resources
        std::set<ResourceBase*> m_resources;

        GroupRemoveResources& m_flushResources;
    };
}
