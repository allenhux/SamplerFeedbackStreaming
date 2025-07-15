//==============================================================
// Copyright © Intel Corporation
//
// SPDX-License-Identifier: MIT
// =============================================================

#include "pch.h"

#include "InternalResources.h"

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
SFS::InternalResources::InternalResources()
{}

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
void SFS::InternalResources::Initialize(ID3D12Device8* in_pDevice, const SFSResourceDesc& in_resourceDesc, UINT in_numQueuedFeedback)
{
    // Create tiled resource
    {
        D3D12_RESOURCE_DESC rd = CD3DX12_RESOURCE_DESC::Tex2D(
            (DXGI_FORMAT)in_resourceDesc.m_textureFormat,
            in_resourceDesc.m_width,
            in_resourceDesc.m_height, 1,
            (UINT16)in_resourceDesc.m_mipInfo.m_numStandardMips + (UINT16)in_resourceDesc.m_mipInfo.m_numPackedMips
        );

        // Layout must be D3D12_TEXTURE_LAYOUT_64KB_UNDEFINED_SWIZZLE when creating reserved resources
        rd.Layout = D3D12_TEXTURE_LAYOUT_64KB_UNDEFINED_SWIZZLE;

        ThrowIfFailed(in_pDevice->CreateReservedResource(
            &rd,
            // application is allowed to use before packed mips are loaded, but it's really a copy dest
            D3D12_RESOURCE_STATE_COMMON,
            nullptr,
            IID_PPV_ARGS(&m_tiledResource)));
        static UINT resourceID;
        m_tiledResource->SetName(AutoString("m_streamingTexture", resourceID++).str().c_str());
    }

    // query the reserved resource for its tile properties
    // allocate data structure according to tile properties
    D3D12_TILE_SHAPE tileShape{}; // e.g. a 64K tile may contain 128x128 texels @ 4B/pixel
    UINT subresourceCount = 1; // only care about the topmost mip level
    D3D12_SUBRESOURCE_TILING tiling{}; // required argument
    D3D12_PACKED_MIP_INFO packedMipInfo{};
    in_pDevice->GetResourceTiling(GetTiledResource(), nullptr, &packedMipInfo, &tileShape, &subresourceCount, 0, &tiling);

    ASSERT(in_resourceDesc.m_standardMipInfo[0].m_widthTiles == tiling.WidthInTiles);
    ASSERT(in_resourceDesc.m_standardMipInfo[0].m_heightTiles == tiling.HeightInTiles);

    UINT numTilesWidth = tiling.WidthInTiles;
    UINT numTilesHeight = tiling.HeightInTiles;
    UINT tileTexelWidth = tileShape.WidthInTexels;
    UINT tileTexelHeight = tileShape.HeightInTexels;
    m_numTilesForPackedMips = packedMipInfo.NumTilesForPackedMips;

    // create the feedback map
    // the dimensions of the feedback map must match the size of the streaming texture
    {
        auto desc = m_tiledResource->GetDesc();
        D3D12_RESOURCE_DESC1 sfbDesc = CD3DX12_RESOURCE_DESC1::Tex2D(
            DXGI_FORMAT_SAMPLER_FEEDBACK_MIN_MIP_OPAQUE,
            desc.Width, desc.Height, desc.DepthOrArraySize, desc.MipLevels);
        sfbDesc.SamplerFeedbackMipRegion = D3D12_MIP_REGION{
            tileTexelWidth, tileTexelHeight, 1 };

        // the feedback texture must be in the unordered state to be written, then transitioned to RESOLVE_SOURCE
        sfbDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

        const auto heapProperties = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
        ThrowIfFailed(in_pDevice->CreateCommittedResource2(
            &heapProperties, D3D12_HEAP_FLAG_NONE,
            &sfbDesc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            nullptr, // not a render target, so optimized clear value illegal. That's ok, clear value is ignored on feedback maps
            nullptr, IID_PPV_ARGS(&m_feedbackResource)));
        m_feedbackResource->SetName(L"m_feedbackResource");
    }

    {
        D3D12_RESOURCE_DESC rd = CD3DX12_RESOURCE_DESC::Buffer(numTilesWidth * numTilesHeight);
        const auto resolvedHeapProperties = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_READBACK);

#if RESOLVE_TO_TEXTURE
        // CopyTextureRegion requires pitch multiple of D3D12_TEXTURE_DATA_PITCH_ALIGNMENT = 256
        constexpr UINT alignmentMask = D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1;
        UINT pitch = numTilesWidth;
        pitch = (pitch + alignmentMask) & ~alignmentMask;
        m_readbackStride = pitch * (UINT)numTilesHeight;
        rd.Width = m_readbackStride * in_numQueuedFeedback;

        ThrowIfFailed(in_pDevice->CreateCommittedResource(
            &resolvedHeapProperties,
            D3D12_HEAP_FLAG_NONE,
            &rd, D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr,
            IID_PPV_ARGS(&m_readback)));
        static UINT resolveCount = 0;
        m_readback->SetName(L"Readback");
        m_readback->Map(0, nullptr, (void**)&m_readbackCpuAddress);
#else
        m_readback.resize(in_numQueuedFeedback);
        m_readbackCpuAddress.resize(in_numQueuedFeedback);

        //-----------------------------------
        //
        // NOTE: Debug Layer incorrectly reports error regarding state of resolve dest if in readback heap.
        // "D3D12 ERROR: ID3D12CommandList::ResolveSubresourceRegion: Using ResolveSubresourceRegion on Command List (0x000001A375B19C90:'Unnamed ID3D12GraphicsCommandList Object'): Resource state (0x400: D3D12_RESOURCE_STATE_COPY_DEST) of resource (0x000001A367BBE4F0:'ResolveDest 1') (subresource: 0) is invalid for use as a destination subresource.  Expected State Bits (all): 0x1000: D3D12_RESOURCE_STATE_RESOLVE_DEST, Actual State: 0x400: D3D12_RESOURCE_STATE_COPY_DEST, Missing State: 0x1000: D3D12_RESOURCE_STATE_RESOLVE_DEST. [ EXECUTION ERROR #538: INVALID_SUBRESOURCE_STATE]"
        // 
        // Microsoft spec for D3D12_HEAP_TYPE_READBACK states:
        // "Resources in this heap must be created with D3D12_RESOURCE_STATE_COPY_DEST, and cannot be changed away from this."
        //
        // However, see Sampler Feedback spec under "Deciding whether to trancode [sic] MinMip feedback maps to a texture or buffer"
        // "applications may choose to decode to a buffer if direct CPU readback is desired."
        // 
        //-----------------------------------

        for (UINT i = 0; i < m_readback.size(); i++)
        {
            ThrowIfFailed(in_pDevice->CreateCommittedResource(
                &resolvedHeapProperties,
                D3D12_HEAP_FLAG_NONE,
                &rd, D3D12_RESOURCE_STATE_RESOLVE_DEST,
                nullptr,
                IID_PPV_ARGS(&m_readback[i])));
            m_readback[i]->Map(0, nullptr, (void**)&m_readbackCpuAddress[i]);
            m_readback[i]->SetName(AutoString(L"ResolveDest_", i).str().c_str());
        }
#endif
    }
}

//-----------------------------------------------------------------------------
// write command to resolve the opaque feedback to a min-mip feedback map
//-----------------------------------------------------------------------------
#if RESOLVE_TO_TEXTURE
void SFS::InternalResources::ResolveFeedback(ID3D12GraphicsCommandList1* out_pCmdList, ID3D12Resource* resolveDest)
{
#else
void SFS::InternalResources::ResolveFeedback(ID3D12GraphicsCommandList1 * out_pCmdList, UINT in_index)
{
    auto resolveDest = m_readback[in_index].Get();
#endif

    // resolve the min mip map
    // can resolve directly to a host readable buffer
    out_pCmdList->ResolveSubresourceRegion(
        resolveDest,
        0,                   // decode target only has 1 layer (or is a buffer)
        0, 0,
        m_feedbackResource.Get(),
        UINT_MAX,            // decode SrcSubresource must be UINT_MAX
        nullptr,             // src rect is not supported for min mip maps
        DXGI_FORMAT_R8_UINT, // decode format must be R8_UINT
        D3D12_RESOLVE_MODE_DECODE_SAMPLER_FEEDBACK
    );

}

#if RESOLVE_TO_TEXTURE
//-----------------------------------------------------------------------------
// write command to copy GPU resolved feedback to CPU readable readback buffer
//-----------------------------------------------------------------------------
void SFS::InternalResources::ReadbackFeedback(ID3D12GraphicsCommandList* out_pCmdList,
    ID3D12Resource* in_pResolvedResource, UINT in_index, UINT in_width, UINT in_height)
{
    ID3D12Resource* pResolvedReadback = m_readback.Get();

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT dstLayout{ in_index * m_readbackStride,
        {DXGI_FORMAT_R8_UINT, in_width, in_height, 1, in_width } };
    // CopyTextureRegion requires pitch multiple of D3D12_TEXTURE_DATA_PITCH_ALIGNMENT = 256
    constexpr UINT alignmentMask = D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1;
    dstLayout.Footprint.RowPitch = (dstLayout.Footprint.RowPitch + alignmentMask) & ~alignmentMask;

    D3D12_TEXTURE_COPY_LOCATION srcLocation = CD3DX12_TEXTURE_COPY_LOCATION(in_pResolvedResource, 0);
    D3D12_TEXTURE_COPY_LOCATION dstLocation = CD3DX12_TEXTURE_COPY_LOCATION(pResolvedReadback, dstLayout);

    D3D12_BOX sourceRegion;
    sourceRegion.left = 0;
    sourceRegion.top = 0;
    sourceRegion.right = in_width;
    sourceRegion.bottom = in_height;
    sourceRegion.front = 0;
    sourceRegion.back = 1;


    out_pCmdList->CopyTextureRegion(
        &dstLocation,
        0, 0, 0,
        &srcLocation,
        &sourceRegion);
}
#endif

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
void* SFS::InternalResources::MapResolvedReadback(UINT in_index) const
{
#if RESOLVE_TO_TEXTURE
    return &m_readbackCpuAddress[in_index * m_readbackStride];
#else
    return m_readbackCpuAddress[in_index];
#endif
}

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
void SFS::InternalResources::UnmapResolvedReadback(UINT) const
{
}
