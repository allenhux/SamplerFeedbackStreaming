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
{}

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

            // limit the number of signals per frame to prevent "storms"
            constexpr UINT signalCounterMax = 8;
            UINT signalCounter = 0;

            // limit the number of DS Enqueues (==tiles) between signals
            constexpr UINT uploadsRequestedMax = 192;

            while (m_threadRunning)
            {
                UINT64 frameFenceValue = m_pSFSManager->GetFrameFenceCompletedValue();

                // only look for new or to-delete resources once per frame
                if (previousFrameFenceValue != frameFenceValue)
                {
                    previousFrameFenceValue = frameFenceValue;

                    auto feedbackTime = m_cpuTimer.GetTicks();
                    signalCounter = 0;

                    // flush uploads from last frame
                    if (uploadsRequested)
                    {
                        SignalFileStreamer();
                        uploadsRequested = 0;
                        signalCounter++; // prevents "storms" of submits
                    }

                    // check for new resources, which probably need to load pack mips
                    if (m_newResourcesStaging.Size())
                    {
                        // grab them and release the lock quickly
                        std::vector<ResourceBase*> tmpResources;
                        m_newResourcesStaging.Swap(tmpResources);

                        if (m_newResources.empty())
                        {
                            m_newResources.swap(tmpResources);
                        }
                        else // accumulate when non-empty
                        {
                            m_newResources.insert(m_newResources.end(), tmpResources.begin(), tmpResources.end());
                        }                        
                    }

                    // compare active resources to resources that are to be removed
                    // NOTE: doing this before InitPackedMips and sharing with ResidencyThread
                    //       as m_newResources may contain resources that have not been
                    //       propagated to residency thread
                    CheckFlushResources();

                    // check for resources where the application has called QueueFeedback() or QueueEviction()
                    if (m_queuedResourceStaging.Size())
                    {
                        // grab them and release the lock quickly
                        if (0 == m_delayedResources.size())
                        {
                            m_queuedResourceStaging.Swap(m_delayedResources);
                        }
                        else
                        {
                            std::set<ResourceBase*> tmpResources;
                            m_queuedResourceStaging.Swap(tmpResources);
                            m_delayedResources.insert(tmpResources.begin(), tmpResources.end());
                        }
                    }

                    if (m_delayedResources.size())
                    {
                        // any evicted tile immediately removed from residency map asap
                        // FIXME: would like to not change residency until later
                        std::set<ResourceBase*> residencyChanged;

                        for (auto i = m_delayedResources.begin(); i != m_delayedResources.end();)
                        {
                            auto pResource = *i;
                            bool hasFutureFeedback = false;
                            bool changed = pResource->ProcessFeedback(frameFenceValue, hasFutureFeedback);
                            if (changed)
                            {
                                residencyChanged.insert(pResource);
                            }
                            if (pResource->HasPendingWork(frameFenceValue))
                            {
                                m_pendingResources.insert(pResource);
                            }
                            if (hasFutureFeedback || pResource->HasDelayedWork())
                            {
                                i++; // check this resource again later
                            }
                            else // no future work for this resource
                            {
                                i = m_delayedResources.erase(i);
                            }
                        }
                        if (residencyChanged.size())
                        {
                            m_dataUploader.AddResidencyChanged(std::move(residencyChanged));
                        }
                    } // end if queued or delayed work

                    m_processFeedbackTime += (m_cpuTimer.GetTicks() - feedbackTime);
                } // end new frame

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

                // push uploads and evictions for stale resources
                if (m_pendingResources.size())
                {
                    auto processingTime = m_cpuTimer.GetTicks();

                    for (auto i = m_pendingResources.begin(); i != m_pendingResources.end();)
                    {
                        if (!(m_dataUploader.GetNumUpdateListsAvailable() && m_threadRunning))
                        {
                            break;
                        }

                        ResourceBase* pResource = *i;

                        UpdateList* pUpdateList = nullptr;

                        // tiles that are "loading" can't be evicted until loads complete
                        pResource->QueuePendingTileEvictions(frameFenceValue, pUpdateList);

                        uploadsRequested += pResource->QueuePendingTileLoads(pUpdateList);

                        if (pUpdateList)
                        {
                            m_pSFSManager->SubmitUpdateList(*pUpdateList);
                        }

                        // Limit the # of DS Enqueues (== # tiles) between signals
                        if ((uploadsRequested > uploadsRequestedMax) && (signalCounter < signalCounterMax))
                        {
                            SignalFileStreamer();
                            uploadsRequested = 0;
                            signalCounter++; // prevents "storms" of submits
                        }

                        if (pResource->HasPendingWork(frameFenceValue)) // still have work to do?
                        {
                            i++;
                        }
                        else
                        {
                            // NOTE: resource removed from pending, but may have updatelists in flight
                            i = m_pendingResources.erase(i);
                        }
                    } // end loop over pending resources
                    m_processFeedbackTime += (m_cpuTimer.GetTicks() - processingTime);
                } // end if pending resources

                // nothing to do? wait for next frame
                // also, can't do work without updatelists. might get one before end of frame, though
                if ((0 == m_pendingResources.size()) || (0 == m_dataUploader.GetNumUpdateListsAvailable()))
                {
                    // don't have to leave uploads lingering before sleeping
                    if (uploadsRequested)
                    {
                        SignalFileStreamer();
                        uploadsRequested = 0;
                        signalCounter++; // prevents "storms" of submits
                    }

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
    auto& v = m_queuedResourceStaging.Acquire();
    if (0 == v.size())
    {
        v.swap(in_resources);
    }
    else
    {
        v.insert(in_resources.begin(), in_resources.end());
    }
    m_queuedResourceStaging.Release();
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
        ContainerRemove(m_queuedResourceStaging.Acquire(), flushResources); m_queuedResourceStaging.Release();

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
