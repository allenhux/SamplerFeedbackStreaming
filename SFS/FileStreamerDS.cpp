//==============================================================
// Copyright © Intel Corporation
//
// SPDX-License-Identifier: MIT
// =============================================================

#include "pch.h"

#include "FileStreamerDS.h"
#include "ResourceDU.h"

#include "UpdateList.h"
#include "SFSHeap.h"

//=======================================================================================
//=======================================================================================
SFS::FileStreamerDS::FileHandleDS::FileHandleDS(IDStorageFactory* in_pFactory, const std::wstring& in_path)
{
    ThrowIfFailed(in_pFactory->OpenFile(in_path.c_str(), IID_PPV_ARGS(&m_file)));
}

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
SFS::FileStreamerDS::FileStreamerDS(ID3D12Device* in_pDevice, IDStorageFactory* in_pDSfactory,
    bool in_traceCaptureMode) :
    m_pFactory(in_pDSfactory),
    SFS::FileStreamer(in_pDevice, in_traceCaptureMode)
{
    DSTORAGE_QUEUE_DESC queueDesc{};
    queueDesc.Capacity = DSTORAGE_MAX_QUEUE_CAPACITY;
    queueDesc.Priority = DSTORAGE_PRIORITY_NORMAL;
    queueDesc.SourceType = DSTORAGE_REQUEST_SOURCE_FILE;
    queueDesc.Device = in_pDevice;

    ThrowIfFailed(in_pDSfactory->CreateQueue(&queueDesc, IID_PPV_ARGS(&m_fileQueue)));

    queueDesc.SourceType = DSTORAGE_REQUEST_SOURCE_MEMORY;
    ThrowIfFailed(in_pDSfactory->CreateQueue(&queueDesc, IID_PPV_ARGS(&m_memoryQueue)));
}

SFS::FileStreamerDS::~FileStreamerDS()
{
}

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
IDStorageFile* SFS::FileStreamerDS::GetFileHandle(const SFS::FileHandle* in_pHandle)
{
    return dynamic_cast<const FileHandleDS*>(in_pHandle)->GetHandle();
}

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
SFS::FileHandle* SFS::FileStreamerDS::OpenFile(const std::wstring& in_path)
{
    auto h = new FileHandleDS(m_pFactory, in_path);
    if (m_traceCaptureMode)
    {
        std::wstring fileName = std::filesystem::path(in_path).filename();
        const wchar_t* pChars = fileName.c_str();
        int buf_len = ::WideCharToMultiByte(CP_UTF8, 0, pChars, -1, NULL, 0, NULL, NULL);
        std::string filename(buf_len, ' ');
        ::WideCharToMultiByte(CP_UTF8, 0, pChars, -1, filename.data(), buf_len, NULL, NULL);
        m_files[h->GetHandle()] = filename;
    }
    return h;
}

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
void SFS::FileStreamerDS::StreamPackedMips(SFS::UpdateList& in_updateList)
{
    DSTORAGE_REQUEST request = {};
    request.Options.SourceType = DSTORAGE_REQUEST_SOURCE_FILE;
    request.Options.DestinationType = DSTORAGE_REQUEST_DESTINATION_MULTIPLE_SUBRESOURCES;
    UpdateList::PackedMip packedMip;
    packedMip.m_coord = in_updateList.m_coords[0];
    request.Source.File.Source = GetFileHandle(in_updateList.m_pResource->GetFileHandle());
    request.Source.File.Size = packedMip.m_mipInfo.numBytes;
    request.Source.File.Offset = packedMip.m_mipInfo.offset;
    request.UncompressedSize = packedMip.m_mipInfo.uncompressedSize;
    request.Destination.MultipleSubresources.Resource = in_updateList.m_pResource->GetTiledResource();
    request.Destination.MultipleSubresources.FirstSubresource = in_updateList.m_pResource->GetPackedMipsFirstSubresource();
    request.Options.CompressionFormat = (DSTORAGE_COMPRESSION_FORMAT)in_updateList.m_pResource->GetCompressionFormat();

    m_fileQueue->EnqueueRequest(&request);

    in_updateList.m_copyFenceValue = m_copyFenceValue;
    in_updateList.m_copyFenceValid = true;
}

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
void SFS::FileStreamerDS::StreamTexture(SFS::UpdateList& in_updateList)
{
    ASSERT(in_updateList.GetNumStandardUpdates());

    Atlas* pAtlas = in_updateList.m_pResource->GetAtlas();

    DSTORAGE_REQUEST request{};
    request.Options.DestinationType = DSTORAGE_REQUEST_DESTINATION_TILES;
    request.Destination.Tiles.TileRegionSize = D3D12_TILE_REGION_SIZE{ 1, FALSE, 0, 0, 0 };
    request.UncompressedSize = D3D12_TILED_RESOURCE_TILE_SIZE_IN_BYTES;

    if (VisualizationMode::DATA_VIZ_NONE == m_visualizationMode)
    {
        request.Options.SourceType = DSTORAGE_REQUEST_SOURCE_FILE;
        request.Source.File.Source = GetFileHandle(in_updateList.m_pResource->GetFileHandle());
        request.Options.CompressionFormat = (DSTORAGE_COMPRESSION_FORMAT)in_updateList.m_pResource->GetCompressionFormat();

        UINT numCoords = (UINT)in_updateList.m_coords.size();
        for (UINT i = 0; i < numCoords; i++)
        {
            auto fileOffset = in_updateList.m_pResource->GetFileOffset(in_updateList.m_coords[i]);
            request.Source.File.Offset = fileOffset.m_offset;
            request.Source.File.Size = fileOffset.m_numBytes;

            D3D12_TILED_RESOURCE_COORDINATE& coord = request.Destination.Tiles.TiledRegionStartCoordinate;
            ID3D12Resource* pDstRsrc = pAtlas->ComputeCoordFromTileIndex(coord, in_updateList.m_heapIndices[i]);

            request.Destination.Tiles.Resource = pDstRsrc;

            m_fileQueue->EnqueueRequest(&request);

            if (m_captureTrace)
            {
                TraceRequest(pDstRsrc, coord, request);
            }
        }
    }
    else // visualization color is loaded from memory
    {
        request.Options.SourceType = DSTORAGE_REQUEST_SOURCE_MEMORY;
        request.Source.Memory.Size = D3D12_TILED_RESOURCE_TILE_SIZE_IN_BYTES;
        DXGI_FORMAT textureFormat = in_updateList.m_pResource->GetTextureFormat();

        UINT numCoords = (UINT)in_updateList.m_coords.size();
        for (UINT i = 0; i < numCoords; i++)
        {
            request.Source.Memory.Source = GetVisualizationData(in_updateList.m_coords[i], textureFormat);

            D3D12_TILED_RESOURCE_COORDINATE coord{};
            ID3D12Resource* pDstRsrc = pAtlas->ComputeCoordFromTileIndex(coord, in_updateList.m_heapIndices[i]);

            request.Destination.Tiles.Resource = pDstRsrc;
            request.Destination.Tiles.TiledRegionStartCoordinate = coord;

            m_memoryQueue->EnqueueRequest(&request);
        }
    }

    in_updateList.m_copyFenceValue = m_copyFenceValue;
    in_updateList.m_copyFenceValid = true;
}

//-----------------------------------------------------------------------------
// signal to submit a set of batches
// must be executed in the same thread as the load methods above to avoid atomic m_copyFenceValue
//-----------------------------------------------------------------------------
void SFS::FileStreamerDS::Signal()
{
    if (VisualizationMode::DATA_VIZ_NONE == m_visualizationMode)
    {
        m_fileQueue->EnqueueSignal(m_copyFence.Get(), m_copyFenceValue);
        m_fileQueue->Submit();

        if (m_captureTrace) { TraceSubmit(); }
    }
    else
    {
        m_memoryQueue->EnqueueSignal(m_copyFence.Get(), m_copyFenceValue);
        m_memoryQueue->Submit();
    }

    m_copyFenceValue++;
}
