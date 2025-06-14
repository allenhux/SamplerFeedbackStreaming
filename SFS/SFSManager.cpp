//==============================================================
// Copyright � Intel Corporation
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
    ASSERT(!m_withinFrame);

    ResourceBase* pRsrc = new Resource(in_filename, in_desc, (SFS::ManagerSR*)this, (SFS::Heap*)in_pHeap);

    m_streamingResources.push_back(pRsrc);
    m_newResources.push_back(pRsrc);

    return pRsrc;
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

    for (auto& s : m_streamingResources)
    {
        s->SetFileHandle(&m_dataUploader);
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
    for (auto o : m_streamingResources)
    {
        o->ClearAllocations();
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
void SFS::Manager::BeginFrame(D3D12_CPU_DESCRIPTOR_HANDLE out_minmipmapDescriptorHandle)
{
    ASSERT(!GetWithinFrame());

    // accumulate gpu time from last frame
    m_gpuFeedbackTime += GetGpuTime();

    // delete resources that have been requested via Remove()
    RemoveResources();

    m_withinFrame = true;

    // the frame fence is used to determine when to read feedback:
    // read back the feedback after the frame that writes to it has completed
    // note the signal is for the previous frame
    // the updated value is used for resolve & copy commands for this frame during EndFrame()
    m_directCommandQueue->Signal(m_frameFence.Get(), m_frameFenceValue);
    m_frameFenceValue++;

    // release old shared residency map
    // not necessary because they will get released on app exit, but reduces runtime memory
    {
        auto i = m_frameFenceValue % m_oldSharedResidencyMaps.size();
        m_oldSharedResidencyMaps[i] = nullptr;
        m_oldSharedClearUavHeaps[i] = nullptr;
    }

    // if new StreamingResources have been created...
    if (m_newResources.size())
    {
        AllocateSharedResidencyMap(out_minmipmapDescriptorHandle);

        // monitor new resources for when they need packed mip transition barriers
        m_packedMipTransitionResources.insert(m_packedMipTransitionResources.end(), m_newResources.begin(), m_newResources.end());

        // share new resources with PFT. PFT will share with residency thread
        m_processFeedbackThread.ShareNewResources(m_newResources);
        m_newResources.clear();
    }

    if (m_packedMipTransitionResources.size())
    {
        for (UINT i = 0; i < m_packedMipTransitionResources.size();)
        {
            auto p = m_packedMipTransitionResources[i];
            if (p->GetPackedMipsNeedTransition())
            {
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

    // step to next frame
    // this happens here in case, someday, BeginFrame() needs to know this e.g. to have its own commandList
    m_renderFrameIndex = (m_renderFrameIndex + 1) % m_numSwapBuffers;
}

//-----------------------------------------------------------------------------
// create clear commands for each feedback buffer used this frame
// also initializes the descriptors, which can change if the heaps are reallocated
//-----------------------------------------------------------------------------
void SFS::Manager::ClearFeedback(ID3D12GraphicsCommandList* in_pCommandList, const std::vector<ResourceBase*>& in_resources)
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
ID3D12CommandList* SFS::Manager::EndFrame()
{
    // NOTE: we are "within frame" until the end of EndFrame()
    ASSERT(GetWithinFrame());

    // start command list for this frame
    m_commandListEndFrame.Reset(m_renderFrameIndex);

    auto pCommandList = m_commandListEndFrame.m_commandList.Get();

    // clear UAV requires heap (to access gpu descriptor)
    ID3D12DescriptorHeap* ppHeaps[] = { m_sharedClearUavHeapBound.Get() };
    pCommandList->SetDescriptorHeaps(_countof(ppHeaps), ppHeaps);

    // transition packed mips if necessary
    // FIXME? if any 1 needs a transition, go ahead and check all of them. not worth optimizing.
    // NOTE: the debug layer may complain about CopyTextureRegion() if the resource state is not state_copy_dest (or common)
    //       despite the fact the copy queue doesn't really care about resource state
    //       CopyTiles() won't complain because this library always targets an atlas that is always state_copy_dest
    if (m_packedMipTransitionBarriers.size())
    {
        pCommandList->ResourceBarrier((UINT)m_packedMipTransitionBarriers.size(), m_packedMipTransitionBarriers.data());
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

        // barrier coalescing
        UINT barrierArraySize = (UINT)m_feedbackReadbacks.size();
#if RESOLVE_TO_TEXTURE
        barrierArraySize *= 2;
#endif
        if (m_barrierResolveSrcToUav.size() < barrierArraySize)
        {
            m_barrierUavToResolveSrc.assign(barrierArraySize, {});
            m_barrierResolveSrcToUav.assign(barrierArraySize, {});
        }

        // resolve to texture incurs a subsequent copy to linear buffer
        UINT numFeedbackReadbacks = (UINT)m_feedbackReadbacks.size();
        for (UINT i = 0; i < numFeedbackReadbacks; i++)
        {
            auto pResource = m_feedbackReadbacks[i]->GetOpaqueFeedback();
            // after drawing, transition the opaque feedback resources from UAV to resolve source
            // transition the feedback decode target to resolve_dest
            m_barrierUavToResolveSrc[i] = CD3DX12_RESOURCE_BARRIER::Transition(pResource, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_RESOLVE_SOURCE);

            // after resolving, transition the opaque resources back to UAV. Transition the resolve destination to copy source for read back on cpu
            m_barrierResolveSrcToUav[i] = CD3DX12_RESOURCE_BARRIER::Transition(pResource, D3D12_RESOURCE_STATE_RESOLVE_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

#if RESOLVE_TO_TEXTURE
            // resolve to texture incurs a subsequent copy to linear buffer
            auto pResolved = m_sharedResolvedResources[i].Get();
            m_barrierUavToResolveSrc[numFeedbackReadbacks + i] = CD3DX12_RESOURCE_BARRIER::Transition(pResolved, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RESOLVE_DEST);
            m_barrierResolveSrcToUav[numFeedbackReadbacks + i] = CD3DX12_RESOURCE_BARRIER::Transition(pResolved, D3D12_RESOURCE_STATE_RESOLVE_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
#endif
        } // end barrier coalescing

        // transition all feedback resources UAV->RESOLVE_SOURCE
        // also transition the (non-opaque) resolved resources COPY_SOURCE->RESOLVE_DEST
        pCommandList->ResourceBarrier(barrierArraySize, m_barrierUavToResolveSrc.data());

        // do the feedback resolves
        for (UINT i = 0; i < (UINT)m_feedbackReadbacks.size(); i++)
        {
            m_numTexelsQueued += m_feedbackReadbacks[i]->GetMinMipMapSize();
#if RESOLVE_TO_TEXTURE
            m_feedbackReadbacks[i]->ResolveFeedback(pCommandList, m_sharedResolvedResources[i].Get());
#else
            m_feedbackReadbacks[i]->ResolveFeedback(pCommandList);
#endif
        }

        // transition all feedback resources RESOLVE_SOURCE->UAV
        // also transition the (non-opaque) resolved resources RESOLVE_DEST->COPY_SOURCE
        pCommandList->ResourceBarrier(barrierArraySize, m_barrierResolveSrcToUav.data());

#if RESOLVE_TO_TEXTURE
        for (UINT i = 0; i < (UINT)m_feedbackReadbacks.size(); i++)
        {
            m_feedbackReadbacks[i]->ReadbackFeedback(pCommandList, m_sharedResolvedResources[i].Get());
        }
#endif
        // now safe to clear feedback buffers
        ClearFeedback(pCommandList, m_feedbackReadbacks);

        m_gpuTimerResolve.EndTimer(pCommandList, m_renderFrameIndex);
        m_gpuTimerResolve.ResolveTimer(pCommandList, m_renderFrameIndex);

        // feedback array consumed, clear for next frame
        m_feedbackReadbacks.clear();

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
