//==============================================================
// Copyright © Intel Corporation
//
// SPDX-License-Identifier: MIT
// =============================================================

/*=============================================================================
Implementation of SFS Manager

manager for tiled resources
contains shared objects, especially the heap
performs asynchronous copies
use this to create SFSResource and SFSHeap
=============================================================================*/

#pragma once

#include <d3d12.h>
#include <vector>
#include <string>

#include "ManagerBase.h"

namespace SFS
{
    class Manager : public ManagerBase
    {
        //-----------------------------------------------------------------
        // external APIs
        //-----------------------------------------------------------------
        virtual void Destroy() override;
        virtual SFSHeap* CreateHeap(UINT in_maxNumTilesHeap) override;
        virtual SFSResource* CreateResource(const struct SFSResourceDesc& in_desc,
            SFSHeap* in_pHeap, const std::wstring& in_filename) override;
        virtual void BeginFrame(D3D12_CPU_DESCRIPTOR_HANDLE out_minmipmapDescriptorHandle) override;
        virtual CommandLists EndFrame() override;
        //virtual void UseDirectStorage(bool in_useDS) override;
        //virtual bool GetWithinFrame() const override;
        virtual float GetGpuTexelsPerMs() const override;
        virtual UINT GetMaxNumFeedbacksPerFrame() const override;
        virtual float GetGpuTime() const override;
        virtual float GetCpuProcessFeedbackTime() override;
        virtual UINT GetTotalNumUploads() const override;
        virtual UINT GetTotalNumEvictions() const override;
        virtual float GetTotalTileCopyLatency() const override;
        virtual UINT GetTotalNumSubmits() const override;
        virtual void CaptureTraceFile(bool in_captureTrace) override;
        virtual void SetVisualizationMode(UINT in_mode) override;
        //-----------------------------------------------------------------
        // end external APIs
        //-----------------------------------------------------------------
    public:
        Manager(const struct SFSManagerDesc& in_desc, ID3D12Device8* in_pDevice);
        virtual ~Manager() {}
    private:
        D3D12GpuTimer m_gpuTimerResolve; // time for feedback resolve
        SFS::BarrierList m_barrierUavToResolveSrc; // transition copy source to resolve dest
        SFS::BarrierList m_barrierResolveSrcToUav; // transition resolve dest to copy source
        SFS::BarrierList m_packedMipTransitionBarriers; // transition packed-mips from common (copy dest)

        INT64 m_previousFeedbackTime{ 0 }; // m_processFeedbackTime at time of last query
        float m_processFeedbackFrameTime{ 0 }; // cpu time spent processing feedback for the most recent frame

        // every n frames swap
        UINT m_feedbackTimingFrequency{ 100 };
        UINT m_numFeedbackTimingFrames{ 0 };
        float m_texelsPerMs{ 50 };
        float m_numTexelsQueued{ 0 };
        float m_gpuFeedbackTime{ 0 };

#if RESOLVE_TO_TEXTURE
        ComPtr<ID3D12Heap> m_resolvedResourceHeap;
        // feedback resolved on gpu
        std::vector<ComPtr<ID3D12Resource>> m_sharedResolvedResources;
#endif
        void ClearFeedback(ID3D12GraphicsCommandList* in_pCommandList, const std::vector<ResourceBase*>& in_resources);
    };
}
