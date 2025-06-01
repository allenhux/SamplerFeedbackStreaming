//******************************************************/
// Copyright 2024 Allen Hux
//
// SPDX-License-Identifier: MIT
//*********************************************************

#pragma once

#include "pch.h"

#include <cstdint>
#include <vector>
#include <array>

class Subdivision
{
public:
    // we need a function that creates a vertex from 2 parent vertex indices,
    // adds it to a storage object, then returns the index of the new vertex.
    // we don't care about the vertex format or the storage object
    // std::function lets the caller capture that into the function
    using AddVertex = std::function<uint32_t(uint32_t, uint32_t)>;
    using Edge = std::array<uint32_t, 2>; // e.g. edge{0,1} is the edge from vert 0 to vert 1
    struct EdgeIndex
    {
        uint32_t direction : 1; // reverse direction for winding order?
        uint32_t index : 31;
    };
    struct Triangle
    {
        EdgeIndex m_edgeIndices[3];
    };

    // initial object to subdivide
    Subdivision(AddVertex in_addVert,
        const std::vector<Edge>& in_edges,
        const std::vector<Triangle>& in_triangles)
        //: m_addVert(in_addVert), m_edges(in_edges), m_triangles(in_triangles)
    {
        m_addVert = in_addVert;
        m_edges = in_edges;
        m_triangles = in_triangles;
    }

    // subdivision appends to the vertex buffer
    // the internal edge list is replaced each iteration
    void Next()
    {
        // subdivide edges (adds a vertex per edge)
        std::vector<Edge> edges;
        edges.reserve(m_edges.size() * 2);
        for (const auto& e : m_edges)
        {
            uint32_t v = m_addVert(e[0], e[1]);
            // children of m_edges[i] will be edges[i*2] and edges[i*2+1]
            edges.push_back({ e[0], v });
            edges.push_back({ v, e[1] });
        }

        // generate triangles (adds 3 more edges)
        std::vector<Triangle> triangles;
        for (const auto& t : m_triangles)
        {
            EdgeIndex e0 = t.m_edgeIndices[0];
            uint32_t e00 = e0.index * 2;
            uint32_t e01 = e00 + 1;
            
            EdgeIndex e1 = t.m_edgeIndices[1];
            uint32_t e10 = e1.index * 2;
            uint32_t e11 = e10 + 1;

            EdgeIndex e2 = t.m_edgeIndices[2];
            uint32_t e20 = e2.index * 2;
            uint32_t e21 = e20 + 1;

            // regardless of the direction of edges 0, 1, 2
            // the generated edges will always have direction 0
            uint32_t m0 = edges[e00][1];
            uint32_t m1 = edges[e10][1];
            uint32_t m2 = edges[e20][1];

            // new center triangle
            uint32_t baseIndex = (uint32_t)edges.size();
            edges.push_back({ m0, m1 });
            edges.push_back({ m1, m2 });
            edges.push_back({ m2, m0 });
            struct EdgeIndex x0 { 0, baseIndex };
            triangles.push_back({ { {0,baseIndex + 0}, {0,baseIndex + 1}, {0,baseIndex + 2} } });

            if (e0.direction) { std::swap(e00, e01); }
            if (e1.direction) { std::swap(e10, e11); }
            if (e2.direction) { std::swap(e20, e21); }
            triangles.push_back({ { {e0.direction, e00}, {1,baseIndex + 2}, {e2.direction, e21} } });
            triangles.push_back({ { {e0.direction, e01}, {e1.direction, e10}, {1, baseIndex + 0} } });
            triangles.push_back({ { {1, baseIndex + 1}, {e1.direction, e11}, {e2.direction, e20} } });
        }

        m_edges.swap(edges);
        m_triangles.swap(triangles);
    }

    // generate index buffer from the current topology
    void GetIndices(std::vector<uint32_t>& out_indices)
    {
        out_indices.clear();
        out_indices.reserve(m_triangles.size() * 3);
        for (auto& t : m_triangles)
        {
            uint32_t index[3];
            index[0] = m_edges[t.m_edgeIndices[0].index][0];
            index[1] = m_edges[t.m_edgeIndices[0].index][1];
            if (t.m_edgeIndices[0].direction)
            {
                std::swap(index[0], index[1]);
            }
            uint32_t i = 1;
            if (t.m_edgeIndices[1].direction)
            {
                i = 0;
            }
            index[2] = m_edges[t.m_edgeIndices[1].index][i];

            out_indices.push_back(index[0]);
            out_indices.push_back(index[1]);
            out_indices.push_back(index[2]);
        }
    }
private:
    std::vector<Edge> m_edges;
    std::vector<Triangle> m_triangles;
    AddVertex m_addVert;

    Subdivision(Subdivision&) = delete;
};
