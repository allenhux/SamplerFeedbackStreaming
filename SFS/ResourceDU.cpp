//==============================================================
// Copyright © Intel Corporation
//
// SPDX-License-Identifier: MIT
// =============================================================

#include "pch.h"

#include "ResourceDU.h"
#include "SFSHeap.h"
#include "ManagerSR.h"

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
void SFS::ResourceDU::LoadPackedMipInfo(UpdateList& out_updateList)
{
    UpdateList::PackedMip packedMip;
    packedMip.m_mipInfo.offset = m_resourceDesc.m_packedMipData.m_offset;
    packedMip.m_mipInfo.numBytes = m_resourceDesc.m_packedMipData.m_numBytes;
    packedMip.m_mipInfo.uncompressedSize = m_resourceDesc.m_mipInfo.m_numUncompressedBytesForPackedMips;
    out_updateList.m_coords.push_back(packedMip.m_coord);
}

//-----------------------------------------------------------------------------
// can map the packed mips as soon as we have heap indices
//-----------------------------------------------------------------------------
void SFS::ResourceDU::MapPackedMips(ID3D12CommandQueue* in_pCommandQueue)
{
    UINT firstSubresource = m_resourceDesc.m_mipInfo.m_numStandardMips;

    // mapping packed mips is different from regular tiles: must be mapped before we can use copytextureregion() instead of copytiles()
    UINT numTiles = m_resources.GetNumTilesForPackedMips();

    std::vector<D3D12_TILE_RANGE_FLAGS> rangeFlags(numTiles, D3D12_TILE_RANGE_FLAG_NONE);

    // if the number of standard (not packed) mips is n, then start updating at subresource n
    D3D12_TILED_RESOURCE_COORDINATE resourceRegionStartCoordinates{ 0, 0, 0, firstSubresource };
    D3D12_TILE_REGION_SIZE resourceRegionSizes{ numTiles, FALSE, 0, 0, 0 };

    // perform packed mip tile mapping on the copy queue
    in_pCommandQueue->UpdateTileMappings(
        GetTiledResource(),
        1, // numRegions
        &resourceRegionStartCoordinates,
        &resourceRegionSizes,
        m_pHeap->GetHeap(),
        numTiles,
        rangeFlags.data(),
        m_packedMipHeapIndices.data(),
        nullptr,
        D3D12_TILE_MAPPING_FLAG_NONE
    );

    // DataUploader will synchronize around a mapping fence before uploading packed mips
}

//-----------------------------------------------------------------------------
// DataUploader has completed updating a reserved texture tile
//-----------------------------------------------------------------------------
void SFS::ResourceDU::NotifyCopyComplete(const std::vector<D3D12_TILED_RESOURCE_COORDINATE>& in_coords)
{
    for (const auto& t : in_coords)
    {
        ASSERT(TileMappingState::Residency::Loading == m_tileMappingState.GetResidency(t));
        m_tileMappingState.SetResidency(t, TileMappingState::Residency::Resident);
    }
}
#if 0
// NOTE: dead code. currently not un-mapping tiles

//-----------------------------------------------------------------------------
// DataUploader has completed updating a reserved texture tile
//-----------------------------------------------------------------------------
void SFS::ResourceDU::NotifyEvicted(const std::vector<D3D12_TILED_RESOURCE_COORDINATE>& in_coords)
{
    ASSERT(in_coords.size());
    for (const auto& t : in_coords)
    {
        ASSERT(TileMappingState::Residency::Evicting == m_tileMappingState.GetResidency(t));
        m_tileMappingState.SetResidency(t, TileMappingState::Residency::NotResident);
    }
}
#endif
//-----------------------------------------------------------------------------
// our packed mips have arrived!
//-----------------------------------------------------------------------------
void SFS::ResourceDU::NotifyPackedMips()
{
    m_packedMipStatus = PackedMipStatus::NEEDS_TRANSITION;
}
