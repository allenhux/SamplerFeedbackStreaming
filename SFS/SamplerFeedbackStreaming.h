//==============================================================
// Copyright © Intel Corporation
//
// SPDX-License-Identifier: MIT
// =============================================================

/*=============================================================================
External API interface for Sampler Feedback Streaming
This is the library equivalent of d3d12.h
NOTE: most APIs are NOT thread-safe (exceptions are SFSResource::Destroy and SFSManager::CreateResource)

Usage:

1. Create an SFSManager
2. Use SFSManager::CreateHeap() to create heaps to be used by 1 or more StreamingResources
3. Use SFSManager::CreateResource() to create 1 or more StreamingResources
    each SFSResource resides in a single heap

Draw loop:
1. BeginFrame() with the SFSManager (SFSM)
2. Draw your assets using the streaming textures, min-mip-map, and sampler feedback SRVs
    Optionally call Resource::QueueFeedback() to get sampler feedback for this draw.
    SRVs can be created using SFSResource methods
3. EndFrame() (with the SFSM) returns 1 command list: afterDrawCommands
4. ExecuteCommandLists() with [yourCommandList, afterDrawCommands] command lists.
=============================================================================*/

#pragma once

// FIXME: resolve to buffer only supported in Win11 and some insider versions of Win10
// When resolving to texture, must copy to cpu-readable buffer from gpu texture (which cannot be in the readback heap)
// Setting this to 0 resolves directly to cpu-readable buffer
#define RESOLVE_TO_TEXTURE 0

#include <d3d12.h>

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
    // There may be internal threads or queues that are actively updating the resource
    // MUST call SFSManager::FlushResources() first. After event signaled, call SFSResource::Destroy()
    virtual void Destroy() = 0;

    //--------------------------------------------
    // applications need access to the resources to create descriptors
    //--------------------------------------------
    virtual void CreateFeedbackView(D3D12_CPU_DESCRIPTOR_HANDLE out_descriptor) = 0;
    virtual void CreateShaderResourceView(D3D12_CPU_DESCRIPTOR_HANDLE in_descriptor) = 0;

    // shader reading min-mip-map buffer will want its dimensions
    virtual UINT GetMinMipMapWidth() const = 0;
    virtual UINT GetMinMipMapHeight() const = 0;
    virtual UINT GetMinMipMapSize() const = 0;

    // shader reading min-mip-map buffer will need an offset into the min-mip-map (residency map)
    // NOTE: all min mip maps are stored in a single buffer. offset into the buffer.
    virtual UINT GetMinMipMapOffset() const = 0;

    // application should not use this texture before it is ready
    // if this resource was previously flushed (SFSManagerFlushResource), this will
    //    put the resource back in a non-flushed state (not safe to delete)
    virtual bool Drawable() = 0;

    // application must explicitly request feedback for each resource each frame
    // this allows the application to limit how much time is spent on feedback, or stop processing e.g. for off-screen objects
    virtual void QueueFeedback() = 0;

    // evict all loaded tiles for this object, e.g. if not visible
    // call any time
    virtual void QueueEviction() = 0;

    virtual ID3D12Resource* GetTiledResource() const = 0;

    virtual ID3D12Resource* GetMinMipMap() const = 0;

    //--------------------------------------------
    // for visualization
    //--------------------------------------------
    // number of tiles reserved (not necessarily committed) for this resource
    virtual UINT GetNumTilesVirtual() const = 0;
    virtual UINT GetNumStandardMips() const = 0;
};

//=============================================================================
// describe SFSManager (default values are recommended)
//=============================================================================
struct SFSManagerDesc
{
    // the Direct command queue the application is using to render, which SFSM monitors to know when new feedback is ready
    ID3D12CommandQueue* m_pDirectCommandQueue{ nullptr };

    // maximum number of in-flight batches
    UINT m_maxNumCopyBatches{ 512 };

    // size of the staging buffer for DirectStorage or reference streaming code
    UINT m_stagingBufferSizeMB{ 128 };

    // the following is product dependent (some HW/drivers seem to have a limit)
    UINT m_maxTileMappingUpdatesPerApiCall{ 4096 };

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

    // NOTE: this parameter applies only when the library is compiled with RESOLVE_TO_TEXTURE 1
    UINT m_resolveHeapSizeMB{ 32 };

    // true: use Microsoft DirectStorage. false: use internal file streaming system
    // NOTE: internal file streaming system does not support DirectStorage compression
    bool m_useDirectStorage{ true };

    // Remember things that are required for later trace file capture
    // Start trace file capture by later calling CaptureTraceFile()
    bool m_traceCaptureMode{ false };
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
    //--------------------------------------------
    virtual SFSHeap* CreateHeap(UINT in_sizeInMB) = 0;

    //--------------------------------------------
    // <thread safe>
    // Create StreamingResources using a common SFSManager
    // Optionally provide a file header (e.g. if app opens many files or uses a custom file format)
    //--------------------------------------------
    virtual SFSResource* CreateResource(const struct SFSResourceDesc& in_desc,
        SFSHeap* in_pHeap, const std::wstring& in_filename) = 0;

    //--------------------------------------------
    // <thread safe>
    // WARNING: blocks until previous call to FlushResources() has signaled
    // Call FlushResources with a vector of resources and an event handle
    //   e.g. pMgr->FlushResources(myVector, eventToSignal)
    // After the event is signaled, the application can SFSResource::Destroy() each resource
    // The resources do not have to be destroyed: they can be used again later.
    //--------------------------------------------
    virtual void FlushResources(const std::vector<SFSResource*>& in_resources, HANDLE in_event) = 0;

    //--------------------------------------------
    // Call BeginFrame() first,
    // once for all SFSManagers that share heap/upload buffers
    //--------------------------------------------
    virtual void BeginFrame() = 0;

    //--------------------------------------------
    // Call EndFrame() last, paired with each BeginFrame() and after all draw commands
    // returns one command list which must be executed immediately after any draw commands
    // the shader resource view descriptor will be set for the min mip map (used as a mip clamp in the pixel shader)
    // NOTE: CreateShaderResourceView() occurs every frame to support rendering architectures that need it,
    //     but the resource only changes if StreamingResources are created/destroyed
    // NOTE: the root signature should set the associated descriptor range as descriptor and data volatile
    //
    // example usage:
    //    auto pCommandList = pSFSManager->EndFrame(minmipmapDescriptor);
    //    ID3D12CommandList* pCommandLists[] = { myCommandList, pCommandList };
    //    m_commandQueue->ExecuteCommandLists(_countof(pCommandLists), pCommandLists);
    //--------------------------------------------
    virtual ID3D12CommandList* EndFrame(D3D12_CPU_DESCRIPTOR_HANDLE out_minmipmapDescriptorHandle) = 0;

    //--------------------------------------------
    // choose DirectStorage vs. manual tile loading
    //--------------------------------------------
    virtual void UseDirectStorage(bool in_useDS) = 0;

    //--------------------------------------------
    // are we between BeginFrame and EndFrame? useful for debugging
    //--------------------------------------------
    virtual bool GetWithinFrame() const = 0;

    //--------------------------------------------
    // use this to time-limit gpu feedback processing
    // multiply this by the amount of time you allow for feedback, e.g. 2 (for 2ms), to get # texels
    // call QueueFeedback() for resources until the sum of GetMinMipMapSize() is >= that number
    //--------------------------------------------
    virtual float GetGpuTexelsPerMs() const = 0 ; // time (ms) as a function of the queued texture dimensions

    // the number of times QueueFeedback() can be called per frame
    // beyond this limit, QueueFeedback() will be ignored
    virtual UINT GetMaxNumFeedbacksPerFrame() const = 0;

    //--------------------------------------------
    // statistics
    //--------------------------------------------
    virtual float GetGpuTime() const = 0; // GPU render queue time (seconds) for resolving feedback buffers (averaged)
    virtual float GetCpuProcessFeedbackTime() = 0; // approx. cpu time (seconds) spent processing feedback last frame (averaged)
    virtual UINT GetTotalNumUploads() const = 0;   // number of tiles uploaded so far
    virtual UINT GetTotalNumEvictions() const = 0; // number of tiles evicted so far
    virtual UINT GetTotalNumSubmits() const = 0;   // equals number of calls to IDStorageQueue::Submit(copy command)
    virtual UINT GetTotalNumSignals() const = 0;   // equals number of calls to IDStorageQueue::Signal(fence)
    virtual float GetTotalTileCopyLatency() const = 0; // very approximate sum of latencies for tile uploads from request to completion
    virtual void CaptureTraceFile(bool in_captureTrace) = 0; // capture a trace file of tile uploads

    //--------------------------------------------
    // for visualization
    //--------------------------------------------
    virtual void SetVisualizationMode(UINT in_mode) = 0;
};

struct SFSResourceDesc
{
    UINT m_width{ 0 };
    UINT m_height{ 0 };
    UINT m_textureFormat{ 0 }; // DXGI_FORMAT
    UINT m_compressionFormat{ 0 }; // DSTORAGE_COMPRESSION_FORMAT

    struct MipInfo
    {
        UINT32 m_numStandardMips;
        UINT32 m_numTilesForStandardMips;
        UINT32 m_numPackedMips;
        UINT32 m_numUncompressedBytesForPackedMips;
    };
    MipInfo m_mipInfo{};

    // use subresource tile dimensions to generate linear tile index
    struct StandardMipInfo
    {
        UINT32 m_widthTiles;
        UINT32 m_heightTiles;
        //UINT32 m_depthTiles; // FIXME? no plan to support 3D textures

        // convenience value, can be computed from sum of previous subresource dimensions
        UINT32 m_subresourceTileIndex;
    };
    std::vector<StandardMipInfo> m_standardMipInfo; // size = MipInfo::m_numStandardMips

    struct TileData
    {
        UINT32 m_offset;          // file offset to tile data
        UINT32 m_numBytes;        // # bytes for the tile
    };

    TileData m_packedMipData{}; // may be unused. offset and # bytes for packed mips
    std::vector<TileData> m_tileData; // size = MipInfo::m_numTilesForStandardMips

    // defines the order of the data in m_tileData
    UINT GetLinearIndex(UINT x, UINT y, UINT s) const
    {
        const auto& data = m_standardMipInfo[s];
        return data.m_subresourceTileIndex + (y * data.m_widthTiles) + x;
    }
};
