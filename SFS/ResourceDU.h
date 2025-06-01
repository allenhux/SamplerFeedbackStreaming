//==============================================================
// Copyright © Intel Corporation
//
// SPDX-License-Identifier: MIT
// =============================================================

//-----------------------------------------------------------------
// custom SFSResource interface for DataUploader
//-----------------------------------------------------------------

#pragma once

#include "ResourceBase.h"
#include "UpdateList.h"

namespace SFS
{
    class ResourceDU : private ResourceBase
    {
    public:
        SFS::Heap* GetHeap() const { return m_pHeap; }

        // just for packed mips
        const UINT GetPackedMipsFirstSubresource() const { return m_maxMip; }

        void NotifyCopyComplete(const std::vector<D3D12_TILED_RESOURCE_COORDINATE>& in_coords);
        void NotifyPackedMips();
        void NotifyEvicted(const std::vector<D3D12_TILED_RESOURCE_COORDINATE>& in_coords);

        ID3D12Resource* GetTiledResource() const { return m_resources.GetTiledResource(); }

        const FileHandle* GetFileHandle() const { return m_pFileHandle.get(); }
        const std::wstring& GetFileName() const { return m_filename; }

        // packed mips are treated differently from regular tiles: they aren't tracked by the data structure, and share heap indices
        void LoadPackedMipInfo(UpdateList& out_updateList);
        void MapPackedMips(ID3D12CommandQueue* in_pCommandQueue);

        UINT GetCompressionFormat() const { return m_resourceDesc.m_compressionFormat; }
        DXGI_FORMAT GetTextureFormat() const { return (DXGI_FORMAT)m_resourceDesc.m_textureFormat; }

        // performance of copy vs. return by reference probably roughly equal
        SFSResourceDesc::TileData GetFileOffset(const D3D12_TILED_RESOURCE_COORDINATE& in_coord) const
        {
            return m_resourceDesc.m_tileData[m_resourceDesc.GetLinearIndex(in_coord.X, in_coord.Y, in_coord.Subresource)];
        }

        void DeferredInitialize() { DeferredInitialize2(); }
    };
}
