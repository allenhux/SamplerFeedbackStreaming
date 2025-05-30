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

#include "InternalResources.h"

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
SFS::InternalResources::InternalResources()
{}

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
void SFS::InternalResources::CreateTiledResource(ID3D12Device8* in_pDevice, const SFSResourceDesc& in_resourceDesc)
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

    NameStreamingTexture();
}

//-----------------------------------------------------------------------------
// Defer creating some resources until after packed mips arrive
// to reduce impact of creating an SFS Resource
//-----------------------------------------------------------------------------
void SFS::InternalResources::Initialize(ID3D12Device8* in_pDevice, UINT in_swapChainBufferCount)
{
    // query the reserved resource for its tile properties
    // allocate data structure according to tile properties
    D3D12_TILE_SHAPE tileShape{}; // e.g. a 64K tile may contain 128x128 texels @ 4B/pixel
    UINT subresourceCount = 1; // only care about the topmost mip level
    D3D12_SUBRESOURCE_TILING tiling;
    in_pDevice->GetResourceTiling(GetTiledResource(), nullptr, nullptr, &tileShape, &subresourceCount, 0, &tiling);

    UINT numTilesWidth = tiling.WidthInTiles;
    UINT numTilesHeight = tiling.HeightInTiles;
    UINT tileTexelWidth = tileShape.WidthInTexels;
    UINT tileTexelHeight = tileShape.HeightInTexels;

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

#if RESOLVE_TO_TEXTURE
    // create gpu-side resolve destination
    {
        const auto heapProperties = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);

        auto textureDesc = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R8_UINT, numTilesWidth, numTilesHeight, 1, 1);
        textureDesc.Alignment = D3D12_SMALL_RESOURCE_PLACEMENT_ALIGNMENT;

        ThrowIfFailed(in_pDevice->CreateCommittedResource(
            &heapProperties,
            D3D12_HEAP_FLAG_NONE,
            &textureDesc,
            // NOTE: though used as RESOLVE_DEST, it is also copied to the CPU
            // start in the copy_source state to align with transition barrier logic in SFSManager
            D3D12_RESOURCE_STATE_COPY_SOURCE,
            nullptr,
            IID_PPV_ARGS(&m_resolvedResource)));
        m_resolvedResource->SetName(L"m_resolvedResource");
    }
#endif

    {
        D3D12_RESOURCE_DESC rd = CD3DX12_RESOURCE_DESC::Buffer(numTilesWidth * numTilesHeight);
        const auto resolvedHeapProperties = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_READBACK);

#if RESOLVE_TO_TEXTURE
        // CopyTextureRegion requires pitch multiple of D3D12_TEXTURE_DATA_PITCH_ALIGNMENT = 256
        UINT pitch = numTilesWidth;
        pitch = (pitch + 0x0ff) & ~0x0ff;
        m_readbackStride = pitch * (UINT)numTilesHeight;
        rd.Width = m_readbackStride * in_swapChainBufferCount;
#endif

        ThrowIfFailed(in_pDevice->CreateCommittedResource(
            &resolvedHeapProperties,
            D3D12_HEAP_FLAG_NONE,
            &rd,
#if RESOLVE_TO_TEXTURE
            D3D12_RESOURCE_STATE_COPY_DEST,
#else
            D3D12_RESOURCE_STATE_RESOLVE_DEST,
#endif
            nullptr,
            IID_PPV_ARGS(&m_readback)));
        static UINT resolveCount = 0;
        m_readback->SetName(L"ResolveDest");
        m_readback->Map(0, nullptr, (void**)&m_readbackCpuAddress);
    }
}

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
void SFS::InternalResources::ClearFeedback(
    ID3D12GraphicsCommandList* out_pCmdList,
    const D3D12_GPU_DESCRIPTOR_HANDLE in_gpuDescriptor,
    // CPU descriptor corresponding to separate CPU heap /not/ bound to command list
    const D3D12_CPU_DESCRIPTOR_HANDLE in_cpuDescriptor)
{
    // note clear value is ignored when clearing feedback maps
    UINT clearValue[4]{};
    out_pCmdList->ClearUnorderedAccessViewUint(
        in_gpuDescriptor,
        in_cpuDescriptor,
        m_feedbackResource.Get(),
        clearValue, 0, nullptr);
}

//-----------------------------------------------------------------------------
// write command to resolve the opaque feedback to a min-mip feedback map
//-----------------------------------------------------------------------------
#if RESOLVE_TO_TEXTURE
void SFS::InternalResources::ResolveFeedback(ID3D12GraphicsCommandList1* out_pCmdList, UINT)
{
    auto resolveDest = m_resolvedResource.Get();
#else
void SFS::InternalResources::ResolveFeedback(ID3D12GraphicsCommandList1 * out_pCmdList, UINT in_index)
{
    auto resolveDest = m_resolvedReadback[in_index].Get();
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
void SFS::InternalResources::ReadbackFeedback(ID3D12GraphicsCommandList* out_pCmdList, UINT in_index)
{
    ID3D12Resource* pResolvedReadback = m_readback.Get();
    auto srcDesc = m_resolvedResource->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout{ in_index * m_readbackStride,
        {srcDesc.Format, (UINT)srcDesc.Width, srcDesc.Height, 1, (UINT)srcDesc.Width } };
    layout.Footprint.RowPitch = (layout.Footprint.RowPitch + 0x0ff) & ~0x0ff;

    D3D12_TEXTURE_COPY_LOCATION srcLocation = CD3DX12_TEXTURE_COPY_LOCATION(m_resolvedResource.Get(), 0);
    D3D12_TEXTURE_COPY_LOCATION dstLocation = CD3DX12_TEXTURE_COPY_LOCATION(pResolvedReadback, layout);

    out_pCmdList->CopyTextureRegion(
        &dstLocation,
        0, 0, 0,
        &srcLocation,
        nullptr);
}
#endif

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
void SFS::InternalResources::NameStreamingTexture()
{
    static UINT m_streamingResourceID = 0;
    m_tiledResource->SetName(
        AutoString("m_streamingTexture", m_streamingResourceID).str().c_str());
    m_streamingResourceID++;
}

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
void* SFS::InternalResources::MapResolvedReadback(UINT in_index) const
{
    return &m_readbackCpuAddress[in_index * m_readbackStride];
}

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
void SFS::InternalResources::UnmapResolvedReadback(UINT) const
{
}
