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

#include "pch.h"

#include "DataUploader.h"
#include "SFSResourceDU.h"
#include "FileStreamerReference.h"
#include "FileStreamerDS.h"
#include "SFSHeap.h"

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
    , m_submitTaskAlloc(in_maxCopyBatches), m_submitTasks(in_maxCopyBatches)
    , m_monitorTaskAlloc(in_maxCopyBatches), m_monitorTasks(in_maxCopyBatches)
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

    InitDirectStorage(in_pDevice);

    //NOTE: SFSManager must call SetStreamer() to start streaming
    //SetStreamer(StreamerType::Reference);
}

SFS::DataUploader::~DataUploader()
{
    // stop updating. all StreamingResources must have been destroyed already, presumably.
    StopThreads();
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
//-----------------------------------------------------------------------------
void SFS::DataUploader::LoadTextureFromMemory(SFS::UpdateList& out_updateList)
{
    UINT uncompressedSize = 0;
    auto& textureBytes = out_updateList.m_pResource->GetPaddedPackedMips(uncompressedSize);

    DSTORAGE_REQUEST request = {};
    request.Options.SourceType = DSTORAGE_REQUEST_SOURCE_MEMORY;
    request.Options.DestinationType = DSTORAGE_REQUEST_DESTINATION_MULTIPLE_SUBRESOURCES;
    request.Source.Memory.Source = textureBytes.data();
    request.Source.Memory.Size = (UINT32)textureBytes.size();
    request.UncompressedSize = uncompressedSize;
    request.Destination.MultipleSubresources.Resource = out_updateList.m_pResource->GetTiledResource();
    request.Destination.MultipleSubresources.FirstSubresource = out_updateList.m_pResource->GetPackedMipInfo().NumStandardMips;
    request.Options.CompressionFormat = (DSTORAGE_COMPRESSION_FORMAT)out_updateList.m_pResource->GetTextureFileInfo()->GetCompressionFormat();

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
SFS::FileStreamer* SFS::DataUploader::SetStreamer(StreamerType in_streamerType)
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
        m_pFileStreamer = std::make_unique<SFS::FileStreamerDS>(device.Get(), m_dsFactory.Get());
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

    m_submitThread = std::thread([&]
        {
            while (m_threadsRunning)
            {
                m_submitFlag.Wait();
                SubmitThread();
            }
        });

    // launch thread to monitor fences
    m_fenceMonitorThread = std::thread([&]
        {
            // initialize timer on the thread that will use it
            RawCpuTimer fenceMonitorThread;
            m_pFenceThreadTimer = &fenceMonitorThread;

            while (m_threadsRunning)
            {
                // if no outstanding work, sleep
                if (0 == m_updateListAllocator.GetAllocated())
                {
                    m_fenceMonitorFlag.Wait();
                }

                FenceMonitorThread();
            }
        });

    SFS::SetThreadPriority(m_submitThread, m_threadPriority);
    SFS::SetThreadPriority(m_fenceMonitorThread, m_threadPriority);
}

void SFS::DataUploader::StopThreads()
{
    FlushCommands();

    if (m_threadsRunning)
    {
        m_threadsRunning = false;

        // wake up threads so they can exit
        m_submitFlag.Set();
        m_fenceMonitorFlag.Set();

        // stop submitting new work
        if (m_submitThread.joinable())
        {
            m_submitThread.join();
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
    if (m_updateListAllocator.GetAllocated())
    {
        DebugPrint("DataUploader waiting on ", m_updateListAllocator.GetAllocated(), " tasks to complete\n");
        while (m_updateListAllocator.GetAllocated()) // wait so long as there is outstanding work
        {
            m_submitFlag.Set(); // (paranoia)
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

    if (m_updateListAllocator.GetAvailable())
    {
        UINT index = m_updateListAllocator.Allocate();
        pUpdateList = &m_updateLists[index];
        ASSERT(UpdateList::State::STATE_FREE == pUpdateList->m_executionState);

        pUpdateList->Reset(in_pStreamingResource);
        pUpdateList->m_executionState = UpdateList::State::STATE_ALLOCATED;

        // start fence polling thread now
        {
            m_monitorTasks[m_monitorTaskAlloc.GetWriteIndex()] = pUpdateList;
            m_monitorTaskAlloc.Allocate();
            m_fenceMonitorFlag.Set();
        }
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
    m_updateListAllocator.Free(i);
}

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
void SFS::DataUploader::SubmitUpdateList(SFS::UpdateList& in_updateList)
{
    ASSERT(UpdateList::State::STATE_ALLOCATED == in_updateList.m_executionState);

    // set to submitted, allowing mapping within submitThread
    // fenceMonitorThread will wait for the copy fence to become valid before progressing state
    in_updateList.m_executionState = UpdateList::State::STATE_SUBMITTED;

    if (in_updateList.GetNumStandardUpdates())
    {
        m_pFileStreamer->StreamTexture(in_updateList);
    }

    // add to submit task queue
    {
        m_submitTasks[m_submitTaskAlloc.GetWriteIndex()] = &in_updateList;
        m_submitTaskAlloc.Allocate();
        m_submitFlag.Set();
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
    bool loadPackedMips = false;

    const UINT numTasks = m_monitorTaskAlloc.GetReadyToRead();
    if (0 == numTasks)
    {
        return;
    }

    const UINT startIndex = m_monitorTaskAlloc.GetReadIndex();
    for (UINT i = startIndex; i < (startIndex + numTasks); i++)
    {
        ASSERT(numTasks != 0);
        auto& updateList = *m_monitorTasks[i % m_monitorTasks.size()];

        bool freeUpdateList = false;

        // assign a start time to every in-flight update list. this will give us an upper bound on latency.
        // latency is only measured for tile uploads
        if ((UpdateList::State::STATE_FREE != updateList.m_executionState) && (0 == updateList.m_copyLatencyTimer))
        {
            updateList.m_copyLatencyTimer = m_pFenceThreadTimer->GetTime();
        }

        switch (updateList.m_executionState)
        {

        case UpdateList::State::STATE_PACKED_MAPPING:
            ASSERT(0 == updateList.GetNumStandardUpdates());
            ASSERT(0 == updateList.GetNumEvictions());

            // wait for mapping complete before streaming packed tiles
            if (m_mappingFence->GetCompletedValue() >= updateList.m_mappingFenceValue)
            {
                LoadTextureFromMemory(updateList);

                loadPackedMips = true; // set flag to signal fence
                updateList.m_executionState = UpdateList::State::STATE_PACKED_COPY_PENDING;
            }
            break;

        case UpdateList::State::STATE_PACKED_COPY_PENDING:
            ASSERT(0 == updateList.GetNumStandardUpdates());
            ASSERT(0 == updateList.GetNumEvictions());

            if (m_memoryFence->GetCompletedValue() >= updateList.m_copyFenceValue)
            {
                updateList.m_pResource->NotifyPackedMips();
                freeUpdateList = true;
            }
            break;

        case UpdateList::State::STATE_UPLOADING:
            ASSERT(0 != updateList.GetNumStandardUpdates());

            // only check copy fence if the fence has been set (avoid race condition)
            if ((updateList.m_copyFenceValid) && m_pFileStreamer->GetCompleted(updateList.m_copyFenceValue))
            {
                updateList.m_executionState = UpdateList::State::STATE_MAP_PENDING;
            }
            else
            {
                break;
            }
            [[fallthrough]];

        case UpdateList::State::STATE_MAP_PENDING:
            if (updateList.m_mappingFenceValue <= m_mappingFence->GetCompletedValue())
            {
                // notify evictions
                if (updateList.GetNumEvictions())
                {
                    updateList.m_pResource->NotifyEvicted(updateList.m_evictCoords);

                    m_numTotalEvictions.fetch_add(updateList.GetNumEvictions(), std::memory_order_relaxed);
                }

                // notify regular tiles
                if (updateList.GetNumStandardUpdates())
                {
                    updateList.m_pResource->NotifyCopyComplete(updateList.m_coords);

                    auto updateLatency = m_pFenceThreadTimer->GetTime() - updateList.m_copyLatencyTimer;
                    m_totalTileCopyLatency.fetch_add(updateLatency * updateList.GetNumStandardUpdates(), std::memory_order_relaxed);

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
            // O(1) array compaction: move the first element to the position of the element to be freed, then reduce size by 1.
            m_monitorTasks[i % m_monitorTasks.size()] = m_monitorTasks[m_monitorTaskAlloc.GetReadIndex()];
            m_monitorTaskAlloc.Free();
            FreeUpdateList(updateList);
        }
    } // end loop over updatelists

    if (loadPackedMips)
    {
        SubmitTextureLoadsFromMemory();
    }
}

//-----------------------------------------------------------------------------
// Submit Thread
// On submission, all updatelists need mapping
// set next state depending on the task
// Note: QueryPerformanceCounter() needs to be called from the same CPU for values to be compared,
//       but this thread starts work while a different thread handles completion
// NOTE: if UpdateTileMappings is slow, throughput will be impacted
//-----------------------------------------------------------------------------
void SFS::DataUploader::SubmitThread()
{
    bool signalMap = false;

    // look through tasks
    while (m_submitTaskAlloc.GetReadyToRead())
    {
        signalMap = true;

        auto& updateList = *m_submitTasks[m_submitTaskAlloc.GetReadIndex()]; // get the next task
        m_submitTaskAlloc.Free(); // consume this task

        ASSERT(UpdateList::State::STATE_SUBMITTED == updateList.m_executionState);

        // set to the fence value to be signaled next
        updateList.m_mappingFenceValue = m_mappingFenceValue;

        // unmap tiles that are being evicted
        if (updateList.GetNumEvictions())
        {
            m_mappingUpdater.UnMap(GetMappingQueue(), updateList.m_pResource->GetTiledResource(), updateList.m_evictCoords);

            // this will skip the uploading state unless there are uploads
            updateList.m_executionState = UpdateList::State::STATE_MAP_PENDING;
        }

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
    }

    if (signalMap)
    {
        m_mappingCommandQueue->Signal(m_mappingFence.Get(), m_mappingFenceValue);
        m_mappingFenceValue++;
    }
}