//==============================================================
// Copyright © Intel Corporation
//
// SPDX-License-Identifier: MIT
// =============================================================

#include "pch.h"

#include "ManagerBase.h"
#include "ResourceBase.h"
#include "SFSHeap.h"
#include "DebugHelper.h"

// agility sdk 1.613.3 or later required for gpu upload heaps 
extern "C" { __declspec(dllexport) extern const UINT D3D12SDKVersion = 615; }
extern "C" { __declspec(dllexport) extern const char* D3D12SDKPath = ".\\D3D12\\"; }

//=============================================================================
// constructor for streaming library base class
//=============================================================================
SFS::ManagerBase::ManagerBase(const SFSManagerDesc& in_desc, ID3D12Device8* in_pDevice) :// required for constructor
    m_numSwapBuffers(in_desc.m_swapChainBufferCount)
    // delay eviction by enough to not affect a pending frame
    , m_evictionDelay(std::max(in_desc.m_swapChainBufferCount + 1, in_desc.m_evictionDelay))
    , m_directCommandQueue(in_desc.m_pDirectCommandQueue)
    , m_device(in_pDevice)
    , m_dataUploader(in_pDevice, in_desc.m_maxNumCopyBatches, in_desc.m_stagingBufferSizeMB, in_desc.m_maxTileMappingUpdatesPerApiCall, (int)in_desc.m_threadPriority)
    , m_traceCaptureMode{ in_desc.m_traceCaptureMode }
    , m_oldSharedResidencyMaps(in_desc.m_swapChainBufferCount + 1, nullptr)
    , m_oldSharedClearUavHeaps(in_desc.m_swapChainBufferCount + 1, nullptr)
    , m_processFeedbackThread((ManagerPFT*)this, m_dataUploader, in_desc.m_minNumUploadRequests, (int)in_desc.m_threadPriority)
    , m_residencyThread((ManagerRT*)this, m_processFeedbackThread.GetFlushResources(), (int)in_desc.m_threadPriority)
{
    ASSERT(D3D12_COMMAND_LIST_TYPE_DIRECT == m_directCommandQueue->GetDesc().Type);

    ThrowIfFailed(in_pDevice->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_frameFence)));
    m_frameFence->SetName(L"SFS::ManagerBase::m_frameFence");

    // GPU upload heap support?
    {
        auto uploadHeapProperties = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
        D3D12_FEATURE_DATA_D3D12_OPTIONS16 options{};
        m_device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS16, &options, sizeof(options));

        m_gpuUploadHeapSupported = options.GPUUploadHeapSupported;
    }

    // advance frame number to the first frame...
    m_frameFenceValue++;

    UseDirectStorage(in_desc.m_useDirectStorage);

    StartThreads();
}

//-----------------------------------------------------------------------------
// destructor
//-----------------------------------------------------------------------------
SFS::ManagerBase::~ManagerBase()
{
    // force DataUploader to flush now, rather than waiting for its destructor
    Finish();

    // the application may have deleted some, but not all, of the resources


    // if a resource is in removeResources, it has already been deleted.
    std::set<ResourceBase*> removeResources;
    m_removeResources.swap(removeResources);

    // possible for a resource to be in both m_removeResources and m_streamingResources if
    //    an application destructor calls Resource::Destroy() but then doesn't call Begin/EndFrame()
    for (auto p : m_streamingResources)
    {
        if (!removeResources.contains(p))
        {
            delete p;
        }
    }

    // possible for a resource to be in newResources if app shut down before resource finished initializing
    std::vector<ResourceBase*> newResources;
    m_newResources.swap(newResources);
    for (auto p : newResources)
    {
        if (!removeResources.contains(p))
        {
            delete p;
        }
    }

    for (auto p : m_streamingHeaps)
    {
        delete p;
    }
}

//-----------------------------------------------------------------------------
// kick off threads that continuously streams tiles
// gives StreamingResources opportunities to update feedback
//-----------------------------------------------------------------------------
void SFS::ManagerBase::StartThreads()
{
    // process sampler feedback buffers, generate upload and eviction commands
    m_processFeedbackThread.Start();
    // update residency maps
    m_residencyThread.Start();
}

//-----------------------------------------------------------------------------
// flushes all internal queues
// submits all outstanding command lists
// stops all processing threads
//-----------------------------------------------------------------------------
void SFS::ManagerBase::Finish()
{
    ASSERT(!GetWithinFrame());
 
    m_processFeedbackThread.Stop();
    m_residencyThread.Stop();

    // now we are no longer producing work for the DataUploader, so its commands can be drained
    m_dataUploader.FlushCommands();
}

//-----------------------------------------------------------------------------
// allocate residency map buffer large enough all SFSResources
// assign offsets to new resources and update all resources on resource allocation
// also allocate shared descriptor heaps for clear UAV
//-----------------------------------------------------------------------------
void SFS::ManagerBase::AllocateSharedResidencyMap()
{
    static constexpr UINT alignmentMask = std::hardware_destructive_interference_size - 1; // cache line size
    static constexpr UINT gpuPageSizeMask = (64 * 1024) - 1;

    // if new resources, will probably need to expand clear descriptor heap. just always re-allocate.
    AllocateSharedClearUavHeap((UINT)m_streamingResources.size());

    // get the buffer size
    UINT bufferSize = 0;
    if (nullptr != m_residencyMap.GetResource())
    {
        bufferSize = (UINT)m_residencyMap.GetResource()->GetDesc().Width;
    }

    // need to lock out ResidencyThread
    // because we are setting offsets and potentially creating a new resource
    m_residencyMapLock.Acquire();

    // because there may have been deletions, there could be "holes"
    // rather than building a proper memory manager, just reset everything on any allocation
    UINT requiredSize = 0;
    UINT descriptorOffset = 0;
    const auto srvUavCbvDescriptorSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    for (auto p : m_streamingResources)
    {
        p->SetResidencyMapOffset(requiredSize);
        // align to cache line size
        requiredSize += (p->GetMinMipMapSize() + alignmentMask) & ~alignmentMask;

        // set clear uav descriptor heap offset
        // note these are views are invalid until set within NotifyPackedMips()
        p->SetClearUavDescriptorOffset(descriptorOffset);
        descriptorOffset += srvUavCbvDescriptorSize;
    }

    if (requiredSize > bufferSize)
    {
        // align to gpu page size
        requiredSize = (requiredSize + gpuPageSizeMask) & ~gpuPageSizeMask;

        auto uploadHeapProperties = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
        if (m_gpuUploadHeapSupported)
        {
            uploadHeapProperties = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_GPU_UPLOAD);
            // per DirectX-Specs, "CPUPageProperty and MemoryPoolPreference must be ..._UNKNOWN"
            ASSERT(D3D12_CPU_PAGE_PROPERTY_UNKNOWN == uploadHeapProperties.CPUPageProperty);
            ASSERT(D3D12_MEMORY_POOL_UNKNOWN == uploadHeapProperties.MemoryPoolPreference);
        }

		// defer deletion of current residency map
        {
            auto i = m_frameFenceValue % m_oldSharedResidencyMaps.size();
            m_oldSharedResidencyMaps[i] = m_residencyMap.GetResource();
        }

        m_residencyMap.Allocate(m_device.Get(), requiredSize, uploadHeapProperties);
    }
    m_residencyMapLock.Release();

    auto pDest = m_residencyMap.GetData();
    for (auto p : m_streamingResources)
    {
        // copy current minmipmap state or initialize to default state
        p->WriteMinMipMap((UINT8*)pDest);
    }
}

//-----------------------------------------------------------------------------
// create a shared heap for clearing the feedback resources
// this heap will not be bound to a command list
//-----------------------------------------------------------------------------
void SFS::ManagerBase::AllocateSharedClearUavHeap(UINT in_numDescriptors)
{
    if ((nullptr == m_sharedClearUavHeapNotBound.Get()) || (m_sharedClearUavHeapNotBound->GetDesc().NumDescriptors < in_numDescriptors))
    {
        {
            auto i = m_frameFenceValue % m_oldSharedClearUavHeaps.size();
            m_oldSharedClearUavHeaps[i] = m_sharedClearUavHeapBound.Get();
        }

        D3D12_DESCRIPTOR_HEAP_DESC desc = {};
        desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        desc.NumDescriptors = in_numDescriptors;
        desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;

        ThrowIfFailed(m_device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&m_sharedClearUavHeapNotBound)));
        m_sharedClearUavHeapNotBound->SetName(L"m_sharedClearUavHeapNotBound");

        desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        ThrowIfFailed(m_device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&m_sharedClearUavHeapBound)));
        m_sharedClearUavHeapNotBound->SetName(L"m_sharedClearUavHeapBound");
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

//-----------------------------------------------------------------------------
// stop tracking resources that have been Destroy()ed
//-----------------------------------------------------------------------------
void SFS::ManagerBase::RemoveResources()
{
    if (m_removeResources.size())
    {
        std::set<ResourceBase*> tmp;
        m_removeResources.swap(tmp);
        ContainerRemove(m_streamingResources, tmp);
    }
}

//-----------------------------------------------------------------------------
// Handle FlushResources()
// called by BeginFrame()
//-----------------------------------------------------------------------------
void SFS::ManagerBase::FlushResourcesInternal()
{
    if (m_flushResources.size())
    {
        std::set<ResourceBase*> removeResources;
        m_flushResources.swap(removeResources);

        ContainerRemove(m_pendingResources, removeResources);
        ContainerRemove(m_packedMipTransitionResources, removeResources);
    }
}

//-----------------------------------------------------------------------------
// destroy heaps that are no longer depended upon
// FIXME: Heaps need refcounting so they aren't destroyed when objects don't have packed mips
// FIXME: test this?
//-----------------------------------------------------------------------------
void SFS::ManagerBase::RemoveHeaps()
{
    for (UINT i = 0; i < m_streamingHeaps.size();)
    {
        auto p = m_streamingHeaps[i];
        if (p->GetDestroyable() && (0 == p->GetAllocator().GetAllocated()))
        {
            delete p;
            m_streamingHeaps[i] = m_streamingHeaps.back();
            m_streamingHeaps.pop_back();
        }
        else
        {
            i++;
        }
    }
}

//-----------------------------------------------------------------------------
// add a feedback readback to the current set of readbacks
//-----------------------------------------------------------------------------
void SFS::ManagerBase::AddReadback(ResourceBase* in_pResource)
{
    if (nullptr == m_pCurrentReadbackSet)
    {
        for (auto& s : m_readbackSets)
        {
            if (s.m_free)
            {
                m_pCurrentReadbackSet = &s;
                m_pCurrentReadbackSet->m_resources.clear();
                break;
            }
        }
    }

    if (m_pCurrentReadbackSet)
    {
        m_pCurrentReadbackSet->m_resources.push_back(in_pResource);
    }
    // else ignore attempt to add readback
}

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
void SFS::ManagerBase::QueueFeedback(SFSResource* in_pResource)
{
    auto pResource = (ResourceBase*)in_pResource;

    if (pResource->GetFirstUse())
    {
        pResource->GetFirstUse() = false;
        m_firstTimeClears.insert(pResource);
    }

    if ((m_maxNumResolvesPerFrame > m_feedbackReadbacks.size()) && (!m_feedbackReadbacks.contains(pResource)))
    {
        m_feedbackReadbacks.insert(pResource);
        m_numTexelsQueued += pResource->GetMinMipMapSize();

        auto pOpaque = pResource->GetOpaqueFeedback();
        m_barrierUavToResolveSrc.push_back(CD3DX12_RESOURCE_BARRIER::Transition(pOpaque, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_RESOLVE_SOURCE));

        // after resolving, transition the opaque resources back to UAV. Transition the resolve destination to copy source for read back on cpu
        m_barrierResolveSrcToUav.push_back(CD3DX12_RESOURCE_BARRIER::Transition(pOpaque, D3D12_RESOURCE_STATE_RESOLVE_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS));


#if RESOLVE_TO_TEXTURE
        // resolve to texture incurs a subsequent copy to linear buffer
        UINT i = (UINT)m_feedbackReadbacks.size() - 1;
        auto pResolved = m_sharedResolvedResources[i].Get();
        m_barrierUavToResolveSrc.push_back(CD3DX12_RESOURCE_BARRIER::Transition(pResolved, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RESOLVE_DEST));
        m_barrierResolveSrcToUav.push_back(CD3DX12_RESOURCE_BARRIER::Transition(pResolved, D3D12_RESOURCE_STATE_RESOLVE_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE));
#endif
    }

    // NOTE: feedback buffers will be cleared after readback
}
