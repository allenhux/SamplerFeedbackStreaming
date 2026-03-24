//==============================================================
// Copyright © Intel Corporation
//
// SPDX-License-Identifier: MIT
// =============================================================

#pragma once

#include "TextureViewer.h"

// create a window to show a buffer of UINT

class BufferViewer : public TextureViewer
{
public:
    BufferViewer(ID3D12Resource* in_pBuffer,
        UINT in_width, UINT in_height, UINT in_rowPitch, UINT in_offset,
        const DXGI_FORMAT in_swapChainFormat);

    virtual ~BufferViewer() {}

    void Draw(ID3D12GraphicsCommandList* in_pCL,
        D3D12_GPU_DESCRIPTOR_HANDLE in_gpuDescriptorHandle,
        DirectX::XMFLOAT2 in_position,
        DirectX::XMFLOAT2 in_windowDim,
        D3D12_VIEWPORT in_viewPort); 
};
