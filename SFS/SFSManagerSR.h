//*********************************************************
//
// Copyright 2020 Intel Corporation 
//
// Permission is hereby granted, free of charge, to any 
// person obtaining a copy of this software and associated 
// documentation files(the "Software"), to deal in the Software 
// without restriction, including without limitation the rights 
// to use, copy, modify, merge, publish, distribute, sublicense, 
// and/or sell copies of the Software, and to permit persons to 
// whom the Software is furnished to do so, subject to the 
// following conditions :
// The above copyright notice and this permission notice shall 
// be included in all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, 
// EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF 
// MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
// NONINFRINGEMENT.IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT 
// HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, 
// WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, 
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER 
// DEALINGS IN THE SOFTWARE.
//
//*********************************************************


/*=============================================================================
SFS Manager interface for the SFS Resource class
=============================================================================*/

#pragma once

#include <d3d12.h>
#include <vector>
#include <memory>
#include <thread>

#include "SFSManagerBase.h"
#include "DataUploader.h"

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
            ASSERT(!GetWithinFrame());
            m_removeResources.insert(in_pResource);
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

        void SetResidencyChanged() { m_residencyThread.Wake(); }

        FileHandle* OpenFile(const std::wstring& in_filename) { return m_dataUploader.OpenFile(in_filename); }

        void SetPending(ResourceBase* in_pResource) { m_pendingResources.push_back(in_pResource); }
    };
}
