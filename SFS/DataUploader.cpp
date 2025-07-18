//==============================================================
// Copyright © Intel Corporation
//
// SPDX-License-Identifier: MIT
// =============================================================

#include "pch.h"

#include "DataUploader.h"
#include "ResourceDU.h"
#include "FileStreamerReference.h"
#include "FileStreamerDS.h"
#include "SFSHeap.h"
#include "ProcessFeedbackThread.h"

//=============================================================================
// Internal class that uploads texture data into a reserved resource
//=============================================================================
SFS::DataUploader::DataUploader(
    ID3D12Device* in_pDevice,
    UINT in_maxCopyBatches,                  // maximum number of batches
    UINT in_stagingBufferSizeMB,             // upload buffer size
    UINT in_maxTileMappingUpdatesPerApiCall, // some HW/drivers seem to have a limit
    int in_threadPriority) :
    m_updateLists(in_maxCopyBatches)
    , m_updateListAllocator(in_maxCopyBatches)
    , m_stagingBufferSizeMB(in_stagingBufferSizeMB)
    , m_mappingUpdater(in_maxTileMappingUpdatesPerApiCall)
    , m_threadPriority(in_threadPriority)
    , m_mappingTasks(in_maxCopyBatches)
    , m_monitorTasks(in_maxCopyBatches)
{
    // copy queue just for UpdateTileMappings() on reserved resources
    {
        D3D12_COMMAND_QUEUE_DESC queueDesc = {};
        queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
        queueDesc.Type = D3D12_COMMAND_LIST_TYPE_COPY;
        ThrowIfFailed(in_pDevice->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&m_mappingCommandQueue)));
        m_mappingCommandQueue->SetName(L"DataUploader::m_mappingCommandQueue");

        // fence exclusively for mapping command queue
        ThrowIfFailed(in_pDevice->CreateFence(m_mappingFenceValue, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_mappingFence)));
        m_mappingFence->SetName(L"DataUploader::m_mappingFence");
        m_mappingFenceValue++;
    }

    // create events for m_fenceMonitorThread
    for (UINT i = 0; i < sizeof(m_fenceEvents) / sizeof(m_fenceEvents[0]); i++)
    {
        m_fenceEvents[i] = ::CreateEvent(nullptr, FALSE, FALSE, nullptr);
        if (m_fenceEvents[i] == nullptr)
        {
            ThrowIfFailed(HRESULT_FROM_WIN32(GetLastError()));
        }
    }

    InitDirectStorage(in_pDevice);

    //NOTE: SFSManager must call SetStreamer() to start streaming
    //SetStreamer(StreamerType::Reference);
}

SFS::DataUploader::~DataUploader()
{
    // stop updating. all StreamingResources must have been destroyed already, presumably.
    StopThreads();
    for (UINT i = 0; i < sizeof(m_fenceEvents) / sizeof(m_fenceEvents[0]); i++)
    {
        ::CloseHandle(m_fenceEvents[i]);
    }
}

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
void SFS::DataUploader::InitDirectStorage(ID3D12Device* in_pDevice)
{
    // initialize to default values
    DSTORAGE_CONFIGURATION dsConfig{};
    DStorageSetConfiguration(&dsConfig);

    ThrowIfFailed(DStorageGetFactory(IID_PPV_ARGS(&m_dsFactory)));

    DSTORAGE_DEBUG debugFlags = DSTORAGE_DEBUG_NONE;
#ifdef _DEBUG
    debugFlags = DSTORAGE_DEBUG_SHOW_ERRORS;
#endif
    m_dsFactory->SetDebugFlags(debugFlags);

    m_dsFactory->SetStagingBufferSize(m_stagingBufferSizeMB * 1024 * 1024);

    DSTORAGE_QUEUE_DESC queueDesc{};
    queueDesc.Capacity = DSTORAGE_MAX_QUEUE_CAPACITY;
    queueDesc.Priority = DSTORAGE_PRIORITY_NORMAL;
    queueDesc.SourceType = DSTORAGE_REQUEST_SOURCE_MEMORY;
    queueDesc.Device = in_pDevice;
    ThrowIfFailed(m_dsFactory->CreateQueue(&queueDesc, IID_PPV_ARGS(&m_memoryQueue)));

    ThrowIfFailed(in_pDevice->CreateFence(m_memoryFenceValue, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_memoryFence)));
    m_memoryFenceValue++;
}

//-----------------------------------------------------------------------------
// handle request to load a texture from cpu memory
// used for packed mips, which don't participate in fine-grained streaming
// FIXME: dead and busted.
//-----------------------------------------------------------------------------
void SFS::DataUploader::LoadTextureFromMemory(SFS::UpdateList& out_updateList)
{
    ASSERT(0);
    UpdateList::PackedMip packedMip{ .m_coord = out_updateList.m_coords[0] };

    DSTORAGE_REQUEST request = {};
    request.Options.SourceType = DSTORAGE_REQUEST_SOURCE_MEMORY;
    request.Options.DestinationType = DSTORAGE_REQUEST_DESTINATION_MULTIPLE_SUBRESOURCES;
    request.Source.Memory.Source = 0; // FIXME
    request.Source.Memory.Size = packedMip.m_mipInfo.numBytes;
    request.UncompressedSize = packedMip.m_mipInfo.uncompressedSize;
    request.Destination.MultipleSubresources.Resource = out_updateList.m_pResource->GetTiledResource();
    request.Destination.MultipleSubresources.FirstSubresource = out_updateList.m_pResource->GetPackedMipsFirstSubresource();
    request.Options.CompressionFormat = (DSTORAGE_COMPRESSION_FORMAT)out_updateList.m_pResource->GetCompressionFormat();

    out_updateList.m_copyFenceValue = m_memoryFenceValue;
    m_memoryQueue->EnqueueRequest(&request);
}

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
void SFS::DataUploader::SubmitTextureLoadsFromMemory()
{
    m_memoryQueue->EnqueueSignal(m_memoryFence.Get(), m_memoryFenceValue);
    m_memoryQueue->Submit();
    m_memoryFenceValue++;
}

//-----------------------------------------------------------------------------
// releases ownership of and returns the old streamer
// calling function may need to delete some other resources before deleting the streamer
//-----------------------------------------------------------------------------
SFS::FileStreamer* SFS::DataUploader::SetStreamer(StreamerType in_streamerType, bool in_traceCaptureMode)
{
    StopThreads();

    ComPtr<ID3D12Device> device;
    m_mappingCommandQueue->GetDevice(IID_PPV_ARGS(&device));

    SFS::FileStreamer* pOldStreamer = m_pFileStreamer.release();

    if (StreamerType::Reference == in_streamerType)
    {
        // buffer size in megabytes * 1024 * 1024 bytes / (tile size = 64 * 1024 bytes)
        UINT maxTileCopiesInFlight = m_stagingBufferSizeMB * (1024 / 64);

        m_pFileStreamer = std::make_unique<SFS::FileStreamerReference>(device.Get(),
            (UINT)m_updateLists.size(), maxTileCopiesInFlight);
    }
    else
    {
        m_pFileStreamer = std::make_unique<SFS::FileStreamerDS>(device.Get(),
            m_dsFactory.Get(), in_traceCaptureMode);
    }

    StartThreads();

    return pOldStreamer;
}

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
void SFS::DataUploader::StartThreads()
{
    // launch notify thread
    ASSERT(false == m_threadsRunning);
    m_threadsRunning = true;

    m_mappingThread = std::thread([&]
        {
            while (m_threadsRunning)
            {
                m_mappingFlag.Wait();
                MappingThread();
            }
        });

    // launch thread to monitor fences
    m_fenceMonitorThread = std::thread([&]
        {
            while (m_threadsRunning)
            {
                FenceMonitorThread();
            }
        });

    SFS::SetThreadPriority(m_mappingThread, m_threadPriority);
    SFS::SetThreadPriority(m_fenceMonitorThread, m_threadPriority);
}

void SFS::DataUploader::StopThreads()
{
    FlushCommands();

    if (m_threadsRunning)
    {
        m_threadsRunning = false;

        // wake up threads so they can exit
        m_mappingFlag.Set();
        m_fenceMonitorFlag.Set();

        // stop submitting new work
        if (m_mappingThread.joinable())
        {
            m_mappingThread.join();
        }

        // finish up any remaining work
        if (m_fenceMonitorThread.joinable())
        {
            m_fenceMonitorThread.join();
        }
    }
}

//-----------------------------------------------------------------------------
// wait for all pending commands to complete, at which point all queues will be drained
//-----------------------------------------------------------------------------
void SFS::DataUploader::FlushCommands()
{
    if (m_updateListAllocator.GetReadableCount())
    {
        DebugPrint("DataUploader waiting on ", m_updateListAllocator.GetReadableCount(), " tasks to complete\n");
        while (m_updateListAllocator.GetReadableCount()) // wait so long as there is outstanding work
        {
            m_mappingFlag.Set(); // (paranoia)
            m_fenceMonitorFlag.Set(); // (paranoia)
            _mm_pause();
        }
    }
    // if this loop doesn't exit, then a race condition occurred while allocating/freeing updatelists

#ifdef _DEBUG
    for (auto& u : m_updateLists)
    {
        ASSERT(UpdateList::State::STATE_FREE == u.m_executionState);
    }
#endif

    // NOTE: all copy and mapping queues must be empty if the UpdateLists have notified
}

//-----------------------------------------------------------------------------
// tries to find an available UpdateList, may return null
//-----------------------------------------------------------------------------
SFS::UpdateList* SFS::DataUploader::AllocateUpdateList(SFS::ResourceDU* in_pStreamingResource)
{
    UpdateList* pUpdateList = nullptr;

    if (m_updateListAllocator.GetWritableCount())
    {
        pUpdateList = &m_updateLists[m_updateListAllocator.GetWriteableValue()];
        m_updateListAllocator.Commit();
        ASSERT(UpdateList::State::STATE_FREE == pUpdateList->m_executionState);

        pUpdateList->Reset(in_pStreamingResource);
        in_pStreamingResource->AddUpdateList(); // increment resource's count of in-flight updatelists
        pUpdateList->m_executionState = UpdateList::State::STATE_ALLOCATED;
    }

    return pUpdateList;
}

//-----------------------------------------------------------------------------
// return UpdateList to free state
//-----------------------------------------------------------------------------
void SFS::DataUploader::FreeUpdateList(SFS::UpdateList& in_updateList)
{
    // NOTE: updatelist is deliberately not cleared until after allocation
    // otherwise there can be a race with the mapping thread
    in_updateList.m_executionState = UpdateList::State::STATE_FREE;

    // return the index to this updatelist to the pool
    UINT i = UINT(&in_updateList - m_updateLists.data());
    m_updateListAllocator.GetReadableValue() = i;
    m_updateListAllocator.Free();
}

//-----------------------------------------------------------------------------
// called from ProcessFeedbackThread to request streaming and mapping
//-----------------------------------------------------------------------------
void SFS::DataUploader::SubmitUpdateList(SFS::UpdateList& in_updateList)
{
    ASSERT(UpdateList::State::STATE_ALLOCATED == in_updateList.m_executionState);

    // set to submitted, allowing mapping within MappingThread
    // fenceMonitorThread will wait for the copy fence to become valid before progressing state
    in_updateList.m_executionState = UpdateList::State::STATE_SUBMITTED;

    if (in_updateList.GetNumStandardUpdates())
    {
        in_updateList.m_copyLatencyTimer = m_fenceThreadTimer.GetTicks();
        m_pFileStreamer->StreamTexture(in_updateList);
    }

    // add to submit task queue
    {
        m_mappingTasks.GetWriteableValue() = &in_updateList;
        m_mappingTasks.Commit();
        m_mappingFlag.Set();
    }

    // add to fence polling thread
    {
        m_monitorTasks.GetWriteableValue() = &in_updateList;
        m_monitorTasks.Commit();
        m_fenceMonitorFlag.Set();
    }
}

//-----------------------------------------------------------------------------
// check necessary fences to determine completion status
// possibilities:
// 1. packed tiles, submitted state, mapping done, move to uploading state
// 2. packed tiles, copy pending state, copy complete
// 3. standard tiles, copy pending state, mapping started and complete, copy complete
// 4. no tiles, mapping started and complete
// all cases: state > allocated
//-----------------------------------------------------------------------------
void SFS::DataUploader::FenceMonitorThread()
{
    // if no outstanding work, sleep
    // m_updateListAllocator.GetReadableCount() would be a more conservative test for whether there are any active tasks
    if (0 == m_monitorTasks.GetReadableCount())
    {
        m_fenceMonitorFlag.Wait();
    }

    bool loadPackedMips = false;

    UINT64 mappingFenceValue = m_mappingFence->GetCompletedValue();
    UINT64 copyFenceValue = m_pFileStreamer->GetCompletedValue();
    for (UINT i = 0; i < m_monitorTasks.GetReadableCount();)
    {
        auto& updateList = *m_monitorTasks.GetReadableValue(i);

        ASSERT(UpdateList::State::STATE_FREE != updateList.m_executionState);

        bool freeUpdateList = false;

        switch (updateList.m_executionState)
        {

        case UpdateList::State::STATE_PACKED_MAPPING:
            ASSERT(0 == updateList.GetNumStandardUpdates());
            ASSERT(0 == updateList.GetNumEvictions());

            // wait for mapping complete before streaming packed tiles
            if (m_mappingFence->GetCompletedValue() >= updateList.m_mappingFenceValue)
            {
                updateList.m_pResource->LoadPackedMipInfo(updateList);
                m_pFileStreamer->StreamPackedMips(updateList);

                loadPackedMips = true;  // set flag so we signal fence below
                updateList.m_executionState = UpdateList::State::STATE_PACKED_INITIALIZE;
            }
            break; // give other resources a chance to start streaming

        case UpdateList::State::STATE_PACKED_INITIALIZE:
            updateList.m_executionState = UpdateList::State::STATE_PACKED_COPY_PENDING;
            [[fallthrough]];

        case UpdateList::State::STATE_PACKED_COPY_PENDING:
            ASSERT(1 == updateList.GetNumStandardUpdates());
            ASSERT(0 == updateList.GetNumEvictions());

            if ((updateList.m_copyFenceValid) && (m_pFileStreamer->GetCompletedValue() >= updateList.m_copyFenceValue))
            {
                updateList.m_pResource->NotifyPackedMips();
                freeUpdateList = true;
            }
            break;

        case UpdateList::State::STATE_UPLOADING:
            ASSERT(0 != updateList.GetNumStandardUpdates());

            // only check copy fence if the fence has been set (avoid race condition)
            if ((updateList.m_copyFenceValid) && (m_pFileStreamer->GetCompletedValue() >= updateList.m_copyFenceValue))
            {
                updateList.m_executionState = UpdateList::State::STATE_MAP_PENDING;
            }
            else
            {
                break;
            }
            [[fallthrough]];

        case UpdateList::State::STATE_MAP_PENDING:
            if (m_mappingFence->GetCompletedValue() >= updateList.m_mappingFenceValue)
            {
#if 0
                // NOTE: dead code. currently not un-mapping tiles

                // notify evictions
                if (updateList.GetNumEvictions())
                {
                    updateList.m_pResource->NotifyEvicted(updateList.m_evictCoords);

                    m_numTotalEvictions.fetch_add(updateList.GetNumEvictions(), std::memory_order_relaxed);
                }
#endif
                // notify regular tiles
                if (updateList.GetNumStandardUpdates())
                {
                    updateList.m_pResource->NotifyCopyComplete(updateList.m_coords);

                    auto updateLatency = m_fenceThreadTimer.GetTicks() - updateList.m_copyLatencyTimer;
                    m_totalTileCopyLatency.fetch_add(updateLatency, std::memory_order_relaxed);
                    m_numTotalUploads.fetch_add(updateList.GetNumStandardUpdates(), std::memory_order_relaxed);
                }

                freeUpdateList = true;
            }
        break;

        default:
            break;
        }

        if (freeUpdateList)
        {
            m_monitorTasks.FreeByIndex(i);
            FreeUpdateList(updateList);
        }
        else
        {
            i++;
        }
    } // end loop over updatelists

    if (loadPackedMips)
    {
        // DS Filestreamer may need a nudge if small amounts of data to move
        m_pFileStreamer->Signal();
    }

    // if still have tasks but fences haven't advanced, can sleep for a bit
    if ((m_monitorTasks.GetReadableCount()) &&
        (m_mappingFence->GetCompletedValue() == mappingFenceValue) &&
        (m_pFileStreamer->GetCompletedValue() == copyFenceValue))
    {
        ThrowIfFailed(m_mappingFence->SetEventOnCompletion(mappingFenceValue + 1, m_fenceEvents[0]));
        m_pFileStreamer->SetEventOnCompletion(copyFenceValue + 1, m_fenceEvents[1]);
        // wait for a bit. expect signal soon.
        WaitForMultipleObjects(2, m_fenceEvents, false, 180);
    }

}

//-----------------------------------------------------------------------------
// Submit Thread
// On submission, all updatelists need mapping
// set next state depending on the task
// NOTE: if UpdateTileMappings is slow, throughput will be impacted
//-----------------------------------------------------------------------------
void SFS::DataUploader::MappingThread()
{
    // under heavy load, can enter a state where all ULs are allocated - at which point
    //     they all get a single fence. In the meantime, no new loads are started.
    // preferably, under heavy load, this loop never exits: new ULs arrive as old ULs complete
    constexpr UINT mapLimit = 32; // FIXME: what is an ideal # before we signal the fence?
    UINT numMaps = 0;

    // look through tasks
    while (m_mappingTasks.GetReadableCount())
    {
        numMaps++;

        auto& updateList = *m_mappingTasks.GetReadableValue(); // get the next task
        m_mappingTasks.Free(); // consume this task. not FREE yet, just no longer tracking in this thread

        ASSERT(UpdateList::State::STATE_SUBMITTED == updateList.m_executionState);

        // set to the fence value to be signaled next
        updateList.m_mappingFenceValue = m_mappingFenceValue;

#if 0
        // NOTE: dead code. currently not un-mapping tiles

        // unmap tiles that are being evicted
        if (updateList.GetNumEvictions())
        {
            m_mappingUpdater.UnMap(GetMappingQueue(), updateList.m_pResource->GetTiledResource(), updateList.m_evictCoords);

            // this will skip the uploading state unless there are uploads
            updateList.m_executionState = UpdateList::State::STATE_MAP_PENDING;
        }
#endif

        // map standard tiles
        // can upload and evict in a single UpdateList
        if (updateList.GetNumStandardUpdates())
        {
            m_mappingUpdater.Map(GetMappingQueue(),
                updateList.m_pResource->GetTiledResource(),
                updateList.m_pResource->GetHeap()->GetHeap(),
                updateList.m_coords, updateList.m_heapIndices);

            updateList.m_executionState = UpdateList::State::STATE_UPLOADING;
        }

        // no uploads or evictions? must be mapping packed mips
        else if (0 == updateList.GetNumEvictions())
        {
            updateList.m_pResource->MapPackedMips(GetMappingQueue());

            updateList.m_executionState = UpdateList::State::STATE_PACKED_MAPPING;
        }

        if (mapLimit <= numMaps)
        {
            numMaps = 0;
            m_mappingCommandQueue->Signal(m_mappingFence.Get(), m_mappingFenceValue);
            m_mappingFenceValue++;
        }
    }

    if (numMaps)
    {
        m_mappingCommandQueue->Signal(m_mappingFence.Get(), m_mappingFenceValue);
        m_mappingFenceValue++;
    }
}
