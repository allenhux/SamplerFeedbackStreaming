//*********************************************************
//
// Copyright 2020 Allen Hux 
//
// Permission is hereby granted, free of charge, to any 
// person obtaining a copy of this software and associated 
// documentation files(the "Software"), to deal in the Software 
// without restriction, including without limitation the rights 
// to use, copy, modify, merge, publish, distribute, sublicense, 
// and/or sell copies of the Software, and to permit persons to 
// whom the Software is furnished to do so, subject to the 
// following conditions :
// The above copyright notice and this permission notice shall 
// be included in all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, 
// EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF 
// MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
// NONINFRINGEMENT.IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT 
// HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, 
// WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, 
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER 
// DEALINGS IN THE SOFTWARE.
//
//*********************************************************

#pragma once

#include "pch.h"

#include "SceneObject.h"
#include "SharedConstants.h"
#include "Scene.h"
#include "AssetUploader.h"
#include "Subdivision.h"

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
static void CreatePlanetResources(
    const std::vector<SphereGen::Vertex>& in_verts, const std::vector<UINT32>& in_indices,
    ID3D12Resource** out_ppVertexBuffer, ID3D12Resource** out_ppIndexBuffer,
    ID3D12Device* in_pDevice, AssetUploader& in_assetUploader)
{
    // build vertex buffer
    {
        UINT vertexBufferSize = UINT(in_verts.size()) * sizeof(in_verts[0]);

        const auto heapProperties = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
        const auto resourceDesc = CD3DX12_RESOURCE_DESC::Buffer(vertexBufferSize);
        ThrowIfFailed(in_pDevice->CreateCommittedResource(
            &heapProperties,
            D3D12_HEAP_FLAG_NONE,
            &resourceDesc,
            D3D12_RESOURCE_STATE_COMMON,
            nullptr,
            IID_PPV_ARGS(out_ppVertexBuffer)));

        in_assetUploader.SubmitRequest(*out_ppVertexBuffer, in_verts.data(), vertexBufferSize,
            D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
    }

    // build index buffer
    {
        UINT indexBufferSize = UINT(in_indices.size()) * sizeof(in_indices[0]);

        const auto heapProperties = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
        const auto resourceDesc = CD3DX12_RESOURCE_DESC::Buffer(indexBufferSize);
        ThrowIfFailed(in_pDevice->CreateCommittedResource(
            &heapProperties,
            D3D12_HEAP_FLAG_NONE,
            &resourceDesc,
            D3D12_RESOURCE_STATE_COMMON,
            nullptr,
            IID_PPV_ARGS(out_ppIndexBuffer)));

        in_assetUploader.SubmitRequest(*out_ppIndexBuffer, in_indices.data(), indexBufferSize,
            D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_INDEX_BUFFER);
    }
}

//-----------------------------------------------------------------------------
// UV from position
//-----------------------------------------------------------------------------
static DirectX::XMFLOAT2 PlanetUV(DirectX::XMFLOAT3 pos)
{
    DirectX::XMFLOAT2 uv = { pos.x, pos.y };

    // radius of this latitude in x/y plane
    // very near the pole, use center uv
    if (std::fabsf(pos.z) > 0.999f)
    {
        return { 0.5f, 0.5f };
    }
    float r = std::sqrtf(pos.x * pos.x + pos.y * pos.y);
    float theta = std::acosf(pos.x / r);

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
    float s = std::powf(0.5f, std::fabsf(pos.z)) * std::sqrtf(2.f);
    uv = { uv.x * s, uv.y * s };

    // -1 .. 1 -> 0 .. 1
    uv.x = (1 + uv.x) * .5f;
    uv.y = (1 + uv.y) * .5f;

    return uv;
}

//=========================================================================
// planets have multiple LoDs
// Texture Coordinates may optionally be mirrored in U
//=========================================================================
SceneObjects::Planet::Planet(const std::wstring& in_filename,
    TileUpdateManager* in_pTileUpdateManager,
    StreamingHeap* in_pStreamingHeap,
    ID3D12Device* in_pDevice, AssetUploader& in_assetUploader,
    UINT in_sampleCount,
    D3D12_CPU_DESCRIPTOR_HANDLE in_srvBaseCPU) :
    BaseObject(in_filename, in_pTileUpdateManager, in_pStreamingHeap,
        in_pDevice, in_srvBaseCPU, nullptr)
{
    SetAxis(DirectX::XMVectorSet(0, 0, 1, 0));

    D3D12_RASTERIZER_DESC rasterizerDesc = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    D3D12_DEPTH_STENCIL_DESC depthStencilDesc = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
    CreatePipelineState(L"terrainPS.cso", L"terrainPS-FB.cso", L"terrainVS.cso", in_pDevice, in_sampleCount, rasterizerDesc, depthStencilDesc);

    const float theta = DirectX::XM_PI / 6; // 30 degrees
    const float x = std::cos(theta);
    const float y = std::sin(theta);

    std::vector<TerrainGenerator::Vertex> verts;
    verts.push_back({ { -x, -y, 0 }, {0, 0, 0}, {0, 0} }); // 0
    verts.push_back({ { x, -y, 0 }, { 0, 0, 0 }, { 0, 0 } }); // 1
    verts.push_back({ { 0, 1.f, 0 }, {0, 0, 0}, {0, 0} }); // 2
    verts.push_back({ { 0, 0, -1.f }, { 0, 0, 0 }, { 0, 0 } }); // 3 (top)
    std::vector<Subdivision::Edge> edges;
    edges.push_back({ 0, 1 }); // 0
    edges.push_back({ 1, 2 }); // 1
    edges.push_back({ 2, 0 }); // 2
    edges.push_back({ 0, 3 }); // 3
    edges.push_back({ 1, 3 }); // 4
    edges.push_back({ 2, 3 }); // 5
    std::vector<Subdivision::Triangle> tris;
    tris.push_back({ { { 0, 3 }, { 1, 4 }, { 1, 0 } } });
    tris.push_back({ { { 0, 4 }, { 1, 5 }, { 1, 1 } } });
    tris.push_back({ { { 0, 5 }, { 1, 3 }, { 1, 2 } } });

    // bottom hemisphere
    verts.push_back({ { 0, 0, 1 }, {0, 0, 0}, {0, 0} }); // 4
    edges.push_back({ 4, 0 }); // 6
    edges.push_back({ 4, 1 }); // 7
    edges.push_back({ 4, 2 }); // 8
    tris.push_back({ { { 0, 6 }, { 0, 0 }, { 1, 7 } } });
    tris.push_back({ { { 0, 7 }, { 0, 1 }, { 1, 8 } } });
    tris.push_back({ { { 0, 8 }, { 0, 2 }, { 1, 6 } } });

    for (auto& v : verts)
    {
        DirectX::XMStoreFloat3(&v.pos, DirectX::XMVector3Normalize(DirectX::XMVectorSet(v.pos.x, v.pos.y, v.pos.z, 0)));
        v.normal = v.pos;
        v.tex = PlanetUV(v.pos);
    }

    Subdivision sub(
        [&](uint32_t a, uint32_t b)
        {
            uint32_t i = (uint32_t)verts.size();
            float x = (verts[a].pos.x + verts[b].pos.x) * 0.5f;
            float y = (verts[a].pos.y + verts[b].pos.y) * 0.5f;
            float z = (verts[a].pos.z + verts[b].pos.z) * 0.5f;

            DirectX::XMFLOAT3 pos;
            DirectX::XMStoreFloat3(&pos, DirectX::XMVector3Normalize(DirectX::XMVectorSet(x, y, z, 0)));
            verts.push_back({ pos, pos, PlanetUV(pos) });

            return i;
        },
        edges, tris);

    // bump it up one level to start
    sub.Next();

    constexpr UINT numLods = 3;
    for (UINT lod = 0; lod < numLods; lod++)
    {
        sub.Next();
        std::vector<uint32_t> indices;
        sub.GetIndices(indices);

        ID3D12Resource* pVertexBuffer{ nullptr };
        ID3D12Resource* pIndexBuffer{ nullptr };
        CreatePlanetResources(verts, indices, &pVertexBuffer, &pIndexBuffer, in_pDevice, in_assetUploader);
        SetGeometry(pVertexBuffer, (UINT)sizeof(verts[0]), pIndexBuffer, numLods - lod - 1);
    }
}
