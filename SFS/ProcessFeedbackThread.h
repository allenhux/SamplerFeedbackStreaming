//*********************************************************
//
// Copyright 2020 Intel Corporation 
//
// Permission is hereby granted, free of charge, to any 
// person obtaining a copy of this software and associated 
// documentation files(the "Software"), to deal in the Software 
// without restriction, including without limitation the rights 
// to use, copy, modify, merge, publish, distribute, sublicense, 
// and/or sell copies of the Software, and to permit persons to 
// whom the Software is furnished to do so, subject to the 
// following conditions :
// The above copyright notice and this permission notice shall 
// be included in all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, 
// EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF 
// MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
// NONINFRINGEMENT.IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT 
// HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, 
// WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, 
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER 
// DEALINGS IN THE SOFTWARE.
//
//*********************************************************


/*=============================================================================
Implementation of SFS Manager
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

        // PFT: initializes this, removes its own resources, then sends pointer to other threads
        // other threads: if (ptr != nullptr) remove resources, ClearFlag(self), ptr = nullptr
        // PFT: if (0==GetFlags()) delete the resources.
        GroupRemoveResources m_removeResources;

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
