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
    class ResourceBase;

    // delay heap free and unmap (if enabled) for n frames (at least swapchain count)
    class EvictionDelay
    {
    public:
        struct Coord
        {
            UINT x : 13;
            UINT y : 13;
            UINT s : 6;
            operator D3D12_TILED_RESOURCE_COORDINATE() const { return {x, y, 0, s}; }
        };
        using Coords = std::vector<Coord>;

        EvictionDelay(UINT in_delay) : m_delay(in_delay) {}

        void Append(UINT64 in_fenceValue, Coords& in_coords)
        {
            m_futureEvictions.emplace_back(in_fenceValue + m_delay);
            m_futureEvictions.back().swap(in_coords);
        }

        // are there delayed evictions ready to process?
        bool HasPendingWork(const UINT64 in_fenceValue) const { return m_futureEvictions.size() && m_futureEvictions.front().m_fenceValue <= in_fenceValue; }

        // any future evictions?
        bool HasDelayedWork() const { return !m_futureEvictions.empty(); }

        // return evictions to process
        auto begin() { return m_futureEvictions.begin(); }
        auto end() { return m_futureEvictions.end(); }
        void Pop() { m_futureEvictions.pop_front(); }

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
