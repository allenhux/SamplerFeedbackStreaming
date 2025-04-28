//*********************************************************
//
// Copyright 2020 Intel Corporation 
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

#include <d3d12.h>
#include "CommandLineArgs.h"
#include "SamplerFeedbackStreaming.h"
#include "CreateSphere.h"

class AssetUploader;

namespace SceneObjects
{
    enum class Descriptors
    {
        HeapOffsetTexture = 0,
        HeapOffsetFeedback,
        NumEntries
    };

    enum class RootSigParams
    {
        ParamObjectTextures = 0,
        ParamSharedTextures,
        ParamConstantBuffers,
        Param32BitConstants,
        ParamSamplers,
        NumParams
    };

    struct DrawParams
    {
        DirectX::XMMATRIX m_projection;
        DirectX::XMMATRIX m_view;
        DirectX::XMMATRIX m_viewInverse;
        D3D12_GPU_DESCRIPTOR_HANDLE m_sharedMinMipMap;
        D3D12_GPU_DESCRIPTOR_HANDLE m_constantBuffers;
        D3D12_GPU_DESCRIPTOR_HANDLE m_samplers;
        D3D12_GPU_DESCRIPTOR_HANDLE m_descriptorHeapBaseGpu;
        D3D12_CPU_DESCRIPTOR_HANDLE m_descriptorHeapBaseCpu;
        UINT m_srvUavCbvDescriptorSize;
        UINT m_descriptorHeapOffset; // before multiplying by descriptor size
        UINT m_windowWidth{ 0 };
        UINT m_windowHeight{ 0 };
        float m_fov;
    };

    class BaseObject
    {
    public:
        virtual ~BaseObject()
        {
            m_pStreamingResource->Destroy();
        }

        // state re-used by a number of objects
        void SetCommonGraphicsState(ID3D12GraphicsCommandList1* in_pCommandList, const SceneObjects::DrawParams& in_drawParams);

        ID3D12RootSignature* GetRootSignature() const
        {
            return m_feedbackEnabled ? m_rootSignatureFB.Get() : m_rootSignature.Get();
        }

        ID3D12PipelineState* GetPipelineState() const
        {
            return m_feedbackEnabled ? m_pipelineStateFB.Get() : m_pipelineState.Get();
        }

        void CreateResource(const std::wstring& in_filename, SFSHeap* in_pHeap,
            const struct XetFileHeader* in_pFileHeader = nullptr);
        void CreateResourceViews(D3D12_CPU_DESCRIPTOR_HANDLE in_baseDescriptorHandle, UINT in_srvUavCbvDescriptorSize);

        // within view frustum?
        virtual bool IsVisible([[maybe_unused]] float in_cotWdiv2, [[maybe_unused]]const float in_zFar ) { return true; }

        // do not draw until minimal assets have been created/uploaded
        bool Drawable() const;

        void Draw(ID3D12GraphicsCommandList1* in_pCommandList, const DrawParams& in_drawParams);

        DirectX::XMMATRIX& GetModelMatrix() { return m_matrix; }
        const DirectX::XMMATRIX& GetCombinedMatrix() { return m_combinedMatrix; }

        // also compute visibility, screen area, and LoD
        void SetCombinedMatrix(const DirectX::XMMATRIX& in_worldProjection,
            UINT in_windowHeight, float in_cotWdiv2, float in_cotHdiv2, float in_zFar);

        void Spin(float in_radians); // spin this object around its desired axis

        // for visualization
        ID3D12Resource* GetTiledResource() const { return m_pStreamingResource->GetTiledResource(); }
        ID3D12Resource* GetMinMipMap() const { return m_pStreamingResource->GetMinMipMap(); }

#if RESOLVE_TO_TEXTURE
        ID3D12Resource* GetResolvedFeedback() const { return m_pStreamingResource->GetResolvedFeedback(); }
#endif

        SFSResource* GetStreamingResource() const { return m_pStreamingResource; }

        void CopyGeometry(const BaseObject* in_pObjectForSharedHeap);

        void SetGeometry(ID3D12Resource* in_pVertexBuffer, UINT in_vertexSize,
            ID3D12Resource* in_pIndexBuffer, UINT in_lod = 0);

        void SetFeedbackEnabled(bool in_value) { m_feedbackEnabled = in_value; }

        void SetAxis(DirectX::XMVECTOR in_vector) { m_axis.v = in_vector; }

        virtual float GetBoundingSphereRadius() { return m_radius; }

        float GetScreenAreaPixels() const { return m_screenAreaPixels; }
        UINT GetLoD() const { return m_lod; }
        bool IsVisible() const { return m_visible; }
    protected:

        // pass in a location in a descriptor heap where this can write 3 descriptors
        BaseObject(
            SFSManager* in_pSFSManager,
            ID3D12Device* in_pDevice);

        BaseObject(
            SFSManager* in_pSFSManager,
            BaseObject* in_pSharedObject);  // to share root sig, etc.

        template<typename T> using ComPtr = Microsoft::WRL::ComPtr<T>;

        bool m_feedbackEnabled{ true };
        SFSManager* m_pSFSManager{ nullptr };

        DirectX::XMMATRIX m_matrix{ DirectX::XMMatrixIdentity() };
        DirectX::XMMATRIX m_combinedMatrix{ DirectX::XMMatrixIdentity() };

        struct ModelConstantData
        {
            DirectX::XMMATRIX g_combinedTransform;
            DirectX::XMMATRIX g_worldTransform;

            int g_minmipmapWidth;
            int g_minmipmapHeight;
            int g_minmipmapOffset;
        };

        virtual void SetModelConstants(ModelConstantData& out_modelConstantData,
            const DirectX::XMMATRIX& in_projection,
            const DirectX::XMMATRIX& in_view);

        SFSResource* m_pStreamingResource{ nullptr };

        void CreatePipelineState(
            const wchar_t* in_ps, const wchar_t* in_psFB, const wchar_t* in_vs,
            ID3D12Device* in_pDevice, UINT in_sampleCount,
            const D3D12_RASTERIZER_DESC& in_rasterizerDesc,
            const D3D12_DEPTH_STENCIL_DESC& in_depthStencilDesc,
            const std::vector<D3D12_INPUT_ELEMENT_DESC>& in_elementDescs = {});

        // pipeline state that does not capture sampler feedback
        void SetRootSigPso(ID3D12GraphicsCommandList1* in_pCommandList)
        {
            in_pCommandList->SetGraphicsRootSignature(m_rootSignature.Get());
            in_pCommandList->SetPipelineState(m_pipelineState.Get());
        }

        // pipeline state with pixel shader that calls WriteSamplerFeedback()
        void SetRootSigPsoFB(ID3D12GraphicsCommandList1* in_pCommandList)
        {
            in_pCommandList->SetGraphicsRootSignature(m_rootSignatureFB.Get());
            in_pCommandList->SetPipelineState(m_pipelineStateFB.Get());
        }

        DirectX::XMVECTORF32 m_axis{ { { 0.0f, 1.0f, 0.0f, 0.0f } } };

        // cache bounding sphere radius
        float m_radius{ 0.0f };
        virtual bool ComputeVisible(
            [[maybe_unused]] float in_cotWdiv2, [[maybe_unused]] float in_cotHdiv2,
            [[maybe_unused]] const float in_zFar) { return true; }

    private:
        bool m_createResourceViews{ true };

        bool m_visible{ true };
        float m_screenAreaPixels{ 0 }; // only updated if visible
        UINT m_lod{ 0 }; // only updated if visible
        float ComputeScreenAreaPixels(UINT in_windowHeight, float in_fov);
        UINT ComputeLod();

        struct Geometry
        {
            UINT m_numIndices{ 0 };
            D3D12_INDEX_BUFFER_VIEW m_indexBufferView;
            D3D12_VERTEX_BUFFER_VIEW m_vertexBufferView;
            ComPtr<ID3D12Resource> m_indexBuffer;
            ComPtr<ID3D12Resource> m_vertexBuffer;
        };

        std::vector<Geometry> m_lods;

        ComPtr<ID3D12RootSignature> m_rootSignature;
        ComPtr<ID3D12PipelineState> m_pipelineState;

        ComPtr<ID3D12RootSignature> m_rootSignatureFB;
        ComPtr<ID3D12PipelineState> m_pipelineStateFB;

        std::wstring GetAssetFullPath(const std::wstring& in_filename);
    };

    void CreateSphere(SceneObjects::BaseObject* out_pObject,
        ID3D12Device* in_pDevice, AssetUploader& in_assetUploader,
        const SphereGen::Properties& in_sphereProperties, UINT in_numLods = 1);

    void CreateSphereResources(ID3D12Resource** out_ppVertexBuffer, ID3D12Resource** out_ppIndexBuffer,
        ID3D12Device* in_pDevice, const SphereGen::Properties& in_sphereProperties,
        AssetUploader& in_assetUploader);

    class Terrain : public BaseObject
    {
    public:
        Terrain(SFSManager* in_pSFSManager,
            ID3D12Device* in_pDevice,
            UINT in_sampleCount,
            const CommandLineArgs& in_args,
            AssetUploader& in_assetUploader);
    };

    class Planet : public BaseObject
    {
    public:
        Planet(SFSManager* in_pSFSManager,
            ID3D12Device* in_pDevice, AssetUploader& in_assetUploader,
            UINT in_sampleCount,
            const SphereGen::Properties& in_properties);

        Planet(Planet* in_pSharedObject);

        Planet(SFSManager* in_pSFSManager,
            ID3D12Device* in_pDevice, AssetUploader& in_assetUploader,
            UINT in_sampleCount);

        virtual bool ComputeVisible(float in_cotWdiv2, float in_cotHdiv2, const float in_zFar) override;
        float GetBoundingSphereRadius() override;
    };

    // special render state (front face cull)
    // lower triangle count
    class Sky : public BaseObject
    {
    public:
        Sky(SFSManager* in_pSFSManager,
            ID3D12Device* in_pDevice, AssetUploader& in_assetUploader,
            UINT in_sampleCount);

        virtual void SetModelConstants(ModelConstantData& out_modelConstantData,
            const DirectX::XMMATRIX& in_projection,
            const DirectX::XMMATRIX& in_view) override;
    };
}
