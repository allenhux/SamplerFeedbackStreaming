//==============================================================
// Copyright © Intel Corporation
//
// SPDX-License-Identifier: MIT
// =============================================================

#pragma once

#include <vector>

#include "Streaming.h"
#include "SamplerFeedbackStreaming.h" // for RESOLVE_TO_TEXTURE

namespace SFS
{
    class InternalResources
    {
    public:
        InternalResources();

        void CreateTiledResource(ID3D12Device8* in_pDevice, const SFSResourceDesc& in_resourceDesc);

        // finish some initialization until when the packed mips arrive
        // need the swap chain count so we can create per-frame readback buffers
        void Initialize(ID3D12Device8* in_pDevice, UINT in_swapChainBufferCount);

        ID3D12Resource* GetTiledResource() const { return m_tiledResource.Get(); }

        void* MapResolvedReadback(UINT in_index) const;
        void UnmapResolvedReadback(UINT in_index) const;

#if RESOLVE_TO_TEXTURE
        // for visualization
        ID3D12Resource* GetResolvedFeedback() const { return m_resolvedResource.Get(); }
#endif

        ID3D12Resource* GetOpaqueFeedback() const { return m_feedbackResource.Get(); }

        void ClearFeedback(ID3D12GraphicsCommandList* out_pCmdList, const D3D12_GPU_DESCRIPTOR_HANDLE in_gpuDescriptor,
            const D3D12_CPU_DESCRIPTOR_HANDLE in_cpuDescriptor);

        void ResolveFeedback(ID3D12GraphicsCommandList1* out_pCmdList, UINT in_index);
#if RESOLVE_TO_TEXTURE
        void ReadbackFeedback(ID3D12GraphicsCommandList* out_pCmdList, UINT in_index);
#endif

    private:
        ComPtr<ID3D12Resource> m_tiledResource;
        ComPtr<ID3D12Resource2> m_feedbackResource;

#if RESOLVE_TO_TEXTURE
        // feedback resolved on gpu for visualization
        ComPtr<ID3D12Resource> m_resolvedResource;
#endif
        // per-swap-buffer cpu readable resolved feedback
        UINT m_readbackStride{ 0 };
        ComPtr<ID3D12Resource> m_readback;
        UINT8* m_readbackCpuAddress{ nullptr };

        void NameStreamingTexture();
    };
}
