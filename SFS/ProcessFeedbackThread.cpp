//==============================================================
// Copyright � Intel Corporation
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

        void SharePendingResourcesRT(const std::set<ResourceBase*>& in_resources)
        {
            m_residencyThread.SharePendingResourcesRT(in_resources);
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
    for (auto p : m_flushResources.Acquire())
    {
        delete p;
    }
    m_flushResources.Release();
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
                bool flushPendingUploadRequests = false;

                bool newFrame = false;
                UINT64 frameFenceValue = m_pSFSManager->GetFrameFenceCompletedValue();

                // only look for new or to-delete resources once per frame
                if (previousFrameFenceValue != frameFenceValue)
                {
                    previousFrameFenceValue = frameFenceValue;
                    newFrame = true;

                    // flush any pending uploads from previous frame
                    if (uploadsRequested) { flushPendingUploadRequests = true; }

                    // check for new resources, which probably need to load pack mips
                    if (m_newResourcesStaging.size())
                    {
                        // grab them and release the lock quickly
                        std::vector<ResourceBase*> tmpResources;
                        m_newResourcesStaging.swap(tmpResources);
                        // accumulate because maybe hasn't been emptied from last time
                        m_newResources.insert(m_newResources.end(), tmpResources.begin(), tmpResources.end());
                    }

                    // check for resources where the application has called QueueFeedback() or QueueEviction()
                    if (m_pendingResourceStaging.size())
                    {
                        // grab them and release the lock quickly
                        std::set<ResourceBase*> tmpResources;
                        m_pendingResourceStaging.swap(tmpResources);
                        // accumulate because maybe hasn't been emptied from last time
                        m_delayedResources.insert(m_delayedResources.end(), tmpResources.begin(), tmpResources.end());
                    }

                    // compare active resources to resources that are to be removed
                    // NOTE: doing this before InitPackedMips and sharing with ResidencyThread
                    //       as m_newResources may contain resources that have not been
                    //       propagated to residency thread
                    CheckFlushResources();
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

                // Once per frame: process feedback buffers
                if (newFrame)
                {
                    for (auto i = m_delayedResources.begin(); i != m_delayedResources.end();)
                    {
                        auto pResource = *i;

                        // fairly frequent, in practice, for frame to change while processing feedback
                        pResource->ProcessFeedback(m_pSFSManager->GetFrameFenceCompletedValue());

                        // resource may now have loads to process asap and evictions scheduled for now or the future

                        if (pResource->HasPendingWork())
                        {
                            m_pendingResources.insert(pResource);
                        }

                        if (pResource->HasDelayedWork())
                        {
                            i++;
                        }
                        else
                        {
                            i = m_delayedResources.erase(i);
                        }
                    } // end loop over active resources

                    // share updated pending resource set with ResidencyThread
                    m_pSFSManager->SharePendingResourcesRT(m_pendingResources);
                } // end if new frame

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
                            && (0 == m_newResources.size()) // upload packed mips first
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

                        if (pResource->HasPendingWork()) // still have work to do?
                        {
                            i++;
                        }
                        else
                        {
                            // NOTE: resource removed from pending, but may have updatelists depending on it or
                            //       have a change in residency state. Residency thread will hold on to these
                            //       resources, which were shared with it above, but may not be shared next frame

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
    auto& v = m_newResourcesStaging.Acquire();
    v.insert(v.end(), in_resources.begin(), in_resources.end());
    m_newResourcesStaging.Release();
}

//-----------------------------------------------------------------------------
// SFSManager acquires staging area and adds resources that have QueueFeedback() called
//-----------------------------------------------------------------------------
void SFS::ProcessFeedbackThread::SharePendingResources(const std::set<ResourceBase*>& in_resources)
{
    auto& v = m_pendingResourceStaging.Acquire();
    v.insert(in_resources.begin(), in_resources.end());
    m_pendingResourceStaging.Release();
}

//-----------------------------------------------------------------------------
// called by SFSManager on the main thread
// shares the list of resources to flush with processfeedback thread
//-----------------------------------------------------------------------------
void SFS::ProcessFeedbackThread::ShareFlushResources(const std::vector<SFSResource*>& in_resources, HANDLE in_event)
{
    // should not ever be possible for only the one bit to be set:
    ASSERT(GroupRemoveResources::Client::ResidencyThread != m_flushResources.GetFlags());

    // add resources to flush to staging
    // WARNING: will block if another flush is in progress
    std::set<ResourceBase*> v;
    for (auto r : in_resources)
    {
        v.insert((ResourceBase*)r);
    }
    m_flushResources.swap(v); // updates size attribute
    ASSERT(0 == v.size());

    // do not release until flush is complete
    m_flushResources.Acquire();

    ASSERT(nullptr == m_flushResourcesEvent);

    m_flushResourcesEvent = in_event;
    m_flushResources.SetFlag(GroupRemoveResources::Client::Initialize);
}

//-----------------------------------------------------------------------------
// CheckFlushResources stops further processing on a set of resources and waits
// for DataUploader and Residency thread to finish before signaling an event
//
// this is part of the system to handle deleting resources asynchronously
//-----------------------------------------------------------------------------
void SFS::ProcessFeedbackThread::CheckFlushResources()
{
    UINT32 f = m_flushResources.GetFlags();

    switch (f)
    {
    default:
        // should not see initialize combined with another bit
        ASSERT(0 == (f & GroupRemoveResources::Client::Initialize));
    case 0:
        // race: possible size changes after reading flags! ASSERT(0 == m_flushResources.size());
        return;

    case GroupRemoveResources::Client::Initialize:
        ASSERT(m_flushResources.size());

        m_flushResources.ClearFlag(GroupRemoveResources::Client::Initialize);

        // remove from my vectors
        ContainerRemove(m_delayedResources, m_flushResources.BypassLockGetValues());
        ContainerRemove(m_pendingResources, m_flushResources.BypassLockGetValues());

        // tell residency thread there's work to do
        m_flushResources.SetFlag(GroupRemoveResources::Client::ResidencyThread);
        // wake up residency thread (which may already be awake)
        m_pSFSManager->WakeResidencyThread();

        // wait for residency thread to verify flushed
        m_flushResources.SetFlag(GroupRemoveResources::Client::ProcessFeedbackThread);
        Wake(); // prevent self from going to sleep right away

        break;

    case GroupRemoveResources::Client::ProcessFeedbackThread:
    {
        // after the residency thread has flushed resources,
        // identify any resources that have pending work in DataUploader
        // when done, set the event and release the lock

        std::set<ResourceBase*> remaining;

        // wait for new resources to migrate to pending
        for (auto r : m_newResources)
        {
            if (m_flushResources.BypassLockGetValues().contains(r))
            {
                remaining.insert(r);
            }
        }

        // wait for UpdateLists to complete (notify)
        for (auto& u : m_dataUploader.GetUpdateLists())
        {
            if ((UpdateList::State::STATE_FREE != u.m_executionState) &&
                (m_flushResources.BypassLockGetValues().contains((ResourceBase*)u.m_pResource)))
            {
                remaining.insert((ResourceBase*)u.m_pResource);
            }
        }

        // eject all heap indices
        for (auto r : m_flushResources.BypassLockGetValues())
        {
            if (!remaining.contains(r))
            {
                r->Reset();
            }
        }

        // set removeResources to just the in-flight resources
        m_flushResources.BypassLockGetValues().swap(remaining);

        // resources have been flushed. clear flags and signal event.
        if (0 == remaining.size())
        {
            m_flushResources.ClearFlag(GroupRemoveResources::Client::ProcessFeedbackThread);
            ASSERT(m_flushResourcesEvent);
            ::SetEvent(m_flushResourcesEvent);
            m_flushResourcesEvent = nullptr;

            m_flushResources.Release();
        }
        else
        {
            // prevent self from going to sleep until all remove resources have drained
            Wake();
        }
    }
    break;

    } // end switch
}
