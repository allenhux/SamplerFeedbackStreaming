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

    // do not immediately decmap:
    // need to withhold until in-flight command buffers have completed
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

        EvictionDelay(UINT in_numSwapBuffers);

        void Append(UINT x, UINT y, UINT s) { m_mappings.front().emplace_back(x, y, s); }

        Coords& GetReadyToEvict() { return m_mappings.back(); }

        // FIXME: this is aspirational. it would also be cool if it looked later, to end() - swapchaincount.
        bool GetResidencyChangeNeeded() const { return !m_mappings.front().empty(); }

        void NextFrame();
        void Clear();

        // drop pending evictions for tiles that now have non-zero refcount
        // return true if tiles were rescued
        bool Rescue(const ResourceBase* in_pResource);

        // total # tiles being tracked
        // FIXME: would like this to be O(1)
        bool HasDelayedWork() const
        {
            ASSERT(m_mappings.size());

            auto end = std::prev(m_mappings.end());
            for (auto i = m_mappings.begin(); i != end; i++)
            {
                if (i->size()) { return true; }
            }

            return false;
        }
    private:
        std::list<Coords> m_mappings;
    };
};
