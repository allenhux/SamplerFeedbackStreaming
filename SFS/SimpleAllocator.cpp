//==============================================================
// Copyright © Intel Corporation
//
// SPDX-License-Identifier: MIT
// =============================================================

#include "pch.h"

#include "SimpleAllocator.h"

//-----------------------------------------------------------------------------
// allocates simply by increasing/decreasing an index into an array of available indices
//-----------------------------------------------------------------------------
SFS::SimpleAllocator::SimpleAllocator(UINT in_maxNumElements) :
    m_index(0), m_heap(in_maxNumElements)
{
    for (auto& i : m_heap)
    {
        i = m_index;
        m_index++;
    }
}

SFS::SimpleAllocator::~SimpleAllocator()
{
#ifdef _DEBUG
    ASSERT(m_index == (UINT)m_heap.size());
    // verify all indices accounted for and unique
    std::sort(m_heap.begin(), m_heap.end());
    for (UINT i = 0; i < (UINT)m_heap.size(); i++)
    {
        ASSERT(i == m_heap[i]);
    }
#endif
}

//-----------------------------------------------------------------------------
// like above, but expects caller to have checked availability first and provided a safe destination
//-----------------------------------------------------------------------------
void SFS::SimpleAllocator::Allocate(UINT* out_pIndices, UINT in_numIndices)
{
    ASSERT(m_index >= in_numIndices);
    m_index -= in_numIndices;
    memcpy(out_pIndices, &m_heap[m_index], in_numIndices * sizeof(UINT));
}

UINT SFS::SimpleAllocator::Allocate()
{
    ASSERT(m_index > 0);
    m_index--;
    return m_heap[m_index];
}

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
void SFS::SimpleAllocator::Free(const UINT* in_pIndices, UINT in_numIndices)
{
    ASSERT(in_numIndices);
    ASSERT((m_index + in_numIndices) <= (UINT)m_heap.size());
    memcpy(&m_heap[m_index], in_pIndices, sizeof(UINT) * in_numIndices);
    m_index += in_numIndices;
}

void SFS::SimpleAllocator::Free(UINT i)
{
    ASSERT(m_index < (UINT)m_heap.size());
    m_heap[m_index] = i;
    m_index++;
}

//-----------------------------------------------------------------------------
// uses a lockless ringbuffer so allocate can be on a different thread than free
//-----------------------------------------------------------------------------
SFS::AllocatorMT::AllocatorMT(UINT in_numElements) :
    m_ringBuffer(in_numElements), m_indices(in_numElements)
{
    for (UINT i = 0; i < in_numElements; i++)
    {
        m_indices[i] = i;
    }
}

SFS::AllocatorMT::~AllocatorMT()
{
#ifdef _DEBUG
    ASSERT(0 == GetAllocated());
    // verify all indices accounted for and unique
    std::sort(m_indices.begin(), m_indices.end());
    for (UINT i = 0; i < (UINT)m_indices.size(); i++)
    {
        ASSERT(i == m_indices[i]);
    }
#endif
}

//-----------------------------------------------------------------------------
// multi-threaded allocator (single allocator, single releaser)
//-----------------------------------------------------------------------------
void SFS::AllocatorMT::Allocate(UINT* out_pIndices, UINT in_numIndices)
{
    ASSERT(m_ringBuffer.GetAvailableToWrite() >= in_numIndices);
    for (UINT i = 0; i < in_numIndices; i++)
    {
        out_pIndices[i] = m_indices[m_ringBuffer.GetWriteIndex(i)];
    }
    m_ringBuffer.Allocate(in_numIndices);
}

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
void SFS::AllocatorMT::Free(const UINT* in_pIndices, UINT in_numIndices)
{
    for (UINT i = 0; i < in_numIndices; i++)
    {
        m_indices[m_ringBuffer.GetReadIndex(i)] = in_pIndices[i];
    }
    m_ringBuffer.Free(in_numIndices);
}

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
UINT SFS::AllocatorMT::Allocate()
{
    UINT baseIndex = m_ringBuffer.GetWriteIndex();
    UINT i = m_indices[baseIndex];
    m_ringBuffer.Allocate();
    return i;
}

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
void SFS::AllocatorMT::Free(UINT i)
{
    UINT baseIndex = m_ringBuffer.GetReadIndex();
    m_indices[baseIndex] = i;
    m_ringBuffer.Free();
}
