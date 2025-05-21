#include "pch.h"

#include "ResidencyThread.h"
#include "SFSManagerBase.h"
#include "SFSResourceBase.h"
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
    , m_removeResources(in_grr)
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

                // add new resources?
                if (m_newResourcesStaging.size() && m_newResourcesLock.TryAcquire())
                {
                    std::vector<ResourceBase*> newResources;
                    newResources.swap(m_newResourcesStaging);
                    m_newResourcesLock.Release();

                    m_streamingResources.insert(m_streamingResources.end(), newResources.begin(), newResources.end());
                }

                // remove resources?
                if (GroupRemoveResources::Client::Residency & m_removeResources.GetFlags())
                {
                    m_removeResources.Acquire();
                    ContainerRemove(m_streamingResources, m_removeResources);
                    m_removeResources.Release();
                    m_removeResources.ClearFlag(GroupRemoveResources::Client::Residency);
                }

                std::vector<ResourceBase*> updated;
                for (auto p : m_streamingResources)
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
void SFS::ResidencyThread::ShareNewResources(const std::vector<ResourceBase*>& in_resources)
{
    m_newResourcesLock.Acquire();
    m_newResourcesStaging.insert(m_newResourcesStaging.end(), in_resources.begin(), in_resources.end());
#ifdef _DEBUG
    for (auto p : m_newResourcesStaging) { ASSERT(p->GetInitialized()); }
#endif
    m_newResourcesLock.Release();
}
