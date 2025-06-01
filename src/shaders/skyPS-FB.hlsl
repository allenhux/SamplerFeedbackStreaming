//==============================================================
// Copyright © Intel Corporation
//
// SPDX-License-Identifier: MIT
// =============================================================

#include "skyPS.hlsl"

FeedbackTexture2D<SAMPLER_FEEDBACK_MIN_MIP> g_feedback : register(u0);

float4 psFB(VS_OUT input) : SV_TARGET0
{
    g_feedback.WriteSamplerFeedback(g_streamingTexture, g_sampler, input.tex.xy);

    return ps(input);
}
