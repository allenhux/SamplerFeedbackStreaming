//==============================================================
// Copyright © Intel Corporation
//
// SPDX-License-Identifier: MIT
// =============================================================

//=============================================================================
// Implementation of the ::SFSResource public (external) interface
//=============================================================================

#include "pch.h"

#include "SFSResource.h"
#include "ManagerSR.h"

//-----------------------------------------------------------------------------
// public interface to destroy object
//-----------------------------------------------------------------------------
void SFS::Resource::Destroy()
{
    delete this;
}

//-----------------------------------------------------------------------------
// create views of resources used directly by the application
//-----------------------------------------------------------------------------
void SFS::ResourceBase::CreateFeedbackView(D3D12_CPU_DESCRIPTOR_HANDLE out_descriptorHandle)
{
    m_pSFSManager->GetDevice()->CreateSamplerFeedbackUnorderedAccessView(
        m_resources.GetTiledResource(),
        m_resources.GetOpaqueFeedback(),
        out_descriptorHandle);
#if 0
    // FIXME? instead of create, could copy m_clearUavDescriptor, except this is method is used to create that descriptor also
    in_pDevice->CopyDescriptorsSimple(1, in_descriptorHandle,
        m_resources.GetClearUavHeap()->GetCPUDescriptorHandleForHeapStart(),
        m_clearUavDescriptor);
#endif
}

void SFS::ResourceBase::CreateShaderResourceView(D3D12_CPU_DESCRIPTOR_HANDLE in_descriptorHandle)
{
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = UINT(-1);
    srvDesc.Format = m_resources.GetTiledResource()->GetDesc().Format;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;

    m_pSFSManager->GetDevice()->CreateShaderResourceView(m_resources.GetTiledResource(), &srvDesc, in_descriptorHandle);
}

//-----------------------------------------------------------------------------
// application should not use this texture before packed mips are loaded AND
// an offset into the shared residency map buffer has been assigned
//-----------------------------------------------------------------------------
bool SFS::Resource::Drawable()
{
    // the application should call Drawable() before EndFrame()
    // during EndFrame() packed mips are transitioned to D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE
    if (PackedMipStatus::NEEDS_TRANSITION <= m_packedMipStatus)
    {
        // SFSManager::AllocateSharedResidencyMap() allocates for all new resources, drawable or not
        return true;        
    }

    if (PackedMipStatus::RESET == m_packedMipStatus)
    {
        // prevent adding this resource more than once
        m_packedMipStatus = PackedMipStatus::UNINITIALIZED;

        m_pSFSManager->Renew((ResourceBase*)this);
    }

    return false;
}

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
void SFS::Resource::QueueFeedback()
{
    m_pSFSManager->QueueFeedback(this);
    // requesting feedback means this resource will be stale
    m_pSFSManager->SetPending(this);
}

//-----------------------------------------------------------------------------
// if an object isn't visible, set all refcounts to 0
// this will schedule all tiles to be evicted
//-----------------------------------------------------------------------------
void SFS::Resource::QueueEviction()
{
    if (!m_refCountsZero)
    {
        m_evictAll = true;
        m_pSFSManager->SetPending(this);
    }
}

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
ID3D12Resource* SFS::Resource::GetMinMipMap() const
{
    return m_pSFSManager->GetResidencyMap().GetResource();
}

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
UINT SFS::Resource::GetNumTilesVirtual() const
{
    return m_resourceDesc.m_mipInfo.m_numTilesForStandardMips + m_resources.GetNumTilesForPackedMips();
}

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
UINT SFS::Resource::GetNumStandardMips() const
{
    return m_maxMip;
}
