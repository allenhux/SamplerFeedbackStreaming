#include "pch.h"
#include "EvictionDelay.h"
#include "ResourceBase.h"

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
SFS::EvictionDelay::EvictionDelay(UINT in_numSwapBuffers)
{
    m_mappings.resize(in_numSwapBuffers);
}

//-----------------------------------------------------------------------------
// step pending evictions once per frame
//-----------------------------------------------------------------------------
void SFS::EvictionDelay::NextFrame()
{
    // move next-to-last vector to front of the list
    m_mappings.splice(m_mappings.begin(), m_mappings, --m_mappings.end());
    // append elements of first vector to end of last vector
    m_mappings.back().insert(m_mappings.back().end(), m_mappings.front().begin(), m_mappings.front().end());
    // clear first vector
    m_mappings.front().clear();
}

//-----------------------------------------------------------------------------
// dump all pending evictions
//-----------------------------------------------------------------------------
void SFS::EvictionDelay::Clear()
{
    for (auto& i : m_mappings)
    {
        i.clear();
    }
}

//-----------------------------------------------------------------------------
// drop pending evictions for tiles that now have non-zero refcount
//-----------------------------------------------------------------------------
bool SFS::EvictionDelay::Rescue(const SFS::ResourceBase* in_pResource)
{
    bool rescued = false;

    // note: it is possible even for the most recent evictions to have refcount > 0
    // because a tile can be evicted then loaded again within a single ProcessFeedback() call
    for (auto& evictions : m_mappings)
    {
        UINT numPending = (UINT)evictions.size();
        if (numPending)
        {
            for (UINT i = 0; i < numPending;)
            {
                auto& c = evictions[i];
                // on rescue, swap a later tile in and re-try the check
                // this re-orders the queue, but we can tolerate that
                // because the residency map is built bottom-up
                if (in_pResource->GetRefCount(c.x, c.y, c.s))
                {
                    numPending--;
                    c = evictions[numPending];
                }
                else // refcount still 0, this tile may still be evicted
                {
                    i++;
                }
            }
            if (numPending != evictions.size()) { rescued = true; }
            evictions.resize(numPending);
        }
    }

    return rescued;
}
