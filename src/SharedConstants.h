//==============================================================
// Copyright © Intel Corporation
//
// SPDX-License-Identifier: MIT
// =============================================================

#pragma once

namespace SharedConstants
{
    // rendering properties
    static constexpr DXGI_FORMAT SWAP_CHAIN_FORMAT = DXGI_FORMAT_R8G8B8A8_UNORM;
    static constexpr DXGI_FORMAT DEPTH_FORMAT = DXGI_FORMAT_D32_FLOAT;
    static constexpr UINT SWAP_CHAIN_BUFFER_COUNT = 2;

    // scene properties
    static constexpr UINT SPHERE_RADIUS = 50;
    static constexpr UINT SPHERE_SPACING = 1; // percent, min gap size between planets
    static constexpr UINT MAX_SPHERE_SCALE = 15; // spheres can be up to this * sphere_scale in size

    static constexpr UINT NUM_SPHERE_LEVELS_OF_DETAIL = 6;
    static constexpr UINT SPHERE_LOD_BIAS = 10; // pixels per triangle
};
