//*********************************************************
//
// Copyright 2024 Allen Hux
//
//*********************************************************

#include "planetVS.hlsl"
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
// Compute UV from model position for sphere
//-----------------------------------------------------------------------------
float2 ComputeUV(float3 pos)
{    
    // radius of this latitude in x/y plane
    // for a sphere, 1 = sqrt(x^2 + y^2 + z^2)
    // since r^2 = x^2 + y^2, 1 = sqrt(r^2 + z^2) and 1^2 - z^2 = r^2
    float rSquared = 1.f - (pos.z * pos.z);

    float rcostheta = abs(pos.x);
    float rsintheta = abs(pos.y);

    // conceptually, cast a ray from the origin through the current position to the edge of a unit square
    // the length of that ray is the scale factor to project "here" to the edge of the square
    // use simple trig to get length: sin = opposite/hypotenuse or cos = adjacent/hypotenuse
    //      e.g. s * cos(theta) = 1, hence s = 1/cos. cos = pos.x / r, hence s = r/pos.x
    //      for theta 45..90 degrees, s = r/pos.y
    // note numerator (r) has been included in the lerp
    float rs = 1.f / max(rcostheta, rsintheta);

    // counteract scale so center appears undistorted:
    const float distortion = sqrt(2.0f) / 2.f;
    // quadratic interpolation of scale factor:
    float s = lerp(distortion, rs, rSquared * (1 - abs(pos.z)) * (1 - abs(pos.z)));

    float2 tex = float2(pos.x, pos.y) * s;
    tex = (1 + tex) * 0.5f;

    return tex;
}

//-----------------------------------------------------------------------------
// shader
//-----------------------------------------------------------------------------
float4 ps(VS_OUT input) : SV_TARGET0
{
    float2 tex = ComputeUV(input.modelPos);
    
    // the CPU provides a buffer that describes the min mip that has been
    // mapped into the streaming texture.
    int2 uv = tex * g_minmipmapDim;
    uint index = g_minmipmapOffset + uv.x + (uv.y * g_minmipmapDim.x);
    uint mipLevel = g_minmipmap.Load(index);

    // clamp the streaming texture to the mip level specified in the min mip map
    float3 color = g_streamingTexture.Sample(g_sampler, tex, 0, mipLevel).rgb;
    color = evaluateLight(input.normal, input.worldPos, color);

    if ((g_visualizeFeedback) && (mipLevel < 16))
    {
        color = lerp(color, GetLodVisualizationColor(mipLevel), 0.3f);
    }

    return float4(color, 1.0f);
}
