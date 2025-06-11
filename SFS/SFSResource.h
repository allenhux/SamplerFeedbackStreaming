//==============================================================
// Copyright © Intel Corporation
//
// SPDX-License-Identifier: MIT
// =============================================================

/*=============================================================================
Implementation of SFS Resource
=============================================================================*/

#pragma once

#include <vector>
#include <d3d12.h>
#include <string>

#include "SamplerFeedbackStreaming.h"
#include "ResourceBase.h"
#include "InternalResources.h"

namespace SFS
{
    class ManagerSR;
    class Heap;

    //=============================================================================
    // unpacked mips are dynamically loaded/evicted, preserving a min-mip-map
    // packed mips are not evicted from the heap (as little as 1 tile for a 16k x 16k texture)
    //=============================================================================
    class Resource : public ResourceBase
    {
        //-----------------------------------------------------------------
        // external APIs
        //-----------------------------------------------------------------
        virtual void Destroy() override;
        //virtual void CreateFeedbackView(D3D12_CPU_DESCRIPTOR_HANDLE out_descriptor) override;
        //virtual void CreateShaderResourceView(D3D12_CPU_DESCRIPTOR_HANDLE in_descriptor) override;
        //virtual UINT GetMinMipMapWidth() const override;
        //virtual UINT GetMinMipMapHeight() const override;
        //virtual UINT GetMinMipMapOffset() const override;
        virtual bool Drawable() const override;
        virtual void QueueFeedback() override;
        virtual void QueueEviction() override;
        //virtual ID3D12Resource* GetTiledResource() const override;
        virtual ID3D12Resource* GetMinMipMap() const override;
        virtual UINT GetNumTilesVirtual() const override;
    public:
        Resource(
            // method that will fill a tile-worth of bits, for streaming
            const std::wstring& in_filename,
            // description with dimension, format, etc.
            const SFSResourceDesc& in_desc,
            // share heap and upload buffers with other InternalResources
            SFS::ManagerSR* in_pSFSManager,
            Heap* in_pHeap)
            : ResourceBase(in_filename, in_desc, in_pSFSManager, in_pHeap) {}
        virtual ~Resource() {}
    };
}
