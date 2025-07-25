//==============================================================
// Copyright � Intel Corporation
//
// SPDX-License-Identifier: MIT
// =============================================================

#pragma once

#include "pch.h"

#include <vector>
#include <intsafe.h>
#include <DirectXMath.h>
#include "DebugHelper.h"

#include "TerrainGenerator.h" // for the shared vertex structure

//===========================================================
// Creates a sphere with u/v such that the image is mirrored in u
// more triangles are spent on the poles for a smooth curve
//===========================================================
namespace SphereGen
{
    using Vertex = TerrainGenerator::Vertex;

    struct Properties
    {
        UINT m_numLong{ 128 }; // must be even, or there will be a discontinuity in texture coordinates
        UINT m_numLat{ 111 }; // must be odd, so there's an equator
        float m_exponent{ 3.14159265358979f }; // exponential function puts more triangles at the poles
        bool m_mirrorU{ true }; // mirror texture around axis of sphere
        bool m_topBottom{ false }; // each hemisphere contains the texture, mirrored across the equator
    };

    inline void Create(std::vector<Vertex>& out_vertices, std::vector<UINT32>& out_indices, Properties in_props)
    {
        in_props.m_numLong &= ~1; // force even
        in_props.m_numLat |= 1;  // force odd

        // when UVs are mirrored or using the "top-bottom" mirror mode, the edge vertices don't have to be duplicated
        bool repeatEdge = !in_props.m_mirrorU;

        std::vector<Vertex> vertices;
        // compute sphere vertices
        // skip top and bottom rows (will just use single points)
        {
            float dLat = DirectX::XM_PI * 2.0f / in_props.m_numLong; // the number of vertical segments tells you how many horizontal steps to take
            float dz = 2.f / (in_props.m_numLat - 1);

            // du is uniform
            float du = 1.0f / float(in_props.m_numLong); // u from 0..1
            if (in_props.m_mirrorU) { du *= 2; }         // when mirroring, u from 0..2

            for (UINT longitude = 1; longitude < (in_props.m_numLat - 1); longitude++)
            {
                float z = dz * longitude;

                // logarithmic step in z produces smoother poles
                if (z < 1) { z = -1 + std::powf(z, in_props.m_exponent); }
                else { z = 1 - std::powf(2 - z, in_props.m_exponent); }

                // radius of this latitude in x/y plane
                float r = std::sqrtf(1 - (z * z));

                // v is constant for this latitude
                float v = std::acosf(-z) / DirectX::XM_PI;

                // create row of vertices
                for (UINT lat = 0; lat < in_props.m_numLong; lat++)
                {
                    float theta = lat * dLat;
                    float x = r * std::cosf(theta);
                    float y = r * std::sinf(theta);
                    DirectX::XMFLOAT3 pos = DirectX::XMFLOAT3(x, y, z);

                    DirectX::XMFLOAT2 uv;

                    // stretch texture across hemisphere. mirrored on other hemisphere.
                    if (in_props.m_topBottom)
                    {
                        uv = { pos.x, pos.y };

                        // radial scale
                        if (theta > DirectX::XM_PI) { theta = DirectX::XM_2PI - theta; }
                        if (theta > DirectX::XM_PIDIV2) { theta = DirectX::XM_PI - theta; }
                        if (theta > DirectX::XM_PIDIV4)
                        {
                            float s = std::cosf(DirectX::XM_PIDIV4) / std::cosf(DirectX::XM_PIDIV2 - theta);
                            uv.y *= (1 - r) + r * s;
                        }
                        else
                        {
                            float s = std::cosf(DirectX::XM_PIDIV4) / std::cosf(theta);
                            uv.x *= (1 - r) + r * s;
                        }

                        // z scale
                        float s = std::powf(0.5f, std::fabsf(z)) * std::sqrtf(2.f);
                        uv = { uv.x * s, uv.y * s };

                        // -1 .. 1 -> 0 .. 1
                        uv.x = (1 + uv.x) * .5f;
                        uv.y = (1 + uv.y) * .5f;
                    }
                    else
                    {
                        // mirror in u? 0..1..0
                        float u = lat * du;
                        if (in_props.m_mirrorU && (u > 1))
                        {
                            u = 2 - u;
                        }

                        uv = DirectX::XMFLOAT2{ u, v };
                    }
                    out_vertices.push_back(Vertex{ pos, pos, uv });
                } // end loops around latitude

                if (repeatEdge) // duplicate the first position, but with u = 1 instead of 0.
                {
                    out_vertices.push_back(out_vertices[out_vertices.size() - in_props.m_numLong]);
                    out_vertices.back().tex.x = 1;
                }
            } // end vertical steps
        }

        UINT numRows = in_props.m_numLat - 3; // skip top and bottom
        UINT vertsPerRow = in_props.m_numLong;

        // generate connectivity to form rows of triangles
        {
            UINT itersPerRow = in_props.m_numLong;
            if (!repeatEdge) { itersPerRow--; }
            else { vertsPerRow++; }

            for (UINT v = 0; v < numRows; v++)
            {
                UINT base = v * vertsPerRow;
                for (UINT i = 0; i < itersPerRow; i += 1)
                {
                    out_indices.push_back(base + i);
                    out_indices.push_back(base + vertsPerRow + i + 1);
                    out_indices.push_back(base + vertsPerRow + i);

                    out_indices.push_back(base + i);
                    out_indices.push_back(base + i + 1);
                    out_indices.push_back(base + vertsPerRow + i + 1);
                }

                if (!repeatEdge) // connect to first vert of each row
                {
                    out_indices.push_back(base + itersPerRow);
                    out_indices.push_back(base + vertsPerRow);
                    out_indices.push_back(base + itersPerRow + vertsPerRow);

                    out_indices.push_back(base + itersPerRow);
                    out_indices.push_back(base);
                    out_indices.push_back(base + vertsPerRow);
                }
            }

        }

        // create top and bottom caps
        // this version creates many little top triangles, each connected to a different "pole" vertex
        // the pole vertices have a u value that equals one of the two base triangles
        // this brings everything to a neat point
        {
            // smooth slight pointiness at poles
            float southZ = (out_vertices[0].pos.z - 1.0f) / 2.f;
            float northZ = (out_vertices[vertsPerRow * numRows].pos.z + 1.0f) / 2.f;
            
            if (in_props.m_topBottom)
            {
                // poles are in the center of the image
                DirectX::XMFLOAT2 uv(0.5f, 0.5f);

                UINT southIndex = (UINT)out_vertices.size();
                out_vertices.push_back(Vertex{ DirectX::XMFLOAT3(0,0,southZ), DirectX::XMFLOAT3(0,0,-1), uv });

                UINT northIndex = (UINT)out_vertices.size();
                out_vertices.push_back(Vertex{ DirectX::XMFLOAT3(0,0,northZ), DirectX::XMFLOAT3(0,0,1), uv });

                for (UINT i = 0; i < in_props.m_numLong; i++)
                {
                    out_indices.push_back(southIndex);
                    UINT nextIndex = i + 1;
                    if (nextIndex == in_props.m_numLong)
                    {
                        nextIndex = 0;
                    }
                    out_indices.push_back(nextIndex);
                    out_indices.push_back(i);

                    out_indices.push_back(northIndex);
                    out_indices.push_back((vertsPerRow * numRows) + i);
                    UINT lastIndex = (vertsPerRow * numRows) + i + 1;
                    if (0 == nextIndex)
                    {
                        lastIndex = (vertsPerRow * numRows);
                    }
                    out_indices.push_back(lastIndex);
                }
            }
            else
            {
                for (UINT i = 0; i < in_props.m_numLong; i++)
                {
                    out_indices.push_back((UINT)out_vertices.size());
                    DirectX::XMFLOAT2 uv(out_vertices[i].tex.x, 0);
                    out_vertices.push_back(Vertex{ DirectX::XMFLOAT3(0,0,southZ), DirectX::XMFLOAT3(0,0,-1), uv });
                    UINT nextIndex = i + 1;
                    if ((!repeatEdge) && (nextIndex == in_props.m_numLong))
                    {
                        nextIndex = 0;
                    }
                    out_indices.push_back(nextIndex);
                    out_indices.push_back(i);

                    out_indices.push_back((UINT)out_vertices.size());
                    uv = DirectX::XMFLOAT2(out_vertices[(vertsPerRow * numRows) + i].tex.x, 1);
                    out_vertices.push_back(Vertex{ DirectX::XMFLOAT3(0,0,northZ), DirectX::XMFLOAT3(0,0,1), uv });
                    out_indices.push_back((vertsPerRow * numRows) + i);
                    UINT lastIndex = (vertsPerRow * numRows) + i + 1;
                    if ((!repeatEdge) && (0 == nextIndex))
                    {
                        lastIndex = (vertsPerRow * numRows);
                    }
                    out_indices.push_back(lastIndex);
                }
            }
        }

    }
};
