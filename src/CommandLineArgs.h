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

#include <string>
#include <cstdint>

#include "TerrainGenerator.h"

struct CommandLineArgs
{
    enum class PreferredArchitecture
    {
        NONE = 0,
        DISCRETE,
        INTEGRATED
    };
    PreferredArchitecture m_preferredArchitecture{ PreferredArchitecture::NONE };
    std::wstring m_adapterDescription;  // e.g. "intel", will pick the GPU with this substring in the adapter description (not case sensitive)

    bool m_useDirectStorage{ true };
    UINT m_stagingSizeMB{ 128 };         // size of the staging buffer for DirectStorage or reference streaming code

    std::wstring m_terrainTexture;
    std::wstring m_skyTexture;
    std::wstring m_earthTexture;
    std::wstring m_mediaDir;

    bool m_vsyncEnabled{ false };
    UINT  m_windowWidth{ 1280 };
    UINT  m_windowHeight{ 800 };
    UINT  m_sampleCount{ 4 };
    float m_lodBias{ 0 };

    bool m_startFullScreen{ false };
    bool m_cameraRollerCoaster{ false };
    bool m_cameraPaintMixer{ false };

    bool m_updateEveryObjectEveryFrame{ false };
    float m_animationRate{ 0 };
    float m_cameraAnimationRate{ 0 }; // puts camera on a track and moves it every frame

    bool m_showUI{ true };
    bool m_uiModeMini{ false };       // just bandwidth and heap occupancy
    bool m_showFeedbackMaps{ true };
    bool m_visualizeMinMip{ false };
    UINT m_maxNumObjects{ 1000 };     // number of descriptors to allocate for scene
    int m_numSpheres{ 0 };
    UINT m_anisotropy{ 16 };          // sampler anisotropy
    bool m_lightFromView{ false };    // light direction is look direction, useful for demos

    float m_maxGpuFeedbackTimeMs{ 10.0f };
    bool m_addAliasingBarriers{ false }; // adds a barrier for each streaming resource: alias(nullptr, pResource)
    UINT m_streamingHeapSize{ 16384 }; // in # of tiles, not bytes
    UINT m_numHeaps{ 1 };

    // e.g. -timingStart 5 -timingEnd 25 writes a CSV showing the time to run 20 frames between those 2 times
    // ignored if end frame == 0
    // -timingStart 3 -timingEnd 3 will time no frames
    // -timingStart 3 -timingEnd 4 will time just frame 3 (1 frame, not frame 4)
    UINT m_timingStartFrame{ 0 };
    UINT m_timingStopFrame{ 0 };
    std::wstring m_timingFrameFileName; // where to write per-frame statistics
    std::wstring m_exitImageFileName;   // write an image on exit
    bool m_waitForAssetLoad{ false };   // wait for assets to load before progressing frame #

    //-------------------------------------------------------
    // state that is not settable from command line:
    //-------------------------------------------------------
    std::vector<std::wstring> m_textures; // textures for things other than the terrain

    UINT m_maxTileUpdatesPerApiCall{ 512 }; // max #tiles (regions) in call to UpdateTileMappings()
    bool m_enableTileUpdates{ true }; // toggle enabling tile uploads/evictions
    int  m_visualizationBaseMip{ 0 };
    bool m_showFeedbackMapVertical{ false };

    enum VisualizationMode : int
    {
        TEXTURE,
        MIPLEVEL,
        RANDOM
    };
    int  m_dataVisualizationMode{ VisualizationMode::TEXTURE }; // instead of texture, show random colors per tile or color = mip level

    bool m_visualizeFrustum{ false };
    bool m_showFeedbackViewer{ true }; // toggle just the raw feedback view in the feedback viewer
    UINT m_statisticsNumFrames{ 30 };
    bool m_cameraUpLock{ true };       // navigation locks "up" to be y=1
    UINT m_numStreamingBatches{ 128 }; // number of in-flight batches of updates (UpdateLists)
    UINT m_minNumUploadRequests{ 2000 }; // milliseconds. heuristic to reduce frequency of Submit() calls
    UINT m_evictionDelay{ 1 }; // frames before tile eviction. internally, at least swap chain size to avoid artifacts

    // planet parameters
    UINT m_sphereLong{ 128 }; // # steps vertically. must be even
    UINT m_sphereLat{ 111 };  // # steps around. must be odd

    TerrainGenerator::Params m_terrainParams;

    bool m_captureTrace{ false }; // capture a trace file of tile uploads
    int m_threadPriority{ 0 }; // applies to internal threads
};
