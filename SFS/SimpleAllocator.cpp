//==============================================================
// Copyright © Intel Corporation
//
// SPDX-License-Identifier: MIT
// =============================================================

#include "pch.h"

#include <numeric>
#include "SimpleAllocator.h"

//-----------------------------------------------------------------------------
// allocates simply by increasing/decreasing an index into an array of available indices
//-----------------------------------------------------------------------------
SFS::SimpleAllocator::SimpleAllocator(UINT in_maxNumElements) :
    m_index(in_maxNumElements), m_heap(in_maxNumElements)
{
    std::iota(m_heap.begin(), m_heap.end(), 0);
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
