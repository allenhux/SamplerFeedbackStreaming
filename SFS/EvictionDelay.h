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
        using Coords = std::vector<D3D12_TILED_RESOURCE_COORDINATE>;

        EvictionDelay(UINT in_numSwapBuffers);

        void Append(const D3D12_TILED_RESOURCE_COORDINATE& in_coord) { m_mappings.front().push_back(in_coord); }

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
