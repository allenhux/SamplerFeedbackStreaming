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
        ID3D12Resource* ComputeCoordFromTileIndex(D3D12_TILED_RESOURCE_COORDINATE& out_coord, UINT in_index);

        DXGI_FORMAT GetFormat() const { return m_format; }
    private:
        const DXGI_FORMAT m_format;

        D3D12_SUBRESOURCE_TILING m_atlasTiling;
        std::vector<SFS::ComPtr<ID3D12Resource>> m_atlases;
        UINT m_numTilesPerAtlas{ 0 };
        const UINT m_atlasNumTiles;

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

        Heap(class ManagerBase* in_pSFS, ID3D12CommandQueue* in_pQueue, UINT in_maxNumTilesHeap);
        virtual ~Heap();

        // allocate atlases for a format. does nothing if format already has an atlas
        void AllocateAtlas(ID3D12CommandQueue* in_pQueue, const DXGI_FORMAT in_format);

        ID3D12Resource* ComputeCoordFromTileIndex(D3D12_TILED_RESOURCE_COORDINATE& out_coord, UINT in_index, const DXGI_FORMAT in_format);
        ID3D12Heap* GetHeap() const { return m_tileHeap.Get(); }
        SimpleAllocator& GetAllocator() { return m_heapAllocator; }
        bool GetDestroyable() const { return m_destroy; }
    private:
        SimpleAllocator m_heapAllocator;

        std::vector<SFS::Atlas*> m_atlases;
        ComPtr<ID3D12Heap> m_tileHeap; // heap to hold tiles resident in GPU memory
        class ManagerBase* const m_pSfsManager{nullptr}; // used in debug mode to validate allocator
        bool m_destroy{ false }; // if true, can be deleted when allocator GetAllocated() == 0 
    };
}
