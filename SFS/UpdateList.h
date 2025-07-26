//==============================================================
// Copyright © Intel Corporation
//
// SPDX-License-Identifier: MIT
// =============================================================

#pragma once

//==================================================
// UpdateList: set of uploads or evictions sharing completion fences
// 1 fence for mapping onto upload heap, one fence for copy (if applicable)
//==================================================
namespace SFS
{
    class ResourceDU;

    struct UpdateList
    {
        enum class State : std::uint32_t
        {
            // states used by the ProcessFeedback thread
            STATE_FREE,                  // available / unused
            STATE_ALLOCATED,             // allocated by SFSResource for ProcessFeedback

            // statistics are gathered on a common thread
            STATE_SUBMITTED,             // start file i/o (DS) if necessary. if mapping only, go directly to notify

            STATE_UPLOADING,             // make sure the copy fence is valid, since copying and mapping can be concurrent
            STATE_MAP_PENDING,           // check for mapping complete

            STATE_PACKED_MAPPING,        // wait for packed mips to be mapped before uploading
            STATE_PACKED_COPY_PENDING    // wait for upload of packed mips to complete
        };

        // initialize to ready
        std::atomic<State> m_executionState{ State::STATE_FREE };
        std::atomic<bool> m_mappingFenceValid{ false };

        // for the tiled resource, streaming info, and to notify complete
        ResourceDU* m_pResource{ nullptr };

        UINT64 m_copyFenceValue{ 0 };     // gpu copy fence
        UINT64 m_mappingFenceValue{ 0 };  // gpu mapping fence

        // tile loads:
        std::vector<D3D12_TILED_RESOURCE_COORDINATE> m_coords; // tile coordinates
        std::vector<UINT> m_heapIndices;                       // indices into shared heap (for mapping)

        // tile evictions:
        std::vector<D3D12_TILED_RESOURCE_COORDINATE> m_evictCoords;

        INT64 m_copyLatencyTimer{ 0 }; // used only to get an approximate latency for tile copies

        union PackedMip
        {
            struct
            {
                UINT offset;
                UINT numBytes;
                UINT uncompressedSize;
            } m_mipInfo;
            D3D12_TILED_RESOURCE_COORDINATE m_coord;
        };

        UINT GetNumStandardUpdates() const { return (UINT)m_coords.size(); }
        UINT GetNumEvictions() const { return (UINT)m_evictCoords.size(); }

        void Reset(SFS::ResourceDU* in_pResource)
        {
            m_pResource = in_pResource;
            m_mappingFenceValid = false;
            m_coords.clear();
            m_heapIndices.clear();
            m_evictCoords.clear();
            m_copyLatencyTimer = 0;
        }
    };
}
