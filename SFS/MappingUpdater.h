//==============================================================
// Copyright © Intel Corporation
//
// SPDX-License-Identifier: MIT
// =============================================================

#pragma once

#include "Streaming.h"

//==================================================
// MappingUpdater updates a reserved resource via UpdateTileMappings
// there are 2 kinds of updates: add and remove
// initialize an updater corresponding to each type
// now, all that is really added is coordinates.
//==================================================
namespace SFS
{
    class MappingUpdater
    {
    public:
        MappingUpdater(UINT in_maxTileMappingUpdatesPerApiCall);

        void Map(ID3D12CommandQueue* in_pCommandQueue, ID3D12Resource* in_pResource, ID3D12Heap* in_pHeap,
            const std::vector<D3D12_TILED_RESOURCE_COORDINATE>& in_coords,
            const std::vector<UINT>& in_indices);

        void UnMap(ID3D12CommandQueue* in_pCommandQueue, ID3D12Resource* in_pResource,
            const std::vector<D3D12_TILED_RESOURCE_COORDINATE>& in_coords);

        UINT GetMaxTileMappingUpdatesPerApiCall() const { return m_maxTileMappingUpdatesPerApiCall; }
    private:
        const UINT m_maxTileMappingUpdatesPerApiCall;

        static std::vector<D3D12_TILE_RANGE_FLAGS> m_rangeFlagsMap;   // all NONE
        static std::vector<D3D12_TILE_RANGE_FLAGS> m_rangeFlagsUnMap; // all NULL
        static std::vector<UINT> m_rangeTileCounts; // all 1s
    };
}
