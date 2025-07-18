//==============================================================
// Copyright � Intel Corporation
//
// SPDX-License-Identifier: MIT
// =============================================================

struct VS_OUT
{
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
    nointerpolation float mipLevel : OUTPUT;
};

cbuffer cb0
{
    float4 g_viewPosition;
    float g_gap;
    float g_visBaseMip;
    bool g_vertical;
};

VS_OUT vs(uint vertexID : SV_VertexID)
{
    // remember: normalized screen space is [-1,-1] [1,1], so everything is doubled

    float level = vertexID >> 2;
    vertexID &= 3;

    // this forms a rect 0,0 2,0 0,2 2,2 : tri-strip starting clockwise
    //float2 grid = float2((vertexID & 1) << 1, vertexID & 2);

    // this forms a rect 0,0 0,2 2,0 2,2 : tri-strip starting counter-clockwise
    float2 grid = float2(vertexID & 2, (vertexID & 1) << 1);

    float width = g_viewPosition.z;
    float height = g_viewPosition.w;

    VS_OUT output;

    // scale window and translate to bottom left of screen
    output.pos = float4((grid * float2(width, height)) + float2(-1.0f, -1.0), 0.0f, 1.0f);

    // horizontal or vertical arrangement
    if (g_vertical)
    {
        height -= g_gap;

        output.pos.x += 2 * g_viewPosition.x;
        output.pos.y += 2 * (g_viewPosition.y - (height * level));
    }
    else
    {
        width += g_gap;

        output.pos.x += 2 * (g_viewPosition.x + (width * level));
        output.pos.y += 2 * g_viewPosition.y;
    }

    // uv from 0,0 to 1,1
    output.uv.xy = 0.5f * grid;
    output.uv.y = 1 - output.uv.y; // the window has 0,0 as bottom-left. the image u,v should have v = 0 in the top-left.
    output.mipLevel = g_visBaseMip + level;

    return output;
}

Texture2D g_texture2D : register(t0);
SamplerState g_sampler : register(s0);

float4 ps(VS_OUT input) : SV_Target
{
    float4 diffuse = g_texture2D.SampleLevel(g_sampler, input.uv.xy, input.mipLevel);

    return float4(diffuse.xyz, 1.0f);
}
