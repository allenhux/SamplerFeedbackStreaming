//==============================================================
// Copyright © Intel Corporation
//
// SPDX-License-Identifier: MIT
// =============================================================

#include "pch.h"
#include <ppl.h>

#include "Scene.h"
#include "D3D12GpuTimer.h"
#include "Gui.h"
#include "TextureViewer.h"
#include "BufferViewer.h"
#include "FrustumViewer.h"
#include "DebugHelper.h"
#include "WindowCapture.h"
#include "PlanetPoseGenerator.h"

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dxguid.lib")

#pragma comment(lib, "SFS.lib")

using namespace DirectX;

const FLOAT Scene::m_clearColor[4] = { 0, 0, 0.05f, 0 };

enum class DescriptorHeapOffsets
{
    FRAME_CBV0,         // b0, one for each swap chain buffer
    FRAME_CBV1,
    FRAME_CBV2,
    GUI,
    SHARED_MIN_MIP_MAP,

    NumEntries
};

#define ErrorMessage(...) { MessageBox(0, AutoString(__VA_ARGS__).str().c_str(), L"Error", MB_OK); exit(-1); }

//-----------------------------------------------------------------------------
// create device, optionally checking adapter description for e.g. "intel"
//-----------------------------------------------------------------------------
void Scene::CreateDeviceWithName(std::wstring& out_adapterDescription)
{
    auto preferredArchitecture = m_args.m_preferredArchitecture;
    std::wstring lowerCaseAdapterDesc = m_args.m_adapterDescription;

    if (lowerCaseAdapterDesc.size())
    {
        for (auto& c : lowerCaseAdapterDesc) { c = ::towlower(c); }
    }

    ComPtr<IDXGIAdapter1> adapter;
    for (UINT i = 0; m_factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i)
    {
        DXGI_ADAPTER_DESC1 desc{};
        adapter->GetDesc1(&desc);

        if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) || (std::wstring(L"Microsoft Basic Render Driver") == desc.Description))
        {
            continue;
        }

        if (lowerCaseAdapterDesc.size())
        {
            std::wstring description(desc.Description);
            for (auto& c : description) { c = ::towlower(c); }
            std::size_t found = description.find(lowerCaseAdapterDesc);
            if (found == std::string::npos)
            {
                continue;
            }
        }

        D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&m_device));

        // if we care about adapter architecture, check that UMA corresponds to integrated vs. discrete
        if (CommandLineArgs::PreferredArchitecture::NONE != preferredArchitecture)
        {
            D3D12_FEATURE_DATA_ARCHITECTURE archFeatures{};
            m_device->CheckFeatureSupport(D3D12_FEATURE_ARCHITECTURE, &archFeatures, sizeof(archFeatures));

            if (((FALSE == archFeatures.UMA) && (CommandLineArgs::PreferredArchitecture::INTEGRATED == preferredArchitecture)) ||
                ((TRUE == archFeatures.UMA) && (CommandLineArgs::PreferredArchitecture::DISCRETE == preferredArchitecture)))
            {
                // adapter does not match requirements (name and/or architecture)
                m_device = nullptr;
                continue;
            }
        }

        // adapter matches requirements (name and/or architecture), exit loop
        break;
    }

    // get the description from whichever adapter was used to create the device
    if (nullptr != m_device.Get())
    {
        DXGI_ADAPTER_DESC1 desc{};
        adapter->GetDesc1(&desc);
        out_adapterDescription = desc.Description;
    }
    else
    {
        ErrorMessage("No adapter found with name \"", m_args.m_adapterDescription, "\" or architecture \"",
            (CommandLineArgs::PreferredArchitecture::NONE == preferredArchitecture ? "none" :
                (CommandLineArgs::PreferredArchitecture::DISCRETE == preferredArchitecture ? "discrete" : "integrated")),
            "\"\n");
    }
}

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
Scene::Scene(const CommandLineArgs& in_args, HWND in_hwnd) :
    m_args(in_args), m_hwnd(in_hwnd)
    , m_frameFenceValues(m_swapBufferCount, 0)
    , m_renderThreadTimes(in_args.m_statisticsNumFrames)
    , m_updateFeedbackTimes(in_args.m_statisticsNumFrames)
{
    m_windowInfo.cbSize = sizeof(WINDOWINFO);

    UINT factoryFlags = 0;

#ifdef _DEBUG
    factoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
    InitDebugLayer();
#endif

    if (FAILED(CreateDXGIFactory2(factoryFlags, IID_PPV_ARGS(&m_factory))))
    {
        factoryFlags &= ~DXGI_CREATE_FACTORY_DEBUG;
        ThrowIfFailed(CreateDXGIFactory2(factoryFlags, IID_PPV_ARGS(&m_factory)));
    }

    std::wstring adapterDescription = m_args.m_adapterDescription;
    CreateDeviceWithName(adapterDescription);


    // does this device support sampler feedback?
    D3D12_FEATURE_DATA_D3D12_OPTIONS7 feedbackOptions{};
    m_device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS7, &feedbackOptions, sizeof(feedbackOptions));

    if (0 == feedbackOptions.SamplerFeedbackTier)
    {
        ErrorMessage(L"Sampler Feedback not supported by ", adapterDescription);
    }

    D3D12_FEATURE_DATA_D3D12_OPTIONS tileOptions{};
    m_device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &tileOptions, sizeof(tileOptions));

    if (0 == tileOptions.TiledResourcesTier)
    {
        ErrorMessage(L"Tiled Resources not supported by ", adapterDescription);
    }

    m_pGpuTimer = new D3D12GpuTimer(m_device.Get(), 8, D3D12GpuTimer::TimerType::Direct);

    // get the adapter this device was created with
    LUID adapterLUID = m_device->GetAdapterLuid();
    ThrowIfFailed(m_factory->EnumAdapterByLuid(adapterLUID, IID_PPV_ARGS(&m_adapter)));

    // descriptor sizes
    m_rtvDescriptorSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    m_srvUavCbvDescriptorSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    m_dsvDescriptorSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);

    // creation order below matters
    CreateDescriptorHeaps();
    CreateCommandQueue();
    CreateSwapChain();
    CreateFence();

    StartStreamingLibrary();

    CreateSampler();
    CreateConstantBuffers();

    m_assetUploader.Init(m_device.Get());

    PrepareScene();

    float eyePos = 100.0f;
    XMVECTOR vEyePt = XMVectorSet(eyePos, eyePos, eyePos, 1.0f);
    XMVECTOR lookAt = XMVectorSet(0.0f, 0.0f, 0.0f, 1.0f);
    XMVECTOR vUpVec = XMVectorSet(0.0f, 1.0f, 0.0f, 1.0f);

    SetViewMatrix(XMMatrixLookAtLH(vEyePt, lookAt, vUpVec));

    UINT minNumObjects = m_args.m_skyTexture.size() ? 2 : 1;
    m_pGui = new Gui(m_hwnd, m_device.Get(), m_srvHeap.Get(),
        (UINT)DescriptorHeapOffsets::GUI, m_swapBufferCount,
        SharedConstants::SWAP_CHAIN_FORMAT, adapterDescription,
        minNumObjects, m_args);

    m_pFrustumViewer = new FrustumViewer(m_device.Get(),
        SharedConstants::SWAP_CHAIN_FORMAT,
        SharedConstants::DEPTH_FORMAT,
        m_args.m_sampleCount,
        m_assetUploader);

    // statistics gathering
    if (m_args.m_timingFrameFileName.size() && (m_args.m_timingStopFrame >= m_args.m_timingStartFrame))
    {
        m_csvFile = std::make_unique<FrameEventTracing>(m_args.m_timingFrameFileName, adapterDescription);
        m_csvFile->Reserve(m_args.m_timingStopFrame - m_args.m_timingStartFrame);
    }
}

Scene::~Scene()
{
    WaitForGpu();

    if (GetSystemMetrics(SM_REMOTESESSION) == 0)
    {
        m_swapChain->SetFullscreenState(FALSE, nullptr);
    }

    ::CloseHandle(m_renderFenceEvent);
    for (auto& b : m_frameConstantBuffers)
    {
        b->Unmap(0, nullptr);
    }

    delete m_pGpuTimer;
    delete m_pGui;

    DeleteTerrainViewers();

    delete m_pFrustumViewer;

    // FIXME: The best (fastest) way to shut down is to Destroy SFS Manager first

    for (auto o : m_objects)
    {
        delete o;
    }

    for (auto h : m_sharedHeaps)
    {
        h->Destroy();
    }

    m_pSFSManager->Destroy();
}

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
RECT Scene::GetGuiRect()
{
    RECT r{};
    r.right = (LONG)m_pGui->GetWidth();
    r.bottom = (LONG)m_pGui->GetHeight();

    return r;
}

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
void Scene::MoveView(int in_x, int in_y, int in_z)
{
    float translationRate = 0.1f * GetFrameTime();

    if (0x8000 & GetKeyState(VK_SHIFT))
    {
        translationRate *= 8;
    }

    float x = in_x * translationRate;
    float y = in_y * translationRate;
    float z = in_z * -translationRate;
    XMMATRIX translation = XMMatrixTranslation(x, y, z);

    SetViewMatrix(XMMatrixMultiply(m_viewMatrix, translation));
}

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
void Scene::RotateView(float in_x, float in_y, float in_z)
{
    XMMATRIX rotation;

    // NOTE: locking the "up" axis feels great when navigating the terrain
    // however, it breaks the controls when flying to other planets
    rotation = XMMatrixRotationRollPitchYaw(in_x, in_y, in_z);
    SetViewMatrix(XMMatrixMultiply(m_viewMatrix, rotation));

    if (m_args.m_cameraUpLock)
    {
        SetViewMatrix(XMMatrixLookToLH(m_viewMatrixInverse.r[3], m_viewMatrixInverse.r[2], XMVectorSet(0, 1, 0, 1)));
    }
}

void Scene::RotateViewKey(int in_x, int in_y, int in_z)
{
    float rotationRate = 0.001f * GetFrameTime();
    float x = in_x * -rotationRate;
    float y = in_y * rotationRate;
    float z = in_z * -rotationRate;
    RotateView(x, y, z);
}

void Scene::RotateViewPixels(int in_x, int in_y)
{
    float xRadians = m_fieldOfView / m_viewport.Width;
    float x = float(in_x) * xRadians;
    float yRadians = xRadians * m_viewport.Width / m_viewport.Height;
    float y = float(in_y) * yRadians;
    RotateView(y, x, 0);
}

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
float Scene::GetFrameTime()
{
    return 1000.0f * m_renderThreadTimes.GetAverageTotal();
}

//-----------------------------------------------------------------------------
// common behavior for device removed/reset
//-----------------------------------------------------------------------------
bool Scene::IsDeviceOk(HRESULT in_hr)
{
    bool success = true;
    if ((DXGI_ERROR_DEVICE_REMOVED == in_hr) || (DXGI_ERROR_DEVICE_RESET == in_hr))
    {
        //HRESULT hr = m_device->GetDeviceRemovedReason();
        m_deviceRemoved = true;
        success = false;
    }
    else
    {
        ThrowIfFailed(in_hr);
    }
    return success;
}

//-----------------------------------------------------------------------------
// handle in/out of fullscreen immediately. defer render target size changes
// FIXME: 1st transition to full-screen on multi-gpu, app disappears (?) - hit ESC and try again
// FIXME: full-screen does not choose the nearest display for the associated adapter, it chooses the 1st
//-----------------------------------------------------------------------------
void Scene::Resize()
{
    if (m_fullScreen != m_desiredFullScreen)
    {
        WaitForGpu();

        // can't full screen with remote desktop
        bool canFullScreen = (GetSystemMetrics(SM_REMOTESESSION) == 0);

        if (m_desiredFullScreen)
        {
            // remember the current placement so we can restore via vk_esc
            GetWindowPlacement(m_hwnd, &m_windowPlacement);

            // take the first attached monitor
            // FIXME? could search for the nearest monitor.
            MONITORINFO monitorInfo;
            monitorInfo.cbSize = sizeof(monitorInfo);
            GetMonitorInfo(MonitorFromWindow(m_hwnd, MONITOR_DEFAULTTONEAREST), &monitorInfo);

            ComPtr<IDXGIOutput> dxgiOutput;
            HRESULT result = m_adapter->EnumOutputs(0, &dxgiOutput);
            if (SUCCEEDED(result))
            {
                if (canFullScreen)
                {
                    ThrowIfFailed(m_swapChain->SetFullscreenState(true, dxgiOutput.Get()));
                }
                // FIXME? borderless window would be nice when using remote desktop
            }
            else // enumerate may fail when multi-gpu and cloning displays
            {
                canFullScreen = false;
                auto width = ::GetSystemMetrics(SM_CXSCREEN);
                auto height = ::GetSystemMetrics(SM_CYSCREEN);
                SetWindowPos(m_hwnd, NULL, 0, 0, width, height, SWP_NOZORDER | SWP_NOACTIVATE);
            }
        }
        else
        {
            if (canFullScreen)
            {
                ThrowIfFailed(m_swapChain->SetFullscreenState(false, nullptr));
            }
            // when leaving full screen, the previous window state isn't restored by the OS
            // however, we saved it earlier...
            int left = m_windowPlacement.rcNormalPosition.left;
            int top = m_windowPlacement.rcNormalPosition.top;
            int width = m_windowPlacement.rcNormalPosition.right - left;
            int height = m_windowPlacement.rcNormalPosition.bottom - top;
            SetWindowPos(m_hwnd, NULL, left, top, width, height, SWP_SHOWWINDOW);
        }
        m_fullScreen = m_desiredFullScreen;
    }

    RECT rect{};
    GetClientRect(m_hwnd, &rect);
    UINT width = rect.right - rect.left;
    UINT height = rect.bottom - rect.top;

    // ignore resize events for 0-sized window
    // not a fatal error. just ignore it.
    if ((0 == height) || (0 == width))
    {
        return;
    }

    if ((width != m_windowWidth) || (height != m_windowHeight))
    {
        m_windowWidth = width;
        m_windowHeight = height;

        WaitForGpu();

        m_viewport = CD3DX12_VIEWPORT(0.0f, 0.0f, static_cast<FLOAT>(width), static_cast<FLOAT>(height));
        m_scissorRect = CD3DX12_RECT(0, 0, width, height);
        m_aspectRatio = m_viewport.Width / m_viewport.Height;
        m_projection = DirectX::XMMatrixPerspectiveFovLH(m_fieldOfView / 2, m_aspectRatio, m_zNear, m_zFar);

        for (UINT i = 0; i < m_swapBufferCount; i++)
        {
            m_renderTargets[i] = nullptr;
        }

        UINT flags = 0;
        if (m_windowedSupportsTearing)
        {
            flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
        }
        HRESULT hr = m_swapChain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, flags);

        if (IsDeviceOk(hr))
        {
            // Create a RTV for each frame.
            CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle(m_rtvHeap->GetCPUDescriptorHandleForHeapStart());
            for (UINT i = 0; i < m_swapBufferCount; i++)
            {
                ThrowIfFailed(m_swapChain->GetBuffer(i, IID_PPV_ARGS(&m_renderTargets[i])));
                m_device->CreateRenderTargetView(m_renderTargets[i].Get(), nullptr, rtvHandle);
                rtvHandle.Offset(1, m_rtvDescriptorSize);
            }

            CreateRenderTargets();

            m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();

            // UI uses window dimensions
            m_args.m_windowWidth = width;
            m_args.m_windowHeight = height;
        }
    }
}

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
void Scene::CreateDescriptorHeaps()
{
    // shader resource view (SRV) heap for e.g. textures
    D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc = {};
    srvHeapDesc.NumDescriptors = (UINT)DescriptorHeapOffsets::NumEntries +
        (m_args.m_maxNumObjects * (UINT)SceneObjects::Descriptors::NumEntries);
    srvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ThrowIfFailed(m_device->CreateDescriptorHeap(&srvHeapDesc, IID_PPV_ARGS(&m_srvHeap)));
    m_srvHeap->SetName(L"m_srvHeap");

    // render target view heap
    // NOTE: we have an MSAA target plus a swap chain, so m_swapBufferCount + 1
    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
    rtvHeapDesc.NumDescriptors = m_swapBufferCount + 1;
    rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    ThrowIfFailed(m_device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&m_rtvHeap)));
    m_srvHeap->SetName(L"m_rtvHeap");

    // depth buffer view heap
    D3D12_DESCRIPTOR_HEAP_DESC dsvHeapDesc = {};
    dsvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    dsvHeapDesc.NumDescriptors = 1;
    ThrowIfFailed(m_device->CreateDescriptorHeap(&dsvHeapDesc, IID_PPV_ARGS(&m_dsvHeap)));
    m_dsvHeap->SetName(L"m_dsvHeap");
}

//-----------------------------------------------------------------------------
// Create synchronization objects
//-----------------------------------------------------------------------------
void Scene::CreateFence()
{
    ThrowIfFailed(m_device->CreateFence(
        m_renderFenceValue,
        D3D12_FENCE_FLAG_NONE,
        IID_PPV_ARGS(&m_renderFence)));

    // Create an event handle to use for frame synchronization.
    m_renderFenceEvent = ::CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (m_renderFenceEvent == nullptr)
    {
        ThrowIfFailed(HRESULT_FROM_WIN32(GetLastError()));
    }
}

//-----------------------------------------------------------------------------
// creates queue, direct command list, and command allocators
//-----------------------------------------------------------------------------
void Scene::CreateCommandQueue()
{
    // Describe and create the command queue.
    D3D12_COMMAND_QUEUE_DESC queueDesc = {};
    queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ThrowIfFailed(m_device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&m_commandQueue)));
    m_commandQueue->SetName(L"m_commandQueue");

    for (UINT i = 0; i < m_swapBufferCount; i++)
    {
        ThrowIfFailed(m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_commandAllocators[i])));
        m_commandAllocators[i]->SetName(AutoString("m_commandAllocators #", i).str().c_str());
    }

    m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_commandAllocators[0].Get(), nullptr, IID_PPV_ARGS(&m_commandList));
    m_commandList->SetName(L"m_commandList");
    m_commandList->Close();
}

//-----------------------------------------------------------------------------
// note creating the swap chain requires a command queue
// hence, if the command queue changes, we must re-create the swap chain
// command queue can change if we toggle the Intel command queue extension
//-----------------------------------------------------------------------------
void Scene::CreateSwapChain()
{
    BOOL allowTearing = FALSE;
    const HRESULT result = m_factory->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allowTearing, sizeof(allowTearing));
    m_windowedSupportsTearing = SUCCEEDED(result) && allowTearing;

    GetWindowInfo(m_hwnd, &m_windowInfo);

    // Describe and create the swap chain.
    DXGI_SWAP_CHAIN_DESC1 swapChainDesc = {};
    swapChainDesc.BufferCount = SharedConstants::SWAP_CHAIN_BUFFER_COUNT;
    swapChainDesc.Width = m_windowInfo.rcClient.right - m_windowInfo.rcClient.left;
    swapChainDesc.Height = m_windowInfo.rcClient.bottom - m_windowInfo.rcClient.top;
    swapChainDesc.Format = SharedConstants::SWAP_CHAIN_FORMAT;
    swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    swapChainDesc.SampleDesc.Count = 1;
    swapChainDesc.Flags = 0;

    if (m_windowedSupportsTearing)
    {
        swapChainDesc.Flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
    }

    DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pFullScreenDesc = nullptr;

    // if full screen mode, launch into the current settings
    DXGI_SWAP_CHAIN_FULLSCREEN_DESC fullScreenDesc = {};
    IDXGIOutput* pOutput = nullptr;

    ComPtr<IDXGISwapChain1> swapChain;
    ThrowIfFailed(m_factory->CreateSwapChainForHwnd(m_commandQueue.Get(), m_hwnd,
        &swapChainDesc, pFullScreenDesc, pOutput, &swapChain));

    /*
    want full screen with tearing.
    from MSDN, DXGI_PRESENT_ALLOW_TEARING:
    - The swap chain must be created with the DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING flag.
    - It can only be used in windowed mode.
    - To use this flag in full screen Win32 apps, the application should present to a fullscreen borderless window
    and disable automatic ALT+ENTER fullscreen switching using IDXGIFactory::MakeWindowAssociation.
     */
    ThrowIfFailed(m_factory->MakeWindowAssociation(m_hwnd, DXGI_MWA_NO_WINDOW_CHANGES | DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_PRINT_SCREEN));

    ThrowIfFailed(swapChain.As(&m_swapChain));

    m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();
}

//-----------------------------------------------------------------------------
// Enable the D3D12 debug layer.
//-----------------------------------------------------------------------------
void Scene::InitDebugLayer()
{
    OutputDebugString(L"<<< WARNING: DEBUG LAYER ENABLED >>>\n");
    {
        ID3D12Debug1* pDebugController = nullptr;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&pDebugController))))
        {
            //pDebugController->SetEnableGPUBasedValidation(TRUE);
            pDebugController->EnableDebugLayer();
            pDebugController->Release();
        }
    }
}

//-----------------------------------------------------------------------------
// modified next-frame logic to return a handle if a wait is required.
// NOTE: be sure to check for non-null handle before WaitForSingleObjectEx() (or equivalent)
//-----------------------------------------------------------------------------
void Scene::MoveToNextFrame()
{
    // Assign the current fence value to the current frame.
    m_frameFenceValues[m_frameIndex] = m_renderFenceValue;

    // Signal and increment the fence value.
    ThrowIfFailed(m_commandQueue->Signal(m_renderFence.Get(), m_renderFenceValue));
    m_renderFenceValue++;

    // Update the frame index.
    m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();

    // If the next frame is not ready to be rendered yet, wait until it is ready.
    if (m_renderFence->GetCompletedValue() < m_frameFenceValues[m_frameIndex])
    {
        ThrowIfFailed(m_renderFence->SetEventOnCompletion(m_frameFenceValues[m_frameIndex], m_renderFenceEvent));
        WaitForSingleObject(m_renderFenceEvent, INFINITE);
    }
}

//-----------------------------------------------------------------------------
// Wait for pending GPU work to complete.
// not interacting with swap chain.
//-----------------------------------------------------------------------------
void Scene::WaitForGpu()
{
    // Add a signal command to the queue.
    ThrowIfFailed(m_commandQueue->Signal(m_renderFence.Get(), m_renderFenceValue));

    // Instruct the fence to set the event object when the signal command completes.
    ThrowIfFailed(m_renderFence->SetEventOnCompletion(m_renderFenceValue, m_renderFenceEvent));
    m_renderFenceValue++;

    // Wait until the signal command has been processed.
    WaitForSingleObject(m_renderFenceEvent, INFINITE);
}

//-----------------------------------------------------------------------------
// initialize SFSManager
//-----------------------------------------------------------------------------
void Scene::StartStreamingLibrary()
{
    SFSManagerDesc desc;
    desc.m_pDirectCommandQueue = m_commandQueue.Get();
    desc.m_maxNumCopyBatches = m_args.m_numStreamingBatches;
    desc.m_stagingBufferSizeMB = m_args.m_stagingSizeMB;
    desc.m_maxTileMappingUpdatesPerApiCall = m_args.m_maxTileUpdatesPerApiCall;
    desc.m_swapChainBufferCount = SharedConstants::SWAP_CHAIN_BUFFER_COUNT;
    desc.m_minNumUploadRequests = m_args.m_minNumUploadRequests;
    desc.m_useDirectStorage = m_args.m_useDirectStorage;
    desc.m_threadPriority = (SFSManagerDesc::ThreadPriority)m_args.m_threadPriority;
    desc.m_resolveHeapSizeMB = m_args.m_resolveHeapSizeMB;
    desc.m_evictionDelay = m_args.m_evictionDelay;
    desc.m_traceCaptureMode = m_args.m_captureTrace;

    m_pSFSManager = SFSManager::Create(desc);

    // create 1 or more heaps to contain our StreamingResources
    for (UINT i = 0; i < m_args.m_numHeaps; i++)
    {
        m_sharedHeaps.push_back(m_pSFSManager->CreateHeap(m_args.m_streamingHeapSize));
    }
}

//-----------------------------------------------------------------------------
// scene and texture fixup,
// precompute position and radius of all objects
//-----------------------------------------------------------------------------
void Scene::PrepareScene()
{
    // is terrain among the textures in the media directory?
    // this logic will find a texture in the media directory that includes a substring, like "terrain"
    if (m_args.m_terrainTexture.size())
    {
        for (auto i = m_args.m_textures.begin(); i != m_args.m_textures.end(); i++)
        {
            if (std::wstring::npos != i->find(m_args.m_terrainTexture))
            {
                m_args.m_terrainTexture = *i;
                break;
            }
        }
        if (std::filesystem::exists(m_args.m_terrainTexture))
        {
            m_args.m_terrainTexture = std::filesystem::absolute(m_args.m_terrainTexture);
        }
        else
        {
            m_args.m_terrainTexture.clear();
        }
    }

    // is there a sky? is it among the textures in the media directory?
    // this logic will find a texture in the media directory that includes a substring, like "sky"
    if (m_args.m_skyTexture.size())
    {
        for (auto i = m_args.m_textures.begin(); i != m_args.m_textures.end(); i++)
        {
            if (std::wstring::npos != i->find(m_args.m_skyTexture))
            {
                m_args.m_skyTexture = *i;
                m_args.m_textures.erase(i);
                break;
            }
        }
        if (std::filesystem::exists(m_args.m_skyTexture))
        {
            m_args.m_skyTexture = std::filesystem::absolute(m_args.m_skyTexture);
            m_args.m_numSpheres = std::max(m_args.m_numSpheres, 2);
        }
        else
        {
            m_args.m_skyTexture.clear();
        }
    }

    // is there an earth? is it among the textures in the media directory?
    // if so, move it to the beginning and clear it. always draw earth instead of texture 0.
    // this logic will find a texture in the media directory that includes a substring, like "earth"
    if (m_args.m_earthTexture.size())
    {
        for (auto i = m_args.m_textures.begin(); i != m_args.m_textures.end(); i++)
        {
            if (std::wstring::npos != i->find(m_args.m_earthTexture))
            {
                m_args.m_earthTexture = *i;
                *i = m_args.m_textures.back();
                m_args.m_textures.resize(m_args.m_textures.size() - 1);
                break;
            }
        }
        if (std::filesystem::exists(m_args.m_earthTexture))
        {
            m_args.m_earthTexture = std::filesystem::absolute(m_args.m_earthTexture);
            m_args.m_textures.push_back(m_args.m_earthTexture);
            // move earth texture to first position in array to simplify loading later
            std::swap(m_args.m_textures[0], m_args.m_textures.back());
        }
        else
        {
            m_args.m_earthTexture.clear();
        }
    }

    // precompute poses
    m_gen.seed(42);

    float minRadius = (float)m_args.m_terrainParams.m_terrainSideSize;

    // start with a tiny universe. will be computed accurately below
    m_universeSize = 2 * minRadius;

    float range = SharedConstants::SPHERE_RADIUS * (SharedConstants::MAX_SPHERE_SCALE - 1);
    float midPoint = .5f * range;
    float stdDev = range / 5.f;
    std::normal_distribution<float>scaleDis(midPoint, stdDev);

    PlanetPoseGenerator::Settings settings{
		.numPoses = m_args.m_maxNumObjects,
        .gap = SharedConstants::SPHERE_RADIUS,
        .minDistance = (float)m_args.m_terrainParams.m_terrainSideSize,
        .minRadius = SharedConstants::SPHERE_RADIUS,
        .maxRadius = SharedConstants::SPHERE_RADIUS * SharedConstants::MAX_SPHERE_SCALE
    };
	PlanetPoseGenerator poseGenerator(settings);
    m_universeSize = poseGenerator.GeneratePoses(m_objectPoses.m_matrix, m_objectPoses.m_radius);

    // load texture file headers
    m_sfsResourceDescs.resize(m_args.m_textures.size());
    UINT i = 0;
    for (const auto& s : m_args.m_textures)
    {
        LoadResourceDesc(m_sfsResourceDescs[i], s);
        i++;
    }

    //--------------------------
    // create one-off objects
    //--------------------------

    { // --- terrain ---
        m_terrainObjectIndex = (UINT)m_objects.size();
        UINT heapIndex = m_terrainObjectIndex % m_sharedHeaps.size();
        SFSHeap* pHeap = m_sharedHeaps[heapIndex];
        m_pTerrain = new SceneObjects::Terrain(this);
        SFSResourceDesc resourceDesc;
        LoadResourceDesc(resourceDesc, m_args.m_terrainTexture);
        m_pTerrain->SetResource(m_pSFSManager->CreateResource(resourceDesc, pHeap, m_args.m_terrainTexture));
        m_objects.push_back(m_pTerrain);
    }

    if (m_args.m_skyTexture.size()) // --- sky ---
    {
        UINT objectIndex = (UINT)m_objects.size();
        UINT heapIndex = objectIndex % m_sharedHeaps.size();
        SFSHeap* pHeap = m_sharedHeaps[heapIndex];
        m_pSky = new SceneObjects::Sky(this);
        SFSResourceDesc resourceDesc;
        LoadResourceDesc(resourceDesc, m_args.m_skyTexture);
        m_pSky->SetResource(m_pSFSManager->CreateResource(resourceDesc, pHeap, m_args.m_skyTexture));
        float scale = m_universeSize * 2; // NOTE: expects universe size to not change
        m_pSky->GetModelMatrix() = DirectX::XMMatrixScaling(scale, scale, scale);
        m_objects.push_back(m_pSky);
    }
}

//-----------------------------------------------------------------------------
// read file contents into SFSResourceDesc
//-----------------------------------------------------------------------------
void Scene::LoadResourceDesc(SFSResourceDesc& out_desc, const std::wstring& in_filename)
{
    std::ifstream inFile(in_filename.c_str(), std::ios::binary);
    ASSERT(!inFile.fail()); // File doesn't exist?

    XetFileHeader fileHeader;
    inFile.read((char*)&fileHeader, sizeof(fileHeader));
    ASSERT(inFile.good()); // Unexpected Error reading header

    out_desc =
    {
        .m_width = fileHeader.m_ddsHeader.width,
        .m_height = fileHeader.m_ddsHeader.height,
        .m_textureFormat = (UINT)fileHeader.m_extensionHeader.dxgiFormat,
        .m_compressionFormat = fileHeader.m_compressionFormat
    };
 
    out_desc.m_mipInfo =
    {
        .m_numStandardMips = fileHeader.m_mipInfo.m_numStandardMips,
        .m_numTilesForStandardMips = fileHeader.m_mipInfo.m_numTilesForStandardMips,
        .m_numPackedMips = fileHeader.m_mipInfo.m_numPackedMips,
        .m_numTilesForPackedMips = fileHeader.m_mipInfo.m_numTilesForPackedMips,
        .m_numUncompressedBytesForPackedMips = fileHeader.m_mipInfo.m_numUncompressedBytesForPackedMips
    };

    ASSERT(fileHeader.m_magic == XetFileHeader::GetMagic()); // valid XET file?
    ASSERT(fileHeader.m_version = XetFileHeader::GetVersion()); // correct XET version?

    std::vector<XetFileHeader::SubresourceInfo> mipInfo;
    mipInfo.resize(fileHeader.m_ddsHeader.mipMapCount);
    inFile.read((char*)mipInfo.data(), mipInfo.size() * sizeof(mipInfo[0]));
    ASSERT(inFile.good()); // Unexpected Error reading subresource info

    out_desc.m_standardMipInfo.resize(out_desc.m_mipInfo.m_numStandardMips);
    for (UINT i = 0; i < out_desc.m_mipInfo.m_numStandardMips; i++)
    {
        out_desc.m_standardMipInfo[i] =
        {
        .m_widthTiles = mipInfo[i].m_standardMipInfo.m_widthTiles,
        .m_heightTiles = mipInfo[i].m_standardMipInfo.m_heightTiles,
        .m_depthTiles = mipInfo[i].m_standardMipInfo.m_depthTiles,

        // convenience value, can be computed from sum of previous subresource dimensions
        .m_subresourceTileIndex = mipInfo[i].m_standardMipInfo.m_subresourceTileIndex
        };
    }

    std::vector<XetFileHeader::TileData> tileData;
    tileData.resize(fileHeader.m_mipInfo.m_numTilesForStandardMips + 1); // plus 1 for the packed mips offset & size
    inFile.read((char*)tileData.data(), tileData.size() * sizeof(tileData[0]));
    ASSERT(inFile.good()); // Unexpected Error reading packed mip info
    inFile.close();

    out_desc.m_packedMipData =
    {
        .m_offset = tileData.back().m_offset,
        .m_numBytes = tileData.back().m_numBytes
    };

    out_desc.m_tileData.resize(out_desc.m_mipInfo.m_numTilesForStandardMips);
    for (UINT i = 0; i < out_desc.m_mipInfo.m_numTilesForStandardMips; i++)
    {
        out_desc.m_tileData[i] =
        {
            .m_offset = tileData[i].m_offset,
            .m_numBytes = tileData[i].m_numBytes
        };
    }
}

//-----------------------------------------------------------------------------
// create MSAA color and depth targets
//-----------------------------------------------------------------------------
void Scene::CreateRenderTargets()
{
    D3D12_DEPTH_STENCIL_VIEW_DESC dsv = {};

    D3D12_RESOURCE_DESC desc = CD3DX12_RESOURCE_DESC::Tex2D(
        SharedConstants::SWAP_CHAIN_FORMAT,
        (UINT64)m_viewport.Width, (UINT)m_viewport.Height,
        1, 1, m_args.m_sampleCount);

    // create color buffer
    {
        D3D12_RESOURCE_DESC colorDesc = desc;
        colorDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

        D3D12_CLEAR_VALUE clearValue = {};
        memcpy(&clearValue.Color, &m_clearColor, sizeof(m_clearColor));
        clearValue.Format = desc.Format;

        const auto heapProperties = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
        ThrowIfFailed(m_device->CreateCommittedResource(
            &heapProperties,
            D3D12_HEAP_FLAG_NONE,
            &colorDesc,
            D3D12_RESOURCE_STATE_RENDER_TARGET,
            &clearValue,
            IID_PPV_ARGS(&m_colorBuffer)));
    }

    // create depth buffer
    {
        D3D12_RESOURCE_DESC depthDesc = desc;
        depthDesc.Format = SharedConstants::DEPTH_FORMAT;
        depthDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

        D3D12_CLEAR_VALUE clearValue = {};
        clearValue.DepthStencil.Depth = 1.0f;
        clearValue.Format = SharedConstants::DEPTH_FORMAT;

        const auto heapProperties = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
        ThrowIfFailed(m_device->CreateCommittedResource(
            &heapProperties,
            D3D12_HEAP_FLAG_NONE,
            &depthDesc,
            D3D12_RESOURCE_STATE_DEPTH_WRITE,
            &clearValue,
            IID_PPV_ARGS(&m_depthBuffer)));

        dsv.Format = depthDesc.Format;
    }

    if (1 == m_args.m_sampleCount)
    {
        dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    }
    else
    {
        dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DMS;
    }

    CD3DX12_CPU_DESCRIPTOR_HANDLE rtvDescriptor(m_rtvHeap->GetCPUDescriptorHandleForHeapStart(), m_swapBufferCount, m_rtvDescriptorSize);
    CD3DX12_CPU_DESCRIPTOR_HANDLE dsvDescriptor(m_dsvHeap->GetCPUDescriptorHandleForHeapStart());

    m_device->CreateRenderTargetView(m_colorBuffer.Get(), nullptr, rtvDescriptor);
    m_device->CreateDepthStencilView(m_depthBuffer.Get(), &dsv, dsvDescriptor);
}

//-----------------------------------------------------------------------------
// 1 static and 1 dynamic constant buffers
// NOTE: do this within LoadAssets, as it will create a staging resource and rely on command list submission
//-----------------------------------------------------------------------------
void Scene::CreateConstantBuffers()
{
    // dynamic constant buffer
    {
        UINT bufferSize = sizeof(FrameConstantData);
        const UINT multipleSize = 256; // required
        bufferSize = (bufferSize + multipleSize - 1) / multipleSize;
        bufferSize *= multipleSize;

        const auto heapProperties = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
        const auto resourceDesc = CD3DX12_RESOURCE_DESC::Buffer(bufferSize);

        m_frameConstantBuffers.resize(m_swapBufferCount);
        m_pFrameConstantData.resize(m_swapBufferCount);

        for (UINT i = 0; i < m_swapBufferCount; i++)
        {
            ThrowIfFailed(m_device->CreateCommittedResource(
                &heapProperties,
                D3D12_HEAP_FLAG_NONE,
                &resourceDesc,
                D3D12_RESOURCE_STATE_GENERIC_READ,
                nullptr,
                IID_PPV_ARGS(&m_frameConstantBuffers[i])));

            CD3DX12_RANGE readRange(0, bufferSize);
            ThrowIfFailed(m_frameConstantBuffers[i]->Map(0, &readRange, reinterpret_cast<void**>(&m_pFrameConstantData[i])));

            m_pFrameConstantData[i]->g_lightDir = XMFLOAT4(-0.538732767f, 0.787301660f, 0.299871892f, 0);
            XMStoreFloat4(&m_pFrameConstantData[i]->g_lightDir, XMVector4Normalize(XMLoadFloat4(&m_pFrameConstantData[i]->g_lightDir)));
            m_pFrameConstantData[i]->g_lightColor = XMFLOAT4(1, 1, 1, 1);
            m_pFrameConstantData[i]->g_specularColor = XMFLOAT4(1, 1, 1, 50.f);

            D3D12_CONSTANT_BUFFER_VIEW_DESC constantBufferView = {};
            constantBufferView.SizeInBytes = bufferSize;
            constantBufferView.BufferLocation = m_frameConstantBuffers[i]->GetGPUVirtualAddress();
            m_device->CreateConstantBufferView(&constantBufferView, CD3DX12_CPU_DESCRIPTOR_HANDLE(
                m_srvHeap->GetCPUDescriptorHandleForHeapStart(), i + (UINT)DescriptorHeapOffsets::FRAME_CBV0, m_srvUavCbvDescriptorSize));
        }
    }
}

//-----------------------------------------------------------------------------
    // the sampler, which can be adjusted by the UI
//-----------------------------------------------------------------------------
void Scene::CreateSampler()
{
    D3D12_DESCRIPTOR_HEAP_DESC desc = {};
    desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
    desc.NumDescriptors = 1; // only need the one for the single feedback map
    desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ThrowIfFailed(m_device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&m_samplerHeap)));
    m_samplerHeap->SetName(L"m_samplerHeap");
}

//-----------------------------------------------------------------------------
// progressively over multiple frames, if there are many
//-----------------------------------------------------------------------------
void Scene::LoadSpheres()
{
    bool updateNumTiles = (m_objects.size() != (UINT)m_args.m_numSpheres);

    if (m_objects.size() < (UINT)m_args.m_numSpheres)
    {
        UINT textureIndex = (UINT)m_objects.size();
        while (m_objects.size() < (UINT)m_args.m_numSpheres)
        {
            // this object's index-to-be
            UINT objectIndex = (UINT)m_objects.size();

            // put this resource into one of our shared heaps
            UINT heapIndex = objectIndex % m_sharedHeaps.size();
            auto pHeap = m_sharedHeaps[heapIndex];

            // grab the next texture
            UINT fileIndex = textureIndex % m_args.m_textures.size();
            textureIndex++;
            const auto& textureFilename = m_args.m_textures[fileIndex];

            SceneObjects::BaseObject* o = nullptr;

            // earth
            if ((0 == fileIndex) && m_args.m_earthTexture.size())
            {
                o = new SceneObjects::Earth(this);
            }
            // planet
            else
            {
                o = new SceneObjects::Planet(this);
                static std::uniform_real_distribution<float> dis(-1.f, 1.f);
                o->SetAxis(DirectX::XMVector3NormalizeEst(DirectX::XMVectorSet(dis(m_gen), dis(m_gen), dis(m_gen), 0)));
            }
            o->SetResource(m_pSFSManager->CreateResource(m_sfsResourceDescs[fileIndex], pHeap, textureFilename));
            o->GetModelMatrix() = m_objectPoses.m_matrix[objectIndex];
            m_objects.push_back(o);
        }
    } // end if adding objects
    else if (m_objects.size() > (UINT)m_args.m_numSpheres) // else evict objects
    {
        WaitForGpu();
        while (m_objects.size() > (UINT)m_args.m_numSpheres)
        {
            SceneObjects::BaseObject* pObject = m_objects.back();

            if (m_pTerrain == pObject)
            {
                DeleteTerrainViewers();
                m_pTerrain = nullptr;
            }

            if (m_pSky == pObject)
            {
                m_pSky = nullptr;
            }

            delete pObject;
            m_objects.resize(m_objects.size() - 1);
        }
    }
    if (updateNumTiles)
    {
        m_numTilesVirtual = 0;
        for (auto& o : m_objects)
        {
            m_numTilesVirtual += o->GetStreamingResource()->GetNumTilesVirtual();
        }
    }
}

//-----------------------------------------------------------------------------
// sampler used for accessing feedback map
// can change dynamically with UI slider
//-----------------------------------------------------------------------------
void Scene::SetSampler()
{
    D3D12_SAMPLER_DESC samplerDesc{};
    samplerDesc.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samplerDesc.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samplerDesc.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;

    if (m_args.m_anisotropy < 2)
    {
        samplerDesc.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    }
    else
    {
        samplerDesc.MaxAnisotropy = std::min((UINT)D3D12_MAX_MAXANISOTROPY, m_args.m_anisotropy);
        samplerDesc.Filter = D3D12_FILTER_ANISOTROPIC;
    }

    samplerDesc.MaxLOD = FLT_MAX;
    samplerDesc.MipLODBias = m_args.m_lodBias;

    CD3DX12_CPU_DESCRIPTOR_HANDLE descHandle(m_samplerHeap->GetCPUDescriptorHandleForHeapStart());
    m_device->CreateSampler(&samplerDesc, descHandle);
}

//----------------------------------------------------------
// draw objects grouped by same pipeline state
//----------------------------------------------------------
void Scene::DrawObjectSet(ID3D12GraphicsCommandList1* out_pCommandList,
    SceneObjects::DrawParams& in_drawParams,
    const ObjectSet& in_objectSet)
{
    auto pObject = in_objectSet[0].pObject;

    // these objects all share pipeline state
    // if feedback is enabled, 2 things:
    // 1. tell the tile update manager to queue a readback of the resolved feedback
    // 2. draw the object with a shader that calls WriteSamplerFeedback()
    out_pCommandList->SetGraphicsRootSignature(pObject->GetRootSignature());
    out_pCommandList->SetPipelineState(pObject->GetPipelineState());

    pObject->SetCommonGraphicsState(out_pCommandList, in_drawParams);

    for (auto& o : in_objectSet)
    {
        in_drawParams.m_descriptorHeapOffset = (o.index * (INT)SceneObjects::Descriptors::NumEntries);
        o.pObject->Draw(out_pCommandList, in_drawParams);
    }
}

//-----------------------------------------------------------------------------
// draw all objects
// uses the min-mip-map created using Sampler Feedback on the GPU
// to recommend updates to the internal memory map managed by the CPU
// returns an actual occupancy min-mip-map
// 
// feedback may only written for a subset of resources depending on GPU feedback timeout
//-----------------------------------------------------------------------------
void Scene::DrawObjects()
{
    if (0 == m_objects.size())
    {
        return;
    }

    std::unordered_map<ID3D12PipelineState*, ObjectSet> frameObjectSets;
    ObjectSet skyObjectSet;

    //------------------------------------------------------------------------------------
    // set feedback state on each object
    // objects with feedback enabled will queue feedback resolve on the SFSManager
    // objects without feedback enabled will not call WriteSamplerFeedback()
    //------------------------------------------------------------------------------------
    {
        const float timePerTexel = m_pSFSManager->GetGpuTexelsPerMs();
        const UINT maxNumFeedbacks = m_pSFSManager->GetMaxNumFeedbacksPerFrame();
        UINT texelLimit = (UINT)(m_args.m_maxGpuFeedbackTimeMs * timePerTexel);
        // early timing values aren't valid, so clamp to a minimal non-0 value
        texelLimit = std::max(texelLimit, (UINT)1000);
        if (!m_args.m_enableTileUpdates) { texelLimit = 0; }
        UINT numTexels = 0;

        // round-robin which objects get feedback
        m_queueFeedbackIndex = m_queueFeedbackIndex % m_objects.size();

        // loop over n objects starting with the range that we want to get sampler feedback from, then wrap around.
        UINT numObjects = m_queueFeedbackIndex + (UINT)m_objects.size();
        m_numFeedbackObjects = 0;
        for (UINT i = m_queueFeedbackIndex; i < numObjects; i++)
        {
            UINT objectIndex = i % (UINT)m_objects.size();
            auto o = m_objects[objectIndex];
            if (!o->Drawable()) { continue; }

            bool isVisible = o->IsVisible();
            // FIXME: magic number. idea is, no need to stream texture data to tiny objects
            bool isTiny = o->GetScreenAreaPixels() < 50;

            // get sampler feedback for this object?
            bool queueFeedback = isVisible && (!isTiny)
                && (numTexels < texelLimit)
                && (m_numFeedbackObjects < maxNumFeedbacks);
            queueFeedback = queueFeedback || m_args.m_updateEveryObjectEveryFrame;

            bool evict = !isVisible || isTiny;

            if (isVisible)
            {
                o->SetFeedbackEnabled(queueFeedback); // must set if false or true
                // group objects by material (PSO)
                if (m_pSky != o)
                {
                    frameObjectSets[o->GetPipelineState()].push_back({ o, objectIndex });
                }
                else
                {
                    skyObjectSet.push_back({ o, objectIndex });
                }
            }

            if (queueFeedback)
            {
                m_numFeedbackObjects++;
                numTexels += o->GetStreamingResource()->GetMinMipMapSize();

                // aliasing barriers for performance analysis only, NOT REQUIRED FOR CORRECT BEHAVIOR
                if (m_args.m_addAliasingBarriers)
                {
                    m_aliasingBarriers.push_back(CD3DX12_RESOURCE_BARRIER::Aliasing(nullptr, o->GetStreamingResource()->GetTiledResource()));
                }
            }

            if (m_args.m_enableTileUpdates && evict)
            {
                o->GetStreamingResource()->QueueEviction();
            }
        }

        // next time, start feedback where we left off this time.
        // note m_queueFeedbackIndex will be adjusted to # of objects next time, above
        m_queueFeedbackIndex += m_numFeedbackObjects;
    }

    // set common draw state
    SceneObjects::DrawParams drawParams;
    drawParams.m_view = m_viewMatrix;
    drawParams.m_viewInverse = m_viewMatrixInverse;
    drawParams.m_sharedMinMipMap = CD3DX12_GPU_DESCRIPTOR_HANDLE(m_srvHeap->GetGPUDescriptorHandleForHeapStart(), (UINT)DescriptorHeapOffsets::SHARED_MIN_MIP_MAP, m_srvUavCbvDescriptorSize);
    drawParams.m_constantBuffers = CD3DX12_GPU_DESCRIPTOR_HANDLE(m_srvHeap->GetGPUDescriptorHandleForHeapStart(), m_frameIndex + (UINT)DescriptorHeapOffsets::FRAME_CBV0, m_srvUavCbvDescriptorSize);
    drawParams.m_samplers = m_samplerHeap->GetGPUDescriptorHandleForHeapStart();
    drawParams.m_srvUavCbvDescriptorSize = m_srvUavCbvDescriptorSize;
    drawParams.m_descriptorHeapBaseGpu = CD3DX12_GPU_DESCRIPTOR_HANDLE(
        m_srvHeap->GetGPUDescriptorHandleForHeapStart(),
        (UINT)DescriptorHeapOffsets::NumEntries, m_srvUavCbvDescriptorSize);
    drawParams.m_descriptorHeapBaseCpu = CD3DX12_CPU_DESCRIPTOR_HANDLE(
        m_srvHeap->GetCPUDescriptorHandleForHeapStart(),
        (UINT)DescriptorHeapOffsets::NumEntries, m_srvUavCbvDescriptorSize);

    if (skyObjectSet.size())
    {
        DrawObjectSet(m_commandList.Get(), drawParams, skyObjectSet);
    }
    for (const auto& f : frameObjectSets)
    {
        DrawObjectSet(m_commandList.Get(), drawParams, f.second);
    }
}

//-------------------------------------------------------------------------
//-------------------------------------------------------------------------
void Scene::MsaaResolve()
{
    ID3D12Resource* pRenderTarget = m_renderTargets[m_frameIndex].Get();

    D3D12_RESOURCE_BARRIER barriers[] = {
        CD3DX12_RESOURCE_BARRIER::Transition(m_colorBuffer.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_RESOLVE_SOURCE),
        CD3DX12_RESOURCE_BARRIER::Transition(pRenderTarget, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RESOLVE_DEST)
    };

    m_commandList->ResourceBarrier(_countof(barriers), barriers);

    m_commandList->ResolveSubresource(pRenderTarget, 0, m_colorBuffer.Get(), 0, pRenderTarget->GetDesc().Format);

    std::swap(barriers[0].Transition.StateBefore, barriers[0].Transition.StateAfter);
    barriers[1].Transition.StateBefore = barriers[1].Transition.StateAfter;
    barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;

    m_commandList->ResourceBarrier(_countof(barriers), barriers);

    // after resolve, set the swap chain as the render target
    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = CD3DX12_CPU_DESCRIPTOR_HANDLE(m_rtvHeap->GetCPUDescriptorHandleForHeapStart(), m_frameIndex, m_rtvDescriptorSize);
    m_commandList->OMSetRenderTargets(1, &rtvHandle, FALSE, nullptr);
}

//-------------------------------------------------------------------------
// capture an image of the render target
//-------------------------------------------------------------------------
void Scene::ScreenShot(std::wstring& in_fileName) const
{
    std::wstring filename(in_fileName);
    filename += L".png";
    WindowCapture::CaptureRenderTarget(m_renderTargets[m_frameIndex].Get(), m_commandQueue.Get(), filename);
}

//-------------------------------------------------------------------------
//-------------------------------------------------------------------------
void Scene::GatherStatistics()
{
    // NOTE: streaming isn't aware of frame time.
    // these numbers are approximately a measure of the number of operations during the last frame
    const UINT numEvictions = m_pSFSManager->GetTotalNumEvictions();
    const UINT numUploads = m_pSFSManager->GetTotalNumUploads();
    static UINT numSubmits = 0;

    m_numEvictionsPreviousFrame = numEvictions - m_numTotalEvictions;
    m_numUploadsPreviousFrame = numUploads - m_numTotalUploads;

    m_numTotalEvictions = numEvictions;
    m_numTotalUploads = numUploads;

    // statistics gathering
    if (m_args.m_timingFrameFileName.size() &&
        (m_frameNumber > m_args.m_timingStartFrame) &&
        (m_frameNumber <= m_args.m_timingStopFrame))
    {
        UINT latestNumSubmits = m_pSFSManager->GetTotalNumSubmits();
        UINT numSubmitsLastFrame = latestNumSubmits - numSubmits;
        numSubmits = latestNumSubmits;

        m_csvFile->Append(m_renderThreadTimes, m_updateFeedbackTimes,
            m_numUploadsPreviousFrame, m_numEvictionsPreviousFrame,
            // Note: these may be off by 1 frame, but probably good enough
            m_pSFSManager->GetCpuProcessFeedbackTime(),
            m_gpuProcessFeedbackTime, m_numFeedbackObjects,
            numSubmitsLastFrame);

        if (m_frameNumber == m_args.m_timingStopFrame)
        {
            float measuredTime = (float)m_cpuTimer.Stop();
            UINT measuredNumUploads = numUploads - m_startUploadCount;
            float tilesPerSecond = float(measuredNumUploads) / measuredTime;
            float bytesPerTileDivMega = float(64 * 1024) / (1000.f * 1000.f);
            float mbps = tilesPerSecond * bytesPerTileDivMega;
            m_totalTileLatency = m_pSFSManager->GetTotalTileCopyLatency() - m_totalTileLatency;
            float approximatePerTileLatency = 1000.f * (m_totalTileLatency / measuredNumUploads);

            DebugPrint(L"Gathering final statistics before exiting\n");

            m_csvFile->WriteEvents(m_hwnd, m_args);
            *m_csvFile
                << "bandwidth_MB/s #uploads seconds latency_ms #submits\n"
                << mbps
                << " " << measuredNumUploads
                << " " << measuredTime
                << " " << approximatePerTileLatency
                << " " << m_pSFSManager->GetTotalNumSubmits() - m_startSubmitCount
                << "\n";
            m_csvFile->close();
            m_csvFile = nullptr;
        }
    }

    // always exit if the stop frame is set
    if ((m_args.m_timingStopFrame > 0) && (m_frameNumber >= m_args.m_timingStopFrame))
    {
        PostQuitMessage(0);
    }

    if (m_frameNumber == m_args.m_timingStartFrame)
    {
        if (m_args.m_captureTrace)
        {
            m_pSFSManager->CaptureTraceFile(true); // start recording
        }

        // start timing and gathering uploads from the very beginning of the timed region
        if (m_args.m_timingFrameFileName.size())
        {
            numSubmits = m_pSFSManager->GetTotalNumSubmits();
            m_startUploadCount = m_pSFSManager->GetTotalNumUploads();
            m_startSubmitCount = m_pSFSManager->GetTotalNumSubmits();
            m_totalTileLatency = m_pSFSManager->GetTotalTileCopyLatency();
            m_cpuTimer.Start();
        }
    }

    m_frameNumber++;
}

//-------------------------------------------------------------------------
// typically animation rates would be a function of frame time
// however, we wanted reproduceable frames for timing purposes
//-------------------------------------------------------------------------
void Scene::Animate()
{
    // animate camera
    if (m_args.m_cameraAnimationRate)
    {
        m_args.m_cameraUpLock = false;

        if (m_args.m_cameraPaintMixer)
        {
            m_args.m_cameraRollerCoaster = (0x04 & m_frameNumber);
        }

        static float theta = -XM_PIDIV2;
        const float delta = 0.01f * m_args.m_cameraAnimationRate;
        float radius = m_universeSize;

        if (m_args.m_cameraRollerCoaster)
        {
            radius /= 2.f;
        }

        theta += delta;

        float x = radius * std::cos(theta);
        float y = 2 * radius * std::cos(theta / 4);
        float z = radius * std::sin(theta);

        if (m_args.m_cameraRollerCoaster)
        {
            static XMVECTOR previous = XMVectorSet(0, 0, 0, 0);
            XMVECTOR pos = XMVectorSet(x * std::sin(theta / 4), y / 2, z / 3, 1);
            XMVECTOR lookTo = XMVector3Normalize(pos - previous);
            lookTo = XMVectorSetW(lookTo, 1.0f);

            XMVECTOR vUpVec = XMVectorSet(0.0f, 1.0f, 0.0f, 1.0f);

            SetViewMatrix(XMMatrixLookToLH(pos, lookTo, vUpVec));

            previous = pos;
        }
        else
        {
            XMVECTOR pos = XMVectorSet(x, y, z, 1);
            SetViewMatrix(XMMatrixLookAtLH(pos, XMVectorSet(0, 0, 0, 0), XMVectorSet(0.0f, 1.0f, 0.0f, 1.0f)));
        }
    }

    // spin objects
    float rotation = m_args.m_animationRate * 0.01f;

    // per-frame per-object compute visibility, lod, etc.
    const XMMATRIX worldProj = m_viewMatrix * m_projection;
    const float cotWdiv2 = XMVectorGetX(m_projection.r[0]);
    const float cotHdiv2 = XMVectorGetY(m_projection.r[1]);
    concurrency::parallel_for_each(m_objects.begin(), m_objects.end(), [&](auto o)
        {
            if (m_pSky != o)
            {
                o->Spin(rotation);
                o->SetCombinedMatrix(worldProj, m_windowHeight, cotWdiv2, cotHdiv2, m_zFar);
            }
            else
            {
                // remove translation from worldproj
                auto tmp = worldProj;
                tmp.r[3] = m_projection.r[3];
                o->SetCombinedMatrix(tmp, m_windowHeight, cotWdiv2, cotHdiv2, m_zFar);
            }
        });
}

//-------------------------------------------------------------------------
// create various windows to inspect terrain object resources
//-------------------------------------------------------------------------
void Scene::CreateTerrainViewers()
{
    ASSERT(m_pTerrain);
    if (nullptr == m_pTextureViewer)
    {
        UINT heapOffset = (UINT)DescriptorHeapOffsets::NumEntries +
            (m_terrainObjectIndex * (UINT)SceneObjects::Descriptors::NumEntries) +
            (UINT)SceneObjects::Descriptors::HeapOffsetTexture;

        // create viewer for the streaming resource
        m_pTextureViewer = new TextureViewer(
            m_pTerrain->GetTiledResource(),
            SharedConstants::SWAP_CHAIN_FORMAT, m_srvHeap.Get(),
            heapOffset);

        D3D12_PACKED_MIP_INFO info;
        m_device->GetResourceTiling(m_pTerrain->GetTiledResource(), nullptr, &info,
            nullptr, nullptr, 0, nullptr);
        m_maxNumTextureViewerWindows = info.NumStandardMips;

    }

    // NOTE: shared minmipmap will be nullptr until after SFSM::BeginFrame()
    // NOTE: the data will be delayed by 1 + 1 frame for each swap buffer (e.g. 3 for double-buffering)
    if (nullptr == m_pMinMipMapViewer)
    {
        // create min-mip-map viewer
        UINT feedbackWidth = m_pTerrain->GetStreamingResource()->GetMinMipMapWidth();
        UINT feedbackHeight = m_pTerrain->GetStreamingResource()->GetMinMipMapHeight();

        // note: the residency map will be invalid until after object is drawable
        m_pMinMipMapViewer = new BufferViewer(
            m_pTerrain->GetMinMipMap(),
            feedbackWidth, feedbackHeight, feedbackWidth,
            m_pTerrain->GetStreamingResource()->GetMinMipMapOffset(),
            SharedConstants::SWAP_CHAIN_FORMAT,
            m_srvHeap.Get(), (INT)DescriptorHeapOffsets::SHARED_MIN_MIP_MAP);
    }
}

//-------------------------------------------------------------------------
// delete various windows to inspect terrain object resources
//-------------------------------------------------------------------------
void Scene::DeleteTerrainViewers()
{
    if (m_pTextureViewer)
    {
        delete m_pTextureViewer;
        m_pTextureViewer = nullptr;
    }
    if (m_pMinMipMapViewer)
    {
        delete m_pMinMipMapViewer;
        m_pMinMipMapViewer = nullptr;
    }
}

//-------------------------------------------------------------------------
//-------------------------------------------------------------------------
void Scene::StartScene()
{
    // the first 0..(m_swapBufferCount-1) rtv handles point to the swap chain
    // there is one rtv in the rtv heap that points to the color buffer, at offset m_swapBufferCount:
    const CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle(m_rtvHeap->GetCPUDescriptorHandleForHeapStart(), m_swapBufferCount, m_rtvDescriptorSize);
    const CD3DX12_CPU_DESCRIPTOR_HANDLE dsvHandle(m_dsvHeap->GetCPUDescriptorHandleForHeapStart());
    ID3D12DescriptorHeap* ppHeaps[] = { m_srvHeap.Get(), m_samplerHeap.Get() };
    m_commandList->SetDescriptorHeaps(_countof(ppHeaps), ppHeaps);

    m_commandList->OMSetRenderTargets(1, &rtvHandle, TRUE, &dsvHandle);

    m_commandList->ClearRenderTargetView(rtvHandle, m_clearColor, 0, nullptr);
    m_commandList->ClearDepthStencilView(dsvHandle, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

    m_commandList->RSSetViewports(1, &m_viewport);
    m_commandList->RSSetScissorRects(1, &m_scissorRect);

    SetSampler();
    DirectX::XMStoreFloat4(&m_pFrameConstantData[m_frameIndex]->g_eyePos, m_viewMatrixInverse.r[3]);
    m_pFrameConstantData[m_frameIndex]->g_visualizeFeedback = m_args.m_visualizeMinMip;

    if (m_args.m_lightFromView)
    {
        constexpr float r = -XM_PIDIV4 * .5f;
        const DirectX::XMMATRIX rotate = XMMatrixRotationRollPitchYaw(r, r, 0);
        XMVECTOR v = XMVector3TransformNormal(m_viewMatrixInverse.r[2], rotate);
        XMStoreFloat4(&m_pFrameConstantData[m_frameIndex]->g_lightDir, v);
    }
    else
    {
        m_pFrameConstantData[m_frameIndex]->g_lightDir = XMFLOAT4(-0.538732767f, -0.787301660f, -0.299871892f, 0);
    }
}

//-------------------------------------------------------------------------
//-------------------------------------------------------------------------
void Scene::DrawUI()
{
    //-------------------------------------------
    // Display various textures
    //-------------------------------------------
    if (m_args.m_showFeedbackMaps && (nullptr != m_pTerrain) && (m_pTerrain->Drawable()))
    {
        CreateTerrainViewers();

        const float minDim = std::min(m_viewport.Height, m_viewport.Width) / 5;
        const DirectX::XMFLOAT2 windowSize = DirectX::XMFLOAT2(minDim, minDim);

        // terrain object's residency map
        if (m_args.m_showFeedbackViewer)
        {
            DirectX::XMFLOAT2 windowPos = DirectX::XMFLOAT2(m_viewport.Width - 2 * minDim, m_viewport.Height);
            m_pMinMipMapViewer->Draw(m_commandList.Get(), windowPos, windowSize, m_viewport);
        }

        // terrain object's texture
        if (m_args.m_showFeedbackMapVertical)
        {
            UINT areaHeight = UINT(m_viewport.Height - minDim);
            UINT numMips = areaHeight / (UINT)minDim;
            numMips = std::min(numMips, m_maxNumTextureViewerWindows);

            if (numMips > 1)
            {
                //DirectX::XMFLOAT2 windowPos = DirectX::XMFLOAT2(m_viewport.Width - minDim, 0);
                DirectX::XMFLOAT2 windowPos = DirectX::XMFLOAT2(m_viewport.Width - minDim, m_viewport.Height - minDim);
                m_pTextureViewer->Draw(m_commandList.Get(), windowPos, windowSize,
                    m_viewport,
                    m_args.m_visualizationBaseMip, numMips - 1,
                    m_args.m_showFeedbackMapVertical);
            }
        }
        else
        {
            UINT numMips = UINT(m_viewport.Width) / (UINT)minDim;
            numMips = std::min(numMips, m_maxNumTextureViewerWindows);

            DirectX::XMFLOAT2 windowPos = DirectX::XMFLOAT2(0, 0);
            m_pTextureViewer->Draw(m_commandList.Get(), windowPos, windowSize,
                m_viewport,
                m_args.m_visualizationBaseMip, numMips,
                m_args.m_showFeedbackMapVertical);
        }
    }


    //-------------------------------------------
    // Display UI
    //-------------------------------------------
    const auto& times = m_pGpuTimer->GetTimes();
    float gpuDrawTime = times[0].first; // frame draw time
    if (m_args.m_showUI)
    {
        // note: TextureViewer and BufferViewer may have internal descriptor heaps
        ID3D12DescriptorHeap* ppHeaps[] = { m_srvHeap.Get(), m_samplerHeap.Get() };
        m_commandList->SetDescriptorHeaps(_countof(ppHeaps), ppHeaps);

        UINT numTilesCommitted = 0;
        for (auto h : m_sharedHeaps)
        {
            numTilesCommitted += h->GetNumTilesAllocated();
        }

        Gui::DrawParams guiDrawParams;
        guiDrawParams.m_gpuDrawTime = gpuDrawTime;
        guiDrawParams.m_gpuFeedbackTime = m_gpuProcessFeedbackTime;
        {
            // pass in raw cpu frame time and raw # uploads. GUI will keep a running average of bandwidth
            auto a = m_renderThreadTimes.GetLatest();
            guiDrawParams.m_cpuDrawTime = a.Get(RenderEvents::PreEndFrame) - a.Get(RenderEvents::PreBeginFrame);
        }

        guiDrawParams.m_cpuFeedbackTime = m_pSFSManager->GetCpuProcessFeedbackTime();
        if (m_pTerrain && m_pTerrain->Drawable())
        {
            guiDrawParams.m_scrollMipDim = m_pTerrain->GetStreamingResource()->GetTiledResource()->GetDesc().MipLevels;
        }
        guiDrawParams.m_numTilesUploaded = m_numUploadsPreviousFrame;
        guiDrawParams.m_numTilesEvicted = m_numEvictionsPreviousFrame;
        guiDrawParams.m_numTilesCommitted = numTilesCommitted;
        guiDrawParams.m_numTilesVirtual = m_numTilesVirtual;
        guiDrawParams.m_totalHeapSize = m_args.m_streamingHeapSize * (UINT)m_sharedHeaps.size();
        guiDrawParams.m_windowHeight = m_args.m_windowHeight;

        if (m_args.m_uiModeMini)
        {
            m_pGui->DrawMini(m_commandList.Get(), guiDrawParams);
        }
        else
        {
            m_pGui->Draw(m_commandList.Get(), m_args, guiDrawParams, m_uiButtonChanges);
        }
    }
}

//-------------------------------------------------------------------------
//-------------------------------------------------------------------------
void Scene::HandleUiToggleFrustum()
{
    static bool enableTileUpdates = m_args.m_enableTileUpdates;
    static float samplerLodBias = m_args.m_lodBias;

    // stop updating while the frustum is shown
    if (m_args.m_visualizeFrustum)
    {
        // stop spinning
        m_args.m_animationRate = 0;

        XMVECTOR lookDir = m_viewMatrixInverse.r[2];
        XMVECTOR pos = m_viewMatrixInverse.r[3];

        // scale to something within universe scale
        float scale = SharedConstants::SPHERE_RADIUS * 2.5;

        m_pFrustumViewer->SetView(m_viewMatrixInverse, scale);

        enableTileUpdates = m_args.m_enableTileUpdates;
        m_args.m_enableTileUpdates = false;
        m_args.m_lodBias = -5.0f;
    }
    else
    {
        m_args.m_enableTileUpdates = enableTileUpdates;
        m_args.m_lodBias = samplerLodBias;
    }
}

//-------------------------------------------------------------------------
//-------------------------------------------------------------------------
void Scene::SwapCameraForDemo(bool in_capture)
{
    static XMMATRIX previousView = XMMatrixIdentity();

    if (in_capture)
    {
        previousView = m_viewMatrix;
    }
    else
    {
        SetViewMatrix(previousView);
    }
}

//-------------------------------------------------------------------------
// handle UI changes (outside of begin/end frame)
//-------------------------------------------------------------------------
void Scene::HandleUIchanges()
{
    if (m_args.m_showUI)
    {
        if (m_uiButtonChanges.m_directStorageToggle)
        {
            m_pSFSManager->UseDirectStorage(m_args.m_useDirectStorage);
        }
        if (m_uiButtonChanges.m_frustumToggle)
        {
            HandleUiToggleFrustum();
        }
        if (m_uiButtonChanges.m_visualizationChange)
        {
            m_pSFSManager->SetVisualizationMode((UINT)m_args.m_dataVisualizationMode);
        }

        if (m_uiButtonChanges.m_toggleBenchmarkMode)
        {
            m_benchmarkMode = !m_benchmarkMode;
            if (!m_demoMode) { SwapCameraForDemo(m_benchmarkMode); }
            else { m_demoMode = false; }
        }
        if (m_uiButtonChanges.m_toggleDemoMode)
        {
            m_demoMode = !m_demoMode;
            if (!m_benchmarkMode) { SwapCameraForDemo(m_demoMode); }
            else { m_benchmarkMode = false; }
        }

        m_uiButtonChanges = Gui::ButtonChanges(); // reset
    }
}

//-------------------------------------------------------------------------
// the m_waitForAssetLoad setting pauses until packed mips load
// do not animate or, for statistics purposes, increment frame #
//-------------------------------------------------------------------------
bool Scene::WaitForAssetLoad()
{
    for (const auto o : m_objects)
    {
        if (!o->Drawable())
        {
            // must give SFSManager a chance to process packed mip requests
            D3D12_CPU_DESCRIPTOR_HANDLE minmipmapDescriptor = CD3DX12_CPU_DESCRIPTOR_HANDLE(m_srvHeap->GetCPUDescriptorHandleForHeapStart(), (UINT)DescriptorHeapOffsets::SHARED_MIN_MIP_MAP, m_srvUavCbvDescriptorSize);
            m_pSFSManager->BeginFrame(minmipmapDescriptor);
            auto commandLists = m_pSFSManager->EndFrame();
            ID3D12CommandList* pCommandLists[] = { commandLists.m_afterDrawCommands };
            m_commandQueue->ExecuteCommandLists(_countof(pCommandLists), pCommandLists);

            MoveToNextFrame();

            return true;
        }
    }
    return false;
}

//-------------------------------------------------------------------------
//-------------------------------------------------------------------------
bool Scene::Draw()
{
    if (m_deviceRemoved)
    {
        return false;
    }

    HandleUIchanges(); // handle UI changes (outside of begin/end frame)

    // handle any changes to window dimensions or enter/exit full screen
    Resize();

#if 0
    m_args.m_cameraAnimationRate = .1f;
    // TEST: creation/deletion and thread safety
    if (m_frameNumber < 1000)
    m_args.m_numSpheres = 2 + (rand() * (m_args.m_maxNumObjects-2)) / RAND_MAX;
    //m_args.m_numSpheres = 1 + (m_frameNumber & 1);
#endif

    // load more spheres?
    // SceneResource destruction/creation must be done outside of BeginFrame/EndFrame
    LoadSpheres();
#if 0
    // FIXME
    // after loading new objects
    if (m_args.m_waitForAssetLoad && WaitForAssetLoad())
    {
        return true;
    }
#endif
    // prepare for new commands (need an open command list for LoadSpheres)
    m_commandAllocators[m_frameIndex]->Reset();
    m_commandList->Reset((ID3D12CommandAllocator*)m_commandAllocators[m_frameIndex].Get(), nullptr);

    // check the non-streaming uploader to see if anything needs to be uploaded or any memory can be freed
    m_assetUploader.WaitForUploads(m_commandQueue.Get(), m_commandList.Get());

    m_renderThreadTimes.Set(RenderEvents::PreBeginFrame);

    // get the total time the GPU spent processing feedback during the previous frame (by calling before SFSM::BeginFrame)
    m_gpuProcessFeedbackTime = m_pSFSManager->GetGpuTime();

    // prepare to update Feedback & stream textures
    D3D12_CPU_DESCRIPTOR_HANDLE minmipmapDescriptor = CD3DX12_CPU_DESCRIPTOR_HANDLE(m_srvHeap->GetCPUDescriptorHandleForHeapStart(), (UINT)DescriptorHeapOffsets::SHARED_MIN_MIP_MAP, m_srvUavCbvDescriptorSize);
    m_pSFSManager->BeginFrame(minmipmapDescriptor);

    Animate();

    //-------------------------------------------
    // draw everything
    //-------------------------------------------
    {
        GpuScopeTimer gpuScopeTimer(m_pGpuTimer, m_commandList.Get(), "GPU Frame Time");

        // set RTV, DSV, descriptor heap, etc.
        StartScene();

        // draw all geometry
        DrawObjects();

        if (m_args.m_visualizeFrustum)
        {
            XMMATRIX combinedTransform = XMMatrixMultiply(m_viewMatrix, m_projection);
            m_pFrustumViewer->Draw(m_commandList.Get(), combinedTransform, m_fieldOfView, m_aspectRatio);
        }

        // MSAA resolve
        {
            //GpuScopeTimer msaaScopeTimer(m_pGpuTimer, m_commandList.Get(), "GPU MSAA resolve");
            MsaaResolve();
        }

        DrawUI();

        D3D12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(m_renderTargets[m_frameIndex].Get(),
            D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
        m_commandList->ResourceBarrier(1, &barrier);
    }

    // Aliasing barriers are unnecessary, as draw commands only access modified resources after a fence has signaled on the copy queue
    // Note it is also theoretically possible for tiles to be re-assigned while a draw command is executing
    // However, performance analysis tools like to know about changes to resources
    if (m_aliasingBarriers.size())
    {
        m_commandList->ResourceBarrier((UINT)m_aliasingBarriers.size(), m_aliasingBarriers.data());
        m_aliasingBarriers.clear();
    }

    m_pGpuTimer->ResolveAllTimers(m_commandList.Get());
    m_commandList->Close();

    //-------------------------------------------
    // execute command lists
    //-------------------------------------------
    m_renderThreadTimes.Set(RenderEvents::PreEndFrame);
    auto commandLists = m_pSFSManager->EndFrame();
    m_renderThreadTimes.Set(RenderEvents::PostEndFrame);

    ID3D12CommandList* pCommandLists[] = { m_commandList.Get(), commandLists.m_afterDrawCommands };
    m_commandQueue->ExecuteCommandLists(_countof(pCommandLists), pCommandLists);

    //-------------------------------------------
    // Present the frame
    //-------------------------------------------
    UINT syncInterval = m_args.m_vsyncEnabled ? 1 : 0;
    UINT presentFlags = 0;
    if ((m_windowedSupportsTearing) && (!m_fullScreen) && (0 == syncInterval))
    {
        presentFlags = DXGI_PRESENT_ALLOW_TEARING;
    }
    bool success = IsDeviceOk(m_swapChain->Present(syncInterval, presentFlags));

    //-------------------------------------------
    // gather statistics before moving to next frame
    //-------------------------------------------
    GatherStatistics();
    m_renderThreadTimes.Set(RenderEvents::PreWaitNextFrame);

    MoveToNextFrame();

    m_renderThreadTimes.Set(RenderEvents::PostWaitNextFrame);
    m_renderThreadTimes.NextFrame();

    return success;
}
