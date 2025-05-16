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
        void BroadcastRemoveResources(GroupRemoveResources* pR)
        {
            m_residencyThread.WakeRemoveResources(pR);
            m_dataUploader.WakeRemoveResources(pR);
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

                CheckRemoveResourcesPFT();

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
                    UINT64 frameFenceValue = m_pSFSManager->GetFrameFenceCompletedValue();
                    if (previousFrameFenceValue != frameFenceValue)
                    {
                        previousFrameFenceValue = frameFenceValue;
                        SignalFileStreamer();
                    }

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
// called by SFSManager on the main thread
// swaps contents of in_resources into internal object
// state of object is Process Feedback thread | initialize state, then messages thread
// initialization state sets state to all clients, then messages other threads
// NOTE! other threads retain a pointer to the object, so they will act as soon as the client flags are set
// each thread does work and clears its flag
// process feedback thread will act again when it is the only flag left, deleting resources
//-----------------------------------------------------------------------------
void SFS::ProcessFeedbackThread::AsyncDestroyResources(std::set<ResourceBase*>& in_resources)
{
    ASSERT(0 != in_resources.size());

    // NOTE! prevents adding any more resources to remove until current ones removed
    if (m_removeResourcesLock.TryAcquire())
    {
        // it is possible SFSManager stages a new resource, but is told to delete it
        // before ProcessFeedbackThread has a change to unstage it - because it's a TryAcquire operation
        // remove those resources and let them be deleted later
        m_newResourcesLock.Acquire();
        ContainerRemove(m_newResourcesStaging, in_resources);
        m_newResourcesLock.Release();

        // likewise, resources may have been staged/queued for feedback
        // don't actually want to do that, so unstage and delete later
        m_pendingLock.Acquire();
        ContainerRemove(m_pendingResourceStaging, in_resources);
        m_pendingLock.Release();

        if (0 == m_removeResources.size())
        {
            m_removeResources.swap(in_resources);
        }
        else
        {
            // main thread managed to delete multiple sets of resources
            // before ProcessFeedbackThread could wake up and start deleting
            m_removeResources.insert(in_resources.begin(), in_resources.end());
            in_resources.clear();
        }
        m_removeResourcesLock.Release();

        m_removeResources.SetFlags(GroupRemoveResources::Client::ProcessFeedback | GroupRemoveResources::Client::Initialize);
        Wake();
    }
}

void SFS::ProcessFeedbackThread::CheckRemoveResourcesPFT()
{
    UINT flags = m_removeResources.GetFlags();
    if (0 == (GroupRemoveResources::Client::ProcessFeedback & flags))
    {
        return;
    }

    if (GroupRemoveResources::Client::ProcessFeedback == flags) // if the other threads have done their thing..
    {
        // delete the resources
        // because ProcessFeedbackThread allocates from tile heap(s) (Atlases),
        // it must be this thread that releases tiles back into the heap
        // the streaming resource destructor releases all tile indices, including packed mip tiles
        for (auto p : m_removeResources)
        {
            delete(p);
        }
        m_removeResources.clear();
        m_removeResources.ClearFlag(GroupRemoveResources::Client::ProcessFeedback);
        m_removeResourcesLock.Release();
        return;
    }

    if (GroupRemoveResources::Client::Initialize & flags)
    {
        // hold this lock until resources have been deleted
        m_removeResourcesLock.Acquire();

        // remove from my vectors
        // do not delete any resources yet; deletion will happen once all clients have removed
        ContainerRemove(m_newResources, m_removeResources);
        ContainerRemove(m_activeResources, m_removeResources);
        ContainerRemove(m_pendingResources, m_removeResources);

        m_removeResources.SetFlags(GroupRemoveResources::Client::AllClients);

        // send a message to the other threads
        m_pSFSManager->BroadcastRemoveResources(&m_removeResources);
    }

    // don't sleep until this is clear
    Wake();
}

SFS::ProcessFeedbackThread::~ProcessFeedbackThread()
{
    for (auto p : m_removeResources)
    {
        delete p;
    }
}
