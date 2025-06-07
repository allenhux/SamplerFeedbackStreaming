//==============================================================
// Copyright © Intel Corporation
//
// SPDX-License-Identifier: MIT
// =============================================================

/*=============================================================================
Implementation of SFS Manager
=============================================================================*/

#pragma once

#include <d3d12.h>
#include <vector>
#include <memory>
#include <thread>

#include "SamplerFeedbackStreaming.h"
#include "D3D12GpuTimer.h"
#include "Timer.h"
#include "Streaming.h" // for ComPtr
#include "DataUploader.h"
#include "ResidencyThread.h"
#include "ProcessFeedbackThread.h"

//=============================================================================
// manager for tiled resources
// contains shared objects, especially the heap
// performs asynchronous copies
// use this to create the SFSResource
//=============================================================================
namespace SFS
{
    class ResourceBase;
    class Heap;
    struct UpdateList;

    class ManagerBase : public ::SFSManager
    {
    public:
        // external api, but also used internally
        virtual bool GetWithinFrame() const override { return m_withinFrame; }
        virtual void UseDirectStorage(bool in_useDS) override;

        void QueueFeedback(SFSResource* in_pResource);

        // force all outstanding commands to complete.
        // used by ~ManagerBase() and to delete an SFSResource
        void Finish();

        ManagerBase(const struct SFSManagerDesc& in_desc, ID3D12Device8* in_pDevice);

        virtual ~ManagerBase();

    protected:
        ComPtr<ID3D12Device8> m_device;

        // the frame fence is used to optimize readback of feedback by SFSResource
        // only read back the feedback after the frame that writes to it has completed
        ComPtr<ID3D12Fence> m_frameFence;
        UINT64 m_frameFenceValue{ 0 };

        const UINT m_numSwapBuffers;
        const UINT m_evictionDelay;

        // this is a function of the size of m_resolvedResourceHeap
        UINT m_maxNumResolvesPerFrame{ UINT(-1) };

        // track the objects that this resource created
        // used to discover which resources have been updated within a frame
        std::vector<ResourceBase*> m_streamingResources;

        // track the heaps resources depend on
        // even after a call to Destroy, keep alive until dependent resources are deleted
        std::vector<Heap*> m_streamingHeaps;

        SFS::DataUploader m_dataUploader;

        // each SFSResource writes current uploaded tile state to min mip map, separate data for each frame
        // internally, use a single buffer containing all the residency maps
        struct ResidencyMap : SFS::UploadBuffer
        {
            UINT m_bytesUsed{ 0 };
        } m_residencyMap;

        // lock between ResidencyThread and main thread around shared residency map
        Lock m_residencyMapLock;

        // list of newly created resources
        // for each, call AllocateResidencyMap() and AllocateSharedClearUavHeap()
        std::vector<ResourceBase*> m_newResources;
        std::set<ResourceBase*> m_removeResources; // resources that are to be removed (deleted)
        std::vector<ResourceBase*> m_pendingResources; // resources where feedback or eviction requested

        // a thread to update residency maps based on feedback
        ResidencyThread m_residencyThread;

        // a thread to process feedback (when available) and queue tile loads / evictions to datauploader
        ProcessFeedbackThread m_processFeedbackThread;

        // are we between BeginFrame and EndFrame? useful for debugging
        std::atomic<bool> m_withinFrame{ false };

        //-------------------------------------------
        // statistics
        //-------------------------------------------
        const bool m_traceCaptureMode{ false };

        void StartThreads();

        // direct queue is used to monitor progress of render frames so we know when feedback buffers are ready to be used
        ComPtr<ID3D12CommandQueue> m_directCommandQueue;

        std::vector<ResourceBase*> m_feedbackReadbacks;

        // should clear feedback buffer before first use
        std::vector<ResourceBase*> m_firstTimeClears;

        //---------------------------------------------------------------------------
        // SFSM creates 2 command lists to be executed Before & After application draw
        // these clear & resolve feedback buffers, coalescing all their barriers
        //---------------------------------------------------------------------------
        enum class CommandListName
        {
            After,    // after all draw calls: resolve feedback
            Num
        };
        ID3D12GraphicsCommandList1* GetCommandList(CommandListName in_name) { return m_commandLists[UINT(in_name)].m_commandList.Get(); }

        UINT m_renderFrameIndex{ 0 }; // between 0 and # swap buffers

        struct CommandList
        {
            ComPtr<ID3D12GraphicsCommandList1> m_commandList;
            std::vector<ComPtr<ID3D12CommandAllocator>> m_allocators;
        };
        std::vector<CommandList> m_commandLists;

        // track per-frame feedback readback
        struct ReadbackSet
        {
            std::atomic<bool> m_free{ true };
            std::vector<ResourceBase*> m_resources;
            UINT64 m_fenceValue{ 0 };
        };
        std::list<ReadbackSet> m_readbackSets;
        ReadbackSet* m_pCurrentReadbackSet{ nullptr };
        void AddReadback(ResourceBase* in_pResource);

        // the min mip map is shared. it must be created (at least) every time a SFSResource is created/destroyed
        void CreateMinMipMapView(D3D12_CPU_DESCRIPTOR_HANDLE in_descriptor);

        // do not delete in-use resources - wait until swapchaincount frames have passed
        std::vector<ComPtr<ID3D12Resource>> m_oldSharedResidencyMaps;
        // adds old resource to m_oldSharedResidencyMaps so it can be safely released after n frames
        void AllocateSharedResidencyMap(D3D12_CPU_DESCRIPTOR_HANDLE in_descriptorHandle);

        // descriptor heaps to clear feedback resources, shared by all resources
        ComPtr<ID3D12DescriptorHeap> m_sharedClearUavHeapNotBound;
        ComPtr<ID3D12DescriptorHeap> m_sharedClearUavHeapBound;

        void AllocateSharedClearUavHeap();

        // after packed mips have arrived for new resources, transition them from copy_dest
        std::vector<ResourceBase*> m_packedMipTransitionResources;

        // delete resources that have been requested via Remove()
        void RemoveResources();
        // delete heaps that have been requested via Destroy()
        void RemoveHeaps();
    private:
		bool m_gpuUploadHeapSupported{ false }; // if supported, use GPU upload heaps for residency maps
    };
}
/*
NOTE:

Because there are several small per-object operations, there can be a lot of barriers if there are many objects.
These have all been coalesced into the per-frame AFTER command list:

after draw commands:
    barriers for packed mips
    barriers for opaque feedback transition UAV->RESOLVE_SOURCE
    feedback resolves
    barriers for opaque feedback transition RESOLVE_SOURCE->UAV
    feedback clears

    // unnecessary when resolving directly to cpu:
    barriers for resolved feedback transition RESOLVE_DEST->COPY_SOURCE
    resolved resource readback copies (to cpu)
    barriers for resolved feedback transition COPY_SOURCE->RESOLVE_DEST
*/
