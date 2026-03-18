//==============================================================
// Copyright © Intel Corporation
//
// SPDX-License-Identifier: MIT
// =============================================================

#pragma once

// creates a windows per mip of a texture
// screen coordinate system: (0, 0) is bottom-left. like normalized device space, (1, 1) is top-right
// u,v coordinates: (0, 0) is top-left. like images, byte 0 is top left

// application must set the descriptor heap on the command list prior to providing it to Draw()

class TextureViewer
{
public:
    template<typename T> using ComPtr = Microsoft::WRL::ComPtr<T>;
    TextureViewer(ID3D12Resource* in_pResource,
        const DXGI_FORMAT in_swapChainFormat);
    virtual ~TextureViewer();

    void Draw(ID3D12GraphicsCommandList* in_pCL,
        D3D12_GPU_DESCRIPTOR_HANDLE in_gpuDescriptorHandle,
        DirectX::XMFLOAT2 in_position,
        DirectX::XMFLOAT2 in_windowDim,
        D3D12_VIEWPORT in_viewPort,
        int in_visualizationBaseMip, int in_numMips, bool in_vertical);
protected:
    static const unsigned int MIN_WINDOW_DIM = 8;

    TextureViewer() {}
    void CreateResources(
        ID3D12Resource* in_pResource,
        const DXGI_FORMAT in_swapChainFormat,
        const wchar_t* in_pShaderFileName, const char* in_psEntryPoint = "ps");
    void DrawWindows(ID3D12GraphicsCommandList* in_pCL,
        D3D12_GPU_DESCRIPTOR_HANDLE in_gpuDescriptorHandle,
        D3D12_VIEWPORT in_viewPort, UINT in_numWindows);

    ComPtr<ID3D12Device> m_device;
    INT m_descriptorOffset{ 0 };

    std::vector<UINT32> m_constants;
private:
    ComPtr<ID3D12RootSignature> m_rootSignature;
    ComPtr<ID3D12PipelineState> m_pipelineState;

    int m_numMips{ 0 };
};
