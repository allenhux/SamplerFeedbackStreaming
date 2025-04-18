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
#include "XeTexture.h"
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
            ProcessFeedbackThread();
        });

    // modify residency maps as a result of gpu completion events
    m_updateResidencyThread = std::thread([&]
        {
            while (m_threadsRunning)
            {
                m_residencyChangedFlag.Wait();

                for (auto p : m_streamingResources)
                {
                    p->UpdateMinMipMap();
                }
            }
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
    // array of indices to resources that need tiles loaded/evicted
    std::vector<UINT> staleResources;
    staleResources.reserve(m_streamingResources.size());

    // flags to prevent duplicates in the staleResources array
    BitVector<UINT32> pending(m_streamingResources.size(), 0);

    UINT uploadsRequested = 0; // remember if any work was queued so we can signal afterwards
    UINT64 previousFrameFenceValue = m_frameFenceValue;
    while (m_threadsRunning)
    {
        // DEBUG: verify that no streaming resources have been added/removed during thread lifetime
        ASSERT(m_streamingResources.size() == pending.size());

        // prioritize loading packed mips, as objects shouldn't be displayed until packed mips load
        bool expected = true;
        if (m_havePackedMipsToLoad.compare_exchange_weak(expected, false))
        {
            for (auto p : m_streamingResources)
            {
                if (!p->InitPackedMips())
                {
                    m_havePackedMipsToLoad = true; // did not finish uploading packed mips, keep trying
                }
            }
            if (m_havePackedMipsToLoad)
            {
                continue; // still working on loading packed mips. don't move on to other streaming tasks yet.
            }
        }

        bool flushPendingUploadRequests = false;

        // process feedback buffers once per frame
        {
            UINT64 frameFenceValue = m_frameFence->GetCompletedValue();
            if (previousFrameFenceValue != frameFenceValue)
            {
                previousFrameFenceValue = frameFenceValue;

                // flush any pending uploads from previous frame
                if (uploadsRequested) { flushPendingUploadRequests = true; }

                auto startTime = m_cpuTimer.GetTime();
                for (UINT i = 0; i < m_streamingResources.size(); i++)
                {
                    m_streamingResources[i]->ProcessFeedback(frameFenceValue);
                    if (m_streamingResources[i]->IsStale() && (0 == pending[i]))
                    {
                        staleResources.push_back(i);
                        pending[i] = 1;
                    }
                }
                // add the amount of time we just spent processing feedback for a single frame
                m_processFeedbackTime += UINT64(m_cpuTimer.GetTime() - startTime);
            }
        }

        // push uploads and evictions for stale resources
        {
            UINT numEvictions = 0;
            UINT newStaleSize = 0; // track number of stale resources, then resize the array to the updated number
            for (auto resourceIndex : staleResources)
            {
                if (m_dataUploader.GetNumUpdateListsAvailable()
                    // with DirectStorage Queue::EnqueueRequest() can block.
                    // when there are many pending uploads, there can be multiple frames of waiting.
                    // if we wait too long in this loop, we miss calling ProcessFeedback() above which adds pending uploads & evictions
                    // this is a vicious feedback cycle that leads to even more pending requests, and even longer delays.
                    // the following check avoids enqueueing more uploads if the frame has changed:
                    && (m_frameFence->GetCompletedValue() == previousFrameFenceValue)
                    && m_threadsRunning) // don't add work while exiting
                {
                    uploadsRequested += m_streamingResources[resourceIndex]->QueueTiles();
                }

                // tiles that are "loading" can't be evicted. as soon as they arrive, they can be.
                // note: since we aren't unmapping evicted tiles, we can evict even if no UpdateLists are available
                numEvictions += m_streamingResources[resourceIndex]->QueuePendingTileEvictions();

                if (m_streamingResources[resourceIndex]->IsStale()) // still have work to do?
                {
                    // keep stale resource in compacted array while retaining oldest-first ordering
                    staleResources[newStaleSize] = resourceIndex;
                    newStaleSize++;
                }
                else
                {
                    pending[resourceIndex] = 0; // clear the flag that prevents duplicates
                }
            }
            staleResources.resize(newStaleSize); // compact array
            if (numEvictions) { m_dataUploader.AddEvictions(numEvictions); }
        }

        // if there are uploads, maybe signal depending on heuristic to minimize # signals
        if (uploadsRequested)
        {
            // tell the file streamer to signal the corresponding fence
            if ((flushPendingUploadRequests) || // flush requests from previous frame
                (0 == staleResources.size()) || // flush because there's no more work to be done (no stale resources, all feedback has been processed)
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
        if (0 == staleResources.size())
        {
            ASSERT(0 == uploadsRequested);
            m_processFeedbackFlag.Wait();
        }
    }
    // if thread exits, flush any pending uploads
    if (uploadsRequested) { SignalFileStreamer(); }
}

//-----------------------------------------------------------------------------
// stop only SFSManager threads. Used by Finish() and CreateResource()
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
// SFSResource::SetResidencyMapOffsetBase() will populate the residency map with latest
// descriptor handle required to update the assoiated shader resource view
//-----------------------------------------------------------------------------
void SFS::ManagerBase::AllocateResidencyMap(D3D12_CPU_DESCRIPTOR_HANDLE in_descriptorHandle)
{
    static const UINT alignment = 32; // these are bytes, so align by 32 corresponds to SIMD32
    static const UINT minBufferSize = 64 * 1024; // multiple of 64KB page

    UINT oldBufferSize = 0;
    if (nullptr != m_residencyMap.GetResource())
    {
        oldBufferSize = (UINT)m_residencyMap.GetResource()->GetDesc().Width;
    }

    // allocate residency map buffer large enough for numswapbuffers * min mip map buffers for each SFSResource
    m_residencyMapOffsets.resize(m_streamingResources.size());
    UINT offset = 0;
    for (UINT i = 0; i < (UINT)m_residencyMapOffsets.size(); i++)
    {
        m_residencyMapOffsets[i] = offset;

        UINT minMipMapSize = m_streamingResources[i]->GetNumTilesWidth() * m_streamingResources[i]->GetNumTilesHeight();

        offset += minMipMapSize;

        offset = (offset + alignment - 1) & ~(alignment-1);
    }

    if (offset > oldBufferSize)
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

        UINT bufferSize = std::max(offset, minBufferSize);
        m_residencyMap.Allocate(m_device.Get(), bufferSize, uploadHeapProperties);

        CreateMinMipMapView(in_descriptorHandle);
    }

    // set offsets AFTER allocating resource. allows SFSResource to initialize buffer state
    for (UINT i = 0; i < (UINT)m_streamingResources.size(); i++)
    {
        m_streamingResources[i]->SetResidencyMapOffsetBase(m_residencyMapOffsets[i]);
    }
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

    UINT sharedClearUavHeapIndex = 0;
    auto srvUavCbvDescriptorSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    for (const auto& r : m_streamingResources)
    {
        CD3DX12_CPU_DESCRIPTOR_HANDLE clearHandle(m_sharedClearUavHeap->GetCPUDescriptorHandleForHeapStart(), sharedClearUavHeapIndex, srvUavCbvDescriptorSize);
        sharedClearUavHeapIndex++;
        r->CreateFeedbackView(m_device.Get(), clearHandle);
        r->SetClearUavDescriptor(clearHandle);
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

