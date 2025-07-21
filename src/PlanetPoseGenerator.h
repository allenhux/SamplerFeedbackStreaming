//*********************************************************
// Copyright 2024 Allen Hux 
//
// SPDX-License-Identifier: MIT
//*********************************************************

#pragma once

#include <random>
#include <vector>
#include <DirectXMath.h>

// Generate positions and radii for a bunch of spheres
// The spheres will be packed as close together as we can get in reasonable runtime

class PlanetPoseGenerator
{
public:
    struct Settings
    {
        UINT numPoses{ 0 };
        float gap{ 0 };
        float minDistance{ 0 };
        float minRadius{ 0 };
        float maxRadius{ 0 };
    };
    PlanetPoseGenerator(const Settings& in_settings) : m_settings(in_settings) {}

    // returns universe size
    float GeneratePoses(std::vector<DirectX::XMVECTOR>& out_planetPose);
private:
    Settings m_settings;
    float m_universeSize{ 0 };
    float m_growSize{ 0 }; // rate of universe growth to fit more objects

    const UINT m_numTries{ 250 }; // more tries results in tighter packing, at the expense of more time

    struct Pos
    {
        DirectX::XMVECTOR pos;
        float radius;
    };
    struct Layer
    {
        std::vector<Pos> m_poses;
        float minDistance{ 0 };
        float maxDistance{ 0 };
    };
    std::default_random_engine m_gen;
    bool CheckFit(const Pos& pose, Layer* in_pLayer, float in_gap);
    float AddPose(Pos& pose, Layer* in_pLayer, Layer* pPreviousLayer);
};
