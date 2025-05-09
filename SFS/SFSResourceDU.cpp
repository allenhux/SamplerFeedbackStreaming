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
#include "pch.h"

#include "SFSResourceDU.h"
#include "SFSHeap.h"
#include "SFSManagerSR.h"

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
void SFS::ResourceDU::LoadPackedMipInfo(UpdateList& out_updateList)
{
    UpdateList::PackedMip packedMip;
    GetPackedMipInfo(packedMip.m_mipInfo.offset, packedMip.m_mipInfo.numBytes, packedMip.m_mipInfo.uncompressedSize);
    out_updateList.m_coords.push_back(packedMip.m_coord);
}

//-----------------------------------------------------------------------------
// can map the packed mips as soon as we have heap indices
//-----------------------------------------------------------------------------
void SFS::ResourceDU::MapPackedMips(ID3D12CommandQueue* in_pCommandQueue)
{
    DeferredInitialize1();

    UINT firstSubresource = GetPackedMipInfo().NumStandardMips;

    // mapping packed mips is different from regular tiles: must be mapped before we can use copytextureregion() instead of copytiles()
    UINT numTiles = GetPackedMipInfo().NumTilesForPackedMips;

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

    SetResidencyChanged();
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

    SetResidencyChanged();
}
#endif
//-----------------------------------------------------------------------------
// our packed mips have arrived!
//-----------------------------------------------------------------------------
void SFS::ResourceDU::NotifyPackedMips()
{
    DeferredInitialize2();

    m_packedMipStatus = PackedMipStatus::NEEDS_TRANSITION;
}
