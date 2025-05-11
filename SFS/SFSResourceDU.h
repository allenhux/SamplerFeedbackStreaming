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

#pragma once

#include "SFSResourceBase.h"
#include "UpdateList.h"

//-----------------------------------------------------------------
// custom SFSResource interface for DataUploader
//-----------------------------------------------------------------
namespace SFS
{
    class ResourceDU : private ResourceBase
    {
    public:
        //const XeTexture* GetTextureFileInfo() const { return &m_textureFileInfo; }
        SFS::Heap* GetHeap() const { return m_pHeap; }

        // just for packed mips
        const UINT GetPackedMipsFirstSubresource() const { return m_maxMip; }

        void NotifyCopyComplete(const std::vector<D3D12_TILED_RESOURCE_COORDINATE>& in_coords);
        void NotifyPackedMips();
        void NotifyEvicted(const std::vector<D3D12_TILED_RESOURCE_COORDINATE>& in_coords);

        ID3D12Resource* GetTiledResource() const { return m_resources->GetTiledResource(); }

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
    };
}
