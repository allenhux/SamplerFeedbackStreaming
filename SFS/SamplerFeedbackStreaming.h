//==============================================================
// Copyright © Intel Corporation
//
// SPDX-License-Identifier: MIT
// =============================================================

/*=============================================================================
External API interface for Sampler Feedback Streaming
This is the library equivalent of d3d12.h

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
#define RESOLVE_TO_TEXTURE 1

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
    virtual void Destroy() = 0;

    //--------------------------------------------
    // applications need access to the resources to create descriptors
    //--------------------------------------------
    virtual void CreateFeedbackView(D3D12_CPU_DESCRIPTOR_HANDLE out_descriptor) = 0;
    virtual void CreateShaderResourceView(D3D12_CPU_DESCRIPTOR_HANDLE in_descriptor) = 0;

    // shader reading min-mip-map buffer will want its dimensions
    virtual UINT GetMinMipMapWidth() const = 0;
    virtual UINT GetMinMipMapHeight() const = 0;

    // shader reading min-mip-map buffer will need an offset into the min-mip-map (residency map)
    // NOTE: all min mip maps are stored in a single buffer. offset into the buffer.
    virtual UINT GetMinMipMapOffset() const = 0;

    // application should not use this texture before it is ready
    virtual bool Drawable() const = 0;

    // application must explicitly request feedback for each resource each frame
    // this allows the application to limit how much time is spent on feedback, or stop processing e.g. for off-screen objects
    // descriptor required to create Clear() and Resolve() commands
    virtual void QueueFeedback(D3D12_GPU_DESCRIPTOR_HANDLE in_gpuDescriptor) = 0;

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
    // parameter is number of 64KB tiles to manage
    //--------------------------------------------
    virtual SFSHeap* CreateHeap(UINT in_maxNumTilesHeap) = 0;

    //--------------------------------------------
    // Create StreamingResources using a common SFSManager
    // Optionally provide a file header (e.g. if app opens many files or uses a custom file format)
    //--------------------------------------------
    virtual SFSResource* CreateResource(const struct SFSResourceDesc& in_desc,
        SFSHeap* in_pHeap, const std::wstring& in_filename) = 0;

    //--------------------------------------------
    // Call BeginFrame() first,
    // once for all SFSManagers that share heap/upload buffers
    // descriptor heap is used per ProcessFeedback() to clear the feedback buffer
    // the shader resource view for the min mip map will be updated if necessary
    //    (which only happens if StreamingResources are created/destroyed)
    // NOTE: the root signature should set the associated descriptor range as descriptor and data volatile
    //--------------------------------------------
    virtual void BeginFrame(ID3D12DescriptorHeap* in_pDescriptorHeap, D3D12_CPU_DESCRIPTOR_HANDLE in_minmipmapDescriptorHandle) = 0;

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
    virtual float GetGpuTimePerTexel() const { return 0; }; // FIXME TBD: time as a function of the queued texture dimensions

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
        UINT32 m_numTilesForPackedMips;
        UINT32 m_numUncompressedBytesForPackedMips;
    };
    MipInfo m_mipInfo;

    // use subresource tile dimensions to generate linear tile index
    struct StandardMipInfo
    {
        UINT32 m_widthTiles;
        UINT32 m_heightTiles;
        UINT32 m_depthTiles;

        // convenience value, can be computed from sum of previous subresource dimensions
        UINT32 m_subresourceTileIndex;
    };
    std::vector<StandardMipInfo> m_standardMipInfo; // size = MipInfo::m_numStandardMips

    // array TileData[m_numTilesForStandardMips + 1], 1 entry for each tile plus a final entry for packed mips
    struct TileData
    {
        UINT32 m_offset;          // file offset to tile data
        UINT32 m_numBytes;        // # bytes for the tile
    };

    TileData m_packedMipData; // may be unused
    std::vector<TileData> m_tileData; // size = MipInfo::m_numTilesForStandardMips

    // defines the order of the data in m_tileData
    UINT GetLinearIndex(UINT x, UINT y, UINT s) const
    {
        const auto& data = m_standardMipInfo[s];
        return data.m_subresourceTileIndex + (y * data.m_widthTiles) + x;
    }
};
