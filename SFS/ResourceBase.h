//==============================================================
// Copyright © Intel Corporation
//
// SPDX-License-Identifier: MIT
// =============================================================

/*=============================================================================
Base class for SFSResource
=============================================================================*/

#pragma once

#include <vector>
#include <d3d12.h>
#include <string>
#include <array>

#include "SamplerFeedbackStreaming.h"
#include "InternalResources.h"

namespace SFS
{
    class ManagerSR;
    class Heap;
    class FileHandle;

    //=============================================================================
    // unpacked mips are dynamically loaded/evicted, preserving a min-mip-map
    // packed mips are not evicted from the heap (as little as 1 tile for a 16k x 16k texture)
    //=============================================================================
    class ResourceBase : public SFSResource
    {
    public:
        //-----------------------------------------------------------------
        // external APIs
        //-----------------------------------------------------------------
        // SFSM needs this for barrier on packed mips
        ID3D12Resource* GetTiledResource() const override { return m_resources.GetTiledResource(); }
        virtual void CreateFeedbackView(D3D12_CPU_DESCRIPTOR_HANDLE out_descriptor) override;
        virtual void CreateShaderResourceView(D3D12_CPU_DESCRIPTOR_HANDLE in_descriptor) override;
        virtual UINT GetMinMipMapWidth() const override { return m_tileReferencesWidth; }
        virtual UINT GetMinMipMapHeight() const override { return m_tileReferencesHeight; }
        virtual UINT GetMinMipMapOffset() const override { return m_residencyMapOffsetBase; }
        virtual UINT GetMinMipMapSize() const override { return GetMinMipMapWidth() * GetMinMipMapHeight(); }

        //-----------------------------------------------------------------
        // end external APIs
        //-----------------------------------------------------------------

        ResourceBase(
            // method that will fill a tile-worth of bits, for streaming
            const std::wstring& in_filename,
            // description with dimension, format, etc.
            const SFSResourceDesc& in_desc,
            // share heap and upload buffers with other InternalResources
            SFS::ManagerSR* in_pSFSManager,
            Heap* in_pHeap);

        virtual ~ResourceBase();

        // called whenever a new SFSResource is created - even one other than "this"
        void SetResidencyMapOffset(UINT in_residencyMapOffsetBase);

        // called when creating/changing FileStreamer
        void SetFileHandle(const class DataUploader* in_pDataUploader);

        //-------------------------------------
        // begin called by SFSM::EndFrame()
        // note: that is, called once per frame
        //-------------------------------------

        // returns true if packed mips are loaded
        // NOTE: this query will only return true one time
        bool GetPackedMipsNeedTransition();

        // the following are called only if the application made a feedback request for the object:

        // called after feedback map has been copied to the cpu
        void SetClearUavDescriptorOffset(UINT64 in_offset) { m_clearUavDescriptorOffset = in_offset; }
        UINT64 GetClearUavDescriptorOffset() const { return m_clearUavDescriptorOffset; }

        ID3D12Resource* GetOpaqueFeedback() { return m_resources.GetOpaqueFeedback(); }

        // call after drawing to get feedback
#if RESOLVE_TO_TEXTURE
        void ResolveFeedback(ID3D12GraphicsCommandList1* out_pCmdList, UINT64 in_frameFenceValue, ID3D12Resource* in_pDestination);
        // call after resolving to read back to CPU
        void ReadbackFeedback(ID3D12GraphicsCommandList* out_pCmdList, ID3D12Resource* in_pResolvedResource);
#else
        void ResolveFeedback(ID3D12GraphicsCommandList1* out_pCmdList, UINT64 in_frameFenceValue);
#endif

        bool& GetFirstUse() { return m_firstUse; }

        //-------------------------------------
        // end called by SFSM::EndFrame()
        //-------------------------------------

        // Called by ResidencyThread (and also on creation via BeginFrame()
        // exits fast if tile residency has not changed (due to addmap or decmap)
        bool UpdateMinMipMap();

        // do this fast!! inside lock
        inline void WriteMinMipMap(UINT8* out_pDest)
        {
            memcpy(m_residencyMapOffsetBase + out_pDest, m_minMipMap.data(), m_minMipMap.size());
        }

        //-------------------------------------
        // called by SFSM::ProcessFeedbackThread
        //-------------------------------------

        // call once per frame (as indicated e.g. by advancement of frame fence)
        // if a feedback buffer is ready, process it to generate lists of tiles to load/evict
        void ProcessFeedback(UINT64 in_frameFenceCompletedValue);

        // try to load/evict tiles.
        // returns # tiles requested for upload
        UINT QueuePendingTileLoads();

        // returns # tiles evicted
        UINT QueuePendingTileEvictions();

        bool HasDelayedWork() // tiles to load / evict now or later
        {
            return m_delayedEvictions.HasDelayedWork();
        }

        bool HasPendingWork() // wants to load / evict tiles this frame
        {
            return (m_pendingTileLoads.size() || m_delayedEvictions.GetReadyToEvict().size());
        }

        bool InitPackedMips();

#ifdef _DEBUG
        bool GetInitialized() { return (m_packedMipStatus >= PackedMipStatus::REQUESTED); }
#endif
        //-------------------------------------
        // end called by SFSM::ProcessFeedbackThread
        //-------------------------------------

        // immediately evicts all except packed mips
        // called by SFSM::SetVisualizationMode()
        void ClearAllocations();

    protected:
        const SFSResourceDesc m_resourceDesc;
        std::wstring m_filename; // only used so we can dynamically change file streamer type :/
        // packed mip status
        enum class PackedMipStatus : UINT32
        {
            UNINITIALIZED = 0, // have we requested packed mips yet?
            HEAP_RESERVED,     // heap spaced reserved
            REQUESTED,         // copy requested
            NEEDS_TRANSITION,  // copy complete, transition to readable
            RESIDENT           // mapped, loaded, and transitioned to pixel shader resource
        };
        PackedMipStatus m_packedMipStatus{ PackedMipStatus::UNINITIALIZED };        
        const UINT16 m_tileReferencesWidth{ 0 };  // function of resource tiling
        const UINT16 m_tileReferencesHeight{ 0 }; // function of resource tiling
        const UINT8 m_maxMip{ 0 }; // equals num standard mips, which is also the first packed mip
        bool m_firstUse{ true }; // queried on first call to queue feedback

        // set by QueueEviction() - render thread
        // read by ProcessFeedback()
        std::atomic<bool> m_evictAll{ false };

        // read by QueueEviction() - render thread, to avoid setting evict all unnecessarily
        // written by ProcessFeedback()
        std::atomic<bool> m_refCountsZero{ true };

        // standard tile copy complete notification (not packed mips)
        // exchanged by UpdateMinMip()
        // set by ProcessFeedback()
        std::atomic<bool> m_tileResidencyChanged{ false };

        SFS::InternalResources m_resources;
        std::unique_ptr<SFS::FileHandle> m_pFileHandle;
        SFS::Heap* m_pHeap{ nullptr };

        UINT64 m_clearUavDescriptorOffset{ 0 };

        // heap indices for packed mips only
        std::vector<UINT> m_packedMipHeapIndices;

        SFS::ManagerSR* const m_pSFSManager{ nullptr };

        //==================================================
        // TileMappingState keeps reference counts and heap indices for resources in a min-mip-map
        //==================================================
        class TileMappingState
        {
        public:
            void Init(const std::vector<SFSResourceDesc::StandardMipInfo>& in_standardMipInfo);

            // 4 states are encoded by the residency state and ref count:
            // residency | refcount | tile state
            // ----------+----------+------------------
            //      0    |    0     | not resident (data not resident & not mapped)
            //      0    |    n     | copy pending (data not resident & not mapped)
            //      1    |    0     | eviction pending (data resident & mapped)
            //      1    |    n     | resident (data resident & mapped)

            // residency is set to Resident or NotResident by the notify thread
            // residency is read by process feedback thread, and set to transient states Evicting or Loading
            enum Residency
            {
                NotResident = 0b00,
                Resident    = 0b01,
                Evicting    = 0b10,
                Loading     = 0b11,
            };

            void SetResidency(UINT x, UINT y, UINT s, Residency in_residency) { m_resident[s](x, y, GetWidth(s)) = (BYTE)in_residency; }
            BYTE GetResidency(UINT x, UINT y, UINT s) const { return m_resident[s](x, y, GetWidth(s)); }
            UINT32& GetRefCount(UINT x, UINT y, UINT s) { return m_refcounts[s](x, y, GetWidth(s)); }

            void SetResidency(const D3D12_TILED_RESOURCE_COORDINATE& in_coord, Residency in_residency) { SetResidency(in_coord.X, in_coord.Y, in_coord.Subresource, in_residency); }
            BYTE GetResidency(const D3D12_TILED_RESOURCE_COORDINATE& in_coord) { return GetResidency(in_coord.X, in_coord.Y, in_coord.Subresource); }
            UINT32 GetRefCount(const D3D12_TILED_RESOURCE_COORDINATE& in_coord) const { return m_refcounts[in_coord.Subresource](in_coord.X, in_coord.Y, GetWidth(in_coord.Subresource)); }

            UINT32& GetHeapIndex(const D3D12_TILED_RESOURCE_COORDINATE& in_coord) { return m_heapIndices[in_coord.Subresource](in_coord.X, in_coord.Y, GetWidth(in_coord.Subresource)); }

            // searches refcount of bottom-most non-packed tile(s). If none are in use, we know nothing is resident.
            // used in both UpdateMinMipMap() and ProcessFeedback()
            bool GetAnyRefCount() const;

            // return true if all bottom layer standard tiles are resident
            // Can accelerate UpdateMinMipMap()
            UINT8 GetMinResidentMip();

            // remove all mappings from a heap. useful when removing an object from a scene
            void FreeHeapAllocations(SFS::Heap* in_pHeap);

            UINT GetWidth(UINT in_s) const { return m_dimensions[in_s].m_width; }
            UINT GetHeight(UINT in_s) const { return m_dimensions[in_s].m_height; }

            std::vector<UINT32>& GetRefLayer(UINT s) { return m_refcounts[s].m_tiles; }

            static const UINT InvalidIndex{ UINT(-1) };
        private:
            template<typename T> struct Layer
            {
            public:
                void Init(UINT in_width, UINT in_height, T value) { m_tiles.assign(in_width * in_height, value); };
                T& operator()(UINT x, UINT y, UINT w) { return m_tiles[y * w + x]; }
                T operator()(UINT x, UINT y, UINT w) const { return m_tiles[y * w + x]; }
                std::vector<T> m_tiles;
            };
            template<typename T> using TileLayers = std::vector<Layer<T>>;

            struct Dim
            {
                UINT m_width;
                UINT m_height;
            };
            std::vector<Dim> m_dimensions;

            TileLayers<BYTE> m_resident;
            TileLayers<UINT32> m_refcounts;
            TileLayers<UINT32> m_heapIndices;
        };
        TileMappingState m_tileMappingState;

        void SetResidencyChanged();
    private:
        // only using double-buffering for feedback history
        static constexpr UINT QUEUED_FEEDBACK_FRAMES = 2;

        // do not immediately decmap:
        // need to withhold until in-flight command buffers have completed
        class EvictionDelay
        {
        public:
            using Coords = std::vector<D3D12_TILED_RESOURCE_COORDINATE>;

            EvictionDelay(UINT in_numSwapBuffers);

            void Append(D3D12_TILED_RESOURCE_COORDINATE in_coord) { m_mappings.front().push_back(in_coord); }
            Coords& GetReadyToEvict() { return m_mappings.back(); }

            void NextFrame();
            void Clear();
            void MoveAllToPending();

            // drop pending evictions for tiles that now have non-zero refcount
            // return true if tiles were rescued
            void Rescue(const TileMappingState& in_tileMappingState);

            // total # tiles being tracked
            bool HasDelayedWork() const
            {
                ASSERT(m_mappings.size());
#if 1
                auto end = m_mappings.end()--;
                for (auto i = m_mappings.begin(); i != end; i++)
                {
                    if (i->size()) { return true; }
                }
#else
                UINT sz = (UINT)m_mappings.size() - 1;
                for (auto& e : m_mappings)
                {
                    if (e.size()) { return true; }
                    sz--;
                    if (0 == sz) { break; }
                }
#endif
                return false;
            }
        private:
            std::list<Coords> m_mappings;
        };
        EvictionDelay m_delayedEvictions;

        std::vector<D3D12_TILED_RESOURCE_COORDINATE> m_pendingTileLoads;

        //--------------------------------------------------------
        // for public interface
        //--------------------------------------------------------
        // minimum mip level referred to by this tile
        // this tile "holds references" to this mip level and all mips greater than this value
        // with a 16kx16k limit, DX will never see 255 mip levels. but, we want a byte so we can modify cache-coherently
        using TileReference = UINT8;
        std::vector<TileReference> m_tileReferences;

        std::vector<BYTE, SFS::AlignedAllocator<BYTE>> m_minMipMap; // local version of min mip map, rectified in UpdateMinMipMap()

        // drop pending loads that are no longer relevant
        void AbandonPendingLoads();

        // if feedback is queued, it is ready to use after the render fence has reached this value
        // support having a feedback queued every frame (num swap buffers)
        struct QueuedFeedback
        {
            UINT64 m_renderFenceForFeedback{ UINT_MAX };
            std::atomic<bool> m_feedbackQueued{ false }; // written by render thread, read by ProcessFeedback() thread
        };
        std::array<QueuedFeedback, QUEUED_FEEDBACK_FRAMES> m_queuedFeedback;

        // update internal refcounts based on the incoming minimum mip
        void SetMinMip(UINT in_x, UINT in_y, UINT in_current, UINT in_desired);

        // AddRef, which requires allocation, might fail
        void AddTileRef(UINT in_x, UINT in_y, UINT in_s);

        // DecRef may decline
        void DecTileRef(UINT in_x, UINT in_y, UINT in_s);

        // index to next min-mip feedback resolve target
        UINT m_readbackIndex{ 0 };

        UINT m_residencyMapOffsetBase{ UINT(-1) };
    };
}
