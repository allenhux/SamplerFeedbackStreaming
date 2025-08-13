//==============================================================
// Copyright © Intel Corporation
//
// SPDX-License-Identifier: MIT
// =============================================================

#pragma once

#include <random>
#include <thread>

#include "CommandLineArgs.h"
#include "SharedConstants.h"
#include "SceneObject.h"
#include "FrameEventTracing.h"
#include "AssetUploader.h"
#include "Gui.h"
#include "XetFileHeader.h"

class Scene
{
public:
    Scene(const CommandLineArgs& in_args, HWND in_hwnd);
    ~Scene();

    // draw returns false on device removed/reset
    bool Draw();
    void SetFullScreen(bool in_fullScreen)
    {
        // if the full screen state desired != current, then the window is transitioning
        // ignore requests unless not transitioning
        if (m_desiredFullScreen == m_fullScreen)
        {
            m_desiredFullScreen = in_fullScreen;
        }
    }
    bool GetFullScreen() const { return m_fullScreen; }

    void MoveView(int in_x, int in_y, int in_z);
    void RotateViewKey(int in_x, int in_y, int in_z);
    void RotateViewPixels(int in_x, int in_y);

    void ToggleUI() { m_args.m_showUI = !m_args.m_showUI; }
    void ToggleUIModeMini() { m_args.m_uiModeMini = !m_args.m_uiModeMini; }
    void ToggleFeedback() { m_args.m_showFeedbackMaps = !m_args.m_showFeedbackMaps; }
    void ToggleMinMipView() { m_args.m_visualizeMinMip = !m_args.m_visualizeMinMip; }
    void ToggleRollerCoaster() { m_args.m_cameraRollerCoaster = !m_args.m_cameraRollerCoaster; }
    void SetVisualizationMode(CommandLineArgs::VisualizationMode in_mode) { m_args.m_dataVisualizationMode = in_mode; }

    void ToggleFrustum() { m_args.m_visualizeFrustum = !m_args.m_visualizeFrustum; HandleUiToggleFrustum(); }

    void ToggleAnimation()
    {
        static float animationRate = 0;
        static float cameraAnimationRate = 0;
        if (animationRate && m_args.m_animationRate) animationRate = 0;
        if (cameraAnimationRate && m_args.m_cameraAnimationRate) cameraAnimationRate = 0;
        std::swap(animationRate, m_args.m_animationRate);
        std::swap(cameraAnimationRate, m_args.m_cameraAnimationRate);
    }
    RECT GetGuiRect();

    void ScreenShot(std::wstring& in_fileName) const;

    // may be used by Object constructors
    const CommandLineArgs& GetArgs() const { return m_args; }
    ID3D12Device8* GetDevice() const { return m_device.Get(); }
    AssetUploader& GetAssetUploader() { return m_assetUploader; }

private:
    Scene(const Scene&) = delete;
    Scene(Scene&&) = delete;
    Scene& operator=(const Scene&) = delete;
    Scene& operator=(Scene&&) = delete;
    template<typename T> using ComPtr = Microsoft::WRL::ComPtr<T>;

    std::atomic<bool> m_fullScreen{ false };
    bool m_desiredFullScreen{ false };
    UINT m_windowWidth{ 0 };
    UINT m_windowHeight{ 0 };
    // remember placement for restore from full screen
    WINDOWPLACEMENT m_windowPlacement{};
    void Resize();
    void RotateView(float in_x, float in_y, float in_z);

    CommandLineArgs m_args;

    const HWND m_hwnd{ nullptr };
    WINDOWINFO m_windowInfo{};
    bool m_windowedSupportsTearing{ false };
    bool m_deviceRemoved{ false }; // when true, resize and draw immediately exit, returning false if appicable

    ComPtr<IDXGIFactory5> m_factory;
    ComPtr<IDXGIAdapter1> m_adapter;
    ComPtr<ID3D12Device8> m_device;
    ComPtr<ID3D12CommandQueue> m_commandQueue;

    // limit the amount allocated to the number of bits for virtual memory minus a healthy amount extra
    UINT32 m_maxVirtualTiles{ 0 };
    std::atomic<UINT> m_maxNumObjects{ 0 }; // set if load hit memory limit

    static const UINT m_swapBufferCount{ SharedConstants::SWAP_CHAIN_BUFFER_COUNT };
    static const FLOAT m_clearColor[4];
    ComPtr<IDXGISwapChain3> m_swapChain;
    UINT m_frameIndex{ 0 }; // comes from swap chain
    UINT64 m_renderFenceValue{ 0 }; // also serves as a frame count
    std::vector<UINT64> m_frameFenceValues;
    ComPtr<ID3D12Fence> m_renderFence;
    HANDLE m_renderFenceEvent{ NULL };

    std::default_random_engine m_gen;

    ComPtr<ID3D12CommandAllocator> m_commandAllocators[m_swapBufferCount];
    ComPtr<ID3D12GraphicsCommandList1> m_commandList;

    ComPtr<ID3D12Resource> m_renderTargets[m_swapBufferCount];
    ComPtr<ID3D12Resource> m_colorBuffer; //  MSAA render target
    ComPtr<ID3D12Resource> m_depthBuffer;

    ComPtr<ID3D12DescriptorHeap> m_srvHeap;
    ComPtr<ID3D12DescriptorHeap> m_dsvHeap;
    ComPtr<ID3D12DescriptorHeap> m_rtvHeap;

    UINT m_rtvDescriptorSize{ 0 };
    UINT m_srvUavCbvDescriptorSize{ 0 };
    UINT m_dsvDescriptorSize{ 0 };

    DirectX::XMMATRIX m_projection;
    DirectX::XMMATRIX m_viewMatrix;
    DirectX::XMMATRIX m_viewMatrixInverse;
    void SetViewMatrix(const DirectX::XMMATRIX& m) { m_viewMatrix = m; m_viewMatrixInverse = XMMatrixInverse(nullptr, m_viewMatrix); }

    float m_aspectRatio{ 0 };
    const float m_fieldOfView{ DirectX::XM_PI / 2.0f }; // 90 degrees
    const float m_zNear{ 1.f };
    const float m_zFar{ 100000.f };

    CD3DX12_VIEWPORT m_viewport;
    CD3DX12_RECT m_scissorRect;

    struct FrameConstantData
    {
        DirectX::XMFLOAT4 g_eyePos;        // eye world position
        DirectX::XMFLOAT4 g_lightDir;      // world space light direction
        DirectX::XMFLOAT4 g_lightColor;    // RGB
        DirectX::XMFLOAT4 g_specularColor; // RGB + specular intensity

        int g_visualizeFeedback;
    };

    std::vector<FrameConstantData*> m_pFrameConstantData{ nullptr }; // left in the mapped state
    std::vector<ComPtr<ID3D12Resource>> m_frameConstantBuffers;

    std::unique_ptr<Gui> m_gui;
    Gui::ButtonChanges m_uiButtonChanges; // track changes in UI settings

    class TextureViewer* m_pTextureViewer{ nullptr };
    UINT m_maxNumTextureViewerWindows{ 0 };
    class BufferViewer* m_pMinMipMapViewer{ nullptr };
    class FrustumViewer* m_pFrustumViewer{ nullptr };

    // minimize state transitions by grouping objects by material each frame
    struct ObjectIndexPair
    {
        SceneObjects::BaseObject* pObject;
        UINT index;
    };
    typedef std::vector<ObjectIndexPair> ObjectSet;
    void DrawObjectSet(
        ID3D12GraphicsCommandList1* out_pCommandList,
        SceneObjects::DrawParams& in_drawParams,
        const ObjectSet& in_objectSet);

    void MsaaResolve();

    void WaitForGpu();

    float GetFrameTimeMs();

    bool IsDeviceOk(HRESULT in_hr);
    void CreateRenderTargets();
    void MoveToNextFrame();

    //-------------------------------
    // startup sequence (constructor)
    //-------------------------------
    void InitDebugLayer();
    void CreateDeviceWithName(std::wstring& out_adapterDescription);
    void DisableDebugLayerMessages();
    void CreateDescriptorHeaps();
    void CreateCommandQueue();
    void CreateSwapChain();
    void CreateFence();
    void CreateConstantBuffers();
    void CreateSampler();
    //-------------------------------

    // just for the sampler bias, which can be adjusted by the UI
    ComPtr<ID3D12DescriptorHeap> m_samplerHeap;
    void SetSampler();

    struct SFSManager* m_pSFSManager{ nullptr };

    void Animate(); // camera and objects
    void DrawObjects();   // draw all the objects

    void CreateTerrainViewers();
    void DeleteTerrainViewers();

    void SetDefaultView();

    //-----------------------------------
    // scene graph
    //-----------------------------------
    AssetUploader m_assetUploader;

    std::vector<class SceneObjects::BaseObject*> m_objects;

    UINT m_terrainObjectIndex{ 0 };
    SceneObjects::BaseObject* m_pTerrain{ nullptr }; // convenience pointer, do not delete or draw, this is also in m_objects.
    SceneObjects::BaseObject* m_pSky{ nullptr }; // lifetime owned by m_objects

    struct ObjectPoses
    {
        void reserve(UINT in_size) { m_positions.reserve(in_size); }
        size_t size() const { return m_positions.size(); }
        DirectX::XMMATRIX GetMatrix(std::default_random_engine& in_gen, UINT i);
        std::vector<DirectX::XMVECTOR> m_positions; // w is planet radius
    };
    ObjectPoses m_objectPoses;
    float m_universeSize{ 0 };
    void PrepareScene();

    // optimize file open/read for many files
    // m_sfsResourceDescs[i] correspond to m_args.m_textures[i]
    std::vector<SFSResourceDesc> m_sfsResourceDescs;
    void LoadResourceDesc(SFSResourceDesc& out_desc, const std::wstring& in_filename);

    void LoadSpheres(); // adjust # of objects
    std::thread m_loadObjectThread;
    enum LoadingThread : UINT
    {
        Idle = 0,
        Running = 1,
        Done = 2
    };
    std::atomic<LoadingThread> m_loadingThreadState{ Idle };
    void LoadObjectThread(UINT in_numObjects, UINT in_index, UINT in_numTilesVirtual); // async object creation
    void DeleteObjectThread(std::vector<class SceneObjects::BaseObject*> in_objects, UINT64 in_waitFenceValue);

    // each frame, update objects until timeout reached
    UINT m_queueFeedbackIndex{ 0 }; // index based on number of gpu feedback resolves per frame
    UINT m_numFeedbackObjects{ 0 }; // for statistics
    UINT m_numObjectsLoaded{ 0 }; // for UI

    using BarrierList = std::vector<D3D12_RESOURCE_BARRIER>;
    BarrierList m_aliasingBarriers; // optional barrier for performance analysis only

    //-----------------------------------
    // statistics gathering
    //-----------------------------------
    FrameEventTracing::RenderEventList m_renderThreadTimes;
    FrameEventTracing::UpdateEventList m_updateFeedbackTimes;
    std::unique_ptr<class D3D12GpuTimer> m_gpuTimer;
    std::unique_ptr<FrameEventTracing> m_csvFile{ nullptr };
    float m_gpuProcessFeedbackTimeMs{ 0 };

    UINT m_numEvictionsPreviousFrame{ 0 };
    UINT m_numUploadsPreviousFrame{ 0 };

    void StartStreamingLibrary();
    std::vector<SFSHeap*> m_sharedHeaps;

    void GatherStatistics();
    UINT m_startUploadCount{ 0 };
    UINT m_startSubmitCount{ 0 };
    UINT m_startSignalCount{ 0 };
    float m_totalTileLatencyMs{ 0 }; // per-tile upload latency. NOT the same as per-UpdateList
    CpuTimer m_cpuTimer;
    INT64 m_cpuTimerStart{ 0 };
    UINT m_numTilesVirtual{ 0 };

    void HandleUIchanges();
    bool AssetsLoaded(); // returns true if all resources are drawable. used to delay start of gathering statistics
    void StartScene();
    void DrawUI();
    void HandleUiToggleFrustum();

    void SwapCameraForDemo(bool in_capture);
};
