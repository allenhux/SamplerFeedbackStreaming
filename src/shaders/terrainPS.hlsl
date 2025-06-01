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

//-------------------------------------------------------------------------
//-------------------------------------------------------------------------
float3 evaluateLight(in float3 normal, in float3 pos, in float3 tex)
{
    float ambient = 0.1f;

    // diffuse
    // directional light. from the point toward the light is the opposite direction.
    float3 pointToLight = -g_lightDir.xyz;
    float diffuse = saturate(dot(pointToLight, normal));
    float3 color = (ambient + (diffuse * g_lightColor.xyz)) * tex;

    // specular
    float3 eyeToPoint = normalize(pos - g_eyePos.xyz);
    float3 reflected = normalize(reflect(eyeToPoint, normal));
    float specDot = saturate(dot(reflected, pointToLight));
    float3 specular = pow(specDot, g_specularColor.a) * g_specularColor.xyz;

    // gamma
    color = pow(color + specular, 1.0f / 1.5f);
    color = saturate(color);

    return color;
}

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
    color = evaluateLight(input.normal, input.worldPos, color);

    if ((g_visualizeFeedback) && (mipLevel < 16))
    {
        color = lerp(color, GetLodVisualizationColor(mipLevel), 0.3f);
    }

    return float4(color, 1.0f);
}
