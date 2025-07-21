//==============================================================
// Copyright © Intel Corporation
//
// SPDX-License-Identifier: MIT
// =============================================================

#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

/*-----------------------------------------------------------------------------
MSDN:
    In general, the performance counter results are consistent across all processors in
    multi-core and multi-processor systems even when measured on different threads or processes

    How often does QPC roll over?
    Not less than 100 years from the most recent system boot, and potentially longer based
    on the underlying hardware timer used. For most applications, rollover isn't a concern.
-----------------------------------------------------------------------------*/

//=============================================================================
// return time in milliseconds
// follows coding example in MSDN, but converting to milliseconds instead of microseconds
// https://learn.microsoft.com/en-us/windows/win32/sysinfo/acquiring-high-resolution-time-stamps
// "To guard against loss-of-precision, we convert to microseconds *before* dividing by ticks-per-second."
//=============================================================================
class CpuTimer
{
public:
    CpuTimer() : m_performanceFrequency({ []() { LARGE_INTEGER l{};
    ::QueryPerformanceFrequency(&l); return l; }() }) {
    }
    static INT64 GetTicks() { LARGE_INTEGER i; QueryPerformanceCounter(&i); return i.QuadPart; }
    float GetMsSince(INT64 in_previousTicks) const { return GetMsFromDelta(GetTicks() - in_previousTicks); }
    float GetMsFromDelta(INT64 in_delta) const
    {
        return float(in_delta * m_ticksToMs) / float(m_performanceFrequency.QuadPart);
    }
private:
    const LARGE_INTEGER m_performanceFrequency;
    static constexpr UINT m_ticksToMs{ 1000 };
};

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
class AverageOver
{
public:
    AverageOver(UINT in_numFrames = 30) : m_index(0), m_sum(0), m_values(in_numFrames, 0), m_numValues(0) {}
    void Update(float in_value)
    {
        if (m_numValues < m_values.size())
        {
            m_numValues++;
        }
        // clamp incoming values to 0
        if (in_value < 0) in_value = 0;
        m_sum = m_sum + in_value - m_values[m_index];
        m_values[m_index] = in_value;
        m_index = (m_index + 1) % (UINT)m_values.size();
    }
    float Get() const { return m_sum / float(m_numValues); }
private:
    std::vector<float> m_values;
    UINT m_numValues; // less than size() for first few values
    UINT m_index;
    float m_sum;
};

/*======================================================
Usage:
Either: call once every loop with Update(), e.g. for average frametime
Or: call in pairs Start()...Update() to average a region
======================================================*/
class TimerAverageOver
{
public:
    TimerAverageOver(UINT in_numFrames = 30) : m_averageOver(in_numFrames) {}

    void Start()
    {
        m_previousTime = m_timer.GetTicks();
    }

    void Update()
    {
        INT64 t = m_timer.GetTicks();
        float delta = m_timer.GetMsFromDelta(t - m_previousTime);
        m_previousTime = t;

        m_averageOver.Update(delta);
    }

    float GetMs() const
    {
        return m_averageOver.Get();
    }

private:
    TimerAverageOver(const TimerAverageOver&) = delete;
    TimerAverageOver(TimerAverageOver&&) = delete;
    TimerAverageOver& operator=(const TimerAverageOver&) = delete;
    TimerAverageOver& operator=(TimerAverageOver&&) = delete;

    CpuTimer m_timer;
    AverageOver m_averageOver;

    INT64 m_previousTime{ 0 };
};

// if something increases with each sample, e.g. time or # of transactions,
// keep the sample values. return the delta over the range
// e.g. if there are 128 samples, the delta will be from Sample(n) - Sample(n-128)
class TotalSince
{
public:
    TotalSince(UINT in_range = 30) : m_values(in_range, 0) {}

    // update with the latest total, e.g. the current time since the beginning in ticks
    void Update(UINT64 in_v)
    {
        m_values[m_index] = in_v;
        m_index = (m_index + 1) % m_values.size();
    }

    // add a delta since last time
    // internally uses a running sum
    void AddDelta(UINT64 in_v)
    {
        m_sum += in_v;
        Update(m_sum);
    }

    // delta between the last two samples
    UINT64 GetMostRecentDelta()
    {
        auto latest = (m_index + m_values.size() - 1) % m_values.size();
        auto prior = (m_index + m_values.size() - 2) % m_values.size();
        return m_values[latest] - m_values[prior];
    }

    // delta across the whole range
    UINT64 GetRange()
    {
        auto latest = (m_index + m_values.size() - 1) % m_values.size();
        return m_values[latest] - m_values[m_index];
    }

    // average across the range
    float GetAverage()
    {
        return (float)GetRange() / m_values.size();
    }

    UINT GetNumEntries() { return (UINT)m_values.size(); }
private:
    std::vector<UINT64> m_values;
    UINT m_index{ 0 };
    UINT64 m_sum{ 0 }; // if adding deltas, keep a running sum
};