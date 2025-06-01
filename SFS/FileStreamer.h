//==============================================================
// Copyright © Intel Corporation
//
// SPDX-License-Identifier: MIT
// =============================================================

#pragma once

#include "Streaming.h"
#include "JsonParser.h"
#include <unordered_map>
#include <string>
#include <dstorage.h>

namespace SFS
{
    struct UpdateList;

    // file handle internals different between reference and DS FileStreamers
    class FileHandle
    {
    public:
        virtual ~FileHandle() {}
    };

    class FileStreamer
    {
    public:
        FileStreamer(ID3D12Device* in_pDevice, bool in_traceCaptureMode = false);
        virtual ~FileStreamer();

        virtual FileHandle* OpenFile(const std::wstring& in_path) = 0;

        virtual void StreamTexture(SFS::UpdateList& in_updateList) = 0;

        virtual void StreamPackedMips(SFS::UpdateList& in_updateList) = 0;

        // Signal must be thread safe because it is called from ProcessFeedbackThread and FenceMonitorThread
        virtual void Signal() = 0;

        enum class VisualizationMode
        {
            DATA_VIZ_NONE,
            DATA_VIZ_MIP,
            DATA_VIZ_TILE
        };
        void SetVisualizationMode(UINT in_mode) { m_visualizationMode = (VisualizationMode)in_mode; }

        UINT64 GetCompletedValue() { return m_copyFence->GetCompletedValue(); }
        void SetEventOnCompletion(UINT64 in_v, HANDLE in_event) { ThrowIfFailed(m_copyFence->SetEventOnCompletion(in_v, in_event)); }

        void CaptureTraceFile(bool in_captureTrace) { m_captureTrace = in_captureTrace; } // enable/disable writing requests/submits to a trace file
    protected:
        const bool m_traceCaptureMode{ false };

        // copy queue fence
        ComPtr<ID3D12Fence> m_copyFence;

        // fence value is atomic to make Signal() thread-safe
        std::atomic<UINT64> m_copyFenceValue{ 0 };

        // Visualization
        VisualizationMode m_visualizationMode{ VisualizationMode::DATA_VIZ_NONE };

        // get visualization colors
        void* GetVisualizationData(const D3D12_TILED_RESOURCE_COORDINATE& in_coord, DXGI_FORMAT in_format);

        static const UINT m_lutSize{ 16 };
        static float m_lut[m_lutSize][3];

        static BYTE m_BC7[m_lutSize][D3D12_TILED_RESOURCE_TILE_SIZE_IN_BYTES];
        void InitializeBC7();

        static BYTE m_BC1[m_lutSize][D3D12_TILED_RESOURCE_TILE_SIZE_IN_BYTES];
        void InitializeBC1();

        // trace file
        bool m_captureTrace{ false };
        // map file handles to path strings
        std::unordered_map<const void*, std::string> m_files;

        void TraceRequest(
            ID3D12Resource* in_pDstResource, const D3D12_TILED_RESOURCE_COORDINATE& in_dstCoord,
            const DSTORAGE_REQUEST& in_request);
        void TraceSubmit();
    private:
        bool m_firstSubmit{ true };
        JsonParser m_trace; // array of submits, each submit is an array of requests
        // map textures to descriptions so they can be properly re-created during playback
        std::unordered_map<ID3D12Resource*, D3D12_RESOURCE_DESC> m_tracingResources;
        struct Trace
        {
            ID3D12Resource* m_pDstResource;
            D3D12_TILED_RESOURCE_COORDINATE m_coord;
            const void* m_pFileHandle;
            UINT64 m_offset;
            UINT32 m_numBytes;
            UINT32 m_compressionFormat;
        };
        std::list<std::vector<Trace>> m_traces = {};
    };
}
