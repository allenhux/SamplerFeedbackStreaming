//==============================================================
// Copyright © Intel Corporation
//
// SPDX-License-Identifier: MIT
// =============================================================

#pragma once

#include "TimeTracing.h"
#include "Timer.h"

#include "CommandLineArgs.h"

enum class RenderEvents
{
    PreBeginFrame,           // call before SFSManager::BeginFrame
    PreEndFrame,             // call before SFSManager::EndFrame
    PostEndFrame,            // call after SFSManager::EndFrame (measures API call time)
    PreWaitNextFrame,         // how long between ExecuteCommandLists and next frame end?
    PostWaitNextFrame,        // after wait() on fence
    Num
};

enum class UpdateEvents
{
    Begin,
    End,
    Num
};

//=============================================================================
//=============================================================================
class FrameEventTracing : public WriteCSV
{
public:
    using RenderEventList = TimeTracing<RenderEvents>;
    using UpdateEventList = TimeTracing<UpdateEvents>;

    FrameEventTracing(const std::wstring& in_fileName, const std::wstring& in_adapterDescription);
    virtual ~FrameEventTracing() {}

    // pre-allocate the right amount of memory as an optimization when collecting statistics
    void Reserve(UINT in_numExpectedEvents) { m_events.reserve(in_numExpectedEvents); }

    void Append(
        const RenderEventList& in_renderList,
        const UpdateEventList& in_updateList,
        UINT in_numUploads, UINT in_numEvictions,
        float in_cpuProcessFeedbackTime,
        float in_gpuProcessFeedbackTime,
        UINT in_numFeedbackResolves, UINT in_numSubmits)
    {
        m_events.push_back({
            in_renderList.GetLatest(),
            in_updateList.GetLatest(),
            in_numUploads, in_numEvictions,
            in_cpuProcessFeedbackTime, in_gpuProcessFeedbackTime,
            in_numFeedbackResolves, in_numSubmits });
    }

    void WriteEvents(HWND in_hWnd, const CommandLineArgs& in_args);
private:
    Timer m_timer;

    struct FrameEvents
    {
        TimeTracing<RenderEvents>::Accessor m_renderTimes;
        TimeTracing<UpdateEvents>::Accessor m_updateTimes;
        UINT m_numTileCopiesQueued;
        UINT m_numTilesEvicted;
        float m_cpuFeedbackTime;
        float m_gpuFeedbackTime;
        UINT m_numGpuFeedbackResolves;
        UINT m_numSubmits;
    };

    std::vector<FrameEvents> m_events;

    const std::wstring m_adapterDescription;
};

//=============================================================================
//=============================================================================
inline FrameEventTracing::FrameEventTracing(
    const std::wstring& in_fileName,
    const std::wstring& in_adapterDescription) :
    WriteCSV(in_fileName), m_adapterDescription(in_adapterDescription)
{
    // reserve a bunch of space
    m_events.reserve(1000);

    m_timer.Start();
}

inline void FrameEventTracing::WriteEvents(HWND in_hWnd, const CommandLineArgs& in_args)
{
    double totalTime = m_timer.Stop();

    RECT windowRect;
    GetClientRect(in_hWnd, &windowRect);

    *this
        << "\n" << GetCommandLineW() << "\n\n"
        << "WindowWidth/Height: " << windowRect.right - windowRect.left << " " << windowRect.bottom - windowRect.top << "\n"
        << "Adapter: " << m_adapterDescription << "\n"
        << "DS enabled: " << in_args.m_useDirectStorage << "\n"
        << "heap size: " << in_args.m_streamingHeapSize << "\n"
        << "num heaps: " << in_args.m_numHeaps << "\n"
        << "paintmixer: " << in_args.m_cameraPaintMixer << "\n"
        << "lod bias: " << in_args.m_lodBias << "\n"
        << "aliasing barriers: " << in_args.m_addAliasingBarriers << "\n"
        << "media dir: " << in_args.m_mediaDir << "\n";

    *this << "\nTimers (ms)\n"
        << "-----------------------------------------------------------------------------------------------------------\n"
        << "cpu_draw SFSM::EndFrame exec_cmd_list wait_present total_frame_time evictions_completed copies_completed cpu_feedback feedback_resolve num_resolves num_submits\n"
        << "-----------------------------------------------------------------------------------------------------------\n";

    for (auto& e : m_events)
    {
        float frameBegin = e.m_renderTimes.Get(RenderEvents::PreBeginFrame);
        float tumEndFrameBegin = e.m_renderTimes.Get(RenderEvents::PreEndFrame);
        float tumEndFrame = e.m_renderTimes.Get(RenderEvents::PostEndFrame);
        float waitOnFencesBegin = e.m_renderTimes.Get(RenderEvents::PreWaitNextFrame);
        float frameEnd = e.m_renderTimes.Get(RenderEvents::PostWaitNextFrame);

        *this
            << (tumEndFrameBegin - frameBegin) * 1000                // render thread drawing via DrawIndexInstanced(), etc.
            << " " << (tumEndFrame - tumEndFrameBegin) * 1000        // SFSM::EndFrame()
            << " " << (waitOnFencesBegin - tumEndFrame) * 1000       // ExecuteCommandLists()
            << " " << (frameEnd - waitOnFencesBegin) * 1000          // WaitForSingleObject()
            << " " << (frameEnd - frameBegin) * 1000                 // frame time

            << " " << e.m_numTilesEvicted // copies queued
            << " " << e.m_numTileCopiesQueued  // tile virtual->physical removed

            << " " << e.m_cpuFeedbackTime * 1000
            << " " << e.m_gpuFeedbackTime * 1000
            << " " << e.m_numGpuFeedbackResolves
            << " " << e.m_numSubmits

            << std::endl;
    }

    *this << "Total Time (s): " << totalTime << std::endl;
}
