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
        virtual SFSHeap* CreateHeap(UINT in_sizeInMB) override;
        virtual SFSResource* CreateResource(const struct SFSResourceDesc& in_desc,
            SFSHeap* in_pHeap, const std::wstring& in_filename) override;
        virtual void FlushResources(const std::vector<SFSResource*>& in_resources, HANDLE in_event) override;
        virtual void BeginFrame() override;
        virtual ID3D12CommandList* EndFrame(D3D12_CPU_DESCRIPTOR_HANDLE out_minmipmapDescriptorHandle) override;
        //virtual void UseDirectStorage(bool in_useDS) override;
        //virtual bool GetWithinFrame() const override;
        virtual float GetGpuTexelsPerMs() const override;
        virtual UINT GetMaxNumFeedbacksPerFrame() const override;
        virtual float GetGpuTimeMs() const override;
        virtual float GetCpuProcessFeedbackTimeMs() override;
        virtual UINT GetTotalNumUploads() const override;
        virtual UINT GetTotalNumEvictions() const override;
        virtual float GetTotalTileCopyLatencyMs() const override;
        virtual UINT GetTotalNumSubmits() const override;
        virtual UINT GetTotalNumSignals() const override;
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
        BarrierList m_packedMipTransitionBarriers; // transition packed-mips from common (copy dest)

        float m_cpuProcessFeedbackFrameTimeMs{ 0 }; // cpu time spent processing feedback (averaged over m_feedbackTimingFrequency)
        float m_gpuProcessFeedbackFrameTimeMs{ 0 };  // gpu render queue time (averaged over m_feedbackTimingFrequency)

        // average over n frames
        UINT m_feedbackTimingFrequency{ 25 };
        UINT m_numFeedbackTimingFrames{ 0 };
        std::vector<UINT64> m_cpuFeedbackTimes; // cumulative processor ticks at time of last query
        std::vector<UINT> m_numTexels;
        std::vector<float> m_gpuFeedbackTimes;

        float m_texelsPerMs{ 500 };
        float m_gpuFeedbackTime{ 0 };

        UINT m_renderFrameIndex{ 0 }; // between 0 and # swap buffers

        struct CommandList
        {
            void Allocate(ID3D12Device* in_pDevice, UINT in_numAllocators, std::wstring in_name);
            void Reset(UINT in_allocatorIndex);
            ComPtr<ID3D12GraphicsCommandList1> m_commandList;
            std::vector<ComPtr<ID3D12CommandAllocator>> m_allocators;
        };
        // command list to be executed after application draw
        // clear & resolve feedback buffers, coalesces all barriers
        CommandList m_commandListEndFrame;

        void ClearFeedback(ID3D12GraphicsCommandList* in_pCommandList, const std::set<ResourceBase*>& in_resources);
    };
}
