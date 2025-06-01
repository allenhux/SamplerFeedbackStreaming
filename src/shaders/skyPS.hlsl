//==============================================================
// Copyright © Intel Corporation
//
// SPDX-License-Identifier: MIT
// =============================================================

#include "terrainVS.hlsl"
#include "GetLodVisualizationColor.h"

Texture2D g_streamingTexture : register(t0);

Buffer<uint> g_minmipmap: register(t1);

SamplerState g_sampler : register(s0);

//-----------------------------------------------------------------------------
// shader
//-----------------------------------------------------------------------------
float4 ps(VS_OUT input) : SV_TARGET0
{
    // the CPU provides a buffer that describes the min mip that has been
    // mapped into the streaming texture.
    int2 uv = input.tex * g_minmipmapDim;
    uint index = g_minmipmapOffset + uv.x + (uv.y * g_minmipmapDim.x);
    uint mipLevel = g_minmipmap.Load(index);

    // clamp the streaming texture to the mip level specified in the min mip map
    float3 color = g_streamingTexture.Sample(g_sampler, input.tex, 0, mipLevel).rgb;
    color = pow(color, 1.518f);

    // returns 0xff if no associated min mip, that is, no texel was touched last frame
    if ((g_visualizeFeedback) && (mipLevel < 16))
    {
        color = lerp(color, GetLodVisualizationColor(mipLevel), 0.3f);
    }

    return float4(color, 1.0f);
}
