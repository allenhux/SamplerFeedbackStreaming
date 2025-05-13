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
    private:
        //-----------------------------------------------------------------
        // external APIs
        //-----------------------------------------------------------------
        virtual void Destroy() override;
        virtual SFSHeap* CreateHeap(UINT in_maxNumTilesHeap) override;
        virtual SFSResource* CreateResource(const struct SFSResourceDesc& in_desc,
            SFSHeap* in_pHeap, const std::wstring& in_filename) override;
        virtual void BeginFrame(ID3D12DescriptorHeap* in_pDescriptorHeap, D3D12_CPU_DESCRIPTOR_HANDLE in_minmipmapDescriptorHandle) override;
        virtual CommandLists EndFrame() override;
        virtual void UseDirectStorage(bool in_useDS) override;
        virtual float GetGpuTime() const override;
        virtual void SetVisualizationMode(UINT in_mode) override;
        virtual void CaptureTraceFile(bool in_captureTrace) override;
        virtual float GetCpuProcessFeedbackTime() override;
        virtual UINT GetTotalNumUploads() const override;
        virtual UINT GetTotalNumEvictions() const override;
        virtual float GetTotalTileCopyLatency() const override;
        virtual UINT GetTotalNumSubmits() const override;
        //-----------------------------------------------------------------
        // end external APIs
        //-----------------------------------------------------------------
    public:
        // external api, but also used internally
        virtual bool GetWithinFrame() const override { return m_withinFrame; }

        void QueueFeedback(SFSResource* in_pResource, D3D12_GPU_DESCRIPTOR_HANDLE in_gpuDescriptor);

        // force all outstanding commands to complete.
        // used by ~ManagerBase() and to delete an SFSResource
        void Finish();

        // delete resources that have been requested via Remove()
        void RemoveResources();

        ManagerBase(const struct SFSManagerDesc& in_desc, ID3D12Device8* in_pDevice); // required for constructor

        virtual ~ManagerBase();

    protected:
        ComPtr<ID3D12Device8> m_device;

        // the frame fence is used to optimize readback of feedback by SFSResource
        // only read back the feedback after the frame that writes to it has completed
        ComPtr<ID3D12Fence> m_frameFence;
        UINT64 m_frameFenceValue{ 0 };

        const UINT m_numSwapBuffers;
        const UINT m_evictionDelay;

        SFS::DataUploader m_dataUploader;

        // each SFSResource writes current uploaded tile state to min mip map, separate data for each frame
        // internally, use a single buffer containing all the residency maps
        struct ResidencyMap : SFS::UploadBuffer
        {
            UINT m_bytesUsed{ 0 };
        } m_residencyMap;

        // lock between ResidencyThread and main thread around shared residency map
        Lock m_residencyMapLock;

        // track the objects that this resource created
        // used to discover which resources have been updated within a frame
        std::vector<ResourceBase*> m_streamingResources;

        std::set<ResourceBase*> m_removeResources; // resources that are to be removed (deleted)
        std::vector<ResourceBase*> m_pendingResources; // resources where feedback or eviction requested

        // a thread to update residency maps based on feedback
        ResidencyThread m_residencyThread;

        // a thread to process feedback (when available) and queue tile loads / evictions to datauploader
        ProcessFeedbackThread m_processFeedbackThread;

    private:
        // direct queue is used to monitor progress of render frames so we know when feedback buffers are ready to be used
        ComPtr<ID3D12CommandQueue> m_directCommandQueue;

        struct FeedbackReadback
        {
            ResourceBase* m_pStreamingResource;
            D3D12_GPU_DESCRIPTOR_HANDLE m_gpuDescriptor;
        };
        std::vector<FeedbackReadback> m_feedbackReadbacks;

        // should clear feedback buffer before first use
        std::vector<FeedbackReadback> m_firstTimeClears;

        bool m_threadsRunning{ false };
        void StartThreads();
        void StopThreads(); // stop only SFSManager threads. Used by Finish()

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

        SFS::BarrierList m_barrierUavToResolveSrc; // transition copy source to resolve dest
        SFS::BarrierList m_barrierResolveSrcToUav; // transition resolve dest to copy source
        SFS::BarrierList m_packedMipTransitionBarriers; // transition packed-mips from common (copy dest)

        UINT m_renderFrameIndex{ 0 }; // between 0 and # swap buffers

        D3D12GpuTimer m_gpuTimerResolve; // time for feedback resolve

        RawCpuTimer m_cpuTimer;
        INT64 m_previousFeedbackTime{ 0 }; // m_processFeedbackTime at time of last query
        float m_processFeedbackFrameTime{ 0 }; // cpu time spent processing feedback for the most recent frame

        // are we between BeginFrame and EndFrame? useful for debugging
        std::atomic<bool> m_withinFrame{ false };

        struct CommandList
        {
            ComPtr<ID3D12GraphicsCommandList1> m_commandList;
            std::vector<ComPtr<ID3D12CommandAllocator>> m_allocators;
        };
        std::vector<CommandList> m_commandLists;

        // the min mip map is shared. it must be created (at least) every time a SFSResource is created/destroyed
        void CreateMinMipMapView(D3D12_CPU_DESCRIPTOR_HANDLE in_descriptor);

        // do not delete in-use resources - wait until swapchaincount frames have passed
        std::vector<ComPtr<ID3D12Resource>> m_oldSharedResidencyMaps;
        // adds old resource to m_oldSharedResidencyMaps so it can be safely released after n frames
        void AllocateSharedResidencyMap(D3D12_CPU_DESCRIPTOR_HANDLE in_descriptorHandle,
            std::vector<ResourceBase*>& in_newResources);

        // heap to clear feedback resources, shared by all
        ComPtr<ID3D12DescriptorHeap> m_sharedClearUavHeap;
        void AllocateSharedClearUavHeap();
        void CreateClearDescriptors();

        // list of newly created resources
        // for each, call AllocateResidencyMap() and AllocateSharedClearUavHeap()
        std::vector<ResourceBase*> m_newResources;

        // after packed mips have arrived for new resources, transition them from copy_dest
        std::vector<ResourceBase*> m_packedMipTransitionResources;

        //-------------------------------------------
        // statistics
        //-------------------------------------------
        const bool m_traceCaptureMode{ false };
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
