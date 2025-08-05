//==============================================================
// Copyright © Intel Corporation
//
// SPDX-License-Identifier: MIT
// =============================================================

#pragma once

// set to enable unmapping of evicted tiles.
// reduces bandwidth 25-30% (bad), can decrease or increase latency (+10% to -30%)
#define ENABLE_UNMAP 0

#include <dstorage.h> // single creation point for factory. internal q for packed mips.

#include "UpdateList.h"
#include "MappingUpdater.h"
#include "FileStreamer.h"
#include "Timer.h"

#include "SimpleAllocator.h"
#include "SynchronizationUtils.h"

//=============================================================================
// Internal class that uploads texture data into a reserved resource
//=============================================================================
namespace SFS
{
    class ResourceDU;
    class ResourceBase;
    class ManagerDU; // for lock on ResidencyMap

    class DataUploader
    {
    public:
        DataUploader(
			ManagerDU* in_pSFSManager,
            class GroupFlushResources& in_gfr,
            ID3D12Device* in_pDevice,
            UINT in_maxCopyBatches,                     // maximum number of batches
            UINT in_stagingBufferSizeMB,                // upload buffer size
            UINT in_maxTileMappingUpdatesPerApiCall,    // some HW/drivers seem to have a limit
            int in_threadPriority
        );
        ~DataUploader();

        FileHandle* OpenFile(const std::wstring& in_path) const { return m_pFileStreamer->OpenFile(in_path); }
 
        // wait for all outstanding commands to complete. 
        void FlushCommands();

        ID3D12CommandQueue* GetMappingQueue() const { return m_mappingCommandQueue.Get(); }

        UINT GetNumUpdateListsAvailable() const { return m_updateListAllocator.GetWritableCount(); }

        // may return null. called by SFSResource.
        UpdateList* AllocateUpdateList(ResourceDU* in_pStreamingResource);

        // SFSResource requests tiles to be uploaded
        void SubmitUpdateList(SFS::UpdateList& in_updateList);

        // SFSM requests file streamer to signal its fence after StreamingResources have queued tile uploads
        void SignalFileStreamer() { m_pFileStreamer->Signal(); }

        enum class StreamerType
        {
            Reference,
            DirectStorage
        };
        SFS::FileStreamer* SetStreamer(StreamerType in_streamerType, bool in_traceCaptureMode);

        const auto& GetUpdateLists() const { return m_updateLists; }

		void CheckFlushResources() { m_fenceMonitorFlag.Set(); } // wake the fence monitor thread to check for flushed resources

        // ProcessFeedbackThread calls this to notify that a resource's residency has changed
        void AddResidencyChanged(std::set<ResourceBase*> in_resources)
        {
            if (0 == m_residencyChangedStaging.Size())
            {
                m_residencyChangedStaging.Swap(in_resources);
            }
            else
            {
                m_residencyChangedStaging.Acquire().merge(in_resources);
                m_residencyChangedStaging.Release();
            }
            m_fenceMonitorFlag.Set();
        }

        //----------------------------------
        // statistics and visualization
        //----------------------------------
        float GetTotalTileCopyLatencyMs() const { return m_fenceThreadTimer.GetMsFromDelta(m_totalTileCopyLatency); } // sum of per-tile latencies so far
        void SetVisualizationMode(UINT in_mode) { m_pFileStreamer->SetVisualizationMode(in_mode); }
        void CaptureTraceFile(bool in_captureTrace) { m_pFileStreamer->CaptureTraceFile(in_captureTrace); }
    private:
        // back-channel for ProcessFeedbackThread to notify residency changes
        LockedContainer<std::set<ResourceBase*>> m_residencyChangedStaging;

        // upload buffer size. Required to allow dynamic changing of file streamer
        const UINT m_stagingBufferSizeMB{ 0 };

        // fence to monitor forward progress of the mapping queue. independent of the frame queue
        ComPtr<ID3D12Fence> m_mappingFence;
        UINT64 m_mappingFenceValue{ 0 };
        // copy queue just for mapping UpdateTileMappings() on reserved resource
        ComPtr<ID3D12CommandQueue> m_mappingCommandQueue;

        // pool of all updatelists
        std::vector<UpdateList> m_updateLists;
        class AllocatorUINT : public AllocatorMT<UINT>
        {
        public:
            AllocatorUINT(UINT n) : AllocatorMT(n)
            {
                for (UINT i = 0; i < n; i++) { m_values[i] = i; }
            }
            ~AllocatorUINT()
            {
#ifdef _DEBUG
                ASSERT(0 == GetReadableCount());
                // verify all indices accounted for and unique
                std::sort(m_values.begin(), m_values.end());
                for (UINT i = 0; i < (UINT)m_values.size(); i++)
                {
                    ASSERT(i == m_values[i]);
                }
#endif
            }
        };
        AllocatorUINT m_updateListAllocator;

        // only the fence thread (which looks for final completion) frees UpdateLists
        void FreeUpdateList(SFS::UpdateList& in_updateList);

        // object that performs UpdateTileMappings() requests
        SFS::MappingUpdater m_mappingUpdater;

        // object that knows how to take data from disk and upload to gpu
        std::unique_ptr<SFS::FileStreamer> m_pFileStreamer;

        // thread to handle UpdateTileMappings calls (at one time a major performance bottleneck)
        void MappingThread();
        std::thread m_mappingThread;
        SFS::SynchronizationFlag m_mappingFlag; // sleeps until flag set
        AllocatorMT<UpdateList*> m_mappingTasks;

        // thread to poll copy and mapping fences
        // this thread could have been designed using WaitForMultipleObjects, but it was found that SetEventOnCompletion() was expensive in a tight thread loop
        // compromise solution is to keep this thread awake so long as there are live UpdateLists.
        void FenceMonitorThread();
        std::thread m_fenceMonitorThread;
        SFS::SynchronizationFlag m_fenceMonitorFlag; // sleeps until flag set
        HANDLE m_fenceEvents[2]{};
        CpuTimer m_fenceThreadTimer;
        AllocatorMT<UpdateList*> m_monitorTasks;

        void StartThreads();
        void StopThreads();
        std::atomic<bool> m_threadsRunning{ false };
        const int m_threadPriority{ 0 };

        // DS memory queue used just for loading packed mips
        // separate memory queue means needing a second fence - can't wait across DS queues
        void InitDirectStorage();
        ComPtr<IDStorageFactory> m_dsFactory;

        ManagerDU* const m_pSFSManager{ nullptr };
        class GroupFlushResources& m_flushResources;

        //-------------------------------------------
        // statistics
        //-------------------------------------------
        std::atomic<UINT> m_numTotalUpdateListsProcessed{ 0 };
        std::atomic<INT64> m_totalTileCopyLatency{ 0 }; // total approximate latency for all copies. divide by m_numTotalUploads then get the time with m_cpuTimer.GetSecondsFromDelta() 
    };
}
