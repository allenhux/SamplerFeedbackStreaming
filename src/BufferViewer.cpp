//==============================================================
// Copyright © Intel Corporation
//
// SPDX-License-Identifier: MIT
// =============================================================

#include "pch.h"
#include "BufferViewer.h"

using namespace DirectX;
using Microsoft::WRL::ComPtr;

struct BufferViewerConstantBuffer
{
    float x, y, width, height;
    int bufferWidth, bufferHeight, rowPitch, offset;
};

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
BufferViewer::BufferViewer(
    ID3D12Resource* in_pBuffer,
    UINT in_width, UINT in_height, UINT in_rowPitch, UINT in_offset,
    const DXGI_FORMAT in_swapChainFormat,
    ID3D12DescriptorHeap* in_pDescriptorHeap, INT in_descriptorOffset)
{
    const char* psEntryPoint = "ps";

    D3D12_SHADER_RESOURCE_VIEW_DESC bufferViewDesc{};
    bufferViewDesc.Shader4ComponentMapping = D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(0, 0, 0, 0);
    bufferViewDesc.Format = DXGI_FORMAT_R8_UINT;

    if (D3D12_RESOURCE_DIMENSION_TEXTURE2D == in_pBuffer->GetDesc().Dimension)
    {
        bufferViewDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        bufferViewDesc.Texture2D.MipLevels = 1;
        psEntryPoint = "psTexture";
    }
    else
    {
        bufferViewDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        bufferViewDesc.Buffer.NumElements = (UINT)in_pBuffer->GetDesc().Width;
    }

    m_constants.resize(sizeof(BufferViewerConstantBuffer) / sizeof(UINT32));

    CreateResources(
        in_pBuffer, bufferViewDesc,
        in_swapChainFormat,
        in_pDescriptorHeap, in_descriptorOffset,
        L"BufferViewer.hlsl", psEntryPoint);

    BufferViewerConstantBuffer* pConstants = (BufferViewerConstantBuffer*)m_constants.data();

    pConstants->bufferWidth = in_width;
    pConstants->bufferHeight = in_height;
    pConstants->rowPitch = in_rowPitch;
    pConstants->offset = in_offset;
}

//-----------------------------------------------------------------------------
// note: screen space is -1,-1 to 1,1
//-----------------------------------------------------------------------------
void BufferViewer::Draw(ID3D12GraphicsCommandList* in_pCL,
    DirectX::XMFLOAT2 in_position, DirectX::XMFLOAT2 in_windowDim,
    D3D12_VIEWPORT in_viewPort)
{
    if ((in_windowDim.x < MIN_WINDOW_DIM) || (in_windowDim.y < MIN_WINDOW_DIM))
    {
        return;
    }

    BufferViewerConstantBuffer* pConstants = (BufferViewerConstantBuffer*)m_constants.data();

    pConstants->x = 2 * float(in_position.x) / in_viewPort.Width;
    pConstants->y = 2 * float(in_position.y) / in_viewPort.Height;
    pConstants->width = in_windowDim.x / in_viewPort.Width;
    pConstants->height = in_windowDim.y / in_viewPort.Height;

    DrawWindows(in_pCL, in_viewPort, 2);
}
