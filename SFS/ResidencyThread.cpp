//==============================================================
// Copyright � Intel Corporation
//
// SPDX-License-Identifier: MIT
// =============================================================

#include "pch.h"

#include "ResidencyThread.h"
#include "ManagerBase.h"
#include "ResourceBase.h"
#include "ProcessFeedbackThread.h"

//=============================================================================
// severely limited SFSManager interface
//=============================================================================
namespace SFS
{
    class ManagerRT : private ManagerBase
    {
    public:
        UINT8* ResidencyMapAcquire()
        {
            m_residencyMapLock.Acquire();
            return (UINT8*)m_residencyMap.GetData();
        }
        void ResidencyMapRelease() { m_residencyMapLock.Release(); }
    };
}

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
SFS::ResidencyThread::ResidencyThread(ManagerRT* in_pSFSManager, GroupRemoveResources& in_grr,
    int in_threadPriority) :
    m_pSFSManager(in_pSFSManager)
    , m_threadPriority(in_threadPriority)
    , m_flushResources(in_grr)
{}

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
void SFS::ResidencyThread::Start()
{
    if (m_threadRunning) { return; }

    m_threadRunning = true;
    m_thread = std::thread([&]
        {
            while (m_threadRunning)
            {
                // flush resources?
                if (GroupRemoveResources::Client::ResidencyThread & m_flushResources.GetFlags())
                {
                    // number of resources likely > # to be removed
                    ContainerRemove(m_resources, m_flushResources.BypassLockGetValues());

                    if (m_resourcesStaging.Size())
                    {
                        ContainerRemove(m_resourcesStaging.Acquire(), m_flushResources.BypassLockGetValues());
                        m_resourcesStaging.Release();
                    }
                    m_flushResources.ClearFlag(GroupRemoveResources::Client::ResidencyThread);
                }

                // new pending resource list?
                if (m_resourcesStaging.Size())
                {
                    // grab the new working set
                    std::set<ResourceBase*> n;
                    m_resourcesStaging.Swap(n);

                    // keep previous resources that may yet have residency changes
                    for (auto r : m_resources)
                    {
                        if (r->HasInFlightUpdates()) { n.insert(r); }
                    }

                    // create new working set
                    m_resources.swap(n);
                }

                std::vector<ResourceBase*> updated;
                updated.reserve(m_resources.size());
                for (auto p : m_resources)
                {
                    if (p->UpdateMinMipMap())
                    {
                        updated.push_back(p);
                    }
                }

                if (updated.size())
                {
                    // only blocks if resource buffer is being re-allocated (effectively never)
                    UINT8* pDest = m_pSFSManager->ResidencyMapAcquire();
                    for (auto p : updated) { p->WriteMinMipMap(pDest); }
                    m_pSFSManager->ResidencyMapRelease();
                }

                m_residencyChangedFlag.Wait();
            }
        });
    SFS::SetThreadPriority(m_thread, m_threadPriority);
}

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
void SFS::ResidencyThread::Stop()
{
    if (m_threadRunning)
    {
        m_threadRunning = false;
        Wake();
        if (m_thread.joinable())
        {
            m_thread.join();
        }
    }
}

//-----------------------------------------------------------------------------
// PFT acquires staging area provides all resources that it is currently working on ("pending")
// RT may have a slightly larger set including resources that have outstanding updatelists or a change in tile residency
//-----------------------------------------------------------------------------
void SFS::ResidencyThread::SharePendingResourcesRT(std::set<ResourceBase*> in_resources)
{
    // add the resources that still have work to do
    auto& v = m_resourcesStaging.Acquire();
    for (auto r : v)
    {
        if (r->HasInFlightUpdates())
        {
            in_resources.insert(r);
        }
    }

    // replace the old set with the new
    v.swap(in_resources);
    m_resourcesStaging.Release();
}
