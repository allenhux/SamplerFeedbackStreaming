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
#include "Streaming.h" // for ComPtr
#include "DataUploader.h"
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
        // used by ~ManagerBase(), UseDirectStorage(), and SetVisualizationMode()
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
        // used by destructor, to allocate shared residence map and shared clear uav heap, SetVisualizationMode(), and UseDirectStorage()
        std::vector<ResourceBase*> m_streamingResources;

        // track the heaps resources depend on
        // even after a call to Destroy, keep alive until dependent resources are deleted
        std::vector<Heap*> m_streamingHeaps;

        SFS::DataUploader m_dataUploader;

        // each SFSResource writes current uploaded tile state to min mip map, separate data for each frame
        // internally, use a single buffer containing all the residency maps
        SFS::UploadBuffer m_residencyMap;

        // lock between ResidencyThread and main thread around shared residency map
        Lock m_residencyMapLock;

        // list of newly created resources
        LockedContainer<std::vector<ResourceBase*>> m_newResources;

        // resources that are being flushed
        LockedContainer<std::set<ResourceBase*>> m_flushResources;

        // resources that have been Destroy()ed
        LockedContainer<std::set<ResourceBase*>> m_removeResources;

        // resources where feedback or eviction requested
        std::set<ResourceBase*> m_pendingResources;

        // a thread to process feedback (when available) and queue tile loads / evictions to datauploader
        ProcessFeedbackThread m_processFeedbackThread;

        // are we between BeginFrame and EndFrame? useful for debugging
        std::atomic<bool> m_withinFrame{ false };

        void StartThreads();

        // direct queue is used to monitor progress of render frames so we know when feedback buffers are ready to be used
        ComPtr<ID3D12CommandQueue> m_directCommandQueue;

        std::set<ResourceBase*> m_feedbackReadbacks;
        float m_numTexelsQueued{ 0 }; // for computing texels/ms

#if RESOLVE_TO_TEXTURE
        // NOTE: this heap and array of resources is allocated in SFSManager.cpp
        ComPtr<ID3D12Heap> m_resolvedResourceHeap;
        // feedback resolved on gpu
        std::vector<ComPtr<ID3D12Resource>> m_sharedResolvedResources;
#endif

        // should clear feedback buffer before first use
        std::set<ResourceBase*> m_firstTimeClears;
        using BarrierList = std::vector<D3D12_RESOURCE_BARRIER>;
        BarrierList m_barrierUavToResolveSrc; // transition copy source to resolve dest
        BarrierList m_barrierResolveSrcToUav; // transition resolve dest to copy source

        // the min mip map is shared. it must be created (at least) every time a SFSResource is created/destroyed
        void CreateMinMipMapView(D3D12_CPU_DESCRIPTOR_HANDLE in_descriptor);

        // do not delete in-use resources - wait until swapchaincount frames have passed
        std::vector<ComPtr<ID3D12Resource>> m_oldSharedResidencyMaps;
        std::vector<ComPtr<ID3D12DescriptorHeap>> m_oldSharedClearUavHeaps;
        // adds old resource to m_oldSharedResidencyMaps so it can be safely released after n frames
        void AllocateSharedResidencyMap();

        // descriptor heaps to clear feedback resources, shared by all resources
        ComPtr<ID3D12DescriptorHeap> m_sharedClearUavHeapNotBound;
        ComPtr<ID3D12DescriptorHeap> m_sharedClearUavHeapBound;

        void AllocateSharedClearUavHeap(UINT in_numDescriptors);

        // after packed mips have arrived for new resources, transition them from copy_dest
        std::vector<ResourceBase*> m_packedMipTransitionResources;

        // handle FlushResources()
        void FlushResourcesInternal();
        // remove resources from m_streamingResources after call to Destroy()
        void RemoveResources();
        // delete heaps that have been requested via Destroy()
        void RemoveHeaps();
        //-------------------------------------------
        // statistics
        //-------------------------------------------
        const bool m_traceCaptureMode{ false };
        std::atomic<UINT> m_numTotalEvictions{ 0 };
        std::atomic<UINT> m_numTotalUploads{ 0 };
        std::atomic<UINT> m_numTotalSubmits{ 0 }; // number of DS::Submit() calls
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
