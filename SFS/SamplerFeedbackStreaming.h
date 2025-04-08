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
Usage:

1. Create an SFSManager
2. Use SFSManager::CreateHeap() to create heaps to be used by 1 or more StreamingResources
3. Use SFSManager::CreateResource() to create 1 or more StreamingResources
    each SFSResource resides in a single heap

Draw loop:
1. BeginFrame() with the SFSManager (SFSM)
2. Draw your assets using the streaming textures, min-mip-map, and sampler feedback SRVs
    Optionally call SFSM::QueueFeedback() to get sampler feedback for this draw.
    SRVs can be created using SFSResource methods
3. EndFrame() (with the SFSM) returns 1 command list: afterDrawCommands
4. ExecuteCommandLists() with [yourCommandList, afterDrawCommands] command lists.
=============================================================================*/

#pragma once

// FIXME: resolve to buffer only supported in Win11 and some insider versions of Win10
// When resolving to texture, must copy to cpu-readable buffer from gpu texture (which cannot be in the readback heap)
// Setting this to 0 resolves directly to cpu-readable buffer
#define RESOLVE_TO_TEXTURE 1

//==================================================
// a streaming resource is associated with a single heap (in this implementation)
// multiple streaming resources can use the same heap
// SFSManager is used to create these
//==================================================
struct SFSHeap
{
    virtual void Destroy() = 0;

    virtual UINT GetNumTilesAllocated() const = 0;
};

//=============================================================================
// a fine-grained streaming, tiled resource
// SFSManager is used to create these
//=============================================================================
struct SFSResource
{
    virtual void Destroy() = 0;

    //--------------------------------------------
    // applications need access to the resources to create descriptors
    //--------------------------------------------
    virtual void CreateFeedbackView(ID3D12Device* in_pDevice, D3D12_CPU_DESCRIPTOR_HANDLE out_descriptor) = 0;
    virtual void CreateShaderResourceView(ID3D12Device* in_pDevice, D3D12_CPU_DESCRIPTOR_HANDLE in_descriptor) = 0;

    // shader reading min-mip-map buffer will want its dimensions
    virtual UINT GetMinMipMapWidth() const = 0;
    virtual UINT GetMinMipMapHeight() const = 0;

    // shader reading min-mip-map buffer will need an offset into the min-mip-map (residency map)
    // NOTE: all min mip maps are stored in a single buffer. offset into the buffer.
    virtual UINT GetMinMipMapOffset() const = 0;

    // check if the packed mips are loaded. application likely will not want to use this texture before they have loaded
    virtual bool GetPackedMipsResident() const = 0;

    // if a resource isn't visible, evict associated data
    // call any time
    virtual void QueueEviction() = 0;

    virtual ID3D12Resource* GetTiledResource() const = 0;

    virtual ID3D12Resource* GetMinMipMap() const = 0;

    //--------------------------------------------
    // for visualization
    //--------------------------------------------
    // number of tiles reserved (not necessarily committed) for this resource
    virtual UINT GetNumTilesVirtual() const = 0;
#if RESOLVE_TO_TEXTURE
    virtual ID3D12Resource* GetResolvedFeedback() const = 0;
#endif
};

//=============================================================================
// describe SFSManager (default values are recommended)
//=============================================================================
struct SFSManagerDesc
{
    // the Direct command queue the application is using to render, which SFSM monitors to know when new feedback is ready
    ID3D12CommandQueue* m_pDirectCommandQueue{ nullptr };

    // maximum number of in-flight batches
    UINT m_maxNumCopyBatches{ 128 };

    // size of the staging buffer for DirectStorage or reference streaming code
    UINT m_stagingBufferSizeMB{ 64 };

    // the following is product dependent (some HW/drivers seem to have a limit)
    UINT m_maxTileMappingUpdatesPerApiCall{ 512 };

    // need the swap chain count so we can create per-frame upload buffers
    UINT m_swapChainBufferCount{ 2 };

    // number of frames to delay before evicting a tile
    UINT m_evictionDelay{ 10 };

    UINT m_minNumUploadRequests{ 2000 }; // heuristic to reduce frequency of Submit() calls

    // applied to all internal threads: submit, fenceMonitor, processFeedback, updateResidency
    // on hybrid systems: performance prefers P cores, efficiency prefers E cores, normal is OS default
    enum class ThreadPriority : int
    {
        Prefer_Normal = 0,
        Prefer_Performance = 1,
        Prefer_Efficiency = -1
    };
    ThreadPriority m_threadPriority{ ThreadPriority::Prefer_Normal };

    // true: use Microsoft DirectStorage. false: use internal file streaming system
    // NOTE: internal file streaming system does not support DirectStorage compression
    bool m_useDirectStorage{ true };
};

//=============================================================================
// manages all the streaming resources
//=============================================================================
struct SFSManager
{
    static SFSManager* Create(const SFSManagerDesc& in_desc);

    virtual void Destroy() = 0;

    //--------------------------------------------
    // Create a heap used by 1 or more StreamingResources
    // parameter is number of 64KB tiles to manage
    //--------------------------------------------
    virtual SFSHeap* CreateHeap(UINT in_maxNumTilesHeap) = 0;

    //--------------------------------------------
    // Create StreamingResources using a common SFSManager
    //--------------------------------------------
    virtual SFSResource* CreateResource(const std::wstring& in_filename, SFSHeap* in_pHeap) = 0;

    //--------------------------------------------
    // Call BeginFrame() first,
    // once for all SFSManagers that share heap/upload buffers
    // descriptor heap is used per ProcessFeedback() to clear the feedback buffer
    // the shader resource view for the min mip map will be updated if necessary
    //    (which only happens if StreamingResources are created/destroyed)
    // NOTE: the root signature should set the associated descriptor range as descriptor and data volatile
    //--------------------------------------------
    virtual void BeginFrame(ID3D12DescriptorHeap* in_pDescriptorHeap, D3D12_CPU_DESCRIPTOR_HANDLE in_minmipmapDescriptorHandle) = 0;

    // application must explicitly request feedback for each resource each frame
    // this allows the application to limit how much time is spent on feedback, or stop processing e.g. for off-screen objects
    // descriptor required to create Clear() and Resolve() commands
    virtual void QueueFeedback(SFSResource* in_pResource, D3D12_GPU_DESCRIPTOR_HANDLE in_gpuDescriptor) = 0;

    //--------------------------------------------
    // Call EndFrame() last, paired with each BeginFrame() and after all draw commands
    // returns one command list:
    //   m_afterDrawCommands: must be called /after/ any draw commands
    // e.g.
    //    auto commandLists = pSFSManager->EndFrame();
    //    ID3D12CommandList* pCommandLists[] = { myCommandList, commandLists.m_afterDrawCommands };
    //    m_commandQueue->ExecuteCommandLists(_countof(pCommandLists), pCommandLists);
    //--------------------------------------------
    struct CommandLists
    {
        ID3D12CommandList* m_afterDrawCommands;
    };
    virtual CommandLists EndFrame() = 0;

    //--------------------------------------------
    // choose DirectStorage vs. manual tile loading
    //--------------------------------------------
    virtual void UseDirectStorage(bool in_useDS) = 0;

    //--------------------------------------------
    // are we between BeginFrame and EndFrame? useful for debugging
    //--------------------------------------------
    virtual bool GetWithinFrame() const = 0;

    //--------------------------------------------
    // GPU time for resolving feedback buffers last frame
    // use this to time-limit gpu feedback processing
    // to determine per-resolve time, divide this time by the number of QueueFeedback() calls during the frame
    //--------------------------------------------
    virtual float GetGpuTime() const = 0;

    //--------------------------------------------
    // for visualization
    //--------------------------------------------
    virtual void SetVisualizationMode(UINT in_mode) = 0;
    virtual void CaptureTraceFile(bool in_captureTrace) = 0; // capture a trace file of tile uploads
    virtual float GetCpuProcessFeedbackTime() = 0; // approx. cpu time spent processing feedback last frame. expected usage is to average over many frames
    virtual UINT GetTotalNumUploads() const = 0;   // number of tiles uploaded so far
    virtual UINT GetTotalNumEvictions() const = 0; // number of tiles evicted so far
    virtual float GetTotalTileCopyLatency() const = 0; // very approximate average latency of tile upload from request to completion
    virtual UINT GetTotalNumSubmits() const = 0;   // number of fence signals for uploads. when using DS, equals number of calls to IDStorageQueue::Submit()
};
