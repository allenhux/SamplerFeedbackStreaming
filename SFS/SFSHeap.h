//==============================================================
// Copyright © Intel Corporation
//
// SPDX-License-Identifier: MIT
// =============================================================

/*==================================================
Implementation of SFS Resource
wraps D3D heap, Allocator, and Atlas
//==================================================*/

#pragma once

#include "Streaming.h" // for ComPtr
#include "SimpleAllocator.h"
#include "SamplerFeedbackStreaming.h"

namespace SFS
{
    // 1 or more aliased write-only resources to cover a heap
    class Atlas
    {
    public:
        Atlas(ID3D12Heap* in_pHeap, ID3D12CommandQueue* in_pQueue, UINT in_numTilesHeap, DXGI_FORMAT in_format);

        // return a resource pointer and a coordinate into that resource from linear tile index
        ID3D12Resource* ComputeCoordFromTileIndex(D3D12_TILED_RESOURCE_COORDINATE& out_coord, UINT in_index)
        {
            ASSERT(in_index < m_atlasNumTiles);

            // which atlas does this land in:
            UINT atlasIndex = in_index / m_numTilesPerAtlas;
            in_index -= (m_numTilesPerAtlas * atlasIndex);

            out_coord.Y = in_index / m_width;
            ASSERT(out_coord.Y < m_height);
            out_coord.X = in_index - (m_width * out_coord.Y);

            return m_atlases[atlasIndex].Get();
        }

        DXGI_FORMAT GetFormat() const { return m_format; }
    private:
        std::vector<SFS::ComPtr<ID3D12Resource>> m_atlases;

        const DXGI_FORMAT m_format;
        const UINT m_atlasNumTiles;

        UINT m_numTilesPerAtlas{ 0 };
        UINT m_width;
#ifdef _DEBUG
        UINT m_height;
#endif

        // returns number of tiles covered by the atlas
        UINT CreateAtlas(ComPtr<ID3D12Resource>& out_pDst,
            ID3D12Heap* in_pHeap, ID3D12CommandQueue* in_pQueue,
            DXGI_FORMAT in_format, UINT in_maxTiles, UINT in_tileOffset);
    };

    // Heap to hold tiles for 1 or more resources
    // contains atlases for the format(s) of the resources
    class Heap : public ::SFSHeap
    {
    public:
        //-----------------------------------------------------------------
        // external APIs
        //-----------------------------------------------------------------
        virtual void Destroy() override;
        virtual UINT GetNumTilesAllocated() const override { return m_heapAllocator.GetAllocated(); }
        //-----------------------------------------------------------------
        // end external APIs
        //-----------------------------------------------------------------

        Heap(class ManagerBase* in_pSFS, ID3D12CommandQueue* in_pQueue, UINT in_sizeInMB);

        // allocate atlases for a format. does nothing if format already has an atlas
        Atlas* AllocateAtlas(ID3D12CommandQueue* in_pQueue, const DXGI_FORMAT in_format);

        ID3D12Heap* GetHeap() const { return m_tileHeap.Get(); }
        SimpleAllocator& GetAllocator() { return m_heapAllocator; }

        bool GetDestroyable() const { return m_destroy; }
        void AddRef() { m_refCount++; }
        void DecRef() { m_refCount--; if (m_destroy && (0 == m_refCount)) { delete this; } }
    private:
        friend ManagerBase;
        virtual ~Heap();

        std::atomic<UINT> m_refCount{ 0 }; // delete only after all dependent resources deleted

        SimpleAllocator m_heapAllocator;

        std::vector<SFS::Atlas*> m_atlases;
        ComPtr<ID3D12Heap> m_tileHeap; // heap to hold tiles resident in GPU memory
        class ManagerBase* const m_pSfsManager{nullptr}; // used for lifetime allocation management
        bool m_destroy{ false }; // if true, can be deleted when allocator GetAllocated() == 0 
    };
}
