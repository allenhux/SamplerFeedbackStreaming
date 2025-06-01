//==============================================================
// Copyright © Intel Corporation
//
// SPDX-License-Identifier: MIT
// =============================================================

#pragma once

//==================================================
// auto t = AutoString("test: ", 3, "*", 2.75f, "\n");
//==================================================
class AutoString : public std::wstringstream
{
public:
    template <typename...Ts> AutoString(Ts...ts)
    {
        Join(ts...);
    }
private:
    void Join() {}
    template <typename T, typename...Ts> void Join(const T& t, const Ts& ... ts)
    {
        *this << t;
        Join(ts...);
    }
};

#ifdef _DEBUG
#include <assert.h>
#define ASSERT(X) assert(X)
template<typename ... Ts> void DebugPrint(const Ts& ... ts)
{
    AutoString autoString(ts...);
    autoString << std::endl;
    OutputDebugString(autoString.str().c_str());
}
inline void ThrowIfFailed(HRESULT hr) { assert(SUCCEEDED(hr)); }
#else
#define ASSERT(X)
#define DebugPrint(...)
#define ThrowIfFailed
#endif
