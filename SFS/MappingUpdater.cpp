//==============================================================
// Copyright © Intel Corporation
//
// SPDX-License-Identifier: MIT
// =============================================================

#include "pch.h"
#include "MappingUpdater.h"

std::vector<D3D12_TILE_RANGE_FLAGS> SFS::MappingUpdater::m_rangeFlagsMap;   // all NONE
std::vector<D3D12_TILE_RANGE_FLAGS> SFS::MappingUpdater::m_rangeFlagsUnMap; // all NULL
std::vector<UINT> SFS::MappingUpdater::m_rangeTileCounts; // all 1s

//=============================================================================
// Internal class that constructs commands that set
// virtual-to-physical mapping for the reserved resource
//=============================================================================
SFS::MappingUpdater::MappingUpdater(UINT in_maxTileMappingUpdatesPerApiCall) :
    m_maxTileMappingUpdatesPerApiCall(std::max(UINT(1), in_maxTileMappingUpdatesPerApiCall))
{
    // ensure static arrays are sized to the maximum of requested sizes
    UINT size = std::max(m_maxTileMappingUpdatesPerApiCall, (UINT)m_rangeTileCounts.size());

    // these will never change size
    m_rangeFlagsMap.assign(size, D3D12_TILE_RANGE_FLAG_NONE);
    m_rangeFlagsUnMap.assign(size, D3D12_TILE_RANGE_FLAG_NULL);
    m_rangeTileCounts.assign(size, 1);
}

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
void SFS::MappingUpdater::Map(ID3D12CommandQueue* in_pCommandQueue,
    ID3D12Resource* in_pResource, ID3D12Heap* in_pHeap,
    const std::vector<D3D12_TILED_RESOURCE_COORDINATE>& in_coords,
    const std::vector<UINT>& in_indices)
{
    // will only hit this assert if there is a bug in the SFS library;
    // should not be possible for application code to induce this assert
    ASSERT(in_coords.size() == in_indices.size());

    UINT numTotal = (UINT)in_coords.size();
    while (numTotal)
    {
        UINT numRegions = std::min(numTotal, m_maxTileMappingUpdatesPerApiCall);
        numTotal -= numRegions;

        in_pCommandQueue->UpdateTileMappings(
            in_pResource,
            numRegions,
            &in_coords[numTotal],
            nullptr,
            in_pHeap,
            numRegions,
            m_rangeFlagsMap.data(),
            &in_indices[numTotal],
            m_rangeTileCounts.data(),
            D3D12_TILE_MAPPING_FLAG_NONE
        );
    }
}

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
void SFS::MappingUpdater::UnMap(ID3D12CommandQueue* in_pCommandQueue, ID3D12Resource* in_pResource,
    const std::vector<D3D12_TILED_RESOURCE_COORDINATE>& in_coords)
{
    UINT numTotal = (UINT)in_coords.size();
    while (numTotal)
    {
        UINT numRegions = std::min(numTotal, m_maxTileMappingUpdatesPerApiCall);
        numTotal -= numRegions;

        in_pCommandQueue->UpdateTileMappings(
            in_pResource,
            numRegions,
            &in_coords[numTotal],
            nullptr,
            nullptr,
            numRegions,
            m_rangeFlagsUnMap.data(),
            nullptr,
            m_rangeTileCounts.data(),
            D3D12_TILE_MAPPING_FLAG_NONE
        );
    }
}

void SFS::MappingUpdater::Both(ID3D12CommandQueue* in_pCommandQueue,
    ID3D12Resource* in_pResource, ID3D12Heap* in_pHeap,
    const std::vector<D3D12_TILED_RESOURCE_COORDINATE>& in_map,
    const std::vector<D3D12_TILED_RESOURCE_COORDINATE>& in_unmap,
    const std::vector<UINT>& in_indices)
{
    std::vector<D3D12_TILED_RESOURCE_COORDINATE> coords;
    std::vector<D3D12_TILE_RANGE_FLAGS> rangeFlags;

    coords.reserve(in_map.size() + in_unmap.size());
    coords.insert(coords.begin(), in_map.begin(), in_map.end());
    coords.insert(coords.end(), in_unmap.begin(), in_unmap.end());

    rangeFlags.resize(in_map.size() + in_unmap.size());
    std::fill(rangeFlags.begin(), rangeFlags.begin() + in_map.size(), D3D12_TILE_RANGE_FLAG_NONE);
    std::fill(rangeFlags.begin() + in_map.size(), rangeFlags.end(), D3D12_TILE_RANGE_FLAG_NULL);

    UINT numTotal = (UINT)coords.size();
    while (numTotal)
    {
        UINT numRegions = std::min(numTotal, m_maxTileMappingUpdatesPerApiCall);
        numTotal -= numRegions;

        in_pCommandQueue->UpdateTileMappings(
            in_pResource,
            numRegions,
            &coords[numTotal],
            nullptr,
            in_pHeap,
            numRegions,
            rangeFlags.data(),
            in_indices.data() + numTotal,
            m_rangeTileCounts.data(),
            D3D12_TILE_MAPPING_FLAG_NONE
        );
    }
}
