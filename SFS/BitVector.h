//==============================================================
// Copyright © Intel Corporation
//
// SPDX-License-Identifier: MIT
// =============================================================

#pragma once
#include <cstdint>

//==================================================
// vector of bits packed into standard type
// expected types: uint64_t, uint32_t, uint16_t, uint8_t
//==================================================
template<typename T> class BitVector
{
public:
    BitVector(size_t in_size, uint32_t in_initialValue = 0) : m_size(in_size),
        m_bits(ComputeArraySize(in_size), in_initialValue ? T(-1) : 0) {}
    uint32_t Get(uint32_t i) { return uint32_t(1) & (GetVector(i) >> (i & BIT_MASK)); }
    void Set(uint32_t i) { GetVector(i) |= GetBit(i); }
    void Clear(uint32_t i) { GetVector(i) &= ~GetBit(i); }
    size_t size() const { return m_size; }
    void resize(size_t in_size)
    {
        m_bits.resize(ComputeArraySize(in_size));
        m_size = in_size;
    }

    class Accessor // helper class so we can replace std::vector<>
    {
    public:
        operator uint32_t() const { return m_b.Get(m_i); }
        void operator=(uint32_t b) { b ? m_b.Set(m_i) : m_b.Clear(m_i); }
    private:
        Accessor(BitVector& b, uint32_t i) : m_b(b), m_i(i) {}
        BitVector& m_b;
        const uint32_t m_i{ 0 };
        friend class BitVector;
    };
    Accessor operator[](uint32_t i) { return Accessor(*this, i); }
private:
    static size_t ComputeArraySize(size_t in_size) { return (in_size + BIT_MASK) >> BIT_SHIFT; }
    size_t m_size{ 0 };
    // use the upper bits of the incoming index to get the array index (shift by log2, e.g. byte type is 3 bits)
    static constexpr uint32_t BIT_SHIFT = (8 == sizeof(T)) ? 6 : (4 == sizeof(T)) ? 5 : (2 == sizeof(T)) ? 4 : 3;
    static constexpr uint32_t BIT_MASK = (T(1) << BIT_SHIFT) - 1;
    std::vector<T> m_bits;
    uint32_t GetBit(uint32_t i) { return T(1) << (i & BIT_MASK); }
    T& GetVector(uint32_t i) { return m_bits[i >> BIT_SHIFT]; }
};
