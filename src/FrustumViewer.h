//==============================================================
// Copyright © Intel Corporation
//
// SPDX-License-Identifier: MIT
// =============================================================

#pragma once

#include <d3d12.h>
#include <wrl.h>
#include <functional>
#include <DirectXMath.h>

class FrustumViewer
{
public:
    FrustumViewer(ID3D12Device* in_pDevice,
        const DXGI_FORMAT in_swapChainFormat,
        const DXGI_FORMAT in_depthFormat,
        UINT in_sampleCount,
        class AssetUploader& in_assetUploader);

    void Draw(ID3D12GraphicsCommandList* in_pCL,
        const DirectX::XMMATRIX& in_combinedTransform,
        float in_fov, float in_aspectRatio);

    void SetView(const DirectX::XMMATRIX& in_viewInverse, float in_scale)
    {
        m_world = in_viewInverse;
        m_frustumConstants.m_scale = in_scale;
    }
private:
    template<typename T> using ComPtr = Microsoft::WRL::ComPtr<T>;
    ComPtr<ID3D12Resource> m_indexBuffer;
    ComPtr<ID3D12Resource> m_vertexBuffer;

    D3D12_INDEX_BUFFER_VIEW m_indexBufferView;
    D3D12_VERTEX_BUFFER_VIEW m_vertexBufferView;

    ComPtr<ID3D12RootSignature> m_rootSignature;
    ComPtr<ID3D12PipelineState> m_pipelineStateFill;
    ComPtr<ID3D12PipelineState> m_pipelineStateWireframe;
    ComPtr<ID3D12Resource> m_constantBuffer;

    struct FrustumConstants
    {
        DirectX::XMMATRIX m_combinedTransform;
        float m_fov;
        float m_aspectRatio;
        float m_scale;
    };
    FrustumConstants m_frustumConstants;

    UINT m_numIndices;

    DirectX::XMMATRIX m_world;
};
