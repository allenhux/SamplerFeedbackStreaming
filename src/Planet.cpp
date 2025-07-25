//==============================================================
// Copyright � Intel Corporation
//
// SPDX-License-Identifier: MIT
// =============================================================

#pragma once

#include "pch.h"

#include <limits>
#include "SceneObject.h"
#include "SharedConstants.h"
#include "Scene.h"
#include "AssetUploader.h"
#include "Subdivision.h"

struct PlanetVertex
{
    DirectX::XMFLOAT3 pos;
    DirectX::XMFLOAT3 normal;
    DirectX::XMFLOAT2 uv;
};

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
static ID3D12Resource* CreatePlanetIndexBuffer(
    ID3D12Device* in_pDevice, AssetUploader& in_assetUploader,
    const std::vector<UINT32>& in_indices)
{
    ID3D12Resource* pResource = nullptr;
    UINT indexBufferSize = UINT(in_indices.size()) * sizeof(in_indices[0]);

    const auto heapProperties = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
    const auto resourceDesc = CD3DX12_RESOURCE_DESC::Buffer(indexBufferSize);
    ThrowIfFailed(in_pDevice->CreateCommittedResource(
        &heapProperties,
        D3D12_HEAP_FLAG_NONE,
        &resourceDesc,
        D3D12_RESOURCE_STATE_COMMON,
        nullptr,
        IID_PPV_ARGS(&pResource)));

    in_assetUploader.SubmitRequest(pResource, in_indices.data(), indexBufferSize,
        D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_INDEX_BUFFER);

    return pResource;
}

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
static ID3D12Resource* CreatePlanetVertexBuffer(
    ID3D12Device* in_pDevice, AssetUploader& in_assetUploader,
    const std::vector<PlanetVertex>& in_verts)
{
    ID3D12Resource* pResource = nullptr;
    UINT vertexBufferSize = UINT(in_verts.size()) * sizeof(in_verts[0]);

    const auto heapProperties = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
    const auto resourceDesc = CD3DX12_RESOURCE_DESC::Buffer(vertexBufferSize);
    ThrowIfFailed(in_pDevice->CreateCommittedResource(
        &heapProperties,
        D3D12_HEAP_FLAG_NONE,
        &resourceDesc,
        D3D12_RESOURCE_STATE_COMMON,
        nullptr,
        IID_PPV_ARGS(&pResource)));

    in_assetUploader.SubmitRequest(pResource, in_verts.data(), vertexBufferSize,
        D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);

    return pResource;
}

//-----------------------------------------------------------------------------
// create planet UV
//-----------------------------------------------------------------------------
static DirectX::XMFLOAT2 PlanetUV(DirectX::XMFLOAT3 pos)
{
    DirectX::XMFLOAT2 uv = { pos.x, pos.y };

    // radius of this latitude in x/y plane
    // for a sphere, 1 = sqrt(x^2 + y^2 + z^2)
    // since r^2 = x^2 + y^2, 1 = sqrt(r^2 + z^2) and 1^2 - z^2 = r^2, so...
    float rSquared = 1.f - (pos.z * pos.z);

    // radial scale: squeeze texture coords into circle
    if (rSquared > std::numeric_limits<float>::min())
    {
        float rcostheta = std::fabsf(pos.x);
        float rsintheta = std::fabsf(pos.y);

        // conceptually, cast a ray from the origin through the current position to the edge of a unit square
        // the length of that ray is the scale factor to project "here" to the edge of the square
        // use simple trig to get length: sin = opposite/hypotenuse or cos = adjacent/hypotenuse
        //      e.g. s * cos(theta) = 1, hence s = 1/cos. cos = pos.x / r, hence s = r/pos.x
        //      for theta 45..90 degrees, s = r/pos.y
        // note numerator (r) has been included in the lerp
        float rs = 1.f / std::max(rcostheta, rsintheta);

        // counteract scale so center appears undistorted:
        const float distortion = std::sqrtf(2.0f) / 2.f;
        // quadratic interpolation of scale factor:
        float s = std::lerp(distortion, rs, rSquared * (1 - std::fabsf(pos.z)) * (1 - std::fabsf(pos.z)));
        uv.x *= s;
        uv.y *= s;
    }

    // [-1 .. 1] -> [0 .. 1]
    uv.x = (1 + uv.x) * .5f;
    uv.y = (1 + uv.y) * .5f;

    return uv;
}

//=========================================================================
// planets have multiple LoDs
// Texture Coordinates may optionally be mirrored in U
//=========================================================================
SceneObjects::Geometry* SceneObjects::Planet::m_pGeometry{ nullptr };
const SceneObjects::Geometry* SceneObjects::Planet::GetGeometry() const { return m_pGeometry; }
SceneObjects::Planet::Planet(Scene* in_pScene)
{
    SetAxis(DirectX::XMVectorSet(0, 0, 1, 0));

    if (nullptr != GetGeometry())
    {
        return;
    }

    ID3D12Device* pDevice = in_pScene->GetDevice();
    auto& assetUploader = in_pScene->GetAssetUploader();
    UINT sampleCount = in_pScene->GetArgs().m_sampleCount;

    m_pGeometry = ConstructGeometry();

    CreateRootSignature(m_pGeometry, pDevice);

    D3D12_RASTERIZER_DESC rasterizerDesc = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    D3D12_DEPTH_STENCIL_DESC depthStencilDesc = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
    // Define the vertex input layout
    std::vector< D3D12_INPUT_ELEMENT_DESC> inputElementDescs = {
        { "POS",    0, DXGI_FORMAT_R32G32B32_FLOAT, 0,  0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD",    0, DXGI_FORMAT_R32G32_FLOAT,    0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };

    CreatePipelineState(m_pGeometry,
        L"planetPS.cso", L"planetPS-FB.cso", L"planetVS.cso",
        pDevice, sampleCount, rasterizerDesc, depthStencilDesc, inputElementDescs);

    std::vector<PlanetVertex> verts;
    verts.push_back({ { 1, 0, 0 }, {0, 0, 0} }); // 0
    verts.push_back({ { 0, 1, 0 }, { 0, 0, 0 } }); // 1
    verts.push_back({ { -1, 0, 0 }, {0, 0, 0} }); // 2
    verts.push_back({ { 0, -1, 0 }, { 0, 0, 0 } }); // 3
    verts.push_back({ { 0, 0, 1 }, { 0, 0, 0 } }); // 4 (top)
    std::vector<Subdivision::Edge> edges;
    edges.push_back({ 0, 1 }); // e0
    edges.push_back({ 1, 2 }); // e1
    edges.push_back({ 2, 3 }); // e2
    edges.push_back({ 3, 0 }); // e3
    edges.push_back({ 0, 4 }); // e4 (to top)
    edges.push_back({ 1, 4 }); // e5 (to top)
    edges.push_back({ 2, 4 }); // e6 (to top)
    edges.push_back({ 3, 4 }); // e7 (to top)
    std::vector<Subdivision::Triangle> tris;
    for (UINT i = 0; i < 4; i++)
    {
        tris.push_back({ { { 0, i }, { 0, 4 + (i + 1) % 4 }, { 1, i + 4 } } });
    }

    // bottom hemisphere
    verts.push_back({ { 0, 0, -1 }, {0, 0, 0} }); // 5 (bottom)
    edges.push_back({ 0, 5 }); // e8 (to bottom)
    edges.push_back({ 1, 5 }); // e9 (to bottom)
    edges.push_back({ 2, 5 }); // e10 (to bottom)
    edges.push_back({ 3, 5 }); // e11 (to bottom)
    for (UINT i = 0; i < 4; i++)
    {
        tris.push_back({ { { 1, i }, { 0, i + 8 }, {1, 8 + (i + 1) % 4 } } });
    }

    for (auto& v : verts)
    {
        DirectX::XMStoreFloat3(&v.pos, DirectX::XMVector3Normalize(DirectX::XMVectorSet(v.pos.x, v.pos.y, v.pos.z, 0)));
        v.normal = v.pos;
        v.uv = PlanetUV(v.pos);
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
            verts.push_back({ pos, pos, PlanetUV(pos)});

            return i;
        },
        edges, tris);

    constexpr UINT numLods = SharedConstants::NUM_SPHERE_LEVELS_OF_DETAIL;

    std::vector<ID3D12Resource*> indexBuffers(numLods);

    for (UINT lod = 0; lod < numLods; lod++)
    {
        sub.Next();
        std::vector<uint32_t> indices;
        sub.GetIndices(indices);
        indexBuffers[lod] = CreatePlanetIndexBuffer(pDevice, assetUploader, indices);
    }

    // only 1 vertex buffer is required for all LoDs because subdivided triangles re-use vertices
    ID3D12Resource* pVertexBuffer = CreatePlanetVertexBuffer(pDevice, assetUploader, verts);

    m_pGeometry->m_lods.resize(numLods);
    for (UINT i = 0; i < numLods; i++)
    {
        auto& lod = m_pGeometry->m_lods[numLods - i - 1];
        lod.m_vertexBuffer = pVertexBuffer;
        lod.m_indexBuffer = indexBuffers[i];
        lod.m_numIndices = (UINT)lod.m_indexBuffer->GetDesc().Width / sizeof(UINT32);
        lod.m_vertexBufferView = { lod.m_vertexBuffer->GetGPUVirtualAddress(), (UINT)lod.m_vertexBuffer->GetDesc().Width, sizeof(verts[0]) };
        lod.m_indexBufferView = { lod.m_indexBuffer->GetGPUVirtualAddress(), (UINT)lod.m_indexBuffer->GetDesc().Width, DXGI_FORMAT_R32_UINT };

        lod.m_indexBuffer->Release();
    }

    pVertexBuffer->Release();
}
