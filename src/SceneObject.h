//==============================================================
// Copyright © Intel Corporation
//
// SPDX-License-Identifier: MIT
// =============================================================

#pragma once

#include <d3d12.h>
#include <list>
#include "CommandLineArgs.h"
#include "SamplerFeedbackStreaming.h"
#include "CreateSphere.h"

class Scene;
class AssetUploader;

namespace SceneObjects
{
    template<typename T> using ComPtr = Microsoft::WRL::ComPtr<T>;

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
        DirectX::XMMATRIX m_view;
        DirectX::XMMATRIX m_viewInverse;
        D3D12_GPU_DESCRIPTOR_HANDLE m_sharedMinMipMap;
        D3D12_GPU_DESCRIPTOR_HANDLE m_constantBuffers;
        D3D12_GPU_DESCRIPTOR_HANDLE m_samplers;
        D3D12_GPU_DESCRIPTOR_HANDLE m_descriptorHeapBaseGpu;
        D3D12_CPU_DESCRIPTOR_HANDLE m_descriptorHeapBaseCpu;
        UINT m_srvUavCbvDescriptorSize;
        UINT m_descriptorHeapOffset; // before multiplying by descriptor size
    };

    // meshes and materials
    struct Geometry
    {

        struct Mesh
        {
            UINT m_numIndices{ 0 };
            D3D12_INDEX_BUFFER_VIEW m_indexBufferView;
            D3D12_VERTEX_BUFFER_VIEW m_vertexBufferView;
            ComPtr<ID3D12Resource> m_indexBuffer;
            ComPtr<ID3D12Resource> m_vertexBuffer;
        };

        std::vector<Mesh> m_lods;

        ComPtr<ID3D12RootSignature> m_rootSignature;
        ComPtr<ID3D12PipelineState> m_pipelineState;

        ComPtr<ID3D12RootSignature> m_rootSignatureFB;
        ComPtr<ID3D12PipelineState> m_pipelineStateFB;
    };

    class BaseObject
    {
    public:
        BaseObject() {}

        virtual ~BaseObject()
        {
            // since we need to SFSManager::FlushResources() first, better to explicitly destroy them outside the destructor
            // m_pStreamingResource->Destroy();
        }

        // state re-used by a number of objects
        static void SetCommonGraphicsState(ID3D12GraphicsCommandList1* out_pCommandList, const SceneObjects::DrawParams& in_drawParams);

        ID3D12RootSignature* GetRootSignature() const
        {
            return m_feedbackEnabled ? GetGeometry()->m_rootSignatureFB.Get() : GetGeometry()->m_rootSignature.Get();
        }

        ID3D12PipelineState* GetPipelineState() const
        {
            return m_feedbackEnabled ? GetGeometry()->m_pipelineStateFB.Get() : GetGeometry()->m_pipelineState.Get();
        }

        // do not draw until minimal assets have been created/uploaded
        bool Drawable() const;

        // within view frustum?
        virtual bool IsVisible([[maybe_unused]] float in_cotWdiv2, [[maybe_unused]]const float in_zFar ) { return true; }

        void Draw(ID3D12GraphicsCommandList1* in_pCommandList, const DrawParams& in_drawParams);

        DirectX::XMMATRIX& GetModelMatrix() { return m_matrix; }
        const DirectX::XMMATRIX& GetCombinedMatrix() const { return m_combinedMatrix; }

        // also compute visibility, screen area, and LoD
        void SetCombinedMatrix(const DirectX::XMMATRIX& in_worldProjection,
            UINT in_windowHeight, float in_cotWdiv2, float in_cotHdiv2, float in_zFar);

        void Spin(float in_radians); // spin this object around its desired axis

        SFSResource* GetStreamingResource() const { return m_pStreamingResource; }
        void SetResource(SFSResource* in_pResource);

        void SetFeedbackEnabled(bool in_value) { m_feedbackEnabled = in_value; }

        void SetAxis(DirectX::XMVECTOR in_vector) { m_axis.v = in_vector; }

        virtual float GetBoundingSphereRadius() { return m_radius; }

        float GetScreenAreaPixels() const { return m_screenAreaPixels; }

        UINT GetScreenAreaThreshold() const { return m_screenAreaThreshold; }

        UINT GetLoD() const { return m_lod; }
        bool IsVisible() const { return m_visible; }
    protected:
        virtual const Geometry* GetGeometry() const = 0;
        Geometry* ConstructGeometry();

        bool m_feedbackEnabled{ true };

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

        void SetModelConstants(ModelConstantData& out_modelConstantData);

        SFSResource* m_pStreamingResource{ nullptr };

        void CreateRootSignature(Geometry* out_pGeometry, ID3D12Device* in_pDevice);
        void CreatePipelineState(Geometry* out_pGeometry,
            const wchar_t* in_ps, const wchar_t* in_psFB, const wchar_t* in_vs,
            ID3D12Device* in_pDevice, UINT in_sampleCount,
            const D3D12_RASTERIZER_DESC& in_rasterizerDesc,
            const D3D12_DEPTH_STENCIL_DESC& in_depthStencilDesc,
            const std::vector<D3D12_INPUT_ELEMENT_DESC>& in_elementDescs = {});

        DirectX::XMVECTORF32 m_axis{ { { 0.0f, 1.0f, 0.0f, 0.0f } } };

        // cache bounding sphere radius
        float m_radius{ 0.0f };

        // based on dimensions of the highest-resolution packed mip
        // if the screen area is less than this, then the object won't need streamed textures
        UINT m_screenAreaThreshold{ 100 };

        virtual bool ComputeVisible(
            [[maybe_unused]] float in_cotWdiv2, [[maybe_unused]] float in_cotHdiv2,
            [[maybe_unused]] const float in_zFar) { return true; }

    private:
        // container for per-class object resources
        // this allows re-use of geometry across many objects, BUT
        // FIXME? refecount or something in a real environment
        using Geometries = std::list<Geometry>;
        static Geometries m_geometries;

        bool m_createResourceViews{ true };
        void CreateResourceViews(D3D12_CPU_DESCRIPTOR_HANDLE in_baseDescriptorHandle, UINT in_srvUavCbvDescriptorSize);

        bool m_visible{ true };
        float m_screenAreaPixels{ 0 }; // only updated if visible
        UINT m_lod{ 0 }; // only updated if visible
        float ComputeScreenAreaPixels(UINT in_windowHeight, float in_fov);
        UINT ComputeLod();

        std::wstring GetAssetFullPath(const std::wstring& in_filename);

        BaseObject(const BaseObject&) = delete;
        BaseObject(BaseObject&&) = delete;
        BaseObject& operator=(const BaseObject&) = delete;
        BaseObject& operator=(BaseObject&&) = delete;
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
        Terrain(Scene* in_pScene);
    private:
        virtual const Geometry* GetGeometry() const override;
        static Geometry* m_pGeometry;
    };

    class Sphere : public BaseObject
    {
    public:
        virtual bool ComputeVisible(float in_cotWdiv2, float in_cotHdiv2, const float in_zFar) override;
        float GetBoundingSphereRadius() override;
    };

    class Planet : public Sphere
    {
    public:
        Planet(Scene* in_pScene);
    private:
        virtual const Geometry* GetGeometry() const override;
        static Geometry* m_pGeometry;
    };

    class Earth : public Sphere
    {
    public:
        Earth(Scene* in_pScene);
    private:
        virtual const Geometry* GetGeometry() const override;
        static Geometry* m_pGeometry;
    };


    // special render state (front face cull)
    // lower triangle count
    class Sky : public BaseObject
    {
    public:
        Sky(Scene* in_pScene);
    private:
        virtual const Geometry* GetGeometry() const override;
        static Geometry* m_pGeometry;
    };
}
