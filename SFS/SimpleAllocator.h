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
        UINT m_index{ 0 };
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
        UINT GetWritableCount() const { return m_size - m_counter; } // how many could be written
        UINT GetWriteIndex(UINT in_offset = 0) const // can start writing here
        {
            ASSERT(in_offset < GetWritableCount()); // fixme? probably an error because there's no valid index
            return (m_writerIndex + in_offset) % m_size;
        }
        void Commit(UINT in_n = 1) // notify reader there's data ready
        {
            ASSERT((m_counter + in_n) <= m_size); // can't have more in-flight than m_size
            m_writerIndex += in_n;
            m_counter += in_n; // notify reader next n values ready to be used
        }

        //-------------------------
        // reader methods
        //-------------------------
        UINT GetReadableCount() const { return m_counter; } // how many can be read
        UINT GetReadIndex(UINT in_offset = 0) const // can start reading here
        {
            ASSERT(in_offset < GetReadableCount()); // fixme? probably an error because there's no valid index
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
    // Terminology:
    //    Writer writes. It then calls "Commit" to notify there are items to read
    //    Reader reads. It then calls "Free" to notify those items have been read
    //==================================================
    template<typename T> class AllocatorMT
    {
    public:
        AllocatorMT(UINT in_numElements) : m_ringBuffer(in_numElements), m_values(in_numElements) {}
        virtual ~AllocatorMT() {}

        // assumes caller is doing due-diligence to allocate destination appropriately and check availability before calling
        void WriteCommit(T* out_pValues, UINT in_numValues);
        void Commit(UINT in_numToCommit = 1);
        
        void Free(const T* in_pValues, UINT in_numValues); // write values to readable region before freeing
        void Free(UINT in_numToFree = 1); // free a range of values [0..n)

        // convenience functions on vectors
        void Commit(const std::vector<T>& out) { Commit(out.data(), (UINT)out.size()); }
        void Free(std::vector<T>& in_values) { Free(in_values.data(), (UINT)in_values.size()); in_values.clear(); }

        // for writer to write (or read)
        T& GetWriteableValue(UINT i = 0) { return m_values[m_ringBuffer.GetWriteIndex(i)]; }
        // for reader to read (or write)
        T& GetReadableValue(UINT i = 0) { return m_values[m_ringBuffer.GetReadIndex(i)]; }

        // free value by index into readable set
        // WARNING: data order is not preserved.
        void FreeByIndex(UINT in_index);

        UINT GetWritableCount() const { return m_ringBuffer.GetWritableCount(); }
        UINT GetCapacity() const { return (UINT)m_values.size(); }
        UINT GetReadableCount() const { return m_ringBuffer.GetReadableCount(); }
    protected:
        std::vector<T> m_values;
        RingBuffer m_ringBuffer;
    };

    template<typename T> void AllocatorMT<T>::WriteCommit(T* out_pValues, UINT in_numValues)
    {
        for (UINT i = 0; i < in_numValues; i++)
        {
            m_values[m_ringBuffer.GetWriteIndex(i)] = out_pValues[i];
        }
        m_ringBuffer.Commit(in_numValues);
    }

    template<typename T> void AllocatorMT<T>::Free(const T* in_pValues, UINT in_numValues)
    {
        for (UINT i = 0; i < in_numValues; i++)
        {
            m_values[m_ringBuffer.GetReadIndex(i)] = in_pValues[i];
        }
        m_ringBuffer.Free(in_numValues);
    }

    template<typename T> void AllocatorMT<T>::Commit(UINT in_numToCommit)
    {
        m_ringBuffer.Commit(in_numToCommit);
    }

    template<typename T> void AllocatorMT<T>::Free(UINT in_numToFree)
    {
        m_ringBuffer.Free(in_numToFree);
    }

    template<typename T> void AllocatorMT<T>::FreeByIndex(UINT in_index)
    {
        m_values[m_ringBuffer.GetReadIndex(in_index)] = m_values[m_ringBuffer.GetReadIndex(0)];
        m_ringBuffer.Free();
    }
}
