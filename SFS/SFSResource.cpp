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

// Implementation of the few methods required for the ::ResourceBase public (external) interface

#include "pch.h"

#include "SFSResourceBase.h"
#include "SFSManagerSR.h"

//-----------------------------------------------------------------------------
// public interface to destroy object
//-----------------------------------------------------------------------------
void SFS::ResourceBase::Destroy()
{
    delete this;
}

//-----------------------------------------------------------------------------
// create views of resources used directly by the application
//-----------------------------------------------------------------------------
void SFS::ResourceBase::CreateFeedbackView(ID3D12Device* in_pDevice, D3D12_CPU_DESCRIPTOR_HANDLE out_descriptorHandle)
{
    ((ID3D12Device8*)in_pDevice)->CreateSamplerFeedbackUnorderedAccessView(
        m_resources->GetTiledResource(),
        m_resources->GetOpaqueFeedback(),
        out_descriptorHandle);
#if 0
    // FIXME? could copy m_clearUavDescriptor, except this is method is used to create that descriptor also
    in_pDevice->CopyDescriptorsSimple(1, in_descriptorHandle,
        m_resources->GetClearUavHeap()->GetCPUDescriptorHandleForHeapStart(),
        m_clearUavDescriptor);
#endif
}

void SFS::ResourceBase::CreateShaderResourceView(ID3D12Device* in_pDevice, D3D12_CPU_DESCRIPTOR_HANDLE in_descriptorHandle)
{
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = UINT(-1);
    srvDesc.Format = m_resources->GetTiledResource()->GetDesc().Format;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;

    in_pDevice->CreateShaderResourceView(m_resources->GetTiledResource(), &srvDesc, in_descriptorHandle);
}

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
ID3D12Resource* SFS::ResourceBase::GetMinMipMap() const
{
    return m_pSFSManager->GetResidencyMap().GetResource();
}

//-----------------------------------------------------------------------------
// shader reading min-mip-map buffer will want its dimensions
//-----------------------------------------------------------------------------
UINT SFS::ResourceBase::GetMinMipMapWidth() const
{
    return GetNumTilesWidth();
}

UINT SFS::ResourceBase::GetMinMipMapHeight() const
{
    return GetNumTilesHeight();
}

//-----------------------------------------------------------------------------
// IMPORTANT: all min mip maps are stored in a single buffer. offset into the buffer.
// this saves a massive amount of GPU memory, since each min mip map is much smaller than 64KB
//-----------------------------------------------------------------------------
UINT SFS::ResourceBase::GetMinMipMapOffset() const
{
    return m_residencyMapOffsetBase;
}

//-----------------------------------------------------------------------------
// // check if the packed mips are loaded. application likely will not want to use this texture before they have loaded
//-----------------------------------------------------------------------------
bool SFS::ResourceBase::GetPackedMipsResident() const
{
    return (PackedMipStatus::RESIDENT == m_packedMipStatus);// || (PackedMipStatus::NEEDS_TRANSITION == m_packedMipStatus);
}

//-----------------------------------------------------------------------------
// if an object isn't visible, set all refcounts to 0
// this will schedule all tiles to be evicted
//-----------------------------------------------------------------------------
void SFS::ResourceBase::QueueEviction()
{
    m_setZeroRefCounts = true;
}

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
UINT SFS::ResourceBase::GetNumTilesVirtual() const
{
    return m_resources->GetNumTilesVirtual();
}
