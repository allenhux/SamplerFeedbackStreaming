#include "pch.h"

#include "ProcessFeedbackThread.h"
#include "SFSManagerBase.h"
#include "SFSResourceBase.h"

//=============================================================================
// severely limited SFSManager interface
//=============================================================================
namespace SFS
{
    class ManagerPFT : private ManagerBase
    {
    public:
        UINT64 GetFrameFenceCompletedValue() { return m_frameFence->GetCompletedValue(); }
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
void SFS::ProcessFeedbackThread::SignalFileStreamer()
{
    m_dataUploader.SignalFileStreamer();
    m_numTotalSubmits.fetch_add(1, std::memory_order_relaxed);
}

//-----------------------------------------------------------------------------
// per frame, call SFSResource::ProcessFeedback()
// expects the no change in # of streaming resources during thread lifetime
//-----------------------------------------------------------------------------
void SFS::ProcessFeedbackThread::Start()
{
    ASSERT(false == m_threadRunning);

    m_threadRunning = true;
    m_thread = std::thread([&]
        {
            UINT uploadsRequested = 0; // remember if any work was queued so we can signal afterwards
            UINT64 previousFrameFenceValue = m_pSFSManager->GetFrameFenceCompletedValue();

            while (m_threadRunning)
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

                // FIXME: TODO while packed mips are loading, process evictions on stale resources

                // prioritize loading packed mips, as objects shouldn't be displayed until packed mips load
                if (m_newResources.size())
                {
                    for (auto i = m_newResources.begin(); i != m_newResources.end();)
                    {
                        // must call on every resource that needs to load packed mips
                        if ((*i)->InitPackedMips())
                        {
                            i = m_newResources.erase(i);
                        }
                        else
                        {
                            i++;
                        }
                    }
                }

                // check for existing resources that have feedback
                if (m_pendingResourceStaging.size() && m_pendingLock.TryAcquire())
                {
                    // grab them and release the lock quickly
                    std::vector<ResourceBase*> tmpResources;
                    tmpResources.swap(m_pendingResourceStaging);
                    m_pendingLock.Release();

                    m_activeResources.insert(tmpResources.begin(), tmpResources.end());
                }

                if (m_newResources.size())
                {
                    continue; // still working on loading packed mips. don't move on to other streaming tasks yet.
                }

                bool flushPendingUploadRequests = false;

                // Once per frame: process feedback buffers
                {
                    UINT64 frameFenceValue = m_pSFSManager->GetFrameFenceCompletedValue();
                    if (previousFrameFenceValue != frameFenceValue)
                    {
                        previousFrameFenceValue = frameFenceValue;

                        // start timer for this frame
                        INT64 perFrameTime = m_cpuTimer.GetTime();

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
                        // add the amount of time we just spent processing feedback for a single frame
                        m_processFeedbackTime += UINT64(m_cpuTimer.GetTime() - perFrameTime);
                    }
                }

                // push uploads and evictions for stale resources
                if (m_pendingResources.size())
                {
                    INT64 pendingStartTime = m_cpuTimer.GetTime();

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

                    m_processFeedbackTime += UINT64(m_cpuTimer.GetTime() - pendingStartTime);
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
                if (0 == m_activeResources.size())
                {                    
                    ASSERT(0 == uploadsRequested);
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
void SFS::ProcessFeedbackThread::ShareNewResources(const std::vector<ResourceBase*>& in_resources)
{
    m_newResourcesLock.Acquire();
    m_newResourcesStaging.insert(m_newResourcesStaging.end(), in_resources.begin(), in_resources.end());
    m_newResourcesLock.Release();
}

//-----------------------------------------------------------------------------
// SFSManager acquires staging area and adds new resources
//-----------------------------------------------------------------------------
void SFS::ProcessFeedbackThread::SharePendingResources(const std::vector<ResourceBase*>& in_resources)
{
    m_pendingLock.Acquire();
    m_pendingResourceStaging.insert(m_pendingResourceStaging.end(), in_resources.begin(), in_resources.end());
    m_pendingLock.Release();
}


//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
void SFS::ProcessFeedbackThread::RemoveResources(const std::set<ResourceBase*>& in_resources)
{
    Stop();
    for (auto i = m_newResources.begin(); i != m_newResources.end();)
    {
        if (in_resources.contains(*i))
        {
            i = m_newResources.erase(i);
        }
        else
        {
            i++;
        }
    }
    for (auto r : in_resources)
    {
        if (m_activeResources.contains(r))
        {
            m_activeResources.erase(r);
        }
        if (m_pendingResources.contains(r))
        {
            m_activeResources.erase(r);
        }
    }
    Start();
}
