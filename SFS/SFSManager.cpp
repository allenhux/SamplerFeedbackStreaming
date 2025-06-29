//==============================================================
// Copyright © Intel Corporation
//
// SPDX-License-Identifier: MIT
// =============================================================

//=============================================================================
// Implementation of the ::SFSManager public (external) interface
//=============================================================================

#include "pch.h"

#include "SFSManager.h"
#include "ManagerSR.h"
#include "SFSResource.h"
#include "DataUploader.h"
#include "SFSHeap.h"

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
SFS::Manager::Manager(const struct SFSManagerDesc& in_desc, ID3D12Device8* in_pDevice)
    : ManagerBase(in_desc, in_pDevice)
    , m_gpuTimerResolve(in_pDevice, in_desc.m_swapChainBufferCount, D3D12GpuTimer::TimerType::Direct)
{
    ASSERT(in_desc.m_resolveHeapSizeMB);

    m_commandListEndFrame.Allocate(m_device.Get(), m_numSwapBuffers, L"SFS::ManagerBase::m_commandListEndFrame");

#if RESOLVE_TO_TEXTURE
    // allocate shared space for per-frame feedback resolves
    {
        // the following limits the implementation to a maximum minmipmap dimension of 64x64,
        // which supports 16k x 16k BC7 textures. BC1 only requires only 32x64.
        // maximum texture width / tile width for bc7 = 16k / 256 = 64
        constexpr UINT BC7_TILE_DIMENSION = 256; // same in u and v
        constexpr UINT MAX_RESOLVE_DIM = D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION / BC7_TILE_DIMENSION; // == 64

        auto textureDesc = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R8_UINT, MAX_RESOLVE_DIM, MAX_RESOLVE_DIM, 1, 1);
        textureDesc.Alignment = D3D12_SMALL_RESOURCE_PLACEMENT_ALIGNMENT;

        // ideally, texture size is 4KB so 32MB is enough for 8192 feedback resolves per frame.
        const UINT HEAP_SIZE = in_desc.m_resolveHeapSizeMB * 1024 * 1024;

        // from alignment and size, compute maximum resolves we can fit in the heap
        D3D12_RESOURCE_ALLOCATION_INFO info = in_pDevice->GetResourceAllocationInfo(0, 1, &textureDesc);
        UINT64 mask = info.Alignment - 1;
        const UINT allocationStep = UINT((info.SizeInBytes + mask) & ~mask);
        m_maxNumResolvesPerFrame = HEAP_SIZE / allocationStep;

        m_sharedResolvedResources.resize(m_maxNumResolvesPerFrame);
        CD3DX12_HEAP_DESC heapDesc(HEAP_SIZE, D3D12_HEAP_TYPE_DEFAULT, 0, D3D12_HEAP_FLAG_DENY_BUFFERS | D3D12_HEAP_FLAG_DENY_RT_DS_TEXTURES);
        ThrowIfFailed(m_device->CreateHeap(&heapDesc, IID_PPV_ARGS(&m_resolvedResourceHeap)));

        auto initialState = D3D12_RESOURCE_STATE_COPY_SOURCE;

        UINT heapOffset = 0;
        for (auto& r : m_sharedResolvedResources)
        {
            in_pDevice->CreatePlacedResource(m_resolvedResourceHeap.Get(), heapOffset, &textureDesc, initialState, nullptr, IID_PPV_ARGS(&r));
            heapOffset += allocationStep;
        }
    }
#endif
}

//-----------------------------------------------------------------------------
// create command list and allocators
//-----------------------------------------------------------------------------
void SFS::Manager::CommandList::Allocate(ID3D12Device* in_pDevice, UINT in_numAllocators, std::wstring in_name)
{
    m_allocators.resize(in_numAllocators);
    for (UINT i = 0; i < in_numAllocators; i++)
    {
        ThrowIfFailed(in_pDevice->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_allocators[i])));
        m_allocators[i]->SetName(AutoString(in_name, ".m_allocators[", i, "]").str().c_str());

    }
    ThrowIfFailed(in_pDevice->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_allocators[0].Get(), nullptr, IID_PPV_ARGS(&m_commandList)));
    m_commandList->SetName(AutoString(in_name, ".m_commandList").str().c_str());

    m_commandList->Close();
}

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
void SFS::Manager::CommandList::Reset(UINT in_allocatorIndex)
{
    m_allocators[in_allocatorIndex]->Reset();
    ThrowIfFailed(m_commandList->Reset(m_allocators[in_allocatorIndex].Get(), nullptr));
}

//--------------------------------------------
// instantiate streaming library
//--------------------------------------------
SFSManager* SFSManager::Create(const SFSManagerDesc& in_desc)
{
    SFS::ComPtr<ID3D12Device8> device;
    in_desc.m_pDirectCommandQueue->GetDevice(IID_PPV_ARGS(&device));
    return new SFS::Manager(in_desc, device.Get());
}

void SFS::Manager::Destroy()
{
    delete this;
}

//--------------------------------------------
// Create a heap used by 1 or more StreamingResources
// parameter is number of 64KB tiles to manage
//--------------------------------------------
SFSHeap* SFS::Manager::CreateHeap(UINT in_maxNumTilesHeap)
{
    auto pStreamingHeap = new SFS::Heap(this, m_dataUploader.GetMappingQueue(), in_maxNumTilesHeap);
    m_streamingHeaps.push_back(pStreamingHeap);
    return (SFSHeap*)pStreamingHeap;
}

//--------------------------------------------
// Create SFS Resources using a common SFSManager
//--------------------------------------------
SFSResource* SFS::Manager::CreateResource(const struct SFSResourceDesc& in_desc,
    SFSHeap* in_pHeap, const std::wstring& in_filename)
{
    ResourceBase* pRsrc = new Resource(in_filename, in_desc, (SFS::ManagerSR*)this, (SFS::Heap*)in_pHeap);

    // NOTE: m_streamingResources won't be updated until EndFrame()
    m_newResources.Acquire().push_back(pRsrc);
    m_newResources.Release();

    return pRsrc;
}

//-----------------------------------------------------------------------------
// resources begin to be flushed in BeginFrame()
//-----------------------------------------------------------------------------
void SFS::Manager::FlushResources(const std::vector<SFSResource*>& in_resources, HANDLE in_event)
{
    ASSERT(in_event);
    ASSERT(in_resources.size());

    // flush from SFSManager internal structures
    auto& v = m_flushResources.Acquire();
    ASSERT(0 == v.size());
    for (auto r : in_resources)
    {
        v.insert((ResourceBase*)r);
    }
    m_flushResources.Release();

    // flush from internal threads
    m_processFeedbackThread.ShareFlushResources(in_resources, in_event);
}

//-----------------------------------------------------------------------------
// set which file streaming system to use
// will reset even if previous setting was the same. so?
//-----------------------------------------------------------------------------
void SFS::ManagerBase::UseDirectStorage(bool in_useDS)
{
    Finish();
    auto streamerType = SFS::DataUploader::StreamerType::Reference;
    if (in_useDS)
    {
        streamerType = SFS::DataUploader::StreamerType::DirectStorage;
    }

    auto pOldStreamer = m_dataUploader.SetStreamer(streamerType, m_traceCaptureMode);

    for (auto p : m_streamingResources)
    {
        p->SetFileHandle(&m_dataUploader);
    }

    delete pOldStreamer;
    StartThreads();
}

//-----------------------------------------------------------------------------
// performance and visualization
//-----------------------------------------------------------------------------
float SFS::Manager::GetGpuTexelsPerMs() const { return m_texelsPerMs; }
UINT SFS::Manager::GetMaxNumFeedbacksPerFrame() const { return m_maxNumResolvesPerFrame; }
float SFS::Manager::GetGpuTime() const { return m_gpuTimerResolve.GetTimes()[m_renderFrameIndex].first; }
float SFS::Manager::GetCpuProcessFeedbackTime() { return m_processFeedbackFrameTime; }
UINT SFS::Manager::GetTotalNumUploads() const { return m_dataUploader.GetTotalNumUploads(); }
UINT SFS::Manager::GetTotalNumEvictions() const { return m_dataUploader.GetTotalNumEvictions(); }
UINT SFS::Manager::GetTotalNumSubmits() const { return m_processFeedbackThread.GetTotalNumSubmits(); }
float SFS::Manager::GetTotalTileCopyLatency() const { return m_dataUploader.GetApproximateTileCopyLatency(); }

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
void SFS::Manager::SetVisualizationMode(UINT in_mode)
{
    ASSERT(!GetWithinFrame());
    Finish();
    for (auto p : m_streamingResources)
    {
        p->ClearAllocations();
    }

    m_dataUploader.SetVisualizationMode(in_mode);
    StartThreads();
}

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
void SFS::Manager::CaptureTraceFile(bool in_captureTrace)
{
    ASSERT(m_traceCaptureMode); // must enable at creation time by setting SFSManagerDesc::m_traceCaptureMode
    m_dataUploader.CaptureTraceFile(in_captureTrace);
}

//-----------------------------------------------------------------------------
// Call this method once for each SFSManager that shares heap/upload buffers
// expected to be called once per frame, before anything is drawn.
//-----------------------------------------------------------------------------
void SFS::Manager::BeginFrame()
{
    ASSERT(!GetWithinFrame());
    m_withinFrame = true;

    // the frame fence is used to determine when to read feedback:
    // read back the feedback after the frame that writes to it has completed
    // note the signal is for the previous frame
    // the updated value is used for resolve & copy commands for this frame during EndFrame()
    m_directCommandQueue->Signal(m_frameFence.Get(), m_frameFenceValue);
    m_frameFenceValue++;

    // accumulate gpu time from last frame
    m_gpuFeedbackTime += GetGpuTime();

    // index used to track command list allocators, timers, etc.
    m_renderFrameIndex = m_frameFenceValue % m_numSwapBuffers;

    // if feedback requested last frame, post affected resources
    if (m_pendingResources.size())
    {
        m_processFeedbackThread.SharePendingResources(m_pendingResources);
        m_pendingResources.clear();
    }

    // every frame, process feedback (also steps eviction history from prior frames)
    m_processFeedbackThread.Wake();

    // capture cpu time spent processing feedback
    {
        INT64 processFeedbackTime = m_processFeedbackThread.GetTotalProcessTime(); // snapshot of live counter
        m_processFeedbackFrameTime = m_processFeedbackThread.GetSecondsFromDelta(processFeedbackTime - m_previousFeedbackTime);
        m_previousFeedbackTime = processFeedbackTime; // remember current time for next call
    }
}

//-----------------------------------------------------------------------------
// create clear commands for each feedback buffer used this frame
// also initializes the descriptors, which can change if the heaps are reallocated
//-----------------------------------------------------------------------------
void SFS::Manager::ClearFeedback(ID3D12GraphicsCommandList* in_pCommandList, const std::set<ResourceBase*>& in_resources)
{
    // note clear value is ignored when clearing feedback maps
    UINT clearValue[4]{};
    const D3D12_CPU_DESCRIPTOR_HANDLE boundStart = m_sharedClearUavHeapBound->GetCPUDescriptorHandleForHeapStart();
    const D3D12_CPU_DESCRIPTOR_HANDLE notBoundStart = m_sharedClearUavHeapNotBound->GetCPUDescriptorHandleForHeapStart();
    const D3D12_GPU_DESCRIPTOR_HANDLE boundStartGpu = m_sharedClearUavHeapBound->GetGPUDescriptorHandleForHeapStart();
    for (auto p : in_resources)
    {
        D3D12_CPU_DESCRIPTOR_HANDLE bound = boundStart;
        D3D12_GPU_DESCRIPTOR_HANDLE boundGpu = boundStartGpu;
        D3D12_CPU_DESCRIPTOR_HANDLE notBound = notBoundStart;

        auto offset = p->GetClearUavDescriptorOffset();
        bound.ptr += offset;
        boundGpu.ptr += offset;
        notBound.ptr += offset;

        p->CreateFeedbackView(bound);
        p->CreateFeedbackView(notBound);

        in_pCommandList->ClearUnorderedAccessViewUint(boundGpu, notBound, p->GetOpaqueFeedback(), clearValue, 0, nullptr);
    }
}

//-----------------------------------------------------------------------------
// Call this method once corresponding to BeginFrame()
// expected to be called once per frame, after everything was drawn.
//
// returns 1 command list to be called after draw calls to:
//    - transition packed mips (do not draw affected objects until subsequent frame)
//    - resolve feedback
//    - copy feedback to readback buffers
//    - clear feedback
//-----------------------------------------------------------------------------
ID3D12CommandList* SFS::Manager::EndFrame(D3D12_CPU_DESCRIPTOR_HANDLE out_minmipmapDescriptorHandle)
{
    // NOTE: we are "within frame" until the end of EndFrame()
    ASSERT(GetWithinFrame());

    // release old shared residency map
    // not necessary because they will get released on app exit, but reduces runtime memory
    {
        auto i = m_frameFenceValue % m_oldSharedResidencyMaps.size();
        m_oldSharedResidencyMaps[i] = nullptr;
        m_oldSharedClearUavHeaps[i] = nullptr;
    }

    // stop tracking resources that have been Destroy()ed
    // must remove resources before calling AllocateSharedResidencyMap()
    RemoveResources();
    // delete heaps that have been requested via SFSHeap::Destroy() and are no longer in use
    RemoveHeaps();

    // if new StreamingResources have been created...
    if (m_newResources.size())
    {
        std::vector<ResourceBase*> newResources;
        m_newResources.Acquire().swap(newResources);
        m_newResources.Release();

        m_streamingResources.insert(m_streamingResources.end(), newResources.begin(), newResources.end());
        AllocateSharedResidencyMap();

        // monitor new resources for when they need packed mip transition barriers
        m_packedMipTransitionResources.insert(m_packedMipTransitionResources.end(), newResources.begin(), newResources.end());

        // share new resources with PFT. PFT will share with residency thread
        m_processFeedbackThread.ShareNewResources(newResources);
    }

    // handle FlushResources(). note occurs after PFT::ShareNewResources
    FlushResourcesInternal();

    // create a view for shared minmipmap, used by the application's pixel shaders
    CreateMinMipMapView(out_minmipmapDescriptorHandle);

    // transition those new resources that are ready to be drawn (after packed mips have arrived)
    if (m_packedMipTransitionResources.size())
    {
        for (UINT i = 0; i < m_packedMipTransitionResources.size();)
        {
            auto p = m_packedMipTransitionResources[i];
            if (p->GetPackedMipsNeedTransition())
            {
                p->SetPackedMipsTransitioned();
                D3D12_RESOURCE_BARRIER b = CD3DX12_RESOURCE_BARRIER::Transition(
                    p->GetTiledResource(),
                    D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                m_packedMipTransitionBarriers.push_back(b);
                m_packedMipTransitionResources[i] = m_packedMipTransitionResources.back();
                m_packedMipTransitionResources.pop_back();
            }
            else
            {
                i++;
            }
        }
    }

    // start command list for this frame
    m_commandListEndFrame.Reset(m_renderFrameIndex);

    auto pCommandList = m_commandListEndFrame.m_commandList.Get();

    // clear UAV heap must be bound to command list
    ID3D12DescriptorHeap* ppHeaps[] = { m_sharedClearUavHeapBound.Get() };
    pCommandList->SetDescriptorHeaps(_countof(ppHeaps), ppHeaps);

    // transition packed mips if necessary
    // FIXME? if any 1 needs a transition, go ahead and check all of them. not worth optimizing.
    // NOTE: the debug layer may complain about CopyTextureRegion() if the resource state is not state_copy_dest (or common)
    //       despite the fact the copy queue doesn't really care about resource state
    //       CopyTiles() won't complain because this library always targets an atlas that is always state_copy_dest
    if (m_packedMipTransitionBarriers.size())
    {
        if (m_feedbackReadbacks.size())
        {
            m_barrierUavToResolveSrc.insert(m_barrierUavToResolveSrc.end(), m_packedMipTransitionBarriers.begin(), m_packedMipTransitionBarriers.end());
        }
        else
        {
            pCommandList->ResourceBarrier((UINT)m_packedMipTransitionBarriers.size(), m_packedMipTransitionBarriers.data());
        }
        m_packedMipTransitionBarriers.clear();
    }

    if (m_feedbackReadbacks.size())
    {
        m_gpuTimerResolve.BeginTimer(pCommandList, m_renderFrameIndex);

        // pre-clear feedback buffers on first use to work around non-clear initial state on some hardware
        if (m_firstTimeClears.size())
        {
            ClearFeedback(pCommandList, m_firstTimeClears);
            m_firstTimeClears.clear();
        }

        // transition all feedback resources UAV->RESOLVE_SOURCE
        // also transition the (non-opaque) resolved resources COPY_SOURCE->RESOLVE_DEST
        pCommandList->ResourceBarrier((UINT)m_barrierUavToResolveSrc.size(), m_barrierUavToResolveSrc.data());

        // do the feedback resolves
#if RESOLVE_TO_TEXTURE
        {
            UINT i = 0;
            for (auto p : m_feedbackReadbacks)
            {
                p->ResolveFeedback(pCommandList, m_frameFenceValue, m_sharedResolvedResources[i].Get());
                i++;
            }
        }
#else
        for (auto p : m_feedbackReadbacks)
        {
            p->ResolveFeedback(pCommandList, m_frameFenceValue);
        }
#endif

        // paranoia?
        m_barrierResolveSrcToUav.push_back(CD3DX12_RESOURCE_BARRIER::UAV(nullptr));

        // transition all feedback resources RESOLVE_SOURCE->UAV
        // also transition the (non-opaque) resolved resources RESOLVE_DEST->COPY_SOURCE
        pCommandList->ResourceBarrier((UINT)m_barrierResolveSrcToUav.size(), m_barrierResolveSrcToUav.data());

#if RESOLVE_TO_TEXTURE
        {
            UINT i = 0;
            for (auto p : m_feedbackReadbacks)
            {
                p->ReadbackFeedback(pCommandList, m_sharedResolvedResources[i].Get());
                i++;
            }
        }
#endif

        // now safe to clear feedback buffers
        ClearFeedback(pCommandList, m_feedbackReadbacks);

        m_gpuTimerResolve.EndTimer(pCommandList, m_renderFrameIndex);
        m_gpuTimerResolve.ResolveTimer(pCommandList, m_renderFrameIndex);

        // feedback array consumed, clear for next frame
        m_feedbackReadbacks.clear();
        m_barrierUavToResolveSrc.clear();
        m_barrierResolveSrcToUav.clear();

        // there was feedback this frame, so include it when measuring texels/ms
        m_numFeedbackTimingFrames++;
    }

    pCommandList->Close();

    m_withinFrame = false;

    if (m_numFeedbackTimingFrames >= m_feedbackTimingFrequency)
    {
        m_numFeedbackTimingFrames = 0;
        m_texelsPerMs = m_numTexelsQueued / (m_gpuFeedbackTime * 1000.f);
        m_numTexelsQueued = 0;
        m_gpuFeedbackTime = 0;
    }

    return pCommandList;
}
