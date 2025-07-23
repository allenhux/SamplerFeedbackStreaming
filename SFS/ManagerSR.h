//==============================================================
// Copyright © Intel Corporation
//
// SPDX-License-Identifier: MIT
// =============================================================

/*=============================================================================
SFS Manager interface for the SFS Resource class
=============================================================================*/

#pragma once

#include <d3d12.h>
#include <vector>
#include <memory>
#include <thread>

#include "ManagerBase.h"
#include "DataUploader.h"
#include "ResourceBase.h"

//=============================================================================
// manager for tiled resources
// contains shared objects, especially the heap
// performs asynchronous copies
// use this to create the SFSResource
//=============================================================================
namespace SFS
{
    class ManagerSR : public ManagerBase
    {
    public:
        ID3D12Device8* GetDevice() const { return m_device.Get(); }

        UINT GetNumSwapBuffers() const { return m_numSwapBuffers; }

        UINT GetEvictionDelay() const { return m_evictionDelay; }

        // stop tracking this SFSResource. Called by its destructor
        void Remove(ResourceBase* in_pResource)
        {
            m_removeResources.Acquire().insert(in_pResource);
            m_removeResources.Release();
        }

        UploadBuffer& GetResidencyMap() { return m_residencyMap; }

        SFS::UpdateList* AllocateUpdateList(ResourceBase* in_pStreamingResource)
        {
            return m_dataUploader.AllocateUpdateList((SFS::ResourceDU*)in_pStreamingResource);
        }

        // called exclusively within ProcessFeedback
        void SubmitUpdateList(SFS::UpdateList& in_updateList)
        {
            m_dataUploader.SubmitUpdateList(in_updateList);
        }

        // a fence on the render (direct) queue used to determine when feedback has been written & resolved
        UINT64 GetFrameFenceValue() const { return m_frameFenceValue; }

        ID3D12CommandQueue* GetMappingQueue() const
        {
            return m_dataUploader.GetMappingQueue();
        }

        FileHandle* OpenFile(const std::wstring& in_filename) { return m_dataUploader.OpenFile(in_filename); }

        void SetPending(ResourceBase* in_pResource) { m_pendingResources.insert(in_pResource); }

        // called by Resource::Drawable() if the resource has been Reset() and now needs packed mips
        void Renew(ResourceBase* in_pResource)
        {
            std::vector<ResourceBase*> v = { (ResourceBase*)in_pResource };
            m_processFeedbackThread.ShareNewResources(std::move(v));
            m_packedMipTransitionResources.push_back(in_pResource);
        }
    };
}
