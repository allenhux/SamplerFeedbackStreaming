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

        // finish some initialization until when the packed mips arrive
        // need the swap chain count so we can create per-frame readback buffers
        void Initialize(ID3D12Device8* in_pDevice, const SFSResourceDesc& in_resourceDesc, UINT in_numQueuedFeedback);

        ID3D12Resource* GetTiledResource() const { return m_tiledResource.Get(); }

        ID3D12Resource* GetOpaqueFeedback() const { return m_feedbackResource.Get(); }

#if RESOLVE_TO_TEXTURE
        void ResolveFeedback(ID3D12GraphicsCommandList1* out_pCmdList, ID3D12Resource* in_pDestination);
        void ReadbackFeedback(ID3D12GraphicsCommandList* out_pCmdList, ID3D12Resource* in_pResolvedResource, UINT in_index, UINT in_width, UINT in_height);
        void* GetResolvedReadback(UINT in_index) const { return &m_readbackCpuAddress[in_index * m_readbackStride]; }
#else
        // resolve directly to a cpu destination
        void ResolveFeedback(ID3D12GraphicsCommandList1* out_pCmdList, UINT in_index);
        void* MapResolvedReadback(UINT in_index) const
        {
            void* pCpuAddress{ nullptr };
            m_readback[in_index]->Map(0, nullptr, &pCpuAddress);
            return pCpuAddress;
        }
        void UnmapResolvedReadback(UINT in_index) const { m_readback[in_index]->Unmap(0, nullptr); }
#endif
        UINT GetNumTilesForPackedMips() const { return m_numTilesForPackedMips; }
    private:
        ComPtr<ID3D12Resource> m_tiledResource;
        ComPtr<ID3D12Resource2> m_feedbackResource;
#if RESOLVE_TO_TEXTURE
        // per-swap-buffer cpu readable resolved feedback
        UINT m_readbackStride{ 0 };
        ComPtr<ID3D12Resource> m_readback;
        UINT8* m_readbackCpuAddress{ nullptr };
#else
        std::vector<ComPtr<ID3D12Resource>> m_readback;
#endif

        UINT m_numTilesForPackedMips{ 0 };
    };
}
