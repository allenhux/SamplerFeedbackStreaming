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
#include "SynchronizationUtils.h"
#include "Timer.h"

struct SFSResource;

//=============================================================================
// a thread to process feedback (when available) and queue tile loads / evictions to datauploader
//=============================================================================
namespace SFS
{
    class ManagerPFT;
    class ResourceBase;
    class DataUploader;

    class GroupRemoveResources : public LockedContainer<std::set<ResourceBase*>>
    {
    public:
        enum Client : UINT32
        {
            Initialize = 1 << 0,
            ProcessFeedbackThread = 1 << 1,
            ResidencyThread = 1 << 2
        };
        UINT32 GetFlags() const { return m_flags; }
        void ClearFlag(Client c) { m_flags &= ~c; }
        void SetFlag(Client c) { m_flags |= c; }

        // the lock is between app/main thread and PFT/Residency threads
        // PFT and Residency maintain coherency via m_flags, and can safely bypass the lock
        auto& BypassLockGetValues() { return m_values; }
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

        void ShareNewResources(std::vector<ResourceBase*> in_resources);

        // transfers and clears the vector (call with std::move())
        void SharePendingResources(std::set<ResourceBase*> in_resources);

        // indicate resources should be flushed
        // in_resources will be cleared
        void ShareFlushResources(const std::vector<SFSResource*>& in_resources, HANDLE in_event);

        // called by SFSManager for Residency thread constructor
        GroupRemoveResources& GetFlushResources() { return m_flushResources; }

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

        // resources with any pending work, including evictions scheduled multiple frames later
        std::list<ResourceBase*> m_delayedResources;

        // resources that need tiles loaded/evicted asap
        std::set<ResourceBase*> m_pendingResources;

        // new resources to be shared with ProcessFeedbackThread
        LockedContainer<std::vector<ResourceBase*>> m_newResourcesStaging;

        // resources that have feedback queued
        LockedContainer<std::set<ResourceBase*>> m_pendingResourceStaging;

        // Resources to delete. Verify other threads (Residency, DataUploader) aren't using them first.
        // this is shared with ResidencyThread
        GroupRemoveResources m_flushResources;
        HANDLE m_flushResourcesEvent{ nullptr };

        // sleep until this is set:
        SFS::SynchronizationFlag m_processFeedbackFlag;

        RawCpuTimer m_cpuTimer;
        std::atomic<UINT64> m_processFeedbackTime{ 0 }; // sum of cpu timer times since start

        std::atomic<bool> m_threadRunning{ false };

        std::atomic<UINT> m_numTotalSubmits{ 0 };

        const UINT m_minNumUploadRequests{ 2000 }; // heuristic to reduce Submit()s
        void SignalFileStreamer();
        void CheckFlushResources();
        void ProcessFeedback(UINT64 in_frameFenceValue);
    };
}
