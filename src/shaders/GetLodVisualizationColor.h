//==============================================================
// Copyright © Intel Corporation
//
// SPDX-License-Identifier: MIT
// =============================================================

#ifdef __cplusplus
#pragma once
struct float3
{
	float v[3];
	float3(float x, float y, float z) { v[0] = x; v[1] = y; v[2] = z; }
	float operator[](int i) { return v[i]; }
};
inline float3 GetLodVisualizationColor(int in_level)
#else
float3 GetLodVisualizationColor(int in_level)
#endif

//-----------------------------------------------------------------------------
// colors to visualize mip usage
//-----------------------------------------------------------------------------
{
	static float3 lut[] =
	{
	{ 1, 1, 1 },             // white
	{ 1, 0.25f, 0.25f },     // light red
	{ 1, 0.647f, 0.1f },     // orange
	{ 1, 1, 0.25f },         // light yellow
	{ 0.25f, 1, 0.25f },     // light green
	{ 0.10f, 0.3f, 1 },      // light blue
	{ 0.294f, 0.1f, 0.51f }, // indigo
	{ 0.5f, 0.1f, 1 },       // violet
	{ 1, 0.25f, 1 },         // light magenta
	{ 0.25f, 1, 1 },         // light cyan
	{ 0.53f, 0.25f, 0.11f },
	{ 0.8f, 0.48f, 0.53f},
	{ 0.64f, 0.8f, 0.48f },
	{ 0.48f, 0.75f, 0.8f },
	{ 0.5f, 0.25f, 0.75f },
	{ 0.99f, 0.68f, 0.42f },
	};
	if (in_level > 15)
	{
		return float3(.3f, .4f, .2f); // olive-ish green
	}
	else
	{
		return lut[in_level];
	}
}
