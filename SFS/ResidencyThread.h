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
        void ShareNewResourcesRT(const std::vector<ResourceBase*>& in_resources);
    private:
        ManagerRT* const m_pSFSManager;
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

        GroupRemoveResources& m_removeResources;
    };
}
