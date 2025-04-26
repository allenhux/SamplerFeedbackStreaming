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

//=========================================================================
// planets have multiple LoDs
// Texture Coordinates may optionally be mirrored in U
//=========================================================================
SceneObjects::Planet::Planet(
    SFSManager* in_pSFSManager,
    ID3D12Device* in_pDevice, AssetUploader& in_assetUploader,
    UINT in_sampleCount) :
    BaseObject(in_pSFSManager, in_pDevice)
{
    SetAxis(DirectX::XMVectorSet(0, 0, 1, 0));

    D3D12_RASTERIZER_DESC rasterizerDesc = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    D3D12_DEPTH_STENCIL_DESC depthStencilDesc = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
    // Define the vertex input layout
    std::vector< D3D12_INPUT_ELEMENT_DESC> inputElementDescs = {
        { "POS",    0, DXGI_FORMAT_R32G32B32_FLOAT, 0,  0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };

    CreatePipelineState(L"planetPS.cso", L"planetPS-FB.cso", L"planetVS.cso",
        in_pDevice, in_sampleCount, rasterizerDesc, depthStencilDesc, inputElementDescs);

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
            verts.push_back({ pos, pos });

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
        indexBuffers[lod] = CreatePlanetIndexBuffer(in_pDevice, in_assetUploader, indices);
    }
    
    // only 1 vertex buffer is required for all LoDs because subdivided triangles re-use vertices
    ID3D12Resource* pVertexBuffer = CreatePlanetVertexBuffer(in_pDevice, in_assetUploader, verts);

    for (UINT lod = 0; lod < numLods; lod++)
    {
        SetGeometry(pVertexBuffer, (UINT)sizeof(verts[0]), indexBuffers[lod], numLods - lod - 1);
    }
}
