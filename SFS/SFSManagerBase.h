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

//=============================================================================
// manager for tiled resources
// contains shared objects, especially the heap
// performs asynchronous copies
// use this to create the SFSResource
//=============================================================================
namespace SFS
{
    class ResourceBase;
    class DataUploader;
    class Heap;
    struct UpdateList;

    class ManagerBase : public ::SFSManager
    {
    public:
        //-----------------------------------------------------------------
        // external APIs
        //-----------------------------------------------------------------
        virtual void Destroy() override;
        virtual SFSHeap* CreateHeap(UINT in_maxNumTilesHeap) override;
        virtual SFSResource* CreateResource(const std::wstring& in_filename, SFSHeap* in_pHeap) override;
        virtual void BeginFrame(ID3D12DescriptorHeap* in_pDescriptorHeap, D3D12_CPU_DESCRIPTOR_HANDLE in_minmipmapDescriptorHandle) override;
        virtual void QueueFeedback(SFSResource* in_pResource, D3D12_GPU_DESCRIPTOR_HANDLE in_gpuDescriptor) override;
        virtual CommandLists EndFrame() override;
        virtual void UseDirectStorage(bool in_useDS) override;
        virtual bool GetWithinFrame() const  override { return m_withinFrame; }
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

        //--------------------------------------------
        // force all outstanding commands to complete.
        // used internally when everthing must drain, e.g. to delete or create a SFSResource
        //--------------------------------------------
        void Finish();

        ManagerBase(const struct SFSManagerDesc& in_desc, ID3D12Device8* in_pDevice); // required for constructor

        virtual ~ManagerBase();

    protected:
        ComPtr<ID3D12Device8> m_device;

        const UINT m_numSwapBuffers;
        const UINT m_evictionDelay;

        // track the objects that this resource created
        // used to discover which resources have been updated within a frame
        std::vector<ResourceBase*> m_streamingResources;
        UINT64 m_frameFenceValue{ 0 };

        SFS::DataUploader m_dataUploader;

        // each SFSResource writes current uploaded tile state to min mip map, separate data for each frame
        // internally, use a single buffer containing all the residency maps
        SFS::UploadBuffer m_residencyMap;

        SFS::SynchronizationFlag m_residencyChangedFlag;

        // allocating/deallocating StreamingResources requires reallocation of shared resources
        bool m_numStreamingResourcesChanged{ false };

        std::atomic<bool> m_packedMipTransition{ false }; // flag that we need to transition a resource due to packed mips

    private:
        // direct queue is used to monitor progress of render frames so we know when feedback buffers are ready to be used
        ComPtr<ID3D12CommandQueue> m_directCommandQueue;

        // the frame fence is used to optimize readback of feedback by SFSResource
        // only read back the feedback after the frame that writes to it has completed
        ComPtr<ID3D12Fence> m_frameFence;

        struct FeedbackReadback
        {
            ResourceBase* m_pStreamingResource;
            D3D12_GPU_DESCRIPTOR_HANDLE m_gpuDescriptor;
        };
        std::vector<FeedbackReadback> m_feedbackReadbacks;

        ComPtr<ID3D12Resource> m_residencyMapLocal; // GPU copy of residency state

        SFS::SynchronizationFlag m_processFeedbackFlag;

        std::atomic<bool> m_havePackedMipsToLoad{ false };

        void StartThreads();
        void ProcessFeedbackThread();
        void StopThreads(); // stop only SFSManager threads. Used by Finish() and CreateResource()

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

        UINT m_renderFrameIndex{ 0 };

        D3D12GpuTimer m_gpuTimerResolve; // time for feedback resolve

        RawCpuTimer m_cpuTimer;
        std::atomic<INT64> m_processFeedbackTime{ 0 }; // sum of cpu timer times since start
        INT64 m_previousFeedbackTime{ 0 }; // m_processFeedbackTime at time of last query
        float m_processFeedbackFrameTime{ 0 }; // cpu time spent processing feedback for the most recent frame

        // are we between BeginFrame and EndFrame? useful for debugging
        std::atomic<bool> m_withinFrame{ false };

        void AllocateResidencyMap(D3D12_CPU_DESCRIPTOR_HANDLE in_descriptorHandle);

        struct CommandList
        {
            ComPtr<ID3D12GraphicsCommandList1> m_commandList;
            std::vector<ComPtr<ID3D12CommandAllocator>> m_allocators;
        };
        std::vector<CommandList> m_commandLists;

        const UINT m_maxTileMappingUpdatesPerApiCall;

        std::atomic<bool> m_threadsRunning{ false };

        const UINT m_minNumUploadRequests{ 2000 }; // heuristic to reduce Submit()s
        void SignalFileStreamer();
        int m_threadPriority{ 0 };

        // a thread to process feedback (when available) and queue tile loads / evictions to datauploader
        std::thread m_processFeedbackThread;

        // UpdateResidency thread's lifetime is bound to m_processFeedbackThread
        std::thread m_updateResidencyThread;

        // the min mip map is shared. it must be created (at least) every time a SFSResource is created/destroyed
        void CreateMinMipMapView(D3D12_CPU_DESCRIPTOR_HANDLE in_descriptor);

        std::vector<UINT> m_residencyMapOffsets; // one min mip map for each SFSResource

        ComPtr<ID3D12DescriptorHeap> m_sharedClearUavHeap; // CPU heap to clear feedback resources, shared by all
        void AllocateSharedClearUavHeap();

        //-------------------------------------------
        // statistics
        //-------------------------------------------
        std::atomic<UINT> m_numTotalSubmits{ 0 };
    };
}
/*
NOTE:

Because there are several small per-object operations, there can be a lot of barriers if there are many objects.
These have all been coalesced into 2 command lists per frame:

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
