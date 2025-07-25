//==============================================================
// Copyright � Intel Corporation
//
// SPDX-License-Identifier: MIT
// =============================================================

#pragma once

#include <vector>
#include <set>
#include <synchapi.h>
#pragma comment(lib, "Synchronization.lib")

#include "DebugHelper.h"

namespace SFS
{
    //==================================================
    // multiple threads may wait on this flag, which may be set by any number of threads
    // use this in the case where it is acceptable to be woken spuriously
    //==================================================
    class SynchronizationFlag
    {
    public:
        void Set()
        {
            m_flag = true;
            WakeByAddressAll(&m_flag);
        }

        void Wait()
        {
            // note: msdn recommends verifying that the value really changed, but we're not.
            bool undesiredValue = false;
            WaitOnAddress(&m_flag, &undesiredValue, sizeof(bool), INFINITE);
            m_flag = false;
        };
    private:
        bool m_flag{ false };
    };

    //==================================================
    // Spin Lock using expensive wait
    //==================================================
    class SpinLock
    {
    public:
        void Acquire()
        {
            const std::uint32_t desired = 1;
            bool waiting = true;

            while (waiting)
            {
                std::uint32_t expected = 0;
                waiting = !m_lock.compare_exchange_weak(expected, desired);
            }
        }

        void Release()
        {
            ASSERT(0 != m_lock);
            m_lock = 0;
        }

        bool TryAcquire()
        {
            std::uint32_t expected = 0;
            const std::uint32_t desired = 2; //  different value useful for debugging
            return m_lock.compare_exchange_weak(expected, desired);
        }
    private:
        std::atomic<uint32_t> m_lock{ 0 };
    };

    //==================================================
    // Efficient Lock using Sync Flag
    //==================================================
    class Lock
    {
    public:
        void Acquire()
        {
            constexpr std::uint32_t desired = 1;

            std::uint32_t expected = 0;
            while (!m_lock.compare_exchange_weak(expected, desired))
            {
                UINT32 undesiredValue = 1;
                WaitOnAddress(&m_lock, &undesiredValue, sizeof(UINT32), INFINITE);
                expected = 0;
            }
        }

        void Release()
        {
            ASSERT(0 != m_lock);
            m_lock = 0;
            WakeByAddressAll(&m_lock);
        }

        bool TryAcquire()
        {
            std::uint32_t expected = 0;
            const std::uint32_t desired = 1;
            return m_lock.compare_exchange_weak(expected, desired);
        }
    private:
        std::atomic<UINT32> m_lock{ 0 };
    };
}
