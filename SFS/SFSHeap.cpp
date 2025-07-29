//==============================================================
// Copyright © Intel Corporation
//
// SPDX-License-Identifier: MIT
// =============================================================

//=============================================================================
// Implementation of the ::SFSHeap public (external) interface
//=============================================================================

#include "pch.h"

#include "SFSHeap.h"
#include "ManagerBase.h"

//-----------------------------------------------------------------------------
// call destructor on derived object
//-----------------------------------------------------------------------------
void SFS::Heap::Destroy()
{
    m_destroy = true;
}

//-----------------------------------------------------------------------------
// create an "atlas" texture that covers the entire heap
//-----------------------------------------------------------------------------
SFS::Atlas::Atlas(ID3D12Heap* in_pHeap, ID3D12CommandQueue* in_pQueue,
    UINT in_numTilesHeap, DXGI_FORMAT in_format) :
    m_atlasNumTiles(in_numTilesHeap)
    , m_format(in_format)
{
    m_atlases.resize(1);

    // if the heap is larger than one texture, we will need multiple internal atlas textures
    UINT numTilesToAllocate = in_numTilesHeap;
    UINT tileOffset = 0;
    m_numTilesPerAtlas = CreateAtlas(m_atlases[0], in_pHeap, in_pQueue, in_format, in_numTilesHeap, tileOffset);

    UINT numAtlases = (numTilesToAllocate + m_numTilesPerAtlas - 1) / m_numTilesPerAtlas;
    m_atlases.resize(numAtlases);
    for (UINT i = 1; i < numAtlases; i++)
    {
        tileOffset += m_numTilesPerAtlas;
        numTilesToAllocate -= m_numTilesPerAtlas;
        CreateAtlas(m_atlases[i], in_pHeap, in_pQueue, in_format, numTilesToAllocate, tileOffset);
    }

    // copies will target this, so mapping needs to complete immediately
    // FIXME? not worth the optimization to try to handle this flush elsewhere.
    {
        ComPtr<ID3D12Device> device;
        in_pHeap->GetDevice(IID_PPV_ARGS(&device));

        HANDLE fenceEvent = ::CreateEvent(nullptr, FALSE, FALSE, nullptr);
        if (fenceEvent == nullptr)
        {
            ThrowIfFailed(HRESULT_FROM_WIN32(GetLastError()));
        }
        ComPtr<ID3D12Fence> tempFence;
        ThrowIfFailed(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&tempFence)));
        ThrowIfFailed(in_pQueue->Signal(tempFence.Get(), 1));
        ThrowIfFailed(tempFence->SetEventOnCompletion(1, fenceEvent));
        WaitForSingleObject(fenceEvent, INFINITE);
        ::CloseHandle(fenceEvent);
    }
}

//-----------------------------------------------------------------------------
// create internal atlas texture
//-----------------------------------------------------------------------------
UINT SFS::Atlas::CreateAtlas(
    ComPtr<ID3D12Resource>& out_pDst,
    ID3D12Heap* in_pHeap, ID3D12CommandQueue* in_pQueue,
    DXGI_FORMAT in_format, UINT in_maxTiles, UINT in_tileOffset)
{
    ComPtr<ID3D12Device> device;
    in_pHeap->GetDevice(IID_PPV_ARGS(&device));

    // only use mip 1 of the resource. Subsequent mips provide little additional coverage while complicating lookup arithmetic
    UINT subresourceCount = 1;

    // create a maximum size reserved resource
    D3D12_RESOURCE_DESC rd = CD3DX12_RESOURCE_DESC::Tex2D(in_format, D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION, D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION);
    rd.MipLevels = (UINT16)subresourceCount;

    // Layout must be D3D12_TEXTURE_LAYOUT_64KB_UNDEFINED_SWIZZLE when creating reserved resources
    rd.Layout = D3D12_TEXTURE_LAYOUT_64KB_UNDEFINED_SWIZZLE;

    // this will only ever be a copy dest
    ThrowIfFailed(device->CreateReservedResource(&rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&out_pDst)));
    out_pDst->SetName(L"DataUploader::m_atlas");

    UINT numAtlasTiles = 0;
    D3D12_SUBRESOURCE_TILING tiling{};
    device->GetResourceTiling(out_pDst.Get(), &numAtlasTiles, nullptr, nullptr, &subresourceCount, 0, &tiling);
    m_width = tiling.WidthInTiles;
#ifdef _DEBUG
    m_height = tiling.HeightInTiles;
#endif

    numAtlasTiles = std::min(in_maxTiles, numAtlasTiles);

    // The following depends on the linear assignment order defined by D3D12_REGION_SIZE UseBox = FALSE
    // https://docs.microsoft.com/en-us/windows/win32/api/d3d12/ns-d3d12-d3d12_tile_region_size

    // we are updating a single region: all the tiles
    UINT numResourceRegions = 1;
    D3D12_TILED_RESOURCE_COORDINATE resourceRegionStartCoordinates{ 0, 0, 0, 0 };
    D3D12_TILE_REGION_SIZE resourceRegionSizes{ numAtlasTiles, FALSE, 0, 0, 0 };

    // we can do this with a single range
    UINT numRanges = 1;
    std::vector<D3D12_TILE_RANGE_FLAGS>rangeFlags(numRanges, D3D12_TILE_RANGE_FLAG_NONE);
    std::vector<UINT> rangeTileCounts(numRanges, numAtlasTiles);

    // D3D12 defines that tiles are linear, not swizzled relative to each other
    // so all the tiles can be mapped in one call
    in_pQueue->UpdateTileMappings(
        out_pDst.Get(),
        numResourceRegions,
        &resourceRegionStartCoordinates,
        &resourceRegionSizes,
        in_pHeap,
        (UINT)rangeFlags.size(),
        rangeFlags.data(),
        &in_tileOffset,
        rangeTileCounts.data(),
        D3D12_TILE_MAPPING_FLAG_NONE
    );

    return numAtlasTiles;
}

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
SFS::Heap::Heap(SFS::ManagerBase* in_pSfsManager,
    ID3D12CommandQueue* in_pQueue, UINT in_sizeInMB)
    : m_heapAllocator(in_sizeInMB * 16), m_pSfsManager(in_pSfsManager)
{
    ComPtr<ID3D12Device> device;
    in_pQueue->GetDevice(IID_PPV_ARGS(&device));

    // create a heap to store streaming tiles
    const UINT64 heapSize = UINT64(in_sizeInMB) * 1024 * 1024;
    CD3DX12_HEAP_DESC heapDesc(heapSize, D3D12_HEAP_TYPE_DEFAULT, 0, D3D12_HEAP_FLAG_DENY_BUFFERS | D3D12_HEAP_FLAG_DENY_RT_DS_TEXTURES);
    ThrowIfFailed(device->CreateHeap(&heapDesc, IID_PPV_ARGS(&m_tileHeap)));
}

SFS::Heap::~Heap()
{
    ASSERT(0 == m_refCount);

    m_pSfsManager->RemoveHeap(this);
    for (auto p : m_atlases)
    {
        delete p;
    }
}

//-----------------------------------------------------------------------------
// creation of new SFSResource must verify there is an atlas for that format
//-----------------------------------------------------------------------------
SFS::Atlas* SFS::Heap::AllocateAtlas(ID3D12CommandQueue* in_pQueue, const DXGI_FORMAT in_format)
{
    SFS::Atlas* pAtlas = nullptr;
    for (auto p : m_atlases)
    {
        if (p->GetFormat() == in_format)
        {
            pAtlas = p;
            break;
        }
    }
    if (nullptr == pAtlas)
    {
        pAtlas = new SFS::Atlas(m_tileHeap.Get(), in_pQueue, m_heapAllocator.GetCapacity(), in_format);
        m_atlases.push_back(pAtlas);
    }
    return pAtlas;
}
