//==============================================================
// Copyright © Intel Corporation
//
// SPDX-License-Identifier: MIT
// =============================================================

/*=============================================================================
Thread to interpret sampler feedback and generate tile loads/evictions
=============================================================================*/

#pragma once

#include <vector>
#include <thread>
#include <set>
#include <atomic>
#include "Streaming.h"
#include "Timer.h"

//=============================================================================
// a thread to process feedback (when available) and queue tile loads / evictions to datauploader
//=============================================================================
namespace SFS
{
    class ManagerPFT;
    class ResourceBase;
    class DataUploader;

    class GroupRemoveResources : public std::set<ResourceBase*>, public Lock
    {
    public:
        enum Client : UINT32
        {
            Initialize = 1 << 0,
            ProcessFeedback = 1 << 1,
            Residency = 1 << 2
        };
        UINT32 GetFlags() const { return m_flags; }
        void ClearFlag(Client c) { m_flags &= ~c; }
        void SetFlag(Client c) { m_flags |= c; }
    private:
        std::atomic<UINT32> m_flags;
    };

    class ProcessFeedbackThread
    {
    public:
        ProcessFeedbackThread(ManagerPFT* in_pSFSManager, DataUploader& in_dataUploader,
            UINT in_minNumUploadRequests, int in_threadPriority);
        ~ProcessFeedbackThread();

        void Start();
        void Stop();

        void Wake() { m_processFeedbackFlag.Set(); }

        void ShareNewResources(const std::vector<ResourceBase*>& in_resources);
        void SharePendingResources(const std::vector<ResourceBase*>& in_resources);

        // attempts to indicate resources should be destroyed
        // may not succeed if resources are already in the process of being deleted
        // on success, in_resources will be cleared
        void AsyncDestroyResources(std::set<ResourceBase*>& in_resources);

        // called by SFSManager for Residency thread constructor
        GroupRemoveResources& GetRemoveResources() { return m_removeResources; }

        UINT GetTotalNumSubmits() const { return m_numTotalSubmits; }
        UINT64 GetTotalProcessTime() const { return m_processFeedbackTime; }
        float GetSecondsFromDelta(INT64 d) { return m_cpuTimer.GetSecondsFromDelta(d); }
    private:
        ManagerPFT* const m_pSFSManager;
        DataUploader& m_dataUploader;
        const int m_threadPriority;

        std::thread m_thread;

        // new resources are prioritized until packed mips are in-flight
        std::vector<ResourceBase*> m_newResources;

        // resources to be shared with residency thread (only after sufficient init, in case they are quickly deleted)
        std::vector<ResourceBase*> m_residencyShareNewResources;

        // resources with any pending work, including evictions scheduled multiple frames later
        std::set<ResourceBase*> m_activeResources;

        // resources that need tiles loaded/evicted asap
        std::set<ResourceBase*> m_pendingResources;

        std::vector<ResourceBase*> m_newResourcesStaging; // resources to be shared with ProcessFeedbackThread
        Lock m_newResourcesLock;   // lock between ProcessFeedbackThread and main thread

        std::vector<ResourceBase*> m_pendingResourceStaging; // resources to be shared with ProcessFeedbackThread
        Lock m_pendingLock;   // lock between ProcessFeedbackThread and main thread

        std::vector<ResourceBase*> m_removeResourcesStaging; // resources to be deleted
        Lock m_removeStagingLock;   // lock between ProcessFeedbackThread and main thread

        // Resources to delete. Verify other threads (Residency, DataUploader) aren't using them first.
        GroupRemoveResources m_removeResources;

        // sleep until this is set:
        SFS::SynchronizationFlag m_processFeedbackFlag;

        RawCpuTimer m_cpuTimer;
        std::atomic<INT64> m_processFeedbackTime{ 0 }; // sum of cpu timer times since start

        std::atomic<bool> m_threadRunning{ false };

        std::atomic<UINT> m_numTotalSubmits{ 0 };

        const UINT m_minNumUploadRequests{ 2000 }; // heuristic to reduce Submit()s
        void SignalFileStreamer();
        void CheckRemoveResources();
    };
}
