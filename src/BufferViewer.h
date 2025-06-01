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
        const DXGI_FORMAT in_swapChainFormat,
        // optionally provide a descriptor heap and an offset into that heap
        // if not provided, will create a descriptor heap just for that texture
        ID3D12DescriptorHeap* in_pDescriptorHeap = nullptr,
        INT in_descriptorOffset = 0);

    virtual ~BufferViewer() {}

    void Draw(ID3D12GraphicsCommandList* in_pCL,
        DirectX::XMFLOAT2 in_position,
        DirectX::XMFLOAT2 in_windowDim,
        D3D12_VIEWPORT in_viewPort); 
};
