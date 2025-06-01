//==============================================================
// Copyright © Intel Corporation
//
// SPDX-License-Identifier: MIT
// =============================================================

#pragma once

#include <d3d12.h>
#include <vector>

#include "Streaming.h"

namespace SFS
{
    class SimpleAllocator
    {
    public:
        SimpleAllocator(UINT in_maxNumElements);
        virtual ~SimpleAllocator();

        // assumes caller is doing due-diligence to allocate destination appropriately and check availability before calling
        void Allocate(UINT* out_pIndices, UINT in_numIndices);

        void Free(const UINT* in_pIndices, UINT in_numIndices);

        // convenience functions on vectors
        void Allocate(std::vector<UINT>& out, UINT in_n) { out.resize(in_n); Allocate(out.data(), in_n); }
        void Free(std::vector<UINT>& in_indices) { Free(in_indices.data(), (UINT)in_indices.size()); in_indices.clear(); }

        // convenience functions for single values
        UINT Allocate();
        void Free(UINT i);

        UINT GetAvailable() const { return m_index; }
        UINT GetCapacity() const { return (UINT)m_heap.size(); }
        UINT GetAllocated() const { return GetCapacity() - GetAvailable(); }
    private:
        std::vector<UINT> m_heap;
        UINT m_index;
    };

    //==================================================
    // lock-free ringbuffer with single writer and single reader
    //==================================================
    class RingBuffer
    {
    public:
        RingBuffer(UINT in_size) : m_size(in_size) {}

        //-------------------------
        // writer methods
        //-------------------------
        UINT GetAvailableToWrite() const { return m_size - m_counter; } // how many could be written
        UINT GetWriteIndex(UINT in_offset = 0) const // can start writing here
        {
            ASSERT(in_offset < GetAvailableToWrite()); // fixme? probably an error because there's no valid index
            return (m_writerIndex + in_offset) % m_size;
        }
        void Allocate(UINT in_n = 1) // notify reader there's data ready
        {
            ASSERT((m_counter + in_n) <= m_size); // can't have more in-flight than m_size
            m_writerIndex += in_n;
            m_counter += in_n; // notify reader next n values ready to be used
        }

        //-------------------------
        // reader methods
        //-------------------------
        UINT GetReadyToRead() const { return m_counter; } // how many can be read
        UINT GetReadIndex(UINT in_offset = 0) const // can start reading here
        {
            ASSERT(in_offset < GetReadyToRead()); // fixme? probably an error because there's no valid index
            return (m_readerIndex + in_offset) % m_size;
        }
        void Free(UINT in_n = 1) // return entries to pool
        {
            ASSERT(m_counter >= in_n);
            m_readerIndex += in_n;
            m_counter -= in_n;
        }

    private:
        std::atomic<UINT> m_counter{ 0 };
        const UINT m_size;
        UINT m_writerIndex{ 0 };
        UINT m_readerIndex{ 0 };
    };

    //==================================================
    // flipped an idea around:
    // normally allocate space in a ringbuffer, write data, notify reader
    //          then reader uses the data, and notifies writer when done
    // instead, data is pre-populated with indices 0,1,2, etc.
    // producer writes nothing, just moves count forward
    // consumer writes indices of UpdateLists that are done, which may occur out-of-order
    //==================================================
    class AllocatorMT
    {
    public:
        AllocatorMT(UINT in_maxNumElements);
        virtual ~AllocatorMT();

        // assumes caller is doing due-diligence to allocate destination appropriately and check availability before calling
        void Allocate(UINT* out_pIndices, UINT in_numIndices);

        void Free(const UINT* in_pIndices, UINT in_numIndices);

        // convenience functions on vectors
        void Allocate(std::vector<UINT>& out, UINT in_n) { out.resize(in_n); Allocate(out.data(), in_n); }
        void Free(std::vector<UINT>& in_indices) { Free(in_indices.data(), (UINT)in_indices.size()); in_indices.clear(); }

        // convenience functions for single values
        UINT Allocate();
        void Free(UINT i);

        UINT GetAvailable() const { return m_ringBuffer.GetAvailableToWrite(); }
        UINT GetCapacity() const { return (UINT)m_indices.size(); }
        UINT GetAllocated() const { return m_ringBuffer.GetReadyToRead(); }
    private:
        std::vector<UINT> m_indices;
        RingBuffer m_ringBuffer;
    };
}
