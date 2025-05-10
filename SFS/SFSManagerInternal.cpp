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

#include "SFSManagerBase.h"
#include "SFSResourceBase.h"
#include "SFSHeap.h"
#include "BitVector.h"

// agility sdk 1.613.3 or later required for gpu upload heaps 
extern "C" { __declspec(dllexport) extern const UINT D3D12SDKVersion = 615; }
extern "C" { __declspec(dllexport) extern const char* D3D12SDKPath = ".\\D3D12\\"; }

//=============================================================================
// constructor for streaming library base class
//=============================================================================
SFS::ManagerBase::ManagerBase(const SFSManagerDesc& in_desc, ID3D12Device8* in_pDevice) :// required for constructor
m_numSwapBuffers(in_desc.m_swapChainBufferCount)
// delay eviction by enough to not affect a pending frame
,m_evictionDelay(std::max(in_desc.m_swapChainBufferCount + 1, in_desc.m_evictionDelay))
, m_gpuTimerResolve(in_pDevice, in_desc.m_swapChainBufferCount, D3D12GpuTimer::TimerType::Direct)
, m_renderFrameIndex(0)
, m_directCommandQueue(in_desc.m_pDirectCommandQueue)
, m_device(in_pDevice)
, m_commandLists((UINT)CommandListName::Num)
, m_maxTileMappingUpdatesPerApiCall(in_desc.m_maxTileMappingUpdatesPerApiCall)
, m_minNumUploadRequests(in_desc.m_minNumUploadRequests)
, m_threadPriority((int)in_desc.m_threadPriority)
, m_dataUploader(in_pDevice, in_desc.m_maxNumCopyBatches, in_desc.m_stagingBufferSizeMB, in_desc.m_maxTileMappingUpdatesPerApiCall, (int)in_desc.m_threadPriority)
, m_traceCaptureMode{in_desc.m_traceCaptureMode}
, m_oldSharedResidencyMaps(in_desc.m_swapChainBufferCount + 1, nullptr)
{
    ASSERT(D3D12_COMMAND_LIST_TYPE_DIRECT == m_directCommandQueue->GetDesc().Type);

    ThrowIfFailed(in_pDevice->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_frameFence)));
    m_frameFence->SetName(L"SFS::ManagerBase::m_frameFence");

    const UINT numAllocators = m_numSwapBuffers;
    for (UINT c = 0; c < (UINT)CommandListName::Num; c++)
    {
        auto& cl = m_commandLists[c];
        cl.m_allocators.resize(numAllocators);
        for (UINT i = 0; i < numAllocators; i++)
        {
            ThrowIfFailed(in_pDevice->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&cl.m_allocators[i])));
            cl.m_allocators[i]->SetName(
                AutoString("SFS::ManagerBase::m_commandLists.m_allocators[",
                    c, "][", i, "]").str().c_str());

        }
        ThrowIfFailed(in_pDevice->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, cl.m_allocators[m_renderFrameIndex].Get(), nullptr, IID_PPV_ARGS(&cl.m_commandList)));
        cl.m_commandList->SetName(
            AutoString("SFS::ManagerBase::m_commandLists.m_commandList[", c, "]").str().c_str());

        cl.m_commandList->Close();
    }

    // advance frame number to the first frame...
    m_frameFenceValue++;

    UseDirectStorage(in_desc.m_useDirectStorage);

    StartThreads(); // have to, or hit assert in debug mode
}

SFS::ManagerBase::~ManagerBase()
{
    // force DataUploader to flush now, rather than waiting for its destructor
    Finish();
}

//-----------------------------------------------------------------------------
// kick off threads that continuously streams tiles
// gives StreamingResources opportunities to update feedback
//-----------------------------------------------------------------------------
void SFS::ManagerBase::StartThreads()
{
    if (m_threadsRunning)
    {
        return;
    }

    m_threadsRunning = true;

    // process sampler feedback buffers, generate upload and eviction commands
    m_processFeedbackThread = std::thread([&]
        {
#ifdef _DEBUG
            m_processFeedbackThreadRunning = true;
#endif
            ProcessFeedbackThread();
#ifdef _DEBUG
            m_processFeedbackThreadRunning = false;
#endif
        });

    // modify residency maps as a result of gpu completion events
    m_updateResidencyThread = std::thread([&]
        {
#ifdef _DEBUG
            m_residencyThreadRunning = true;
#endif

            std::vector<ResourceBase*> streamingResources;

            while (m_threadsRunning)
            {
                m_residencyChangedFlag.Wait();

                if (m_newResourcesShareRT.size() && m_newResourcesLockRT.TryAcquire())
                {
                    std::vector<ResourceBase*> newResources;
                    newResources.swap(m_newResourcesShareRT);
                    m_newResourcesLockRT.Release();

                    streamingResources.insert(streamingResources.end(), newResources.begin(), newResources.end());
                }

                std::vector<ResourceBase*> updated;
                for (auto p : streamingResources)
                {
                    if (p->UpdateMinMipMap())
                    {
                        updated.push_back(p);
                    }
                }
                if (updated.size())
                {
                    // only blocks if resource buffer is being re-allocated (effectively never)
                    m_residencyMapLock.Acquire();
                    UINT8* pDest = (UINT8*)m_residencyMap.GetData();
                    for (auto p : updated) { p->WriteMinMipMap(pDest); }
                    m_residencyMapLock.Release();
                }
            }
#ifdef _DEBUG
            m_residencyThreadRunning = false;
#endif
        });

    SFS::SetThreadPriority(m_processFeedbackThread, m_threadPriority);
    SFS::SetThreadPriority(m_updateResidencyThread, m_threadPriority);
}

//-----------------------------------------------------------------------------
// per frame, call SFSResource::ProcessFeedback()
// expects the no change in # of streaming resources during thread lifetime
//-----------------------------------------------------------------------------
void SFS::ManagerBase::SignalFileStreamer()
{
    m_dataUploader.SignalFileStreamer();
    m_numTotalSubmits.fetch_add(1, std::memory_order_relaxed);
}
void SFS::ManagerBase::ProcessFeedbackThread()
{
    // new resources are prioritized until packed mips are in-flight
    std::list<ResourceBase*> newResources;

    // resources with any pending work, including evictions scheduled multiple frames later
    std::set<ResourceBase*> activeResources;

    // resources that need tiles loaded/evicted asap
    std::set<ResourceBase*> pendingResources;

    UINT uploadsRequested = 0; // remember if any work was queued so we can signal afterwards
    UINT64 previousFrameFenceValue = m_frameFenceValue;

    while (m_threadsRunning)
    {
        // check for new resources
        if (m_newResourcesSharePFT.size() && m_newResourcesLockPFT.TryAcquire())
        {
            // grab them and release the lock quickly
            std::vector<ResourceBase*> tmpResources;
            tmpResources.swap(m_newResourcesSharePFT);
            m_newResourcesLockPFT.Release();

            newResources.insert(newResources.end(), tmpResources.begin(), tmpResources.end());
        }

        // FIXME: TODO while packed mips are loading, process evictions on stale resources

        // prioritize loading packed mips, as objects shouldn't be displayed until packed mips load
        if (newResources.size())
        {
            for (auto i = newResources.begin(); i != newResources.end();)
            {
                // must call on every resource that needs to load packed mips
                if ((*i)->InitPackedMips())
                {
                    i = newResources.erase(i);
                }
                else
                {
                    i++;
                }
            }
        }

        // check for existing resources that have feedback
        if (m_pendingSharePFT.size() && m_pendingLockPFT.TryAcquire())
        {
            // grab them and release the lock quickly
            std::vector<ResourceBase*> tmpResources;
            tmpResources.swap(m_pendingSharePFT);
            m_pendingLockPFT.Release();

            activeResources.insert(tmpResources.begin(), tmpResources.end());
        }

        if (newResources.size())
        {
            continue; // still working on loading packed mips. don't move on to other streaming tasks yet.
        }

        bool flushPendingUploadRequests = false;

        // Once per frame: process feedback buffers
        {
            UINT64 frameFenceValue = m_frameFence->GetCompletedValue();
            if (previousFrameFenceValue != frameFenceValue)
            {
                previousFrameFenceValue = frameFenceValue;

                // flush any pending uploads from previous frame
                if (uploadsRequested) { flushPendingUploadRequests = true; }

                auto startTime = m_cpuTimer.GetTime();
                for (auto i = activeResources.begin(); i != activeResources.end();)
                {
                    auto pResource = *i;
                    pResource->ProcessFeedback(frameFenceValue);
                    if (pResource->HasAnyWork())
                    {
                        pendingResources.insert(pResource);
                        i++;
                    }
                    else
                    {
                        i = activeResources.erase(i);
                    }
                }
                // add the amount of time we just spent processing feedback for a single frame
                m_processFeedbackTime += UINT64(m_cpuTimer.GetTime() - startTime);
            }
        }

        // push uploads and evictions for stale resources
        {
            UINT numEvictions = 0;
            for (auto i = pendingResources.begin(); i != pendingResources.end();)
            {
                ResourceBase* pResource = *i;

                // tiles that are "loading" can't be evicted. as soon as they arrive, they can be.
                // note: since we aren't unmapping evicted tiles, we can evict even if no UpdateLists are available
                numEvictions += pResource->QueuePendingTileEvictions();

                if (m_dataUploader.GetNumUpdateListsAvailable()
                    // with DirectStorage Queue::EnqueueRequest() can block.
                    // when there are many pending uploads, there can be multiple frames of waiting.
                    // if we wait too long in this loop, we miss calling ProcessFeedback() above which adds pending uploads & evictions
                    // this is a vicious feedback cycle that leads to even more pending requests, and even longer delays.
                    // the following check avoids enqueueing more uploads if the frame has changed:
                    && (m_frameFence->GetCompletedValue() == previousFrameFenceValue)
                    && m_threadsRunning) // don't add work while exiting
                {
                    uploadsRequested += pResource->QueueTiles();
                }

                if (!pResource->IsStale()) // still have work to do?
                {
                    i = pendingResources.erase(i);
                }
                else
                {
                    i++;
                }
            }
            if (numEvictions) { m_dataUploader.AddEvictions(numEvictions); }
        }

        // if there are uploads, maybe signal depending on heuristic to minimize # signals
        if (uploadsRequested)
        {
            // tell the file streamer to signal the corresponding fence
            if ((flushPendingUploadRequests) || // flush requests from previous frame
                (0 == pendingResources.size()) || // flush because there's no more work to be done (no stale resources, all feedback has been processed)
                // if we need updatelists and there is a minimum amount of pending work, go ahead and submit
                // this minimum heuristic prevents "storms" of submits with too few tiles to sustain good throughput
                ((0 == m_dataUploader.GetNumUpdateListsAvailable()) && (uploadsRequested > m_minNumUploadRequests)))
            {
                SignalFileStreamer();
                uploadsRequested = 0;
            }
        }

        // nothing to do? wait for next frame
        // development note: do not Wait() if uploadsRequested != 0. safe because uploadsRequested was cleared above.
        if (0 == activeResources.size())
        {
            ASSERT(0 == uploadsRequested);
            m_processFeedbackFlag.Wait();
        }
    }

    // if thread exits, flush any pending uploads
    if (uploadsRequested) { SignalFileStreamer(); }
}

//-----------------------------------------------------------------------------
// stop only SFSManager threads. Used by Finish()
//-----------------------------------------------------------------------------
void SFS::ManagerBase::StopThreads()
{
    if (m_threadsRunning)
    {
        // stop SFSManager threads
        // do not want ProcessFeedback generating more work
        // don't want UpdateResidency to write to min maps when that might be replaced
        m_threadsRunning = false;

        // wake up threads so they can exit
        m_processFeedbackFlag.Set();
        m_residencyChangedFlag.Set();

        if (m_processFeedbackThread.joinable())
        {
            m_processFeedbackThread.join();
        }

        if (m_updateResidencyThread.joinable())
        {
            m_updateResidencyThread.join();
        }
        ASSERT(false == m_processFeedbackThreadRunning);
        ASSERT(false == m_residencyThreadRunning);
    }
}

//-----------------------------------------------------------------------------
// flushes all internal queues
// submits all outstanding command lists
// stops all processing threads
//-----------------------------------------------------------------------------
void SFS::ManagerBase::Finish()
{
    ASSERT(!GetWithinFrame());
 
    StopThreads();

    // now we are no longer producing work for the DataUploader, so its commands can be drained
    m_dataUploader.FlushCommands();
}

//-----------------------------------------------------------------------------
// allocate residency map buffer large enough for numswapbuffers * min mip map buffers for each SFSResource
// assign offsets to new resources and update all resources on resource allocation
// descriptor handle required to update the assoiated shader resource view
//-----------------------------------------------------------------------------
void SFS::ManagerBase::AllocateSharedResidencyMap(D3D12_CPU_DESCRIPTOR_HANDLE in_descriptorHandle,
    std::vector<ResourceBase*>& in_newResources)
{
    static constexpr UINT alignment = std::hardware_destructive_interference_size; // cache line size
    static constexpr UINT gpuPageSize = 64 * 1024;

    UINT bufferSize = 0;
    if (nullptr != m_residencyMap.GetResource())
    {
        bufferSize = (UINT)m_residencyMap.GetResource()->GetDesc().Width;
    }

    UINT requiredSize = m_residencyMap.m_bytesUsed;
    ASSERT(0 == (requiredSize & (alignment - 1)));
    for (const auto r : in_newResources)
    {
        UINT minMipMapSize = r->GetMinMipMapSize();
        requiredSize += (minMipMapSize + alignment - 1) & ~(alignment - 1);
    }

    UINT offset = m_residencyMap.m_bytesUsed;
    auto* updateArray = &in_newResources;

    // changing offsets or the residency resource necessitates halting the residency thread
    bool lockAcquired = false;

     // allocate residency map buffer large enough for all SFSResources
    if (requiredSize > bufferSize)
    {
        lockAcquired = true;
        m_residencyMapLock.Acquire();

        // before we allocate, see if re-arranging will work
        requiredSize = 0;
        for (auto r : m_streamingResources)
        {
            UINT minMipMapSize = r->GetMinMipMapSize();
            ASSERT(0 != minMipMapSize);
            requiredSize += (minMipMapSize + alignment - 1) & ~(alignment - 1);
        }

        // definitely doesn't fit, allocate new buffer
        if (requiredSize > bufferSize)
        {
            // if available, use GPU Upload Heaps
            auto uploadHeapProperties = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
            {
                D3D12_FEATURE_DATA_D3D12_OPTIONS16 options{};
                m_device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS16, &options, sizeof(options));

                if (options.GPUUploadHeapSupported)
                {
                    uploadHeapProperties = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_GPU_UPLOAD);
                    // per DirectX-Specs, "CPUPageProperty and MemoryPoolPreference must be ..._UNKNOWN"
                    uploadHeapProperties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
                    uploadHeapProperties.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
                }
            }

            bufferSize = (requiredSize + gpuPageSize - 1) & ~(gpuPageSize - 1);

            {
                auto p = m_residencyMap.Detach();
                auto i = m_frameFenceValue % m_oldSharedResidencyMaps.size();
                m_oldSharedResidencyMaps[i].Attach(p);
            }

            m_residencyMap.Allocate(m_device.Get(), bufferSize, uploadHeapProperties);

            CreateMinMipMapView(in_descriptorHandle);
        }

        // need to re-assign all the streaming resources
        offset = 0;
        updateArray = &m_streamingResources;
    }

    // set offsets AFTER allocating resource. allows SFSResource to initialize buffer state
    // assign all if we allocated above. if re-using, just assign in_newResources
    for (auto r : *updateArray)
    {
        r->SetResidencyMapOffset(offset);
        UINT minMipMapSize = r->GetMinMipMapSize();
        offset += (minMipMapSize + alignment - 1) & ~(alignment - 1);
    }
    ASSERT(offset <= requiredSize);

    if (lockAcquired)
    {
        m_residencyMapLock.Release();
    }

    m_residencyMap.m_bytesUsed = offset;
}

//-----------------------------------------------------------------------------
// create a shared heap for clearing the feedback resources
// this heap will not be bound to a command list
//-----------------------------------------------------------------------------
void SFS::ManagerBase::AllocateSharedClearUavHeap()
{
    UINT numDescriptorsNeeded = (UINT)m_streamingResources.size();
    if ((nullptr == m_sharedClearUavHeap.Get()) || (m_sharedClearUavHeap->GetDesc().NumDescriptors < numDescriptorsNeeded))
    {
        D3D12_DESCRIPTOR_HEAP_DESC desc = {};
        desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        desc.NumDescriptors = numDescriptorsNeeded;
        desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        ThrowIfFailed(m_device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&m_sharedClearUavHeap)));
        m_sharedClearUavHeap->SetName(L"m_sharedClearUavHeap");
    }
}

//-----------------------------------------------------------------------------
// create uav descriptors in the clear uav heap
// could optimize this for just the new resources, but CreateResourceView() is designed to be fast
//     and this function is expected to be called very infrequently
//-----------------------------------------------------------------------------
void SFS::ManagerBase::CreateClearDescriptors()
{
    auto srvUavCbvDescriptorSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    CD3DX12_CPU_DESCRIPTOR_HANDLE clearHandle(m_sharedClearUavHeap->GetCPUDescriptorHandleForHeapStart(), 0, srvUavCbvDescriptorSize);
    for (const auto& r : m_streamingResources)
    {
        // only update resources that are sufficiently initialized
        if (r->Drawable())
        {
            r->CreateFeedbackView(clearHandle);
            r->SetClearUavDescriptor(clearHandle);
            clearHandle.ptr += srvUavCbvDescriptorSize;
        }
    }
}

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
void SFS::ManagerBase::CreateMinMipMapView(D3D12_CPU_DESCRIPTOR_HANDLE in_descriptorHandle)
{
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    srvDesc.Buffer.NumElements = (UINT)m_residencyMap.GetResource()->GetDesc().Width;
    srvDesc.Format = DXGI_FORMAT_R8_UINT;
    // there is only 1 channel
    srvDesc.Shader4ComponentMapping = D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(0, 0, 0, 0);

    m_device->CreateShaderResourceView(m_residencyMap.GetResource(), &srvDesc, in_descriptorHandle);
}

