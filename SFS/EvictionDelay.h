#pragma once

#include <vector>
#include <list>
#include <d3d12.h>
#include "DebugHelper.h"

//=============================================================================
// class used to delay evictions by a number of frames (must be >= swapchain count)
//=============================================================================

namespace SFS
{
    // supports 8k x 8k /tiles/ and 64 mip levels.
    struct Coord
    {
        UINT x : 13;
        UINT y : 13;
        UINT s : 6;
        // std::set<> requires operator<
        constexpr bool operator< (const Coord& o) const { return (UINT&)(*this) < (UINT&)o; }
    };
    using Coords = std::vector<Coord>;

    // delay heap free and unmap (if enabled) for n frames (at least swapchain count)
    class EvictionDelay
    {
    public:
        EvictionDelay(UINT in_delay) : m_delay(in_delay) {}

        void Append(UINT64 in_fenceValue, Coords& in_coords)
        {
            m_futureEvictions.emplace_back(in_fenceValue + m_delay).swap(in_coords);
        }

        // any future evictions?
        bool HasDelayedWork() const
        {
            return m_futureEvictions.size();
        }

        // has work that can be done right now?
        bool HasPendingWork(UINT64 in_fenceValue) const
        {
            return HasDelayedWork() && (m_futureEvictions.front().m_fenceValue <= in_fenceValue);
        }

        // return evictions to process
        auto begin() { return m_futureEvictions.begin(); }
        auto end() { return m_futureEvictions.end(); }
        void Pop() { m_futureEvictions.pop_front(); }
		auto erase(auto i) { return m_futureEvictions.erase(i); }

        // dump all pending evictions
        void Clear() { m_futureEvictions.clear(); }
    private:
        struct Evictions : public Coords
        {
            Evictions(UINT64 in_fenceValue) : m_fenceValue(in_fenceValue) {}
            const UINT64 m_fenceValue{ 0 };
        };
        std::list<Evictions> m_futureEvictions;
        const UINT m_delay;
    };
};
