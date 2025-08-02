#include "pch.h"
#include "EvictionDelay.h"
#include "ResourceBase.h"

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
SFS::EvictionDelay::EvictionDelay(UINT in_delay) : m_delay(in_delay)
{
}

//-----------------------------------------------------------------------------
// drop pending evictions for tiles that now have non-zero refcount
//-----------------------------------------------------------------------------
bool SFS::EvictionDelay::Rescue(const SFS::ResourceBase* in_pResource)
{
    bool rescued = false;

    // note: it is possible even for the most recent evictions to have refcount > 0
    // because a tile can be evicted then loaded again within a single ProcessFeedback() call
    for (auto fi = m_futureEvictions.begin(); fi != m_futureEvictions.end();)
    {
        auto& evictions = *fi;
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
            if (numPending)
            {
                evictions.resize(numPending);
                fi++;
            }
            else
            {
                fi = m_futureEvictions.erase(fi);
            }
        }
    }

    return rescued;
}

//-----------------------------------------------------------------------------
// return a pointer to the first set of coords if it satisfies the given fence
// coalesce if more than one set qualifies
//-----------------------------------------------------------------------------
SFS::EvictionDelay::Coords* SFS::EvictionDelay::GetReadyToEvict(const UINT64 in_fence)
{
    Coords* pCoords = nullptr;
    for (auto i = m_futureEvictions.begin(); i != m_futureEvictions.end();)
    {
        if (i->empty())
        {
            i = m_futureEvictions.erase(i);
            continue;
        }
        if (in_fence >= i->m_fenceValue)
        {
            auto& v = *i;
            if (nullptr == pCoords) { pCoords = &v; i++; }
            else
            {
                pCoords->insert(pCoords->end(), v.begin(), v.end());
                i = m_futureEvictions.erase(i);
            }
        }
        else
        {
            break;
        }
    }

    return pCoords;
}
