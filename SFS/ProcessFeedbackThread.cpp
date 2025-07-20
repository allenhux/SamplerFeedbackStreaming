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

        void SubmitUpdateList(SFS::UpdateList& in_updateList)
        {
            m_numTotalUploads.fetch_add((UINT)in_updateList.m_coords.size(), std::memory_order_relaxed);
            m_numTotalEvictions.fetch_add((UINT)in_updateList.m_evictCoords.size(), std::memory_order_relaxed);
            m_numTotalSubmits.fetch_add(1, std::memory_order_relaxed);

            m_dataUploader.SubmitUpdateList(in_updateList);
        }

        void CheckFlushResources()
        {
            m_dataUploader.CheckFlushResources();
		}
    };
}

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
SFS::ProcessFeedbackThread::ProcessFeedbackThread(ManagerPFT* in_pSFSManager,
    DataUploader& in_dataUploader, int in_threadPriority) :
    m_pSFSManager(in_pSFSManager)
    , m_dataUploader(in_dataUploader)
    , m_threadPriority(in_threadPriority)
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
    m_numTotalSignals.fetch_add(1, std::memory_order_relaxed);
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
            INT64 prevFrameTime = m_cpuTimer.GetTicks();

            // limit the number of signals per frame to prevent "storms"
            constexpr UINT signalCounterMax = 16;
            UINT signalCounter = 0;

            // limit the number of DS Enqueues (==tiles) between signals
            constexpr UINT uploadsRequestedMax = 128;

            while (m_threadRunning)
            {
                bool newFrame = false;
                UINT64 frameFenceValue = m_pSFSManager->GetFrameFenceCompletedValue();

                // only look for new or to-delete resources once per frame
                if (previousFrameFenceValue != frameFenceValue)
                {
                    previousFrameFenceValue = frameFenceValue;
                    newFrame = true;

                    // check for new resources, which probably need to load pack mips
                    if (m_newResourcesStaging.Size())
                    {
                        // grab them and release the lock quickly
                        std::vector<ResourceBase*> tmpResources;
                        m_newResourcesStaging.Swap(tmpResources);
                        // accumulate because maybe hasn't been emptied from last time
                        m_newResources.insert(m_newResources.end(), tmpResources.begin(), tmpResources.end());
                    }

                    // check for resources where the application has called QueueFeedback() or QueueEviction()
                    if (m_pendingResourceStaging.Size())
                    {
                        // grab them and release the lock quickly
                        std::set<ResourceBase*> tmpResources;
                        m_pendingResourceStaging.Swap(tmpResources);
                        // accumulate because maybe hasn't been emptied from last time
                        m_delayedResources.insert(m_delayedResources.end(), tmpResources.begin(), tmpResources.end());
                    }

                    // compare active resources to resources that are to be removed
                    // NOTE: doing this before InitPackedMips and sharing with ResidencyThread
                    //       as m_newResources may contain resources that have not been
                    //       propagated to residency thread
                    CheckFlushResources();

                    signalCounter = 0;
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
                } // end loop over new resources

                if (newFrame)
                {
                    // Once per frame: process feedback buffers
                    std::set<ResourceBase*> residencyChanged;

                    prevFrameTime = m_cpuTimer.GetTicks();
                    for (auto i = m_delayedResources.begin(); i != m_delayedResources.end();)
                    {
                        auto pResource = *i;

                        // fairly frequent, in practice, for frame to change while processing feedback
                        if (pResource->ProcessFeedback(m_pSFSManager->GetFrameFenceCompletedValue()))
                        {
                            residencyChanged.insert(pResource);
                        }

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

                    m_dataUploader.AddResidencyChanged(residencyChanged);

                    m_processFeedbackTime += (m_cpuTimer.GetTicks() - prevFrameTime);
                } // end if new frame

                // push uploads and evictions for stale resources
                if (m_pendingResources.size())
                {
                    UINT sumEvictions = 0;
					UINT sumUploads = 0;
                    for (auto i = m_pendingResources.begin(); i != m_pendingResources.end();)
                    {
                        ResourceBase* pResource = *i;

                        if (m_dataUploader.GetNumUpdateListsAvailable() && m_threadRunning) // don't add work while exiting
                        {
                            UpdateList* pUpdateList = nullptr;

                            // tiles that are "loading" can't be evicted. as soon as they arrive, they can be.
                            sumEvictions += pResource->QueuePendingTileEvictions(pUpdateList);

                            UINT numUploads = 0;
                            if (0 == m_newResources.size()) // load packed mips before loading other tiles
                            {
                                numUploads = pResource->QueuePendingTileLoads(pUpdateList);
                            }

                            if (pUpdateList)
                            {
                                m_pSFSManager->SubmitUpdateList(*pUpdateList);
                            }

                            if (numUploads)
                            {
                                sumUploads += numUploads; // statistics
                                uploadsRequested += numUploads; // submit heuristics
                                // Limit the # of DS Enqueues (== # tiles) between signals
                                if ((uploadsRequested > uploadsRequestedMax) && (signalCounter < signalCounterMax))
                                {
                                    SignalFileStreamer();
                                    uploadsRequested = 0;
                                    signalCounter++; // prevents "storms" of submits
                                }
                            }
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
                } // end loop over pending resources

                // catch remainder uploads before next frame
                if (uploadsRequested && (signalCounter < signalCounterMax))
                {
                    SignalFileStreamer();
                    uploadsRequested = 0;
                    signalCounter++; // prevents "storms" of submits
                }

                // nothing to do? wait for next frame
                // development note: ok to Wait() if uploadsRequested != 0. signalCounter will be reset next frame, and those updates will get a signal
                if (0 == m_pendingResources.size())
                {
                    m_processFeedbackFlag.Wait();
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
void SFS::ProcessFeedbackThread::ShareNewResources(std::vector<ResourceBase*> in_resources)
{
    auto& v = m_newResourcesStaging.Acquire();
    if (0 == v.size())
    {
        v.swap(in_resources);
    }
    else
    {
        v.insert(v.end(), in_resources.begin(), in_resources.end());
    }
    m_newResourcesStaging.Release();
}

//-----------------------------------------------------------------------------
// SFSManager acquires staging area and adds resources that have QueueFeedback() called
//-----------------------------------------------------------------------------
void SFS::ProcessFeedbackThread::SharePendingResources(std::set<ResourceBase*> in_resources)
{
    // ideally, pending resources from SFSManager gets passed by value directly to ProcessFeedbackThread
    // otherwise, accumulate with prior resources
    auto& v = m_pendingResourceStaging.Acquire();
    if (0 == v.size())
    {
        v.swap(in_resources);
    }
    else
    {
        v.insert(in_resources.begin(), in_resources.end());
    }
    m_pendingResourceStaging.Release();
}

//-----------------------------------------------------------------------------
// shares the list of resources to flush with processfeedback thread
// called by SFSManager on the main thread
// in_resources is owned by the application
// WARNING: blocks if prior call to FlushResources() hasn't signaled
//-----------------------------------------------------------------------------
void SFS::ProcessFeedbackThread::ShareFlushResources(const std::vector<SFSResource*>& in_resources, HANDLE in_event)
{
    // do not release until flush is complete
    auto& resources = m_flushResources.Acquire();
    ASSERT(0 == resources.size());
    ASSERT(nullptr == m_flushResourcesEvent);

    // should not ever be possible for only the one bit to be set:
    ASSERT(GroupFlushResources::Flags::ResidencyThread != m_flushResources.GetFlags());

    // add resources to flush to staging
    // note size attribute is not updated
    for (auto r : in_resources)
    {
        resources.insert((ResourceBase*)r);
    }

    m_flushResourcesEvent = in_event;
    m_flushResources.SetFlag(GroupFlushResources::Flags::Initialize);
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
        ASSERT(0 == (f & GroupFlushResources::Flags::Initialize));
    case 0:
        return;

    case GroupFlushResources::Flags::Initialize:
    {
        auto& flushResources = m_flushResources.BypassLockGetValues();

        ASSERT(flushResources.size());

        m_flushResources.ClearFlag(GroupFlushResources::Flags::Initialize);

        // remove from my vectors
        ContainerRemove(m_delayedResources, flushResources);
        ContainerRemove(m_pendingResources, flushResources);

        // tell residency thread there's work to do
        m_flushResources.SetFlag(GroupFlushResources::Flags::ResidencyThread);

        // wake up residency thread (which may already be awake)
        m_pSFSManager->CheckFlushResources();

        // wait for residency thread to verify flushed
        m_flushResources.SetFlag(GroupFlushResources::Flags::ProcessFeedbackThread);
        Wake(); // prevent self from going to sleep right away
    }
    break;

    case GroupFlushResources::Flags::ProcessFeedbackThread:
    {
        // after the residency thread has flushed resources,
        // identify any resources that have pending work in DataUploader
        // when done, set the event and release the lock

        auto& flushResources = m_flushResources.BypassLockGetValues();
        std::set<ResourceBase*> remaining;

        // wait for new resources to migrate to pending
        for (auto r : m_newResources)
        {
            if (flushResources.contains(r))
            {
                remaining.insert(r);
            }
        }

        // wait for UpdateLists to complete (notify)
        for (auto& u : m_dataUploader.GetUpdateLists())
        {
            if ((UpdateList::State::STATE_FREE != u.m_executionState) &&
                (flushResources.contains((ResourceBase*)u.m_pResource)))
            {
                remaining.insert((ResourceBase*)u.m_pResource);
            }
        }

        // eject all heap indices
        for (auto r : flushResources)
        {
            if (!remaining.contains(r))
            {
                r->Reset();
            }
        }

        // set removeResources to just the in-flight resources
        flushResources.swap(remaining);

        // resources have been flushed. clear flags and signal event.
        if (0 == flushResources.size())
        {
            m_flushResources.ClearFlag(GroupFlushResources::Flags::ProcessFeedbackThread);
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
