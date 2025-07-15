//==============================================================
// Copyright © Intel Corporation
//
// SPDX-License-Identifier: MIT
// =============================================================

#include "pch.h"

#include "ResourceBase.h"
#include "ManagerSR.h"

#include "UpdateList.h"

#include "SFSHeap.h"
#include "DataUploader.h"

/*-----------------------------------------------------------------------------
* Rules regarding order of operations:
*
* 1. A tile cannot be evicted (DecTileRef tries to set refcount = 0) if resident = 0 because a copy is pending
* 2. A tile cannot be loaded (AddTileRef tries to set refcount = 1) if resident = 1 because an eviction is pending
//---------------------------------------------------------------------------*/

//=============================================================================
// data structure to manage reserved resource
// NOTE: constructor must be thread safe
//       because thread-safe, AllocateAtlas() must be called elsewhere
//=============================================================================
SFS::ResourceBase::ResourceBase(
    // file containing streaming texture and tile offsets
    const std::wstring& in_filename,
    // description with dimension, format, etc.
    const SFSResourceDesc& in_desc,
    // share upload buffers with other InternalResources
    SFS::ManagerSR* in_pSFSManager,
    // share heap with other StreamingResources
    SFS::Heap* in_pHeap) :
    m_pSFSManager(in_pSFSManager)
    , m_delayedEvictions(in_pSFSManager->GetEvictionDelay())
    , m_pHeap(in_pHeap)
    , m_resourceDesc(in_desc)
    , m_filename(in_filename)
    , m_maxMip((UINT8)m_resourceDesc.m_mipInfo.m_numStandardMips)
    , m_tileReferencesWidth((UINT16)m_resourceDesc.m_standardMipInfo[0].m_widthTiles)
    , m_tileReferencesHeight((UINT16)m_resourceDesc.m_standardMipInfo[0].m_heightTiles)
{
    // most internal allocation deferred
    // there had better be standard mips, otherwise, why stream?
    ASSERT(m_maxMip);

    // with m_maxMip and m_minMipMap, SFSManager will be able allocate/init the shared residency map
    UINT alignedMask = RESIDENCY_MAP_ALIGNMENT - 1;
    UINT alignedSize = (GetMinMipMapSize() + alignedMask) & ~alignedMask;
    m_minMipMap.assign(alignedSize, m_maxMip);

    m_resources.Initialize(m_pSFSManager->GetDevice(), m_resourceDesc, QUEUED_FEEDBACK_FRAMES);

    m_tileMappingState.Init(m_resourceDesc.m_standardMipInfo);

    // initialize a structure that holds ref counts with dimensions equal to min-mip-map
    // m_tileReferences tracks what tiles we want to have
    // m_minMipMap represents the tiles we actually have, and is read directly by pixel shaders
    m_tileReferences.assign(GetMinMipMapSize(), m_maxMip);

    m_pFileHandle.reset(m_pSFSManager->OpenFile(m_filename));
}

//-----------------------------------------------------------------------------
// destroy this object
// Free() all heap allocations, then release the heap
//-----------------------------------------------------------------------------
SFS::ResourceBase::~ResourceBase()
{
    m_pSFSManager->Remove(this);

    // remove tile allocations from the heap
    m_tileMappingState.FreeHeapAllocations(m_pHeap);

    // remove packed mip allocations from the heap
    if (m_packedMipHeapIndices.size())
    {
        m_pHeap->GetAllocator().Free(m_packedMipHeapIndices);
    }
}

//-----------------------------------------------------------------------------
// when the SFSResource gets an offset into the shared ResidencyMap,
// it can be initialized to the current minmipmap state
//-----------------------------------------------------------------------------
void SFS::ResourceBase::SetResidencyMapOffset(UINT in_residencyMapOffsetBase)
{
    m_residencyMapOffsetBase = in_residencyMapOffsetBase;
}

//-----------------------------------------------------------------------------
// Update internal refcounts of tiles based on the incoming minimum mip
// the refcount is the number of regions that depend on that underlying tile
// consider this min mip map:
// 1 2 2 1
// 2 1 2 2
// 0 2 2 2
// 2 2 2 3
// these are the corresponding refcounts for each mip layer:
// 0 0 0 0 | 2 1 | 15
// 0 0 0 0 | 1 0
// 1 0 0 0 |
// 0 0 0 0 |
//-----------------------------------------------------------------------------
void SFS::ResourceBase::SetMinMip(UINT in_x, UINT in_y, UINT s, UINT in_desired)
{
    // s is the mip level is currently referenced at this tile

    // addref mips we want
    // AddRef()s are ordered from bottom mip to top (if we can't load them all, prefer the bottom mips first)
    while (s > in_desired)
    {
        s -= 1; // already have "this" tile. e.g. have s == 1, desired in_s == 0, start with 0.
        AddTileRef(in_x >> s, in_y >> s, s);
    }

    // decref mips we don't need
    // DecRef()s are ordered from top mip to bottom (evict lower resolution tiles after all higher resolution ones)
    while (s < in_desired)
    {
        DecTileRef(in_x >> s, in_y >> s, s);
        s++;
    }
}

//-----------------------------------------------------------------------------
// add to refcount for a tile
// if first time, add tile to list of pending loads
//-----------------------------------------------------------------------------
void SFS::ResourceBase::AddTileRef(UINT in_x, UINT in_y, UINT in_s)
{
    auto& refCount = m_tileMappingState.GetRefCount(in_x, in_y, in_s);

    // if refcount is 0xffff... then adding to it will wrap around. shouldn't happen.
    ASSERT(~refCount);

    // need to allocate?
    if (0 == refCount)
    {
        m_pendingTileLoads.push_back(D3D12_TILED_RESOURCE_COORDINATE{ in_x, in_y, 0, in_s });
    }
    refCount++;
}

//-----------------------------------------------------------------------------
// reduce ref count
// if 0, add tile to list of pending evictions
//-----------------------------------------------------------------------------
void SFS::ResourceBase::DecTileRef(UINT in_x, UINT in_y, UINT in_s)
{
    auto& refCount = m_tileMappingState.GetRefCount(in_x, in_y, in_s);

    ASSERT(0 != refCount);

    // last reference? try to evict
    if (1 == refCount)
    {
        // queue up a decmapping request that will release the heap index after mapping and clear the resident flag
        m_delayedEvictions.Append(D3D12_TILED_RESOURCE_COORDINATE{ in_x, in_y, 0, in_s });
    }
    refCount--;
}

//-----------------------------------------------------------------------------
// initialize data structure afther creating the reserved resource and querying its tiling properties
//-----------------------------------------------------------------------------
void SFS::ResourceBase::TileMappingState::Init(const std::vector<SFSResourceDesc::StandardMipInfo>& in_standardMipInfo)
{
    UINT numMips = (UINT)in_standardMipInfo.size();
    m_refcounts.resize(numMips);
    m_heapIndices.resize(numMips);
    m_resident.resize(numMips);

    for (UINT mip = 0; mip < numMips; mip++)
    {
        UINT width = in_standardMipInfo[mip].m_widthTiles;
        UINT height = in_standardMipInfo[mip].m_heightTiles;

        m_dimensions.push_back({ width, height });

        m_refcounts[mip].Init(width, height, 0);
        m_heapIndices[mip].Init(width, height, TileMappingState::InvalidIndex);
        m_resident[mip].Init(width, height, 0);
    }
}

//-----------------------------------------------------------------------------
// remove all allocations from the (shared) heap
//-----------------------------------------------------------------------------
void SFS::ResourceBase::TileMappingState::FreeHeapAllocations(SFS::Heap* in_pHeap)
{
    std::vector<UINT> indices;
    for (auto layer = m_heapIndices.begin(); layer != m_heapIndices.end(); layer++)
    {
        for (auto& i : layer->m_tiles)
        {
            if (TileMappingState::InvalidIndex != i)
            {
                indices.push_back(i);
                i = TileMappingState::InvalidIndex;
            }
        }
    }
    if (indices.size())
    {
        in_pHeap->GetAllocator().Free(indices);
    }
}

//-----------------------------------------------------------------------------
// search bottom layer. if refcount of any is positive, there is something resident.
//-----------------------------------------------------------------------------
bool SFS::ResourceBase::TileMappingState::GetAnyRefCount() const
{
    for (auto i : m_refcounts.back().m_tiles)
    {
        if (i)
        {
            return true;
        }
    }

    return false;
}

//-----------------------------------------------------------------------------
// if the residency changes, must also notify SFSM
//-----------------------------------------------------------------------------
void SFS::ResourceBase::SetResidencyChanged()
{
    m_tileResidencyChanged = true;
    m_pSFSManager->SetResidencyChanged();
}

//-----------------------------------------------------------------------------
// called once per frame
// adds virtual memory updates to command queue
// queues memory content updates to copy thread
// Algorithm: evict then load tiles
//            loads lower mip dependencies first
// e.g. if we need tile 0,0,0 then 0,0,1 must have previously been loaded
//-----------------------------------------------------------------------------
void SFS::ResourceBase::ProcessFeedback(UINT64 in_frameFenceCompletedValue)
{
    // handle (some) pending evictions
    m_delayedEvictions.NextFrame();

    bool changed = false;

    // idea of evictall is that the application knows the object is no longer visible (e.g. offscreen),
    // so no reason to draw it then read and process feedback to tell it what it already knows 
    if (m_evictAll)
    {
        m_evictAll = false;

        // has this resource already been zeroed? don't clear again, early exit
        // future processing of feedback will set this to false if "changed"
        if (m_refCountsZero)
        {
            return;
        }
        m_refCountsZero = true;

        // all prior feedback is irrelevant
        for (auto& f : m_queuedFeedback)
        {
            f.m_feedbackQueued = false;
        }

        // since we're evicting everything, don't need to loop over the reference count structure
        // just set it all to max mip, then schedule eviction for any tiles that have refcounts

        // set everything to max mip
        memset(m_tileReferences.data(), m_maxMip, m_tileReferences.size());

        // queue all resident tiles for eviction
        for (INT s = m_maxMip - 1; s >= 0; s--)
        {
            bool layerChanged = false;

            auto& l = m_tileMappingState.GetRefLayer(s); // directly access data for performance
            UINT width = m_tileMappingState.GetWidth(s);

            for (UINT i = 0; i < (UINT)l.size(); i++)
            {
                auto& refCount = l[i];
                if (refCount)
                {
                    layerChanged = true;
                    refCount = 0;
                    m_delayedEvictions.Append(D3D12_TILED_RESOURCE_COORDINATE{ i % width, i / width, 0, (UINT)s });
                }
            }

            // if no tiles had refcounts (to change to 0) on this mip layer, won't be any tiles on higher-res mip layers
            if (!layerChanged)
            {
                break; // if refcount of all tiles on this layer = 0, early out
            }
            changed = true;
        } // end loop over layers

        // this would bypass the usual delay
        // m_delayedEvictions.MoveAllToPending();

        // abandon all pending loads - all refcounts are 0
        m_pendingTileLoads.clear();

        // FIXME? could reset the min mip map now, avoiding updateminmipmap via setresidencychanged. any chance of a race?
    }
    else
    {
        //------------------------------------------------------------------
        // determine if there is feedback to process
        // if there is more than one feedback ready to process (unlikely), only use the most recent one
        //------------------------------------------------------------------
        QueuedFeedback* pQueuedFeedback = nullptr;
        UINT feedbackIndex = 0;
        for (UINT i = 0; i < (UINT)m_queuedFeedback.size(); i++)
        {
            auto& q = m_queuedFeedback[i];
            if (q.m_feedbackQueued && (q.m_renderFenceForFeedback <= in_frameFenceCompletedValue))
            {
                // return the newest set first
                if ((nullptr == pQueuedFeedback) || (pQueuedFeedback->m_renderFenceForFeedback > q.m_renderFenceForFeedback))
                {
                    pQueuedFeedback = &q;
                    feedbackIndex = i;
                }
                q.m_feedbackQueued = false;
            }
        }
        if (nullptr == pQueuedFeedback)
        {
            return;
        }

        //------------------------------------------------------------------
        // update the refcount of each tile based on feedback
        //------------------------------------------------------------------
        {
            const UINT width = GetMinMipMapWidth();
            const UINT height = GetMinMipMapHeight();

            // mapped host feedback buffer
            UINT8* pResolvedData = (UINT8*)m_resources.MapResolvedReadback(feedbackIndex);

            TileReference* pTileRow = m_tileReferences.data();
            for (UINT y = 0; y < height; y++)
            {
                for (UINT x = 0; x < width; x++)
                {
                    // clamp to the maximum we are tracking (not tracking packed mips)
                    UINT8 desired = std::min(pResolvedData[x], m_maxMip);
                    UINT8 currentValue = pTileRow[x];
                    if (desired != currentValue)
                    {
                        changed = true;
                        SetMinMip(x, y, currentValue, desired);
                        pTileRow[x] = desired;
                    }
                } // end loop over x
                pTileRow += width;

#if RESOLVE_TO_TEXTURE
                // CopyTextureRegion requires pitch multiple of D3D12_TEXTURE_DATA_PITCH_ALIGNMENT = 256
                constexpr UINT alignmentMask = D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1;
                pResolvedData += (width + alignmentMask) & ~alignmentMask;
#else
                pResolvedData += width;
#endif

            } // end loop over y
            m_resources.UnmapResolvedReadback(feedbackIndex);
        }

        // if refcount changed, there's a new pending upload or eviction
        // take time to rescue and abandon pending actions due to new refcounts
        // only need to SetResidencyChanged() on rescue
        if (changed)
        {
            // abandon pending loads that are no longer relevant
            AbandonPendingLoads();

            // clear pending evictions that are no longer relevant
            if (m_tileMappingState.GetAnyRefCount())
            {
                m_refCountsZero = false;
                m_delayedEvictions.Rescue(m_tileMappingState);
            }
            else
            {
                // ended up with no tiles resident
                m_refCountsZero = true;
            }
        } // end if changed
    }

    // if refcount -> 0, treat as though evicted even though resident. memory not recycled until later.
    // if refcount -> 1+ and resident, treat as though loaded immediately. memory was never released.

    // update min mip map to adjust to new references
    // required so the eviction timeout is relative to the current expected mapping
    if (changed)
    {
        SetResidencyChanged();
    }
}

//-----------------------------------------------------------------------------
// drop pending loads that are no longer relevant
//-----------------------------------------------------------------------------
void SFS::ResourceBase::AbandonPendingLoads()
{
    UINT numPending = (UINT)m_pendingTileLoads.size();
    for (UINT i = 0; i < numPending;)
    {
        auto& c = m_pendingTileLoads[i];
        if (m_tileMappingState.GetRefCount(c))
        {
            i++; // refcount still non-0, still want to load this tile
        }
        // on abandon, swap a later tile in and re-try the check
        // this re-orders the queue, but we can tolerate that
        // because the residency map is built bottom-up
        else
        {
            numPending--;
            c = m_pendingTileLoads[numPending];
        }
    }
    m_pendingTileLoads.resize(numPending);
}


/*-----------------------------------------------------------------------------
This technique depends on a logic table that prevents race conditions

The table has two halves:
1) refcount > 0 is handled by loads. pending loads with refcount = 0 are dropped ("abandoned")
2) refcount == 0 is handled by evictions. pending evictions with refcount > 0 are dropped ("rescued")

ProcessFeedback() scans pending loads and pending evictions prune the drops above.

The logic table for evictions:

    ref count | heap index | resident | action
    ----------+------------+----------+--------
        0     |  invalid   |    0     | drop (tile already not resident)
        0     |  invalid   |    1     | drop (tile already has pending eviction)
        0     |   valid    |    0     | delay (tile has pending load, wait for it to complete)
        0     |   valid    |    1     | evict (tile is resident, so can be evicted)

The logic table for loads:

    ref count | heap index | resident | action
    ----------+------------+----------+--------
        n     |  invalid   |    0     | load (tile not resident, so can be loaded)
        n     |  invalid   |    1     | delay (tile has pending eviction, wait for it to complete)
        n     |   valid    |    0     | drop (tile already has pending load)
        n     |   valid    |    1     | drop (tile already resident)

Residency is set by the notification functions called by DataUploader (a separate thread)
Allocating and freeing heap indices is handled respectively by the load and eviction routines below
Note that the multi-frame delay for evictions prevents allocation of an index that is in-flight for a different tile

-----------------------------------------------------------------------------*/

//-----------------------------------------------------------------------------
// evict unused tiles
// this is after a multi-frame delay and avoiding potential race conditions to avoid visual  artifacts
//
// note there are only tiles to evict after processing feedback, but it's possible
// there was no UpdateList available at the time, so they haven't been evicted yet.
//-----------------------------------------------------------------------------
UINT SFS::ResourceBase::QueuePendingTileEvictions()
{
    if (0 == m_delayedEvictions.GetReadyToEvict().size()) { return 0; }

    auto& pendingEvictions = m_delayedEvictions.GetReadyToEvict();

    UINT numDelayed = 0;
    UINT numEvictions = 0;
    for (auto& coord : pendingEvictions)
    {
        // if the heap index is valid, but the tile is not resident, there's a /pending load/
        // a pending load might be streaming OR it might be in the pending list
        // if in the pending list, we will observe if the refcount is 0 and abandon the load

        // NOTE! assumes refcount is 0
        // ProcessFeedback() clears all pending evictions with refcount > 0
        // Hence, ProcessFeedback() must be called before this function
        ASSERT(0 == m_tileMappingState.GetRefCount(coord));

        auto residency = m_tileMappingState.GetResidency(coord);
        if (TileMappingState::Residency::Resident == residency)
        {
            // NOTE: effectively removed "Evicting." Now remove tiles from data structure, not from memory mapping.
            // result is improved perf from fewer UpdateTileMappings() calls.
            // existing artifacts (cracks when sampler crosses tile boundaries) are "no worse"
            // to put it back: set residency to evicting and add tiles to updatelist for eviction

            m_tileMappingState.SetResidency(coord, TileMappingState::Residency::NotResident);
            UINT& heapIndex = m_tileMappingState.GetHeapIndex(coord);
            m_pHeap->GetAllocator().Free(heapIndex);
            heapIndex = TileMappingState::InvalidIndex;

            numEvictions++;
        }
        // valid index but not resident means there is a pending load, do not evict
        // try again later
        else if (TileMappingState::Residency::Loading == residency)
        {
            pendingEvictions[numDelayed] = coord;
            numDelayed++;
        }
        // if evicting or not resident, drop

        // else: refcount positive or eviction already in progress? rescue this eviction (by not adding to pending evictions)
    }

    // tiles evicted here were identified multiple frames earlier, leaving time for tiles to be Rescue()d.
    // Do not call SetResidencyChanged() here; tiles on their way out were identified in ProcessFeedback() (once per frame)
    // i.e. do not: if (numEvictions) { SetResidencyChanged(); }

    // narrow the ready evictions to just the delayed evictions.
    pendingEvictions.resize(numDelayed);

    return numEvictions;
}

//-----------------------------------------------------------------------------
// queue one UpdateList worth of uploads
//-----------------------------------------------------------------------------
UINT SFS::ResourceBase::QueuePendingTileLoads()
{
    // clamp to heap availability
    UINT maxCopies = std::min((UINT)m_pendingTileLoads.size(), m_pHeap->GetAllocator().GetAvailable());

    if (0 == maxCopies)
    {
        return 0;
    }

    std::vector<D3D12_TILED_RESOURCE_COORDINATE> uploads;
    uploads.reserve(maxCopies);

    UINT skippedIndex = 0;
    UINT numConsumed = 0;
    for (const auto& coord : m_pendingTileLoads)
    {
        numConsumed++;

        // expect refcount is non-zero
        ASSERT(m_tileMappingState.GetRefCount(coord));

        auto residency = m_tileMappingState.GetResidency(coord);

        // only load if definitely not resident
        if (TileMappingState::Residency::NotResident == residency)
        {
            m_tileMappingState.SetResidency(coord, TileMappingState::Residency::Loading);
            uploads.push_back(coord);

            maxCopies--;
            if (0 == maxCopies)
            {
                break;
            }
        }
        // if there is a pending eviction, do not load. Try again later.
        else if (TileMappingState::Residency::Evicting == residency)
        {
            // accumulate skipped tiles at front of the pending list
            m_pendingTileLoads[skippedIndex] = coord;
            skippedIndex++;
        }
        // if loading or resident, abandon this load
        // can happen if Rescue()d or duplicate (latter shouldn't happen)
    }

    // delete consumed tiles, which are in-between the skipped tiles and the still-pending tiles
    m_pendingTileLoads.erase(m_pendingTileLoads.begin() + skippedIndex, m_pendingTileLoads.begin() + numConsumed);

    UINT numUploads = (UINT)uploads.size();
    if (numUploads)
    {
        // calling function checked for availability, so UL allocation must succeed
        UpdateList* pUpdateList = m_pSFSManager->AllocateUpdateList(this);
        ASSERT(pUpdateList);

        auto& heapIndices = pUpdateList->m_heapIndices;
        heapIndices.resize(numUploads);
        // uploads was clamped to heap availability, so heap allocate will succeed
        m_pHeap->GetAllocator().Allocate(heapIndices.data(), numUploads);
        for (UINT i = 0; i < numUploads; i++)
        {
            m_tileMappingState.GetHeapIndex(uploads[i]) = heapIndices[i];
        }
        pUpdateList->m_coords.swap(uploads);
        m_pSFSManager->SubmitUpdateList(*pUpdateList);
    }
    return numUploads;
}

//-----------------------------------------------------------------------------
// Update the residency map (which acts as a texture clamp)
// ProcessFeedback() produced loads and evictions that are a delta from
//    what we had (m_minMipMap) to the desired state (m_tileReferences)
// Set the residency based on what is actually resident (m_tileMappingState)
// NOTE many of these values are likely being changed by other threads:
//     m_tileReferences by ProcessFeedbackThread and m_tileMappingState by DataUploader
//-----------------------------------------------------------------------------
bool SFS::ResourceBase::UpdateMinMipMap()
{
    // m_tileResidencyChanged is an atomic that forms a happens-before relationship between this thread and DataUploader Notify* routines
    // m_tileResidencyChanged is also set when ClearAll() evicts everything
    bool expected = true;
    if (!m_tileResidencyChanged.compare_exchange_weak(expected, false))
    {
        return false;
    }

    // NOTE: packed mips status is not atomic, but m_tileResidencyChanged is sufficient
    ASSERT(Drawable());

    if (m_tileMappingState.GetAnyRefCount())
    {
        const UINT width = GetMinMipMapWidth();
        const UINT height = GetMinMipMapHeight();

        // Search bottom up for best mip
        // tiles that have refcounts may still have pending copies, so we have to check residency (can't just memcpy m_tileReferences)
        // note that tiles can load out of order, but the min mip map cannot have holes, so exit if any lower-res tile is absent
        // for 16kx16k textures, that's 7-1 iterations maximum (maximum for bc7: 64*64*(7-1)=24576, bc1: 32*64*(6-1)=10240)
        // in practice don't expect to hit the maximum, as the entire texture would have to be loaded
        // FIXME? could probably optimize e.g. by vectorizing
        UINT tileIndex = 0;
        for (UINT y = 0; y < height; y++)
        {
            for (UINT x = 0; x < width; x++)
            {
                // look at what we had last time (m_minMipMap). do we have the tiles resident to be able to clamp to what feedback says we need?
                if (m_tileReferences[tileIndex] != m_minMipMap[tileIndex])
                {
                    UINT8 minMip = std::max(m_tileReferences[tileIndex], m_minMipMap[tileIndex]);
                    while (minMip > m_tileReferences[tileIndex])
                    {
                        UINT8 s = minMip - 1;
                        if (TileMappingState::Residency::Resident == m_tileMappingState.GetResidency(x >> s, y >> s, s))
                        {
                            minMip = s;
                        }
                        else
                        {
                            break;
                        }
                    }
                    m_minMipMap[tileIndex] = minMip;
                }
                tileIndex++;
            } // end y
        } // end x
    }
    // if we know that only packed mips are resident, then write a basic residency map
    // if refcount is 0, then tile state is either not resident or eviction pending
    else
    {
        m_minMipMap.assign(m_minMipMap.size(), m_maxMip);
    }
    return true;
}

//=============================================================================
// class used to delay decmaps by a number of frames = # swap buffers
// easy way to prevent decmapping an in-flight tile
//=============================================================================
SFS::ResourceBase::EvictionDelay::EvictionDelay(UINT in_numSwapBuffers)
{
    m_mappings.resize(in_numSwapBuffers);
}

//-----------------------------------------------------------------------------
// step pending evictions once per frame
//-----------------------------------------------------------------------------
void SFS::ResourceBase::EvictionDelay::NextFrame()
{
    // move next-to-last vector to front of the list
    m_mappings.splice(m_mappings.begin(), m_mappings, --m_mappings.end());
    // append elements of first vector to end of last vector
    m_mappings.back().insert(m_mappings.back().end(), m_mappings.front().begin(), m_mappings.front().end());
    // clear first vector
    m_mappings.front().clear();
}

//-----------------------------------------------------------------------------
// dump all pending evictions. return heap indices to heap
//-----------------------------------------------------------------------------
void SFS::ResourceBase::EvictionDelay::Clear()
{
    for (auto& i : m_mappings)
    {
        i.clear();
    }
}

void SFS::ResourceBase::EvictionDelay::MoveAllToPending()
{
    auto end = --m_mappings.end();
    for (auto i = m_mappings.begin(); i != end; i++)
    {
        m_mappings.back().insert(m_mappings.back().end(), i->begin(), i->end());
        i->clear();
    }
}

//-----------------------------------------------------------------------------
// drop pending evictions for tiles that now have non-zero refcount
//-----------------------------------------------------------------------------
void SFS::ResourceBase::EvictionDelay::Rescue(const SFS::ResourceBase::TileMappingState& in_tileMappingState)
{
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
                if (in_tileMappingState.GetRefCount(c))
                {
                    numPending--;
                    c = evictions[numPending];
                }
                else // refcount still 0, this tile may still be evicted
                {
                    i++;
                }
            }
            evictions.resize(numPending);
        }
    }
}

//-----------------------------------------------------------------------------
// called when creating/changing FileStreamer
//-----------------------------------------------------------------------------
void SFS::ResourceBase::SetFileHandle(const DataUploader* in_pDataUploader)
{
    m_pFileHandle.reset(in_pDataUploader->OpenFile(m_filename));
}

//-----------------------------------------------------------------------------
// set mapping and initialize bits for the packed tile(s)
// called by ProcessFeedbackThread
//-----------------------------------------------------------------------------
bool SFS::ResourceBase::InitPackedMips()
{
    // nothing to do if the copy has been requested
    // return true if ready to sample
    if ((UINT)m_packedMipStatus >= (UINT)PackedMipStatus::REQUESTED)
    {
        return true;
    }

    UINT numTilesForPackedMips = m_resources.GetNumTilesForPackedMips();

    // no packed mips? odd, but possible. no need to check/update this variable again.
    if (0 == m_resourceDesc.m_mipInfo.m_numPackedMips)
    {
        // make sure my heap has an atlas corresponding to my format
        m_pHeap->AllocateAtlas(m_pSFSManager->GetMappingQueue(), (DXGI_FORMAT)m_resourceDesc.m_textureFormat);

        m_packedMipStatus = PackedMipStatus::NEEDS_TRANSITION;
        return true;
    }

    // allocate heap space
    // only allocate if all required tiles can be allocated at once
    if (PackedMipStatus::HEAP_RESERVED > m_packedMipStatus)
    {
        if (m_pHeap->GetAllocator().GetAvailable() >= numTilesForPackedMips)
        {
            m_pHeap->GetAllocator().Allocate(m_packedMipHeapIndices, numTilesForPackedMips);
            m_packedMipStatus = PackedMipStatus::HEAP_RESERVED;
        }
        else
        {
            return false; // heap full
        }
    }

    ASSERT(m_packedMipHeapIndices.size() == numTilesForPackedMips);

    // attempt to upload by acquiring an update list. may take many tries.
    SFS::UpdateList* pUpdateList = m_pSFSManager->AllocateUpdateList(this);

    if (pUpdateList)
    {
        // make sure my heap has an atlas corresponding to my format
        m_pHeap->AllocateAtlas(m_pSFSManager->GetMappingQueue(), (DXGI_FORMAT)m_resourceDesc.m_textureFormat);

        m_packedMipStatus = PackedMipStatus::REQUESTED;

        pUpdateList->m_heapIndices = m_packedMipHeapIndices;
        m_pSFSManager->SubmitUpdateList(*pUpdateList);

        return true;
    }

    return false;
}

//-----------------------------------------------------------------------------
// command to resolve feedback to the appropriate non-opaque buffer
//-----------------------------------------------------------------------------
#if RESOLVE_TO_TEXTURE
void SFS::ResourceBase::ResolveFeedback(ID3D12GraphicsCommandList1* out_pCmdList, UINT64 in_frameFenceValue, ID3D12Resource* in_pDestination)
#else
void SFS::ResourceBase::ResolveFeedback(ID3D12GraphicsCommandList1* out_pCmdList, UINT64 in_frameFenceValue)
#endif
{
    // move to next readback index
    m_readbackIndex = (m_readbackIndex + 1) % m_queuedFeedback.size();

    // remember that feedback was queued, and which frame it was queued in.
    auto& f = m_queuedFeedback[m_readbackIndex];
    f.m_renderFenceForFeedback = in_frameFenceValue;
    f.m_feedbackQueued = true;

#if RESOLVE_TO_TEXTURE
    m_resources.ResolveFeedback(out_pCmdList, in_pDestination);
#else
    m_resources.ResolveFeedback(out_pCmdList, m_readbackIndex);
#endif
}

#if RESOLVE_TO_TEXTURE
//-----------------------------------------------------------------------------
// call after resolving to read back to CPU
//-----------------------------------------------------------------------------
void SFS::ResourceBase::ReadbackFeedback(ID3D12GraphicsCommandList* out_pCmdList, ID3D12Resource* in_pResolvedResource)
{
    // write readback command to command list if resolving to texture
    m_resources.ReadbackFeedback(out_pCmdList, in_pResolvedResource, m_readbackIndex, GetMinMipMapWidth(), GetMinMipMapHeight());
}
#endif

//-----------------------------------------------------------------------------
// eject all tiles and remove mappings into heap
// Only called by SfsManager::SetVisualizationMode()
//-----------------------------------------------------------------------------
void SFS::ResourceBase::ClearAllocations()
{
    m_tileMappingState.FreeHeapAllocations(m_pHeap);
    m_tileMappingState.Init(m_resourceDesc.m_standardMipInfo);
    m_tileReferences.assign(m_tileReferences.size(), m_maxMip);
    m_minMipMap.assign(m_minMipMap.size(), m_maxMip);

    m_delayedEvictions.Clear();
    m_pendingTileLoads.clear();
}

//-----------------------------------------------------------------------------
// Return to "like new" state
// used by FlushResources()
//-----------------------------------------------------------------------------
void SFS::ResourceBase::Reset()
{
    ClearAllocations();
    if (m_packedMipHeapIndices.size())
    {
        m_pHeap->GetAllocator().Free(m_packedMipHeapIndices);
        m_packedMipStatus = PackedMipStatus::RESET;
    }
}
