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
SFS::ResidencyThread::ResidencyThread(ManagerRT* in_pSFSManager, int in_threadPriority) :
    m_pSFSManager(in_pSFSManager)
    , m_threadPriority(in_threadPriority)
{}

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
void SFS::ResidencyThread::Start()
{
    ASSERT(false == m_threadRunning);

    m_thread = std::thread([&]
        {
            m_threadRunning = true;

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
                if ((nullptr != m_pRemoveResources) &&
                    (m_pRemoveResources->GetFlags() & GroupRemoveResources::Client::Residency))
                {
                    ContainerRemove(m_streamingResources, *m_pRemoveResources);
                    m_pRemoveResources->ClearFlag(GroupRemoveResources::Client::Residency);
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
    m_newResourcesLock.Release();
}
