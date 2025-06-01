//==============================================================
// Copyright © Intel Corporation
//
// SPDX-License-Identifier: MIT
// =============================================================

#include "terrainVS.hlsl"

VS_OUT skyVS(VS_IN input)
{
    VS_OUT result;
    result.pos = mul(g_combinedTransform, float4(input.pos, 1.0f));
    result.tex = input.tex;
    return result;
}
