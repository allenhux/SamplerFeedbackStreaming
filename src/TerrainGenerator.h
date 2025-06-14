//==============================================================
// Copyright � Intel Corporation
//
// SPDX-License-Identifier: MIT
// =============================================================

#pragma once

class TerrainGenerator
{
public:
    struct Params
    {
        // terrain object parameters
        UINT  m_terrainSideSize{ 256 };
        float m_heightScale{ 50 };
        float m_noiseScale{ 25 };
        UINT  m_numOctaves{ 8 };
        float m_mountainSize{ 4000 };
    };

    TerrainGenerator(const Params& in_args);

    struct Vertex
    {
        DirectX::XMFLOAT3 pos;
        DirectX::XMFLOAT3 normal;
        DirectX::XMFLOAT2 tex;
    };
    UINT GetNumIndices() const { return m_numIndices; }
    UINT GetIndexBufferSize() const { return UINT(m_numIndices * sizeof(UINT)); }

    void GenerateIndices(UINT* pResult);
    const std::vector<Vertex>& GetVertices() const { return m_vertices; }
private:
    UINT m_numIndices{ 0 };
    std::vector<DirectX::XMFLOAT2> m_noiseLattice;
    std::vector<Vertex> m_vertices;
    const Params& m_args;

    struct int2
    {
        int x, y;
        int2() { x = 0; y = 0; }
        int2(int a, int b) : x(a), y(b) {}

        friend int2 operator+(const int2& lhs, const int2& rhs)
        {
            return int2(lhs.x + rhs.x, lhs.y + rhs.y);
        }
    };
    float Noise(DirectX::XMFLOAT2 scaledLocation);
    DirectX::XMFLOAT2 ReadLattice(int2 location);

    template <typename T> T Lerp(T lo, T hi, float w) { return (lo + ((hi - lo) * w)); }
    float Dot(DirectX::XMFLOAT2 lhs, DirectX::XMFLOAT2 rhs) { return lhs.x * rhs.x + lhs.y * rhs.y; }
    float Bilinear(float lo, float hi, float t)
    {
        float weight = Spline(t);
        return lo + weight * (hi - lo);
    }
    float Spline(float t) { return 3 * (t * t) - 2 * t * t * t; }
    float Gaussian(float x, float width) { return exp(-(x * x) / width); }

    DirectX::XMVECTOR ComputeNormal(UINT vtx0, UINT vtx1, UINT vtx2) const;

    void Add(DirectX::XMFLOAT3& out_a, DirectX::XMVECTOR in_b);
    void Normalize(DirectX::XMFLOAT3& out_v);

    void GenerateVertices();
};
