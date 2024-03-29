//*********************************************************
//
// Copyright 2020 Intel Corporation 
//
// Permission is hereby granted, free of charge, to any 
// person obtaining a copy of this software and associated 
// documentation files(the "Software"), to deal in the Software 
// without restriction, including without limitation the rights 
// to use, copy, modify, merge, publish, distribute, sublicense, 
// and/or sell copies of the Software, and to permit persons to 
// whom the Software is furnished to do so, subject to the 
// following conditions :
// The above copyright notice and this permission notice shall 
// be included in all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, 
// EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF 
// MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
// NONINFRINGEMENT.IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT 
// HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, 
// WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, 
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER 
// DEALINGS IN THE SOFTWARE.
//
//*********************************************************
#pragma once
#include <cstdint>

//==================================================
// vector of bits packed into standard type
// expected types: uint64_t, uint32_t, uint16_t, uint8_t
//==================================================
template<typename T> class BitVector
{
public:
    BitVector(size_t in_size, uint32_t in_initialValue = 0) : m_size(in_size), m_bits((in_size + BIT_MASK) >> BIT_SHIFT, in_initialValue ? T(-1) : 0) {}
    uint32_t Get(uint32_t i) { return uint32_t(1) & (GetVector(i) >> (i & BIT_MASK)); }
    void Set(uint32_t i) { GetVector(i) |= GetBit(i); }
    void Clear(uint32_t i) { GetVector(i) &= ~GetBit(i); }
    size_t size() { return m_size; }

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
    const size_t m_size{ 0 };
    // use the upper bits of the incoming index to get the array index (shift by log2, e.g. byte type is 3 bits)
    static constexpr uint32_t BIT_SHIFT = (8 == sizeof(T)) ? 6 : (4 == sizeof(T)) ? 5 : (2 == sizeof(T)) ? 4 : 3;
    static constexpr uint32_t BIT_MASK = (T(1) << BIT_SHIFT) - 1;
    std::vector<T> m_bits;
    uint32_t GetBit(uint32_t i) { return T(1) << (i & BIT_MASK); }
    T& GetVector(uint32_t i) { return m_bits[i >> BIT_SHIFT]; }
};
