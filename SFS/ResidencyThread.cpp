//==============================================================
// Copyright © Intel Corporation
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
                m_residencyChangedFlag.Wait();

                // flush resources?
                if (GroupRemoveResources::Client::Residency & m_flushResources.GetFlags())
                {
                    // number of resources likely > # to be removed
                    ContainerRemove(m_resources.Acquire(), m_flushResources.BypassLockGetValues());
                    m_resources.Release();
                    m_flushResources.ClearFlag(GroupRemoveResources::Client::Residency);
                }

                std::vector<ResourceBase*> updated;
                updated.reserve(m_resources.size());
                for (auto p : m_resources.Acquire())
                {
                    if (p->UpdateMinMipMap())
                    {
                        updated.push_back(p);
                    }
                }
                m_resources.Release();

                if (updated.size())
                {
                    // only blocks if resource buffer is being re-allocated (effectively never)
                    UINT8* pDest = m_pSFSManager->ResidencyMapAcquire();
                    for (auto p : updated) { p->WriteMinMipMap(pDest); }
                    m_pSFSManager->ResidencyMapRelease();
                }
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
// SFSManager acquires staging area and adds new resources
//-----------------------------------------------------------------------------
void SFS::ResidencyThread::SharePendingResourcesRT(const std::set<ResourceBase*>& in_resources)
{
    std::vector<ResourceBase*> n;
    n.insert(n.begin(), in_resources.begin(), in_resources.end());

    auto& v = m_resources.Acquire();
    for (auto r : v)
    {
        if ((!in_resources.contains(r)) && (r->HasInFlightUpdates()))
        {
            n.push_back(r);
        }
    }
    v.swap(n);
    m_resources.Release();
}
