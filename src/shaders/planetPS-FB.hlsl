//*********************************************************
// Copyright 2024 Allen Hux
//
// SPDX-License-Identifier: MIT
//*********************************************************

#include "planetPS.hlsl"

FeedbackTexture2D<SAMPLER_FEEDBACK_MIN_MIP> g_feedback : register(u0);

float4 psFB(VS_OUT input) : SV_TARGET0
{
    float2 tex = ComputeUV(input.modelPos); // FIXME: reuse tex
    g_feedback.WriteSamplerFeedback(g_streamingTexture, g_sampler, tex);

    return ps(input);
}
