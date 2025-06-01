//*********************************************************
// Copyright 2024 Allen Hux
//
// SPDX-License-Identifier: MIT
//*********************************************************

//-------------------------------------------------------------------------
// Constant buffers
//-------------------------------------------------------------------------
cbuffer ModelConstantData : register(b1)
{
    float4x4    g_combinedTransform;
    float4x4    g_worldTransform;

    int2   g_minmipmapDim;
    int    g_minmipmapOffset;
};

cbuffer FrameConstantData : register(b0)
{
    float4 g_eyePos;
    float4 g_lightDir;
    float4 g_lightColor;    // RGB
    float4 g_specularColor; // RGB + specular intensity
    bool g_visualizeFeedback;
};

//-------------------------------------------------------------------------
// draw the scene
//-------------------------------------------------------------------------

struct VS_IN
{
    float3 pos        : POS;
    float3 normal     : NORMAL;
};

struct VS_OUT
{
    float4 pos        : SV_POSITION;
    float3 normal     : NORMAL;
    float3 worldPos   : WORLDPOS;
    float3 modelPos   : MODELPOS;
};

VS_OUT vs(VS_IN input)
{
    VS_OUT result;
    result.pos = mul(g_combinedTransform, float4(input.pos, 1.0f));

    // rotate normal into light coordinate frame (world)
    result.normal = normalize(mul((float3x3)g_worldTransform, input.normal));

    // transform position into light coordinate frame (world)
    result.worldPos = mul(g_worldTransform, float4(input.pos, 1.0f)).xyz;

    result.modelPos = input.pos;
    return result;
}
