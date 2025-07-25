//==============================================================
// Copyright � Intel Corporation
//
// SPDX-License-Identifier: MIT
// =============================================================

/*
Navigation Keys:

Q / W / E : strafe left / forward / strafe right
A / S / D : rotate left /  back   / rotate right
Z   /   C : rotate around look direction
X : toggle "UP" lock for rotation. ON is good for looking at terrain, OFF for general flight
shift: faster translation

SPACE: toggle animation
HOME: toggle all UI
END: toggle all UI
*/

#include "pch.h"

#include <filesystem>

#include "Scene.h"
#include "CommandLineArgs.h"
#include "ArgParser.h"
#include "JsonParser.h"
#include "Mouse.h"

Scene* g_pScene = nullptr;

bool g_hasFocus = false;
Mouse g_mouse;

std::wstring g_configFileName = L"config.json";

struct KeyState
{
    union
    {
        UINT32 m_anyKeyDown;
        struct
        {
            int32_t forward : 2;
            int32_t back    : 2;
            int32_t left    : 2;
            int32_t right   : 2;
            int32_t up      : 2;
            int32_t down    : 2;
            int32_t rotxl   : 2;
            int32_t rotxr   : 2;
            int32_t rotyl   : 2;
            int32_t rotyr   : 2;
            int32_t rotzl   : 2;
            int32_t rotzr   : 2;
        } key;
    };
    KeyState() { m_anyKeyDown = 0; }
} g_keyState;

// load arguments from a config file
void LoadConfigFile(std::wstring& in_configFileName, CommandLineArgs& out_args);

//-----------------------------------------------------------------------------
// check an incoming path. if not found, try relative to exe. if found, replace parameter
//-----------------------------------------------------------------------------
void CorrectPath(std::wstring& inout_path)
{
    if (!std::filesystem::exists(inout_path))
    {
        WCHAR buffer[MAX_PATH];
        GetModuleFileName(nullptr, buffer, MAX_PATH);
        std::filesystem::path exePath(buffer);
        exePath.remove_filename().append(inout_path);
        if (std::filesystem::exists(exePath))
        {
            inout_path = exePath;
        }
    }
}

//-----------------------------------------------------------------------------
// apply limits arguments
// e.g. # spheres, path to terrain texture
//-----------------------------------------------------------------------------
void AdjustArguments(CommandLineArgs& out_args)
{
    out_args.m_numObjects = std::clamp(out_args.m_numObjects, 1, (int)out_args.m_maxNumObjects);
    out_args.m_sampleCount = std::min(out_args.m_sampleCount, (UINT)D3D12_MAX_MULTISAMPLE_SAMPLE_COUNT);
    out_args.m_anisotropy = std::min(out_args.m_anisotropy, (UINT)D3D12_REQ_MAXANISOTROPY);

    // if there's a media directory, sky and earth are relative to media
    if (out_args.m_mediaDir.size())
    {
        // convenient for fixing other texture relative paths
        if (out_args.m_mediaDir.back() != L'\\')
        {
            out_args.m_mediaDir += L'\\';
        }

        // treat terrain path separately
        if (out_args.m_terrainTexture.size())
        {
            auto f = out_args.m_terrainTexture;
            CorrectPath(f);
            auto path = std::filesystem::absolute(f);
            if (std::filesystem::exists(path))
            {
                out_args.m_terrainTexture = path;
            }
        }

        CorrectPath(out_args.m_mediaDir);
        if (std::filesystem::exists(out_args.m_mediaDir))
        {
            for (const auto& filename : std::filesystem::directory_iterator(out_args.m_mediaDir))
            {
                auto path = std::filesystem::absolute(filename.path());
                auto z = path.extension();
                if (path.extension() != ".xet")
                {
                    continue;
                }
                std::wstring f = path;
                out_args.m_textures.push_back(f);

                // matched the requested terrain texture name? substitute the full path
                if ((out_args.m_terrainTexture.size()) && (std::wstring::npos != f.find(out_args.m_terrainTexture)))
                {
                    out_args.m_terrainTexture = f;
                }
            }

            // no terrain texture set or not found? set to something.
            if ((0 == out_args.m_terrainTexture.size()) || (!std::filesystem::exists(out_args.m_terrainTexture)))
            {
                out_args.m_terrainTexture = out_args.m_textures[0];
            }
        }
        else
        {
            std::wstringstream caption;
            caption << "NOT FOUND: -mediaDir " << out_args.m_mediaDir;
            MessageBox(0, caption.str().c_str(), L"ERROR", MB_OK);
            exit(-1);
        }
    }
}

//-----------------------------------------------------------------------------
// arguments are consumed in-order
// config.json is always consumed. expanse -config foo.cfg will overwrite previously configured parameters
//-----------------------------------------------------------------------------
void ParseCommandLine(CommandLineArgs& out_args)
{
    ArgParser argParser;

    argParser.AddArg(L"-config", [&]() { auto f = argParser.GetNextArg(); LoadConfigFile(f, out_args); }, L"Config File");

    {
        auto& sfsParams = out_args.m_sfsParams;
        argParser.AddArg(L"-directStorage", [&]() { sfsParams.m_useDirectStorage = true; }, L"force enable DirectStorage");
        argParser.AddArg(L"-stagingSizeMB", sfsParams.m_stagingBufferSizeMB, L"DirectStorage staging buffer size");
        argParser.AddArg(L"-captureTrace", [&]() { sfsParams.m_traceCaptureMode = true; }, false, L"capture a trace of tile requests and submits (DS only)");
    }

    argParser.AddArg(L"-numHeaps", out_args.m_numHeaps);
    argParser.AddArg(L"-heapSizeMB", out_args.m_sfsHeapSizeMB);
    argParser.AddArg(L"-maxFeedbackTime", out_args.m_maxGpuFeedbackTimeMs);
    argParser.AddArg(L"-addAliasingBarriers", out_args.m_addAliasingBarriers, L"Add per-draw aliasing barriers to assist PIX analysis");

    argParser.AddArg(L"-fullScreen", out_args.m_startFullScreen);
    argParser.AddArg(L"-vsync", out_args.m_vsyncEnabled);
    argParser.AddArg(L"-WindowWidth", out_args.m_windowWidth);
    argParser.AddArg(L"-WindowHeight", out_args.m_windowHeight);
    argParser.AddArg(L"-SampleCount", out_args.m_sampleCount);
    argParser.AddArg(L"-LodBias", out_args.m_lodBias);
    argParser.AddArg(L"-anisotropy", out_args.m_anisotropy);

    argParser.AddArg(L"-adapter", out_args.m_adapterDescription, L"find an adapter containing this string in the description, ignoring case");
    argParser.AddArg(L"-arch", (UINT&)out_args.m_preferredArchitecture, L"none (0), discrete (1), integrated (2)");

    argParser.AddArg(L"-animationRate", out_args.m_animationRate);
    argParser.AddArg(L"-cameraRate", out_args.m_cameraAnimationRate);
    argParser.AddArg(L"-rollerCoaster", out_args.m_cameraRollerCoaster);
    argParser.AddArg(L"-paintMixer", out_args.m_cameraPaintMixer);

    argParser.AddArg(L"-mediaDir", out_args.m_mediaDir);
    argParser.AddArg(L"-skyTexture", out_args.m_skyTexture);
    argParser.AddArg(L"-earthTexture", out_args.m_earthTexture);
    argParser.AddArg(L"-terrainTexture", out_args.m_terrainTexture);
    argParser.AddArg(L"-texture",
        [&] {
            out_args.m_mediaDir.clear();
            out_args.m_skyTexture.clear();
            out_args.m_textures.clear();
            std::wstring textureFileName = argParser.GetNextArg();
            CorrectPath(textureFileName);
            out_args.m_terrainTexture = textureFileName;
            out_args.m_textures.push_back(textureFileName);
        }, "use only one texture, with this name");

    argParser.AddArg(L"-maxNumObjects", out_args.m_maxNumObjects);
    argParser.AddArg(L"-numObjects", out_args.m_numObjects);
    argParser.AddArg(L"-lightFromView", out_args.m_lightFromView, L"Light direction is look direction");

    argParser.AddArg(L"-visualizeMinMip", [&]() { out_args.m_visualizeMinMip = true; }, out_args.m_visualizeMinMip);
    argParser.AddArg(L"-hideFeedback", [&]() { out_args.m_showFeedbackMaps = false; }, false, L"start with no feedback viewer");
    argParser.AddArg(L"-hideUI", [&]() { out_args.m_showUI = false; }, false, L"start with no visible UI");
    argParser.AddArg(L"-miniUI", [&]() { out_args.m_uiModeMini = true; }, false, L"start with mini UI");
    argParser.AddArg(L"-updateAll", out_args.m_updateEveryObjectEveryFrame);
    argParser.AddArg(L"-waitForAssetLoad", out_args.m_waitForAssetLoad, L"stall animation & statistics until assets have minimally loaded");

    argParser.AddArg(L"-timingStart", out_args.m_timingStartFrame);
    argParser.AddArg(L"-timingStop", out_args.m_timingStopFrame);
    argParser.AddArg(L"-timingFileFrames", out_args.m_timingFrameFileName);
    argParser.AddArg(L"-exitImageFile", out_args.m_exitImageFileName);

    argParser.Parse();
}

//-----------------------------------------------------------------------------
// Main message handler for the sample.
//-----------------------------------------------------------------------------
extern LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    LRESULT guiResult = ImGui_ImplWin32_WndProcHandler(hWnd, message, wParam, lParam);
    if (guiResult)
    {
        //return guiResult;
    }

    switch (message)
    {

    case WM_SIZE:
    {
        bool isFullScreen = (SIZE_MAXIMIZED == wParam);
        g_pScene->SetFullScreen(isFullScreen);
    }
    break;

    case WM_CREATE:
    {
        LPCREATESTRUCT pCreateStruct = reinterpret_cast<LPCREATESTRUCT>(lParam);
        CommandLineArgs* pArgs = reinterpret_cast<CommandLineArgs*>(pCreateStruct->lpCreateParams);

        ASSERT(nullptr == g_pScene);

        ASSERT(nullptr != pArgs);
        g_pScene = new Scene(*pArgs, hWnd);
    }
    break;

    case WM_KILLFOCUS:
        g_keyState.m_anyKeyDown = 0;
        g_hasFocus = false;
        break;

    case WM_SETFOCUS:
        g_hasFocus = true;
        break;

    case WM_MOUSEMOVE:
        g_mouse.Update(wParam, lParam);
        break;

    case WM_KEYUP:
        switch (static_cast<UINT8>(wParam))
        {
        case 'W':
            g_keyState.key.forward = 0;
            break;
        case 'S':
            g_keyState.key.back = 0;
            break;
        case 'A':
            g_keyState.key.rotyl = 0;
            break;
        case 'D':
            g_keyState.key.rotyr = 0;
            break;
        case 'Q':
            g_keyState.key.left = 0;
            break;
        case 'E':
            g_keyState.key.right = 0;
            break;
        case 'F':
            g_keyState.key.up = 0;
            break;
        case 'V':
            g_keyState.key.down = 0;
            break;

        case 'Z':
            g_keyState.key.rotzl = 0;
            break;
        case 'C':
            g_keyState.key.rotzr = 0;
            break;

        case VK_UP:
            g_keyState.key.rotxl = 0;
            break;
        case VK_DOWN:
            g_keyState.key.rotxr = 0;
            break;
        case VK_LEFT:
            g_keyState.key.rotyl = 0;
            break;
        case VK_RIGHT:
            g_keyState.key.rotyr = 0;
            break;
        }
        break;

    case WM_KEYDOWN:
        switch (static_cast<UINT8>(wParam))
        {
        case 'W':
            g_keyState.key.forward = 1;
            break;
        case 'A':
            g_keyState.key.rotyl = 1;
            break;
        case 'S':
            g_keyState.key.back = 1;
            break;
        case 'D':
            g_keyState.key.rotyr = 1;
            break;
        case 'Q':
            g_keyState.key.left = 1;
            break;
        case 'E':
            g_keyState.key.right = 1;
            break;
        case 'F':
            g_keyState.key.up = 1;
            break;
        case 'V':
            g_keyState.key.down = 1;
            break;

        case 'Z':
            g_keyState.key.rotzl = 1;
            break;
        case 'C':
            g_keyState.key.rotzr = 1;
            break;

        case '1':
            g_pScene->SetVisualizationMode(CommandLineArgs::VisualizationMode::TEXTURE);
            break;
        case '2':
            g_pScene->SetVisualizationMode(CommandLineArgs::VisualizationMode::MIPLEVEL);
            break;
        case '3':
            g_pScene->SetVisualizationMode(CommandLineArgs::VisualizationMode::RANDOM);
            break;

        case VK_UP:
            g_keyState.key.rotxl = 1;
            break;
        case VK_DOWN:
            g_keyState.key.rotxr = 1;
            break;
        case VK_LEFT:
            g_keyState.key.rotyl = 1;
            break;
        case VK_RIGHT:
            g_keyState.key.rotyr = 1;
            break;

        case VK_HOME:
            if (0x8000 & GetKeyState(VK_SHIFT))
            {
                g_pScene->ToggleUIModeMini();
            }
            else
            {
                g_pScene->ToggleUI();
            }
            break;

        case VK_END:
            g_pScene->ToggleFeedback();
            break;

        case VK_PRIOR:
            g_pScene->ToggleMinMipView();
            break;
        case VK_NEXT:
            g_pScene->ToggleRollerCoaster();
            break;

        case VK_SPACE:
            g_pScene->ToggleAnimation();
            break;

        case VK_INSERT:
            g_pScene->ToggleFrustum();
            break;

        case VK_ESCAPE:
            if (g_pScene->GetFullScreen())
            {
                ShowWindow(hWnd, SW_RESTORE);
            }
            else
            {
                DebugPrint("Normal Exit via ESC\n");
                PostQuitMessage(0);
            }
            break;
        }
        break;

    case WM_DESTROY:
        DebugPrint("Exit via DESTROY\n");
        PostQuitMessage(0);
        break;
    default:
        // Handle any other messages
        return DefWindowProc(hWnd, message, wParam, lParam);
    }

    return 0;
}

//-----------------------------------------------------------------------------
// Configuration file load
//-----------------------------------------------------------------------------
std::wstring StrToWstr(const std::string& in)
{
    std::wstringstream w;
    w << in.c_str();
    return w.str();
}

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
void LoadConfigFile(std::wstring& in_configFileName, CommandLineArgs& out_args)
{
    bool success = false;

    CorrectPath(in_configFileName);

    if (std::filesystem::exists(in_configFileName))
    {
        const JsonParser parser(in_configFileName);

        success = parser.GetReadSuccess();

        if (success)
        {
            const auto& root = parser.GetRoot();

            {
                auto& sfsParams = out_args.m_sfsParams;
                const auto& sfsDesc = root["SFSManagerDesc"];
                if (sfsDesc.isMember("directStorage")) sfsParams.m_useDirectStorage = sfsDesc["directStorage"].asBool();
                if (sfsDesc.isMember("stagingSizeMB")) sfsParams.m_stagingBufferSizeMB = sfsDesc["stagingSizeMB"].asUInt();
                if (sfsDesc.isMember("threadPriority")) sfsParams.m_threadPriority = (SFSManagerDesc::ThreadPriority)sfsDesc["threadPriority"].asInt();
                if (sfsDesc.isMember("resolveHeapSizeMB")) sfsParams.m_resolveHeapSizeMB = sfsDesc["resolveHeapSizeMB"].asInt();
                if (sfsDesc.isMember("maxTileUpdatesPerApiCall")) sfsParams.m_maxTileMappingUpdatesPerApiCall = sfsDesc["maxTileUpdatesPerApiCall"].asUInt();
                if (sfsDesc.isMember("numStreamingBatches")) sfsParams.m_maxNumCopyBatches = sfsDesc["numStreamingBatches"].asUInt();
                if (sfsDesc.isMember("evictionDelay")) sfsParams.m_evictionDelay = sfsDesc["evictionDelay"].asUInt();
            }

            {
                auto& terrainParams = out_args.m_terrainParams;
                const auto& terrain = root["terrainParams"];
                if (terrain.isMember("terrainSideSize")) terrainParams.m_terrainSideSize = terrain["terrainSideSize"].asUInt();
                if (terrain.isMember("heightScale")) terrainParams.m_heightScale = terrain["heightScale"].asFloat();
                if (terrain.isMember("noiseScale")) terrainParams.m_noiseScale = terrain["noiseScale"].asFloat();
                if (terrain.isMember("octaves")) terrainParams.m_numOctaves = terrain["octaves"].asUInt();
                if (terrain.isMember("mountainSize")) terrainParams.m_mountainSize = terrain["mountainSize"].asFloat();
            }

            if (root.isMember("fullScreen")) out_args.m_startFullScreen = root["fullScreen"].asBool();
            if (root.isMember("vsync")) out_args.m_vsyncEnabled = root["vsync"].asBool();
            if (root.isMember("windowWidth")) out_args.m_windowWidth = root["windowWidth"].asUInt();
            if (root.isMember("windowHeight")) out_args.m_windowHeight = root["windowHeight"].asUInt();
            if (root.isMember("sampleCount")) out_args.m_sampleCount = root["sampleCount"].asUInt();
            if (root.isMember("lodBias")) out_args.m_lodBias = root["lodBias"].asFloat();
            if (root.isMember("anisotropy")) out_args.m_anisotropy = root["anisotropy"].asUInt();


            if (root.isMember("animationRate")) out_args.m_animationRate = root["animationRate"].asFloat();
            if (root.isMember("cameraRate")) out_args.m_cameraAnimationRate = root["cameraRate"].asFloat();
            if (root.isMember("rollerCoaster")) out_args.m_cameraRollerCoaster = root["rollerCoaster"].asBool();
            if (root.isMember("paintMixer")) out_args.m_cameraRollerCoaster = root["paintMixer"].asBool();

            if (root.isMember("mediaDir")) out_args.m_mediaDir = StrToWstr(root["mediaDir"].asString());
            if (root.isMember("terrainTexture")) out_args.m_terrainTexture = StrToWstr(root["terrainTexture"].asString());
            if (root.isMember("skyTexture")) out_args.m_skyTexture = StrToWstr(root["skyTexture"].asString());
            if (root.isMember("earthTexture")) out_args.m_earthTexture = StrToWstr(root["earthTexture"].asString());
            if (root.isMember("maxNumObjects")) out_args.m_maxNumObjects = root["maxNumObjects"].asUInt();
            if (root.isMember("numObjects")) out_args.m_numObjects = root["numObjects"].asUInt();
            if (root.isMember("lightFromView")) out_args.m_lightFromView = root["lightFromView"].asBool();

            if (root.isMember("sphereLong")) out_args.m_sphereLong = root["sphereLong"].asUInt();
            if (root.isMember("sphereLat")) out_args.m_sphereLat = root["sphereLat"].asUInt();

            if (root.isMember("reservedMemoryGB")) out_args.m_reservedMemoryGB = root["reservedMemoryGB"].asUInt();

            if (root.isMember("heapSizeMB")) out_args.m_sfsHeapSizeMB = root["heapSizeMB"].asUInt();
            if (root.isMember("numHeaps")) out_args.m_numHeaps = root["numHeaps"].asUInt();

            if (root.isMember("maxFeedbackTime")) out_args.m_maxGpuFeedbackTimeMs = root["maxFeedbackTime"].asFloat();

            if (root.isMember("visualizeMinMip")) out_args.m_visualizeMinMip = root["visualizeMinMip"].asBool();
            if (root.isMember("hideFeedback")) out_args.m_showFeedbackMaps = !root["hideFeedback"].asBool();
            if (root.isMember("feedbackVertical")) out_args.m_showFeedbackMapVertical = root["feedbackVertical"].asBool();

            if (root.isMember("hideUI")) out_args.m_showUI = !root["hideUI"].asBool();
            if (root.isMember("miniUI")) out_args.m_uiModeMini = root["miniUI"].asBool();

            if (root.isMember("addAliasingBarriers")) out_args.m_addAliasingBarriers = root["addAliasingBarriers"].asBool();
            if (root.isMember("updateAll")) out_args.m_updateEveryObjectEveryFrame = root["updateAll"].asBool();

            if (root.isMember("timingStart")) out_args.m_timingStartFrame = root["timingStart"].asUInt();
            if (root.isMember("timingStop")) out_args.m_timingStopFrame = root["timingStop"].asUInt();
            if (root.isMember("timingFileFrames")) out_args.m_timingFrameFileName = StrToWstr(root["timingFileFrames"].asString());
            if (root.isMember("exitImageFile")) out_args.m_exitImageFileName = StrToWstr(root["exitImage"].asString());

            if (root.isMember("waitForAssetLoad")) out_args.m_waitForAssetLoad = root["waitForAssetLoad"].asBool();
            if (root.isMember("adapter")) out_args.m_adapterDescription = StrToWstr(root["adapter"].asString());

        } // end if successful load
    } // end if file exists

    if (!success)
    {
        std::wstring msg(L"Invalid Configuration file path: ");
        msg += in_configFileName;
        MessageBox(0, msg.c_str(), L"ERROR", MB_OK);
        exit(-1);
    }
}

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
int WINAPI WinMain(
    _In_ HINSTANCE hInstance,
    _In_opt_ HINSTANCE,
    _In_ LPSTR,
    _In_ int)
{
    // Enable run-time memory check for debug builds.
#if defined(DEBUG) | defined(_DEBUG)
    _CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF);
#endif

    // load default configuration
    CommandLineArgs args;
    LoadConfigFile(g_configFileName, args);

    // command line can load a different configuration file, overriding part or all of the default config
    ParseCommandLine(args);

    // apply limits and other constraints
    AdjustArguments(args);

    WNDCLASSEX wcex = {};
    wcex.cbSize = sizeof(WNDCLASSEX);
    wcex.style = CS_HREDRAW | CS_VREDRAW;
    wcex.lpfnWndProc = WndProc;
    wcex.lpszClassName = TEXT("Sampler Feedback Streaming");

    RegisterClassEx(&wcex);

    RECT windowRect = { 0, 0, (LONG)args.m_windowWidth, (LONG)args.m_windowHeight };
    AdjustWindowRect(&windowRect, WS_VISIBLE | WS_OVERLAPPEDWINDOW, FALSE);

    HWND hWnd = CreateWindow(
        wcex.lpszClassName, L"Sampler Feedback Streaming",
        WS_VISIBLE | WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT,
        windowRect.right - windowRect.left,
        windowRect.bottom - windowRect.top,
        0, 0, hInstance,
        &args);

    if (!hWnd)
    {
        UnregisterClass(wcex.lpszClassName, hInstance);
        return -1;
    }

    // full screen?
    if (args.m_startFullScreen)
    {
        PostMessage(hWnd, WM_SYSCOMMAND, SC_MAXIMIZE, 0);
    }

    MSG msg{};
    while (WM_QUIT != msg.message)
    {
        if (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        else
        {
            // handle mouse movement
            {
                auto delta = g_mouse.GetDelta(); // clears the delta
                if (delta.x || delta.y)
                {
                    // only rotate if mouse outside of gui
                    RECT guiRect = g_pScene->GetGuiRect();
                    if (((guiRect.right) < g_mouse.GetPosX()) || (guiRect.bottom < g_mouse.GetPosY()))
                    {
                        g_pScene->RotateViewPixels(delta.x, delta.y);
                    }
                }
            }

            if (g_keyState.m_anyKeyDown)
            {
                g_pScene->MoveView(
                    g_keyState.key.left - g_keyState.key.right,
                    g_keyState.key.down - g_keyState.key.up,
                    g_keyState.key.forward - g_keyState.key.back);
                g_pScene->RotateViewKey(g_keyState.key.rotxl - g_keyState.key.rotxr, g_keyState.key.rotyl - g_keyState.key.rotyr, g_keyState.key.rotzl - g_keyState.key.rotzr);
            }

            bool drawSuccess = g_pScene->Draw();
            if (!drawSuccess)
            {
                MessageBox(0, L"Device Lost", L"ERROR", MB_OK);
                exit(-1);
            }
        }
    }

    if (g_pScene)
    {
        if (args.m_exitImageFileName.size())
        {
            g_pScene->ScreenShot(args.m_exitImageFileName);
        }

        delete g_pScene;
    }

    UnregisterClass(wcex.lpszClassName, hInstance);

    return 0;
}
