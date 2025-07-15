//==============================================================
// Copyright © Intel Corporation
//
// SPDX-License-Identifier: MIT
// =============================================================

#pragma once

#include <string>
#include <cstdint>

#include "TerrainGenerator.h"
#include "SamplerFeedbackStreaming.h"

struct CommandLineArgs
{
	SFSManagerDesc m_sfsParams;
	TerrainGenerator::Params m_terrainParams;

	enum class PreferredArchitecture
	{
		NONE = 0,
		DISCRETE,
		INTEGRATED
	};
	PreferredArchitecture m_preferredArchitecture{ PreferredArchitecture::NONE };
	std::wstring m_adapterDescription;  // e.g. "intel", will pick the GPU with this substring in the adapter description (not case sensitive)

	std::wstring m_terrainTexture;
	std::wstring m_skyTexture;
	std::wstring m_earthTexture;
	std::wstring m_mediaDir;

	bool m_vsyncEnabled{ false };
	UINT m_windowWidth{ 1280 };
	UINT m_windowHeight{ 800 };
	UINT m_sampleCount{ 4 };
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
	int m_numObjects{ 0 };
	UINT m_anisotropy{ 16 };          // sampler anisotropy
	bool m_lightFromView{ false };    // light direction is look direction, useful for demos

	float m_maxGpuFeedbackTimeMs{ 10.0f };
	bool m_addAliasingBarriers{ false }; // adds a barrier for each streaming resource: alias(nullptr, pResource)

	UINT m_reservedMemoryGB{ 128 };
	UINT m_sfsHeapSizeMB{ 1024 };
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

	// planet parameters
	UINT m_sphereLong{ 128 }; // # steps vertically. must be even
	UINT m_sphereLat{ 111 };  // # steps around. must be odd
};
