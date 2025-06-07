//==============================================================
// Copyright © Intel Corporation
//
// SPDX-License-Identifier: MIT
// =============================================================

#include "pch.h"

#include "ProcessFeedbackThread.h"
#include "ManagerBase.h"
#include "ResourceBase.h"

//=============================================================================
// severely limited SFSManager interface
//=============================================================================
namespace SFS
{
    class ManagerPFT : private ManagerBase
    {
    public:
        UINT64 GetFrameFenceCompletedValue() { return m_frameFence->GetCompletedValue(); }

        void WakeResidencyThread() { m_residencyThread.Wake(); }

        void ShareNewResourcesRT(const std::vector<ResourceBase*>& in_resources)
        {
            m_residencyThread.ShareNewResourcesRT(in_resources);
        }
        auto GetReadbackSet(UINT64 in_frameFenceValue)
        {
            ReadbackSet* pSet = nullptr;
            for (auto i = m_readbackSets.begin(); i != m_readbackSets.end(); i++)
            {
                if ((!i->m_free) && (i->m_fenceValue <= in_frameFenceValue))
                {
                    pSet = &(*i);
                    break;
                }
            }
            return pSet;
        }
    };
}

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
SFS::ProcessFeedbackThread::ProcessFeedbackThread(ManagerPFT* in_pSFSManager,
    DataUploader& in_dataUploader, UINT in_minNumUploadRequests, int in_threadPriority) :
    m_pSFSManager(in_pSFSManager)
    , m_dataUploader(in_dataUploader)
    , m_threadPriority(in_threadPriority)
    , m_minNumUploadRequests(in_minNumUploadRequests)
{
}

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
SFS::ProcessFeedbackThread::~ProcessFeedbackThread()
{
    // delete resources that were in the progress of deletion when the thread was stopped
    for (auto p : m_removeResources)
    {
        delete p;
    }

    for (auto p : m_removeResourcesStaging)
    {
        delete p;
    }
}

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
void SFS::ProcessFeedbackThread::SignalFileStreamer()
{
    m_dataUploader.SignalFileStreamer();
    m_numTotalSubmits.fetch_add(1, std::memory_order_relaxed);
}

//-----------------------------------------------------------------------------
// per frame, call SFSResource::ProcessFeedback()
//-----------------------------------------------------------------------------
void SFS::ProcessFeedbackThread::Start()
{
    if (m_threadRunning) { return; }

    m_threadRunning = true;
    m_thread = std::thread([&]
        {
            UINT uploadsRequested = 0; // remember if any work was queued so we can signal afterwards
            UINT64 previousFrameFenceValue = m_pSFSManager->GetFrameFenceCompletedValue();


            // start timer for this frame
            INT64 prevFrameTime = m_cpuTimer.GetTime();

            while (m_threadRunning)
            {

                bool newFrame = false;
                UINT64 frameFenceValue = m_pSFSManager->GetFrameFenceCompletedValue();
                if (previousFrameFenceValue != frameFenceValue)
                {
                    previousFrameFenceValue = frameFenceValue;
                    newFrame = true;
                }

                // only look for new or to-delete resources once per frame
                if (newFrame)
                {
                    // check for new resources
                    if (m_newResourcesStaging.size() && m_newResourcesLock.TryAcquire())
                    {
                        // grab them and release the lock quickly
                        std::vector<ResourceBase*> tmpResources;
                        tmpResources.swap(m_newResourcesStaging);
                        m_newResourcesLock.Release();

                        m_newResources.insert(m_newResources.end(), tmpResources.begin(), tmpResources.end());
                    }

                    // check for resources where the application has called QueueFeedback() or QueueEviction()
                    if (m_pendingResourceStaging.size() && m_pendingLock.TryAcquire())
                    {
                        // grab them and release the lock quickly
                        std::vector<ResourceBase*> tmpResources;
                        tmpResources.swap(m_pendingResourceStaging);
                        m_pendingLock.Release();
                        m_activeResources.insert(tmpResources.begin(), tmpResources.end());
                    }

                    // compare active resources to resources that are to be removed
                    // NOTE: doing this before InitPackedMips and sharing with ResidencyThread
                    //       as m_newResources may contain resources that have not been
                    //       propagated to residency thread
                    CheckRemoveResources();

                    // propagate new resources to residency thread only after they have packed mips in-flight
                    // note these are new resources that completed initialization last frame
                    // note only propagating once per frame
                    if (m_residencyShareNewResources.size())
                    {
#ifdef _DEBUG
                        for (auto p : m_residencyShareNewResources) { ASSERT(p->GetInitialized()); }
#endif
                        m_pSFSManager->ShareNewResourcesRT(m_residencyShareNewResources);
                        m_residencyShareNewResources.clear();
                    }
                }

                // prioritize loading packed mips, as objects shouldn't be displayed until packed mips load                
                // InitPackedMips() must be called on every resource that needs to load packed mips
                // once packed mips are scheduled to load, can share that resource with the residency thread
                // NOTE: can successfully InitPackedMips() (on different resources) multiple times per frame
                if (m_newResources.size())
                {
                    UINT num = (UINT)m_newResources.size();
                    for (UINT i = 0; i < num;)
                    {
                        auto pResource = m_newResources[i];
                        if (pResource->InitPackedMips())
                        {
                            m_residencyShareNewResources.push_back(pResource);
                            // O(1) remove element from vector
                            num--;
                            m_newResources[i] = m_newResources[num];
                        }
                        else
                        {
                            i++;
                        }
                    }
                    uploadsRequested += (UINT)m_newResources.size() - num;
                    m_newResources.resize(num);
                }

                bool flushPendingUploadRequests = false;

                // Once per frame: process feedback buffers
                if (newFrame)
                {
                    // flush any pending uploads from previous frame
                    if (uploadsRequested) { flushPendingUploadRequests = true; }

                    for (auto i = m_activeResources.begin(); i != m_activeResources.end();)
                    {
                        auto pResource = *i;
                        pResource->ProcessFeedback(frameFenceValue);
                        if (pResource->HasAnyWork())
                        {
                            if (pResource->IsStale())
                            {
                                m_pendingResources.insert(pResource);
                            }
                            i++;
                        }
                        else
                        {
                            i = m_activeResources.erase(i);
                        }
                    }
#if 0
                    // FIXME PoC not robust
                    // loop over feedback buffers that need processing
                    // fairly often, have more than 1 set (frame's worth) of feedback buffers to process
                    // start with newest buffer, then only update resources in older buffers
                    //     that haven't been updated already (with newer data)
                    std::set<ResourceBase*> pending2;
                    while (1)
                    {
                        auto pReadbackSet = m_pSFSManager->GetReadbackSet(previousFrameFenceValue);
                        if (pReadbackSet)
                        {
                            if (0 == pending2.size())
                            {
                                pending2.insert(pReadbackSet->m_resources.begin(), pReadbackSet->m_resources.end());
                            }
                            pReadbackSet->m_resources.clear();
                            pReadbackSet->m_free = true;
                            continue;
                        }
                        break;
                    }
#endif
                }

                // push uploads and evictions for stale resources
                if (m_pendingResources.size())
                {
                    UINT numEvictions = 0;
                    for (auto i = m_pendingResources.begin(); i != m_pendingResources.end();)
                    {
                        ResourceBase* pResource = *i;

                        // tiles that are "loading" can't be evicted. as soon as they arrive, they can be.
                        // note: since we aren't unmapping evicted tiles, we can evict even if no UpdateLists are available
                        numEvictions += pResource->QueuePendingTileEvictions();

                        if (m_dataUploader.GetNumUpdateListsAvailable()
                            // with DirectStorage Queue::EnqueueRequest() can block.
                            // when there are many pending uploads, there can be multiple frames of waiting.
                            // if we wait too long in this loop, we miss calling ProcessFeedback() above which adds pending uploads & evictions
                            // this is a vicious feedback cycle that leads to even more pending requests, and even longer delays.
                            // the following check avoids enqueueing more uploads if the frame has changed:
                            && (m_pSFSManager->GetFrameFenceCompletedValue() == previousFrameFenceValue)
                            && m_threadRunning) // don't add work while exiting
                        {
                            uploadsRequested += pResource->QueuePendingTileLoads();
                        }

                        if (pResource->IsStale()) // still have work to do?
                        {
                            i++;
                        }
                        else
                        {
                            i = m_pendingResources.erase(i);
                        }
                    }
                    if (numEvictions) { m_dataUploader.AddEvictions(numEvictions); }
                }

                // if there are uploads, maybe signal depending on heuristic to minimize # signals
                if (uploadsRequested)
                {
                    // tell the file streamer to signal the corresponding fence
                    if ((flushPendingUploadRequests) || // flush requests from previous frame
                        (0 == m_pendingResources.size()) || // flush because there's no more work to be done (no stale resources, all feedback has been processed)
                        // if we need updatelists and there is a minimum amount of pending work, go ahead and submit
                        // this minimum heuristic prevents "storms" of submits with too few tiles to sustain good throughput
                        ((0 == m_dataUploader.GetNumUpdateListsAvailable()) && (uploadsRequested > m_minNumUploadRequests)))
                    {
                        SignalFileStreamer();
                        uploadsRequested = 0;
                    }
                }

                // nothing to do? wait for next frame
                // development note: do not Wait() if uploadsRequested != 0. safe because uploadsRequested was cleared above.
                if (0 == m_pendingResources.size())
                {
                    m_processFeedbackTime += m_cpuTimer.GetTime() - prevFrameTime;

                    ASSERT(0 == uploadsRequested);
                    m_processFeedbackFlag.Wait();

                    prevFrameTime = m_cpuTimer.GetTime();
                }
            }

            // if thread exits, flush any pending uploads
            if (uploadsRequested) { SignalFileStreamer(); }
        });
    SFS::SetThreadPriority(m_thread, m_threadPriority);
}

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
void SFS::ProcessFeedbackThread::Stop()
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
void SFS::ProcessFeedbackThread::ShareNewResources(const std::vector<ResourceBase*>& in_resources)
{
    m_newResourcesLock.Acquire();
    m_newResourcesStaging.insert(m_newResourcesStaging.end(), in_resources.begin(), in_resources.end());
    m_newResourcesLock.Release();
}

//-----------------------------------------------------------------------------
// SFSManager acquires staging area and adds resources that have QueueFeedback() called
//-----------------------------------------------------------------------------
void SFS::ProcessFeedbackThread::SharePendingResources(std::vector<ResourceBase*>& in_resources)
{
    m_pendingLock.TryAcquire();
    m_pendingResourceStaging.insert(m_pendingResourceStaging.end(), in_resources.begin(), in_resources.end());
    in_resources.clear();
    m_pendingLock.Release();
}

//-----------------------------------------------------------------------------
// called by SFSManager on the main thread
// shares the list of resources to remove with processfeedback thread
//-----------------------------------------------------------------------------
void SFS::ProcessFeedbackThread::AsyncDestroyResources(std::set<ResourceBase*>& in_resources)
{
    ASSERT(0 != in_resources.size());

    // should not be possible for only the one bit to be set:
    ASSERT(GroupRemoveResources::Client::Residency != m_removeResources.GetFlags());

    // add resources to remove to staging
    m_removeStagingLock.Acquire();
    m_removeResourcesStaging.insert(m_removeResourcesStaging.end(), in_resources.begin(), in_resources.end());
    m_removeStagingLock.Release();
    m_removeResources.SetFlag(GroupRemoveResources::Client::Initialize);

    Wake();
}

//-----------------------------------------------------------------------------
// handle deleting resources asynchronously
// can be deleted if no UpdateLists reference them and the residency thread isn't tracking them
// to avoid race when resources are quickly created then deleted,
//     withhold new resources from the residency thread until packed mips are scheduled
// ultimately, two expensive phases:
//     1) received a new list of resources to remove
//     2) after other threads have removed resources, delete them
//-----------------------------------------------------------------------------
void SFS::ProcessFeedbackThread::CheckRemoveResources()
{
    UINT flags = m_removeResources.GetFlags();
    if (0 == flags)
    {
        ASSERT(0 == m_removeResources.size());
        return;
    }

    if (GroupRemoveResources::Client::Initialize & flags)
    {
        ASSERT(m_removeResourcesStaging.size());

        // move the staged changes local
        std::vector<ResourceBase*> tmp;
        m_removeStagingLock.Acquire();
        m_removeResourcesStaging.swap(tmp);
        m_removeResources.ClearFlag(GroupRemoveResources::Client::Initialize);
        m_removeStagingLock.Release();
        ASSERT(tmp.size());

        // 3 possibilities at this point:
        // flags = 0, flags = pft, flags = pft | res
        // technically init could be set again, meaning more work is in staging, but don't worry about it

        // form what will be the future m_removeResources
        std::set<ResourceBase*> tmpSet = m_removeResources;
        tmpSet.insert(tmp.begin(), tmp.end());

        // remove from my vectors
        ContainerRemove(m_residencyShareNewResources, tmpSet);
        ContainerRemove(m_activeResources, tmpSet);
        ContainerRemove(m_pendingResources, tmpSet);

        // if new resources haven't got packed mips yet, then residency won't know about them
        // therefore, they are safe to delete
        UINT num = (UINT)m_newResources.size();
        for (UINT i = 0; i < num;)
        {
            auto pResource = m_newResources[i];
            // NOTE: this method is called before InitPackedMips
            // there may be previously created resources that have not had packed mips scheduled
            // all those resources are safe to delete, because no other internal threads are using them
            auto t = tmpSet.find(pResource);
            if (t != tmpSet.end())
            {
                ASSERT(!pResource->GetInitialized());
                // if found: delete resource, remove from both new set and new resource
                delete pResource;
                tmpSet.erase(t);
                num--;
                m_newResources[i] = m_newResources[num];
            }
            else
            {
                i++;
            }
        }
        m_newResources.resize(num);

        m_removeResources.Acquire();
        m_removeResources.swap(tmpSet);
        m_removeResources.Release();

        // tell residency thread if there's work to do. wait for it.
        if (m_removeResources.size())
        {
            m_removeResources.SetFlag(GroupRemoveResources::Client::Residency);
            // wake up residency thread (which may already be awake)
            m_pSFSManager->WakeResidencyThread();
            m_removeResources.SetFlag(GroupRemoveResources::Client::ProcessFeedback);
            Wake(); // prevent self from going to sleep right away
        }
        else
        {
            // no other work required to remove resources
            m_removeResources.ClearFlag(GroupRemoveResources::Client::ProcessFeedback);
        }
    }

    // if the residency thread has process removed resources,
    // isolate the remaining resources that have pending work in DataUploader
    // then delete them as they become available
    if (GroupRemoveResources::Client::ProcessFeedback == m_removeResources.GetFlags())
    {
        std::set<ResourceBase*> remaining;
        auto& updateLists = m_dataUploader.GetUpdateLists();
        for (auto& u : updateLists)
        {
            if (UpdateList::State::STATE_FREE != u.m_executionState)
            {
                auto i = m_removeResources.find((ResourceBase*)u.m_pResource);
                if (i != m_removeResources.end())
                {
                    // found a resource with in-flight activity
                    remaining.insert((ResourceBase*)u.m_pResource);
                    m_removeResources.erase(i);
                }
            }
        }

        // m_removeResources is the resources to delete that don't have in-flight work
        for (auto p : m_removeResources)
        {
            delete p;
        }

        // set removeResources to just the in-flight resources
        m_removeResources.swap(remaining);

        // nothing left to do, so this thread can return to normal
        if (0 == m_removeResources.size())
        {
            m_removeResources.ClearFlag(GroupRemoveResources::Client::ProcessFeedback);
        }
        else
        {
            // prevent self from going to sleep until all remove resources have drained
            Wake();
        }
    }
}
