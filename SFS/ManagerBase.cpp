//==============================================================
// Copyright © Intel Corporation
//
// SPDX-License-Identifier: MIT
// =============================================================

#include "pch.h"

#include "ManagerBase.h"
#include "ResourceBase.h"
#include "SFSHeap.h"
#include "BitVector.h"
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
    , m_renderFrameIndex(0)
    , m_directCommandQueue(in_desc.m_pDirectCommandQueue)
    , m_device(in_pDevice)
    , m_commandLists((UINT)CommandListName::Num)
    , m_dataUploader(in_pDevice, in_desc.m_maxNumCopyBatches, in_desc.m_stagingBufferSizeMB, in_desc.m_maxTileMappingUpdatesPerApiCall, (int)in_desc.m_threadPriority)
    , m_traceCaptureMode{ in_desc.m_traceCaptureMode }
    , m_oldSharedResidencyMaps(in_desc.m_swapChainBufferCount + 1, nullptr)
    , m_processFeedbackThread((ManagerPFT*)this, m_dataUploader, in_desc.m_minNumUploadRequests, (int)in_desc.m_threadPriority)
    , m_residencyThread((ManagerRT*)this, m_processFeedbackThread.GetRemoveResources(), (int)in_desc.m_threadPriority)
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

SFS::ManagerBase::~ManagerBase()
{
    // force DataUploader to flush now, rather than waiting for its destructor
    Finish();
    for (auto p : m_removeResources)
    {
        delete p;
    }
    for (auto p : m_streamingHeaps)
    {
        p->Destroy();
    }
    RemoveHeaps();
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
// allocate residency map buffer large enough for numswapbuffers * min mip map buffers for each SFSResource
// assign offsets to new resources and update all resources on resource allocation
// descriptor handle required to update the assoiated shader resource view
//-----------------------------------------------------------------------------
void SFS::ManagerBase::AllocateSharedResidencyMap(D3D12_CPU_DESCRIPTOR_HANDLE in_descriptorHandle)
{
    static constexpr UINT alignmentMask = std::hardware_destructive_interference_size - 1; // cache line size
    static constexpr UINT gpuPageSizeMask = (64 * 1024) - 1;

    // if new resources, will probably need to expand clear descriptor heap. just always re-allocate.
    AllocateSharedClearUavHeap();

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
    for (auto r : m_streamingResources)
    {
        r->SetResidencyMapOffset(requiredSize);
        // align to cache line size
        requiredSize += (r->GetMinMipMapSize() + alignmentMask) & ~alignmentMask;
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
            auto p = m_residencyMap.Detach();
            auto i = m_frameFenceValue % m_oldSharedResidencyMaps.size();
            m_oldSharedResidencyMaps[i].Attach(p);
        }

        m_residencyMap.Allocate(m_device.Get(), requiredSize, uploadHeapProperties);
    }
    m_residencyMapLock.Release();

    CreateMinMipMapView(in_descriptorHandle);

    // remember how many bytes are in use
    m_residencyMap.m_bytesUsed = requiredSize;

    auto srvUavCbvDescriptorSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    auto pDest = m_residencyMap.GetData();
    UINT offset = 0;
    for (auto r : m_streamingResources)
    {
        // copy current minmipmap state or initialize to default state
        r->WriteMinMipMap((UINT8*)pDest);
        // set clear uav descriptor heap offset
        // note these are views are invalid until set within NotifyPackedMips()
        r->SetClearUavDescriptorOffset(offset);
        offset += srvUavCbvDescriptorSize;
    }
}

//-----------------------------------------------------------------------------
// create a shared heap for clearing the feedback resources
// this heap will not be bound to a command list
//-----------------------------------------------------------------------------
void SFS::ManagerBase::AllocateSharedClearUavHeap()
{
    UINT numDescriptorsNeeded = (UINT)m_streamingResources.size();
    if ((nullptr == m_sharedClearUavHeapNotBound.Get()) || (m_sharedClearUavHeapNotBound->GetDesc().NumDescriptors < numDescriptorsNeeded))
    {
        D3D12_DESCRIPTOR_HEAP_DESC desc = {};
        desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        desc.NumDescriptors = numDescriptorsNeeded;
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
// delete resources that have been requested via Remove()
// only used by BeginFrame()
//-----------------------------------------------------------------------------
void SFS::ManagerBase::RemoveResources()
{
    if (m_removeResources.size())
    {
        ContainerRemove(m_streamingResources, m_removeResources);
        ContainerRemove(m_pendingResources, m_removeResources);
        ContainerRemove(m_packedMipTransitionResources, m_removeResources);

        // theoretically possible? would have to create and destroy the same resource in the same frame
#ifdef _DEBUG
        for (auto p : m_newResources)
        {
            ASSERT(!m_removeResources.contains(p));
        }
        //ContainerRemove(m_newResources, m_removeResources);
#endif
        m_processFeedbackThread.AsyncDestroyResources(m_removeResources);
        m_removeResources.clear();
    }
}

//-----------------------------------------------------------------------------
// destroy heaps that are no longer depended upon
// FIXME: when to call this during runtime? also keep an array of pending remove heaps?
//-----------------------------------------------------------------------------
void SFS::ManagerBase::RemoveHeaps()
{
    for (auto& p : m_streamingHeaps)
    {
        if (p->GetDestroyable() && (0 == p->GetAllocator().GetAllocated()))
        {
            delete p;
            p = nullptr;
        }
    }
    m_streamingHeaps.erase(std::remove(m_streamingHeaps.begin(), m_streamingHeaps.end(), nullptr), m_streamingHeaps.end());
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
                break;
            }
        }
    }
    if (nullptr == m_pCurrentReadbackSet)
    {
        m_readbackSets.emplace_back();
        m_pCurrentReadbackSet = &m_readbackSets.back();
        m_pCurrentReadbackSet->m_resources.reserve(m_maxNumResolvesPerFrame);
    }

    // something is probably wrong if # readback sets vastly exceeds # swapchain buffers
    // FIXME: block?
    ASSERT(m_readbackSets.size() < 5 * m_numSwapBuffers);

    m_pCurrentReadbackSet->m_resources.push_back(in_pResource);
}

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
void SFS::ManagerBase::QueueFeedback(SFSResource* in_pResource)
{
    auto pResource = (SFS::ResourceBase*)in_pResource;

    if (pResource->FirstUse())
    {
        m_firstTimeClears.push_back(pResource);
    }

    if (m_maxNumResolvesPerFrame > m_feedbackReadbacks.size())
    {
        // FIXME: only add the resource one time per frame!
        m_feedbackReadbacks.push_back(pResource);
        //AddReadback(pResource); // FIXME PoC not robust
    }

    // NOTE: feedback buffers will be cleared after readback, in CommandListName::After
}
